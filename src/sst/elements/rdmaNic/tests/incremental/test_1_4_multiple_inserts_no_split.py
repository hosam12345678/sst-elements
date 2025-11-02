#!/usr/bin/env python3
"""
Test 1.4: Multiple Inserts (No Split)
Phase: Basic Operations (Foundation)

Test Objective: Verify multiple inserts maintain sorted order without triggering splits
Expected Behavior:
  - Insert multiple keys (10, 20, 30, 40) into empty tree
  - All inserts succeed without triggering splits
  - Keys are stored in sorted order in the leaf
  - All inserted keys can be found by search
  - Tree height remains 1

Validates:
  - Sorted insertion logic (insert_into_leaf)
  - Multiple keys in a single leaf node
  - Binary search in leaf with multiple keys
  - No premature splits (fanout=16, only 4 keys)
"""

import sst

print("=" * 80)
print("TEST 1.4: Multiple Inserts (No Split)")
print("=" * 80)
print("Phase: Basic Operations (Foundation)")
print()
print("Test Objective:")
print("  Insert multiple keys and verify sorted order is maintained")
print()
print("Expected Behavior:")
print("  1. Tree initializes with empty root leaf (0 keys)")
print("  2. INSERT keys in non-sorted order (various keys 0-9)")
print("  3. Keys are inserted in sorted order within leaf")
print("  4. No splits occur (fanout=16, inserting < 16 keys)")
print("  5. All SEARCH operations find their keys")
print("  6. Tree height remains 1")
print()

# Create compute server
compute = sst.Component("compute_0", "rdmaNic.computeServer")
compute.addParams({
    "verbose": 1,
    "node_id": 0,
    "num_memory_nodes": 1,
    "operations_per_second": 100,
    "simulation_duration_us": 200000,  # 200ms
    "read_ratio": 0.3,  # 30% searches, 70% inserts
    "key_range": 10,  # Keys 0-9
    "btree_fanout": 16,  # Large fanout - no splits with <16 keys
    "key_distribution": "uniform",
})

# Create memory server
memory = sst.Component("memory_0", "rdmaNic.memoryServer")
memory.addParams({
    "verbose": 1,
    "memory_server_id": 0,
    "num_compute_nodes": 1,
    "memory_size_mb": 16,
    "base_addr": "0x10000000",
})

# Setup network interfaces
compute_iface = compute.setSubComponent("mem_interface_0", "memHierarchy.standardInterface")
memory_iface = memory.setSubComponent("mem_interface_0", "memHierarchy.standardInterface")

# Connect compute to memory
link = sst.Link("compute_memory_link")
link.connect((compute_iface, "lowlink", "1ns"), (memory_iface, "lowlink", "1ns"))

print("Test Configuration:")
print("  - 1 compute server, 1 memory server")
print("  - Fanout: 16 keys per node")
print("  - Operations: ~20 total (mix of inserts and searches)")
print("  - Key range: 0-9 (10 unique keys)")
print("  - 70% inserts, 30% searches")
print()
print("Expected Output:")
print("  ✓ Multiple INSERT operations succeed")
print("  ✓ Messages: '✓ Inserted key=X at position Y'")
print("  ✓ Node serialization shows increasing num_keys")
print("  ✓ Keys stored in sorted order (keys[0] < keys[1] < ...)")
print("  ✓ SEARCH operations find all inserted keys")
print("  ✓ Tree height remains 1 (no split triggered)")
print()
print("Critical Validations:")
print("  - insert_into_leaf() maintains sorted order")
print("  - Multiple keys coexist in single leaf")
print("  - Binary search works with multiple keys")
print("  - No 'Leaf FULL' messages (capacity not exceeded)")
print("=" * 80)
