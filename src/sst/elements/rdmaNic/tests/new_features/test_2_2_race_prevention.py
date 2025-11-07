#!/usr/bin/env python3
"""
Test 2.2: Multi-Node Race Prevention During Initialization
Phase: Root Metadata Locking (CRITICAL)

Test Objective: Multiple compute nodes don't race during B+tree initialization
Expected Behavior:
  - Only ONE node completes initialization (atomic VALIDITY_BIT check)
  - Other nodes detect VALIDITY_BIT=1 and skip initialization
  - All nodes eventually read ROOT_METADATA and complete operations
  - No duplicate root allocations
  - No corrupted metadata

Validates:
  - Atomic VALIDITY_BIT check-and-set
  - Race condition prevention
  - Multi-node coordination
  - Metadata consistency
"""

import sst

print("=" * 80)
print("TEST 2.2: Multi-Node Race Prevention During Initialization")
print("=" * 80)
print("Phase: Root Metadata Locking (CRITICAL)")
print()
print("Test Objective:")
print("  Verify multiple compute nodes don't race during initialization")
print()
print("Expected Behavior:")
print("  1. All 4 nodes start simultaneously at t=0")
print("  2. All 4 nodes check VALIDITY_BIT (initially 0)")
print("  3. ONE node wins race, initializes tree")
print("  4. Other 3 nodes detect VALIDITY_BIT=1, skip init")
print("  5. All 4 nodes read ROOT_METADATA")
print("  6. All operations complete successfully")
print()

# ============================================================================
# Component Configuration
# ============================================================================
num_compute_nodes = 4  # All start simultaneously - race condition test
num_memory_servers = 4
btree_fanout = 16

# Workload Configuration
operations_per_second = 1000  # Aggressive to increase race window
simulation_duration_us = 1000000  # 1 second
read_ratio = 0.0  # 100% writes to trigger initialization immediately
key_range = 10000
key_distribution = "uniform"
zipfian_alpha = 0.0  # Uniform to spread keys

# ============================================================================
# Instantiate Components
# ============================================================================

# Compute Server(s)
compute_servers = []
for i in range(num_compute_nodes):
    compute = sst.Component(f"compute_{i}", "rdmaNic.computeServer")
    compute.addParams({
        "verbose": 2,
        "node_id": i,
        "num_memory_nodes": num_memory_servers,
        "operations_per_second": operations_per_second,
        "simulation_duration_us": simulation_duration_us,
        "read_ratio": read_ratio,
        "key_distribution": key_distribution,
        "zipfian_alpha": zipfian_alpha,
        "key_range": key_range,
        "btree_fanout": btree_fanout,
    })
    compute_servers.append(compute)

# Memory Server(s)
memory_servers = []
for i in range(num_memory_servers):
    memory = sst.Component(f"memory_{i}", "rdmaNic.memoryServer")
    memory.addParams({
        "verbose": 2,
        "memory_server_id": i,
        "num_compute_nodes": num_compute_nodes,
    })
    memory_servers.append(memory)

# ============================================================================
# Connect Compute ↔ Memory (Full Mesh: Every compute to every memory)
# ============================================================================
for comp_idx, compute in enumerate(compute_servers):
    for mem_idx, memory in enumerate(memory_servers):
        # Setup subcomponents for interfaces
        compute_iface = compute.setSubComponent(f"mem_interface_{mem_idx}", "memHierarchy.standardInterface")
        memory_iface = memory.setSubComponent(f"mem_interface_{comp_idx}", "memHierarchy.standardInterface")
        
        # Connect compute to memory
        link = sst.Link(f"link_c{comp_idx}_m{mem_idx}")
        link.connect((compute_iface, "lowlink", "1ns"), (memory_iface, "lowlink", "1ns"))

sst.setStatisticLoadLevel(1)

print("Test Configuration:")
print(f"  - {num_compute_nodes} compute servers × {num_memory_servers} memory servers (full mesh)")
print(f"  - Fanout: {btree_fanout} keys per node")
print(f"  - Workload: 100% INSERT (read_ratio=0.0)")
print(f"  - Ops/sec: {operations_per_second} per node (aggressive)")
print(f"  - Duration: {simulation_duration_us/1000}ms")
print()
print("Expected Output:")
print("  ✓ Exactly ONE 'Initializing B+tree' message")
print("  ✓ Three nodes detect VALIDITY_BIT=1 and skip init")
print("  ✓ All 4 nodes complete operations successfully")
print("  ✓ No duplicate root allocations")
print("  ✓ VALIDITY_BIT written exactly once")
print()
print("Critical Validations:")
print("  - Atomic VALIDITY_BIT check prevents double initialization")
print("  - No race conditions in metadata writes")
print("  - All nodes see consistent ROOT_METADATA")
print("  - Final tree structure is valid")
print()
print("🔴 CRITICAL TEST: Watch for race conditions!")
print("=" * 80)
