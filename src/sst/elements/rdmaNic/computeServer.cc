// Copyright 2009-2025 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2025, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top-level directory of the
// distribution.
//
// Disaggregated Memory Compute Server Implementation
// Implements B+tree operations using RDMA to remote memory servers
//
// ═══════════════════════════════════════════════════════════════════════════
// MEMORY LAYOUT ARCHITECTURE
// ═══════════════════════════════════════════════════════════════════════════
//
// This system implements a disaggregated memory architecture where:
// - COMPUTE SERVERS: Run application logic, issue RDMA operations
// - MEMORY SERVERS: Store data, respond to RDMA read/write requests
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │                    REMOTE MEMORY ADDRESS SPACE                          │
// │                    (Disaggregated Memory Servers)                       │
// └─────────────────────────────────────────────────────────────────────────┘
//
// Each Memory Server has 16 MB of address space:
//
//   Memory Server 0: 0x10000000 - 0x10FFFFFF  (16 MB)
//   Memory Server 1: 0x11000000 - 0x11FFFFFF  (16 MB)
//   Memory Server 2: 0x12000000 - 0x12FFFFFF  (16 MB)
//   Memory Server 3: 0x13000000 - 0x13FFFFFF  (16 MB)
//   ...
//   Memory Server N: 0x10000000 + N*0x1000000 to 0x10000000 + (N+1)*0x1000000
//
// Within EACH Memory Server's 16MB space, B+tree nodes are organized by level:
//
//   ┌──────────────────────────────────────────────────────────────┐
//   │ Address Range        │ Tree Level      │ Size      │ Usage   │
//   ├──────────────────────────────────────────────────────────────┤
//   │ 0x00000 - 0x0FFFF   │ Root (Level 0)   │ 64 KB     │ Root    │
//   │ 0x10000 - 0x1FFFF   │ Level 1          │ 64 KB     │ Internal│
//   │ 0x20000 - 0x3FFFF   │ Level 2          │ 128 KB    │ Internal│
//   │ 0x40000 - 0x1FFFFF  │ Level 3+         │ 1.75 MB   │ Internal│
//   │ 0x200000 - 0xFFFFFF │ Leaves           │ 14 MB     │ Leaf    │
//   └──────────────────────────────────────────────────────────────┘
//
// Example: Root node on Memory Server 0 = 0x10000000
//          Leaf node on Memory Server 2 = 0x12200000 + offset
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │                    LOCAL COMPUTE SERVER MEMORY                          │
// │                    (Temporary RDMA Read Buffers)                        │
// └─────────────────────────────────────────────────────────────────────────┘
//
// Compute servers use local memory to temporarily store data fetched via RDMA:
//
//   ┌──────────────────────────────────────────────────────────────┐
//   │ Buffer Address       │ Purpose                    │ Size     │
//   ├──────────────────────────────────────────────────────────────┤
//   │ 0x2000000-0x20FFFFF │ Tree traversal buffers      │ 1 MB     │
//   │   0x2000000         │   - Level 0 read buffer     │ 64 KB    │
//   │   0x2010000         │   - Level 1 read buffer     │ 64 KB    │
//   │   0x2020000         │   - Level 2 read buffer     │ 64 KB    │
//   │   ...               │   - (+ 0x10000 per level)   │          │
//   │ 0x3000000           │ Leaf node read buffer       │ 64 KB    │
//   │ 0x4000000           │ Parent node buffer (splits) │ 64 KB    │
//   │ 0x5000000-0x50FFFFF │ find_parent traversal bufs  │ 1 MB     │
//   │ 0x6000000           │ Parent verification buffer  │ 64 KB    │
//   └──────────────────────────────────────────────────────────────┘
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │                    COMPUTE-TO-MEMORY INTERACTION                        │
// └─────────────────────────────────────────────────────────────────────────┘
//
// Compute Server operates on B+tree stored across memory servers:
//
// 1. RDMA READ Operation:
//    compute_server.rdma_read(remote_addr=0x10200000, size=512, local_buf=0x2000000)
//    ↓
//    Extracts target memory server: (0x10200000 - 0x10000000) / 0x1000000 = 0
//    ↓
//    Sends read request to Memory Server 0 via rdma_interface[0]
//    ↓
//    Memory Server 0 responds with data from address 0x10200000
//    ↓
//    Data arrives at compute server's local buffer 0x2000000
//
// 2. RDMA WRITE Operation:
//    compute_server.rdma_write(remote_addr=0x12300000, size=512, local_buf=0x3000000)
//    ↓
//    Extracts target memory server: (0x12300000 - 0x10000000) / 0x1000000 = 2
//    ↓
//    Sends write request to Memory Server 2 via rdma_interface[2]
//    ↓
//    Memory Server 2 stores data at address 0x12300000
//
// 3. Tree Traversal Example (search for key=12345):
//    Step 1: Read root from Server 0
//      rdma_read(0x10000000, sizeof(node), 0x2000000)  // Level 0 buffer
//    Step 2: Read internal node from Server 1
//      rdma_read(0x11010000, sizeof(node), 0x2010000)  // Level 1 buffer
//    Step 3: Read leaf from Server 3
//      rdma_read(0x13250000, sizeof(node), 0x3000000)  // Leaf buffer
//    Step 4: Search key in local buffer 0x3000000
//
// ═══════════════════════════════════════════════════════════════════════════

#include <sst_config.h>
#include "computeServer.h"
#include <sst/core/interfaces/stdMem.h>
#include <cstdio>
#include <cassert>

// ═══════════════════════════════════════════════════════════════════════════
// MEMORY ADDRESS CONSTANTS
// ═══════════════════════════════════════════════════════════════════════════

// Remote Memory Server Address Space
const uint64_t MEMORY_BASE_ADDRESS = 0x10000000;   // Base address for all memory servers
const uint64_t MEMORY_SERVER_SIZE  = 0x1000000;    // 16 MB per memory server
// Memory Server N range: [MEMORY_BASE_ADDRESS + N*MEMORY_SERVER_SIZE, 
//                         MEMORY_BASE_ADDRESS + (N+1)*MEMORY_SERVER_SIZE)

// Per-Server B+tree Level Offsets (relative to server's base)
const uint64_t BTREE_LEVEL0_OFFSET = 0x00000;      // Root level: 0-64KB
const uint64_t BTREE_LEVEL1_OFFSET = 0x10000;      // Level 1: 64KB-128KB
const uint64_t BTREE_LEVEL2_OFFSET = 0x20000;      // Level 2: 128KB-256KB
const uint64_t BTREE_LEVEL3_OFFSET = 0x40000;      // Level 3+: 256KB-2MB
const uint64_t BTREE_LEAF_OFFSET   = 0x200000;     // Leaves: 2MB-16MB

// Local Compute Server Buffers (temporary storage for RDMA reads)
const uint64_t LOCAL_BUFFER_BASE       = 0x2000000;  // Base for tree traversal buffers
const uint64_t LOCAL_BUFFER_SPACING    = 0x10000;    // 64 KB spacing between level buffers
const uint64_t LOCAL_LEAF_BUFFER       = 0x3000000;  // Dedicated leaf read buffer
const uint64_t LOCAL_PARENT_BUFFER     = 0x4000000;  // Parent node buffer (for splits)
const uint64_t LOCAL_FINDPARENT_BUFFER = 0x5000000;  // find_parent traversal buffers
const uint64_t LOCAL_VERIFY_BUFFER     = 0x6000000;  // Parent verification buffer

// Helper macros
#define GET_MEMORY_SERVER(addr) (((addr) - MEMORY_BASE_ADDRESS) / MEMORY_SERVER_SIZE)
#define GET_LOCAL_BUFFER(level) (LOCAL_BUFFER_BASE + (level) * LOCAL_BUFFER_SPACING)

using namespace SST;
using namespace SST::MemHierarchy;

ComputeServer::ComputeServer(ComponentId_t id, Params& params) :
    SST::Component(id),
    rng(std::random_device{}() + id),  // Add component ID to ensure different seeds
    uniform_dist(0.0, 1.0),
    last_op_time(0)
{
    // Parse configuration parameters
    node_id = params.find<uint32_t>("node_id", 0);
    num_memory_nodes = params.find<uint32_t>("num_memory_nodes", 4);
    workload_type = params.find<std::string>("workload_type", "ycsb_a");
    ops_per_second = params.find<uint32_t>("operations_per_second", 10000);
    simulation_duration = params.find<SimTime_t>("simulation_duration_us", 1000000) * 1000; // Convert to ns
    zipfian_alpha = params.find<double>("zipfian_alpha", 0.9);
    std::string key_dist = params.find<std::string>("key_distribution", "zipfian"); // New parameter
    read_ratio = params.find<double>("read_ratio", 0.95);
    btree_fanout = params.find<uint32_t>("btree_fanout", 16);
    key_range = params.find<uint64_t>("key_range", 1000000);
    verbose_level = params.find<int>("verbose", 0);

    // Override zipfian_alpha based on distribution type
    if (key_dist == "uniform") {
        zipfian_alpha = 0.0;  // Force uniform distribution
    }
    
    // Configure uniform integer distribution with actual key range    
    // Initialize key frequency tracking (for first 100 keys to show distribution)
    key_frequencies.resize(std::min(key_range, static_cast<uint64_t>(100)), 0);

    // Initialize B+tree state - compute server can connect to multiple memory servers
    tree_height = 1;  // Start with just root (which is also a leaf)
    next_node_id = 0;  // Start node ID counter
    root_address = MEMORY_BASE_ADDRESS;  // Root always at memory server 0's base address

    // Setup debug output with maximum verbosity for address visibility
    dbg.init("", 5, 0, (Output::output_location_t)1);  // Force high verbosity
    out.init("ComputeServer[@p:@l]: ", 1, 0, Output::STDOUT);

    // Initialize statistics
    stat_inserts = registerStatistic<uint64_t>("btree_inserts");
    stat_searches = registerStatistic<uint64_t>("btree_searches");
    stat_deletes = registerStatistic<uint64_t>("btree_deletes");
    stat_rdma_reads = registerStatistic<uint64_t>("rdma_reads");
    stat_rdma_writes = registerStatistic<uint64_t>("rdma_writes");
    stat_total_latency = registerStatistic<uint64_t>("total_latency");
    stat_ops_completed = registerStatistic<uint64_t>("operations_completed");

    // Setup multiple RDMA interfaces (one per memory server)
    auto rdma_handler = new SST::Interfaces::StandardMem::Handler2<ComputeServer,&ComputeServer::handleMemoryEvent>(this);
    
    // Load RDMA interfaces for ALL memory servers (many-to-many connectivity)
    for (int i = 0; i < num_memory_nodes; i++) {
        std::string interface_name = "rdma_nic_" + std::to_string(i);
        auto rdma_interface_i = loadUserSubComponent<SST::Interfaces::StandardMem>(interface_name, SST::ComponentInfo::SHARE_NONE, 
                                                                                   registerTimeBase("1ns"), rdma_handler);
        if (rdma_interface_i) {
            if (i == 0) {
                rdma_interface = rdma_interface_i;  // Store first interface as primary
            } else {
                rdma_interfaces.push_back(rdma_interface_i);
            }
            out.output("  Loaded RDMA interface to Memory Server %d: %s\n", i, interface_name.c_str());
        } else {
            out.fatal(CALL_INFO, -1, "Failed to load RDMA interface %s\n", interface_name.c_str());
        }
    }
    
    out.output("  Many-to-Many RDMA connectivity: %d interfaces loaded\n", (int)rdma_interfaces.size() + 1);
    out.output("  Can connect to ALL %d memory servers\n", num_memory_nodes);

    // Set up clock for operation generation
    std::string clock_freq = std::to_string(ops_per_second) + "Hz";
    clock_handler = new SST::Clock::Handler2<ComputeServer,&ComputeServer::tick>(this);
    registerClock(clock_freq, clock_handler);

    out.output("Compute Server %d initialized\n", node_id);
    out.output("  Workload: %s, Ops/sec: %d, Read ratio: %.2f\n", 
               workload_type.c_str(), ops_per_second, read_ratio);
    out.output("  Key distribution: %s (alpha=%.2f), Key range: %lu\n", 
               (zipfian_alpha <= 0.0) ? "UNIFORM" : "ZIPFIAN", zipfian_alpha, key_range);
    
    // DEBUG: Force output of parameters for debugging
    out.output("DEBUG: key_dist='%s', zipfian_alpha=%.6f, key_range=%lu\n", 
               key_dist.c_str(), zipfian_alpha, key_range);
}

ComputeServer::~ComputeServer() {
    // Cleanup
}

void ComputeServer::init(unsigned int phase) {
    rdma_interface->init(phase);
    
    // Initialize all additional interfaces
    for (auto& interface : rdma_interfaces) {
        interface->init(phase);
    }
    
    if (phase == 0) {
        // Debug initialization parameters
        out.output("DEBUG Node %d: Initializing with alpha=%.1f, key_range=%lu, distribution=%s\n", 
                   node_id, zipfian_alpha, key_range, 
                   (zipfian_alpha <= 0.0) ? "UNIFORM" : "ZIPFIAN");
        
        // Initialize B+tree structure
        initialize_btree();
        
        // Generate initial workload
        generate_workload();
        out.output("Generated %zu operations for workload\n", pending_operations.size());
    }
}

void ComputeServer::setup() {
    rdma_interface->setup();
    
    // Setup all additional interfaces
    for (auto& interface : rdma_interfaces) {
        interface->setup();
    }
}

void ComputeServer::finish() {
    rdma_interface->finish();
    
    // Finish all additional interfaces
    for (auto& interface : rdma_interfaces) {
        interface->finish();
    }
    
    // Output final statistics
    out.output("Compute Server %d completed:\n", node_id);
    out.output("  Total operations: %lu\n", stat_ops_completed->getCollectionCount());
    out.output("  RDMA reads: %lu, RDMA writes: %lu\n", 
               stat_rdma_reads->getCollectionCount(), stat_rdma_writes->getCollectionCount());
    
    // Output key distribution analysis
    out.output("\n📊 Key Distribution Analysis (first 20 keys):\n");
    out.output("  Distribution type: %s (alpha=%.2f)\n", 
               (zipfian_alpha <= 0.0) ? "UNIFORM" : "ZIPFIAN", zipfian_alpha);
    
    uint64_t total_accesses = 0;
    for (size_t i = 0; i < std::min(key_frequencies.size(), static_cast<size_t>(20)); i++) {
        total_accesses += key_frequencies[i];
    }
    
    for (size_t i = 0; i < std::min(key_frequencies.size(), static_cast<size_t>(20)); i++) {
        if (key_frequencies[i] > 0) {
            double percentage = (total_accesses > 0) ? (key_frequencies[i] * 100.0 / total_accesses) : 0.0;
            out.output("  Key %2zu: %4lu accesses (%.1f%%)\n", i, key_frequencies[i], percentage);
        }
    }
}

bool ComputeServer::tick(SST::Cycle_t cycle) {
    SimTime_t current_time = getCurrentSimTime();
    
    // Check if simulation should end
    if (current_time > simulation_duration) {
        return true;  // Stop clock
    }
    
    // Process next operation if available
    if (!pending_operations.empty()) {
        WorkloadOp op = pending_operations.front();
        pending_operations.pop();
        
        dbg.debug(CALL_INFO, 1, 0, "Processing %s operation for key %lu\n",
                 (op.op_type == BTREE_INSERT) ? "INSERT" : 
                 (op.op_type == BTREE_SEARCH) ? "SEARCH" : "DELETE", op.key);
        
        process_btree_operation(op);
    }
    
    return false;  // Continue ticking
}

void ComputeServer::handleMemoryEvent(SST::Interfaces::StandardMem::Request* req) {
    // Handle RDMA response events
    auto req_id = req->getID();
    SimTime_t current_time = getCurrentSimTime();
    
    if (auto read_req = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req)) {
        // Handle read response
        if (outstanding_reads.count(req_id)) {
            SimTime_t latency = current_time - outstanding_reads[req_id];
            stat_total_latency->addData(latency);
            outstanding_reads.erase(req_id);
            dbg.debug(CALL_INFO, 3, 0, "RDMA READ completed: latency=%lu ns\n", latency);
        }
    } else if (auto write_req = dynamic_cast<SST::Interfaces::StandardMem::WriteResp*>(req)) {
        // Handle write response  
        if (outstanding_writes.count(req_id)) {
            SimTime_t latency = current_time - outstanding_writes[req_id];
            stat_total_latency->addData(latency);
            outstanding_writes.erase(req_id);
            dbg.debug(CALL_INFO, 3, 0, "RDMA WRITE completed: latency=%lu ns\n", latency);
        }
    }
    
    delete req;
}

void ComputeServer::generate_workload() {
    out.output("DEBUG Node %d: Starting workload generation...\n", node_id);
    
    // Calculate time interval between operations
    // SST uses nanoseconds as base time unit (SimTime_t is in nanoseconds)
    // 1 second = 1,000,000,000 nanoseconds
    // Example: If ops_per_second = 1000:
    //   op_interval = 1,000,000,000 / 1000 = 1,000,000 ns = 1 ms
    SimTime_t op_interval = 1000000000ULL / ops_per_second;  // Interval in nanoseconds
    SimTime_t current_time = 0;  // Start at 0 nanoseconds
    
    // Generate operations for the simulation duration
    while (current_time < simulation_duration) {
        WorkloadOp op = generate_next_operation();
        op.timestamp = current_time;  // When to execute (in nanoseconds)
        op.node_id = node_id;
        
        pending_operations.push(op);
        current_time += op_interval;  // Add nanoseconds to schedule next operation
    }
    
    out.output("DEBUG Node %d: Generated %zu operations\n", node_id, pending_operations.size());
}

WorkloadOp ComputeServer::generate_next_operation() {
    WorkloadOp op;
    
    // Determine operation type based on read ratio
    double rand_val = uniform_dist(rng);
    if (rand_val < read_ratio) {
        op.op_type = BTREE_SEARCH;
    } else {
        // Split writes between inserts and deletes (90% insert, 10% delete)
        if (uniform_dist(rng) < 0.9) {
            op.op_type = BTREE_INSERT;
        } else {
            op.op_type = BTREE_DELETE;
        }
    }
    
    // Generate key using Zipfian distribution
    op.key = get_zipfian_key();
    op.value = op.key * 1000 + node_id;  // Simple value generation
    
    return op;
}

uint64_t ComputeServer::get_zipfian_key() {
    uint64_t key;
    double rand_val = uniform_dist(rng);
    
    if (zipfian_alpha <= 0.0) {
        // UNIFORM distribution - force a range of keys for testing
        key = static_cast<uint64_t>(rand_val * key_range);
        
        // Use SST output instead of printf for visibility
        out.output("DEBUG UNIFORM: node=%d, rand_val=%.6f, key_range=%lu -> key=%lu (alpha=%.1f)\n", 
                   node_id, rand_val, key_range, key, zipfian_alpha);
    } else {
        // Zipfian distribution using inverse power method
        // Avoid rand_val = 0 to prevent pow(0, negative) = infinity
        if (rand_val == 0.0) rand_val = 1e-10;
        
        key = static_cast<uint64_t>(pow(rand_val, -1.0 / zipfian_alpha)) % key_range;
        
        // Use SST output instead of printf for visibility
        out.output("DEBUG ZIPFIAN: node=%d, rand_val=%.6f -> pow(%.6f, -1/%.1f) = %.2f -> key=%lu\n", 
                   node_id, rand_val, rand_val, zipfian_alpha, pow(rand_val, -1.0 / zipfian_alpha), key);
    }
    
    // Track frequency for first 100 keys to show distribution pattern
    if (key < key_frequencies.size()) {
        key_frequencies[key]++;
    }
    
    return key;
}

void ComputeServer::process_btree_operation(const WorkloadOp& op) {
    switch (op.op_type) {
        case BTREE_INSERT:
            btree_insert(op.key, op.value);
            stat_inserts->addData(1);
            break;
        case BTREE_SEARCH:
            btree_search(op.key);
            stat_searches->addData(1);
            break;
        case BTREE_DELETE:
            btree_delete(op.key);
            stat_deletes->addData(1);
            break;
    }
    stat_ops_completed->addData(1);
}

void ComputeServer::btree_insert(uint64_t key, uint64_t value) {
    dbg.debug(CALL_INFO, 2, 0, "B+Tree INSERT: key=%lu, value=%lu\n", key, value);
    
    out.output("\n🔹 INSERT Operation: key=%lu, value=%lu\n", key, value);
    
    stat_inserts->addData(1);
    
    // Step 1: Traverse tree to find appropriate leaf node
    // traverse_to_leaf() already performs the RDMA read and returns the node data
    BTreeNode leaf_node = traverse_to_leaf(key);
    
    // Step 2: Check if leaf has space and insert key/value pair
    // We insert a KEY-VALUE PAIR into the node, not a new node!
    out.output("   Leaf node has %u/%u keys (address=0x%lx)\n", 
               leaf_node.num_keys, btree_fanout, leaf_node.node_address);
    
    if (leaf_node.num_keys < btree_fanout) {
        // Space available - insert key in sorted order
        uint32_t insert_pos = 0;
        
        // Find insertion position to maintain sorted order
        while (insert_pos < leaf_node.num_keys && leaf_node.keys[insert_pos] < key) {
            insert_pos++;
        }
        
        // Check for duplicate key
        if (insert_pos < leaf_node.num_keys && leaf_node.keys[insert_pos] == key) {
            out.output("   ⚠️  Duplicate key=%lu - updating value %lu → %lu\n",
                       key, leaf_node.values[insert_pos], value);
            leaf_node.values[insert_pos] = value;  // Update existing value
        } else {
            // Shift keys/values to make room
            for (uint32_t i = leaf_node.num_keys; i > insert_pos; i--) {
                leaf_node.keys[i] = leaf_node.keys[i-1];
                leaf_node.values[i] = leaf_node.values[i-1];
            }
            
            // Insert new key-value pair
            leaf_node.keys[insert_pos] = key;
            leaf_node.values[insert_pos] = value;
            leaf_node.num_keys++;
            
            out.output("   ✓ Inserted key=%lu at position %u (now %u keys)\n", 
                       key, insert_pos, leaf_node.num_keys);
        }
        
        // Step 3: Write back modified leaf node via RDMA
        rdma_write(leaf_node.node_address, &leaf_node, sizeof(BTreeNode));
        
        // Step 4: Update cache
        cached_nodes[leaf_node.node_address] = leaf_node;
        
    } else {
        // Leaf is full - NEED TO SPLIT!
        out.output("   ⚠️  Leaf FULL (%u/%u) - performing SPLIT operation\n", 
                   leaf_node.num_keys, btree_fanout);
        
        split_leaf(leaf_node, key, value);
    }
}

void ComputeServer::btree_search(uint64_t key) {
    dbg.debug(CALL_INFO, 2, 0, "B+tree SEARCH: key=%lu\n", key);
    
    out.output("\n🔍 SEARCH Operation: key=%lu\n", key);
    
    // Step 1: Traverse tree from root to leaf (already performs RDMA reads)
    BTreeNode leaf_node = traverse_to_leaf(key);
    
    // Step 2: Search for key in the leaf node (binary search for efficiency)
    bool found = false;
    uint64_t found_value = 0;
    
    for (uint32_t i = 0; i < leaf_node.num_keys; i++) {
        if (leaf_node.keys[i] == key) {
            found = true;
            found_value = leaf_node.values[i];
            out.output("   ✓ FOUND key=%lu at position %u, value=%lu\n", key, i, found_value);
            break;
        } else if (leaf_node.keys[i] > key) {
            // Keys are sorted, so if we pass the key, it's not here
            break;
        }
    }
    
    if (!found) {
        out.output("   ✗ NOT FOUND key=%lu in leaf (has %u keys)\n", key, leaf_node.num_keys);
    }
}

void ComputeServer::btree_delete(uint64_t key) {
    dbg.debug(CALL_INFO, 2, 0, "B+tree DELETE: key=%lu\n", key);
    
    out.output("\n🗑️  DELETE Operation: key=%lu\n", key);
    
    // Step 1: Traverse to find the leaf containing the key
    BTreeNode leaf_node = traverse_to_leaf(key);
    
    // Step 2: Find and remove the key from the leaf
    bool found = false;
    uint32_t delete_pos = 0;
    
    for (uint32_t i = 0; i < leaf_node.num_keys; i++) {
        if (leaf_node.keys[i] == key) {
            found = true;
            delete_pos = i;
            break;
        }
    }
    
    if (found) {
        // Shift keys and values to fill the gap
        for (uint32_t i = delete_pos; i < leaf_node.num_keys - 1; i++) {
            leaf_node.keys[i] = leaf_node.keys[i+1];
            leaf_node.values[i] = leaf_node.values[i+1];
        }
        leaf_node.num_keys--;
        
        out.output("   ✓ Deleted key=%lu from position %u (now %u keys)\n", 
                   key, delete_pos, leaf_node.num_keys);
        
        // Check if underflow (less than fanout/2 keys)
        uint32_t min_keys = (btree_fanout + 1) / 2;
        if (leaf_node.num_keys < min_keys && leaf_node.node_address != root_address) {
            out.output("   ⚠️  UNDERFLOW (%u < %u) - would need merge/redistribute:\n",
                       leaf_node.num_keys, min_keys);
            out.output("       1. Try to borrow from sibling\n");
            out.output("       2. If not possible, merge with sibling\n");
            out.output("       3. May propagate up the tree\n");
        }
        
        // Write back modified leaf
        rdma_write(leaf_node.node_address, &leaf_node, sizeof(BTreeNode));
        
        // Update cache
        cached_nodes[leaf_node.node_address] = leaf_node;
    } else {
        out.output("   ✗ Key=%lu NOT FOUND in leaf (nothing to delete)\n", key);
    }
}

void ComputeServer::rdma_read(uint64_t remote_address, size_t size, uint64_t local_buffer) {
    // Assert to verify function is called
    assert(remote_address != 0 && "RDMA read called with valid address");
    
    dbg.debug(CALL_INFO, 3, 0, "RDMA READ: addr=0x%lx, size=%zu, buffer=0x%lx\n", 
              remote_address, size, local_buffer);
    
    // Always print address information showing many-to-many connectivity
    uint64_t target_memory_server = GET_MEMORY_SERVER(remote_address);
    out.output("🔍 Compute %d → Memory %lu: RDMA READ from address 0x%lx (size=%zu bytes) [Many-to-Many]\n", 
               node_id, target_memory_server, remote_address, size);
    
    // Create RDMA read request
    auto req = new SST::Interfaces::StandardMem::Read(remote_address, size);
    
    // Track outstanding request
    outstanding_reads[req->getID()] = getCurrentSimTime();
    
    // Send to appropriate RDMA interface based on target memory server
    SST::Interfaces::StandardMem* target_interface = get_rdma_interface_for_address(remote_address);
    target_interface->send(req);
    stat_rdma_reads->addData(1);
}

void ComputeServer::rdma_write(uint64_t remote_address, void* data, size_t size) {
    // Assert to verify function is called
    assert(remote_address != 0 && "RDMA write called with valid address");
    assert(data != nullptr && "RDMA write called with valid data pointer");
    
    dbg.debug(CALL_INFO, 3, 0, "RDMA WRITE: addr=0x%lx, size=%zu\n", remote_address, size);
    
    // Always print address information showing many-to-many connectivity
    uint64_t target_memory_server = GET_MEMORY_SERVER(remote_address);
    out.output("🔍 Compute %d → Memory %lu: RDMA WRITE to address 0x%lx (size=%zu bytes) [Many-to-Many]\n", 
               node_id, target_memory_server, remote_address, size);
    
    // Create RDMA write request
    std::vector<uint8_t> write_data(static_cast<uint8_t*>(data), 
                                   static_cast<uint8_t*>(data) + size);
    auto req = new SST::Interfaces::StandardMem::Write(remote_address, size, write_data);
    
    // Track outstanding request
    outstanding_writes[req->getID()] = getCurrentSimTime();
    
    // Send to appropriate RDMA interface based on target memory server
    SST::Interfaces::StandardMem* target_interface = get_rdma_interface_for_address(remote_address);
    target_interface->send(req);
    stat_rdma_writes->addData(1);
}

uint64_t ComputeServer::hash_key_to_memory_server(uint64_t key) {
    // Many-to-Many: Each compute server can access ANY memory server
    // Use consistent hash-based distribution across ALL available memory servers
    // This ensures load balancing across all memory servers
    return (key * 2654435761ULL) % num_memory_nodes;  // Use better hash function for distribution
}

void ComputeServer::initialize_btree() {
    // Calculate optimal tree height based on key range and fanout
    tree_height = calculate_tree_height(key_range);
    
    out.output("🌳 Initializing B+tree structure:\n");
    out.output("   Fanout: %u keys per node\n", btree_fanout);
    out.output("   Tree height: %u levels\n", tree_height);
    out.output("   Key range: %lu keys\n", key_range);
    out.output("   Estimated leaf nodes: %lu\n", (key_range + btree_fanout - 1) / btree_fanout);
    
    // Create root node (initially a leaf)
    BTreeNode root(btree_fanout);
    root.is_leaf = true;
    root.num_keys = 0;
    root.node_address = allocate_node_address(next_node_id++, 0);  // Root at level 0
    
    root_address = root.node_address;
    cached_nodes[root_address] = root;
    
    out.output("   Root address: 0x%lx (Memory Server %lu)\n", 
               root_address, GET_MEMORY_SERVER(root_address));
}

uint64_t ComputeServer::calculate_tree_height(uint64_t num_keys) {
    // Calculate tree height needed to store num_keys
    // Each leaf can hold 'fanout' keys
    // Each internal node can have 'fanout+1' children
    
    if (num_keys == 0) return 1;
    
    uint64_t leaves_needed = (num_keys + btree_fanout - 1) / btree_fanout;
    uint32_t height = 1;  // At least the leaf level
    
    uint64_t nodes_at_level = leaves_needed;
    while (nodes_at_level > 1) {
        nodes_at_level = (nodes_at_level + btree_fanout) / (btree_fanout + 1);
        height++;
    }
    
    return height;
}

uint64_t ComputeServer::allocate_node_address(uint64_t node_id, uint32_t level) {
    // Allocate address for a B+tree node based on:
    // 1. Node ID (unique identifier)
    // 2. Level in tree (0=root, tree_height-1=leaves)
    //
    // Memory Layout per server (16 MB):
    // ┌────────────────────────────────────────┐
    // │ Level 0 (Root):        0x00000 - 0x0FFFF  (64 KB)   │
    // │ Level 1 (Internal):    0x10000 - 0x3FFFF  (192 KB)  │
    // │ Level 2 (Internal):    0x40000 - 0x1FFFFF (1.75 MB) │
    // │ Leaves:                0x200000 - 0xFFFFFF (14 MB)  │
    // └────────────────────────────────────────┘
    
    // Determine which memory server based on node_id for load balancing
    uint64_t memory_server = node_id % num_memory_nodes;
    uint64_t base_address = MEMORY_BASE_ADDRESS + memory_server * MEMORY_SERVER_SIZE;
    
    uint64_t offset;
    if (level == 0) {
        // Root level: First 64 KB
        offset = BTREE_LEVEL0_OFFSET;
    } else if (level < tree_height - 1) {
        // Internal nodes: Allocate in regions based on level
        // Each level gets more space as we go down the tree
        uint64_t level_base = BTREE_LEVEL1_OFFSET * level;  // 64 KB per level
        offset = level_base + (node_id % 10000) * sizeof(BTreeNode);
    } else {
        // Leaf nodes: Start at 2 MB offset, use most of the space
        offset = BTREE_LEAF_OFFSET + (node_id % 100000) * sizeof(BTreeNode);
    }
    
    uint64_t final_address = base_address + offset;
    
    out.output("📍 Allocated Node %lu (Level %u) → Memory Server %lu: Address 0x%lx\n",
               node_id, level, memory_server, final_address);
    
    return final_address;
}

uint64_t ComputeServer::get_child_index_for_key(const BTreeNode& node, uint64_t key) {
    // Find the child pointer index for a given key in an internal node
    // B+tree property: keys[i] is the minimum key in child[i+1]
    
    if (node.num_keys == 0) {
        return 0;  // Empty node, use first child
    }
    
    // Binary search for the right child
    for (uint32_t i = 0; i < node.num_keys; i++) {
        if (key < node.keys[i]) {
            return i;  // Key belongs in child[i]
        }
    }
    
    // Key is >= all keys, belongs in rightmost child
    return node.num_keys;
}

BTreeNode ComputeServer::parse_node_from_buffer(uint64_t address, uint64_t buffer_address) {
    // In a real implementation, this would deserialize the BTreeNode structure
    // from the memory buffer that was filled by the RDMA read operation.
    // 
    // For now, we simulate this by either:
    // 1. Returning cached data if available
    // 2. Creating a placeholder node (in real system, would read from buffer_address)
    
    BTreeNode node(btree_fanout);
    node.node_address = address;
    
    // Check if we have this node cached
    if (cached_nodes.count(address)) {
        node = cached_nodes[address];
        out.output("   � Parsed node from CACHE: addr=0x%lx, keys=%u, leaf=%d\n",
                   address, node.num_keys, node.is_leaf);
    } else {
        // In a real implementation, we would:
        // 1. Cast buffer_address to a BTreeNode*
        // 2. Deserialize the data (handle endianness, alignment, etc.)
        // 3. Populate the node structure
        //
        // Simplified: Create empty node as placeholder
        // TODO: Actually read from buffer_address in a real implementation
        node.num_keys = 0;
        node.is_leaf = true;  // Will be corrected based on tree level
        
        out.output("   📦 Parsed node from RDMA BUFFER: addr=0x%lx (placeholder - empty)\n", address);
    }
    
    return node;
}

BTreeNode ComputeServer::traverse_to_leaf(uint64_t key) {
    // Traverse from root to the appropriate leaf for this key
    // This simulates RDMA reads as we walk down the tree
    // Returns the actual leaf node (not just address) so caller doesn't need to read again
    // ALSO builds parent_map as we traverse for use in split operations
    
    uint64_t current_address = root_address;
    uint64_t parent_address = 0;  // Track parent as we go
    uint32_t current_level = 0;
    
    out.output("🔍 Traversing tree for key %lu (root=0x%lx):\n", key, root_address);
    
    // Walk down the tree until we hit a leaf
    while (current_level < tree_height - 1) {
        // RDMA read current node
        uint64_t local_buffer = GET_LOCAL_BUFFER(current_level);
        rdma_read(current_address, sizeof(BTreeNode), local_buffer);
        
        // Parse the node from the buffer (now actually using the read data!)
        BTreeNode current_node = parse_node_from_buffer(current_address, local_buffer);
        current_node.is_leaf = false;  // Internal node
        
        out.output("   Level %u: Node at 0x%lx has %u keys\n", 
                   current_level, current_address, current_node.num_keys);
        
        // Find which child to follow
        uint64_t child_index = get_child_index_for_key(current_node, key);
        
        // Get child address from node's children array
        uint64_t child_address;
        if (current_node.num_keys > 0 && child_index <= current_node.num_keys) {
            // Use actual child pointer from node
            child_address = current_node.children[child_index];
            out.output("   → Using cached child[%lu] = 0x%lx\n", child_index, child_address);
        } else {
            // Calculate based on structure (fallback for empty nodes)
            uint64_t child_node_id = next_node_id++;
            child_address = allocate_node_address(child_node_id, current_level + 1);
            out.output("   → Allocated new child[%lu] = 0x%lx\n", child_index, child_address);
        }
        
        // Record parent relationship for split operations
        parent_map[child_address] = current_address;
        out.output("   📝 Recorded parent_map[0x%lx] = 0x%lx\n", child_address, current_address);
        
        // Move to child
        parent_address = current_address;
        current_address = child_address;
        current_level++;
    }
    
    // Now read the final leaf node
    rdma_read(current_address, sizeof(BTreeNode), LOCAL_LEAF_BUFFER);
    
    // Parse the leaf node from buffer
    BTreeNode leaf_node = parse_node_from_buffer(current_address, LOCAL_LEAF_BUFFER);
    leaf_node.is_leaf = true;  // Ensure it's marked as leaf
    
    // Record parent of leaf
    if (parent_address != 0) {
        parent_map[current_address] = parent_address;
        out.output("   📝 Recorded parent_map[LEAF 0x%lx] = 0x%lx\n", current_address, parent_address);
    }
    
    out.output("   ✓ Reached leaf at 0x%lx (Level %u) with %u keys\n", 
               current_address, current_level, leaf_node.num_keys);
    
    return leaf_node;  // Return the actual node data, not just address
}

uint64_t ComputeServer::get_lock_offset() {
    // Lock is located at offset 0 within the node
    // First 8 bytes of the node structure is the lock
    return 0;
}

void ComputeServer::split_leaf(BTreeNode& old_leaf, uint64_t new_key, uint64_t new_value) {
    // Split a full leaf node into two nodes
    // This is called when a leaf has fanout keys and we need to insert another
    
    out.output("\n🔀 SPLITTING LEAF NODE:\n");
    out.output("   Old leaf: addr=0x%lx, keys=%u/%u\n", 
               old_leaf.node_address, old_leaf.num_keys, btree_fanout);
    
    // Step 1: Create new leaf node
    uint64_t new_node_id = next_node_id++;
    uint64_t new_leaf_address = allocate_node_address(new_node_id, tree_height - 1);  // Same level as old leaf
    
    BTreeNode new_leaf(btree_fanout);
    new_leaf.node_address = new_leaf_address;
    new_leaf.is_leaf = true;
    new_leaf.num_keys = 0;
    
    // Step 2: Determine split point (middle of the node)
    uint32_t split_point = btree_fanout / 2;
    
    // Step 3: Create temporary array with all keys (old + new)
    std::vector<uint64_t> all_keys(btree_fanout + 1);
    std::vector<uint64_t> all_values(btree_fanout + 1);
    
    // Insert new key in sorted position
    uint32_t insert_pos = 0;
    while (insert_pos < old_leaf.num_keys && old_leaf.keys[insert_pos] < new_key) {
        insert_pos++;
    }
    
    // Copy keys before insert position
    for (uint32_t i = 0; i < insert_pos; i++) {
        all_keys[i] = old_leaf.keys[i];
        all_values[i] = old_leaf.values[i];
    }
    
    // Insert new key
    all_keys[insert_pos] = new_key;
    all_values[insert_pos] = new_value;
    
    // Copy keys after insert position
    for (uint32_t i = insert_pos; i < old_leaf.num_keys; i++) {
        all_keys[i + 1] = old_leaf.keys[i];
        all_values[i + 1] = old_leaf.values[i];
    }
    
    // Step 4: Split keys between old and new leaf
    // Old leaf keeps first half, new leaf gets second half
    old_leaf.num_keys = split_point;
    for (uint32_t i = 0; i < split_point; i++) {
        old_leaf.keys[i] = all_keys[i];
        old_leaf.values[i] = all_values[i];
    }
    
    new_leaf.num_keys = (btree_fanout + 1) - split_point;
    for (uint32_t i = 0; i < new_leaf.num_keys; i++) {
        new_leaf.keys[i] = all_keys[split_point + i];
        new_leaf.values[i] = all_values[split_point + i];
    }
    
    out.output("   Split complete:\n");
    out.output("     Old leaf (0x%lx): %u keys [%lu..%lu]\n", 
               old_leaf.node_address, old_leaf.num_keys,
               old_leaf.keys[0], old_leaf.keys[old_leaf.num_keys - 1]);
    out.output("     New leaf (0x%lx): %u keys [%lu..%lu]\n",
               new_leaf.node_address, new_leaf.num_keys,
               new_leaf.keys[0], new_leaf.keys[new_leaf.num_keys - 1]);
    
    // Step 5: Write both nodes back to memory
    rdma_write(old_leaf.node_address, &old_leaf, sizeof(BTreeNode));
    rdma_write(new_leaf.node_address, &new_leaf, sizeof(BTreeNode));
    
    // Step 6: Update cache
    cached_nodes[old_leaf.node_address] = old_leaf;
    cached_nodes[new_leaf.node_address] = new_leaf;
    
    // Step 7: Update parent to add new child pointer
    uint64_t separator_key = new_leaf.keys[0];  // First key of new leaf
    out.output("   Updating parent with separator key=%lu → new leaf 0x%lx\n",
               separator_key, new_leaf.node_address);
    
    insert_into_parent(old_leaf.node_address, separator_key, new_leaf.node_address, tree_height - 1);
}

void ComputeServer::insert_into_parent(uint64_t old_node_addr, uint64_t separator_key, uint64_t new_node_addr, uint32_t level) {
    // Insert separator key and new child pointer into parent node
    // This is called after splitting a node (leaf or internal)
    
    out.output("\n   📤 Inserting into parent: sep_key=%lu, new_child=0x%lx, level=%u\n",
               separator_key, new_node_addr, level);
    
    // Special case: If old node is root, we need to create a new root
    if (old_node_addr == root_address) {
        out.output("      Splitting ROOT - creating new root (tree height %u → %u)\n",
                   tree_height, tree_height + 1);
        
        // Create new root
        uint64_t new_root_id = next_node_id++;
        uint64_t new_root_addr = allocate_node_address(new_root_id, 0);  // New root at level 0
        
        BTreeNode new_root(btree_fanout);
        new_root.node_address = new_root_addr;
        new_root.is_leaf = false;  // Root is now internal
        new_root.num_keys = 1;
        new_root.keys[0] = separator_key;
        new_root.children[0] = old_node_addr;  // Old root becomes left child
        new_root.children[1] = new_node_addr;  // New node becomes right child
        
        // Write new root
        rdma_write(new_root_addr, &new_root, sizeof(BTreeNode));
        cached_nodes[new_root_addr] = new_root;
        
        // Update tree metadata
        root_address = new_root_addr;
        tree_height++;
        
        out.output("      ✓ New root created at 0x%lx, tree height now %u\n",
                   root_address, tree_height);
        return;
    }
    
    // Find parent node
    uint64_t parent_addr = find_parent_address(old_node_addr, level);
    
    // Read parent node
    rdma_read(parent_addr, sizeof(BTreeNode), LOCAL_PARENT_BUFFER);
    BTreeNode parent = parse_node_from_buffer(parent_addr, LOCAL_PARENT_BUFFER);
    parent.is_leaf = false;  // Parents are always internal nodes
    
    out.output("      Parent at 0x%lx has %u/%u keys\n",
               parent_addr, parent.num_keys, btree_fanout);
    
    // Check if parent has space
    if (parent.num_keys < btree_fanout) {
        // Parent has space - insert separator key and new child pointer
        
        // Find insertion position
        uint32_t insert_pos = 0;
        while (insert_pos < parent.num_keys && parent.keys[insert_pos] < separator_key) {
            insert_pos++;
        }
        
        // Shift keys and children to make room
        for (uint32_t i = parent.num_keys; i > insert_pos; i--) {
            parent.keys[i] = parent.keys[i-1];
            parent.children[i+1] = parent.children[i];
        }
        
        // Insert new key and child
        parent.keys[insert_pos] = separator_key;
        parent.children[insert_pos + 1] = new_node_addr;
        parent.num_keys++;
        
        out.output("      ✓ Inserted into parent at position %u (now %u keys)\n",
                   insert_pos, parent.num_keys);
        
        // Write back parent
        rdma_write(parent_addr, &parent, sizeof(BTreeNode));
        cached_nodes[parent_addr] = parent;
        
    } else {
        // Parent is full - need to split parent recursively
        out.output("      ⚠️  Parent FULL - splitting parent recursively\n");
        split_internal(parent, separator_key, new_node_addr, level - 1);
    }
}

void ComputeServer::split_internal(BTreeNode& old_internal, uint64_t new_key, uint64_t new_child_addr, uint32_t level) {
    // Split an internal (non-leaf) node
    // Similar to split_leaf but handles children pointers
    
    out.output("\n   🔀 SPLITTING INTERNAL NODE at level %u:\n", level);
    out.output("      Old internal: addr=0x%lx, keys=%u/%u\n",
               old_internal.node_address, old_internal.num_keys, btree_fanout);
    
    // Create new internal node
    uint64_t new_node_id = next_node_id++;
    uint64_t new_internal_addr = allocate_node_address(new_node_id, level);
    
    BTreeNode new_internal(btree_fanout);
    new_internal.node_address = new_internal_addr;
    new_internal.is_leaf = false;
    new_internal.num_keys = 0;
    
    // Determine split point
    uint32_t split_point = btree_fanout / 2;
    
    // Create temporary arrays with all keys and children (old + new)
    std::vector<uint64_t> all_keys(btree_fanout + 1);
    std::vector<uint64_t> all_children(btree_fanout + 2);
    
    // Find insertion position for new key
    uint32_t insert_pos = 0;
    while (insert_pos < old_internal.num_keys && old_internal.keys[insert_pos] < new_key) {
        insert_pos++;
    }
    
    // Copy keys and children before insert position
    for (uint32_t i = 0; i < insert_pos; i++) {
        all_keys[i] = old_internal.keys[i];
        all_children[i] = old_internal.children[i];
    }
    all_children[insert_pos] = old_internal.children[insert_pos];
    
    // Insert new key and child
    all_keys[insert_pos] = new_key;
    all_children[insert_pos + 1] = new_child_addr;
    
    // Copy keys and children after insert position
    for (uint32_t i = insert_pos; i < old_internal.num_keys; i++) {
        all_keys[i + 1] = old_internal.keys[i];
        all_children[i + 2] = old_internal.children[i + 1];
    }
    
    // Split: old node keeps first half, new node gets second half
    // Middle key gets promoted to parent
    uint64_t promoted_key = all_keys[split_point];
    
    old_internal.num_keys = split_point;
    for (uint32_t i = 0; i < split_point; i++) {
        old_internal.keys[i] = all_keys[i];
        old_internal.children[i] = all_children[i];
    }
    old_internal.children[split_point] = all_children[split_point];
    
    new_internal.num_keys = btree_fanout - split_point;
    for (uint32_t i = 0; i < new_internal.num_keys; i++) {
        new_internal.keys[i] = all_keys[split_point + 1 + i];
        new_internal.children[i] = all_children[split_point + 1 + i];
    }
    new_internal.children[new_internal.num_keys] = all_children[btree_fanout + 1];
    
    out.output("      Split complete (promoted key=%lu):\n", promoted_key);
    out.output("        Old internal (0x%lx): %u keys\n",
               old_internal.node_address, old_internal.num_keys);
    out.output("        New internal (0x%lx): %u keys\n",
               new_internal.node_address, new_internal.num_keys);
    
    // Write both nodes back
    rdma_write(old_internal.node_address, &old_internal, sizeof(BTreeNode));
    rdma_write(new_internal.node_address, &new_internal, sizeof(BTreeNode));
    
    cached_nodes[old_internal.node_address] = old_internal;
    cached_nodes[new_internal.node_address] = new_internal;
    
    // Recursively insert promoted key into parent
    insert_into_parent(old_internal.node_address, promoted_key, new_internal.node_address, level);
}

uint64_t ComputeServer::find_parent_address(uint64_t child_address, uint32_t child_level) {
    // Find the parent of a given node using the parent_map built during traversal
    
    out.output("      🔍 Finding parent of child=0x%lx at level %u\n", child_address, child_level);
    
    // Sanity check: cannot find parent of root
    if (child_level == 0 || child_address == root_address) {
        out.fatal(CALL_INFO, -1, "Cannot find parent of root node\n");
    }
    
    // Check parent_map (built during traverse_to_leaf)
    if (parent_map.count(child_address)) {
        uint64_t parent_addr = parent_map[child_address];
        out.output("      ✓ Found parent in parent_map: 0x%lx\n", parent_addr);
        return parent_addr;
    }
    
    // If not in map, parent level is child_level - 1
    uint32_t parent_level = child_level - 1;
    
    // If parent is root level, return root
    if (parent_level == 0) {
        out.output("      ✓ Parent is root at 0x%lx\n", root_address);
        return root_address;
    }
    
    // Fallback: traverse tree to find parent (slower but correct)
    // This happens when we split a node that wasn't in the recent traversal path
    out.output("      ⚠️  Parent not in map, traversing tree to find it...\n");
    
    uint64_t current_address = root_address;
    uint32_t current_level = 0;
    
    // Traverse down to parent level
    while (current_level < parent_level) {
        uint64_t buffer = LOCAL_FINDPARENT_BUFFER + current_level * LOCAL_BUFFER_SPACING;
        rdma_read(current_address, sizeof(BTreeNode), buffer);
        BTreeNode current_node = parse_node_from_buffer(current_address, buffer);
        current_node.is_leaf = false;
        
        // At parent level - 1, check which child contains our target
        if (current_level == parent_level - 1) {
            // Check all children to find which one has child_address
            for (uint32_t i = 0; i <= current_node.num_keys; i++) {
                uint64_t test_child = current_node.children[i];
                
                // Read this potential parent
                rdma_read(test_child, sizeof(BTreeNode), LOCAL_VERIFY_BUFFER);
                BTreeNode test_node = parse_node_from_buffer(test_child, LOCAL_VERIFY_BUFFER);
                
                // Check if it contains child_address as a child
                for (uint32_t j = 0; j <= test_node.num_keys; j++) {
                    if (test_node.children[j] == child_address) {
                        out.output("      ✓ Found parent via traversal: 0x%lx\n", test_child);
                        return test_child;
                    }
                }
            }
        }
        
        // Move to first child (simplified - should search based on keys)
        current_address = current_node.children[0];
        current_level++;
    }
    
    out.output("      ✓ Reached parent level, parent at: 0x%lx\n", current_address);
    return current_address;
}

SST::Interfaces::StandardMem* ComputeServer::get_rdma_interface_for_address(uint64_t address) {
    // Many-to-Many: Determine which memory server this address belongs to
    uint64_t memory_server_id = GET_MEMORY_SERVER(address);
    
    // Clamp to valid range - this compute server can connect to ANY memory server
    if (memory_server_id >= num_memory_nodes) {
        memory_server_id = 0;  // Default to primary interface
    }
    
    // Debug output for interface selection
    out.output("🔄 Compute %d: Address 0x%lx → Memory Server %lu, selecting interface...\n", 
               node_id, address, memory_server_id);
    
    // Return appropriate interface for many-to-many connectivity
    if (memory_server_id == 0) {
        out.output("   → Using PRIMARY interface for Memory Server 0\n");
        return rdma_interface;  // Primary interface for memory server 0
    } else {
        // Use additional interfaces for other memory servers
        size_t interface_index = memory_server_id - 1;
        if (interface_index < rdma_interfaces.size()) {
            out.output("   → Using ADDITIONAL interface %zu for Memory Server %lu\n", interface_index, memory_server_id);
            return rdma_interfaces[interface_index];
        } else {
            // Fallback to primary interface if additional interface not available
            out.output("WARNING: No interface for memory server %lu, using primary [Many-to-Many fallback]\n", memory_server_id);
            return rdma_interface;
        }
    }
}