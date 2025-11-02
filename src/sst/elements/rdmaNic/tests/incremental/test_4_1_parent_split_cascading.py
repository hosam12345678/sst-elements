#!/usr/bin/env python3
"""
================================================================================
TEST 4.1: Parent Split (Cascading/Recursive)
================================================================================
Phase: Recursive Splits

Test Objective:
  Verify that when a leaf split causes its parent to become full, the parent
  correctly splits and propagates the separator upward (cascading split).

Expected Behavior:
  1. Build a tree with multiple leaf splits to fill the parent (internal node)
  2. Parent reaches capacity (fanout=4, so 4 separators = 5 children)
  3. Next leaf split attempts to insert 5th separator → parent FULL
  4. Parent splits, promoting middle separator to grandparent
  5. Cascading split propagates upward through the tree

Test Configuration:
  - 1 compute server, 1 memory server
  - Fanout: 4 (small to trigger parent fill quickly)
  - Operations: 100% inserts
  - Key range: 64 (large enough to trigger multiple splits)
  - Need ~20+ unique inserts to:
    * Trigger 4+ leaf splits (each adds separator to parent)
    * Fill parent to 4 separators (5 children)
    * Trigger parent split on next leaf split

Expected Output:
  ✓ Multiple leaf splits fill parent internal node
  ✓ Parent reaches capacity: 4 keys (5 children)
  ✓ Next leaf split detects "Parent FULL" condition
  ✓ Parent splits into two internal nodes
  ✓ Middle separator from parent promoted to grandparent
  ✓ Tree correctly rebalances with cascading split

Critical Validations:
  - Parent fill detection (4 keys = full for fanout 4)
  - Parent split logic (internal node split)
  - Separator promotion from parent to grandparent
  - Cascading split propagation up the tree
  - Post-split tree structure is correct

This is the key test for recursive splits - validates that splits can
propagate upward through multiple levels of the tree.
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
btree_fanout = 4  # Small fanout to trigger cascading splits

# Workload Configuration
workload_type = "ycsb_a"
operations_per_second = 100  # Higher rate to generate more operations
simulation_duration_us = 500000  # 500ms for more operations
read_ratio = 0.0  # 100% inserts to maximize split opportunities

# Key distribution
key_distribution = "uniform"
zipfian_alpha = 0.0
key_range = 64  # Large range to hit many different keys

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
