# In-Memory Lock Design for Shared B+tree

## Quick Reference

| Operation | Locking Strategy | Why? |
|-----------|------------------|------|
| **INSERT** | ✓ Exclusive locks root→leaf with crabbing | Prevents concurrent modifications, holds locks during splits |
| **SEARCH** | ✓ Shared locks root→leaf with crabbing | Prevents reading during splits, allows concurrent reads |
| **Node ID allocation** | ✓ Exclusive lock metadata | Prevents collisions in shared counter |
| **Root update** | ✓ Exclusive lock metadata | Atomic root pointer updates during root splits |

**Key Concepts:**
- **Lock Crabbing**: Lock child, release parent when safe
- **Shared Lock**: Multiple readers can hold simultaneously
- **Exclusive Lock**: Only one writer, blocks all readers/writers
- **Safe Node (reads)**: Always safe - can release parent immediately
- **Safe Node (writes)**: `num_keys < fanout` (has space)
- **Transaction**: Hold locks during multi-step operations (e.g., split + parent update)

---

## Architecture Overview

Locks are **embedded directly in memory** at the beginning of each B+tree node. No separate lock table or wait queues.

```
Memory Layout of B+tree Node:
┌─────────────────┬────────────────────────────────────┐
│ Lock (8 bytes)  │  Node Data (120+ bytes)           │
│ owner_id        │  num_keys, is_leaf, keys, etc.    │
└─────────────────┴────────────────────────────────────┘
  Address: X        Address: X+8
```

## Lock Protocol

### Lock Structure (Shared/Exclusive Locks)
```cpp
struct NodeLock {
    uint64_t state;
    // state encoding:
    // 0           = unlocked
    // 1-0x7FFF... = shared lock (count of readers, max ~9 quintillion readers)
    // 0x8000...   = exclusive lock (high bit set, lower bits = owner compute_id)
};

// Helper macros
#define LOCK_EXCLUSIVE_BIT  0x8000000000000000ULL
#define LOCK_UNLOCKED       0
#define IS_UNLOCKED(state)  ((state) == 0)
#define IS_SHARED(state)    ((state) > 0 && !((state) & LOCK_EXCLUSIVE_BIT))
#define IS_EXCLUSIVE(state) (((state) & LOCK_EXCLUSIVE_BIT) != 0)
#define GET_OWNER(state)    ((state) & ~LOCK_EXCLUSIVE_BIT)
#define MAKE_EXCLUSIVE(id)  (LOCK_EXCLUSIVE_BIT | (id))
```

**Design:** Single 64-bit value encodes both lock modes

### Lock Compatibility Matrix

|           | Unlocked | Shared | Exclusive |
|-----------|----------|--------|-----------|
| **Acquire Shared** | ✓ Grant | ✓ Grant (increment) | ✗ Deny |
| **Acquire Exclusive** | ✓ Grant | ✗ Deny | ✗ Deny |

### Why Use Locks for Both Reads and Writes?

**Benefits:**
- **Guaranteed consistency**: Reads never see inconsistent state
- **Concurrent reads**: Multiple searches can proceed simultaneously
- **Simple correctness**: No race conditions, easier to reason about

**Tradeoffs:**
- More lock overhead for reads
- Read-write contention at hot nodes (root)

**Optimization:** Lock crabbing for reads means we rarely hold more than 1 lock at a time

### Operations

#### 1. Try Acquire Shared Lock (for SEARCH)
```cpp
bool try_acquire_shared_lock(node_address) {
    // Read current lock state
    uint64_t current_state = read_memory(node_address, 8);
    
    // Check if we can acquire shared
    if (IS_UNLOCKED(current_state)) {
        // Unlocked → shared with count=1
        write_memory(node_address, 1);
        return true;
    } else if (IS_SHARED(current_state)) {
        // Already shared → increment reader count
        write_memory(node_address, current_state + 1);
        return true;
    } else {
        // Exclusive lock held
        return false;  // DENIED - retry later
    }
}
```

#### 2. Try Acquire Exclusive Lock (for INSERT)
```cpp
bool try_acquire_exclusive_lock(node_address, compute_id) {
    // Read current lock state
    uint64_t current_state = read_memory(node_address, 8);
    
    // Can only acquire if unlocked
    if (IS_UNLOCKED(current_state)) {
        // Write exclusive lock with owner ID
        uint64_t exclusive_state = MAKE_EXCLUSIVE(compute_id);
        write_memory(node_address, exclusive_state);
        
        // Verify we got it
        uint64_t verify = read_memory(node_address, 8);
        if (verify == exclusive_state) {
            return true;  // SUCCESS
        }
    }
    
    return false;  // DENIED - locked by someone else
}
```

#### 3. Release Shared Lock
```cpp
bool release_shared_lock(node_address) {
    // Read current lock state
    uint64_t current_state = read_memory(node_address, 8);
    
    if (IS_SHARED(current_state)) {
        // Decrement reader count
        if (current_state == 1) {
            // Last reader - unlock
            write_memory(node_address, 0);
        } else {
            // Still other readers
            write_memory(node_address, current_state - 1);
        }
        return true;
    }
    
    return false;  // ERROR - not a shared lock
}
```

#### 4. Release Exclusive Lock
```cpp
bool release_exclusive_lock(node_address, compute_id) {
    // Read current lock state
    uint64_t current_state = read_memory(node_address, 8);
    
    // Verify we own it
    if (IS_EXCLUSIVE(current_state) && GET_OWNER(current_state) == compute_id) {
        // Release (write 0)
        write_memory(node_address, 0);
        return true;
    }
    
    return false;  // ERROR - we don't own this lock!
}
```

### Compute Server Retry Loop
```cpp
// Compute server keeps trying until it gets the lock
void acquire_lock_with_retry(node_address, compute_id) {
    while (!try_acquire_lock(node_address, compute_id)) {
        // Failed to acquire, retry after delay
        wait(LOCK_RETRY_INTERVAL);  // e.g., 10ns
    }
    // Lock acquired!
}
```

## Transaction Coordination for Multi-Step Operations

### Problem: Leaf Split Requires 3 Steps

When splitting a leaf node, we need multiple memory operations to appear atomic:

```
Step 1: Write old leaf (modified)
Step 2: Write new leaf (created from split)
Step 3: Update parent (insert separator key + new child pointer)
```

**Without coordination:** Another compute could modify the parent between step 2 and step 3.

### Solution: Lock Crabbing (Hand-Over-Hand Locking)

#### Phase 1: INSERT with Lock Crabbing (Hand-Over-Hand Locking)

**Lock Crabbing Rules:**
1. Lock nodes from root → leaf
2. When you lock a child, check if it's "safe"
3. **Safe node**: `num_keys < fanout` (has space, won't split)
4. If child is safe, **release all ancestor locks** (early release optimization)
5. If child is unsafe (full), **keep holding ancestor locks**

```cpp
insert(key, value) {
    // Track which locks we hold
    std::vector<uint64_t> locked_path;
    
    // 1. Lock root
    acquire_lock(root_address, my_id);
    locked_path.push_back(root_address);
    current_address = root_address;
    node = read_node(root_address + 8);  // +8 to skip lock header
    
    // 2. Traverse tree with lock crabbing
    while (!node.is_leaf) {
        // Find which child to visit
        child_idx = find_child_for_key(node, key);
        child_address = node.children[child_idx];
        
        // Lock child before reading it
        acquire_lock(child_address, my_id);
        child_node = read_node(child_address + 8);
        
        // Check if child is "safe" (won't split)
        if (child_node.num_keys < fanout) {
            // SAFE - release all ancestors, keep only child
            for (uint64_t ancestor : locked_path) {
                release_lock(ancestor, my_id);
            }
            locked_path.clear();
        }
        
        // Add child to locked path
        locked_path.push_back(child_address);
        
        // Move to child
        current_address = child_address;
        node = child_node;
    }
    
    // 3. At leaf - we hold locks on:
    //    - Leaf (always)
    //    - Ancestors back to last unsafe node (if any will split)
    
    // Example scenarios:
    // Scenario A: All nodes have space
    //   locked_path = [leaf_address]  (released all ancestors)
    //
    // Scenario B: Leaf is full (will split)
    //   locked_path = [parent_address, leaf_address]  (kept parent)
    //
    // Scenario C: Leaf and parent both full (cascade split)
    //   locked_path = [grandparent, parent, leaf]  (kept chain)
}
```

**Why this works:**
- If leaf has space: Just insert, no split needed → only need leaf lock
- If leaf is full: Need to split and update parent → keep parent lock
- If parent is also full: Cascade split → keep chain of unsafe nodes

#### Phase 2: Multi-Step Split Transaction

```cpp
split_leaf_transaction(leaf_address, parent_address) {
    // PRECONDITION: We hold locks on both leaf and parent
    
    // Step 1: Allocate new node
    new_leaf_address = allocate_node_address(next_node_id++, leaf_level);
    acquire_lock(new_leaf_address, my_id);  // Lock before writing
    
    // Step 2: Split keys between old and new
    old_leaf = read_node(leaf_address + 8);
    new_leaf = create_node();
    separator_key = split_keys(old_leaf, new_leaf);
    
    // Step 3: Write both leaves (atomic - we hold both locks)
    write_node(leaf_address + 8, old_leaf);
    write_node(new_leaf_address + 8, new_leaf);
    
    // Step 4: Update parent (atomic - we hold parent lock)
    parent = read_node(parent_address + 8);
    insert_into_internal(parent, separator_key, new_leaf_address);
    write_node(parent_address + 8, parent);
    
    // Step 5: Release all locks in reverse order
    release_lock(new_leaf_address, my_id);
    release_lock(leaf_address, my_id);
    release_lock(parent_address, my_id);
    
    // POSTCONDITION: Split is complete and atomic from external view
}
```

### Lock Holding Rules

1. **Always lock top-down** (root → leaf) to avoid deadlock
2. **Hold locks during multi-step operations**
3. **Release locks bottom-up** after operation completes
4. **Early release optimization**: Release parent if child is "safe" (won't split)

### Example: Concurrent Inserts with Locks

```
Timeline:

Compute 0                         Compute 1
─────────────────────────────────────────────────────
Lock root ✓                       Try lock root ✗ (retry...)
Read root                         
Lock child ✓                      
Release root ✓                    Lock root ✓ (now available)
                                  Read root
Insert into leaf                  Lock child ✗ (still held by C0, retry...)
Release child ✓                   
                                  Lock child ✓ (now available)
                                  Insert into different leaf
                                  Release child ✓
                                  Release root ✓
```

## Shared Metadata Management

### Problem: Node ID Allocation Race

```cpp
// Both computes start with next_node_id = 0
Compute 0: allocates Node 0
Compute 1: allocates Node 0  // COLLISION!
```

### Solution: Shared Metadata Server (Memory Server 0)

```
Special metadata region at base address:
┌────────────────────────────────────────┐
│ Metadata Block @ 0x10000000           │
├────────────────────────────────────────┤
│ Lock (8 bytes)                         │
│ root_address (8 bytes)                 │
│ next_node_id (8 bytes)                 │
│ tree_height (4 bytes)                  │
└────────────────────────────────────────┘
```

#### Allocate Node ID (Atomic)
```cpp
uint64_t allocate_node_id(compute_id) {
    metadata_addr = 0x10000000;
    
    // Lock metadata
    acquire_lock(metadata_addr, compute_id);
    
    // Read counter
    next_id = read_memory(metadata_addr + 16, 8);  // +8 for lock, +8 for root
    
    // Increment counter
    write_memory(metadata_addr + 16, next_id + 1);
    
    // Release lock
    release_lock(metadata_addr, compute_id);
    
    return next_id;
}
```

#### Update Root Pointer (Atomic)
```cpp
void set_root_address(new_root_address, compute_id) {
    metadata_addr = 0x10000000;
    
    // Lock metadata
    acquire_lock(metadata_addr, compute_id);
    
    // Update root pointer
    write_memory(metadata_addr + 8, new_root_address);
    
    // Release lock
    release_lock(metadata_addr, compute_id);
}
```

## Implementation Details

### Memory Server: Lock Operations

```cpp
// In memoryServer.cc:

bool MemoryServer::try_acquire_lock(uint64_t node_address, uint64_t compute_id) {
    // Read lock (first 8 bytes of node)
    std::vector<uint8_t> lock_data = read_memory(node_address, LOCK_HEADER_SIZE);
    uint64_t current_owner;
    memcpy(&current_owner, lock_data.data(), sizeof(uint64_t));
    
    if (current_owner == 0) {
        // Unlocked - try to acquire
        std::vector<uint8_t> new_lock_data(LOCK_HEADER_SIZE);
        memcpy(new_lock_data.data(), &compute_id, sizeof(uint64_t));
        write_memory(node_address, new_lock_data);
        
        total_locks_acquired++;
        stat_locks_acquired->addData(1);
        return true;
    } else {
        // Already locked
        total_lock_conflicts++;
        stat_lock_conflicts->addData(1);
        return false;
    }
}

bool MemoryServer::release_lock(uint64_t node_address, uint64_t compute_id) {
    // Read current lock
    std::vector<uint8_t> lock_data = read_memory(node_address, LOCK_HEADER_SIZE);
    uint64_t current_owner;
    memcpy(&current_owner, lock_data.data(), sizeof(uint64_t));
    
    if (current_owner == compute_id) {
        // We own it - release
        uint64_t zero = 0;
        std::vector<uint8_t> unlock_data(LOCK_HEADER_SIZE);
        memcpy(unlock_data.data(), &zero, sizeof(uint64_t));
        write_memory(node_address, unlock_data);
        
        total_locks_released++;
        stat_locks_released->addData(1);
        return true;
    } else {
        // Error: trying to release lock we don't own
        out.output("ERROR: Compute %lu tried to release lock at 0x%lx owned by %lu\n",
                   compute_id, node_address, current_owner);
        return false;
    }
}
```

### Compute Server: Lock Protocol Integration

```cpp
// In computeServer.cc:

void ComputeServer::btree_insert_with_locks(uint64_t key, uint64_t value) {
    // Phase 1: Lock root and start traversal
    acquire_lock_with_retry(root_address);
    
    // Initiate async operation (now with lock held)
    auto req = new SST::Interfaces::StandardMem::Read(
        root_address + LOCK_HEADER_SIZE,  // Skip lock when reading node
        get_serialized_node_size()
    );
    
    // Track that we hold lock
    pending_ops[req_id].held_locks.push_back(root_address);
    
    // Continue traversal...
}

void ComputeServer::handle_split_operation(AsyncOperation& op) {
    // We hold locks on: leaf and parent
    
    // Allocate new node ID (requires metadata lock)
    uint64_t new_node_id = allocate_node_id_atomic();
    uint64_t new_leaf_addr = allocate_node_address(new_node_id, leaf_level);
    
    // Lock new node before writing
    acquire_lock_with_retry(new_leaf_addr);
    op.held_locks.push_back(new_leaf_addr);
    
    // Perform split...
    
    // After all writes complete, release all locks
    for (uint64_t addr : op.held_locks) {
        release_lock(addr);
    }
}
```

## Locking Strategy Summary

### INSERT Operations (Exclusive Locks)
```
✓ USE EXCLUSIVE LOCKS with lock crabbing
✓ Lock from root → leaf
✓ Early release optimization for safe nodes (num_keys < fanout)
✓ Hold locks during splits (multi-step transaction)
✓ Blocks all other operations on locked nodes
```

### SEARCH Operations (Shared Locks)
```
✓ USE SHARED LOCKS with lock crabbing
✓ Guarantees consistency (no race conditions)
✓ Allows concurrent reads (multiple searchers)
✓ Blocks during writes (exclusive locks)
```

**Lock Crabbing for Reads:**
```cpp
search(key) {
    // 1. Acquire shared lock on root
    acquire_shared_lock(root_address);
    node = read_node(root_address + 8);
    
    // 2. Traverse tree
    while (!node.is_leaf) {
        child_idx = find_child_for_key(node, key);
        child_address = node.children[child_idx];
        
        // Lock child
        acquire_shared_lock(child_address);
        
        // For reads, always safe to release parent immediately
        // (we only need to prevent concurrent writes, not other reads)
        release_shared_lock(current_address);
        
        current_address = child_address;
        node = read_node(child_address + 8);
    }
    
    // 3. At leaf - search for key
    result = find_key_in_leaf(node, key);
    
    // 4. Release leaf lock
    release_shared_lock(current_address);
    
    return result;
}
```

**Why This Works:**

**Concurrent Reads (both use shared locks):**
```
Time    Reader 1 (SEARCH key=5)          Reader 2 (SEARCH key=12)
─────────────────────────────────────────────────────────────────
T0      Acquire shared(root)             Acquire shared(root) ✓
T1      Read root, find child            Read root, find child
T2      Acquire shared(leaf_A)           Acquire shared(leaf_B) ✓
T3      Release shared(root)             Release shared(root)
T4      Search in leaf_A                 Search in leaf_B
T5      Release shared(leaf_A)           Release shared(leaf_B)
```
Both succeed! Shared locks allow concurrent reads.

**Read Blocked by Write:**
```
Time    Writer (INSERT)                  Reader (SEARCH key=12)
─────────────────────────────────────────────────────────────────
T0      Acquire exclusive(leaf)          
T1      Perform split operation          Try acquire shared(leaf) ✗
T2      Write nodes                      Retry... (blocked)
T3      Update parent                    Retry... (blocked)
T4      Release exclusive(leaf)          Retry... ✓ GRANTED
T5                                       Read consistent state
```
Reader waits until writer finishes - guaranteed consistency!

### When to Keep Locks During INSERT

| Scenario | Locks Held | Why? |
|----------|-----------|------|
| Leaf has space | Leaf only | No split needed |
| Leaf full, parent has space | Parent + Leaf | Leaf split, parent update |
| Leaf full, parent full | Grandparent + Parent + Leaf | Cascade split up |
| Root split | All unsafe ancestors | May create new root |

## Testing Strategy

### Test 1: Lock Acquire/Release (Basic)
```
- Single compute acquires lock
- Verifies lock value = compute_id
- Releases lock
- Verifies lock value = 0
```

### Test 2: Lock Contention
```
- 2 computes try to acquire same lock simultaneously
- One succeeds (lock value = its ID)
- Other retries until first releases
- Second succeeds after first release
```

### Test 3: Concurrent Reads (Shared Locks)
```
- Compute 0 performs search (acquires shared locks)
- Compute 1 performs search (acquires shared locks on same nodes)
- Both succeed concurrently
- Verify both read same consistent state
```

### Test 4: Read-Write Contention
```
- Compute 0 starts insert (exclusive lock on leaf)
- Compute 1 tries to search same leaf
- Compute 1 blocked until Compute 0 finishes
- Compute 1 then reads consistent post-insert state
```

### Test 5: Multi-Step Transaction (Split)
```
- Compute performs leaf split (3 steps)
- Another compute tries to INSERT to same subtree
- Second compute blocked by locks until transaction completes
- Verify parent consistency after split
```

### Test 4: Metadata Atomicity
```
- Multiple computes allocate node IDs concurrently
- Verify all get unique IDs (no collisions)
```

## Performance Considerations

### Lock Overhead
- **Extra 8 bytes per node** for lock
- **2x network round-trips** per locked operation (acquire + release)
- **Retry storms** under high contention

### Optimizations

#### 1. Adaptive Backoff
```cpp
void acquire_lock_with_backoff(node_address) {
    int retry_delay = INITIAL_DELAY;  // e.g., 10ns
    
    while (!try_acquire_lock(node_address)) {
        wait(retry_delay);
        retry_delay = min(retry_delay * 2, MAX_DELAY);  // Exponential backoff
    }
}
```

#### 2. Hierarchical Locking
```
Lock entire subtree with single lock on root
- Reduces lock overhead for bulk operations
- Increases contention at subtree roots
```

#### 3. Read/Write Locks (Future)
```cpp
struct NodeLock {
    uint64_t owner_id;
    uint8_t mode;  // 0=unlocked, 1=shared(read), 2=exclusive(write)
    uint8_t reader_count;
};
```

## Next Steps

1. ✅ Define in-memory lock structure (NodeLock)
2. ✅ Add lock operations to memory server
3. [ ] Implement lock acquire/release in memory server
4. [ ] Add lock protocol to compute server insert/search
5. [ ] Implement shared metadata management
6. [ ] Test lock contention with 2+ compute servers
7. [ ] Measure performance overhead
