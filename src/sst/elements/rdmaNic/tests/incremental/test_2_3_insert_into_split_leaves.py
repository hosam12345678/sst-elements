#!/usr/bin/env python3
"""
================================================================================
TEST 2.3: Leaf Split - Insert into Split Leaves
================================================================================
Phase: Simple Splits (No Recursion)

Test Objective:
  Verify that INSERT operations correctly navigate to left/right leaves after split

Expected Behavior:
  1. Trigger initial leaf split (similar to Test 2.1)
  2. Tree becomes 2-level: Root + 2 leaves with separator
  3. INSERT keys < separator → route to LEFT leaf
  4. INSERT keys ≥ separator → route to RIGHT leaf
  5. Both leaves maintain sorted order

Test Configuration:
  - 1 compute server, 1 memory server
  - Fanout: 4 keys per node
  - Operations: 100% inserts
  - Key range: 0-9 (enough to split and insert into both leaves)

Expected Output:
  ✓ Initial inserts fill root to 4 keys
  ✓ 5th insert triggers split with separator (e.g., separator=2)
  ✓ Subsequent inserts < separator go to LEFT leaf
  ✓ Subsequent inserts ≥ separator go to RIGHT leaf
  ✓ Root correctly routes inserts: "→ Continue to child[0]" or "child[1]"
  ✓ Both leaves maintain sorted order after all inserts

Critical Validations:
  - Root navigation based on separator comparison
  - Inserts distributed correctly to left/right leaves
  - Both leaves maintain sorted invariant
  - No incorrect routing
================================================================================
"""

import sst

# ============================================================================
# Component Configuration
# ============================================================================
clock_rate = "1GHz"
memory_capacity_mb = 16384  # 16 GB
memory_base_address = 0x10000000

# B+tree Configuration
btree_fanout = 4  # Small fanout to trigger split quickly

# Workload Configuration
workload_type = "ycsb_a"
operations_per_second = 50
simulation_duration_us = 300000  # 300ms
read_ratio = 0.0  # 100% inserts to focus on insert routing

# Key distribution
key_distribution = "uniform"
zipfian_alpha = 0.0
key_range = 32  # 0-31: allows keys like 12 and 25

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
