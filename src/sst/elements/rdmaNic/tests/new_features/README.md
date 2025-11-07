# Root Metadata Locking - New Feature Tests

This directory contains tests for the new **Root Metadata Locking** feature, which implements distributed B+tree coordination using a dedicated metadata node with reader-writer locking.

## Feature Overview

### Problem Solved
Previously, each ComputeServer maintained local copies of `root_address` and `tree_height`. This caused race conditions when multiple nodes tried to modify the tree structure simultaneously, leading to stale metadata and incorrect operations.

### Solution Implemented
- **Dedicated Root Metadata Node** at `MEMORY_BASE_ADDRESS` (Memory Server 0)
- **32-byte metadata structure**: Lock(8) + root_ptr(8) + tree_height(4) + reserved(12)
- **VALIDITY_BIT** at offset 64 for initialization synchronization
- **Reader-Writer Locking Protocol**:
  - SHARED locks for reads (concurrent searches)
  - EXCLUSIVE locks for root splits (atomic metadata updates)
- **No local caching** - all operations read fresh metadata from memory

### Memory Layout
```
Memory Server 0:
  [0x10000000 + 0x0000] ROOT_METADATA (32 bytes)
  [0x10000000 + 0x0040] VALIDITY_BIT (1 byte)
  [0x10000000 + 0x1000] Chunk Pool Start (128 × 8MB chunks)

Each Memory Server:
  Reserved Space:     0x1000 (4KB)
  Chunk Pool:         128 × 8MB = 1GB
  Total Address Space: 0x40001000 per server
```

## Test Structure

All tests follow the standardized format from `incremental/` tests:
- Clear documentation of test objective
- Phase identification
- Expected behavior description
- Validation criteria
- Consistent component configuration
- Network link setup (many-to-many mesh)

## Test Files

### Test 1.1: Memory Layout Validation
**File**: `test_1_1_memory_layout.py`

**Objective**: Verify ComputeServer correctly understands MemoryServer's memory layout

**Configuration**:
- 2 Compute Servers × 2 Memory Servers
- Minimal operations (just initialization)
- Verbose level = 5

**Expected Output**:
```
Memory layout validated:
  ADDRESS_SPACE_PER_SERVER = 0x40001000 (1024 MB)
  RESERVED_METADATA_SIZE = 0x1000 (4096 bytes)
  ROOT_METADATA at 0x10000000 (server 0)
  VALIDITY_BIT at 0x10000040 (server 0)
```

**Validates**:
- GET_MEMORY_SERVER macro calculates correct server IDs
- Address routing works properly
- Reserved metadata space recognized
- No fatal layout mismatch errors

---

### Test 2.1: Single Node Initialization
**File**: `test_2_1_single_init.py`

**Objective**: First compute node initializes B+tree correctly

**Configuration**:
- 1 Compute Server × 4 Memory Servers
- 100% INSERT workload (read_ratio=0.0)
- 100 ops/sec, 500ms duration

**Expected Sequence**:
1. Check VALIDITY_BIT → returns 0 (uninitialized)
2. Allocate root node from Memory Server
3. Initialize root as leaf node
4. Write ROOT_METADATA (root_address, tree_height=1)
5. Write VALIDITY_BIT = 1
6. Complete INSERT operation

**Validates**:
- Single-node initialization works correctly
- VALIDITY_BIT atomic check
- ROOT_METADATA write
- Proper root node allocation
- No race conditions with single node

---

### Test 2.2: Multi-Node Race Prevention (CRITICAL)
**File**: `test_2_2_race_prevention.py`

**Objective**: Multiple compute nodes don't race during initialization

**Configuration**:
- 4 Compute Servers × 4 Memory Servers (all start at t=0)
- 100% INSERT workload on all nodes
- 1000 ops/sec per node (aggressive), 1 second duration

**Expected Behavior**:
1. All 4 nodes start simultaneously
2. All 4 nodes check VALIDITY_BIT (initially 0)
3. **ONE node wins race**, initializes tree
4. Other 3 nodes detect VALIDITY_BIT=1, skip initialization
5. All 4 nodes read ROOT_METADATA
6. All operations complete successfully

**Validates** (CRITICAL):
- Atomic VALIDITY_BIT check-and-set prevents double initialization
- No duplicate root allocations
- No metadata corruption
- All nodes see consistent ROOT_METADATA
- Race condition prevention works correctly

**⚠️ This is the most critical test** - it validates the core synchronization mechanism.

---

## Running Tests

### Individual Test
```bash
cd /workspaces/sst-elements/src/sst/elements/rdmaNic/tests/new_features
sst test_1_1_memory_layout.py
sst test_2_1_single_init.py
sst test_2_2_race_prevention.py
```

### All Tests
```bash
cd /workspaces/sst-elements/src/sst/elements/rdmaNic/tests
./run_tests.sh  # If test runner script exists
```

## Success Criteria

### Must Pass
- ✅ Test 1.1: No memory layout errors
- ✅ Test 2.1: Single node initializes correctly
- ✅ Test 2.2: Exactly one initialization message (race prevention works)

### Output Validation
For Test 2.2, verify in output:
```bash
# Should appear exactly ONCE:
grep -c "Initializing B+tree" output.log
# Expected: 1

# Should NOT appear:
grep "FATAL" output.log
grep "duplicate root" output.log
grep "memory layout validation FAILED" output.log
```

## Code Changes Summary

### Files Modified
1. **computeServer.cc**:
   - Removed local `root_address` and `tree_height` variables
   - Added `read_root_metadata_async()` - reads metadata before each operation
   - Added `update_root_metadata_async()` - writes metadata after root splits
   - Added startup validation in constructor

2. **memoryServer.h**:
   - Added public constants: `RESERVED_METADATA_SIZE`, `ADDRESS_SPACE_PER_SERVER`
   - Removed unused `memory_capacity_gb` and `btree_node_size` parameters
   - Removed unused `memory_latency_ns` parameter

3. **memoryServer.cc**:
   - Removed phase 0 initialization code
   - Updated address calculations to use named constants
   - Removed dead code (`store_btree_node`, `load_btree_node`)

### Constants Defined
```cpp
// computeServer.cc
#define ROOT_METADATA_ADDRESS (MEMORY_BASE_ADDRESS)
#define ROOT_METADATA_SIZE 32
#define VALIDITY_BIT_OFFSET 64
#define VALIDITY_BIT_ADDRESS (MEMORY_BASE_ADDRESS + VALIDITY_BIT_OFFSET)

// memoryServer.h
static constexpr uint64_t RESERVED_METADATA_SIZE = 0x1000;  // 4KB
static constexpr uint64_t CHUNK_SIZE = 8 * 1024 * 1024;     // 8MB
static constexpr uint64_t CHUNKS_PER_SERVER = 128;          // 128 chunks
static constexpr uint64_t ADDRESS_SPACE_PER_SERVER = 
    RESERVED_METADATA_SIZE + (CHUNKS_PER_SERVER * CHUNK_SIZE);
```

## Next Steps

After these tests pass:
1. Test root split operations (metadata updates)
2. Test concurrent readers (SHARED lock performance)
3. Test concurrent root splits (EXCLUSIVE lock conflicts)
4. Stress tests with high concurrency
5. Correctness validation (sequential and distributed consistency)

## References
- See `BTREE_TEST_PLAN.md` for comprehensive test plan
- See `ASYNC_REFACTOR_SUMMARY.md` for async operation architecture
- See parent directory `incremental/` for test structure examples
