# Chunk Allocation Protocol via Magic Addresses

## Overview

Chunk allocation uses a "magic address" protocol to request memory chunks from remote memory servers without requiring a separate control network or RPC mechanism.

## Protocol Design

### Magic Address Format
```
MAGIC_ALLOCATE_CHUNK_BASE = 0xFFFFFFFF00000000ULL
Request address = MAGIC_BASE | memory_server_id
Example: 0xFFFFFFFF00000002 = Request chunk from Memory Server 2
```

### Why Magic Addresses?

**Benefits:**
1. **No separate control plane**: Uses existing memory network
2. **Uniform interface**: Same StandardMem interface for data + control
3. **Simple routing**: Memory servers detect magic addresses and handle specially

**Semantics Choice: READ vs WRITE**

#### Current Implementation (WRITE-based):
```cpp
// Compute Server sends WRITE to magic address
Write* req = new Write(MAGIC_BASE | server_id, 4, dummy_data);
// Memory Server responds with ReadResp containing chunk_id
```

❓ **Problem**: Writing to get data is semantically confusing!

#### **Improved Implementation (READ-based)**: ✅
```cpp
// Compute Server sends READ from magic address  
Read* req = new Read(MAGIC_BASE | server_id, 4);
// Memory Server responds with ReadResp containing chunk_id
```

✅ **Better**: Reading from a "chunk allocator" makes semantic sense!

---

## Request Flow

### Step 1: Compute Server Requests Chunk

```cpp
// In computeServer.cc
void ComputeServer::request_chunk_from_memory_server(uint32_t memory_server_id) {
    // Create async operation to track this request
    AsyncOperation op;
    op.type = AsyncOperation::CHUNK_ALLOCATE;
    op.chunk_allocation_complete = false;
    op.chunk_allocation_failed = false;
    
    // Create READ request to magic address
    uint64_t magic_addr = MemoryServer::MAGIC_ALLOCATE_CHUNK_BASE | memory_server_id;
    auto read_req = new SST::Interfaces::StandardMem::Read(
        magic_addr,  // Magic address
        4            // Request 4 bytes (chunk_id is uint32_t)
    );
    
    // Track this request
    SST::Interfaces::StandardMem::Request::id_t req_id = read_req->getID();
    pending_ops[req_id] = op;
    
    // Route to correct memory server interface
    auto interface = get_interface_for_memory_server(memory_server_id);
    interface->send(read_req);
    
    out.output("📦 Compute %d → Memory %d: REQUEST_CHUNK (via magic address 0x%lx)\n",
               node_id, memory_server_id, magic_addr);
}
```

### Step 2: Memory Server Detects Magic Address

```cpp
// In memoryServer.cc - handleMemoryEventFromInterface()
void MemoryServer::handleMemoryEventFromInterface(
    SST::Interfaces::StandardMem::Request* req, int interface_id) {
    
    uint64_t address = req->pAddr;
    
    // Check if this is a magic address operation
    if ((address & 0xFFFFFFFF00000000ULL) == MAGIC_ALLOCATE_CHUNK_BASE) {
        // This is a chunk allocation request
        if (auto read_req = dynamic_cast<SST::Interfaces::StandardMem::Read*>(req)) {
            handle_magic_allocate_chunk(read_req, interface_id);
            return;
        }
    }
    
    // Regular memory operation...
}
```

### Step 3: Memory Server Allocates Chunk

```cpp
void MemoryServer::handle_magic_allocate_chunk(
    SST::Interfaces::StandardMem::Read* req, int interface_id) {
    
    uint64_t address = req->pAddr;
    uint32_t requested_server_id = address & 0xFFFFFFFF;
    
    out.output("🪄 Memory %d ← Compute: ALLOCATE_CHUNK request\n", memory_server_id);
    
    // Verify request is for this server
    if (requested_server_id != memory_server_id) {
        // Wrong server - send failure
        send_chunk_allocation_response(req, interface_id, 0xFFFFFFFF);
        return;
    }
    
    // Allocate chunk
    int32_t chunk_id = allocate_chunk();
    
    if (chunk_id < 0) {
        // No free chunks
        out.output("   ✗ ALLOCATION FAILED: No free chunks\n");
        send_chunk_allocation_response(req, interface_id, 0xFFFFFFFF);
        return;
    }
    
    // Success!
    uint64_t chunk_address = chunk_id_to_address(chunk_id);
    out.output("   ✓ ALLOCATED: chunk_id=%d, address=0x%lx\n", 
               chunk_id, chunk_address);
    
    // Send chunk_id back as response data
    send_chunk_allocation_response(req, interface_id, (uint32_t)chunk_id);
}

void MemoryServer::send_chunk_allocation_response(
    SST::Interfaces::StandardMem::Read* req, 
    int interface_id, 
    uint32_t chunk_id) {
    
    // Pack chunk_id into response data
    std::vector<uint8_t> response_data(4);
    memcpy(response_data.data(), &chunk_id, sizeof(uint32_t));
    
    // Create ReadResp with chunk_id
    auto resp = new SST::Interfaces::StandardMem::ReadResp(req, response_data);
    
    // Route response back through correct interface
    SST::Interfaces::StandardMem* response_interface = 
        (interface_id >= 0 && interface_id < (int)all_mem_interfaces.size())
        ? all_mem_interfaces[interface_id]
        : mem_interface;
    
    response_interface->send(resp);
}
```

### Step 4: Compute Server Receives Response

```cpp
// In computeServer.cc - handleMemoryEvent()
void ComputeServer::handleMemoryEvent(SST::Interfaces::StandardMem::Request* req) {
    auto req_id = req->getID();
    auto op_it = pending_ops.find(req_id);
    
    if (op_it == pending_ops.end()) return;
    
    AsyncOperation& op = op_it->second;
    
    if (op.type == AsyncOperation::CHUNK_ALLOCATE) {
        // This is a chunk allocation response
        if (auto read_resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req)) {
            handle_chunk_allocation_response(req_id, read_resp);
        }
        delete req;
        return;
    }
    
    // Handle other operation types...
}

void ComputeServer::handle_chunk_allocation_response(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    SST::Interfaces::StandardMem::ReadResp* resp) {
    
    auto& op = pending_ops[req_id];
    
    // Extract chunk_id from response data
    uint32_t chunk_id;
    memcpy(&chunk_id, resp->data.data(), sizeof(uint32_t));
    
    if (chunk_id == 0xFFFFFFFF) {
        // Allocation failed
        out.output("   ✗ Compute %d: CHUNK_ALLOCATE FAILED\n", node_id);
        op.chunk_allocation_failed = true;
    } else {
        // Success!
        op.chunk_allocation_complete = true;
        op.allocated_chunk_id = chunk_id;
        
        // Calculate chunk address
        // Need to know which memory server allocated this chunk
        uint64_t memory_base = 0x10000000 + (memory_server_id * 0x1000000);
        op.allocated_chunk_address = memory_base + 0x10000 + (chunk_id * CHUNK_SIZE);
        
        out.output("   ✓ Compute %d: CHUNK_ALLOCATE SUCCESS\n", node_id);
        out.output("      chunk_id=%u, address=0x%lx\n", 
                   chunk_id, op.allocated_chunk_address);
    }
    
    // Complete the operation
    pending_ops.erase(req_id);
}
```

---

## Why This Design?

### 1. **READ is semantically correct**
- Compute server **requests** data (chunk_id)
- Memory server **provides** data
- Natural request/response pattern

### 2. **AsyncOperation tracks the request**
- `op.type = CHUNK_ALLOCATE` clearly identifies this operation
- State tracking: `chunk_allocation_complete`, `chunk_allocation_failed`
- Response handling is clean and type-safe

### 3. **Magic address is just routing**
- Magic address tells memory server "this is a control command"
- Lower bits specify which memory server to target
- Network routes normally, memory server detects and handles specially

---

## Summary

**Protocol Flow:**
```
Compute Server                Memory Server
     |                             |
     |  READ(0xFFFFFFFF00000002)  |
     |--------------------------->|
     |                             | [Detect magic address]
     |                             | [Allocate chunk #42]
     |                             |
     |   ReadResp(data=[42])       |
     |<---------------------------|
     | [Parse chunk_id from data]  |
     | [Calculate chunk address]   |
     | [Mark op complete]          |
```

**Key Points:**
- ✅ Uses READ (not WRITE) for semantic clarity
- ✅ Uses AsyncOperation::CHUNK_ALLOCATE for type safety
- ✅ Magic address is just a routing mechanism
- ✅ Response contains allocated chunk_id or 0xFFFFFFFF for failure
