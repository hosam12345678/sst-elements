# TODO: Future Enhancements and Missing Features

## 📋 Overview
This document tracks future work for the disaggregated memory B+tree implementation. The current version is a **simplified prototype** with basic functionality. This file documents what needs to be added for a production-ready system.

---

## 🔴 HIGH PRIORITY - Core B+tree Functionality

### 1. Complete Delete Operation with Rebalancing
**Status:** ⚠️ Partially Implemented  
**Current State:** Delete removes keys but doesn't handle underflow  
**Missing:**
- [ ] `borrow_from_sibling()` - Redistribute keys when node underflows
- [ ] `merge_with_sibling()` - Merge nodes when borrowing not possible
- [ ] Recursive underflow propagation up the tree
- [ ] Handle tree height decrease when root becomes empty
- [ ] Update parent pointers after merge/borrow operations

**Impact:** Without this, deleted keys leave sparse nodes, degrading performance over time

**Files to modify:**
- `src/sst/elements/rdmaNic/computeServer.cc` - Add borrow/merge logic to `btree_delete()`
- `src/sst/elements/rdmaNic/computeServer.h` - Add method declarations

**Estimated effort:** 2-3 days

---

### 2. Add Range Query Support
**Status:** ❌ Not Implemented  
**Current State:** Only single-key search is supported  
**Missing:**
- [ ] Sibling pointers in leaf nodes (for sequential scan)
- [ ] `range_query(key_start, key_end)` function
- [ ] Handle cross-server leaf chains (RDMA reads across multiple memory servers)
- [ ] Efficient batch processing for range scans

**Impact:** Cannot perform range queries (e.g., "find all keys between 100-200")

**Files to modify:**
- `src/sst/elements/rdmaNic/computeServer.h` - Add `BTreeNode::sibling_pointer`
- `src/sst/elements/rdmaNic/computeServer.cc` - Implement `range_query()`

**Estimated effort:** 1-2 days

---

### 3. Add Bulk Load Operation
**Status:** ❌ Not Implemented  
**Current State:** Tree built one insert at a time  
**Missing:**
- [ ] `bulk_load(sorted_keys[], values[])` function
- [ ] Bottom-up tree construction
- [ ] Batch RDMA writes for efficiency

**Impact:** Initial tree population is slow (many RDMA round trips)

**Estimated effort:** 1-2 days

---

## 🟡 MEDIUM PRIORITY - Performance & Realism

### 4. Replace cached_nodes with Real Async RDMA
**Status:** ⚠️ Using Simulation Shortcut  
**Current State:** `cached_nodes` map provides instant memory persistence  
**Why it exists:** Simplifies synchronous code, avoids async complexity  
**Missing:**
- [ ] Remove `cached_nodes` map
- [ ] Parse actual RDMA response data in `handleMemoryEvent()`
- [ ] Implement request completion tracking
- [ ] Use actual buffer contents instead of cache lookups
- [ ] Handle realistic RDMA latencies

**Impact:** Current timing is unrealistic - shows 2ns latency but no actual data dependency

**Reference:** See `SST_SIMULATION_FRAMEWORK.md` and `CACHED_NODES_EXPLAINED.md`

**Files to modify:**
- `src/sst/elements/rdmaNic/computeServer.cc` - Remove cache, parse responses
- `src/sst/elements/rdmaNic/computeServer.h` - Remove `cached_nodes` field

**Estimated effort:** 3-5 days (requires understanding SST async patterns)

---

### 5. Add Realistic RDMA NIC Simulation
**Status:** ❌ Using Simplified Links  
**Current State:** Direct StandardInterface connections with 1ns latency  
**Missing:**
- [ ] Use actual `rdmaNic.nic` component instead of StandardInterface
- [ ] Configure DMA cache and MMIO interface
- [ ] Set realistic queue depths (maxPendingCmds, maxMemReqs, cmdQSize)
- [ ] Add command queue management
- [ ] Simulate NIC processing overhead

**Current config:**
```python
# Simple direct connection (2ns total)
link.connect((compute_interface, "lowlink", "1ns"),
             (memory_interface, "lowlink", "1ns"))
```

**Realistic config should use:**
```python
nic = sst.Component("compute_nic", "rdmaNic.nic")
nic.addParams({
    "clock": "1GHz",
    "maxPendingCmds": 128,    # Queue depth
    "maxMemReqs": 256,         # Outstanding requests
    "cmdQSize": 64,            # Command queue size
})
```

**Reference:** See `src/sst/elements/rdmaNic/tests/rdmaNic.py` for example

**Impact:** Current latencies are unrealistic (2ns), doesn't model queue contention

**Files to modify:**
- `src/sst/elements/rdmaNic/tests/disagg_uniform_workload.py`
- `src/sst/elements/rdmaNic/tests/disagg_skewed_workload.py`

**Estimated effort:** 2-3 days

---

### 6. Add Merlin Network Topology Simulation
**Status:** ❌ Not Implemented  
**Current State:** Point-to-point links only  
**Missing:**
- [ ] Add Merlin network fabric simulation
- [ ] Configure realistic topology (torus, dragonfly, etc.)
- [ ] Set bandwidth limits (100GB/s InfiniBand)
- [ ] Add router latencies (input/output port processing)
- [ ] Simulate network congestion and queueing

**Example config:**
```python
network = sst.Component("network", "merlin.hr_router")
network.addParams({
    "link_bw": "100GB/s",        # Bandwidth
    "link_lat": "1us",           # Wire latency
    "xbar_bw": "100GB/s",        # Crossbar bandwidth
    "input_latency": "100ns",    # Router processing
    "output_latency": "100ns",
    "flit_size": "8B",
})
```

**Impact:** Cannot study network effects (contention, routing, congestion)

**Estimated effort:** 3-5 days

---

### 7. Add Realistic Latency Configuration
**Status:** ⚠️ Using Placeholder Values  
**Current State:** 1ns link latency (unrealistic)  
**Realistic values for RDMA:**
- PCIe latency: 1-2 μs
- Network latency: 1-5 μs (InfiniBand)
- Memory access: 50-100 ns (DRAM)
- Total RDMA read: 2-10 μs

**Files to modify:**
- All test Python scripts in `src/sst/elements/rdmaNic/tests/`

**Estimated effort:** 1 day (configuration tuning)

---

## 🟢 LOW PRIORITY - Advanced Features

### 8. Add Concurrency Control (Locking)
**Status:** ❌ Not Implemented  
**Current State:** Single-threaded, no concurrent access  
**Missing:**
- [ ] Node-level locking (lock bits in BTreeNode)
- [ ] RDMA atomic operations for lock acquisition
- [ ] Lock-free algorithms (optimistic concurrency)
- [ ] Version-based validation
- [ ] Deadlock detection/prevention

**Impact:** Cannot handle multiple compute servers accessing same tree

**Estimated effort:** 1-2 weeks (complex distributed locking)

---

### 9. Add Crash Recovery & Durability
**Status:** ❌ Not Implemented  
**Current State:** In-memory only, no persistence  
**Missing:**
- [ ] Write-ahead logging (WAL)
- [ ] Checkpointing to persistent storage
- [ ] Crash recovery protocol
- [ ] Undo/redo logs for transactions

**Impact:** Data lost on crash, no ACID guarantees

**Estimated effort:** 1-2 weeks

---

### 10. Add Performance Monitoring & Profiling
**Status:** ⚠️ Basic Statistics Only  
**Current State:** Counts operations but limited analysis  
**Missing:**
- [ ] Detailed latency histograms (min/max/p50/p99)
- [ ] Per-level RDMA traffic analysis
- [ ] Hot key detection and reporting
- [ ] Network bandwidth utilization
- [ ] Queue depth statistics
- [ ] Cache hit rate (if caching added)

**Files to modify:**
- `src/sst/elements/rdmaNic/computeServer.cc` - Add detailed stats collection

**Estimated effort:** 3-5 days

---

### 11. Add Smart Caching
**Status:** ❌ Not Implemented  
**Current State:** No caching (every access is RDMA)  
**Potential optimizations:**
- [ ] LRU cache for frequently accessed nodes
- [ ] Prefetching for sequential scans
- [ ] Write-back buffering for batch updates
- [ ] Hot node replication

**Impact:** Could significantly reduce RDMA traffic

**Estimated effort:** 1 week

---

### 12. Add Load Balancing Improvements
**Status:** ⚠️ Basic Hash Distribution  
**Current State:** Simple modulo hash for key-to-server mapping  
**Missing:**
- [ ] Dynamic load balancing based on access patterns
- [ ] Hot key detection and replication
- [ ] Adaptive partitioning
- [ ] Load-aware routing

**Files to modify:**
- `src/sst/elements/rdmaNic/computeServer.cc` - `hash_key_to_memory_server()`

**Estimated effort:** 1 week

---

## 🔧 TECHNICAL DEBT & Code Quality

### 13. Remove Hardcoded Limits
**Status:** ⚠️ Many Hardcoded Values  
**Current issues:**
- [ ] BTreeNode has fixed 64-key arrays (should be dynamic based on fanout)
- [ ] Max 8 memory servers hardcoded in port definitions
- [ ] Fixed tree height calculations
- [ ] Buffer size assumptions

**Files to review:**
- `src/sst/elements/rdmaNic/computeServer.h` - BTreeNode structure

**Estimated effort:** 2-3 days

---

### 14. Add Comprehensive Testing
**Status:** ⚠️ Manual Testing Only  
**Missing:**
- [ ] Unit tests for B+tree operations (insert/search/delete)
- [ ] Integration tests for multi-server scenarios
- [ ] Stress tests for high concurrency
- [ ] Correctness validation (tree invariants)
- [ ] Performance regression tests

**Test framework:** Use SST's test infrastructure (see `tests/testsuite_default_rdmaNic.py`)

**Estimated effort:** 1 week

---

### 15. Improve Error Handling
**Status:** ⚠️ Minimal Error Handling  
**Current issues:**
- [ ] Many assertions that crash simulation
- [ ] No graceful degradation
- [ ] Limited error reporting
- [ ] No retry logic for RDMA failures

**Estimated effort:** 2-3 days

---

### 16. Add Documentation
**Status:** ⚠️ Code Comments Only  
**Missing:**
- [ ] API documentation (Doxygen)
- [ ] User guide for running simulations
- [ ] Configuration parameter guide
- [ ] Performance tuning guide
- [ ] Architecture diagrams

**Estimated effort:** 1 week

---

## 📚 Research & Exploration

### 17. Compare with Sherman Paper Implementation
**Status:** 🔬 Research Task  
**Goal:** Validate against Sherman paper (https://arxiv.org/pdf/2112.07320)  
**Tasks:**
- [ ] Implement Sherman's hierarchical NIC locks
- [ ] Add coalesced dependent commands
- [ ] Implement two-level version layout
- [ ] Run YCSB-like workloads and compare

**Estimated effort:** 2-3 weeks

---

### 18. Explore Alternative Tree Structures
**Status:** 🔬 Research Task  
**Alternatives to explore:**
- [ ] Bw-tree (latch-free B+tree)
- [ ] Masstree (cache-optimized trie)
- [ ] ART (Adaptive Radix Tree)

**Estimated effort:** Variable (ongoing research)

---

## 📊 Priority Matrix

| Task | Priority | Effort | Impact | When to Do |
|------|----------|--------|--------|------------|
| Complete Delete (borrow/merge) | 🔴 HIGH | 2-3 days | High | **Next** |
| Remove cached_nodes | 🟡 MEDIUM | 3-5 days | High | After delete works |
| Add realistic RDMA NIC | 🟡 MEDIUM | 2-3 days | Medium | For performance testing |
| Range queries | 🔴 HIGH | 1-2 days | Medium | For completeness |
| Concurrency control | 🟢 LOW | 1-2 weeks | High | For multi-client |
| Merlin network | 🟡 MEDIUM | 3-5 days | Medium | For realistic network |
| Performance monitoring | 🟢 LOW | 3-5 days | Medium | For analysis |
| Testing infrastructure | 🔴 HIGH | 1 week | High | For validation |

---

## 🎯 Recommended Development Phases

### Phase 1: Complete Core Functionality (Current Focus)
- [x] Basic insert/search
- [ ] Complete delete with rebalancing
- [ ] Range queries
- [ ] Basic testing

**Goal:** Fully functional B+tree implementation

---

### Phase 2: Add Realism
- [ ] Remove cached_nodes
- [ ] Add realistic RDMA NIC simulation
- [ ] Configure realistic latencies
- [ ] Add performance monitoring

**Goal:** Accurate performance measurement

---

### Phase 3: Production Features
- [ ] Concurrency control
- [ ] Crash recovery
- [ ] Smart caching
- [ ] Load balancing

**Goal:** Production-ready system

---

### Phase 4: Optimization & Research
- [ ] Sherman paper validation
- [ ] Alternative data structures
- [ ] Advanced optimizations
- [ ] Publish results

**Goal:** Research contributions

---

## 📝 Notes

**Current Status Summary:**
- ✅ **Working:** Basic insert, search, partial delete, memory distribution
- ⚠️ **Needs work:** Delete rebalancing, async RDMA, realistic timing
- ❌ **Missing:** Concurrency, durability, range queries, production features

**For Prototyping:** Current implementation is **perfectly fine**  
**For Performance Testing:** Need Phase 2 (remove cached_nodes, add realistic NIC)  
**For Production:** Need all phases

---

## 🔗 Related Documentation

- `BTREE_IMPLEMENTATION_STATUS.md` - Detailed implementation status
- `SST_SIMULATION_FRAMEWORK.md` - SST framework explanation
- `CACHED_NODES_EXPLAINED.md` - Why cached_nodes exists
- `MEMORY_LAYOUT.md` - Memory architecture guide

---

**Last Updated:** 2025-10-20  
**Version:** 1.0 (Initial prototype)
