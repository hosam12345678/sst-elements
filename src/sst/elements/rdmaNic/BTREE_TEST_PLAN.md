# B+Tree Root Metadata Locking - Test Plan

## Overview
This test plan validates the new root metadata node implementation with distributed locking for B+tree operations.

## Test Environment
- **Compute Servers**: Multiple (2, 4, 8 nodes)
- **Memory Servers**: 4 nodes
- **B+tree Fanout**: 16
- **Key Range**: Variable per test

---

## Test Suite 1: Memory Layout Validation

### Test 1.1: Startup Validation
**Objective**: Verify ComputeServer understands MemoryServer's memory layout

**Setup**:
- 1 Compute Server, 4 Memory Servers
- Verbose level = 1

**Expected Output**:
```
Memory layout validated:
  ADDRESS_SPACE_PER_SERVER = 0x40001000 (1024 MB)
  RESERVED_METADATA_SIZE = 0x1000 (4096 bytes)
  ROOT_METADATA at 0x10000000 (server 0)
  VALIDITY_BIT at 0x10000040 (server 0)
```

**Pass Criteria**:
- No fatal errors about memory layout mismatch
- Correct server IDs calculated for ROOT_METADATA (0) and VALIDITY_BIT (0)
- ADDRESS_SPACE_PER_SERVER matches: RESERVED_METADATA_SIZE + (CHUNKS_PER_SERVER * CHUNK_SIZE)

---

### Test 1.2: Address Routing Validation
**Objective**: Verify GET_MEMORY_SERVER macro routes addresses correctly

**Setup**:
- 1 Compute Server, 4 Memory Servers
- Insert operations to different memory servers

**Test Cases**:
| Address | Expected Server | Calculation |
|---------|----------------|-------------|
| 0x10000000 | 0 | (0x10000000 - 0x10000000) / 0x40001000 = 0 |
| 0x50001000 | 1 | (0x50001000 - 0x10000000) / 0x40001000 = 1 |
| 0x90002000 | 2 | (0x90002000 - 0x10000000) / 0x40001000 = 2 |
| 0xD0003000 | 3 | (0xD0003000 - 0x10000000) / 0x40001000 = 3 |

**Pass Criteria**:
- All addresses route to correct memory server
- No "Address validation failed" errors
- Operations complete successfully

---

### Test 1.3: Reserved Metadata Space
**Objective**: Verify first 4KB of Memory Server 0 is reserved

**Setup**:
- 1 Compute Server, 4 Memory Servers
- Allocate chunks and verify addresses

**Test Cases**:
1. ROOT_METADATA at offset 0 (32 bytes)
2. VALIDITY_BIT at offset 64 (1 byte)
3. First chunk starts at base_address + 0x1000

**Pass Criteria**:
- ROOT_METADATA writes go to 0x10000000
- VALIDITY_BIT writes go to 0x10000040
- First allocated chunk >= 0x10001000
- No chunk addresses in [0x10000000, 0x10001000) range

---

## Test Suite 2: B+Tree Initialization

### Test 2.1: Single Node Initialization
**Objective**: First compute node initializes B+tree

**Setup**:
- 1 Compute Server, 4 Memory Servers
- First operation: INSERT key=100

**Expected Sequence**:
1. Check VALIDITY_BIT at 0x10000040 → returns 0 (uninitialized)
2. Allocate root node from Memory Server 0
3. Initialize root as leaf node
4. Write root node data
5. Write ROOT_METADATA (root_address, tree_height=1)
6. Write VALIDITY_BIT = 1
7. Complete INSERT operation

**Pass Criteria**:
- Tree initialized exactly once
- VALIDITY_BIT transitions 0 → 1
- ROOT_METADATA contains valid address and height=1
- Root node is valid leaf node with key=100

---

### Test 2.2: Multi-Node Race Prevention
**Objective**: Multiple compute nodes don't race during initialization

**Setup**:
- 4 Compute Servers (all start simultaneously)
- 4 Memory Servers
- Each node issues INSERT at time=0

**Expected Behavior**:
- Only ONE node completes initialization (atomic VALIDITY_BIT check)
- Other 3 nodes detect VALIDITY_BIT=1 and skip initialization
- All 4 nodes eventually read ROOT_METADATA
- All 4 operations complete successfully

**Pass Criteria**:
- Exactly 1 "Initializing B+tree" message
- No duplicate root allocations
- VALIDITY_BIT written exactly once
- All 4 keys inserted (no duplicates, no failures)

---

### Test 2.3: Post-Initialization Behavior
**Objective**: Operations after initialization skip init sequence

**Setup**:
- 1 Compute Server, 4 Memory Servers
- Run 1000 operations after initialization

**Expected Behavior**:
- First operation initializes tree
- Subsequent 999 operations:
  - Read VALIDITY_BIT → 1
  - Skip initialization
  - Read ROOT_METADATA
  - Continue with normal operation

**Pass Criteria**:
- Only 1 initialization
- 1000 VALIDITY_BIT reads
- 999 reads return 1 (initialized)
- No redundant root allocations

---

## Test Suite 3: Root Metadata Reading

### Test 3.1: SHARED Lock Acquisition
**Objective**: Multiple readers acquire SHARED lock concurrently

**Setup**:
- 4 Compute Servers, 4 Memory Servers
- All nodes run SEARCH operations simultaneously
- Verbose level = 2

**Expected Behavior**:
- All 4 nodes read ROOT_METADATA with SHARED lock
- Lock state shows reader_count = 4 (or various counts during execution)
- No lock conflicts for concurrent reads
- All SEARCH operations succeed

**Pass Criteria**:
- Multiple "Reading root metadata" messages
- Lock manager shows SHARED locks acquired
- No "lock acquisition failed" for reads
- All searches complete successfully

---

### Test 3.2: Root Metadata Cache-Less Reads
**Objective**: Every operation reads fresh metadata (no local caching)

**Setup**:
- 2 Compute Servers, 4 Memory Servers
- Node 0: INSERT operations (causes root splits)
- Node 1: SEARCH operations (concurrent)

**Test Sequence**:
1. Insert 1000 keys on Node 0 (triggers root splits)
2. Node 1 searches every 10ms
3. Monitor tree_height reads on Node 1

**Expected Behavior**:
- Node 1 sees tree_height increase (1 → 2 → 3)
- Each operation on Node 1 reads ROOT_METADATA fresh
- No stale tree_height values used

**Pass Criteria**:
- Node 1 detects tree_height changes
- No "invalid tree_height" errors
- All searches find correct keys (or correctly report not found)

---

### Test 3.3: Metadata Read Performance
**Objective**: Measure overhead of reading metadata per operation

**Setup**:
- 1 Compute Server, 4 Memory Servers
- 10,000 SEARCH operations
- Measure total latency

**Metrics**:
- Total operations: 10,000
- Metadata reads: 10,000 (1 per operation)
- Average latency per operation
- Metadata read latency vs data read latency

**Pass Criteria**:
- All operations complete
- Metadata read adds < 10% overhead
- No failed metadata reads

---

## Test Suite 4: Root Split Handling

### Test 4.1: Single Root Split
**Objective**: Root split updates ROOT_METADATA atomically

**Setup**:
- 1 Compute Server, 4 Memory Servers
- Fanout = 4 (small, for easier testing)
- Insert 5 keys (triggers root split)

**Expected Sequence**:
1. Insert keys 1,2,3,4 → root is leaf
2. Insert key 5 → root split triggered
3. Acquire EXCLUSIVE lock on ROOT_METADATA
4. Create new internal root
5. Update ROOT_METADATA (new root address, tree_height=2)
6. Release EXCLUSIVE lock
7. Complete operation

**Pass Criteria**:
- tree_height transitions 1 → 2
- ROOT_METADATA.root_address points to new internal node
- EXCLUSIVE lock acquired and released
- New root has 2 children (old root split)

---

### Test 4.2: Concurrent Root Split Prevention
**Objective**: Only one node can split root at a time

**Setup**:
- 2 Compute Servers, 4 Memory Servers
- Fanout = 4
- Both nodes insert keys simultaneously to trigger root split

**Expected Behavior**:
- First node acquires EXCLUSIVE lock on ROOT_METADATA
- Second node waits (EXCLUSIVE lock blocks)
- First node completes root split
- Second node acquires lock, re-reads metadata, continues
- Only ONE root split occurs (atomic update)

**Pass Criteria**:
- Exactly 1 root split
- No duplicate root nodes
- Second operation either:
  - Finds root already split (inserts into new structure)
  - Splits a different node (not root anymore)
- Final tree structure is valid

---

### Test 4.3: Root Split with Concurrent Readers
**Objective**: SHARED readers don't block root split

**Setup**:
- 4 Compute Servers, 4 Memory Servers
- Node 0: INSERT (triggers root split)
- Nodes 1-3: Continuous SEARCH operations

**Expected Behavior**:
- Nodes 1-3 acquire SHARED locks (concurrent reads)
- Node 0 requests EXCLUSIVE lock for root split
- Node 0 waits for SHARED locks to release
- SHARED locks released, EXCLUSIVE acquired
- Root split completes
- Subsequent SHARED readers see new metadata

**Pass Criteria**:
- Root split completes successfully
- SEARCH operations see either old or new tree_height (no invalid states)
- No deadlocks
- All operations complete

---

## Test Suite 5: Stress Tests

### Test 5.1: High Concurrency
**Objective**: Many nodes accessing ROOT_METADATA simultaneously

**Setup**:
- 8 Compute Servers, 4 Memory Servers
- Mixed workload: 95% SEARCH, 5% INSERT
- 10,000 operations per node
- Duration: 10 seconds

**Metrics**:
- Total metadata reads
- Total metadata updates (root splits)
- Lock conflicts (EXCLUSIVE lock wait time)
- Operations per second

**Pass Criteria**:
- All 80,000 operations complete
- No deadlocks
- No corrupted metadata
- Final tree is valid B+tree

---

### Test 5.2: Rapid Root Splits
**Objective**: Sequential inserts causing many root splits

**Setup**:
- 1 Compute Server, 4 Memory Servers
- Fanout = 4
- Insert 1000 sequential keys

**Expected Behavior**:
- Multiple root splits (log_4(1000) ≈ 5 splits)
- tree_height increases: 1 → 2 → 3 → 4 → 5
- Each split updates ROOT_METADATA

**Pass Criteria**:
- Final tree_height matches expected
- All 1000 keys inserted
- ROOT_METADATA updated for each split
- Tree structure valid at each height

---

### Test 5.3: Interleaved Operations
**Objective**: INSERT and SEARCH interleaved with root splits

**Setup**:
- 4 Compute Servers, 4 Memory Servers
- Nodes 0-1: INSERT operations (sequential)
- Nodes 2-3: SEARCH operations (random)
- 5000 operations total per node

**Test Pattern**:
- T=0-100ms: Heavy inserts (trigger root splits)
- T=100-200ms: Heavy searches (read new metadata)
- T=200-300ms: Mixed workload

**Pass Criteria**:
- All searches find inserted keys (or correctly report not found)
- No stale metadata errors
- No lock deadlocks
- Final tree valid

---

## Test Suite 6: Failure Cases

### Test 6.1: Invalid Metadata Detection
**Objective**: Detect corrupted ROOT_METADATA

**Setup**:
- Manually corrupt ROOT_METADATA (invalid address or height)
- Run SEARCH operation

**Expected Behavior**:
- Operation reads corrupted metadata
- Detects invalid tree_height (e.g., 0 or > 100)
- Logs error and aborts operation

**Pass Criteria**:
- Error detected
- No segfaults or crashes
- Operation fails gracefully

---

### Test 6.2: Lock Timeout (Future)
**Objective**: Detect lock acquisition timeout

**Setup**:
- Simulate stuck EXCLUSIVE lock holder
- Other nodes attempt to acquire lock

**Expected Behavior**:
- Lock acquisition times out after threshold
- Error logged
- Operation retries or aborts

**Pass Criteria**:
- No infinite waiting
- Timeout detected
- System doesn't hang

---

## Test Suite 7: Correctness Validation

### Test 7.1: Sequential Consistency
**Objective**: Operations see consistent view of tree

**Setup**:
- 1 Compute Server, 4 Memory Servers
- Insert keys: 1,2,3,...,1000
- After each insert, search all previous keys

**Pass Criteria**:
- All previous keys found
- No false negatives
- No stale root metadata used

---

### Test 7.2: Distributed Consistency
**Objective**: All nodes see same tree structure

**Setup**:
- 4 Compute Servers, 4 Memory Servers
- Node 0: Insert 1000 keys
- Nodes 1-3: Wait, then search all 1000 keys

**Pass Criteria**:
- All nodes read same ROOT_METADATA
- All nodes find all 1000 keys
- No inconsistencies

---

## Test Execution Plan

### Phase 1: Basic Validation (Tests 1.x, 2.1)
- Verify memory layout
- Single-node initialization
- **Duration**: 30 minutes

### Phase 2: Multi-Node Coordination (Tests 2.2-2.3, 3.1-3.2)
- Race prevention
- Concurrent reads
- **Duration**: 1 hour

### Phase 3: Root Split Operations (Tests 4.x)
- Single and concurrent root splits
- **Duration**: 1 hour

### Phase 4: Stress & Correctness (Tests 5.x, 7.x)
- High concurrency
- Consistency validation
- **Duration**: 2 hours

### Phase 5: Failure Cases (Tests 6.x)
- Error handling
- **Duration**: 30 minutes

---

## Success Criteria

### Must Pass:
- All Test Suite 1 (Memory Layout)
- All Test Suite 2 (Initialization)
- Test 3.1, 3.2 (Metadata reads)
- Test 4.1 (Single root split)
- Test 7.1, 7.2 (Correctness)

### Should Pass:
- Test 4.2, 4.3 (Concurrent root splits)
- Test 5.1, 5.3 (High concurrency)

### Nice to Have:
- Test 5.2 (Rapid splits)
- Test 6.x (Failure cases)

---

## Test Implementation

Each test should:
1. Generate SST configuration file (Python script)
2. Run `sst test_config.py`
3. Capture output
4. Validate against expected behavior
5. Report PASS/FAIL

## Next Steps

1. Implement Test 1.1 first (memory layout validation)
2. Run and verify output
3. Proceed incrementally through test suite
