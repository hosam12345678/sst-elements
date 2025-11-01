#!/usr/bin/env python3
"""
Distributed Test 1: Many Compute Servers to Many Memory Servers (M:N)
Tests: B+tree node distribution across multiple memory servers
Config: M compute servers + N memory servers
Expected: Nodes distributed across servers, tree operations work correctly
"""

import sst

# Configuration - easily change to test different architectures
num_compute_servers = 1   # 1 = One-to-Many, >1 = Many-to-Many
num_memory_servers = 4    # Can be changed to 2, 4, 8, etc.

print("=" * 80)
if num_compute_servers == 1:
    print("DISTRIBUTED TEST 1: One-to-Many ({} Compute + {} Memory Servers)".format(num_compute_servers, num_memory_servers))
else:
    print("DISTRIBUTED TEST 1: Many-to-Many ({} Compute + {} Memory Servers)".format(num_compute_servers, num_memory_servers))
print("=" * 80)
print("Test Objective: Verify B+tree distribution across multiple memory servers")
print("Configuration: {} compute + {} memory servers".format(num_compute_servers, num_memory_servers))
print("Expected Behavior:")
print("  1. Root node allocated to one memory server")
print("  2. As tree grows, nodes distributed across all {} servers".format(num_memory_servers))
print("  3. Node N allocated to Memory Server (N % {})".format(num_memory_servers))
print("  4. Tree operations work across server boundaries")
print("  5. All {} memory servers show activity".format(num_memory_servers))
print()

# Create M compute servers
compute_servers = []
for comp_id in range(num_compute_servers):
    compute = sst.Component("compute_{}".format(comp_id), "rdmaNic.computeServer")
    compute.addParams({
        "verbose": 1,
        "node_id": comp_id,
        "num_memory_nodes": num_memory_servers,
        "operations_per_second": 100,
        "simulation_duration_us": 300000,  # 300ms
        "read_ratio": 0.0,  # 100% writes to trigger splits
        "key_range": 16,  # 16 keys to generate multiple nodes
        "btree_fanout": 4,  # Small fanout to force splits
        "key_distribution": "uniform",
    })
    compute_servers.append(compute)

# Create N memory servers
memory_servers = []
for mem_id in range(num_memory_servers):
    memory = sst.Component("memory_{}".format(mem_id), "rdmaNic.memoryServer")
    memory.addParams({
        "verbose": 1,
        "memory_server_id": mem_id,
        "num_compute_nodes": num_compute_servers,
        "memory_size_mb": 16,
        "base_addr": "0x10000000",
    })
    memory_servers.append(memory)

# Setup network interfaces and links (Many-to-Many connectivity)
# Each compute server connects to all memory servers
for comp_id, compute in enumerate(compute_servers):
    for mem_id, memory in enumerate(memory_servers):
        # Create interface on compute side
        compute_iface = compute.setSubComponent(
            "mem_interface_{}".format(mem_id), 
            "memHierarchy.standardInterface"
        )
        
        # Create interface on memory side
        memory_iface = memory.setSubComponent(
            "mem_interface_{}".format(comp_id), 
            "memHierarchy.standardInterface"
        )
        
        # Connect them with a link
        link = sst.Link("link_c{}_m{}".format(comp_id, mem_id))
        link.connect((compute_iface, "lowlink", "1ns"), (memory_iface, "lowlink", "1ns"))

print("Test Setup:")
print("  Compute servers: {}".format(num_compute_servers))
print("  Memory servers: {}".format(num_memory_servers))
print("  Fanout: 4 keys per node")
print("  Key range: 0-15 (16 keys per compute)")
print("  Operations: 100% writes to trigger splits")
print("  Expected: Multiple nodes distributed across {} memory servers".format(num_memory_servers))
print()
print("Expected Output:")
print("  ✓ {} compute server(s) start operations".format(num_compute_servers))
print("  ✓ Node allocation messages showing different server IDs")
print("  ✓ 'Allocated Node X (Level Y) → Memory Server Z'")
print("  ✓ Mix of server IDs in allocation pattern")
print("  ✓ Tree operations complete successfully")
print("  ✓ All {} memory servers show remote reads/writes in statistics".format(num_memory_servers))
if num_compute_servers > 1:
    print("  ✓ Concurrent operations from {} compute servers visible".format(num_compute_servers))
print("=" * 80)
