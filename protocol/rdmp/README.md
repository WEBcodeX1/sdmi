# RDMP – Reliable Message Distribution Protocol

## Overview

RDMP is a C++ implementation of a reliable, distributed message distribution
protocol designed for the SDMI (Odyssey) management plane.  It combines:

- **UDP multicast** for low-latency, fan-out task announcement (data plane)
- **S3 / Ceph object storage** for durable, shared task state (control plane)

Both the client and server are **single-threaded** and event-loop based.

---

## Architecture

```
Client 1 ─┐                          ┌─ Server 1
Client 2 ─┼──── UDP Multicast ────── ┼─ Server 2
           │                          └─ Server 3
           │
           └──── S3 Bucket (shared task queue + status cache)
                         ▲
                         │  All nodes read/write task/status objects
```

### Client (MSG Distributor Client)

1. `addNewTask(payload)` / `addNewMessage(payload)`:
   - Generates a UUID v4 for the task.
   - Persists the task JSON to `tasks/<uuid>` in the shared S3 bucket.
   - Enqueues a **burst** of `multicast_repeat_count` UDP datagrams (sent at
     `multicast_repeat_interval_ms` intervals) to the configured multicast
     group so all server instances receive the announcement concurrently.

2. **S3 relay loop** (called by `runOnce()`):
   - Polls the S3 bucket for task keys not yet in the local cache.
   - For every newly discovered UUID (regardless of which client created it),
     schedules an identical UDP multicast burst.
   - This ensures that even if a client misses a burst from another client,
     it will relay the task independently – providing the parallel propagation
     described in the specification.

### Server (MSG Distributor Server)

1. Joins the UDP multicast group at startup.

2. **Receive loop** (called by `runOnce()`):
   - Non-blocking `recvfrom`; parses the RDMP wire-format header.
   - For each `TASK_ANNOUNCE` datagram:
     - Checks the local status cache.  Skips if already completed.
     - Calls `tryClaimTask(uuid)`: reads S3 status → if PENDING/absent,
       writes `status=executing, server_id=<self>` → re-reads to verify
       ownership (optimistic last-write-wins concurrency).
     - If the claim succeeds, calls the registered `TaskHandler` callback
       and updates S3 status to `completed` or `failed`.

3. **Watchdog** (rate-limited, runs inside `runOnce()`):
   - Scans locally known tasks for stale `EXECUTING` entries.
   - For tasks that have exceeded `task_execution_ms`, marks them `failed`
     in S3 and re-runs the claim/execute cycle.
   - Also discovers unknown executing tasks from S3 list to handle
     peer-node crashes.

4. **Degradation detection**:
   - Tracks the first receipt time of each UUID per source IP.
   - If a source IP's message arrives more than `degradation_threshold_ms`
     after the first, a `CRITICAL` alert is emitted to stderr.

---

## Wire Format

All RDMP messages are carried as UDP datagrams.

```
Offset  Size  Field
──────  ────  ─────────────────────────────────────────────
 0       4    Magic number: 0x52444D50 ("RDMP"), big-endian
 4       1    Protocol version: 0x01
 5       1    Message type: 0x01 = TASK_ANNOUNCE, 0x02 = HEARTBEAT
 6      36    Task UUID (ASCII, e.g. "550e8400-e29b-41d4-a716-446655440000")
42       4    Payload length N (big-endian uint32)
46       N    Payload (arbitrary bytes)
```

---

## S3 Object Schema

### Task object – `tasks/<uuid>`

```json
{
  "uuid":       "550e8400-e29b-41d4-a716-446655440000",
  "payload":    "<application-defined message>",
  "created_by": "client1",
  "created_at": "2024-01-15T10:30:00Z"
}
```

### Status object – `status/<uuid>`

```json
{
  "uuid":       "550e8400-e29b-41d4-a716-446655440000",
  "status":     "completed",
  "server_id":  "server2",
  "updated_at": "2024-01-15T10:30:01Z",
  "result":     "ok"
}
```

Status values: `pending` → `executing` → `completed` | `failed`

---

## Build

### Prerequisites

| Package    | Minimum version | Purpose                     |
|------------|-----------------|-----------------------------|
| CMake      | 3.14            | Build system                |
| GCC / Clang| C++17           | Compiler                    |
| libcurl    | any recent      | HTTP requests to S3         |
| OpenSSL    | 1.1+            | AWS Signature V4 (HMAC-SHA256) |

On Debian/Ubuntu:
```bash
sudo apt-get install build-essential cmake libcurl4-openssl-dev libssl-dev
```

### Compile

```bash
cd protocol/rdmp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Produces two binaries in `build/`:  `rdmp_client`  and  `rdmp_server`.

---

## Configuration

Both binaries accept a single argument: the path to an INI-style config file.

```bash
cp config/client.conf.example /etc/rdmp/client.conf
cp config/server.conf.example /etc/rdmp/server.conf
# Edit the files to match your environment
```

Key configuration sections:

| Section     | Key                         | Description                                     |
|-------------|-----------------------------|-------------------------------------------------|
| `multicast` | `group`                     | IPv4 multicast group (e.g. `239.1.2.3`)        |
| `multicast` | `port`                      | UDP port (same for all nodes)                   |
| `multicast` | `ttl`                       | Multicast IP TTL                                |
| `multicast` | `interface`                 | Outbound / receive interface name (optional)    |
| `s3`        | `endpoint`                  | Ceph/S3 HTTP base URL                           |
| `s3`        | `bucket`                    | Shared task-queue bucket name                   |
| `s3`        | `access_key` / `secret_key` | Credentials (leave empty for anonymous)         |
| `timeouts`  | `task_execution_ms`         | Watchdog task-execution timeout                 |
| `timeouts`  | `s3_poll_interval_ms`       | Client S3 poll interval                         |
| `timeouts`  | `degradation_threshold_ms`  | Controller latency alert threshold              |
| `timeouts`  | `multicast_repeat_count`    | Number of UDP sends per task burst              |
| `timeouts`  | `multicast_repeat_interval_ms` | Interval (ms) between burst datagrams        |
| `node`      | `id`                        | Unique node identifier                          |

---

## Usage

### Start servers (on each server host)

```bash
./build/rdmp_server /etc/rdmp/server.conf
```

### Start clients in relay mode

```bash
./build/rdmp_client /etc/rdmp/client.conf
```

### Submit a one-shot task

```bash
./build/rdmp_client /etc/rdmp/client.conf "scale-out node-42"
# → Task UUID: 550e8400-e29b-41d4-a716-446655440000
```

### Integrate the client library into your application

```cpp
#include "rdmp_client.hpp"

rdmp::RDMPClient client("/etc/rdmp/client.conf");
std::string uuid = client.addNewTask("my-control-plane-message");

// Event loop integration (call from your own loop):
while (running) {
    client.runOnce();
    usleep(1000);
}
```

### Register a custom task handler on the server

```cpp
#include "rdmp_server.hpp"

rdmp::RDMPServer server("/etc/rdmp/server.conf");

server.setTaskHandler([](const std::string& uuid,
                         const std::string& payload) -> std::string {
    // Application-specific processing
    applyControlPlaneChange(payload);
    return "applied";
});

server.run();
```

---

## Reliability Guarantees

| Property                  | Mechanism                                                   |
|---------------------------|-------------------------------------------------------------|
| At-least-once delivery    | Each client bursts N UDP datagrams; multiple clients relay  |
| Exactly-once execution    | S3 optimistic claim (PUT → GET verify)                      |
| Crash recovery            | Watchdog re-claims stale EXECUTING tasks after timeout      |
| Degraded controller alert | Per-source receipt-time comparison on servers               |
| No SPOF                   | All state in shared S3; any node can retry any task         |

---

## Licence

See the top-level `LICENSE` file in this repository.
