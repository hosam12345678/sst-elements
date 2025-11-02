#!/usr/bin/env python3
"""
Test 1.3: Single Insert + Search
Phase: Basic Operations (Foundation)

Test Objective: Verify that inserted keys can be found by search
Expected Behavior:
  - Insert key=100 into empty tree succeeds
  - Search for key=100 returns FOUND
  - Search for non-existent key returns NOT FOUND

Validates:
  - Insert operation works correctly
  - Search finds inserted key
  - Integration between insert and search paths
  - Value retrieval after insert
"""

import sst

print("=" * 80)
print("TEST 1.3: Single Insert + Search")
print("=" * 80)
print("Phase: Basic Operations (Foundation)")
print()
print("Test Objective:")
print("  Verify that inserted keys can be found by subsequent search")
print()
print("Expected Behavior:")
print("  1. Tree initializes with empty root leaf (0 keys)")
print("  2. INSERT key=100 succeeds")
print("  3. SEARCH for key=100 returns FOUND with correct value")
print("  4. SEARCH for key=200 (not inserted) returns NOT FOUND")
print()

# Create compute server - configure to do insert then searches
compute = sst.Component("compute_0", "rdmaNic.computeServer")
compute.addParams({
    "verbose": 1,
    "node_id": 0,
    "num_memory_nodes": 1,
    "operations_per_second": 50,  # Slow for observation
    "simulation_duration_us": 200000,  # 200ms
    "read_ratio": 0.7,  # 70% searches, 30% inserts
    "key_range": 5,  # Keys 0-4 (will generate mix of inserts and searches)
    "btree_fanout": 16,  # No splits expected
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
print("  - 1 compute server, 1 memory server")
print("  - Fanout: 16 keys per node")
print("  - Operations: ~10 total (mix of inserts and searches)")
print("  - Key range: 0-4")
print("  - 70% searches, 30% inserts")
print()
print("Expected Output:")
print("  ✓ Initial searches return NOT FOUND (empty tree)")
print("  ✓ After INSERT key=X: '✓ Inserted key=X'")
print("  ✓ Subsequent SEARCH for key=X: '✓ FOUND key=X'")
print("  ✓ Searches for non-inserted keys: '✗ NOT FOUND'")
print("  ✓ Tree height remains 1")
print()
print("Critical Validations:")
print("  - Insert-then-search sequence works correctly")
print("  - Value stored during insert matches value returned by search")
print("  - Multiple inserts and searches interleave properly")
print("  - Lock acquisition/release for both operations")
print("=" * 80)
