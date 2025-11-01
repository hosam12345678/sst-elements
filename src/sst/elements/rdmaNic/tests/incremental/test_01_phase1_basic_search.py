#!/usr/bin/env python3
"""
Incremental Test 1: Phase 1 - Basic Search Operations
Tests: Search in empty tree and search after manual tree setup
Focus: Verify traversal logic works correctly
"""

import sst

print("=" * 80)
print("INCREMENTAL TEST 1: Phase 1 - Basic Search Operations")
print("=" * 80)
print("Test Objective: Verify search traversal in single-level and multi-level trees")
print("Configuration: 1 compute + 1 memory server")
print()
print("Test Phases:")
print("  Phase A: Search in empty tree (should not find)")
print("  Phase B: Search after single insert")
print("  Phase C: Multiple searches with different keys")
print()

# Create compute server
compute = sst.Component("compute_0", "rdmaNic.computeServer")
compute.addParams({
    "verbose": 1,
    "node_id": 0,
    "num_memory_nodes": 1,
    "operations_per_second": 50,  # Slow rate for observation
    "simulation_duration_us": 300000,  # 300ms
    "read_ratio": 0.8,  # 80% reads (searches), 20% writes
    "key_range": 20,  # Keys 0-19
    "btree_fanout": 16,  # Large fanout - no splits expected
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
print("  - Fanout: 16 (no splits)")
print("  - ~15 operations total (~12 searches, ~3 inserts)")
print("  - Tree remains single-level")
print()
print("Expected Output:")
print("  ✓ Initial searches: Key NOT found (empty tree)")
print("  ✓ After insert: Key FOUND in root leaf")
print("  ✓ handle_leaf_search() executed for leaf operations")
print("  ✓ Proper lock acquisition and release")
print("  ✓ No crashes or assertion failures")
print()
print("Watch For:")
print("  - 'SEARCH operation: Key X' messages")
print("  - 'Found in leaf node' or 'Not found' messages")
print("  - Lock operations (acquire/release)")
print("=" * 80)
