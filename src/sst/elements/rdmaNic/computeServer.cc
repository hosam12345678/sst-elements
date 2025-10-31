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
// Implements B+tree operations using remote memory access
//
// ═══════════════════════════════════════════════════════════════════════════
// MEMORY LAYOUT ARCHITECTURE
// ═══════════════════════════════════════════════════════════════════════════
//
// This system implements a disaggregated memory architecture where:
// - COMPUTE SERVERS: Run application logic, issue memory operations
// - MEMORY SERVERS: Store data, respond to read/write requests
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
// │                    (Temporary Remote Read Buffers)                      │
// └─────────────────────────────────────────────────────────────────────────┘
//
// Compute servers use local memory to temporarily store data fetched remotely:
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
// 1. REMOTE READ Operation:
//    compute_server.remote_read(remote_addr=0x10200000, size=512, local_buf=0x2000000)
//    ↓
//    Extracts target memory server: (0x10200000 - 0x10000000) / 0x1000000 = 0
//    ↓
//    Sends read request to Memory Server 0 via interface[0]
//    ↓
//    Memory Server 0 responds with data from address 0x10200000
//    ↓
//    Data arrives at compute server's local buffer 0x2000000
//
// 2. REMOTE WRITE Operation:
//    compute_server.remote_write(remote_addr=0x12300000, size=512, local_buf=0x3000000)
//    ↓
//    Extracts target memory server: (0x12300000 - 0x10000000) / 0x1000000 = 2
//    ↓
//    Sends write request to Memory Server 2 via interface[2]
//    ↓
//    Memory Server 2 stores data at address 0x12300000
//
// 3. Tree Traversal Example (search for key=12345):
//    Step 1: Read root from Server 0
//      remote_read(0x10000000, sizeof(node), 0x2000000)  // Level 0 buffer
//    Step 2: Read internal node from Server 1
//      remote_read(0x11010000, sizeof(node), 0x2010000)  // Level 1 buffer
//    Step 3: Read leaf from Server 3
//      remote_read(0x13250000, sizeof(node), 0x3000000)  // Leaf buffer
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

// Local Compute Server Buffers (temporary storage for remote reads)
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

    // Initialize B+tree state
    tree_height = 1;  // Start with just root (which is also a leaf)
    next_node_id = 0;  // Start node ID counter
    root_address = MEMORY_BASE_ADDRESS;  // Root always at memory server 0's base address

    // Setup debug output with maximum verbosity for address visibility
    dbg.init("", 5, 0, (Output::output_location_t)1);  // Force high verbosity
    out.init("ComputeServer[@p:@l]: ", 1, 0, Output::STDOUT);

    // Initialize statistics
    stat_inserts = registerStatistic<uint64_t>("btree_inserts");
    stat_searches = registerStatistic<uint64_t>("btree_searches");
    stat_network_reads = registerStatistic<uint64_t>("network_reads");
    stat_network_writes = registerStatistic<uint64_t>("network_writes");
    stat_total_latency = registerStatistic<uint64_t>("total_latency");
    stat_ops_completed = registerStatistic<uint64_t>("operations_completed");

    // Setup multiple network interfaces (one per memory server)
    auto mem_handler = new SST::Interfaces::StandardMem::Handler2<ComputeServer,&ComputeServer::handleMemoryEvent>(this);
    
    // Load network interfaces for ALL memory servers (many-to-many connectivity)
    for (int i = 0; i < num_memory_nodes; i++) {
        std::string interface_name = "mem_interface_" + std::to_string(i);
        auto interface_i = loadUserSubComponent<SST::Interfaces::StandardMem>(interface_name, SST::ComponentInfo::SHARE_NONE, 
                                                                                   registerTimeBase("1ns"), mem_handler);
        if (interface_i) {
            if (i == 0) {
                memory_interface = interface_i;  // Store first interface as primary
            } else {
                memory_interfaces.push_back(interface_i);
            }
            out.output("  Loaded network interface to Memory Server %d: %s\n", i, interface_name.c_str());
        } else {
            out.fatal(CALL_INFO, -1, "Failed to load network interface %s\n", interface_name.c_str());
        }
    }
    
    out.output("  Many-to-Many connectivity: %d interfaces loaded\n", (int)memory_interfaces.size() + 1);
    out.output("  Can connect to ALL %d memory servers\n", num_memory_nodes);

    // Set up clock at high frequency (1MHz) to avoid time faults
    // We'll process operations based on their scheduled timestamps, not clock ticks
    std::string clock_freq = "1MHz";  // High enough to avoid time ordering issues
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
    memory_interface->init(phase);
    
    // Initialize all additional interfaces
    for (auto& interface : memory_interfaces) {
        interface->init(phase);
    }
    
    if (phase == 0) {
        // Debug initialization parameters
        out.output("DEBUG Node %d: Initializing with alpha=%.1f, key_range=%lu, distribution=%s\n", 
                   node_id, zipfian_alpha, key_range, 
                   (zipfian_alpha <= 0.0) ? "UNIFORM" : "ZIPFIAN");
        
        // Don't initialize B+tree here - wait for setup() after address exchange completes
        
        // Generate initial workload
        generate_workload();
        out.output("Generated %zu operations for workload\n", pending_operations.size());
    }
}

void ComputeServer::setup() {
    memory_interface->setup();
    
    // Setup all additional interfaces
    for (auto& interface : memory_interfaces) {
        interface->setup();
    }
    
    // NOW initialize B+tree after init() phases complete and address routing is established
    initialize_btree();
}

void ComputeServer::finish() {
    memory_interface->finish();
    
    // Finish all additional interfaces
    for (auto& interface : memory_interfaces) {
        interface->finish();
    }
    
    // Output final statistics
    out.output("Compute Server %d completed:\n", node_id);
    out.output("  Total operations: %lu\n", stat_ops_completed->getCollectionCount());
    out.output("  Network reads: %lu, Network writes: %lu\n", 
               stat_network_reads->getCollectionCount(), stat_network_writes->getCollectionCount());
    
    // Output key distribution analysis
    out.output("\n📊 Key Distribution Analysis:\n");
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
    
    // Process operations whose scheduled time has arrived
    while (!pending_operations.empty()) {
        WorkloadOp& op = pending_operations.front();
        
        // Check if it's time to process this operation
        if (op.timestamp > current_time) {
            // Not yet time for this operation
            break;
        }
        
        // Time to process this operation
        dbg.debug(CALL_INFO, 1, 0, "Processing %s operation for key %lu at time %lu\n",
                 (op.op_type == BTREE_INSERT) ? "INSERT" : "SEARCH", op.key, current_time);
        
        process_btree_operation(op);
        pending_operations.pop();
    }
    
    return false;  // Continue ticking
}

void ComputeServer::handleMemoryEvent(SST::Interfaces::StandardMem::Request* req) {
    // Handle network memory response events
    auto req_id = req->getID();
    SimTime_t current_time = getCurrentSimTime();
    
    if (auto read_resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req)) {
        // Handle read response with async state machine
        dbg.debug(CALL_INFO, 3, 0, "Network READ response received, req_id=%lu\n", req_id);
        handle_read_response(req_id, read_resp->data);
        
    } else if (auto write_resp = dynamic_cast<SST::Interfaces::StandardMem::WriteResp*>(req)) {
        // Handle write response
        dbg.debug(CALL_INFO, 3, 0, "Network WRITE response received, req_id=%lu\n", req_id);
        handle_write_response(req_id);
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
        // All writes are inserts
        op.op_type = BTREE_INSERT;
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
            btree_insert_async(op.key, op.value);
            break;
        case BTREE_SEARCH:
            btree_search_async(op.key);
            break;
    }
    // Note: stat_ops_completed will be updated when operation completes asynchronously
}

// ═══════════════════════════════════════════════════════════════════════════
// ASYNC B+TREE OPERATIONS - Entry points that start async state machines
// ═══════════════════════════════════════════════════════════════════════════

void ComputeServer::btree_insert_async(uint64_t key, uint64_t value) {
    dbg.debug(CALL_INFO, 2, 0, "B+Tree INSERT (async): key=%lu, value=%lu\n", key, value);
    out.output("\n🔹 INSERT Operation (async): key=%lu, value=%lu\n", key, value);
    
    // Create read request for root node to start traversal
    auto req = new SST::Interfaces::StandardMem::Read(root_address, get_serialized_node_size());
    auto req_id = req->getID();
    
    // Track this async operation
    pending_ops[req_id] = AsyncOperation();
    pending_ops[req_id].type = AsyncOperation::INSERT;
    pending_ops[req_id].key = key;
    pending_ops[req_id].value = value;
    pending_ops[req_id].current_level = 0;
    pending_ops[req_id].current_address = root_address;
    pending_ops[req_id].start_time = getCurrentSimTime();
    
    // Send read request
    SST::Interfaces::StandardMem* target_interface = get_interface_for_address(root_address);
    target_interface->send(req);
    stat_network_reads->addData(1);
    
    out.output("   Started async traversal from root=0x%lx\n", root_address);
}

void ComputeServer::btree_search_async(uint64_t key) {
    dbg.debug(CALL_INFO, 2, 0, "B+tree SEARCH (async): key=%lu\n", key);
    out.output("\n🔍 SEARCH Operation (async): key=%lu\n", key);
    
    // Create read request for root node
    auto req = new SST::Interfaces::StandardMem::Read(root_address, get_serialized_node_size());
    auto req_id = req->getID();
    
    // Track this async operation
    pending_ops[req_id] = AsyncOperation();
    pending_ops[req_id].type = AsyncOperation::SEARCH;
    pending_ops[req_id].key = key;
    pending_ops[req_id].current_level = 0;
    pending_ops[req_id].current_address = root_address;
    pending_ops[req_id].start_time = getCurrentSimTime();
    
    // Send read request
    SST::Interfaces::StandardMem* target_interface = get_interface_for_address(root_address);
    target_interface->send(req);
    stat_network_reads->addData(1);
    
    out.output("   Started async traversal from root=0x%lx\n", root_address);
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
    
    // Write root node to memory (NOT cached locally)
    write_node_back(root);
    
    out.output("   Root address: 0x%lx (Memory Server %lu)\n", 
               root_address, GET_MEMORY_SERVER(root_address));
    out.output("   ✓ Root node written to remote memory\n");
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
        // Root level: First 64 KB (only 1 node ever at level 0)
        offset = BTREE_LEVEL0_OFFSET;
    } else if (level < tree_height - 1) {
        // Internal nodes: Allocate in regions based on level
        // Each level gets more space as we go down the tree
        uint64_t level_base = BTREE_LEVEL1_OFFSET * level;  // 64 KB per level
        offset = level_base + (node_id % 10000) * get_serialized_node_size();
    } else {
        // Leaf nodes: Start at 2 MB offset, use most of the space
        offset = BTREE_LEAF_OFFSET + (node_id % 100000) * get_serialized_node_size();
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



SST::Interfaces::StandardMem* ComputeServer::get_interface_for_address(uint64_t address) {
    // Many-to-Many: Determine which memory server this address belongs to
    uint64_t memory_server_id = GET_MEMORY_SERVER(address);
    
    // Clamp to valid range - this compute server can connect to ANY memory server
    if (memory_server_id >= num_memory_nodes) {
        memory_server_id = 0;  // Default to primary interface
    }
    
    // Debug output for interface selection
    dbg.debug(CALL_INFO, 4, 0, "Address 0x%lx → Memory Server %lu\n", address, memory_server_id);
    
    // Return appropriate interface for many-to-many connectivity
    if (memory_server_id == 0) {
        return memory_interface;  // Primary interface for memory server 0
    } else {
        // Use additional interfaces for other memory servers
        size_t interface_index = memory_server_id - 1;
        if (interface_index < memory_interfaces.size()) {
            return memory_interfaces[interface_index];
        } else {
            // Fallback to primary interface if additional interface not available
            dbg.debug(CALL_INFO, 2, 0, "WARNING: No interface for memory server %lu, using primary\n", memory_server_id);
            return memory_interface;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// ASYNC RESPONSE HANDLERS - State machine continuation
// ═══════════════════════════════════════════════════════════════════════════

void ComputeServer::handle_read_response(SST::Interfaces::StandardMem::Request::id_t req_id,
                                         const std::vector<uint8_t>& data) {
    // Check if this is one of our tracked async operations
    if (!pending_ops.count(req_id)) {
        dbg.debug(CALL_INFO, 2, 0, "WARNING: Received read response for unknown request\n");
        return;
    }
    
    auto& op = pending_ops[req_id];
    
    // Special case: READ_PARENT phase of split operation
    if (op.split_phase == AsyncOperation::READ_PARENT) {
        BTreeNode parent = deserialize_node(data);
        
        // Check if we're still traversing to find the parent (when parent_address was 0)
        if (op.parent_address == 0 && !parent.is_leaf) {
            // Still traversing internal nodes to find the parent of the split node
            // Use the separator key to find which child to follow
            uint64_t child_idx = 0;
            while (child_idx < parent.num_keys && parent.keys[child_idx] < op.separator_key) {
                child_idx++;
            }
            
            uint64_t child_addr = parent.children[child_idx];
            
            // Check if this child is one of our split nodes
            if (child_addr == op.old_node.node_address || child_addr == op.new_node.node_address) {
                // Found the parent!
                out.output("   ✓ Phase 3: Found parent at 0x%lx during traversal\n", parent.node_address);
                op.parent_address = parent.node_address;
                // Continue with inserting separator key (fall through to insertion logic below)
            } else {
                // Continue traversing down
                out.output("   → Traversing to child[%lu] = 0x%lx to find parent\n", child_idx, child_addr);
                
                op.current_level++;
                op.current_address = child_addr;
                
                auto next_req = new SST::Interfaces::StandardMem::Read(child_addr, get_serialized_node_size());
                auto next_req_id = next_req->getID();
                pending_ops[next_req_id] = op;
                
                SST::Interfaces::StandardMem* target_interface = get_interface_for_address(child_addr);
                target_interface->send(next_req);
                stat_network_reads->addData(1);
                
                pending_ops.erase(req_id);
                return;
            }
        } else {
            out.output("   ✓ Phase 3: Parent node read complete\n");
        }
        
        // Check if parent has space for separator key
        if (parent.num_keys < btree_fanout) {
            out.output("   Parent has space (%u/%u) - inserting separator key=%lu\n",
                       parent.num_keys, btree_fanout, op.separator_key);
            
            // Find insertion position
            uint32_t insert_pos = 0;
            while (insert_pos < parent.num_keys && parent.keys[insert_pos] < op.separator_key) {
                insert_pos++;
            }
            
            // Shift keys and children
            for (uint32_t i = parent.num_keys; i > insert_pos; i--) {
                parent.keys[i] = parent.keys[i-1];
                parent.children[i+1] = parent.children[i];
            }
            
            // Insert separator key and new child
            parent.keys[insert_pos] = op.separator_key;
            parent.children[insert_pos + 1] = op.new_node.node_address;
            parent.num_keys++;
            
            out.output("   ✓ Inserted separator at position %u (now %u keys)\n",
                       insert_pos, parent.num_keys);
            
            // Write parent back
            op.split_phase = AsyncOperation::UPDATE_PARENT_NODE;
            
            auto req = new SST::Interfaces::StandardMem::Write(
                parent.node_address, get_serialized_node_size(), serialize_node(parent));
            auto req_id_write = req->getID();
            pending_ops[req_id_write] = op;
            
            SST::Interfaces::StandardMem* target_interface = get_interface_for_address(parent.node_address);
            target_interface->send(req);
            stat_network_writes->addData(1);
            
            pending_ops.erase(req_id);
            
        } else {
            out.output("   ⚠️  Parent FULL (%u/%u) - need to split parent recursively\n",
                       parent.num_keys, btree_fanout);
            
            // Parent is full - split it recursively
            split_internal_async(op, parent, op.separator_key, op.new_node.node_address);
            pending_ops.erase(req_id);
        }
        
        return;
    }
    
    // Regular traversal read
    BTreeNode node = deserialize_node(data);
    op.path.push_back(node);  // Save for potential splits
    
    out.output("   Level %u: Read node at 0x%lx, keys=%u, is_leaf=%d\n",
               op.current_level, op.current_address, node.num_keys, node.is_leaf);
    
    // Check if we've reached a leaf node
    if (node.is_leaf || op.current_level >= tree_height - 1) {
        // Reached leaf - perform the actual operation
        out.output("   ✓ Reached leaf at 0x%lx (Level %u) with %u keys\n",
                   op.current_address, op.current_level, node.num_keys);
        handle_leaf_operation(op, node);
        
        // Check if operation returned early (e.g., split started)
        if (pending_ops.count(req_id)) {
            // Operation complete - record statistics (only if not splitting)
            SimTime_t latency = getCurrentSimTime() - op.start_time;
            stat_total_latency->addData(latency);
            stat_ops_completed->addData(1);
            
            // Clean up
            pending_ops.erase(req_id);
        }
        
    } else {
        // Internal node - continue traversal
        uint64_t child_idx = get_child_index_for_key(node, op.key);
        uint64_t child_addr = node.children[child_idx];
        
        out.output("   → Continue to child[%lu] = 0x%lx\n", child_idx, child_addr);
        
        // Record parent relationship for potential splits
        parent_map[child_addr] = op.current_address;
        
        // Create next read request
        auto next_req = new SST::Interfaces::StandardMem::Read(child_addr, get_serialized_node_size());
        auto next_req_id = next_req->getID();
        
        // Transfer state to new request
        pending_ops[next_req_id] = op;
        pending_ops[next_req_id].current_level++;
        pending_ops[next_req_id].current_address = child_addr;
        
        // Send request
        SST::Interfaces::StandardMem* target_interface = get_interface_for_address(child_addr);
        target_interface->send(next_req);
        stat_network_reads->addData(1);
        
        // Clean up old request
        pending_ops.erase(req_id);
    }
}

void ComputeServer::handle_write_response(SST::Interfaces::StandardMem::Request::id_t req_id) {
    // Check if this write is part of a split operation
    if (pending_ops.count(req_id)) {
        auto& op = pending_ops[req_id];
        
        if (op.type == AsyncOperation::SPLIT_LEAF || op.type == AsyncOperation::SPLIT_INTERNAL) {
            // This is a split operation write - continue the split state machine
            handle_split_response(op);
            pending_ops.erase(req_id);
        } else {
            // Regular write completion
            dbg.debug(CALL_INFO, 3, 0, "Write completed for req_id=%lu\n", req_id);
        }
    } else {
        // Write not tracked (might be from regular operations)
        dbg.debug(CALL_INFO, 3, 0, "Write completed for req_id=%lu (not tracked)\n", req_id);
    }
}

void ComputeServer::handle_leaf_operation(AsyncOperation& op, BTreeNode& leaf) {
    switch (op.type) {
        case AsyncOperation::INSERT: {
            out.output("   Executing INSERT in leaf: key=%lu, value=%lu\n", op.key, op.value);
            stat_inserts->addData(1);
            
            if (leaf.num_keys < btree_fanout) {
                // Space available - insert key
                uint32_t insert_pos = 0;
                while (insert_pos < leaf.num_keys && leaf.keys[insert_pos] < op.key) {
                    insert_pos++;
                }
                
                // Check for duplicate
                if (insert_pos < leaf.num_keys && leaf.keys[insert_pos] == op.key) {
                    out.output("   ⚠️  Duplicate key=%lu - updating value\n", op.key);
                    leaf.values[insert_pos] = op.value;
                } else {
                    // Shift and insert
                    for (uint32_t i = leaf.num_keys; i > insert_pos; i--) {
                        leaf.keys[i] = leaf.keys[i-1];
                        leaf.values[i] = leaf.values[i-1];
                    }
                    leaf.keys[insert_pos] = op.key;
                    leaf.values[insert_pos] = op.value;
                    leaf.num_keys++;
                    out.output("   ✓ Inserted key=%lu at position %u (now %u keys)\n",
                               op.key, insert_pos, leaf.num_keys);
                }
                
                // Write back modified leaf
                write_node_back(leaf);
            } else {
                // Leaf is full - need to split (async)
                out.output("   ⚠️  Leaf FULL (%u/%u) - initiating ASYNC SPLIT\n", 
                           leaf.num_keys, btree_fanout);
                split_leaf_async(op, leaf, op.key, op.value);
                return;  // Don't complete operation yet, split will continue
            }
            break;
        }
            
        case AsyncOperation::SEARCH: {
            out.output("   Executing SEARCH in leaf: key=%lu\n", op.key);
            stat_searches->addData(1);
            
            // Search for key
            bool found = false;
            for (uint32_t i = 0; i < leaf.num_keys; i++) {
                if (leaf.keys[i] == op.key) {
                    out.output("   ✓ FOUND key=%lu at position %u, value=%lu\n",
                               op.key, i, leaf.values[i]);
                    found = true;
                    break;
                } else if (leaf.keys[i] > op.key) {
                    break;
                }
            }
            if (!found) {
                out.output("   ✗ NOT FOUND key=%lu\n", op.key);
            }
            break;
        }
            
        default:
            break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// ASYNC SPLIT OPERATIONS
// ═══════════════════════════════════════════════════════════════════════════

void ComputeServer::split_leaf_async(AsyncOperation& op, BTreeNode& old_leaf, uint64_t new_key, uint64_t new_value) {
    out.output("\n🔀 ASYNC LEAF SPLIT: old_leaf=0x%lx, keys=%u/%u\n",
               old_leaf.node_address, old_leaf.num_keys, btree_fanout);
    
    // Step 1: Create new leaf node
    // If splitting root, leaves will be at the NEW tree_height after split
    uint32_t leaf_level = op.is_root_split ? tree_height : (tree_height - 1);
    uint64_t new_node_id = next_node_id++;
    uint64_t new_leaf_address = allocate_node_address(new_node_id, leaf_level);
    
    BTreeNode new_leaf(btree_fanout);
    new_leaf.node_address = new_leaf_address;
    new_leaf.is_leaf = true;
    new_leaf.num_keys = 0;
    
    // Step 2: Determine split point
    uint32_t split_point = btree_fanout / 2;
    
    // Step 3: Create temporary array with all keys (old + new)
    std::vector<uint64_t> all_keys(btree_fanout + 1);
    std::vector<uint64_t> all_values(btree_fanout + 1);
    
    // Find insertion position for new key
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
    
    // Step 5: Save split state in operation
    op.type = AsyncOperation::SPLIT_LEAF;
    op.split_phase = AsyncOperation::WRITE_OLD_NODE;
    op.separator_key = new_leaf.keys[0];  // First key of new leaf
    
    // Check if splitting root
    if (old_leaf.node_address == root_address) {
        op.is_root_split = true;
        out.output("   ⚠️  Splitting ROOT node - will create new root\n");
        
        // When splitting root, allocate NEW address for old leaf (root address will be reused for new root)
        uint64_t old_leaf_new_id = next_node_id++;
        uint64_t old_leaf_new_address = allocate_node_address(old_leaf_new_id, leaf_level);
        old_leaf.node_address = old_leaf_new_address;  // Update old leaf to use new address
        out.output("   → Moving old root to new address 0x%lx\n", old_leaf_new_address);
    } else {
        op.is_root_split = false;
        // Parent is the second-to-last node in the traversal path
        if (op.path.size() >= 2) {
            BTreeNode parent = op.path[op.path.size() - 2];
            op.parent_address = parent.node_address;
            out.output("   Parent address: 0x%lx (from traversal path)\n", op.parent_address);
        } else {
            out.output("   ERROR: Path too short (%zu nodes), cannot find parent\n", op.path.size());
            op.parent_address = 0;
        }
    }
    
    // Save nodes to operation AFTER potentially updating old_leaf address
    op.old_node = old_leaf;
    op.new_node = new_leaf;
    
    // Step 6: Start async write sequence - write old node first
    auto req = new SST::Interfaces::StandardMem::Write(
        old_leaf.node_address, get_serialized_node_size(), serialize_node(old_leaf));
    auto req_id = req->getID();
    
    // Transfer operation state to this request
    pending_ops[req_id] = op;
    
    SST::Interfaces::StandardMem* target_interface = get_interface_for_address(old_leaf.node_address);
    target_interface->send(req);
    stat_network_writes->addData(1);
    
    out.output("   → Phase 1: Writing old node 0x%lx\n", old_leaf.node_address);
}

void ComputeServer::split_internal_async(AsyncOperation& op, BTreeNode& old_internal, uint64_t new_key, uint64_t new_child) {
    out.output("\n🔀 ASYNC INTERNAL SPLIT: old_internal=0x%lx, keys=%u/%u, level=%u\n",
               old_internal.node_address, old_internal.num_keys, btree_fanout, op.current_level);
    
    // Create new internal node
    uint64_t new_node_id = next_node_id++;
    uint64_t new_internal_address = allocate_node_address(new_node_id, op.current_level);
    
    BTreeNode new_internal(btree_fanout);
    new_internal.node_address = new_internal_address;
    new_internal.is_leaf = false;
    new_internal.num_keys = 0;
    
    // Determine split point
    uint32_t split_point = btree_fanout / 2;
    
    // Create temporary arrays
    std::vector<uint64_t> all_keys(btree_fanout + 1);
    std::vector<uint64_t> all_children(btree_fanout + 2);
    
    // Find insertion position
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
    all_children[insert_pos + 1] = new_child;
    
    // Copy keys and children after insert position
    for (uint32_t i = insert_pos; i < old_internal.num_keys; i++) {
        all_keys[i + 1] = old_internal.keys[i];
        all_children[i + 2] = old_internal.children[i + 1];
    }
    
    // Split: middle key gets promoted to parent
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
    
    out.output("   Split complete (promoted key=%lu):\n", promoted_key);
    out.output("     Old internal (0x%lx): %u keys\n",
               old_internal.node_address, old_internal.num_keys);
    out.output("     New internal (0x%lx): %u keys\n",
               new_internal.node_address, new_internal.num_keys);
    
    // Save split state
    op.type = AsyncOperation::SPLIT_INTERNAL;
    op.split_phase = AsyncOperation::WRITE_OLD_NODE;
    op.separator_key = promoted_key;
    
    // Check if splitting root
    if (old_internal.node_address == root_address) {
        op.is_root_split = true;
        out.output("   ⚠️  Splitting ROOT node - will create new root\n");
        
        // When splitting root, allocate NEW address for old internal (root address will be reused for new root)
        uint64_t old_internal_new_id = next_node_id++;
        uint64_t old_internal_new_address = allocate_node_address(old_internal_new_id, op.current_level);
        old_internal.node_address = old_internal_new_address;  // Update old internal to use new address
        out.output("   → Moving old root to new address 0x%lx\n", old_internal_new_address);
    } else {
        op.is_root_split = false;
        // Parent is the second-to-last node in the traversal path
        if (op.path.size() >= 2) {
            BTreeNode parent = op.path[op.path.size() - 2];
            op.parent_address = parent.node_address;
            out.output("   Parent address: 0x%lx (from traversal path)\n", op.parent_address);
        } else {
            out.output("   ERROR: Path too short (%zu nodes), cannot find parent\n", op.path.size());
            op.parent_address = 0;
        }
    }
    
    // Save nodes to operation AFTER potentially updating old_internal address
    op.old_node = old_internal;
    op.new_node = new_internal;
    
    // Start async write sequence
    auto req = new SST::Interfaces::StandardMem::Write(
        old_internal.node_address, get_serialized_node_size(), serialize_node(old_internal));
    auto req_id = req->getID();
    
    pending_ops[req_id] = op;
    
    SST::Interfaces::StandardMem* target_interface = get_interface_for_address(old_internal.node_address);
    target_interface->send(req);
    stat_network_writes->addData(1);
    
    out.output("   → Phase 1: Writing old node 0x%lx\n", old_internal.node_address);
}

void ComputeServer::handle_split_response(AsyncOperation& op) {
    switch (op.split_phase) {
        case AsyncOperation::WRITE_OLD_NODE:
            out.output("   ✓ Phase 1 complete: Old node written\n");
            out.output("   → Phase 2: Writing new node 0x%lx\n", op.new_node.node_address);
            
            // Write new node
            op.split_phase = AsyncOperation::WRITE_NEW_NODE;
            {
                auto req = new SST::Interfaces::StandardMem::Write(
                    op.new_node.node_address, get_serialized_node_size(), serialize_node(op.new_node));
                auto req_id = req->getID();
                pending_ops[req_id] = op;
                
                SST::Interfaces::StandardMem* target_interface = get_interface_for_address(op.new_node.node_address);
                target_interface->send(req);
                stat_network_writes->addData(1);
            }
            break;
            
        case AsyncOperation::WRITE_NEW_NODE:
            out.output("   ✓ Phase 2 complete: New node written\n");
            
            // Now update parent
            if (op.is_root_split) {
                out.output("   → Creating new root (tree height %u → %u)\n",
                           tree_height, tree_height + 1);
                
                // Create new root
                uint64_t new_root_id = next_node_id++;
                uint64_t new_root_addr = allocate_node_address(new_root_id, 0);
                
                BTreeNode new_root(btree_fanout);
                new_root.node_address = new_root_addr;
                new_root.is_leaf = false;
                new_root.num_keys = 1;
                new_root.keys[0] = op.separator_key;
                new_root.children[0] = op.old_node.node_address;
                new_root.children[1] = op.new_node.node_address;
                
                out.output("   DEBUG: New root children: [0]=0x%lx, [1]=0x%lx\n",
                           new_root.children[0], new_root.children[1]);
                
                // Write new root
                auto req = new SST::Interfaces::StandardMem::Write(
                    new_root_addr, get_serialized_node_size(), serialize_node(new_root));
                
                SST::Interfaces::StandardMem* target_interface = get_interface_for_address(new_root_addr);
                target_interface->send(req);
                stat_network_writes->addData(1);
                
                // Update tree metadata
                root_address = new_root_addr;
                tree_height++;
                
                out.output("   ✓ New root created at 0x%lx, tree height now %u\n",
                           root_address, tree_height);
                
                // Split complete - operation done
                SimTime_t latency = getCurrentSimTime() - op.start_time;
                stat_total_latency->addData(latency);
                stat_ops_completed->addData(1);
                
            } else {
                // Non-root split - need to update parent
                
                // Check if we have valid parent address
                if (op.parent_address == 0) {
                    // Parent not in map - need to traverse from root to find it
                    out.output("   → Phase 3: Parent unknown, traversing from root 0x%lx to find parent\n", 
                               root_address);
                    
                    op.split_phase = AsyncOperation::READ_PARENT;
                    op.current_address = root_address;
                    op.current_level = 0;
                    
                    // Start traversal from root using separator key
                    auto req = new SST::Interfaces::StandardMem::Read(root_address, get_serialized_node_size());
                    auto req_id = req->getID();
                    pending_ops[req_id] = op;
                    
                    SST::Interfaces::StandardMem* target_interface = get_interface_for_address(root_address);
                    target_interface->send(req);
                    stat_network_reads->addData(1);
                } else {
                    // Have parent address, read it directly
                    out.output("   → Phase 3: Reading parent 0x%lx to insert separator key=%lu\n",
                               op.parent_address, op.separator_key);
                    
                    op.split_phase = AsyncOperation::READ_PARENT;
                    
                    auto req = new SST::Interfaces::StandardMem::Read(op.parent_address, get_serialized_node_size());
                    auto req_id = req->getID();
                    pending_ops[req_id] = op;
                    
                    SST::Interfaces::StandardMem* target_interface = get_interface_for_address(op.parent_address);
                    target_interface->send(req);
                    stat_network_reads->addData(1);
                }
            }
            break;
            
        case AsyncOperation::READ_PARENT:
            // Parent read complete - this is handled in handle_read_response
            out.output("   ERROR: READ_PARENT should be handled in handle_read_response\n");
            break;
            
        case AsyncOperation::UPDATE_PARENT_NODE: {
            out.output("   ✓ Phase 3 complete: Parent updated\n");
            
            // Split complete - operation done
            SimTime_t latency = getCurrentSimTime() - op.start_time;
            stat_total_latency->addData(latency);
            stat_ops_completed->addData(1);
            break;
        }
            
        default:
            break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// DATA SERIALIZATION/DESERIALIZATION
// ═══════════════════════════════════════════════════════════════════════════

BTreeNode ComputeServer::deserialize_node(const std::vector<uint8_t>& data) {
    // Deserialize BTreeNode from raw bytes
    // Manual deserialization to handle std::vector properly
    BTreeNode node(btree_fanout);
    
    if (data.size() < sizeof(uint32_t) * 2 + sizeof(bool) + sizeof(uint64_t)) {
        out.output("   ⚠️  WARNING: Data too small: %zu bytes\n", data.size());
        return node;
    }
    
    size_t offset = 0;
    
    // Deserialize metadata
    std::memcpy(&node.num_keys, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    
    std::memcpy(&node.fanout, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    
    std::memcpy(&node.is_leaf, data.data() + offset, sizeof(bool));
    offset += sizeof(bool);
    
    std::memcpy(&node.node_address, data.data() + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    
    // Deserialize keys array
    if (node.num_keys > 0 && offset + node.num_keys * sizeof(uint64_t) <= data.size()) {
        std::memcpy(node.keys.data(), data.data() + offset, node.num_keys * sizeof(uint64_t));
        offset += node.fanout * sizeof(uint64_t);  // Always advance by full fanout size
    }
    
    // Deserialize values array (for leaf nodes)
    if (node.is_leaf && node.num_keys > 0 && offset + node.num_keys * sizeof(uint64_t) <= data.size()) {
        std::memcpy(node.values.data(), data.data() + offset, node.num_keys * sizeof(uint64_t));
    }
    offset += node.fanout * sizeof(uint64_t);  // Always advance by full fanout size (values region)
    
    // Deserialize children array (for internal nodes)
    if (!node.is_leaf && offset + (node.num_keys + 1) * sizeof(uint64_t) <= data.size()) {
        std::memcpy(node.children.data(), data.data() + offset, (node.num_keys + 1) * sizeof(uint64_t));
        out.output("   DEBUG DESER: Copied %u children from offset %zu\n", node.num_keys + 1, offset);
    } else if (!node.is_leaf) {
        out.output("   DEBUG DESER: SKIPPED children! is_leaf=%d, offset=%zu, need=%zu, data_size=%zu\n",
                   node.is_leaf, offset, (node.num_keys + 1) * sizeof(uint64_t), data.size());
    }
    
    out.output("   📦 Deserialized node: num_keys=%u, is_leaf=%d, addr=0x%lx",
               node.num_keys, node.is_leaf, node.node_address);
    if (node.num_keys > 0) {
        out.output(", keys[0]=%lu", node.keys[0]);
    }
    out.output("\n");
    
    return node;
}

size_t ComputeServer::get_serialized_node_size() const {
    // Calculate serialized size for ANY node with this fanout
    return sizeof(uint32_t) * 2 + sizeof(bool) + sizeof(uint64_t) +  // metadata
           btree_fanout * sizeof(uint64_t) +  // keys
           btree_fanout * sizeof(uint64_t) +  // values
           (btree_fanout + 1) * sizeof(uint64_t);  // children
}

std::vector<uint8_t> ComputeServer::serialize_node(const BTreeNode& node) {
    // Serialize BTreeNode to raw bytes
    // Manual serialization to handle std::vector properly
    size_t data_size = get_serialized_node_size();
    
    std::vector<uint8_t> data(data_size, 0);
    size_t offset = 0;
    
    // Serialize metadata
    std::memcpy(data.data() + offset, &node.num_keys, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    
    std::memcpy(data.data() + offset, &node.fanout, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    
    std::memcpy(data.data() + offset, &node.is_leaf, sizeof(bool));
    offset += sizeof(bool);
    
    std::memcpy(data.data() + offset, &node.node_address, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    
    // Serialize keys array (always serialize full fanout size for fixed layout)
    std::memcpy(data.data() + offset, node.keys.data(), node.fanout * sizeof(uint64_t));
    offset += node.fanout * sizeof(uint64_t);
    
    // Serialize values array (for leaf nodes)
    std::memcpy(data.data() + offset, node.values.data(), node.fanout * sizeof(uint64_t));
    offset += node.fanout * sizeof(uint64_t);
    
    // Serialize children array (for internal nodes)
    std::memcpy(data.data() + offset, node.children.data(), (node.fanout + 1) * sizeof(uint64_t));
    
    out.output("   📦 Serialized node: num_keys=%u, is_leaf=%d, addr=0x%lx",
               node.num_keys, node.is_leaf, node.node_address);
    if (node.num_keys > 0) {
        out.output(", keys[0]=%lu", node.keys[0]);
    }
    out.output("\n");
    
    return data;
}

void ComputeServer::write_node_back(const BTreeNode& node) {
    // Serialize and write node back to memory
    auto data = serialize_node(node);
    
    auto req = new SST::Interfaces::StandardMem::Write(node.node_address, data.size(), data);
    
    SST::Interfaces::StandardMem* target_interface = get_interface_for_address(node.node_address);
    target_interface->send(req);
    stat_network_writes->addData(1);
    
    out.output("   ✍️  Wrote node back to address 0x%lx\n", node.node_address);
}