# Disaggregated Memory Architecture - Complete Memory Layout

## System Overview

This system implements a **disaggregated memory architecture** with:
- **Compute Servers**: Run application logic, issue RDMA operations
- **Memory Servers**: Store data, respond to RDMA read/write requests
- **Many-to-Many Connectivity**: Each compute server can access ANY memory server via RDMA

```
┌─────────────┐       RDMA        ┌─────────────┐
│  Compute    │◄─────────────────►│   Memory    │
│  Server 0   │                   │  Server 0   │
└─────────────┘                   └─────────────┘
       │                                 │
       │         RDMA                    │
       ├────────────────────────────┐    │
       │                            │    │
       │                            ▼    ▼
       │                          ┌─────────────┐
       │                          │   Memory    │
       │                          │  Server 1   │
       │                          └─────────────┘
       │                                 │
       │         RDMA                    │
       └────────────────────────────────►│
                                         ▼
                                   ┌─────────────┐
                                   │   Memory    │
                                   │  Server N   │
                                   └─────────────┘
```

## Remote Memory Address Space (Disaggregated Memory)

### Memory Server Address Ranges

Each memory server owns a **16 MB address space**:

```
Memory Server 0:  0x10000000 - 0x10FFFFFF  (16 MB)
Memory Server 1:  0x11000000 - 0x11FFFFFF  (16 MB)
Memory Server 2:  0x12000000 - 0x12FFFFFF  (16 MB)
Memory Server 3:  0x13000000 - 0x13FFFFFF  (16 MB)
...
Memory Server N:  0x10000000 + N*0x1000000 to 0x10000000 + (N+1)*0x1000000
```

**Formula to determine which memory server owns an address:**
```cpp
memory_server_id = (address - 0x10000000) / 0x1000000
```

**Constants:**
```cpp
MEMORY_BASE_ADDRESS = 0x10000000   // Base address for all memory servers
MEMORY_SERVER_SIZE  = 0x1000000    // 16 MB per server
```

### B+tree Layout Within Each Memory Server

Within each 16MB memory server space, B+tree nodes are organized by level:

```
┌───────────────────────────────────────────────────────────────┐
│                    Per-Server Memory Layout                    │
│                         (16 MB total)                          │
├────────────────┬───────────────┬───────────┬──────────────────┤
│ Address Range  │ Tree Level    │ Size      │ Usage            │
├────────────────┼───────────────┼───────────┼──────────────────┤
│ 0x00000        │ Root (Lvl 0)  │ 64 KB     │ Root node only   │
│   - 0x0FFFF    │               │           │                  │
├────────────────┼───────────────┼───────────┼──────────────────┤
│ 0x10000        │ Level 1       │ 64 KB     │ Internal nodes   │
│   - 0x1FFFF    │               │           │                  │
├────────────────┼───────────────┼───────────┼──────────────────┤
│ 0x20000        │ Level 2       │ 128 KB    │ Internal nodes   │
│   - 0x3FFFF    │               │           │                  │
├────────────────┼───────────────┼───────────┼──────────────────┤
│ 0x40000        │ Level 3+      │ 1.75 MB   │ Internal nodes   │
│   - 0x1FFFFF   │               │           │                  │
├────────────────┼───────────────┼───────────┼──────────────────┤
│ 0x200000       │ Leaves        │ 14 MB     │ Leaf nodes       │
│   - 0xFFFFFF   │               │           │ (actual data)    │
└────────────────┴───────────────┴───────────┴──────────────────┘
```

**Constants:**
```cpp
BTREE_LEVEL0_OFFSET = 0x00000      // Root level
BTREE_LEVEL1_OFFSET = 0x10000      // Level 1 (64 KB)
BTREE_LEVEL2_OFFSET = 0x20000      // Level 2 (128 KB)
BTREE_LEVEL3_OFFSET = 0x40000      // Level 3+ (256 KB)
BTREE_LEAF_OFFSET   = 0x200000     // Leaves (2 MB)
```

### Example Addresses

**Root Node** (always on Memory Server 0):
```
Address: 0x10000000
Server:  0
Offset:  0x00000 (BTREE_LEVEL0_OFFSET)
```

**Leaf Node on Memory Server 2:**
```
Address: 0x12200000
Server:  2
Offset:  0x200000 (BTREE_LEAF_OFFSET)
```

**Internal Node (Level 1) on Memory Server 3:**
```
Address: 0x13010000
Server:  3
Offset:  0x10000 (BTREE_LEVEL1_OFFSET)
```

## Local Compute Server Memory (RDMA Buffers)

Compute servers use **local memory** to temporarily store data fetched via RDMA reads:

```
┌────────────────────────────────────────────────────────────────┐
│              Local Compute Server Buffer Space                 │
├────────────────┬──────────────────────────────┬────────────────┤
│ Buffer Address │ Purpose                      │ Size           │
├────────────────┼──────────────────────────────┼────────────────┤
│ 0x2000000      │ Tree Traversal Buffers       │ 1 MB total     │
│   - 0x20FFFFF  │                              │                │
│                ├──────────────────────────────┼────────────────┤
│   0x2000000    │   Level 0 read buffer        │ 64 KB          │
│   0x2010000    │   Level 1 read buffer        │ 64 KB          │
│   0x2020000    │   Level 2 read buffer        │ 64 KB          │
│   0x2030000    │   Level 3 read buffer        │ 64 KB          │
│   ...          │   (+ 0x10000 per level)      │                │
├────────────────┼──────────────────────────────┼────────────────┤
│ 0x3000000      │ Leaf Node Read Buffer        │ 64 KB          │
├────────────────┼──────────────────────────────┼────────────────┤
│ 0x4000000      │ Parent Node Buffer           │ 64 KB          │
│                │ (used during splits)         │                │
├────────────────┼──────────────────────────────┼────────────────┤
│ 0x5000000      │ find_parent Traversal Buffs  │ 1 MB total     │
│   - 0x50FFFFF  │                              │                │
│   0x5000000    │   Level 0 buffer             │ 64 KB          │
│   0x5010000    │   Level 1 buffer             │ 64 KB          │
│   ...          │   (+ 0x10000 per level)      │                │
├────────────────┼──────────────────────────────┼────────────────┤
│ 0x6000000      │ Parent Verification Buffer   │ 64 KB          │
└────────────────┴──────────────────────────────┴────────────────┘
```

**Constants:**
```cpp
LOCAL_BUFFER_BASE       = 0x2000000   // Base for tree traversal
LOCAL_BUFFER_SPACING    = 0x10000     // 64 KB spacing per level
LOCAL_LEAF_BUFFER       = 0x3000000   // Leaf reads
LOCAL_PARENT_BUFFER     = 0x4000000   // Parent splits
LOCAL_FINDPARENT_BUFFER = 0x5000000   // Parent finding
LOCAL_VERIFY_BUFFER     = 0x6000000   // Verification
```

**Helper Macro:**
```cpp
#define GET_LOCAL_BUFFER(level) (LOCAL_BUFFER_BASE + (level) * LOCAL_BUFFER_SPACING)
```

## Compute-to-Memory Interaction

### 1. RDMA Read Operation

**Scenario:** Compute Server needs to read a node from remote memory

```
Step 1: Compute server calls rdma_read()
   rdma_read(remote_addr=0x12300000, size=512, local_buf=0x2000000)

Step 2: Determine target memory server
   memory_server_id = (0x12300000 - 0x10000000) / 0x1000000
                    = 0x2300000 / 0x1000000
                    = 2

Step 3: Select RDMA interface
   interface = rdma_interfaces[memory_server_id]
             = rdma_interfaces[2]

Step 4: Send read request to Memory Server 2
   Memory Server 2 receives request for address 0x12300000

Step 5: Memory Server 2 responds with data
   Data is sent back to compute server

Step 6: Data arrives at local buffer
   local_buf=0x2000000 now contains the node data
```

### 2. RDMA Write Operation

**Scenario:** Compute Server needs to write a node to remote memory

```
Step 1: Compute server calls rdma_write()
   rdma_write(remote_addr=0x11250000, size=512, local_buf=0x3000000)

Step 2: Determine target memory server
   memory_server_id = (0x11250000 - 0x10000000) / 0x1000000
                    = 0x1250000 / 0x1000000
                    = 1

Step 3: Select RDMA interface
   interface = rdma_interfaces[1]

Step 4: Send write request to Memory Server 1
   Memory Server 1 receives write for address 0x11250000
   Data comes from compute server's local buffer 0x3000000

Step 5: Memory Server 1 stores data
   Address 0x11250000 now contains the new node data
```

### 3. Complete Tree Traversal Example

**Scenario:** Search for key=12345 in a 3-level tree

```
┌──────────────────────────────────────────────────────────────┐
│ Step 1: Read Root (Level 0)                                  │
└──────────────────────────────────────────────────────────────┘
   local_buffer = GET_LOCAL_BUFFER(0) = 0x2000000
   rdma_read(root_address=0x10000000, sizeof(node), 0x2000000)
   
   Target: Memory Server 0 (0x10000000)
   Result: Root node data in local buffer 0x2000000
   
   Parse node, find child index for key=12345 → child_index=2
   Next address: root.children[2] = 0x11010000

┌──────────────────────────────────────────────────────────────┐
│ Step 2: Read Internal Node (Level 1)                         │
└──────────────────────────────────────────────────────────────┘
   local_buffer = GET_LOCAL_BUFFER(1) = 0x2010000
   rdma_read(0x11010000, sizeof(node), 0x2010000)
   
   Target: Memory Server 1 (0x11010000)
   Result: Internal node data in local buffer 0x2010000
   
   Parse node, find child index for key=12345 → child_index=5
   Next address: internal.children[5] = 0x13250000

┌──────────────────────────────────────────────────────────────┐
│ Step 3: Read Leaf Node (Level 2)                             │
└──────────────────────────────────────────────────────────────┘
   local_buffer = LOCAL_LEAF_BUFFER = 0x3000000
   rdma_read(0x13250000, sizeof(node), 0x3000000)
   
   Target: Memory Server 3 (0x13250000)
   Result: Leaf node data in local buffer 0x3000000
   
   Parse leaf, search for key=12345 in local buffer
   Found at index 7 → value = leaf.values[7]
```

**Cross-Server Access Pattern:**
```
Compute Server → Memory Server 0 (Root)
              → Memory Server 1 (Internal)
              → Memory Server 3 (Leaf)
```

## Load Balancing Strategy

### Node Distribution Across Memory Servers

Nodes are distributed across memory servers using:

```cpp
memory_server = node_id % num_memory_nodes
```

**Example with 4 memory servers:**

```
Node 0  → Server 0 (0x10000000)
Node 1  → Server 1 (0x11000000)
Node 2  → Server 2 (0x12000000)
Node 3  → Server 3 (0x13000000)
Node 4  → Server 0 (0x10000000)
Node 5  → Server 1 (0x11000000)
...
```

This ensures:
- **Even distribution** of nodes across all memory servers
- **Load balancing** for RDMA traffic
- **Parallel access** to different memory servers

## Address Calculation Functions

### allocate_node_address(node_id, level)

```cpp
uint64_t allocate_node_address(uint64_t node_id, uint32_t level) {
    // 1. Determine memory server (load balancing)
    uint64_t memory_server = node_id % num_memory_nodes;
    
    // 2. Calculate base address for this server
    uint64_t base_address = MEMORY_BASE_ADDRESS + 
                           memory_server * MEMORY_SERVER_SIZE;
    
    // 3. Determine offset within server based on level
    uint64_t offset;
    if (level == 0) {
        offset = BTREE_LEVEL0_OFFSET;  // Root
    } else if (level < tree_height - 1) {
        // Internal nodes
        uint64_t level_base = BTREE_LEVEL1_OFFSET * level;
        offset = level_base + (node_id % 10000) * sizeof(BTreeNode);
    } else {
        // Leaf nodes
        offset = BTREE_LEAF_OFFSET + (node_id % 100000) * sizeof(BTreeNode);
    }
    
    // 4. Final address
    return base_address + offset;
}
```

### get_rdma_interface_for_address(address)

```cpp
SST::Interfaces::StandardMem* get_rdma_interface_for_address(uint64_t address) {
    // Extract memory server ID from address
    uint64_t memory_server_id = GET_MEMORY_SERVER(address);
    
    // Select appropriate RDMA interface
    if (memory_server_id == 0) {
        return rdma_interface;  // Primary interface
    } else {
        return rdma_interfaces[memory_server_id - 1];
    }
}
```

## Why This Architecture?

### Benefits of Disaggregated Memory

1. **Scalability**: Memory capacity scales independently from compute
2. **Resource Sharing**: Multiple compute servers share memory pool
3. **Flexibility**: Dynamic memory allocation across servers
4. **Performance**: Parallel RDMA access to different memory servers

### Many-to-Many Connectivity

Each compute server has **direct RDMA connections** to all memory servers:

```
Compute Server has:
- rdma_interface       → Memory Server 0
- rdma_interfaces[0]   → Memory Server 1
- rdma_interfaces[1]   → Memory Server 2
- rdma_interfaces[2]   → Memory Server 3
- ...
```

This enables:
- **Load balancing** across all memory servers
- **Parallel operations** to different servers
- **Fault tolerance** (can route around failed servers)
- **High bandwidth** utilization

## Summary

| Component | Address Space | Purpose |
|-----------|---------------|---------|
| **Remote Memory** | `0x10000000 - 0x10FFFFFF` | Memory Server 0 (16 MB) |
|  | `0x11000000 - 0x11FFFFFF` | Memory Server 1 (16 MB) |
|  | ... | Additional memory servers |
| **Local Buffers** | `0x2000000 - 0x20FFFFF` | Tree traversal buffers |
|  | `0x3000000` | Leaf node buffer |
|  | `0x4000000` | Parent split buffer |
|  | `0x5000000 - 0x50FFFFF` | Parent finding buffers |
|  | `0x6000000` | Verification buffer |

**Key Insight:** Remote addresses (0x1xxxxxxx) are accessed via RDMA across the network, while local buffers (0x2xxxxxx-0x6xxxxxx) are in the compute server's local memory for temporary storage.
