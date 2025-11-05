#!/usr/bin/env python3
"""
Test 1.2: Single Insert
Phase: Basic Operations (Foundation)

Test Objective: Verify basic insert operation into an empty tree
Expected Behavior:
  - Insert keys into empty tree succeeds
  - Tree grows correctly
  - Inserts complete without errors
  - LL/SC lock acquisition works with write operations

Validates:
  - Basic insert operation
  - Leaf modification (adding key to keys array)
  - Node serialization and write back to memory
  - LL/SC lock protocol with concurrent inserts
"""

import sst

print("=" * 80)
print("TEST 1.2: Basic Insert Operations")
print("=" * 80)
print("Phase: Basic Operations (Foundation)")
print()
print("Test Objective:")
print("  Insert keys into empty B+tree with concurrent compute nodes")
print()
print("Expected Behavior:")
print("  1. Tree initializes with empty root leaf (0 keys)")
print("  2. Inserts add keys to tree successfully")
print("  3. LL/SC lock acquisition works correctly")
print("  4. Lock release with LL/SC completes without errors")
print()

# ============================================================================
# Component Configuration
# ============================================================================
num_compute_nodes = 2 # Multiple compute nodes
num_memory_servers = 2  # Multiple memory servers
memory_capacity_mb = 16
memory_base_address = 0x10000000
btree_fanout = 16

# Workload Configuration
operations_per_second = 100
simulation_duration_us = 200000  # 200ms
read_ratio = 0.3  # 30% reads (70% inserts)
key_range = 10  # Keys 0-9
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
print(f"  - {num_compute_nodes} compute servers × {num_memory_servers} memory servers (many-to-many)")
print("  - Fanout: 16 keys per node")
print("  - Operations: ~10 inserts per node")
print("  - Key range: 0-9")
print("  - Tree starts empty (root with 0 keys)")
print()
print("Expected Output:")
print("  ✓ Tree initializes with height=1, root has 0 keys")
print("  ✓ INSERT operations add keys to tree")
print("  ✓ LL/SC lock acquisition succeeds (retry #0 or low retry count)")
print("  ✓ Lock release with LL/SC completes successfully")
print("  ✓ No assertion failures or crashes")
print()
print("Critical Validations:")
print("  - LL/SC lock acquisition works with EXCLUSIVE locks (inserts)")
print("  - Lock release uses LL/SC protocol")
print("  - Reference counts handled correctly")
print("  - No deadlocks or race conditions")
print("=" * 80)
