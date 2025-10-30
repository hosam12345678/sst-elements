# B+Tree Implementation TODO List

## ✅ **Completed Features**

### Core Operations
- [x] Async B+tree INSERT with traversal
- [x] Async B+tree SEARCH with traversal  
- [x] Async B+tree DELETE (simple version)
- [x] Root-to-leaf traversal with state machine
- [x] Parent tracking via parent_map

### Split Operations
- [x] Async leaf node splitting
- [x] Async internal node splitting
- [x] Recursive parent splits
- [x] Root split (tree height increase)
- [x] Multi-phase async split state machine

### Infrastructure
- [x] Serialization/deserialization
- [x] Network read/write operations
- [x] Duplicate key handling (update value)
- [x] Statistics collection
- [x] Workload generation (YCSB-A, YCSB-B, etc.)

---

## 🔴 **High Priority - Correctness Issues**

### 1. Delete Underflow Handling
**Status**: Not implemented  
**Impact**: Tree becomes sparse over time with many deletes  
**Description**:
- Currently, DELETE removes keys but doesn't handle underflow
- When a node has fewer than `fanout/2` keys, should:
  1. Try to borrow keys from siblings
  2. If siblings are also minimal, merge nodes
  3. Propagate merges up the tree (may reduce tree height)

**Implementation Steps**:
```cpp
void handle_delete_underflow_async(AsyncOperation& op, BTreeNode& leaf) {
    uint32_t min_keys = (btree_fanout + 1) / 2;
    if (leaf.num_keys < min_keys && leaf.node_address != root_address) {
        // Phase 1: Read left and right siblings
        // Phase 2: Try to borrow from left sibling
        // Phase 3: Try to borrow from right sibling
        // Phase 4: If both minimal, merge with sibling
        // Phase 5: Update parent (may trigger parent merge)
    }
}
```

**Files to modify**:
- `computeServer.h`: Add BORROW_FROM_SIBLING, MERGE_NODES enum values
- `computeServer.cc`: Implement borrow/merge logic in handle_leaf_operation

---

### 2. Parent Address Lookup Fallback
**Status**: Warning printed but not handled  
**Impact**: Splits fail if parent not in parent_map  
**Description**:
- `parent_map` is built during traversal, but parent might not be there
- Currently prints warning and sets `parent_address = 0`
- Should implement actual parent search by traversing tree

**Implementation Steps**:
```cpp
void find_parent_async(uint64_t child_addr, uint32_t child_level) {
    // Create READ_PARENT_SEARCH state
    // Traverse from root to level above child
    // At each level, check children pointers
    // When found, save to parent_map for future use
}
```

**Files to modify**:
- `computeServer.h`: Add FIND_PARENT_SEARCH phase
- `computeServer.cc`: Implement tree traversal to find parent

---

## 🟡 **Medium Priority - Performance Optimizations**

### 3. Binary Search in Leaf Nodes
**Status**: Currently using linear search  
**Impact**: O(n) search time in leaves instead of O(log n)  
**Description**:
- `handle_leaf_operation` uses linear scan for SEARCH and DELETE
- Should use binary search for better performance

**Implementation**:
```cpp
uint32_t binary_search_leaf(const BTreeNode& leaf, uint64_t key) {
    uint32_t left = 0, right = leaf.num_keys;
    while (left < right) {
        uint32_t mid = (left + right) / 2;
        if (leaf.keys[mid] < key) left = mid + 1;
        else right = mid;
    }
    return left;
}
```

**Files to modify**:
- `computeServer.cc`: Add binary_search_leaf helper function

---

### 4. Sibling Pointers for Range Scans
**Status**: Not implemented  
**Impact**: Cannot do efficient range queries  
**Description**:
- Leaf nodes should have `next_leaf` pointer to right sibling
- Enables efficient range scans without tree traversal

**Implementation Steps**:
```cpp
struct BTreeNode {
    // ... existing fields ...
    uint64_t next_leaf;  // Pointer to next leaf (0 if last)
};

void split_leaf_async(...) {
    // After split:
    new_leaf.next_leaf = old_leaf.next_leaf;
    old_leaf.next_leaf = new_leaf.node_address;
}

void btree_range_scan_async(uint64_t start_key, uint64_t end_key) {
    // 1. Find leaf containing start_key
    // 2. Follow next_leaf pointers until end_key
    // 3. Return all keys in range
}
```

**Files to modify**:
- `computeServer.h`: Add next_leaf to BTreeNode, add range scan function
- `computeServer.cc`: Update splits, implement range_scan_async

---

### 5. Bulk Loading Optimization
**Status**: Not implemented  
**Impact**: Building large trees is slower than necessary  
**Description**:
- Currently insert one key at a time
- Bulk loading can build tree bottom-up more efficiently

**Implementation**:
```cpp
void bulk_load_async(std::vector<std::pair<uint64_t, uint64_t>>& sorted_keys) {
    // 1. Sort keys
    // 2. Create leaf nodes in one pass
    // 3. Build internal nodes bottom-up
    // 4. Write all nodes to memory
}
```

---

## 🟢 **Low Priority - Nice to Have**

### 6. Tree Consistency Checker
**Status**: Not implemented  
**Impact**: Harder to debug correctness issues  
**Description**:
- Validate tree structure after operations
- Check: key ordering, fanout constraints, parent-child consistency

**Implementation**:
```cpp
bool validate_tree_async() {
    // 1. Traverse entire tree
    // 2. Check each node:
    //    - Keys are sorted
    //    - Num keys within [min, max]
    //    - Children pointers valid
    // 3. Check all leaves reachable
    // 4. Check total key count matches expected
}
```

---

### 7. Tree Visualization/Debugging
**Status**: Not implemented  
**Impact**: Harder to understand tree structure during development  
**Description**:
- Print tree structure in readable format
- Show keys, addresses, levels

**Implementation**:
```cpp
void print_tree_structure() {
    // Breadth-first traversal
    // Print each level with indentation
    // Show keys and addresses
}
```

---

### 8. Concurrency Control (Future)
**Status**: Not needed for single-compute-server simulation  
**Impact**: Required for multi-compute-server workloads  
**Description**:
- Add locking mechanism
- Lock coupling during traversal
- Deadlock detection/avoidance

**Note**: Only needed when simulating multiple compute servers accessing same tree concurrently.

---

### 9. Write-Ahead Logging (Future)
**Status**: Not needed for simulation  
**Impact**: Required for crash recovery  
**Description**:
- Log operations before executing
- Replay log after crash
- Ensure durability

**Note**: SST simulation doesn't model crashes, so this is academic.

---

## 📊 **Testing TODO**

### Unit Tests Needed
- [ ] Test insert into empty tree
- [ ] Test insert causing leaf split
- [ ] Test insert causing internal split
- [ ] Test insert causing root split
- [ ] Test search in single-level tree
- [ ] Test search in multi-level tree
- [ ] Test delete from leaf (no underflow)
- [ ] Test delete causing underflow (TODO: implement first)
- [ ] Test duplicate key insertion (should update value)

### Integration Tests Needed
- [ ] Run YCSB-A workload (95% reads, 5% writes)
- [ ] Run YCSB-B workload (95% reads, 5% writes, uniform)
- [ ] Run insert-heavy workload (10% reads, 90% inserts)
- [ ] Run mixed workload (50% reads, 25% inserts, 25% deletes)
- [ ] Verify tree height grows correctly
- [ ] Verify statistics are accurate

### Performance Tests Needed
- [ ] Measure latency vs tree size
- [ ] Measure throughput vs fanout
- [ ] Compare Zipfian vs uniform distribution
- [ ] Measure network traffic (reads/writes)

---

## 🔧 **Code Quality TODO**

### Documentation
- [ ] Add function-level comments for all async split functions
- [ ] Document state machine transitions
- [ ] Add ASCII diagrams for split operations
- [ ] Update ASYNC_REFACTOR_SUMMARY.md with split details

### Error Handling
- [ ] Add timeout detection for pending_ops
- [ ] Handle memory allocation failures
- [ ] Validate node addresses before access
- [ ] Add bounds checking for arrays

### Code Cleanup
- [ ] Remove debug output statements (or make conditional)
- [ ] Consolidate duplicate code in split functions
- [ ] Add asserts for invariants
- [ ] Improve variable naming consistency

---

## 📈 **Performance Monitoring TODO**

### Additional Statistics
- [ ] Track split operations (leaf splits, internal splits)
- [ ] Track tree height over time
- [ ] Track average keys per node
- [ ] Track memory usage per memory server
- [ ] Track parent_map hit rate

### Profiling
- [ ] Identify bottleneck operations
- [ ] Measure time spent in each state machine phase
- [ ] Analyze network traffic patterns

---

## 🎯 **Priority Order for Implementation**

1. **FIRST**: Compile and test what we have ✅ (NEXT STEP)
2. **SECOND**: Fix parent address lookup fallback (HIGH priority bug)
3. **THIRD**: Add delete underflow handling (correctness issue)
4. **FOURTH**: Binary search optimization (easy performance win)
5. **FIFTH**: Sibling pointers for range scans (if needed by workload)
6. **LATER**: Bulk loading, consistency checker, visualization

---

## 📝 **Notes**

- Current implementation is SUFFICIENT for testing and performance evaluation
- Known limitations (no merge, no range scan) are acceptable for initial testing
- Focus on testing existing functionality before adding more features
- Keep TODO list updated as features are implemented
