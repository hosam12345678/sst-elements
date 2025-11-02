#!/usr/bin/env python3
"""
Test 1.1: Empty Tree Search
Phase: Basic Operations (Foundation)

Test Objective: Verify search behavior in an empty tree
Expected Behavior:
  - Search for key in empty tree returns NOT FOUND
  - Traversal reaches root leaf node (tree is single-level)
  - No errors or crashes occur

Validates:
  - Traversal to empty root
  - Search logic in empty leaf
  - Proper handling of empty key array
"""

import sst

print("=" * 80)
print("TEST 1.1: Empty Tree Search")
print("=" * 80)
print("Phase: Basic Operations (Foundation)")
print()
print("Test Objective:")
print("  Search for keys in a completely empty B+tree")
print()
print("Expected Behavior:")
print("  1. Tree initializes with empty root leaf (0 keys)")
print("  2. All searches return NOT FOUND")
print("  3. Traversal completes without errors")
print("  4. Tree height remains 1 (single-level)")
print()

# ============================================================================
# Component Configuration
# ============================================================================
num_compute_nodes = 2  # Multiple compute nodes
num_memory_servers = 2  # Multiple memory servers
memory_capacity_mb = 16
memory_base_address = 0x10000000
btree_fanout = 16

# Workload Configuration
operations_per_second = 100
simulation_duration_us = 100000  # 100ms
read_ratio = 1.0  # 100% reads (searches only, NO inserts)
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
print("  - Operations: ~10 searches only (NO inserts)")
print("  - Key range: 0-9")
print("  - Tree starts empty (root with 0 keys)")
print()
print("Expected Output:")
print("  ✓ Tree initializes with height=1, root has 0 keys")
print("  ✓ Every SEARCH operation returns '✗ NOT FOUND'")
print("  ✓ Traversal reaches leaf at 0x10000000 with 0 keys")
print("  ✓ No assertion failures or crashes")
print()
print("Critical Validations:")
print("  - handle_leaf_search() processes empty leaf correctly")
print("  - No array out-of-bounds access with num_keys=0")
print("  - Lock operations complete successfully")
print("=" * 80)
