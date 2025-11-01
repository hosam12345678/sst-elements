#!/usr/bin/env python3
"""
Unit Test 4: Insert Causing Root Split
Tests: Root split specifically (both leaf and internal)
Config: 1 compute server + 1 memory server
Expected: Root address remains same, old root moves to new address
"""

import sst

print("=" * 80)
print("UNIT TEST 4: Root Split Behavior")
print("=" * 80)
print("Test Objective: Verify root split preserves root address")
print("Configuration: 1 compute + 1 memory server")
print("Expected Behavior:")
print("  Key Insight: When root splits (leaf or internal):")
print("    1. New root is created at a NEW address")
print("    2. Old root moves to a different address") 
print("    3. root_address variable is updated to new root")
print("    4. Tree height increases by 1")
print()
print("This test verifies:")
print("  - Root leaf split (height 1→2)")
print("  - Root address update after split")
print("  - Old root gets new address (not 0x10000000)")
print("  - New root becomes the tree entry point")
print()

# Create compute server
compute = sst.Component("compute_0", "rdmaNic.computeServer")
compute.addParams({
    "verbose": 1,
    "node_id": 0,
    "num_memory_nodes": 1,
    "operations_per_second": 100,
    "simulation_duration_us": 150000,  # 150ms
    "read_ratio": 0.0,  # 100% writes
    "key_range": 8,  # Keys 0-7
    "btree_fanout": 4,  # Fanout 4
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
print("  Key range: 0-7")
print("  Focus: Root address management during split")
print()
print("Expected Sequence:")
print("  1. Initial root: 0x10000000 (leaf, height=1)")
print("  2. Insert keys 0,1,2,3 → root fills")
print("  3. Insert key 4 → ROOT SPLIT:")
print("     - 'Splitting ROOT node' message")
print("     - 'Moving old root to new address 0x102xxxxx'")
print("     - Old root (now leaf) gets new address")
print("     - New root created at NEW address")
print("     - root_address updated")
print("  4. Tree height 1 → 2")
print("  5. Subsequent operations use NEW root address")
print()
print("Watch For:")
print("  ✓ Initial root address: 0x10000000")
print("  ✓ 'Splitting ROOT node' message")
print("  ✓ 'Moving old root to new address' (NOT 0x10000000)")
print("  ✓ 'New root created at 0x...' message")
print("  ✓ Tree height increases to 2")
print("  ✓ No address conflicts (child[0] != child[1])")
print("=" * 80)
