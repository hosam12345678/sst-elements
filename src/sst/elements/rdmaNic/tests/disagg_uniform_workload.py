#!/usr/bin/env python3
"""
Disaggregated Memory B+tree Demo - UNIFORM Key Distribution
Tests disaggregated B+tree with uniform key access patterns.
"""

import sst

# Configuration
num_compute_nodes = 2
num_memory_nodes = 2
total_memory_instances = num_compute_nodes * num_memory_nodes  # Dedicated instances

# Track interfaces for linking
compute_interfaces = {}
memory_interfaces = {}

print(f"=== Disaggregated Memory B+tree: Uniform Distribution ===")
print(f"Architecture: {num_compute_nodes} Compute Servers → {total_memory_instances} Memory Server Instances")
print(f"Key Distribution: UNIFORM (equal probability for all keys)")
print(f"Expected Pattern: All keys should have similar access frequencies")

# Create compute servers with UNIFORM key distribution
compute_servers = []
for compute_id in range(num_compute_nodes):
    compute_server = sst.Component(
        f"compute_server_{compute_id}", 
        "rdmaNic.computeServer"
    )
    compute_server.addParams({
        "verbose": 1,
        "node_id": compute_id,
        "num_memory_nodes": num_memory_nodes,
        "operations_per_second": 1000,
        "simulation_duration_us": 20000,  # 20ms - longer for better statistics
        "read_ratio": 0.7,
        "key_range": 100,  # Small range to see distribution clearly
        "key_distribution": "uniform",  # NEW: Force uniform distribution
        "zipfian_alpha": 0.0,  # Explicit uniform setting
    })
    
    # Create RDMA interfaces as subcomponents for connecting to memory servers
    for memory_id in range(num_memory_nodes):
        interface_name = f"rdma_nic_{memory_id}"
        rdma_interface = compute_server.setSubComponent(interface_name, "memHierarchy.standardInterface")
        rdma_interface.addParams({"debug": 0, "debug_level": 1})
        compute_interfaces[(compute_id, memory_id)] = rdma_interface
    
    compute_servers.append(compute_server)

# Dedicated memory servers - one instance per connection
memory_servers = []
for compute_id in range(num_compute_nodes):
    for memory_id in range(num_memory_nodes):
        # Each memory server instance handles exactly one interface
        memory_server = sst.Component(
            f"memory_server_c{compute_id}_m{memory_id}", 
            "rdmaNic.memoryServer"
        )
        memory_server.addParams({
            "verbose": 1,
            "memory_server_id": memory_id,
            "memory_size_mb": 16,
            "base_addr": hex(0x10000000 + memory_id * 0x1000000),
        })
        
        # Single RDMA interface per dedicated instance
        rdma_interface = memory_server.setSubComponent("rdma_nic", "memHierarchy.standardInterface")
        rdma_interface.addParams({"debug": 0, "debug_level": 1})
        memory_interfaces[(compute_id, memory_id)] = rdma_interface
        
        memory_servers.append(memory_server)

print(f"Created {len(compute_servers)} compute servers and {len(memory_servers)} dedicated memory server instances")

# Create dedicated connections (one-to-one between instances)
connections_created = 0
for compute_id in range(num_compute_nodes):
    for memory_id in range(num_memory_nodes):
        # Link compute server interface to corresponding dedicated memory server instance
        compute_interface = compute_interfaces[(compute_id, memory_id)]
        memory_interface = memory_interfaces[(compute_id, memory_id)]
        
        link_name = f"rdma_link_c{compute_id}_m{memory_id}"
        link = sst.Link(link_name)
        link.connect((compute_interface, "lowlink", "1ns"), 
                    (memory_interface, "lowlink", "1ns"))
        
        connections_created += 1

print(f"Created {connections_created} dedicated RDMA connections")
print("Expected Result: Uniform key distribution with roughly equal access frequencies")