# Project Name: jerboa

> **Historical document.** This is the original MVP brief that kicked off
> jerboa. It is preserved for context only and does **not** reflect the
> current design (0.1.0-beta ships 21 node types, a DAG scheduler with
> per-port queues, and much more). For up-to-date documentation see
> [`README.md`](../README.md), [`architecture.md`](architecture.md),
> [`nodes.md`](nodes.md), and [`CHANGELOG.md`](../CHANGELOG.md).

## 1. Executive Summary
jerboa is an ultra-minimal, high-performance, flow-based runtime written in C11. It executes a directed graph of processing "nodes" across a multi-threaded thread pool. The pipeline configuration is defined via a simple, human-readable text file.

## 2. Core Concepts
- **Node:** A functional block that takes an input packet, processes it, and emits an output packet. (e.g., a "Filter" node or a "Logger" node).
- **Packet:** The data unit passing through the flow (a simple structure with a string or raw byte buffer payload).
- **Queue:** A thread-safe, ring-buffer queue connecting one node's output to another node's input.

## 3. Core Features (MVP)
- **Thread Pool Execution:** A fixed number of worker threads pull available packets from node queues and execute the node's underlying C function.
- **Configurable Architecture:** The pipeline layout is loaded at runtime from a simple `flow.conf` file specifying connections (e.g., `NodeA -> NodeB`).
- **Built-in Nodes:** Deliver exactly 3 minimal node types to prove the concept:
  1. `Generator`: Periodically creates a text packet (simulating an inject node).
  2. `Uppercase`: Converts string payloads to uppercase.
  3. `Printer`: Outputs the packet payload to stdout.

## 4. Architectural Data Structures (The Core)
```c
typedef struct Packet {
    void *data;
    size_t len;
} Packet;

typedef struct Node {
    char name[32];
    void (*process)(Packet *in, Packet *out);
    struct Queue *input_queue;
    struct Node *next_node;
} Node;
```

## 6. Testing, Verification, & Feedback Loop

### A. The "Compilation First" Guard
Before verifying any logic, the code must successfully compile with our mandatory production flags:
`gcc -Wall -Wextra -Werror -O2 -g -fstack-protector-strong -D_FORTIFY_SOURCE=2 -pthread main.c jerboa.c -o jerboa`
If the compiler throws even a single warning, the implementation is considered failed, and the agent must refactor it immediately.

### B. Multi-Threaded Sanitation (The Concurrency Test)
Because this program utilizes a multi-threaded thread pool, every successful compilation must be audited for data races and thread leaks. We will use ThreadSanitizer (TSan) to verify correctness:
`gcc -fsanitize=thread -g -pthread main.c jerboa.c -o jerboa_test`
- The agent must ensure that shared structures (like the Node input queues) are perfectly guarded by mutexes.
- If a data race is reported by TSan, the agent will be provided with the trace and must fix the synchronization logic.

### C. Memory Leak Audit
The application must be able to run, process a configured number of packets, and shut down cleanly with **zero memory leaks**.
- We will test this using Valgrind: `valgrind --leak-check=full ./jerboa`
- Any unfreed packet, unjoined thread, or unclosed file configuration pointer must be diagnosed and resolved by the agent upon receiving the Valgrind log.

### D. How to Provide Feedback to the Agent
When a test fails, copy and paste the feedback using this strict format so the agent can debug effectively:
1. **Symptom:** (e.g., Segmentation Fault, Thread Hang/Deadlock, Valgrind Leak, or TSan Data Race)
2. **Log/Trace:** [Paste the exact error message or terminal dump here]
3. **Current State:** [Paste the function or file code where you suspect the issue lies]