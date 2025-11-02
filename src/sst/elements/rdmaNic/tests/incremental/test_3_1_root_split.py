#!/usr/bin/env python3
"""
================================================================================
TEST 3.1: Root Split (Height 1→2)
================================================================================
Phase: Tree Growth (Height Increase)

Test Objective:
  Verify that when the ROOT node itself becomes full and splits, the tree
  correctly increases its height by creating a new root parent.

Expected Behavior:
  1. Start with empty tree (height = 1, just root leaf)
  2. Fill root leaf to capacity (fanout=4, so 4 keys)
  3. 5th insert triggers ROOT SPLIT:
     - Old root becomes a regular leaf at new address
     - New leaf created for split
     - NEW ROOT created as parent with separator
     - Tree height: 1 → 2
  4. Subsequent operations navigate through new 2-level tree

Test Configuration:
  - 1 compute server, 1 memory server
  - Fanout: 4 keys per node (small to trigger root split quickly)
  - Operations: 100% inserts
  - Key range: 8 keys (enough to fill root and trigger split)

Expected Output:
  ✓ Inserts 1-4: Root fills to capacity (4 keys)
  ✓ Insert 5: "Leaf FULL (4/4) - initiating ASYNC SPLIT"
  ✓ "Splitting ROOT node - will create new root"
  ✓ "Moving old root to new address"
  ✓ "Creating new root (tree height 2 → 3)"
  ✓ New root created with 1 separator key
  ✓ Tree now has 2 levels: root + 2 leaves

Critical Validations:
  - Root split detection when root is full
  - Old root relocated to new address
  - New root created at original root address
  - Separator key promoted to new root
  - Tree height increases by 1
  - Subsequent operations work with new tree structure

This is the foundational test for tree growth - validates the special case
where the root itself needs to split, requiring a new parent root to be created.
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
btree_fanout = 4  # Small fanout to trigger root split with just 5 keys

# Workload Configuration
workload_type = "ycsb_a"
operations_per_second = 50
simulation_duration_us = 300000  # 300ms
read_ratio = 0.0  # 100% inserts to focus on root split

# Key distribution
key_distribution = "uniform"
zipfian_alpha = 0.0
key_range = 8  # 0-7: enough to fill root and trigger split

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
