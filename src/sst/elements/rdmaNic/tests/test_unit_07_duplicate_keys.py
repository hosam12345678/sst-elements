#!/usr/bin/env python3
"""
Unit Test 7: Duplicate Key Insertion
Tests: Handling of duplicate key inserts (update vs reject)
Config: 1 compute server + 1 memory server
Expected: Duplicate keys update existing value, no new entry created
"""

import sst

print("=" * 80)
print("UNIT TEST 7: Duplicate Key Insertion")
print("=" * 80)
print("Test Objective: Verify duplicate key handling")
print("Configuration: 1 compute + 1 memory server")
print("Expected Behavior:")
print("  B+tree duplicate key policy: UPDATE existing value")
print("  1. Insert key=5, value=100")
print("  2. Insert key=5, value=200 → updates existing entry")
print("  3. Node key count stays same (no new entry)")
print("  4. Search key=5 → returns latest value (200)")
print()

# Create compute server
compute = sst.Component("compute_0", "rdmaNic.computeServer")
compute.addParams({
    "verbose": 1,
    "node_id": 0,
    "num_memory_nodes": 1,
    "operations_per_second": 100,
    "simulation_duration_us": 300000,  # 300ms
    "read_ratio": 0.0,  # 100% writes to test duplicates
    "key_range": 3,  # Keys 0-2 (small range forces duplicates)
    "btree_fanout": 16,  # Large fanout - no splits
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
print("  Fanout: 16 keys per node (no splits)")
print("  Key range: 0-2 (only 3 keys)")
print("  Operations: ~30 inserts over 300ms")
print("  Expected: Many duplicate key inserts")
print()
print("Duplicate Key Scenarios:")
print("  Insert #1: key=0, value=0 → New entry, num_keys=1")
print("  Insert #2: key=1, value=1000 → New entry, num_keys=2")
print("  Insert #3: key=0, value=2000 → Duplicate! Update existing")
print("    - Output: '⚠️  Duplicate key=0 - updating value'")
print("    - num_keys stays 2 (no new entry)")
print("    - key=0 now maps to value=2000")
print()
print("Expected Output:")
print("  ✓ Tree height=1 (single root leaf)")
print("  ✓ First inserts of keys 0,1,2 add new entries")
print("  ✓ Subsequent inserts show 'Duplicate key=X' messages")
print("  ✓ Node key count stays at 3 (no growth beyond unique keys)")
print("  ✓ No splits (max 3 keys < fanout 16)")
print("  ✓ Values get updated on duplicate inserts")
print()
print("Key Insight:")
print("  This test ensures duplicate keys don't cause:")
print("    - Array overflow (keys[] going beyond num_keys)")
print("    - Incorrect key counts")
print("    - Unnecessary splits")
print("=" * 80)
