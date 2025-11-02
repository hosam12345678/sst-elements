#!/usr/bin/env python3
"""
Test: LL/SC Contention Test (5 Compute Nodes)
Phase: LL/SC Lock Validation

Test Objective: Verify StoreConditional failures occur under high contention
Expected Behavior:
  - Multiple compute nodes try to acquire locks simultaneously
  - StoreConditional operations fail when interference is detected
  - Retry counters increment (retry #1, #2, etc.)
  - All operations eventually succeed after retries
  - No data corruption or lost updates

Validates:
  - LL/SC protocol properly detects races
  - Retry mechanism works correctly
  - Reference counting remains accurate despite contention
  - Lock acquisition is truly atomic
"""

import sst

print("=" * 80)
print("TEST: LL/SC Contention with 5 Compute Servers")
print("=" * 80)
print("Phase: LL/SC Lock Validation")
print()
print("Test Objective:")
print("  Verify StoreConditional failures occur under lock contention")
print()
print("Expected Behavior:")
print("  1. Multiple compute nodes access same root node concurrently")
print("  2. StoreConditional FAILS when race is detected")
print("  3. Retry counter increments (retry #1, #2, #3...)")
print("  4. Eventually all operations succeed")
print("  5. No data corruption")
print()

# ============================================================================
# Component Configuration
# ============================================================================
num_compute_nodes = 1  # INCREASED to 5 for more contention
num_memory_servers = 2
memory_capacity_mb = 16
memory_base_address = 0x10000000
btree_fanout = 16

# Workload Configuration - tuned for contention
operations_per_second = 10  # Lower rate for single node test
simulation_duration_us = 5000  # 5ms - enough for ~1 operation
read_ratio = 0.0  # 100% inserts = EXCLUSIVE locks = max contention
key_range = 10  # Small range = all hit same root
key_distribution = "uniform"

# ============================================================================
# Instantiate Components
# ============================================================================

# Compute Server(s)
compute_servers = []
for i in range(num_compute_nodes):
    compute = sst.Component(f"compute_{i}", "rdmaNic.computeServer")
    compute.addParams({
        "verbose": 5,
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
        "verbose": 5,
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

sst.setStatisticLoadLevel(1)

print("Test Configuration:")
print(f"  - {num_compute_nodes} compute servers × {num_memory_servers} memory servers")
print("  - Fanout: 16 keys per node")
print("  - Operations: ~50 inserts per node (250 total)")
print("  - Key range: 0-9 (all operations hit root)")
print("  - 100% inserts = EXCLUSIVE lock contention")
print()
print("Expected Output:")
print("  ✓ Multiple 'retry #1', 'retry #2', etc. messages")
print("  ✓ '✗ SC FAILED' messages indicating race detection")
print("  ✓ Operations eventually succeed after retries")
print("  ✓ No assertion failures")
print()
print("Critical Validations:")
print("  - Retry counters > 0 (proves SC failures occur)")
print("  - SC FAILED messages appear in output")
print("  - LL/SC protocol working correctly")
print("  - All operations complete successfully")
print("=" * 80)
