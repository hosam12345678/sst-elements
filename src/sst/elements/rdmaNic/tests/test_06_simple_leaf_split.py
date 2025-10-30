#!/usr/bin/env python3
"""
Test 6: Simple Leaf Split
First split test - insert just enough keys to trigger ONE leaf split.
Expected: Root splits into 2 leaf nodes with 1 internal root node.
"""

import sst

print("=" * 70)
print("TEST 6: Simple Leaf Split")
print("=" * 70)
print("Goal: Trigger exactly ONE leaf node split")
print("Expected: Tree grows from height 1 → height 2")
print()

# Create compute server
compute = sst.Component("compute_0", "rdmaNic.computeServer")
compute.addParams({
    "verbose": 1,
    "node_id": 0,
    "num_memory_nodes": 1,
    "operations_per_second": 100,
    "simulation_duration_us": 300000,  # 300ms
    "read_ratio": 0.0,  # 100% inserts to force split quickly
    "key_range": 10,  # Small range - will insert 5-6 keys
    "btree_fanout": 4,  # SMALL fanout - split after 4 keys
    "key_distribution": "uniform",
})

# Create memory server
memory = sst.Component("memory_0", "rdmaNic.memoryServer")
memory.addParams({
    "verbose": 1,
    "memory_server_id": 0,
    "memory_size_mb": 16,
    "base_addr": "0x10000000",
})

# Create interfaces - standardInterface with simple direct connection
compute_iface = compute.setSubComponent("mem_interface_0", "memHierarchy.standardInterface")

memory_iface = memory.setSubComponent("mem_interface", "memHierarchy.standardInterface")

# Connect them directly with lowlink ports
link = sst.Link("memory_link")
link.connect((compute_iface, "lowlink", "1ns"), (memory_iface, "lowlink", "1ns"))

print("Test Configuration:")
print("  - Fanout: 4 keys per node (SMALL to trigger split)")
print("  - Key range: 10 keys")
print("  - Operations: ~30 inserts")
print("  - Expected: 1 leaf split after 4th insert")
print()
print("Watch for:")
print("  ✓ 'Leaf node FULL' message after 4 keys")
print("  ✓ 'Phase 1: Writing old node' (split starts)")
print("  ✓ 'Phase 2: Writing new node'")
print("  ✓ 'Creating new root node' (tree height increases)")
print("  ✓ Tree height becomes 2")
print("=" * 70)
