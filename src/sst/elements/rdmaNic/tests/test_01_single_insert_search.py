#!/usr/bin/env python3
"""
Test 1: Single Insert & Search
Simplest test - insert one key and search for it.
Expected: Key found in root node, operation completes successfully.
"""

import sst

print("=" * 70)
print("TEST 1: Single Insert & Search")
print("=" * 70)
print("Goal: Verify basic async insert and search operations")
print("Expected: Insert 1 key, then successfully search for it")
print()

# Create compute server
compute = sst.Component("compute_0", "rdmaNic.computeServer")
compute.addParams({
    "verbose": 1,
    "node_id": 0,
    "num_memory_nodes": 1,
    "operations_per_second": 10,  # Very slow rate - only need 2 operations
    "simulation_duration_us": 500000,  # 500ms - plenty of time
    "read_ratio": 0.5,  # 50% read, 50% write - will do 1 insert, 1 search
    "key_range": 10,  # Small range
    "btree_fanout": 16,  # Normal fanout (no splits expected)
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
print("  - 1 compute server, 1 memory server")
print("  - Fanout: 16 (no splits)")
print("  - Operations: ~5 total (1-2 inserts, 1-2 searches)")
print("  - Key range: 10 keys")
print()
print("Watch for:")
print("  ✓ 'INSERT' operations completing")
print("  ✓ 'SEARCH' operations finding keys")
print("  ✓ No errors or timeouts")
print("=" * 70)
