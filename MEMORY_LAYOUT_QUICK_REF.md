# Memory Layout Quick Reference Card

## Constants (computeServer.cc)

```cpp
// Remote Memory Server Address Space
const uint64_t MEMORY_BASE_ADDRESS = 0x10000000;   // Base for all memory servers
const uint64_t MEMORY_SERVER_SIZE  = 0x1000000;    // 16 MB per server

// Per-Server B+tree Level Offsets
const uint64_t BTREE_LEVEL0_OFFSET = 0x00000;      // Root: 0-64KB
const uint64_t BTREE_LEVEL1_OFFSET = 0x10000;      // Level 1: 64KB-128KB
const uint64_t BTREE_LEVEL2_OFFSET = 0x20000;      // Level 2: 128KB-256KB
const uint64_t BTREE_LEVEL3_OFFSET = 0x40000;      // Level 3+: 256KB-2MB
const uint64_t BTREE_LEAF_OFFSET   = 0x200000;     // Leaves: 2MB-16MB

// Local Compute Server Buffers
const uint64_t LOCAL_BUFFER_BASE       = 0x2000000;  // Tree traversal base
const uint64_t LOCAL_BUFFER_SPACING    = 0x10000;    // 64 KB per level
const uint64_t LOCAL_LEAF_BUFFER       = 0x3000000;  // Leaf reads
const uint64_t LOCAL_PARENT_BUFFER     = 0x4000000;  // Parent splits
const uint64_t LOCAL_FINDPARENT_BUFFER = 0x5000000;  // Parent finding
const uint64_t LOCAL_VERIFY_BUFFER     = 0x6000000;  // Verification

// Helper Macros
#define GET_MEMORY_SERVER(addr) (((addr) - MEMORY_BASE_ADDRESS) / MEMORY_SERVER_SIZE)
#define GET_LOCAL_BUFFER(level) (LOCAL_BUFFER_BASE + (level) * LOCAL_BUFFER_SPACING)
```

## Address Space Map

### Remote Memory (Disaggregated)
```
0x10000000 ─────┐
                │ Memory Server 0 (16 MB)
0x10FFFFFF ─────┤
0x11000000 ─────┤
                │ Memory Server 1 (16 MB)
0x11FFFFFF ─────┤
0x12000000 ─────┤
                │ Memory Server 2 (16 MB)
0x12FFFFFF ─────┤
     ...
```

### Local Buffers (Compute Server)
```
0x2000000 ──────┐
                │ Tree Traversal Buffers (1 MB)
                │   0x2000000 = Level 0
                │   0x2010000 = Level 1
                │   0x2020000 = Level 2
0x20FFFFF ──────┤
0x3000000 ──────┤ Leaf Buffer (64 KB)
0x4000000 ──────┤ Parent Buffer (64 KB)
0x5000000 ──────┤
                │ find_parent Buffers (1 MB)
0x50FFFFF ──────┤
0x6000000 ──────┤ Verify Buffer (64 KB)
```

## Common Operations

### Get Memory Server ID from Address
```cpp
uint64_t server = GET_MEMORY_SERVER(address);
// Example: GET_MEMORY_SERVER(0x12300000) = 2
```

### Get Local Buffer for Tree Level
```cpp
uint64_t buffer = GET_LOCAL_BUFFER(level);
// Example: GET_LOCAL_BUFFER(1) = 0x2010000
```

### Allocate Node Address
```cpp
uint64_t addr = allocate_node_address(node_id, level);
// Returns: MEMORY_BASE_ADDRESS + (node_id % num_servers) * MEMORY_SERVER_SIZE + offset
```

### RDMA Read from Remote Memory
```cpp
rdma_read(remote_addr, size, local_buffer);
// Reads from remote_addr on memory server
// Stores result in local_buffer
```

### RDMA Write to Remote Memory
```cpp
rdma_write(remote_addr, size, local_buffer);
// Writes data from local_buffer
// To remote_addr on memory server
```

## Per-Server Layout (Each 16 MB)

```
Offset      Size      Purpose
─────────────────────────────────
0x000000    64 KB     Root (Level 0)
0x010000    64 KB     Level 1 internal nodes
0x020000    128 KB    Level 2 internal nodes
0x040000    1.75 MB   Level 3+ internal nodes
0x200000    14 MB     Leaf nodes (data)
```

## Example Addresses

| Address | Server | Level | Description |
|---------|--------|-------|-------------|
| `0x10000000` | 0 | Root | Root node (always server 0) |
| `0x10010000` | 0 | 1 | Internal node, level 1 |
| `0x10200000` | 0 | Leaf | Leaf node on server 0 |
| `0x11000000` | 1 | Root | Root offset on server 1 |
| `0x11200000` | 1 | Leaf | Leaf node on server 1 |
| `0x12000000` | 2 | Root | Root offset on server 2 |
| `0x13250000` | 3 | Leaf | Leaf node on server 3 |

## Compute-to-Memory Flow

```
┌─────────────┐
│  Compute    │  1. Generate key
│  Server     │
└──────┬──────┘
       │
       │ 2. Traverse tree
       │
       ├──► RDMA READ 0x10000000 → LOCAL_BUFFER_BASE      (Root, Server 0)
       │
       ├──► RDMA READ 0x11010000 → LOCAL_BUFFER_BASE+64KB (L1, Server 1)
       │
       └──► RDMA READ 0x13250000 → LOCAL_LEAF_BUFFER      (Leaf, Server 3)
            
            3. Parse leaf from LOCAL_LEAF_BUFFER
            4. Return value
```

## Key Formulas

**Memory Server Selection:**
```
server_id = node_id % num_memory_nodes
```

**Remote Address Calculation:**
```
address = MEMORY_BASE_ADDRESS + 
          server_id * MEMORY_SERVER_SIZE + 
          level_offset + 
          node_offset
```

**Local Buffer Selection:**
```
buffer = LOCAL_BUFFER_BASE + level * LOCAL_BUFFER_SPACING
```

## Many-to-Many Connectivity

Each compute server has RDMA connections to **ALL** memory servers:

```
Compute Server:
  rdma_interface         → Memory Server 0
  rdma_interfaces[0]     → Memory Server 1
  rdma_interfaces[1]     → Memory Server 2
  rdma_interfaces[2]     → Memory Server 3
  ...
```

Use `get_rdma_interface_for_address(addr)` to select the correct interface.
