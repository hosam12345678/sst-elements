#!/usr/bin/env python3
"""
Test 5.1: Sequential Inserts (Stress Test)

Purpose: Validate B+tree behavior under worst-case sequential insert pattern
Scenario: Insert 1000 keys sequentially (1, 2, 3, ...), then search random keys
Expected: All keys found, tree remains balanced despite sequential pattern
Validates: 
  - Worst-case sequential insertion handling
  - Tree balance maintenance with monotonic keys
  - Large-scale tree construction (many splits)
  - Search performance after many sequential splits

Configuration:
  - Fanout: 4 (triggers many splits)
  - Operations: 1000 inserts sequentially
  - Key range: 1-1000 (sequential)
  - Duration: 2000ms (longer for more operations)
  - Memory: 64MB (more capacity for larger tree)
  - Read ratio: 0.2 (mix searches to validate correctness)

Sequential Pattern Challenge:
  - B+trees can become unbalanced with purely sequential inserts
  - All inserts go to rightmost leaf, causing repeated right-side splits
  - Should still maintain O(log N) height and search performance
  - This test validates split cascading up the right spine of the tree
"""

import sst

# ============================================================================
# Component Configuration
# ============================================================================
clock_rate = "1GHz"
memory_capacity_mb = 64  # More memory for larger tree
memory_base_address = 0x10000000

# B+tree Configuration
btree_fanout = 4  # Small fanout to trigger many splits

# Workload Configuration
workload_type = "ycsb_a"
operations_per_second = 500  # Higher rate for more operations
simulation_duration_us = 2000000  # 2000ms = 2 seconds
read_ratio = 0.2  # 20% reads, 80% writes (mostly inserts)

# Key distribution
key_distribution = "uniform"
zipfian_alpha = 0.0
key_range = 1000  # Sequential keys 0-999

# Network Configuration
num_compute_nodes = 1
num_memory_servers = 1

# ============================================================================
# Instantiate Components
# ============================================================================

# Compute Server(s)
compute_servers = []
for i in range(num_compute_nodes):
    compute = sst.Component(f"compute_{i}", "rdmaNic.computeServer")
    compute.addParams({
        "verbose": 1,
        "node_id": i,
        "num_memory_nodes": num_memory_servers,
        "operations_per_second": operations_per_second,
        "simulation_duration_us": simulation_duration_us,
        "read_ratio": read_ratio,
        "key_distribution": key_distribution,
        "key_range": key_range,
        "btree_fanout": btree_fanout,
    })
    compute_servers.append(compute)

# Memory Server(s)
memory_servers = []
for i in range(num_memory_servers):
    memory = sst.Component(f"memory_{i}", "rdmaNic.memoryServer")
    memory.addParams({
        "verbose": 1,
        "memory_server_id": i,
        "num_compute_nodes": num_compute_nodes,
        "memory_size_mb": memory_capacity_mb,
        "base_addr": f"0x{memory_base_address + (i * (memory_capacity_mb << 20)):x}",
    })
    memory_servers.append(memory)

# ============================================================================
# Connect Compute ↔ Memory (Many-to-Many)
# ============================================================================
for comp_idx, compute in enumerate(compute_servers):
    for mem_idx, memory in enumerate(memory_servers):
        # Setup subcomponents for interfaces
        compute_iface = compute.setSubComponent(f"mem_interface_{mem_idx}", "memHierarchy.standardInterface")
        memory_iface = memory.setSubComponent(f"mem_interface_{comp_idx}", "memHierarchy.standardInterface")
        
        # Connect compute to memory
        link = sst.Link(f"link_c{comp_idx}_m{mem_idx}")
        link.connect((compute_iface, "lowlink", "1ns"), (memory_iface, "lowlink", "1ns"))

# ============================================================================
# Statistics (Optional)
# ============================================================================
sst.setStatisticLoadLevel(1)
