#!/usr/bin/env python3
"""
Unit Test 2: Insert Causing Leaf Split
Tests: Leaf node split when capacity is exceeded
Config: 1 compute server + 1 memory server
Expected: When root leaf fills up, split into two leaves with new root
"""

import sst

print("=" * 80)
print("UNIT TEST 2: Insert Causing Leaf Split (Root Split)")
print("=" * 80)
print("Test Objective: Verify leaf split mechanism and tree height increase")
print("Configuration: 1 compute + 1 memory server")
print("Expected Behavior:")
print("  1. Root starts as leaf node (height = 1)")
print("  2. Insert keys 0, 1, 2, 3 → root fills to capacity (fanout=4)")
print("  3. Insert key 4 → triggers LEAF SPLIT:")
print("     - Old leaf splits into two leaves")
print("     - New root created as internal node")
print("     - Tree height increases 1 → 2")
print("  4. Separator key promoted to new root")
print()

# Create compute server
compute = sst.Component("compute_0", "rdmaNic.computeServer")
compute.addParams({
    "verbose": 1,
    "node_id": 0,
    "num_memory_nodes": 1,
    "operations_per_second": 100,
    "simulation_duration_us": 200000,  # 200ms - enough for ~20 operations
    "read_ratio": 0.0,  # 100% writes (inserts only)
    "key_range": 10,  # Keys 0-9
    "btree_fanout": 4,  # Small fanout to trigger split quickly
    "key_distribution": "uniform",
})

# Create memory server
memory = sst.Component("memory_0", "rdmaNic.memoryServer")
memory.addParams({
    "verbose": 1,
    "memory_server_id": 0,
    "num_compute_nodes": 1,  # Only 1 compute server in this test
    "memory_size_mb": 16,
    "base_addr": "0x10000000",
})

# Setup network interfaces
compute_iface = compute.setSubComponent("mem_interface_0", "memHierarchy.standardInterface")
memory_iface = memory.setSubComponent("mem_interface_0", "memHierarchy.standardInterface")

# Connect compute to memory
link = sst.Link("compute_memory_link")
link.connect((compute_iface, "lowlink", "1ns"), (memory_iface, "lowlink", "1ns"))

print("Test Setup:")
print("  Fanout: 4 keys per node")
print("  Key range: 0-9 (need 5+ unique keys to trigger split)")
print("  Operations: ~20 inserts over 200ms")
print()
print("Critical Moments:")
print("  Insert #1-4: Root fills up (keys 0,1,2,3)")
print("  Insert #5:   🔀 LEAF SPLIT triggered")
print("               - Root is leaf, so this is ROOT SPLIT")
print("               - Old root moves to new address")
print("               - New root created as internal node")
print("               - Tree height: 1 → 2")
print()
print("Expected Output:")
print("  ✓ Tree starts at height=1")
print("  ✓ 'Leaf FULL' message when 4 keys inserted")
print("  ✓ 'Splitting ROOT node' message")
print("  ✓ 'Moving old root to new address' message")
print("  ✓ 'New root created' message")
print("  ✓ Tree height increases to 2")
print("  ✓ All inserts complete successfully")
print("=" * 80)
