#!/usr/bin/env python3
"""
================================================================================
TEST 4.2: Multiple Level Splits (Deep Recursion)
================================================================================
Phase: Recursive Splits

Test Objective:
  Verify that splits can cascade up multiple levels (3+ levels) when each
  parent in the chain becomes full. This tests the deep recursion handling
  of the split propagation mechanism.

Expected Behavior:
  1. Build a deep tree (3-4 levels) with most nodes near capacity
  2. Insert a key that triggers a leaf split
  3. Leaf split causes parent (level 1) to become full → split
  4. Level 1 split causes grandparent (level 2) to become full → split
  5. Level 2 split causes great-grandparent (level 3) to become full → split
  6. Cascading split propagates up 3+ levels
  7. All levels correctly updated with promoted separators

Test Configuration:
  - 1 compute server, 1 memory server
  - Fanout: 4 (small to maximize split opportunities)
  - Operations: 100% inserts
  - Key range: 128 (very large to hit many different ranges)
  - High operation rate: 150 ops/sec
  - Long duration: 800ms (more time to build deep tree)

Expected Output:
  ✓ Tree builds to 3-4 levels deep
  ✓ Multiple internal nodes fill to capacity at different levels
  ✓ Single insert triggers cascading split chain:
    - "Leaf FULL" → leaf splits
    - "Parent FULL" → level 1 internal node splits
    - "Parent FULL" → level 2 internal node splits
    - "Parent FULL" → level 3 internal node splits (if reached)
  ✓ Each split promotes separator to next level
  ✓ Tree remains balanced and navigable after deep cascade

Critical Validations:
  - Deep recursion doesn't cause stack overflow
  - Each level correctly detects full parent condition
  - Separator promotion works across multiple levels
  - Tree structure remains consistent after multi-level cascade
  - Post-cascade operations navigate correctly

This is the stress test for recursive splits - validates that the
implementation can handle arbitrarily deep cascading without issues.
================================================================================
"""

import sst

# ============================================================================
# Component Configuration
# ============================================================================
clock_rate = "1GHz"
memory_capacity_mb = 32  # More memory for larger tree
memory_base_address = 0x10000000

# B+tree Configuration
btree_fanout = 4  # Small fanout to maximize split opportunities

# Workload Configuration
workload_type = "ycsb_a"
operations_per_second = 150  # Higher rate to generate more operations quickly
simulation_duration_us = 800000  # 800ms - longer to build deeper tree
read_ratio = 0.0  # 100% inserts to maximize splits

# Key distribution
key_distribution = "uniform"
zipfian_alpha = 0.0
key_range = 128  # Very large range to spread keys across tree

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
