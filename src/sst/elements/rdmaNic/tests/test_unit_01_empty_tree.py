#!/usr/bin/env python3
"""
Unit Test 1: Insert into Empty Tree
Tests: Basic tree initialization and first insert into empty tree
Config: 1 compute server + 1 memory server
Expected: Root node (initially leaf) receives first key-value pair
"""

import sst

print("=" * 80)
print("UNIT TEST 1: Insert into Empty Tree")
print("=" * 80)
print("Test Objective: Verify basic tree initialization and first insert")
print("Configuration: 1 compute + 1 memory server")
print("Expected Behavior:")
print("  1. Tree initializes with empty root node (which is also a leaf)")
print("  2. First insert adds key to root leaf node")
print("  3. Tree height remains 1 (single-level tree)")
print("  4. Key is successfully inserted and can be found")
print()

# Create compute server
compute = sst.Component("compute_0", "rdmaNic.computeServer")
compute.addParams({
    "verbose": 1,
    "node_id": 0,
    "num_memory_nodes": 1,
    "operations_per_second": 100,
    "simulation_duration_us": 100000,  # 100ms
    "read_ratio": 0.0,  # 100% writes (inserts only)
    "key_range": 1,  # Only 1 key to test single insert
    "btree_fanout": 4,  # Small fanout for easier testing
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
print("  Operations: Multiple inserts of key=0")
print("  Expected tree height: 1 (root is leaf)")
print()
print("Expected Output:")
print("  ✓ Tree initialization with height=1")
print("  ✓ Root node address: 0x10000000")
print("  ✓ INSERT operations complete successfully")
print("  ✓ Duplicate key inserts update existing value")
print("=" * 80)
