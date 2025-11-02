#!/usr/bin/env python3
"""
Test 2.1: Leaf Split - Fill and Split
Phase: Simple Splits (No Recursion)

Test Objective: Verify basic leaf split mechanism
Expected Behavior:
  - Insert 4 keys with fanout=4 (fills root leaf to capacity)
  - Next insert triggers leaf split
  - Left leaf gets first half of keys
  - Right leaf gets second half of keys
  - Separator key promoted to parent
  - Tree height increases 1 → 2

Validates:
  - Leaf split detection (num_keys == fanout)
  - Key redistribution (middle-based split)
  - Separator key promotion
  - New root creation
  - Sibling pointer maintenance (next_leaf)
"""

import sst

print("=" * 80)
print("TEST 2.1: Leaf Split - Fill and Split")
print("=" * 80)
print("Phase: Simple Splits (No Recursion)")
print()
print("Test Objective:")
print("  Verify that a full leaf node correctly splits when inserting next key")
print()
print("Expected Behavior:")
print("  1. Tree initializes with empty root leaf (fanout=4, max 4 keys)")
print("  2. INSERT keys 0, 1, 2, 3 → root fills to capacity (4 keys)")
print("  3. INSERT key 4 → triggers LEAF SPLIT:")
print("     - Left leaf: [0, 1]")
print("     - Right leaf: [2, 3, 4]")
print("     - Separator key=2 promoted to new root")
print("     - Tree height: 1 → 2")
print("  4. New root (internal node) contains separator")
print("  5. Root points to left and right leaves")
print()

# Create compute server
compute = sst.Component("compute_0", "rdmaNic.computeServer")
compute.addParams({
    "verbose": 1,
    "node_id": 0,
    "num_memory_nodes": 1,
    "operations_per_second": 50,  # Slow for observation
    "simulation_duration_us": 300000,  # 300ms
    "read_ratio": 0.0,  # 100% inserts to trigger split
    "key_range": 6,  # Keys 0-5 (need 5+ to trigger split)
    "btree_fanout": 4,  # Small fanout to trigger split quickly
    "key_distribution": "uniform",
})

# Create memory server
memory = sst.Component("memory_0", "rdmaNic.memoryServer")
memory.addParams({
    "verbose": 1,
    "memory_server_id": 0,
    "num_compute_nodes": 1,
    "memory_size_mb": 16,
    "base_addr": "0x10000000",
})

# Setup network interfaces
compute_iface = compute.setSubComponent("mem_interface_0", "memHierarchy.standardInterface")
memory_iface = memory.setSubComponent("mem_interface_0", "memHierarchy.standardInterface")

# Connect compute to memory
link = sst.Link("compute_memory_link")
link.connect((compute_iface, "lowlink", "1ns"), (memory_iface, "lowlink", "1ns"))

print("Test Configuration:")
print("  - 1 compute server, 1 memory server")
print("  - Fanout: 4 keys per node (triggers split at 5th insert)")
print("  - Operations: ~15 inserts")
print("  - Key range: 0-5")
print()
print("Expected Output:")
print("  ✓ Inserts 1-4: Root fills up (num_keys increases to 4)")
print("  ✓ Insert 5: 🔀 'Leaf FULL, triggering split'")
print("  ✓ 'Splitting ROOT node' (since root is the leaf)")
print("  ✓ 'Left leaf: X keys, Right leaf: Y keys'")
print("  ✓ 'Separator key=X promoted'")
print("  ✓ 'New root created' at new address")
print("  ✓ Tree height: 1 → 2")
print()
print("Critical Validations:")
print("  - handle_leaf_split() detects full leaf")
print("  - Middle-based split distributes keys evenly")
print("  - handle_root_split() creates new root")
print("  - Left and right leaves written to memory")
print("  - Parent updated with separator")
print("=" * 80)
