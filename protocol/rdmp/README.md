# RDMP – Reliable Message Distribution Protocol

## What RDMP Does

RDMP transmits **tasks 100% reliably** using multiple client and server entities so that each task is executed **on at least one** server endpoint – or, in bypass mode, on every server endpoint.

The canonical use case in the SDMI context is issuing scale-up / scale-down commands to single decentralized infrastructure nodes: a task is generated once, propagated to all participating servers via UDP multicast, and the cluster's S3 bucket acts as the shared arbitrator to minimise duplicate execution. Under bad network conditions (packet loss or delayed retransmission) a task **may still be executed by more than one server**; the implementor of the executing network entity **should** therefore check the task UUID on their side to guard against unintended re-execution.

---

## Architecture

```
Client 1 ─┐                            ┌─ Server 1
Client 2 ─┼────> UDP Multicast >────── ┼─ Server 2
          │                            └─ Server 3
          │
          └──── S3 / Ceph Object Storage (shared task queue + status)
                ▲
                │  All nodes read / write task / status objects
```

### Diagrams / SDMI Integration

As example the desired SDMI integration for scaling up a distributed docker environment (multi-datacenter setup).

- Docker Container Subnet: 172.17.100.0/24
- Docker Orchestrator Subnet: 10.10.100.0/24
- Docker Orchestrator VM1 (datacenter1): 10.10.100.253
- Docker Orchestrator VM2 (datacenter2): 10.10.100.254

The following diagrams shows exactly how the SDMI orchestrator distributes 2 up-scale tasks to `sdmi-orch-seg1-node1` with IPv4 10.10.100.253 and `sdmi-orch-seg1-node2` with IPv4 10.10.100.254 using the SDMI RDMP architecture.

**Orchestartor Communication Node1**

![OrchestratorUpscaleNode1](./diagrams/SDMI-Orchestrator-Example-Upscale-Node1.png)

**Orchestartor Communication Node2**

![OrchestratorUpscaleNode2](./diagrams/SDMI-Orchestrator-Example-Upscale-Node2.png)

> [NOTE!]
> Note that the monitoring nodes shold be responsible for task-re-execution on task failure, **not** the RDMP protocol for a strict layer seperation.

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
     guarantee.

### Server (MSG Distributor Server)

1. Joins the UDP multicast group at startup.

2. **Receive loop** (called by `runOnce()`):
   - Non-blocking `recvfrom`; parses the RDMP wire-format header.
   - For each `TASK_ANNOUNCE` datagram:

     **Normal mode (`bypass_pending_check = false`, default):**
     - Checks the local status cache. Skips if already completed / executing / failed.
     - Reads `status/<uuid>` from S3. If status is anything other than "pending"
       (i.e. "executing", "completed", or "failed"), **does not process** the task.
     - If status is "pending" (no status object, or explicit pending): writes
       `status=executing, server_id=<self>` to S3, re-reads to verify ownership
       (optimistic last-write-wins concurrency). If ownership confirmed: execute.
     - On completion: writes `status=completed` (or `failed`) to `status/<uuid>`.
     - Result: **1 status object** per task in the bucket under normal conditions.
        Under adverse network conditions (delayed duplicate announces or S3 write
        races) **more than one server may execute the same task**. The implementor
        of the executing network entity **should** check the task UUID on their
        side to prevent unintended re-execution.

     **Bypass mode (`bypass_pending_check = true`):**
     - Skips the S3 status check entirely. Every server that receives the
       announce executes the task independently.
     - Each server writes its own per-server status record to
       `status/<uuid>/<server_id>`.
     - Result: **N status objects** per task (one per server). De-duplication
       of the actual side-effect is the responsibility of the executing
       application (e.g. checking a UUID in disk / memory).

---

## Wire Format

All RDMP messages are carried as UDP datagrams.

```
Offset  Size  Field
──────  ────  ─────────────────────────────────────────────
 0       4    Magic number: 0x52444D50 ("RDMP"), big-endian
 4       1    Protocol version: 0x01
 5       1    Message type: 0x01 = TASK_ANNOUNCE
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

### Status object (normal mode) – `status/<uuid>`

```json
{
  "uuid":       "550e8400-e29b-41d4-a716-446655440000",
  "status":     "completed",
  "server_id":  "server2",
  "updated_at": "2024-01-15T10:30:01Z",
  "result":     "ok"
}
```

### Status object (bypass mode) – `status/<uuid>/<server_id>`

```json
{
  "uuid":       "550e8400-e29b-41d4-a716-446655440000",
  "status":     "completed",
  "server_id":  "server1",
  "updated_at": "2024-01-15T10:30:01Z",
  "result":     "ok"
}
```

Status values: `pending` → `executing` → `completed` | `failed`

---

## Build

### Prerequisites

| Package     | Minimum version | Purpose                        |
|-------------|-----------------|--------------------------------|
| CMake       | 3.14            | Build system                   |
| GCC / Clang | C++17           | Compiler                       |
| libcurl     | any recent      | HTTP requests to S3            |
| OpenSSL     | 1.1+            | AWS Signature V4 (HMAC-SHA256) |
| Boost.Test  | 1.71+           | Unit test framework            |

On Debian/Ubuntu:
```bash
sudo apt-get install build-essential cmake libcurl4-openssl-dev libssl-dev libboost-test-dev
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

Both binaries accept a single argument: the path to a **JSON** config file.

### Full config schema

#### Client (`config/client.json`)

```json
{
    "global": {
        "synctype": "s3"
    },
    "multicast": {
        "group": "239.1.2.3",
        "port": 5000,
        "ttl": 32,
        "interface": ""
    },
    "s3": {
        "endpoint": "http://s3.internal.example.com:9000",
        "endpoints": [
            "http://s3-replica1.internal.example.com:9000",
            "http://s3-replica2.internal.example.com:9000"
        ],
        "max_answer_timeout_ms": 5000,
        "bucket": "rdmp-tasks",
        "access_key": "your-access-key",
        "secret_key": "your-secret-key",
        "region": "int-segment-1"
    },
    "local_files": {
        "base_path": "/tmp/rdmp-tasks"
    },
    "timeouts": {
        "task_execution_ms": 5000,
        "s3_poll_interval_ms": 1000,
        "retry_delay_ms": 3000,
        "multicast_repeat_count": 3,
        "multicast_repeat_interval_ms": 100
    },
    "node": {
        "id": "client1"
    }
}
```

#### Server (`config/server.json`)

```json
{
    "global": {
        "synctype": "s3"
    },
    "multicast": {
        "group": "239.1.2.3",
        "port": 5000,
        "ttl": 32,
        "interface": ""
    },
    "s3": {
        "endpoint": "http://s3.internal.example.com:9000",
        "endpoints": [],
        "max_answer_timeout_ms": 5000,
        "bucket": "rdmp-tasks",
        "access_key": "your-access-key",
        "secret_key": "your-secret-key",
        "region": "int-segment-1"
    },
    "local_files": {
        "base_path": "/tmp/rdmp-tasks"
    },
    "timeouts": {
        "task_execution_ms": 5000,
        "s3_poll_interval_ms": 1000,
        "retry_delay_ms": 3000,
        "multicast_repeat_count": 3,
        "multicast_repeat_interval_ms": 100
    },
    "node": {
        "id": "server1"
    },
    "server": {
        "bypass_pending_check": false
    }
}
```

### Key configuration fields

| Section        | Key                            | Description                                                      |
|----------------|--------------------------------|------------------------------------------------------------------|
| `global`       | `synctype`                     | `"s3"` (default, production) or `"local-files"` (testing)        |
| `multicast`    | `group`                        | IPv4 multicast group (e.g. `239.1.2.3`)                          |
| `multicast`    | `port`                         | UDP port (same for all nodes)                                    |
| `multicast`    | `ttl`                          | Multicast IP TTL                                                 |
| `multicast`    | `interface`                    | Outbound / receive interface name (optional)                     |
| `s3`           | `endpoint`                     | Primary Ceph/S3 HTTP base URL                                    |
| `s3`           | `endpoints`                    | Optional list of fallback S3 endpoints (tried on timeout)        |
| `s3`           | `max_answer_timeout_ms`        | Per-request S3 timeout in milliseconds (default 10000)           |
| `s3`           | `bucket`                       | Shared task-queue bucket name                                    |
| `s3`           | `access_key` / `secret_key`    | Credentials (leave empty for anonymous)                          |
| `local_files`  | `base_path`                    | Root directory for the local-files backend                       |
| `timeouts`     | `task_execution_ms`            | Maximum time (ms) allowed for task execution                     |
| `timeouts`     | `s3_poll_interval_ms`          | Client backend poll interval                                     |
| `timeouts`     | `multicast_repeat_count`       | Number of UDP sends per task burst                               |
| `timeouts`     | `multicast_repeat_interval_ms` | Interval (ms) between burst datagrams                            |
| `node`         | `id`                           | Unique node identifier                                           |
| `server`       | `bypass_pending_check`         | `false` (default): single execution; `true`: all-servers execute |

### Multiple S3 endpoints

If an S3 endpoint fails (timeout or network error), the client automatically
rotates to the next endpoint in the `endpoints` list and retries the same
request.  The primary `endpoint` is always tried first.

```json
"s3": {
    "endpoint": "http://s3-primary:9000",
    "endpoints": ["http://s3-replica1:9000", "http://s3-replica2:9000"],
    "max_answer_timeout_ms": 3000
}
```

### Storage backends

RDMP supports two backends, selected via `global.synctype`:

| Value          | Description                                                       |
|----------------|-------------------------------------------------------------------|
| `"s3"`         | AWS S3 / Ceph-compatible object storage (default, production)    |
| `"local-files"`| Filesystem-backed storage under `local_files.base_path` (testing)|

The `local-files` backend writes files atomically (write to `.tmp` then `rename`).

### Recommended backend: local Ceph with S3 object storage

For production deployments, it is **strongly recommended** to run a **local Ceph cluster** (co-located with your RDMP nodes) and configure RDMP to use its S3-compatible object storage (Ceph RadosGW).

A local Ceph cluster provides:

- **Lowest possible latency** – storage I/O stays on the local network segment, avoiding WAN round-trips that would increase claim-cycle times.
- **No external dependency** – the entire RDMP shared state is contained within your own infrastructure.
- **High availability** – Ceph replicates data across multiple OSDs, so there is no single storage point of failure.
- **S3 API compatibility** – no code changes required; simply point the `s3.endpoint` at your Ceph RadosGW URL.

Example configuration snippet for a local Ceph RadosGW:

```json
"s3": {
    "endpoint": "http://ceph-rgw.internal:7480",
    "bucket": "rdmp-tasks",
    "access_key": "your-ceph-access-key",
    "secret_key": "your-ceph-secret-key",
    "region": "int-segment-1",
    "max_answer_timeout_ms": 3000
}
```

---

## Usage

### Start servers (on each server host)

```bash
./build/rdmp_server /etc/rdmp/server.json
```

### Start clients in relay mode

```bash
./build/rdmp_client /etc/rdmp/client.json
```

### Submit a one-shot task

```bash
./build/rdmp_client /etc/rdmp/client.json "scale-out node-42"
# → Task UUID: 550e8400-e29b-41d4-a716-446655440000
```

### Integrate the client library into your application

```cpp
#include "rdmp_client.hpp"

rdmp::RDMPClient client("/etc/rdmp/client.json");
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

rdmp::RDMPServer server("/etc/rdmp/server.json");

server.setTaskHandler([](const std::string& uuid,
                         const std::string& payload) -> std::string {
    // Application-specific processing
    applyControlPlaneChange(payload);
    return "applied";
});

server.run();
```

### Python bindings

```python
import rdmp

# Create a client and submit a task
client = rdmp.RDMPClient("client.json")
uuid = client.add_new_task("scale-out node-7")
client.run_once()

# Create a server with a custom handler
server = rdmp.RDMPServer("server.json")

def my_handler(uuid, payload):
    print(f"Executing task {uuid}: {payload}")
    return "ok"

server.set_task_handler(my_handler)
server.run()
```

---

## Testing

The test suite lives in `tests/` and uses Boost.Test.
Tests run without any real S3 cluster or network sockets, using the `local-files` backend.

### Run tests

```bash
cd protocol/rdmp
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

### Test coverage

| Test file                      | What is tested                                                     |
|--------------------------------|--------------------------------------------------------------------|
| `test_config.cpp`              | JSON config parsing, defaults, synctype, S3 endpoints, bypass flag |
| `test_common.cpp`              | UUID generation, task/status JSON round-trips, status conversions  |
| `test_local_files_backend.cpp` | put/get, list (recursive), overwrite, atomic writes                |
| `test_wire_format.cpp`         | UDP datagram serialisation/deserialisation, edge cases             |
| `test_end_to_end.cpp`          | Full task lifecycle, jitter, retries, node failures, races         |

The end-to-end tests specifically cover:

- Basic lifecycle (store → claim → execute → completed)
- Multiple concurrent tasks
- Duplicate announces (idempotent execution)
- Claim race between two servers (exactly-once guarantee)
- Multiple burst sends (idempotent)
- Client node failure – task discovered via backend polling
- Jitter – delayed second announce after task already completed
- Send delay – announce ignored when task already executed by peer
- Multi-client relay scenario
- 2-client / 3-server normal mode workflow
- 2-client / 3-server bypass mode workflow

---

## Licence

See the top-level `LICENSE` file in this repository.
