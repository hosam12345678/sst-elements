#!/usr/bin/env python3
"""
Test 2.1: Single Node B+Tree Initialization
Phase: Root Metadata Locking

Test Objective: First compute node initializes B+tree correctly
Expected Sequence:
  1. Check VALIDITY_BIT → returns 0 (uninitialized)
  2. Allocate root node from Memory Server
  3. Initialize root as leaf node
  4. Write ROOT_METADATA (root_address, tree_height)
  5. Write VALIDITY_BIT = 1
  6. Complete INSERT operation

Validates:
  - Single-node initialization sequence
  - VALIDITY_BIT atomic check
  - ROOT_METADATA write
  - Correct root node allocation
"""

import sst

print("=" * 80)
print("TEST 2.1: Single Node B+Tree Initialization")
print("=" * 80)
print("Phase: Root Metadata Locking")
print()
print("Test Objective:")
print("  Verify single compute node initializes B+tree correctly")
print()
print("Expected Behavior:")
print("  1. VALIDITY_BIT check returns 0 (uninitialized)")
print("  2. Root node allocated from Memory Server")
print("  3. ROOT_METADATA written (root_address, tree_height=1)")
print("  4. VALIDITY_BIT written = 1")
print("  5. First INSERT operation completes")
print()

# ============================================================================
# Component Configuration
# ============================================================================
num_compute_nodes = 1  # Single node test
num_memory_servers = 2
btree_fanout = 4       # Small fanout to trigger splits with fewer inserts

# Workload Configuration
operations_per_second = 10   # Moderate rate
simulation_duration_us = 500000  # 500ms
read_ratio = 0.0  # 100% writes
key_range = 100  # Enough keys to trigger splits
key_distribution = "uniform"
zipfian_alpha = 0.0

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

sst.setStatisticLoadLevel(1)

print("Test Configuration:")
print(f"  - {num_compute_nodes} compute server × {num_memory_servers} memory servers")
print(f"  - Fanout: {btree_fanout} keys per node")
print(f"  - Workload: 100% INSERT (read_ratio=0.0)")
print(f"  - Ops/sec: {operations_per_second}")
print(f"  - Duration: {simulation_duration_us/1000}ms")
print()
print("Expected Output:")
print("  ✓ 'Initializing B+tree with root metadata at 0x...'")
print("  ✓ Root node allocation message")
print("  ✓ ROOT_METADATA write")
print("  ✓ VALIDITY_BIT write")
print("  ✓ First INSERT completes successfully")
print()
print("Critical Validations:")
print("  - Exactly ONE initialization message")
print("  - VALIDITY_BIT transitions 0 → 1")
print("  - ROOT_METADATA contains valid address and height=1")
print("  - No race conditions (single node)")
print("=" * 80)
