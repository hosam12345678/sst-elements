# When to Call `request_chunk_allocation()`

## Current Status: ⚠️ Not Called Yet

The `request_chunk_allocation()` method is **implemented but not integrated** into the workload yet.

---

## Usage Scenarios

### Scenario 1: **Pre-allocation During Initialization** ✅ Recommended

Allocate chunks upfront when the B+tree is initialized:

```cpp
void ComputeServer::initialize_btree() {
    if (node_id != 0) {
        // Non-initializing nodes check validity bit
        return;
    }
    
    out.output("Node %d: Initializing B+tree\n", node_id);
    
    // Pre-allocate chunks from each memory server
    for (uint32_t mem_server = 0; mem_server < num_memory_nodes; mem_server++) {
        request_chunk_allocation(mem_server);
        out.output("   Pre-allocated chunk from Memory Server %u\n", mem_server);
    }
    
    // Continue with root initialization...
}
```

**Benefits:**
- Chunks ready before workload starts
- Predictable allocation overhead
- No runtime allocation delays

---

### Scenario 2: **On-Demand During Node Allocation** 

Request chunks when the address space runs out:

```cpp
uint64_t ComputeServer::allocate_node_address(uint64_t node_id, uint32_t level) {
    uint64_t server_id = node_id % num_memory_nodes;
    
    // Check if we need to request a chunk from this memory server
    if (!has_available_space(server_id)) {
        // Request chunk allocation
        request_chunk_allocation(server_id);
        
        // Wait for allocation to complete...
        // (This requires making allocate_node_address async!)
    }
    
    // Allocate node from available space
    return get_next_node_address(server_id);
}
```

**Benefits:**
- Only allocate when needed
- Efficient memory usage

**Challenges:**
- Makes node allocation **asynchronous**
- Requires state machine for waiting

---

### Scenario 3: **Test/Demo Function**

Add a simple test that demonstrates chunk allocation:

```cpp
void ComputeServer::test_chunk_allocation() {
    out.output("\n=== Testing Chunk Allocation Protocol ===\n");
    
    // Request chunks from all memory servers
    for (uint32_t i = 0; i < num_memory_nodes; i++) {
        out.output("Requesting chunk from Memory Server %u...\n", i);
        request_chunk_allocation(i);
    }
    
    out.output("Chunk allocation requests sent!\n");
}
```

Call this from `setup()` or during a special test phase:

```cpp
void ComputeServer::setup() {
    // Setup interfaces
    for (auto& interface : memory_interfaces) {
        interface->setup();
    }
    
    // TEST: Demonstrate chunk allocation
    if (node_id == 0 && verbose_level >= 1) {
        test_chunk_allocation();
    }
    
    // Initialize B+tree
    initialize_btree();
}
```

---

## Recommended Integration: **Pre-allocation**

Add this to the compute server to pre-allocate chunks during initialization:

```cpp
void ComputeServer::setup() {
    // Setup all interfaces
    for (auto& interface : memory_interfaces) {
        interface->setup();
    }
    
    // Pre-allocate chunks if this is the initializing node
    if (node_id == 0) {
        out.output("Node %d: Pre-allocating chunks from memory servers...\n", node_id);
        for (uint32_t mem_server = 0; mem_server < num_memory_nodes; mem_server++) {
            request_chunk_allocation(mem_server);
        }
        out.output("Node %d: Chunk allocation requests sent\n", node_id);
    }
    
    // Initialize B+tree after address routing is established
    initialize_btree();
}
```

This ensures:
1. ✅ Only the initializing node (node_id=0) requests chunks
2. ✅ Chunks are ready before workload starts
3. ✅ Simple synchronous flow (no waiting for chunks during operations)
4. ✅ Demonstrates the chunk allocation protocol working

---

## Response Handling

The response is already handled in `handle_chunk_allocation_response()`:

```cpp
void ComputeServer::handle_chunk_allocation_response(req_id, resp) {
    // Parse chunk_id and chunk_address
    if (chunk_id == 0xFFFFFFFF) {
        out.output("❌ Chunk allocation FAILED\n");
    } else {
        out.output("✅ Chunk allocated: chunk_id=%u, address=0x%lx\n", 
                  chunk_id, chunk_address);
        
        // TODO: Store this chunk for later use
        // allocated_chunks[memory_server_id] = chunk_address;
    }
}
```

**Next Step:** Store allocated chunks so node allocation can use them!

---

## Summary

**Current State:** Method exists but is never called

**Quick Fix:** Add to `setup()` for pre-allocation demo

**Full Integration:** Requires:
1. Store allocated chunks in compute server
2. Use chunks when allocating B+tree nodes
3. Request more chunks when running out of space
