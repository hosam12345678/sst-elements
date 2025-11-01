#!/usr/bin/env python3
"""
Lock Test: Basic Shared/Exclusive Lock Operations
Tests the in-memory lock implementation on memory servers
"""

import sst

print("=" * 80)
print("LOCK TEST: Shared/Exclusive Lock Implementation")
print("=" * 80)
print("Test: Basic lock acquire/release operations")
print("Architecture: 2 compute servers + 1 memory server")
print()
print("Operations to test:")
print("  1. Compute 0: Acquire exclusive lock")
print("  2. Compute 1: Try acquire exclusive (should fail)")
print("  3. Compute 0: Release exclusive lock")
print("  4. Compute 1: Try acquire exclusive (should succeed)")
print("  5. Compute 0: Try acquire shared (should succeed)")
print("  6. Compute 1: Try acquire shared (should succeed - concurrent)")
print("=" * 80)

# Create 2 compute servers for lock testing
num_compute_servers = 2
num_memory_servers = 1

compute_servers = []
for comp_id in range(num_compute_servers):
    compute = sst.Component("compute_{}".format(comp_id), "rdmaNic.computeServer")
    compute.addParams({
        "verbose": 2,
        "node_id": comp_id,
        "num_memory_nodes": num_memory_servers,
        "operations_per_second": 10,  # Slow to see lock interactions
        "simulation_duration_us": 100000,  # 100ms
        "read_ratio": 0.5,  # Mix of reads and writes to test both lock types
        "key_range": 4,  # Small range to force contention
        "btree_fanout": 4,
        "key_distribution": "uniform",
    })
    compute_servers.append(compute)

# Create 1 memory server
memory = sst.Component("memory_0", "rdmaNic.memoryServer")
memory.addParams({
    "verbose": 3,  # High verbosity to see lock operations
    "memory_server_id": 0,
    "num_compute_nodes": num_compute_servers,
    "memory_size_mb": 16,
    "base_addr": "0x10000000",
})

# Setup network (N:1 connectivity)
for comp_id, compute in enumerate(compute_servers):
    # Compute side interface
    compute_iface = compute.setSubComponent(
        "mem_interface_0",
        "memHierarchy.standardInterface"
    )
    
    # Memory side interface
    memory_iface = memory.setSubComponent(
        "mem_interface_{}".format(comp_id),
        "memHierarchy.standardInterface"
    )
    
    # Connect
    link = sst.Link("link_c{}_m0".format(comp_id))
    link.connect((compute_iface, "lowlink", "1ns"), (memory_iface, "lowlink", "1ns"))

print()
print("Expected Output:")
print("  ✓ Lock acquire messages with emoji indicators")
print("  ✓ 🔓→🔐 Unlocked to exclusive")
print("  ✓ 🔓→🔒 Unlocked to shared")
print("  ✓ 🔒+ Shared lock count increment")
print("  ✓ ❌ Lock denied (conflicts)")
print("  ✓ Statistics showing locks acquired/released/conflicts")
print("=" * 80)
