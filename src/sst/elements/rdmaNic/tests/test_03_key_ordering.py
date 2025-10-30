#!/usr/bin/env python3
"""
Test 3: Sequential vs Random Key Insertion
Test that key ordering works correctly for both sequential and random keys.
Expected: Keys stored in sorted order regardless of insertion order.
"""

import sst

print("=" * 70)
print("TEST 3: Sequential vs Random Key Insertion")
print("=" * 70)
print("Goal: Verify keys are stored in sorted order")
print("Expected: Keys inserted in any order are found correctly")
print()

# Create compute server
compute = sst.Component("compute_0", "rdmaNic.computeServer")
compute.addParams({
    "verbose": 1,
    "node_id": 0,
    "num_memory_nodes": 1,
    "operations_per_second": 200,
    "simulation_duration_us": 150000,  # 150ms
    "read_ratio": 0.3,  # 70% inserts, 30% searches - more inserts
    "key_range": 20,  # Medium range
    "btree_fanout": 16,
    "key_distribution": "uniform",  # Random order insertion
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
print("  - Key range: 20 (inserted in random order due to uniform distribution)")
print("  - Operations: ~30 total (21 inserts + 9 searches)")
print("  - Expected: Keys stored in sorted order")
print()
print("Watch for:")
print("  ✓ Keys inserted in random order")
print("  ✓ All searches find their keys")
print("  ✓ Keys appear sorted in node output")
print("=" * 70)
