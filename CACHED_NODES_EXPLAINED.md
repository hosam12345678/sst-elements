# cached_nodes: Quick Visual Explanation

## The Problem in One Picture

### Without cached_nodes (BROKEN):

```
Time: t=0
┌──────────────┐
│ Compute      │  btree_insert(key=5, value=100)
│ Server       │  └─ Create node with key=5
└──────────────┘
       │
       │ rdma_write(node)
       ▼
Time: t=1  (write scheduled but not completed)
┌──────────────┐
│ Memory       │  (hasn't received write yet)
│ Server       │  memory_data[] = [empty]
└──────────────┘

Time: t=2
┌──────────────┐
│ Compute      │  btree_search(key=5)
│ Server       │  └─ traverse_to_leaf(5)
└──────────────┘
       │
       │ rdma_read(node_address)
       ▼
Time: t=3
┌──────────────┐
│ Memory       │  ❌ PROBLEM: Write still hasn't arrived!
│ Server       │  memory_data[] = [empty]
└──────────────┘  Returns empty data
       │
       ▼
┌──────────────┐
│ Compute      │  ❌ Gets empty node (key=5 not found!)
│ Server       │  Search FAILS even though we just inserted it!
└──────────────┘
```

### With cached_nodes (WORKS):

```
Time: t=0
┌──────────────┐
│ Compute      │  btree_insert(key=5, value=100)
│ Server       │  └─ Create node with key=5
│              │  
│ cached_nodes │  cached_nodes[addr] = node ← Instant!
│  [addr→node] │  
└──────────────┘
       │
       │ rdma_write(node)  (async, happens later)
       ▼

Time: t=2
┌──────────────┐
│ Compute      │  btree_search(key=5)
│ Server       │  └─ traverse_to_leaf(5)
│              │  
│ cached_nodes │  ✅ Checks cache first
│  [addr→node] │  Returns cached node immediately
└──────────────┘
       │
       │ ✅ SUCCESS: Found key=5!
       ▼
```

## The Async Problem

### Real RDMA Timeline:

```
Nanoseconds:  0     50    100   150   200   250   300   350
              │      │      │     │     │     │     │     │
Compute:      WRITE─┘      │     READ──┘     │     │     │
              │            │     │           │     │     │
Network:      ├────────────┘     ├───────────┘     │     │
              │ (latency)        │ (latency)       │     │
Memory:       │            WRITE─┘                 │     READ─┘
              │            stored                   │     returned
```

**The Issue:** Read at t=150 happens BEFORE Write completes at t=100!

### With Cache (Simulation Hack):

```
Nanoseconds:  0     50    100   150   200
              │      │      │     │     │
Compute:      WRITE─┘      │     READ──┘
              │            │     │
cached_nodes: [INSTANT]────┼─────[FOUND!]
              Store        │     Retrieve
              
              (No network delay simulated)
```

## Code Comparison

### Real RDMA (Future Implementation):

```cpp
void btree_insert(uint64_t key, uint64_t value) {
    // Step 1: Start async read of leaf
    rdma_read_async(leaf_addr, [=](BTreeNode leaf) {
        // Step 2: Called when read completes (100ns later)
        leaf.keys[i] = key;
        
        // Step 3: Start async write
        rdma_write_async(leaf_addr, &leaf, [=]() {
            // Step 4: Called when write completes (another 100ns)
            printf("Insert completed!\n");
        });
    });
    // Returns immediately, insert happens in background
}
```

### Current (With cached_nodes):

```cpp
void btree_insert(uint64_t key, uint64_t value) {
    // Looks synchronous:
    BTreeNode leaf = traverse_to_leaf(key);  // ← Uses cache, instant
    leaf.keys[i] = key;
    rdma_write(leaf_addr, &leaf);            // ← Sends async
    cached_nodes[leaf_addr] = leaf;          // ← But cache is instant!
    printf("Insert completed!\n");           // ← Runs immediately
    
    // Next operation can read from cache immediately
}
```

## Bottom Line

**cached_nodes = Training Wheels**

- ✅ Lets you develop B+tree logic without async complexity
- ✅ Operations work correctly (tree structure is valid)
- ✅ Easy to debug (linear code flow)
- ❌ Doesn't measure real latency
- ❌ Hides timing bugs
- ❌ Not how real systems work

**When to remove it:**
- ✅ B+tree operations debugged and working
- ✅ Ready to measure performance
- ✅ Need realistic latency numbers
- ✅ Want to compare architectures

**For now:** Keep it! It's helping you focus on correctness, not async complexity.
