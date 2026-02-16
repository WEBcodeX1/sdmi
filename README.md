# SDMI (Odyssey)

SDMI - Simple (SDN Driven) Docker Management Instrumentation.

## 1. Abstract

Why does the world need another docker management instrumentation today?

- Docker management infrastructure (including compose) increases in complexity over time where it should reduce
- Most frameworks ship with sub-optimal Network abstraction layers
- Software Defined Networking (SDN) was never an option where reducing complexity a lot ... also provides nearly 100% reliablility
- Management (orchestration plane) often lacks OOP principles
- Systems interation (e.g. metrics) often needs higher level programming skills

The SDMI (codename Odyssey) addresses these topics and provides:

- Code base easy to understand and to maintain
- Minimalistic bloat (prevent including submodule 289, submodule 290 and so forth)
- This reduces refactoring / update effort on dependend packages update a lot
- Why to reinvent the wheel when stable, reliable **and** field proven mechanisms already exist (SDN)

## 2. History

Over multiple decades ago (the virtualization age was rising up) smart engineers thought about software managed platform infrastructure setup.
Massive user count explosions and increasing service complexity forced ...

As first we have to state out: software defined networking can be misunderstood. When we primarily speak about SDN we are talking about the OpenFlow protocol. In some circumstances people often "verwechseln" the terminology with software which controls networking setups (including linux as a whole).

The SDMI uses **both**: SDN (OpenFlow) and parts of software (NETCONFG, ), but **no** *linux container or VM based* network abstraction (OpenStack or similar). Network processing should take place only in rock-solid, field-proven and future- Ethernet-Switch-Hardware (Cisco Nexus 9000 / Allied Telesis).

Such network abstractions also make it **very hard** to integrate appropriate reliability (single-point-of-failure-less architecture) where a SDN driven approach does.

## 3. Concrete Goals

- Automatic on-demand up / down scaling
- Easy, OOP based systems integration (orchestration, metrics)
- Roling updates (with zero downtime)
- Recursive network dependency management
- Multi-Host, Datacenter "Betrieb" / Hardware Virtual Machine Abstraction

## 3. Architecture

#TODO: add architecture description
#TODO: add architecture diagram(s)

## 4. Milestones

## 4.1 Basic Network Orchestration - Milestone 1

- Excellent orchestrator OOP abstraction
- Build an example with multi-network dependencies

#TODO: add example diagram(s)

The orchestrators design must be **native* object oriented (OOP), not . As programming language, the Python programming language version 3+ will be used.

A *fast*, working orchestrator (non OOP) implementation (draft) can be found here: ... and will be used as "vorlage" ...

## 5. Dependencies

- Micro-ESB
- SimpleRPCSocket
