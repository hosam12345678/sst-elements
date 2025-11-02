#!/usr/bin/env python3
"""
================================================================================
TEST 6.3: Large Fanout (Edge Case)
================================================================================
Phase: Edge Cases

Test Objective:
  Validate B+tree behavior with large fanout (256), which creates very wide,
  shallow trees. This tests the opposite extreme from small fanout tests and
  validates that the implementation scales to realistic production fanout values.

Expected Behavior:
  1. Fanout=256 means each node can hold up to 256 keys
  2. With large fanout, far fewer splits occur
  3. Tree remains very shallow (1-2 levels) even with many keys
  4. Each node stores many keys before splitting
  5. Search still efficient (fewer levels to traverse)
  6. Memory usage per node is higher but total nodes fewer

Test Configuration:
  - 1 compute server, 1 memory server
  - Fanout: 256 (large production-like value)
  - Operations: 1000+ insertions
  - Key range: 1000 (0-999)
  - Duration: 2000ms
  - Read ratio: 0.3 (30% reads for validation)
  - Distribution: Uniform for predictable testing

Expected Output:
  ✓ Tree builds successfully with large fanout
  ✓ Very few splits occur (each node holds 256 keys)
  ✓ Tree height remains 1-2 levels despite many keys
  ✓ All searches return correct results
  ✓ Wide nodes serialize/deserialize correctly
  ✓ Memory usage shows fewer but larger nodes

Critical Validations:
  - Node serialization works with 256 keys + 257 children
  - Split logic handles large fanout correctly
  - Search navigation works in wide shallow trees
  - Memory layout supports large node sizes
  - Performance benefits of shallow tree (fewer reads)
  - No buffer overflows with large node data

Comparison with Small Fanout:
  - Fanout=4: ~200 splits for 1000 keys, tree height ~5-6
  - Fanout=256: ~4 splits for 1000 keys, tree height ~2
  - Trade-off: Fewer I/Os (shallow) vs more data per I/O (wide nodes)

This test validates scalability to production-realistic fanout values
where B+trees are typically tuned to page/block sizes (e.g., 4KB pages).
================================================================================
"""

import sst

# ============================================================================
# Component Configuration
# ============================================================================
clock_rate = "1GHz"
memory_capacity_mb = 128  # More memory for larger nodes
memory_base_address = 0x10000000

# B+tree Configuration
btree_fanout = 256  # LARGE fanout - production-like value

# Workload Configuration
workload_type = "ycsb_a"
operations_per_second = 500  # Moderate rate
simulation_duration_us = 2000000  # 2000ms = 2 seconds
read_ratio = 0.3  # 30% reads, 70% writes for testing

# Key distribution
key_distribution = "uniform"
zipfian_alpha = 0.0
key_range = 1000  # 1000 keys (0-999)

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
