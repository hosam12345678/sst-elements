#!/usr/bin/env python3
"""
Unit Test 5: Search in Single-Level Tree
Tests: Search operations when tree has only root (leaf)
Config: 1 compute server + 1 memory server
Expected: Direct search in root leaf node
"""

import sst

print("=" * 80)
print("UNIT TEST 5: Search in Single-Level Tree")
print("=" * 80)
print("Test Objective: Verify search in simplest tree structure")
print("Configuration: 1 compute + 1 memory server")
print("Expected Behavior:")
print("  1. Tree has only root node (which is a leaf)")
print("  2. Insert keys 10, 20, 30")
print("  3. Search for existing keys → FOUND")
print("  4. Search for non-existing keys → NOT FOUND")
print("  5. All searches only access root node (1 network read)")
print()

# Create compute server
compute = sst.Component("compute_0", "rdmaNic.computeServer")
compute.addParams({
    "verbose": 1,
    "node_id": 0,
    "num_memory_nodes": 1,
    "operations_per_second": 50,
    "simulation_duration_us": 400000,  # 400ms for ~20 operations
    "read_ratio": 0.5,  # 50% reads, 50% writes
    "key_range": 10,  # Keys 0-9
    "btree_fanout": 16,  # Large fanout - no splits
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
print("  Fanout: 16 keys per node (no splits)")
print("  Key range: 0-9")
print("  Mix: 50% inserts, 50% searches")
print("  Tree structure: Single root leaf node")
print()
print("Search Scenarios:")
print("  Case 1: Search for key that exists")
print("    - Read root → deserialize → find key in keys[] array")
print("    - Output: '✓ FOUND key=X at position Y, value=Z'")
print()
print("  Case 2: Search for key that doesn't exist")
print("    - Read root → deserialize → key not in keys[] array")
print("    - Output: '✗ NOT FOUND key=X'")
print()
print("Expected Output:")
print("  ✓ Tree height=1 throughout test")
print("  ✓ Mix of INSERT and SEARCH operations")
print("  ✓ 'FOUND' messages for existing keys")
print("  ✓ 'NOT FOUND' messages for missing keys")
print("  ✓ Each search = 1 network read (root only)")
print("  ✓ No splits (fanout 16 > key range 10)")
print("=" * 80)
