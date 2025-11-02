#!/usr/bin/env python3
"""
================================================================================
TEST 2.2: Search After Leaf Split
================================================================================
Phase: Simple Splits (No Recursion)

Test Objective:
  Verify that SEARCH operations correctly navigate a 2-level tree after split

Expected Behavior:
  1. Insert enough keys to trigger leaf split (fanout=4)
  2. Tree becomes 2-level with root + 2 leaves
  3. SEARCH for keys in both left and right leaves
  4. All searches correctly navigate through root to appropriate leaf
  5. Validate both FOUND and NOT FOUND cases

Test Configuration:
  - 1 compute server, 1 memory server
  - Fanout: 4 keys per node
  - Operations: ~15 operations (50% search, 50% insert)
  - Key range: 0-7

Expected Output:
  ✓ Initial inserts fill and split leaf
  ✓ Tree height: 1 → 2
  ✓ SEARCH for keys in left leaf (< separator) → FOUND
  ✓ SEARCH for keys in right leaf (≥ separator) → FOUND
  ✓ SEARCH for non-existent keys → NOT FOUND
  ✓ Root correctly routes searches to left/right children

Critical Validations:
  - Root navigation based on separator key
  - Binary search in both leaves works
  - Search traversal follows correct child pointers
  - NOT FOUND handling works in split tree
================================================================================
"""

import sst

# ============================================================================
# Component Configuration
# ============================================================================
clock_rate = "1GHz"
memory_capacity_mb = 16
memory_base_address = 0x10000000

# B+tree Configuration
btree_fanout = 4  # Small fanout to trigger split quickly

# Workload Configuration
workload_type = "ycsb_a"
operations_per_second = 50
simulation_duration_us = 300000  # 300ms
read_ratio = 0.5  # 50% searches, 50% inserts to test search after split

# Key distribution
key_distribution = "uniform"
zipfian_alpha = 0.0
key_range = 8  # 0-7: enough to trigger split and test both leaves

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
