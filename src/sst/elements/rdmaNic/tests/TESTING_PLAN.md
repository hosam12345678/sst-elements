# B+Tree Async Implementation - Testing Plan

## Test Strategy
Testing incrementally from simplest to most complex operations.

---

## Phase 1: Basic Functionality ✅ Ready to Test

### Test 1: Single Insert & Search
**File:** `test_01_single_insert_search.py`
**Goal:** Verify basic async insert and search operations work
**Setup:**
- 1 insert, 1 search
- Fanout: 16 (no splits)
- Key range: 10
**Expected Results:**
- ✓ Insert completes successfully
- ✓ Search finds the inserted key
- ✓ No errors or timeouts

### Test 2: Multiple Inserts (No Split)
**File:** `test_02_multiple_inserts.py`
**Goal:** Insert multiple keys without triggering splits
**Setup:**
- ~10 inserts, ~10 searches
- Fanout: 16 (no splits expected)
- Key range: 10
**Expected Results:**
- ✓ All inserts complete
- ✓ All searches find their keys
- ✓ NO split messages
- ✓ Tree height remains 1 (root only)

### Test 3: Sequential vs Random Keys
**File:** `test_03_key_ordering.py`
**Goal:** Verify keys stored in sorted order regardless of insertion order
**Setup:**
- ~20 inserts with uniform distribution (random order)
- ~10 searches
- Fanout: 16
**Expected Results:**
- ✓ Keys inserted in random order
- ✓ All searches succeed
- ✓ Keys appear sorted in node

---

## Phase 2: Edge Cases (To Be Created)

### Test 4: Search Non-existent Keys
**Goal:** Test search misses
**Setup:** Search for keys that don't exist
**Expected:** Graceful handling (search returns not found)

### Test 5: Duplicate Key Inserts
**Goal:** Test key overwrites
**Setup:** Insert same key twice with different values
**Expected:** Second insert overwrites first value

---

## Phase 3: Split Operations ✅ Ready to Test

### Test 6: Simple Leaf Split
**File:** `test_06_simple_leaf_split.py`
**Goal:** Trigger exactly ONE leaf node split
**Setup:**
- ~30 inserts (100% write)
- Fanout: 4 (SMALL - split after 4 keys)
- Key range: 10
**Expected Results:**
- ✓ "Leaf node FULL" message after 4th key
- ✓ Split phase messages:
  - Phase 1: Writing old node
  - Phase 2: Writing new node
  - Creating new root node
- ✓ Tree height increases from 1 → 2

### Test 7: Multiple Leaf Splits (To Be Created)
**Goal:** Multiple splits at same level
**Setup:** Fanout=4, insert 20+ keys
**Expected:** Multiple leaf nodes, height stays 2

### Test 8: Internal Node Split (To Be Created)
**Goal:** Full tree split with internal node split
**Setup:** Fanout=4, insert 50+ keys
**Expected:** Internal node splits, height = 3

---

## Phase 4: Stress Testing (To Be Created)

### Test 9: Mixed Operations
**Goal:** Read/write mix
**Setup:** 50% inserts, 50% searches
**Expected:** All operations complete successfully

### Test 10: High Volume
**Goal:** Performance test
**Setup:** Large key range, many operations
**Expected:** Good throughput, no deadlocks

---

## Test Execution Order

**START HERE:**
1. ✅ Test 1: Single Insert & Search
2. ✅ Test 2: Multiple Inserts
3. ✅ Test 3: Key Ordering

**THEN (if basics work):**
4. ✅ Test 6: Simple Leaf Split

**THEN (if split works):**
5. Create and run Tests 4, 5, 7, 8, 9, 10

---

## How to Run Tests

```bash
# Run a single test
sst test_01_single_insert_search.py

# With more verbose output
sst test_01_single_insert_search.py --verbose

# Save output to file
sst test_01_single_insert_search.py > test1_output.txt 2>&1
```

---

## What to Watch For

### Success Indicators:
- ✓ Operations complete without errors
- ✓ Statistics show expected counts
- ✓ "Operation completed" messages appear
- ✓ No deadlocks or timeouts

### Failure Indicators:
- ✗ "ERROR" messages in output
- ✗ Simulation hangs/doesn't complete
- ✗ Wrong key values returned
- ✗ Memory access violations
- ✗ Assertion failures

---

## Current Status
- **Created:** Tests 1, 2, 3, 6
- **Ready to Run:** Tests 1, 2, 3, 6
- **Next:** Run Test 1 first!
