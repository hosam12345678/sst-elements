# Understanding SST Simulation Framework

## What is SST?

**SST (Structural Simulation Toolkit)** is a discrete-event simulation framework developed by Sandia National Laboratories for simulating high-performance computing systems.

### Key Concepts:

1. **Discrete-Event Simulation**: Time advances in discrete steps (events), not continuously
2. **Component-Based**: System composed of components that communicate via links
3. **Time-Driven**: Events scheduled at specific simulation times
4. **No Real Execution**: Components simulate behavior, don't execute real operations

---

## SST Architecture in Your Code

### Component Hierarchy

```
┌─────────────────────────────────────────────────────────────┐
│                    SST Simulation Engine                     │
│                  (Event Scheduler & Clock)                   │
└──────────────┬──────────────────────────────┬───────────────┘
               │                              │
               ▼                              ▼
    ┌──────────────────┐          ┌──────────────────┐
    │ ComputeServer    │          │  MemoryServer    │
    │  Component       │◄────────►│   Component      │
    │                  │  Events  │                  │
    └──────────────────┘          └──────────────────┘
         │                              │
         │ StandardMem                  │ StandardMem
         │ Interface                    │ Interface
         ▼                              ▼
    [Simulated RDMA]              [Simulated Memory]
```

### Your Components:

#### 1. **ComputeServer** (Application Logic)
```cpp
class ComputeServer : public SST::Component {
    // Inherits from SST::Component
    // Gets scheduled by SST clock
    // Issues RDMA operations
    // Receives responses via handleMemoryEvent()
}
```

#### 2. **MemoryServer** (Storage Backend)
```cpp
class MemoryServer : public SST::Component {
    // Inherits from SST::Component  
    // Stores actual data in memory_data[]
    // Receives RDMA requests
    // Sends responses back
}
```

---

## The Problem: No Real RDMA Hardware

### In Real Systems:

```
┌──────────────┐      RDMA Network       ┌──────────────┐
│   Compute    │─────────────────────────►│   Memory     │
│   Server     │      (Real Hardware)     │   Server     │
└──────────────┘◄─────────────────────────└──────────────┘
      ↓                                           ↓
  CPU + NIC                                  DRAM + NIC
  
  Write: NIC sends data over network → Memory stores in DRAM
  Read:  NIC requests data → Memory reads DRAM → Sends back
```

### In SST Simulation:

```
┌──────────────┐    StandardMem Events   ┌──────────────┐
│ComputeServer │─────────────────────────►│ MemoryServer │
│ Component    │   (Simulated Messages)   │  Component   │
└──────────────┘◄─────────────────────────└──────────────┘
      ↓                                           ↓
  No real CPU                               No real DRAM
  No real NIC                               No real NIC
  
  Everything is SIMULATION - just C++ code scheduling events!
```

---

## Why We Need `cached_nodes`

### The Core Issue:

**In SST simulation, there's a timing problem:**

```cpp
// Step 1: ComputeServer issues RDMA WRITE
void btree_insert(key, value) {
    BTreeNode node = ...;
    rdma_write(0x10000000, &node, sizeof(node));  // Send to memory server
    // ⚠️  Write is NOT instant! It's an EVENT scheduled in future
}

// Step 2: rdma_write sends event to MemoryServer
void rdma_write(address, data, size) {
    auto req = new StandardMem::Write(address, size, data);
    rdma_interface->send(req);  // Schedules event for future time
    // ⚠️  Returns IMMEDIATELY, write hasn't happened yet!
}

// Step 3: Later, ComputeServer tries to READ that node
void btree_search(key) {
    rdma_read(0x10000000, sizeof(node), local_buffer);
    // ❌ Problem: Previous write might not have completed yet!
    // ❌ MemoryServer might not have the data!
}
```

### The Timing Problem Visualized:

```
Time (nanoseconds):
t=0    ComputeServer: rdma_write(node @ 0x10000000)
       ├─ Creates Write event
       └─ Sends to MemoryServer (scheduled for t=100)
       
t=1    ComputeServer: rdma_read(0x10000000)  ← TOO SOON!
       └─ MemoryServer hasn't received write yet!
       
t=100  MemoryServer: Receives Write event
       └─ Stores data in memory_data[]
       
t=101  MemoryServer: Receives Read event
       └─ NOW the data exists!
```

### Solution: `cached_nodes`

```cpp
// In ComputeServer.h:
std::map<uint64_t, BTreeNode> cached_nodes;  // Simulates memory persistence

// Every write ALSO updates cache:
void btree_insert(key, value) {
    BTreeNode node = ...;
    rdma_write(0x10000000, &node, sizeof(node));      // Async to MemoryServer
    cached_nodes[0x10000000] = node;                   // Instant local cache
    //                         ^^^^
    //                         Simulates what RDMA would eventually write
}

// Reads check cache first:
BTreeNode parse_node_from_buffer(address, buffer) {
    if (cached_nodes.count(address)) {
        return cached_nodes[address];  // ✅ Return what we "wrote" earlier
    }
    // Otherwise return empty node (placeholder)
}
```

---

## How SST StandardMem Interface Works

### Interface Definition (from SST core):

```cpp
namespace SST::Interfaces {
    class StandardMem {
        // Request types
        class Read  { uint64_t pAddr; size_t size; };
        class Write { uint64_t pAddr; std::vector<uint8_t> data; };
        
        // Response types  
        class ReadResp  { std::vector<uint8_t> data; };
        class WriteResp { };
        
        // Send request (schedules event in future)
        void send(Request* req);
    };
}
```

### Your Code Flow:

#### Compute → Memory (RDMA Write):

```cpp
// ComputeServer.cc
void rdma_write(uint64_t remote_address, void* data, size_t size) {
    // 1. Create write request
    std::vector<uint8_t> write_data(static_cast<uint8_t*>(data), 
                                    static_cast<uint8_t*>(data) + size);
    auto req = new StandardMem::Write(remote_address, size, write_data);
    
    // 2. Track request
    outstanding_writes[req->getID()] = getCurrentSimTime();
    
    // 3. Select interface based on target memory server
    uint64_t memory_server_id = GET_MEMORY_SERVER(remote_address);
    StandardMem* interface = rdma_interfaces[memory_server_id];
    
    // 4. Send request (becomes event in SST engine)
    interface->send(req);  // ← Schedules event for future time
    //                         (e.g., current_time + network_latency)
}
```

#### Memory Receives Request:

```cpp
// MemoryServer.cc
void handleMemoryEvent(StandardMem::Request* req) {
    // Called by SST when event reaches this component
    
    if (auto write_req = dynamic_cast<StandardMem::Write*>(req)) {
        // Extract data
        uint64_t address = write_req->pAddr;
        const std::vector<uint8_t>& data = write_req->data;
        
        // Store in memory
        write_memory(address, data);  // Copies to memory_data[]
        
        // Send response
        auto resp = new StandardMem::WriteResp(req);
        rdma_interface->send(resp);  // ← Another event back to ComputeServer
    }
}
```

#### Compute Receives Response:

```cpp
// ComputeServer.cc
void handleMemoryEvent(StandardMem::Request* req) {
    // Called when response arrives
    
    if (auto write_resp = dynamic_cast<StandardMem::WriteResp*>(req)) {
        // Calculate latency
        SimTime_t latency = getCurrentSimTime() - outstanding_writes[req->getID()];
        stat_total_latency->addData(latency);
        
        // Write completed!
        outstanding_writes.erase(req->getID());
    }
}
```

---

## Why `cached_nodes` is Temporary

### Current Architecture (Simulation):

```
ComputeServer                    MemoryServer
     │                                │
     ├─ rdma_write(node) ────────────►│
     ├─ cached_nodes[addr] = node     │ (stores in memory_data[])
     │   ^^^^^^^^^^^^^^^^^^^^^        │
     │   Instant cache update         │
     │                                │
     ├─ rdma_read(addr) ──────────────►│
     ├─ parse_node_from_buffer()      │
     │  └─ returns cached_nodes[addr] │ (would read from memory_data[])
     │      ^^^^^^^^^^^^^^^^^^^^^^^    │
     │      Uses cache, not real data │
```

### Problem with Cache:

1. **Not realistic**: Real systems don't have perfect caches
2. **Hides timing**: Doesn't simulate actual RDMA latency
3. **Synchronous behavior**: Acts like local memory, not remote

### Future Architecture (Full Simulation):

```
ComputeServer                           MemoryServer
     │                                       │
     ├─ rdma_write(node) ───────────────────►│ Stores in memory_data[]
     │  └─ Returns IMMEDIATELY                │
     │                                        │
     ├─ ... time passes ...                  │
     │     (100-1000 ns network latency)     │
     │                                        │
     ├─ handleMemoryEvent(WriteResp) ◄───────┤ Confirms write
     │  └─ Write completed!                  │
     │                                        │
     ├─ rdma_read(addr) ─────────────────────►│ Reads from memory_data[]
     │  └─ Returns IMMEDIATELY                │
     │                                        │
     ├─ ... time passes ...                  │
     │                                        │
     ├─ handleMemoryEvent(ReadResp) ◄────────┤ Data arrives
     │  └─ Parse data from response          │
     │      memcpy(local_buf, resp.data)     │
     │      BTreeNode node = deserialize()   │
     │                                        │
```

### What Needs to Change:

```cpp
// CURRENT (uses cache):
BTreeNode parse_node_from_buffer(address, buffer) {
    if (cached_nodes.count(address)) {
        return cached_nodes[address];  // ← Instant cache lookup
    }
    return BTreeNode();  // Empty placeholder
}

// FUTURE (uses actual RDMA response):
BTreeNode parse_node_from_buffer(address, buffer) {
    // buffer contains actual bytes from MemoryServer's memory_data[]
    uint8_t* buf_ptr = reinterpret_cast<uint8_t*>(buffer);
    
    BTreeNode node;
    // Deserialize from buffer:
    memcpy(&node.num_keys, buf_ptr, sizeof(uint32_t));
    buf_ptr += sizeof(uint32_t);
    
    memcpy(&node.fanout, buf_ptr, sizeof(uint32_t));
    buf_ptr += sizeof(uint32_t);
    
    memcpy(&node.is_leaf, buf_ptr, sizeof(bool));
    buf_ptr += sizeof(bool);
    
    // Deserialize keys array
    size_t keys_size = node.fanout * sizeof(uint64_t);
    node.keys.resize(node.fanout);
    memcpy(node.keys.data(), buf_ptr, keys_size);
    buf_ptr += keys_size;
    
    // ... deserialize values, children, etc.
    
    return node;  // ← Parsed from real RDMA data!
}
```

---

## MemoryServer's Role

### Current Implementation:

```cpp
// MemoryServer.cc
class MemoryServer : public Component {
private:
    uint8_t* memory_data;  // Actual simulated DRAM
    uint64_t memory_size;  // e.g., 16 MB
    
    void write_memory(uint64_t address, const std::vector<uint8_t>& data) {
        uint64_t offset = address - base_address;
        memcpy(memory_data + offset, data.data(), data.size());
        //     ^^^^^^^^^^^           ^^^^^^^^^^^
        //     Simulated DRAM        Incoming data
    }
    
    std::vector<uint8_t> read_memory(uint64_t address, size_t size) {
        uint64_t offset = address - base_address;
        std::vector<uint8_t> result(size);
        memcpy(result.data(), memory_data + offset, size);
        //                    ^^^^^^^^^^^
        //                    Simulated DRAM
        return result;  // This should go to ComputeServer's buffer!
    }
}
```

### The Connection Problem:

**MemoryServer stores data correctly, but ComputeServer doesn't use it!**

```cpp
// What SHOULD happen:
ComputeServer                           MemoryServer
     │                                       │
     ├─ rdma_read(0x10000000, 512, buf) ───►│
     │                                       ├─ read_memory(0x10000000, 512)
     │                                       │  └─ Returns 512 bytes from memory_data[]
     │                                       │
     ├─ handleMemoryEvent(ReadResp) ◄───────┤ ReadResp contains those 512 bytes
     │  │                                    │
     │  ├─ Extract data from response:      │
     │  │   auto data = read_resp->data;    │
     │  │   memcpy(buf, data.data(), 512);  │
     │  │                                    │
     │  └─ parse_node_from_buffer(addr, buf)│
     │      └─ Deserialize from buf         │
     │                                       │

// What ACTUALLY happens (because of cached_nodes):
ComputeServer                           MemoryServer
     │                                       │
     ├─ rdma_read(0x10000000, 512, buf) ───►│
     │  └─ Returns immediately                │
     │                                       │
     ├─ parse_node_from_buffer(addr, buf) ───┤ (never uses response!)
     │  └─ Returns cached_nodes[addr]       │
     │      ^^^^^^^^^^^^^^^^^^^             │
     │      Ignores MemoryServer data!      │
```

---

## How to Transition to Real Implementation

### Phase 1: Make RDMA Operations Async

Currently, operations are synchronous (blocking):

```cpp
// CURRENT (blocking):
void btree_insert(key, value) {
    BTreeNode leaf = traverse_to_leaf(key);  // ← Blocks until traversal done
    leaf.keys[...] = key;
    rdma_write(leaf.address, &leaf, sizeof(leaf));  // ← Blocks until write done
    cached_nodes[leaf.address] = leaf;
}
```

**Problem:** SST is event-driven, but code acts synchronous!

**Solution:** Use state machines or callbacks:

```cpp
// FUTURE (async with callbacks):
void btree_insert(key, value) {
    // Step 1: Start async traversal
    traverse_to_leaf_async(key, [this, key, value](BTreeNode leaf) {
        // Step 2: Called when leaf arrives
        leaf.keys[...] = key;
        
        // Step 3: Start async write
        rdma_write_async(leaf.address, &leaf, sizeof(leaf), [this, leaf]() {
            // Step 4: Called when write completes
            out.output("Insert completed for key %lu\n", key);
        });
    });
    // Returns immediately, insert continues in background
}
```

### Phase 2: Remove cached_nodes

```cpp
// Remove this:
std::map<uint64_t, BTreeNode> cached_nodes;  // DELETE

// Update parse_node_from_buffer:
BTreeNode parse_node_from_buffer(address, buffer) {
    // No more cache lookup!
    // Actually deserialize from buffer bytes
    return deserialize_btree_node(buffer);
}
```

### Phase 3: Use Response Data

```cpp
void handleMemoryEvent(StandardMem::Request* req) {
    if (auto read_resp = dynamic_cast<StandardMem::ReadResp*>(req)) {
        // Get the actual data from response
        const std::vector<uint8_t>& data = read_resp->data;
        
        // Copy to local buffer (would be done by RDMA NIC in hardware)
        uint64_t local_buffer = /* get from request context */;
        memcpy((void*)local_buffer, data.data(), data.size());
        
        // Now parse from local_buffer
        BTreeNode node = parse_node_from_buffer(address, local_buffer);
        
        // Continue operation (call callback, etc.)
        handle_read_completion(node);
    }
}
```

---

## Summary Table

| Aspect | Current (with cached_nodes) | Future (without cache) |
|--------|----------------------------|------------------------|
| **Write** | Instant cache update | Async event to MemoryServer |
| **Read** | Returns cached data | Waits for MemoryServer response |
| **Timing** | Unrealistic (instant) | Realistic (network latency) |
| **Data Source** | Local cache | Remote memory_data[] |
| **Complexity** | Simple (synchronous) | Complex (async callbacks) |
| **Realism** | Low | High |
| **Current Validity** | ✅ Good for prototyping | ✅ Required for accuracy |

---

## Why This Matters for Your Work

### Current State: Development/Prototyping
- `cached_nodes` lets you **develop and test B+tree logic** without async complexity
- Operations work correctly (inserts, splits, searches)
- Easy to debug (synchronous flow)

### Next Step: Realistic Simulation
- Remove `cached_nodes` to measure **real RDMA latency**
- See impact of network delays on tree operations
- Compare different memory architectures

### The Key Insight:

**`cached_nodes` is a simulation shortcut** that:
1. ✅ Makes development easier (synchronous code)
2. ✅ Lets B+tree logic work correctly
3. ❌ Hides real timing/latency behavior
4. ❌ Not how real RDMA systems work

When you're ready to measure **performance** and **latency**, you'll need to remove it and handle async RDMA properly. But for **correctness testing** of B+tree operations, it's perfectly fine!
