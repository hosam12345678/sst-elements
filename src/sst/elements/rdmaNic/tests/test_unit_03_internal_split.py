#!/usr/bin/env python3
"""
Unit Test 3: Insert Causing Internal Node Split
Tests: Internal node split when capacity is exceeded
Config: 1 compute server + 1 memory server
Expected: When internal node fills, split and promote separator key to parent
"""

import sst

print("=" * 80)
print("UNIT TEST 3: Insert Causing Internal Node Split")
print("=" * 80)
print("Test Objective: Verify internal node split with recursive propagation")
print("Configuration: 1 compute + 1 memory server")
print("Expected Behavior:")
print("  Phase 1: Build tree to height 2 (1 internal + leaves)")
print("    - Insert keys 0-4 → root leaf splits → height 2")
print("  Phase 2: Fill multiple leaves to create more children")
print("    - Each leaf split adds child to root internal node")
print("    - Continue until root internal has 4 children (capacity)")
print("  Phase 3: Trigger internal node split")
print("    - Next leaf split tries to add 5th child to root")
print("    - Root internal node is full → INTERNAL SPLIT")
print("    - Since root is splitting → new root created → height 3")
print()

# Create compute server
compute = sst.Component("compute_0", "rdmaNic.computeServer")
compute.addParams({
    "verbose": 1,
    "node_id": 0,
    "num_memory_nodes": 1,
    "operations_per_second": 200,  # Faster to generate more keys
    "simulation_duration_us": 500000,  # 500ms for ~100 operations
    "read_ratio": 0.0,  # 100% writes (inserts only)
    "key_range": 30,  # Need many keys: 5 splits * 4 keys/leaf + margin
    "btree_fanout": 4,  # Small fanout
    "key_distribution": "uniform",
})

# Create memory server
memory = sst.Component("memory_0", "rdmaNic.memoryServer")
memory.addParams({
    "verbose": 1,
    "memory_server_id": 0,
    "num_compute_nodes": 1,  # Only 1 compute server in this test
    "memory_size_mb": 16,
    "base_addr": "0x10000000",
})

# Setup network interfaces
compute_iface = compute.setSubComponent("mem_interface_0", "memHierarchy.standardInterface")
memory_iface = memory.setSubComponent("mem_interface_0", "memHierarchy.standardInterface")

# Connect compute to memory
link = sst.Link("compute_memory_link")
link.connect((compute_iface, "lowlink", "1ns"), (memory_iface, "lowlink", "1ns"))

print("Test Setup:")
print("  Fanout: 4 keys per node")
print("  Key range: 0-29 (30 unique keys)")
print("  Operations: ~100 inserts over 500ms")
print()
print("Tree Growth Pattern (fanout=4):")
print("  Keys 0-3:    Root leaf (4 keys)")
print("  Key 4:       🔀 Root splits → height 2, root has 2 children")
print("  Keys 5-7:    Fill 2nd leaf (4 keys)")
print("  Key 8:       🔀 Leaf splits → root has 3 children")
print("  Keys 9-11:   Fill 3rd leaf (4 keys)")
print("  Key 12:      🔀 Leaf splits → root has 4 children (FULL)")
print("  Keys 13-15:  Fill 4th leaf (4 keys)")
print("  Key 16:      🔀 Leaf splits, but root internal is FULL")
print("               🔀 INTERNAL NODE SPLIT (root)")
print("               - Old root internal splits")
print("               - New root created → height 3")
print()
print("Expected Output:")
print("  ✓ Multiple 'LEAF SPLIT' messages (keys 4, 8, 12, 16)")
print("  ✓ 'Parent has space' messages (keys 4, 8, 12)")
print("  ✓ 'Parent FULL' message (key 16)")
print("  ✓ 'INTERNAL SPLIT' message (key 16)")
print("  ✓ 'Splitting ROOT node' (key 16)")
print("  ✓ Tree height increases 2 → 3")
print("=" * 80)
