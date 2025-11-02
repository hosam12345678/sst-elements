#!/usr/bin/env python3
"""
================================================================================
TEST 5.3: Zipfian Workload (Stress Test)
================================================================================
Phase: Stress Tests

Test Objective:
  Validate B+tree behavior under realistic Zipfian workload distribution
  where a small number of "hot" keys receive the majority of accesses.
  This mimics real-world scenarios where certain data is accessed far more
  frequently than others (e.g., popular products, trending topics).

Expected Behavior:
  1. Insert 1000 keys with Zipfian distribution (alpha=0.9)
  2. Some keys will be accessed very frequently (hot keys)
  3. Most keys will be accessed rarely (cold keys)
  4. All inserted keys should be found during searches
  5. Hot keys should show high access counts in distribution analysis
  6. Tree should handle skewed access patterns without issues

Test Configuration:
  - 1 compute server, 1 memory server
  - Fanout: 4 (standard test fanout)
  - Operations: 1000 with Zipfian distribution
  - Key range: 1000 (0-999)
  - Duration: 2000ms (enough time for 1000+ operations)
  - Read ratio: 0.5 (50% reads, 50% writes - realistic mix)
  - Zipfian alpha: 0.9 (high skew - 80/20 rule applies)

Expected Output:
  ✓ Tree built successfully with all unique keys
  ✓ Key distribution shows clear skew (few hot keys, many cold keys)
  ✓ Hot keys accessed significantly more than cold keys
  ✓ All searches return correct results
  ✓ Tree navigates efficiently even with skewed access
  ✓ No crashes or consistency issues

Critical Validations:
  - Zipfian generator produces valid key distribution
  - Hot keys (low indices) show high access counts
  - Cold keys (high indices) show low access counts
  - Tree handles repeated accesses to same keys (upserts)
  - Search performance remains good despite access skew
  - Memory and CPU stats show realistic workload behavior

This test validates real-world workload patterns where data access
is heavily skewed toward a small subset of popular keys.
================================================================================
"""

import sst

# ============================================================================
# Component Configuration
# ============================================================================
clock_rate = "1GHz"
memory_capacity_mb = 64  # Sufficient for large tree
memory_base_address = 0x10000000

# B+tree Configuration
btree_fanout = 4  # Standard test fanout

# Workload Configuration
workload_type = "ycsb_a"
operations_per_second = 500  # Moderate rate for 1000+ operations
simulation_duration_us = 2000000  # 2000ms = 2 seconds
read_ratio = 0.5  # 50% reads, 50% writes (realistic YCSB-A workload)

# Key distribution - ZIPFIAN for skewed access pattern
key_distribution = "zipfian"
zipfian_alpha = 0.9  # High skew: 80/20 rule (20% of keys get 80% of accesses)
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
        "zipfian_alpha": zipfian_alpha,  # Skew parameter
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
