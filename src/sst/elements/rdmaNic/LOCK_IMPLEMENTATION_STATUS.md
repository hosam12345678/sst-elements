# Shared/Exclusive Lock Implementation Summary

## What We've Implemented

### 1. Lock Data Structure (memoryServer.h)

**In-Memory Lock Format:**
```cpp
struct NodeLock {
    uint64_t state;  // 8 bytes at start of each node
    
    // Encoding:
    // 0                    = Unlocked
    // 1-0x7FFFFFFFFFFFFFFF = Shared (reader count)
    // 0x8000000000000000|id = Exclusive (high bit + owner)
};
```

**Memory Layout:**
```
Node Address:
┌─────────────┬────────────────────────┐
│ Lock        │  Node Data            │
│ (8 bytes)   │  (120+ bytes)         │
└─────────────┴────────────────────────┘
  0x10000000    0x10000008
```

### 2. Lock Operations (memoryServer.cc)

**Implemented Functions:**

#### Shared Locks (for SEARCH/reads):
- `bool try_acquire_shared_lock(uint64_t node_address)`
  - If unlocked → set state=1 (first reader)
  - If shared → increment count (concurrent reader)
  - If exclusive → return false (blocked)

- `bool release_shared_lock(uint64_t node_address)`
  - Decrement reader count
  - If count=1 → unlock (last reader)

#### Exclusive Locks (for INSERT/writes):
- `bool try_acquire_exclusive_lock(uint64_t node_address, uint64_t compute_id)`
  - If unlocked → set state=(0x8000...|compute_id)
  - If locked (any mode) → return false (blocked)
  - Includes race detection (verify after write)

- `bool release_exclusive_lock(uint64_t node_address, uint64_t compute_id)`
  - Verify owner matches
  - Set state=0 (unlock)

#### Query Functions:
- `uint64_t read_lock_state(uint64_t node_address)`
- `bool is_locked_shared(uint64_t node_address)`
- `bool is_locked_exclusive(uint64_t node_address)`

### 3. Statistics Tracking

**Lock Statistics:**
- `locks_acquired` - Total locks granted
- `locks_released` - Total locks released
- `lock_conflicts` - Lock denials (contention)
- `lock_wait_time` - Time waiting for locks

**Visual Indicators (verbose >= 3):**
- 🔓→🔐 Unlocked → Exclusive
- 🔐→🔓 Exclusive → Unlocked
- 🔓→🔒 Unlocked → Shared
- 🔒+ Shared count increment
- 🔒- Shared count decrement
- 🔒→🔓 Shared → Unlocked (last reader)
- ❌ Lock denied (conflict)

## How It Works

### Scenario 1: Concurrent Reads (Both Succeed)
```
Time  Compute 0             Compute 1             Lock State
─────────────────────────────────────────────────────────────
T0    acquire_shared()      -                     1 (reader)
T1    -                     acquire_shared()      2 (readers)
T2    read node             read node             2 (both reading)
T3    release_shared()      -                     1 (reader)
T4    -                     release_shared()      0 (unlocked)
```

### Scenario 2: Read-Write Conflict (Write Blocks)
```
Time  Compute 0             Compute 1             Lock State
─────────────────────────────────────────────────────────────
T0    acquire_shared()      -                     1 (reader)
T1    -                     acquire_exclusive()   1 (DENIED ❌)
T2    read node             retry...              1
T3    release_shared()      retry...              0
T4    -                     acquire_exclusive()   0x8000...1 (SUCCESS ✓)
```

### Scenario 3: Write-Write Conflict (Serialized)
```
Time  Compute 0             Compute 1             Lock State
─────────────────────────────────────────────────────────────
T0    acquire_exclusive()   -                     0x8000...0
T1    write node            acquire_exclusive()   0x8000...0 (DENIED ❌)
T2    split operation       retry...              0x8000...0
T3    release_exclusive()   retry...              0
T4    -                     acquire_exclusive()   0x8000...1 (SUCCESS ✓)
```

## Next Steps to Complete Implementation

### Phase 1: Compute Server Lock Integration ⏳
Need to add to `computeServer.cc`:

1. **Lock acquire with retry loop:**
```cpp
void acquire_shared_lock_with_retry(uint64_t node_address) {
    while (!try_acquire_shared_lock(node_address)) {
        wait(LOCK_RETRY_DELAY);  // e.g., 10ns
    }
}

void acquire_exclusive_lock_with_retry(uint64_t node_address) {
    while (!try_acquire_exclusive_lock(node_address, node_id)) {
        wait(LOCK_RETRY_DELAY);
    }
}
```

2. **Track held locks during operation:**
```cpp
struct AsyncOperation {
    // ... existing fields ...
    std::vector<uint64_t> held_locks;  // Addresses we currently hold locks on
    std::vector<bool> lock_types;      // true=exclusive, false=shared
};
```

3. **Integrate into INSERT (exclusive locks):**
```cpp
void btree_insert_async(key, value) {
    // Lock root
    acquire_exclusive_lock_with_retry(root_address);
    op.held_locks.push_back(root_address);
    
    // Read node (skip lock header)
    read_node(root_address + LOCK_HEADER_SIZE);
    
    // Traverse with lock crabbing...
}
```

4. **Integrate into SEARCH (shared locks):**
```cpp
void btree_search_async(key) {
    // Lock root (shared)
    acquire_shared_lock_with_retry(root_address);
    op.held_locks.push_back(root_address);
    
    // Read and traverse...
}
```

5. **Release locks after operation:**
```cpp
void cleanup_operation(AsyncOperation& op) {
    for (size_t i = 0; i < op.held_locks.size(); i++) {
        if (op.lock_types[i]) {
            release_exclusive_lock(op.held_locks[i], node_id);
        } else {
            release_shared_lock(op.held_locks[i]);
        }
    }
}
```

### Phase 2: Shared Metadata Management ⏳

Add metadata block at base address (0x10000000):
```cpp
struct BTreeMetadata {
    NodeLock lock;           // 8 bytes (lock for metadata)
    uint64_t root_address;   // 8 bytes
    uint64_t next_node_id;   // 8 bytes
    uint32_t tree_height;    // 4 bytes
    // Total: 28 bytes
};

// Atomic operations on metadata
uint64_t allocate_node_id_atomic(compute_id) {
    metadata_addr = 0x10000000;
    acquire_exclusive_lock(metadata_addr, compute_id);
    next_id = read(metadata_addr + 16);  // Skip lock + root_address
    write(metadata_addr + 16, next_id + 1);
    release_exclusive_lock(metadata_addr, compute_id);
    return next_id;
}
```

### Phase 3: Testing ⏳

Run tests:
```bash
cd tests
sst test_locks_basic.py     # Basic lock operations
sst test_dist_02_many_to_one.py  # N:1 with locks (update to use locks)
```

Expected results:
- Lock statistics show conflicts when expected
- No data corruption
- Correct serialization of conflicting operations

### Phase 4: Performance Tuning ⏳

1. **Measure lock overhead:**
   - Compare locked vs unlocked performance
   - Identify hot spots (root node)

2. **Optimize retry strategy:**
   - Exponential backoff
   - Adaptive delays based on contention

3. **Consider optimizations:**
   - Lock-free root for reads (B-link trees)
   - Batch lock acquisition
   - Lock escalation/de-escalation

## Current Status

✅ **Completed:**
- Lock data structure defined
- Shared/exclusive lock operations implemented
- Lock statistics tracking
- Visual debug output

⏳ **In Progress:**
- Compute server integration (lock acquire/release protocol)
- Shared metadata management
- Testing with concurrent operations

📋 **TODO:**
- Lock crabbing implementation
- Multi-step transaction coordination
- Performance benchmarking
- Deadlock prevention (timeout-based)

## Files Modified

1. `memoryServer.h` - Lock structures and function declarations
2. `memoryServer.cc` - Lock operation implementations
3. `IN_MEMORY_LOCKS.md` - Design documentation
4. `test_locks_basic.py` - Basic lock test (created)

## How to Test Current Implementation

Once compute server integration is complete:
```bash
cd /workspaces/sst-elements/src/sst/elements/rdmaNic/tests
/workspaces/sst/bin/sst test_locks_basic.py
```

Look for:
- 🔓→🔐 and 🔓→🔒 lock acquisitions
- ❌ lock conflicts showing proper blocking
- Statistics: locks_acquired, locks_released, lock_conflicts
