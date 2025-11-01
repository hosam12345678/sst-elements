#!/usr/bin/env python3
"""
Unit Test 6: Search in Multi-Level Tree
Tests: Search traversal through internal nodes to reach leaves
Config: 1 compute server + 1 memory server
Expected: Multiple network reads during tree traversal
"""

import sst

print("=" * 80)
print("UNIT TEST 6: Search in Multi-Level Tree")
print("=" * 80)
print("Test Objective: Verify search traversal through tree levels")
print("Configuration: 1 compute + 1 memory server")
print("Expected Behavior:")
print("  1. Build tree to height 2+ (root internal + leaves)")
print("  2. Search operations traverse:")
print("     Step 1: Read root (internal node)")
print("     Step 2: Find child pointer using keys")
print("     Step 3: Read leaf node")
print("     Step 4: Search for key in leaf")
print("  3. Each search = multiple network reads (height = # reads)")
print()

# Create compute server
compute = sst.Component("compute_0", "rdmaNic.computeServer")
compute.addParams({
    "verbose": 1,
    "node_id": 0,
    "num_memory_nodes": 1,
    "operations_per_second": 100,
    "simulation_duration_us": 500000,  # 500ms
    "read_ratio": 0.3,  # 30% reads (after tree builds), 70% writes
    "key_range": 15,  # Keys 0-14
    "btree_fanout": 4,  # Small fanout to force height 2
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
print("  Key range: 0-14")
print("  Operations: ~50 total (35 inserts, 15 searches)")
print()
print("Tree Building Phase:")
print("  - Insert keys to trigger split → height 2")
print("  - Root becomes internal node with 2+ leaf children")
print()
print("Search Traversal Example (height 2):")
print("  🔍 SEARCH key=7")
print("  Step 1: Read root at 0x10xxxxxx")
print("    - Root is internal node with keys [4, 8, 12]")
print("    - Key 7: between 4 and 8 → follow child[1]")
print("  Step 2: Read leaf at 0x102xxxxx")
print("    - Leaf contains keys [4, 5, 6, 7]")
print("    - Search for 7 → FOUND at position 3")
print()
print("Expected Output:")
print("  ✓ Tree grows to height 2+")
print("  ✓ SEARCH operations show:")
print("    - 'Level 0: Read node at 0x...' (root internal)")
print("    - '→ Continue to child[X] = 0x...'")
print("    - 'Level 1: Read node at 0x...' (leaf)")
print("    - '✓ Reached leaf at 0x...'")
print("    - '✓ FOUND key=X' OR '✗ NOT FOUND key=X'")
print("  ✓ Network reads = tree height")
print("  ✓ Correct child selection based on keys")
print("=" * 80)
