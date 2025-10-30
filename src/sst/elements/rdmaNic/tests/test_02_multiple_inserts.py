#!/usr/bin/env python3
"""
Test 2: Multiple Inserts (No Split)
Insert several keys (less than fanout) and search for them.
Expected: All keys found, no splits, single root node.
"""

import sst

print("=" * 70)
print("TEST 2: Multiple Inserts (No Split)")
print("=" * 70)
print("Goal: Insert multiple keys without triggering splits")
print("Expected: All keys inserted into root node, all searches succeed")
print()

# Create compute server
compute = sst.Component("compute_0", "rdmaNic.computeServer")
compute.addParams({
    "verbose": 1,
    "node_id": 0,
    "num_memory_nodes": 1,
    "operations_per_second": 100,  # Moderate rate
    "simulation_duration_us": 200000,  # 200ms
    "read_ratio": 0.5,  # 50% inserts, 50% searches
    "key_range": 10,  # Small key range - will insert ~10 keys
    "btree_fanout": 16,  # Fanout of 16 - no splits with 10 keys
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
print("  - Fanout: 16 keys per node")
print("  - Key range: 10 keys")
print("  - Operations: ~20 total (10 inserts + 10 searches)")
print("  - Expected: NO splits (10 keys < 16 fanout)")
print()
print("Watch for:")
print("  ✓ Multiple INSERT operations completing")
print("  ✓ All SEARCH operations finding keys")
print("  ✓ NO split messages")
print("  ✓ Tree height remains 1 (root only)")
print("=" * 70)
