# B+Tree Unit Test Suite

## Overview
Comprehensive unit tests for disaggregated B+tree implementation with 1 compute + 1 memory server configuration.

## Test Files Created

### ✅ Test 1: Insert into Empty Tree
**File:** `test_unit_01_empty_tree.py`
**Purpose:** Verify basic tree initialization and first insert
**Configuration:**
- 1 compute + 1 memory server
- Fanout: 4
- Key range: 1 (single key)
- Operations: Multiple inserts of same key

**Expected Behavior:**
1. Tree initializes with empty root (leaf) at height=1
2. First insert adds key to root
3. Duplicate inserts update existing value
4. Tree height remains 1

**Success Criteria:**
- ✓ Tree initialization message
- ✓ Root address: 0x10000000
- ✓ INSERT operations complete
- ✓ Duplicate key messages

---

### ✅ Test 2: Insert Causing Leaf Split (Root Split)
**File:** `test_unit_02_leaf_split.py`
**Purpose:** Verify leaf split when root fills up
**Configuration:**
- 1 compute + 1 memory server
- Fanout: 4
- Key range: 10
- Operations: ~20 inserts

**Expected Behavior:**
1. Insert keys 0,1,2,3 → root fills (4 keys)
2. Insert key 4 → triggers ROOT SPLIT
3. Old root moves to new address
4. New root created as internal node
5. Tree height: 1 → 2

**Success Criteria:**
- ✓ "Leaf FULL" message at 4 keys
- ✓ "Splitting ROOT node" message
- ✓ "Moving old root to new address"
- ✓ "New root created" message
- ✓ Tree height increases to 2

---

### ✅ Test 3: Insert Causing Internal Node Split
**File:** `test_unit_03_internal_split.py`
**Purpose:** Verify internal node split with recursive propagation
**Configuration:**
- 1 compute + 1 memory server
- Fanout: 4
- Key range: 30
- Operations: ~100 inserts

**Expected Behavior:**
1. Build to height 2 (root internal + leaves)
2. Fill leaves until root internal has 4 children
3. Next split tries to add 5th child → root internal FULL
4. Internal node split occurs
5. Tree height: 2 → 3

**Tree Growth Pattern (fanout=4):**
- Keys 0-3: Root leaf (4 keys)
- Key 4: Root splits → height 2
- Key 8: 2nd leaf splits → root has 3 children
- Key 12: 3rd leaf splits → root has 4 children (FULL)
- Key 16: 4th leaf splits → root internal FULL → INTERNAL SPLIT → height 3

**Success Criteria:**
- ✓ Multiple "LEAF SPLIT" messages
- ✓ "Parent has space" messages (early splits)
- ✓ "Parent FULL" message (key 16)
- ✓ "INTERNAL SPLIT" message
- ✓ Tree height increases to 3

---

### ✅ Test 4: Root Split Behavior
**File:** `test_unit_04_root_split.py`
**Purpose:** Verify root address management during split
**Configuration:**
- 1 compute + 1 memory server
- Fanout: 4
- Key range: 8
- Focus: Address allocation

**Expected Behavior:**
1. Initial root at 0x10000000
2. Root fills with 4 keys
3. 5th key triggers ROOT SPLIT:
   - Old root moves to NEW address (not 0x10000000)
   - New root created at NEW address
   - root_address variable updated
4. No address conflicts (child[0] ≠ child[1])

**Success Criteria:**
- ✓ Initial root: 0x10000000
- ✓ "Splitting ROOT node"
- ✓ "Moving old root to new address"
- ✓ New root at different address
- ✓ Unique child addresses

---

### ✅ Test 5: Search in Single-Level Tree
**File:** `test_unit_05_search_single_level.py`
**Purpose:** Verify search in simplest tree (height=1)
**Configuration:**
- 1 compute + 1 memory server
- Fanout: 16 (no splits)
- Key range: 10
- Operations: 50% inserts, 50% searches

**Expected Behavior:**
1. Tree has only root leaf
2. Inserts add keys to root
3. Searches access only root (1 network read)
4. Existing keys → FOUND
5. Missing keys → NOT FOUND

**Success Criteria:**
- ✓ Tree height=1 throughout
- ✓ Mix of INSERT and SEARCH
- ✓ "FOUND" messages for existing keys
- ✓ "NOT FOUND" for missing keys
- ✓ Each search = 1 network read

---

### ✅ Test 6: Search in Multi-Level Tree
**File:** `test_unit_06_search_multi_level.py`
**Purpose:** Verify search traversal through multiple levels
**Configuration:**
- 1 compute + 1 memory server
- Fanout: 4
- Key range: 15
- Operations: 70% inserts, 30% searches

**Expected Behavior:**
1. Build tree to height 2+
2. Search traversal:
   - Read root (internal)
   - Find child pointer
   - Read leaf
   - Search in leaf
3. Network reads = tree height

**Search Example (height 2):**
```
Search key=7:
  Level 0: Read root (internal) with keys [4,8,12]
           Key 7 between 4 and 8 → child[1]
  Level 1: Read leaf with keys [4,5,6,7]
           Found key=7 at position 3
```

**Success Criteria:**
- ✓ Tree height 2+
- ✓ "Level 0: Read node" (root)
- ✓ "→ Continue to child[X]"
- ✓ "Level 1: Read node" (leaf)
- ✓ "✓ Reached leaf"
- ✓ Correct child selection

---

### ✅ Test 7: Duplicate Key Insertion
**File:** `test_unit_07_duplicate_keys.py`
**Purpose:** Verify duplicate key handling (update policy)
**Configuration:**
- 1 compute + 1 memory server
- Fanout: 16 (no splits)
- Key range: 3 (forces duplicates)
- Operations: 100% inserts

**Expected Behavior:**
1. First insert of key → new entry
2. Duplicate insert → UPDATE value (no new entry)
3. Node key count stays same
4. Latest value overwrites old value

**Duplicate Scenario:**
```
Insert key=0, value=100   → New entry, num_keys=1
Insert key=0, value=200   → Duplicate! Update existing
                             "⚠️ Duplicate key=0 - updating value"
                             num_keys stays 1
                             key=0 now has value=200
```

**Success Criteria:**
- ✓ "Duplicate key=X" messages
- ✓ Key count stays at unique keys only
- ✓ No array overflow
- ✓ No unnecessary splits
- ✓ Values get updated

---

## Running the Tests

### Run Individual Test
```bash
cd /workspaces/sst-elements/src/sst/elements/rdmaNic/tests
/workspaces/sst/bin/sst test_unit_01_empty_tree.py
```

### Run All Unit Tests
```bash
for test in test_unit_*.py; do
    echo "Running $test..."
    /workspaces/sst/bin/sst "$test"
    echo ""
done
```

### Expected Timeline
- Test 1: ~100ms simulation
- Test 2: ~200ms simulation
- Test 3: ~500ms simulation (most comprehensive)
- Test 4: ~150ms simulation
- Test 5: ~400ms simulation
- Test 6: ~500ms simulation
- Test 7: ~300ms simulation

---

## Next Steps

### Phase 2: Many-to-One Testing
After confirming 1:1 works, expand to:
- **N compute + 1 memory:** Test concurrent operations
- Config: 4 compute servers, 1 memory server
- Focus: Concurrency, request ordering

### Phase 3: One-to-Many Testing
- **1 compute + M memory:** Test load distribution
- Config: 1 compute server, 4 memory servers
- Focus: Node distribution across memory servers

### Phase 4: Many-to-Many Testing
- **N compute + M memory:** Full distributed system
- Config: 4 compute servers, 4 memory servers
- Focus: Full system integration

---

## Test Coverage Matrix

| Test | Empty Tree | Leaf Split | Internal Split | Root Split | Search (H=1) | Search (H>1) | Duplicates |
|------|-----------|-----------|----------------|-----------|-------------|-------------|-----------|
| 1    | ✅        | -         | -              | -         | -           | -           | ✅        |
| 2    | -         | ✅        | -              | ✅        | -           | -           | -         |
| 3    | -         | ✅        | ✅             | ✅        | -           | -           | -         |
| 4    | -         | -         | -              | ✅        | -           | -           | -         |
| 5    | -         | -         | -              | -         | ✅          | -           | -         |
| 6    | -         | ✅        | -              | -         | -           | ✅          | -         |
| 7    | -         | -         | -              | -         | -           | -           | ✅        |

---

## Success Criteria Summary

All tests should:
1. ✅ Complete without crashes
2. ✅ Show expected log messages
3. ✅ Maintain correct tree height
4. ✅ Allocate unique node addresses
5. ✅ Handle splits correctly
6. ✅ Traverse tree properly
7. ✅ Update statistics correctly

## Known Issues to Watch For
- ❌ Address collisions (child[0] = child[1])
- ❌ Incorrect tree height
- ❌ Missing split messages
- ❌ Parent not found errors
- ❌ Key ordering violations
