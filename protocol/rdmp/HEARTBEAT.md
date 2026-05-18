# RDMP HEARTBEAT (`0x02`)

## What is the HEARTBEAT message?

The RDMP `HEARTBEAT` message type (`0x02`) is a lightweight UDP multicast
datagram that a client periodically sends to the multicast group so that all
servers on the group can observe it.

Its sole purpose is **liveness signalling**: a server that stops receiving
heartbeats from a client can deduce that the client has crashed, gone offline,
or been partitioned from the network.

---

## Wire format

The HEARTBEAT datagram follows the standard RDMP wire format:

```
Offset  Size  Field
──────  ────  ─────────────────────────────────────────────────────────────
 0       4    Magic number: 0x52444D50 ("RDMP"), big-endian uint32
 4       1    Protocol version: 0x01
 5       1    Message type:  0x02  (HEARTBEAT)
 6      36    Node UUID (ASCII UUID v4 of the sending client)
42       4    Payload length N (big-endian uint32) – typically 0
46       N    Optional payload (empty for plain heartbeats)
```

The node UUID in the heartbeat is the **client's own node identifier** (not a
task UUID), so servers can map incoming heartbeats to specific client nodes.

---

## What happens under the hood

### Client side

1. The client sends a HEARTBEAT datagram to the configured multicast group
   at a configurable interval (tied to the `s3_poll_interval_ms` cadence or
   a dedicated heartbeat interval in future versions).
2. The HEARTBEAT is sent as a standard UDP multicast with the same TTL and
   interface settings as `TASK_ANNOUNCE` datagrams.

### Server side

1. Every RDMP server that is subscribed to the multicast group receives the
   HEARTBEAT datagram alongside `TASK_ANNOUNCE` datagrams.
2. The server's receive loop detects `msg_type == 0x02` and records the
   `source_ip` and `timestamp` of the heartbeat for the sending node.
3. No acknowledgement is sent back to the client (UDP, unidirectional).
4. **Degradation detection** uses the heartbeat receipt times:
   - If a known client's heartbeat is not received within
     `degradation_threshold_ms` of the previous one, the server emits a
     `CRITICAL` alert to stderr (or to the application log).
   - This does **not** trigger any automatic failover in the current
     implementation; the alert is informational.

```
Client A                     Multicast Group               Server 1, 2, 3
   |                              |                               |
   |--HEARTBEAT(uuid=clientA)---> |                               |
   |                              |--HEARTBEAT(uuid=clientA)----> |
   |                              |--HEARTBEAT(uuid=clientA)----> |
   |                              |--HEARTBEAT(uuid=clientA)----> |
   |                              |                               |
   |  (t + interval)              |                               |
   |--HEARTBEAT(uuid=clientA)---> |                               |
   |                              |--HEARTBEAT(uuid=clientA)----> |  ← timestamps
   |                              |   ...                         |   recorded
```

---

## Relationship with TASK_ANNOUNCE

| Property          | TASK_ANNOUNCE (`0x01`)      | HEARTBEAT (`0x02`)           |
|-------------------|-----------------------------|------------------------------|
| Initiator         | Client                      | Client                       |
| Direction         | Client → Servers (via MC)   | Client → Servers (via MC)    |
| Carries           | Task UUID + payload         | Client node UUID (no task)   |
| Triggers          | Task execution on server    | Liveness recording on server |
| Repeated          | N times (burst)             | Periodically                 |
| S3 interaction    | Yes (read/write task state) | No                           |
| Response          | None (UDP one-way)          | None (UDP one-way)           |

---

## Why no server → client reply?

RDMP is deliberately **asymmetric**: servers never send UDP messages back to
clients.  The shared S3 bucket serves as the status channel:

- Clients discover task outcomes by polling / listing `status/*` objects.
- This avoids the complexity of reverse-path multicast routing and removes any
  "reply group" (previously known as `multicast-reply`) from the protocol.

---

## Current implementation status

- [x] `MsgType::HEARTBEAT = 0x02` defined in `rdmp_common.hpp`
- [x] Wire-format serialisation / deserialisation tested in `test_wire_format.cpp`
- [x] Server receive loop recognises `0x02` and records receipt time
- [ ] Client periodic heartbeat sender (scheduled in the event loop) – planned
- [ ] Configurable heartbeat interval – planned
- [ ] Automatic client-failure actions (beyond logging) – planned

The protocol message type is reserved and the serialisation layer is complete.
The periodic send loop and advanced failure-handling responses will be added in
a subsequent release.

---

## Example log output

When a server detects a delayed heartbeat / missing client:

```
[RDMP/Server] CRITICAL: degradation detected for task <uuid> from source 10.0.0.1
              first_receipt=1718440200000 ms  current=1718440202500 ms
              delta=2500 ms  threshold=2000 ms
```

---

## Configuration

There are currently no dedicated `heartbeat_*` keys in the server or client
config; heartbeat detection uses the existing `degradation_threshold_ms`
from the `timeouts` section:

```json
"timeouts": {
    "degradation_threshold_ms": 2000
}
```

A dedicated `heartbeat_interval_ms` key is planned for the next release.
