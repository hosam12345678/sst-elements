// Copyright 2009-2025 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2025, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include <sst_config.h>
#include "computeServer.h"
#include "memoryServer.h"  // For CHUNK_SIZE constant
#include "workload/workload_generator.h"
#include <cassert>

using namespace SST;
using namespace SST::MemHierarchy;

// Memory address macros for disaggregated memory
#define MEMORY_BASE_ADDRESS 0x10000000ULL
#define GET_MEMORY_SERVER(addr) (((addr) - MEMORY_BASE_ADDRESS) / MemoryServer::ADDRESS_SPACE_PER_SERVER)

// Lock header size - reserved space before node data for lock metadata
#define LOCK_HEADER_SIZE 8

// Maximum serialized node size (in bytes)
#define NODE_MAX_SIZE 512

// Root Metadata Node - stores root pointer and tree height
// This is the SINGLE SOURCE OF TRUTH for tree metadata (no local caching!)
// Location: Memory Server 0, at base address
// Layout: [Lock(8)] [root_ptr(8) + tree_height(4) + reserved(up to NODE_MAX_SIZE)]
// Total size matches regular B+tree nodes for consistency
#define ROOT_METADATA_ADDRESS (MEMORY_BASE_ADDRESS)
// Note: ROOT_METADATA_SIZE is not defined here - it uses get_serialized_node_size() like regular nodes

// Validity bit for B+tree initialization synchronization
// Location: Memory Server 0, placed after ROOT_METADATA to avoid overlap
// VALIDITY_BIT is at: ROOT_METADATA_ADDRESS + NODE_MAX_SIZE + LOCK_HEADER_SIZE
// This ensures it's after the maximum possible ROOT_METADATA size
#define VALIDITY_BIT_OFFSET (NODE_MAX_SIZE + LOCK_HEADER_SIZE)
#define VALIDITY_BIT_ADDRESS (MEMORY_BASE_ADDRESS + VALIDITY_BIT_OFFSET)

// Special signal address for restart operations (after validity bit)
#define RESTART_SIGNAL_ADDRESS (VALIDITY_BIT_ADDRESS + 8)

// ═══════════════════════════════════════════════════════════════════════════
// CONSTRUCTOR
// ═══════════════════════════════════════════════════════════════════════════

ComputeServer::ComputeServer(ComponentId_t id, Params& params) :
    SST::Component(id),
    last_op_time(0)
{
    // Parse configuration parameters
    node_id = params.find<uint32_t>("node_id", 0);
    num_memory_nodes = params.find<uint32_t>("num_memory_nodes", 4);
    workload_type = params.find<std::string>("workload_type", "ycsb_a");
    ops_per_second = params.find<uint32_t>("operations_per_second", 10000);
    simulation_duration = params.find<SimTime_t>("simulation_duration_us", 1000000) * 1000; // Convert to ns
    zipfian_alpha = params.find<double>("zipfian_alpha", 0.9);
    std::string key_dist = params.find<std::string>("key_distribution", "zipfian");
    read_ratio = params.find<double>("read_ratio", 0.95);
    btree_fanout = params.find<uint32_t>("btree_fanout", 16);
    key_range = params.find<uint64_t>("key_range", 1000000);
    verbose_level = params.find<int>("verbose", 0);

    // Override zipfian_alpha based on distribution type
    if (key_dist == "uniform") {
        zipfian_alpha = 0.0;  // Force uniform distribution
    }

    // Validate fanout against maximum node size
    // BTreeNode size (worst case - internal node):
    // = 21 bytes (header: num_keys=4, is_leaf=1, node_address=8, next_leaf=8)
    // + (fanout-1) * 8 bytes (keys array)
    // + fanout * 8 bytes (children array for internal nodes)
    // = 21 + 8*(fanout-1) + 8*fanout = 21 + 16*fanout - 8 = 13 + 16*fanout
    const uint64_t NODE_HEADER_SIZE = 21;
    const uint64_t BYTES_PER_KEY = 8;
    const uint64_t BYTES_PER_CHILD = 8;
    uint64_t max_internal_node_size = NODE_HEADER_SIZE + 
                                       (btree_fanout - 1) * BYTES_PER_KEY + 
                                       btree_fanout * BYTES_PER_CHILD;
    
    if (max_internal_node_size > NODE_MAX_SIZE) {
        uint32_t max_supported_fanout = (NODE_MAX_SIZE - 13) / 16;
        out.fatal(CALL_INFO, -1, 
                  "Fanout %u is too large! Max internal node size would be %lu bytes (limit: %u bytes)\n"
                  "Maximum supported fanout = (%u - 13) / 16 = %u\n",
                  btree_fanout, max_internal_node_size, NODE_MAX_SIZE, 
                  NODE_MAX_SIZE, max_supported_fanout);
    }
    
    if (verbose_level >= 1) {
        out.output("B+tree fanout validated: %u (max node size: %lu bytes)\n", 
                   btree_fanout, max_internal_node_size);
    }
    
    // Initialize B+tree state
    // NOTE: root_address and tree_height are NO LONGER stored locally!
    // They are read from ROOT_METADATA_ADDRESS at the start of each operation
    next_node_id = 0;
    tree_initialized = false;  // Will be set true after validity bit check passes
    checking_validity_bit = false;  // Will be set true when checking validity
    
    // Initialize round-robin memory server selection
    // Start with server based on compute node_id to distribute initial load
    current_memory_server = node_id % num_memory_nodes;

    // Setup debug output
    dbg.init("", 5, 0, (Output::output_location_t)1);
    out.init("ComputeServer[@p:@l]: ", 1, 0, Output::STDOUT);
    
    // Initialize B+tree components
    serializer = new BTreeSerializer(btree_fanout, &out);
    lock_manager = new BTreeLockManager(node_id, verbose_level, &out);
    btree_ops = new BTreeOperations(btree_fanout, verbose_level, &out);
    
    // Initialize workload generator
    workload_gen = new WorkloadGenerator(node_id, key_range, zipfian_alpha, read_ratio,
                                        ops_per_second, simulation_duration, &out, 
                                        std::random_device{}() + id);

    // Initialize statistics
    stat_inserts = registerStatistic<uint64_t>("btree_inserts");
    stat_searches = registerStatistic<uint64_t>("btree_searches");
    stat_network_reads = registerStatistic<uint64_t>("network_reads");
    stat_network_writes = registerStatistic<uint64_t>("network_writes");
    stat_total_latency = registerStatistic<uint64_t>("total_latency");
    stat_ops_completed = registerStatistic<uint64_t>("operations_completed");

    // Setup multiple network interfaces (one per memory server)
    auto mem_handler = new SST::Interfaces::StandardMem::Handler2<ComputeServer,&ComputeServer::handleMemoryEvent>(this);
    
    for (int i = 0; i < num_memory_nodes; i++) {
        std::string interface_name = "mem_interface_" + std::to_string(i);
        auto interface_i = loadUserSubComponent<SST::Interfaces::StandardMem>(interface_name, 
            SST::ComponentInfo::SHARE_NONE, registerTimeBase("1ns"), mem_handler);
        if (interface_i) {
            memory_interfaces.push_back(interface_i);
            out.output("  Loaded network interface to Memory Server %d\n", i);
        } else {
            out.fatal(CALL_INFO, -1, "Failed to load network interface %s\n", interface_name.c_str());
        }
    }
    
    out.output("  Many-to-Many connectivity: %d interfaces loaded\n", (int)memory_interfaces.size());

    // Validate memory layout consistency between ComputeServer and MemoryServer
    // This ensures our address calculations match the memory servers' expectations
    uint64_t expected_address_space = MemoryServer::ADDRESS_SPACE_PER_SERVER;
    uint64_t test_addr = MEMORY_BASE_ADDRESS + expected_address_space;
    uint32_t calculated_server_id = GET_MEMORY_SERVER(test_addr);
    
    if (calculated_server_id != 1) {
        out.fatal(CALL_INFO, -1, 
                  "Memory layout validation FAILED!\n"
                  "  ADDRESS_SPACE_PER_SERVER = 0x%lx (%lu bytes)\n"
                  "  Test address (MS0_base + ADDRESS_SPACE) = 0x%lx\n"
                  "  Expected server_id: 1, Got: %u\n"
                  "  GET_MEMORY_SERVER formula: (addr - BASE) / ADDRESS_SPACE_PER_SERVER\n"
                  "  ComputeServer and MemoryServer have inconsistent memory layout constants!\n",
                  expected_address_space, expected_address_space,
                  test_addr, calculated_server_id);
    }
    
    if (verbose_level >= 1) {
        out.output("Memory layout validated:\n");
        out.output("  ADDRESS_SPACE_PER_SERVER = 0x%llx (%llu MB)\n", 
                   (unsigned long long)expected_address_space, (unsigned long long)expected_address_space / (1024*1024));
        out.output("  RESERVED_METADATA_SIZE = 0x%llx (%llu bytes)\n",
                   (unsigned long long)MemoryServer::RESERVED_METADATA_SIZE, (unsigned long long)MemoryServer::RESERVED_METADATA_SIZE);
        out.output("  ROOT_METADATA at 0x%llx (server %llu)\n", 
                   (unsigned long long)ROOT_METADATA_ADDRESS, (unsigned long long)GET_MEMORY_SERVER(ROOT_METADATA_ADDRESS));
        out.output("  VALIDITY_BIT at 0x%llx (server %llu)\n",
                   (unsigned long long)VALIDITY_BIT_ADDRESS, (unsigned long long)GET_MEMORY_SERVER(VALIDITY_BIT_ADDRESS));
    }

    // Setup clock
    clock_handler = new SST::Clock::Handler2<ComputeServer,&ComputeServer::tick>(this);
    registerClock("1MHz", clock_handler);

    out.output("Compute Server %d initialized\n", node_id);
    out.output("  Workload: %s, Ops/sec: %d, Read ratio: %.2f\n", 
               workload_type.c_str(), ops_per_second, read_ratio);
    out.output("  Key distribution: %s (alpha=%.2f), Key range: %lu\n", 
               (zipfian_alpha <= 0.0) ? "UNIFORM" : "ZIPFIAN", zipfian_alpha, key_range);
}

ComputeServer::~ComputeServer() {
    if (serializer) {
        delete serializer;
        serializer = nullptr;
    }
    if (lock_manager) {
        delete lock_manager;
        lock_manager = nullptr;
    }
    if (btree_ops) {
        delete btree_ops;
        btree_ops = nullptr;
    }
    if (workload_gen) {
        delete workload_gen;
        workload_gen = nullptr;
    }
}

void ComputeServer::init(unsigned int phase) {
    // Initialize all interfaces
    for (auto& interface : memory_interfaces) {
        interface->init(phase);
    }
    
    if (phase == 0) {
        out.output("Node %d: Initializing with alpha=%.1f, key_range=%lu, distribution=%s\n", 
                   node_id, zipfian_alpha, key_range, 
                   (zipfian_alpha <= 0.0) ? "UNIFORM" : "ZIPFIAN");
        
        // Generate workload using WorkloadGenerator
        workload_gen->generate_workload(pending_operations);
        out.output("Generated %zu operations\n", pending_operations.size());
    }
}

void ComputeServer::setup() {
    // Setup all interfaces
    for (auto& interface : memory_interfaces) {
        interface->setup();
    }
    
    // Request initial chunk from the current memory server
    // This ensures we have at least one chunk available before starting operations
    if (verbose_level >= 1) {
        out.output("\n=== Chunk Allocation Setup ===\n");
        out.output("Compute %d: Requesting initial chunk from memory server %u (round-robin start)\n",
                   node_id, current_memory_server);
    }
    request_chunk_allocation(current_memory_server);
    
    // NOTE: B+tree initialization (node 0 only) will be triggered in tick() 
    // after chunk allocation completes, ensuring chunks are ready before allocating nodes
}

void ComputeServer::finish() {
    // Finish all interfaces
    for (auto& interface : memory_interfaces) {
        interface->finish();
    }
    
    out.output("\n=== Final Statistics ===\n");
    out.output("Search operations completed: %lu\n", stat_ops_completed->getCollectionCount());
    out.output("Network reads: %lu\n", stat_network_reads->getCollectionCount());
    out.output("Average latency: %lu ns\n", 
               stat_total_latency->getCollectionCount() / std::max(1UL, stat_ops_completed->getCollectionCount()));
}

bool ComputeServer::tick(Cycle_t current_cycle) {
    // Wait for initial chunk allocation to complete before starting operations
    // This ensures we have memory available before initializing the tree
    if (allocated_chunks.find(current_memory_server) == allocated_chunks.end() ||
        allocated_chunks[current_memory_server].empty()) {
        // Initial chunk not ready yet - keep waiting
        return false;
    } else {
        // Chunks are ready - node 0 can now initialize the tree
        if (node_id == 0 && !tree_initialized && !checking_validity_bit) {
            initialize_btree();
            return false;  // Wait for initialization to complete
        }
    }
    
    // Wait for tree initialization to complete before starting operations
    if (!tree_initialized) {
        // Non-initializing nodes need to check the validity bit
        if (node_id != 0) {
            check_tree_initialization();
        }
        return false;  // Tree not ready yet
    }
    
    if (!pending_operations.empty()) {
        WorkloadOp next_op = pending_operations.front();
        pending_operations.pop();
        process_btree_operation(next_op);   
    }

    
    SimTime_t current_time = getCurrentSimTime();
    bool time_expired = current_time >= simulation_duration;
    bool no_pending_workload = pending_operations.empty();
    bool no_pending_async = pending_ops.empty();
    // out.output("Tick: current_time=%d, pending_ops_size=%ld\n",
    //            simulation_duration, pending_ops.size());
    if (time_expired && no_pending_workload && no_pending_async) {
        if (verbose_level >= 1) {
            out.output("Node %u: Simulation complete at time %lu ns (duration %lu ns)\n",
                      node_id, current_time, simulation_duration);
        }
        return true;
    }
    
    return false;
}

void ComputeServer::handleMemoryEvent(Interfaces::StandardMem::Request* req) {
    auto req_id = req->getID();
    
    // Check if this is a lock operation (LL/SC protocol)
    if (handle_lock_operations(req)) {
        delete req;
        return;
    }

    // Check if this is a special operation (chunk allocation, etc.)
    if (handle_special_operation_response(req)) {
        delete req;
        return;
    }
    
    
    // Handle regular read/write responses
    if (auto* read_resp = dynamic_cast<Interfaces::StandardMem::ReadResp*>(req)) {
        handle_read_response(req_id, read_resp->data);
    } else if (auto* write_resp = dynamic_cast<Interfaces::StandardMem::WriteResp*>(req)) {
        handle_write_response(req_id, write_resp);
    }
    
    delete req;
}

bool ComputeServer::handle_special_operation_response(Interfaces::StandardMem::Request* req) {
    auto req_id = req->getID();
    
    // Check if this is a response to one of our pending operations
    auto op_it = pending_ops.find(req_id);
    if (op_it == pending_ops.end()) {
        return false;  // Not a pending operation
    }
    
    AsyncOperation& op = op_it->second;
    
    // Check operation type and handle accordingly
    switch (op.type) {
        case AsyncOperation::CHUNK_ALLOCATE:
            if (auto* read_resp = dynamic_cast<Interfaces::StandardMem::ReadResp*>(req)) {
                handle_chunk_allocation_response(req_id, read_resp);
                return true;  // Handled
            }
            break;
            
        case AsyncOperation::VALIDITY_CHECK:
            if (auto* read_resp = dynamic_cast<Interfaces::StandardMem::ReadResp*>(req)) {
                handle_validity_check_response(req_id, read_resp);
                return true;  // Handled
            }
            break;
            
        case AsyncOperation::READ_ROOT_METADATA:
            if (auto* read_resp = dynamic_cast<Interfaces::StandardMem::ReadResp*>(req)) {
                handle_root_metadata_response(req_id, read_resp->data);
                return true;  // Handled
            }
            break;
            
        case AsyncOperation::INIT_WRITE:
            if (auto* write_resp = dynamic_cast<Interfaces::StandardMem::WriteResp*>(req)) {
                handle_btree_initialization_write(req_id, op);
                return true;  // Handled
            }
            break;
            
        case AsyncOperation::RESTART_SIGNAL:
            if (auto* write_resp = dynamic_cast<Interfaces::StandardMem::WriteResp*>(req)) {
                handle_restart_after_lock_release(req_id, op);
                return true;  // Handled
            }
            break;
        
        default:
            // Not a special operation, let normal handlers process it
            return false;
    }
    
    return false;  // Not handled as special operation
}

void ComputeServer::btree_search_async(uint64_t key) {
    // Step 1: Read root metadata to get current root address and tree height
    // We do NOT cache this locally - always read from memory!
    AsyncOperation op;
    op.type = AsyncOperation::READ_ROOT_METADATA;
    op.intended_operation_type = AsyncOperation::SEARCH;  // Store the actual operation type
    op.key = key;
    op.current_level = 0;
    op.start_time = getCurrentSimTime();
    
    if (verbose_level >= 2) {
        out.output("SEARCH key=%lu - reading root metadata first\n", key);
    }
    
    // Read root metadata with SHARED lock
    read_root_metadata_async(op);
}

void ComputeServer::btree_insert_async(uint64_t key, uint64_t value) {
    // Step 1: Read root metadata to get current root address and tree height
    // We do NOT cache this locally - always read from memory!
    AsyncOperation op;
    op.type = AsyncOperation::READ_ROOT_METADATA;
    op.intended_operation_type = AsyncOperation::INSERT;  // Store the actual operation type
    op.key = key;
    op.value = value;  // Store actual value
    op.current_level = 0;
    op.start_time = getCurrentSimTime();
    
    if (verbose_level >= 1) {
        out.output("INSERT key=%lu, value=%lu - reading root metadata first\n", key, value);
    }
    
    // Read root metadata with SHARED lock
    read_root_metadata_async(op);
}

// ═══════════════════════════════════════════════════════════════════════════
// LOCK PROTOCOL HANDLERS (LL/SC - LoadLink/StoreConditional)
// ═══════════════════════════════════════════════════════════════════════════

bool ComputeServer::handle_lock_operations(Interfaces::StandardMem::Request* req) {
    auto req_id = req->getID();
    
    // Check if this is a response to one of our pending operations
    auto op_it = pending_ops.find(req_id);
    if (op_it == pending_ops.end()) {
        return false;  // Not a pending operation
    }
    
    AsyncOperation& op = op_it->second;
    
    // Try lock acquisition first
    if (handle_lock_acquisition(req_id, op, req)) {
        return true;
    }
    
    // Try lock release second
    if (handle_lock_release(req_id, op, req)) {
        return true;
    }
    
    return false;  // Not a lock operation
}

bool ComputeServer::handle_lock_acquisition(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op,
    SST::Interfaces::StandardMem::Request* req) {
    
    // Handle LoadLink (Read) responses - step 1 of LL/SC acquisition
    if (op.waiting_for_loadlink_response) {
        if (auto* read_resp = dynamic_cast<Interfaces::StandardMem::ReadResp*>(req)) {
            lock_manager->handle_loadlink_response(req_id, read_resp->data, pending_ops, 
                                                  get_interface_for_address(op.lock_target_address),
                                                  get_serialized_node_size());
            stat_network_reads->addData(1);
            return true;  // Handled
        }
    }
    
    // Handle StoreConditional (Write) responses - step 2 of LL/SC acquisition
    if (op.waiting_for_sc_response) {
        if (auto* write_resp = dynamic_cast<Interfaces::StandardMem::WriteResp*>(req)) {
            lock_manager->handle_storeconditional_response(req_id, write_resp, pending_ops, 
                                                           get_interface_for_address(op.lock_target_address),
                                                           get_serialized_node_size());
            stat_network_writes->addData(1);
            return true;  // Handled
        }
    }
    
    return false;  // Not a lock acquisition operation
}

bool ComputeServer::handle_lock_release(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op,
    SST::Interfaces::StandardMem::Request* req) {
    
    // Handle LoadLink (Read) responses during lock release
    if (op.waiting_for_release_ll) {
        if (auto* read_resp = dynamic_cast<Interfaces::StandardMem::ReadResp*>(req)) {
            auto interface_getter = [this](uint64_t addr) { return get_interface_for_address(addr); };
            lock_manager->handle_release_loadlink_response(req_id, read_resp->data, pending_ops, interface_getter);
            stat_network_reads->addData(1);
            return true;  // Handled
        }
    }
    
    // Handle StoreConditional (Write) responses during lock release
    if (op.waiting_for_release_sc) {
        if (auto* write_resp = dynamic_cast<Interfaces::StandardMem::WriteResp*>(req)) {
            auto interface_getter = [this](uint64_t addr) { return get_interface_for_address(addr); };
            lock_manager->handle_release_storeconditional_response(req_id, write_resp, pending_ops, 
                                                                   interface_getter, RESTART_SIGNAL_ADDRESS);
            stat_network_writes->addData(1);
            
            // Check if operation can complete now (all locks released)
            if (pending_ops.count(req_id) && lock_manager->is_operation_complete(pending_ops[req_id])) {
                auto& completed_op = pending_ops[req_id];
                // Record latency and completion
                SimTime_t latency = getCurrentSimTime() - completed_op.start_time;
                stat_total_latency->addData(latency);
                stat_ops_completed->addData(1);
                
                // Remove from pending operations
                pending_ops.erase(req_id);
            }
            return true;  // Handled
        }
    }
    
    return false;  // Not a lock release operation
}

void ComputeServer::handle_read_response(Interfaces::StandardMem::Request::id_t req_id,
                                         const std::vector<uint8_t>& data) {
    if (!pending_ops.count(req_id)) return;
    
    auto& op = pending_ops[req_id];
    
    // Deserialize the node we just read
    BTreeNode node = serializer->deserialize(data);
    node.node_address = op.current_address;
    op.path.push_back(node);
    
    // Check if we've reached a leaf node
    // NOTE: tree_height is stored in op.tree_height (read from metadata at operation start)
    bool at_leaf = (node.is_leaf || op.current_level >= op.tree_height - 1);
    
    if (!at_leaf) {
        // Internal node - continue traversal
        handle_btree_traversal(req_id, op, node);
        return;
    }
    
    // ========================================================================
    // We've reached the LEAF node - handle based on operation type
    // ========================================================================
    
    if (op.type == AsyncOperation::SEARCH) {
        handle_leaf_search(req_id, op, node);
    } else if (op.type == AsyncOperation::INSERT) {
        handle_leaf_insert(req_id, op, node);
    }
}

void ComputeServer::handle_write_response(Interfaces::StandardMem::Request::id_t req_id,
                                          Interfaces::StandardMem::WriteResp* resp) {
    if (!pending_ops.count(req_id)) return;
    
    auto& op = pending_ops[req_id];
    
    if (op.waiting_for_write) {
        // Check if this is part of a split operation
        if (op.split_phase == AsyncOperation::WRITE_OLD_NODE || 
            op.split_phase == AsyncOperation::WRITE_NEW_NODE) {
            handle_split_write_response(req_id, op);
            return;
        }
        
        // Check if this is a root split operation
        if (op.is_root_split || op.current_address == ROOT_METADATA_ADDRESS) {
            handle_root_split_write_response(req_id, op);
            return;
        }
        
        // Simple write (no split) or final write - complete the operation
        handle_simple_write_completion(req_id, op);
    }
}

void ComputeServer::handle_split_write_response(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op) {
    
    if (op.split_phase == AsyncOperation::WRITE_OLD_NODE) {
        // Left leaf written, now write right leaf
        if (verbose_level >= 2) {
            out.output("      Left leaf write complete, writing right leaf...\n");
        }
        
        op.split_phase = AsyncOperation::WRITE_NEW_NODE;
        op.waiting_for_write = false;
        write_split_nodes(req_id, op);
        
    } else if (op.split_phase == AsyncOperation::WRITE_NEW_NODE) {
        // Both leaves written, now need to update parent
        if (verbose_level >= 1) {
            out.output("   SPLIT: Both leaves written, updating parent with separator=%lu\n",
                      op.separator_key);
        }
        
        // Phase 4: Update parent with separator key
        op.split_phase = AsyncOperation::UPDATE_PARENT_NODE;
        op.waiting_for_write = false;
        update_parent_after_split(req_id, op);
    }
}

void ComputeServer::handle_root_split_write_response(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op) {
    
    // Check if this was a root metadata update (after root split)
    // Only operations writing to ROOT_METADATA_ADDRESS are metadata updates
    if (op.current_address == ROOT_METADATA_ADDRESS) {
        if (verbose_level >= 1) {
            out.output("   SPLIT: Root metadata update complete (root=0x%lx, height=%u)\n",
                      op.parent_address, op.tree_height);
            out.output("   INSERT key=%lu - root split with metadata update finished\n", op.key);
        }
        
        stat_inserts->addData(1);
        
        // Mark operation as ready to complete
        op.ready_to_complete = true;
        op.waiting_for_write = false;
        
        // Release all locks held during operation (async with LL/SC)
        auto interface_getter = [this](uint64_t addr) { return get_interface_for_address(addr); };
        lock_manager->release_all_locks(req_id, op, interface_getter, pending_ops);
        
        // Check if operation can complete immediately (no locks to release)
        if (lock_manager->is_operation_complete(op)) {
            // Record latency and completion
            SimTime_t latency = getCurrentSimTime() - op.start_time;
            stat_total_latency->addData(latency);
            
            if (verbose_level >= 1) {
                out.output("   INSERT key=%lu COMPLETE (latency: %lu cycles)\n", op.key, latency);
            }
            
            pending_ops.erase(req_id);
        }
        return;
    }
    
    // Check if this was a root split (new root node write, before metadata update)
    if (op.is_root_split) {
        if (verbose_level >= 1) {
            out.output("   SPLIT: Root split complete, updating tree metadata\n");
        }
        
        // Update root metadata at MEMORY_BASE_ADDRESS
        // New root address is stored in op.parent_address
        // New tree height is stored in op.tree_height
        update_root_metadata_async(req_id, op, op.parent_address, op.tree_height);
        
        if (verbose_level >= 1) {
            out.output("   New root: 0x%lx, Tree height: %u (updating metadata)\n", 
                      op.parent_address, op.tree_height);
        }
        
        // NOTE: We do NOT mark ready_to_complete here yet!
        // The metadata update will complete the operation when done
        return;  // Exit here, metadata update will continue
    }
}

void ComputeServer::handle_simple_write_completion(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op) {
    
    if (verbose_level >= 1) {
        out.output("   INSERT key=%lu - write complete, operation finished\n", op.key);
    }
    
    stat_inserts->addData(1);
    
    // Mark operation as ready to complete
    op.ready_to_complete = true;
    
    // Release all locks held during operation (async with LL/SC)
    auto interface_getter = [this](uint64_t addr) { return get_interface_for_address(addr); };
    lock_manager->release_all_locks(req_id, op, interface_getter, pending_ops);
    
    // Check if operation can complete immediately (no locks to release)
    if (lock_manager->is_operation_complete(op)) {
        // Record latency and completion
        SimTime_t latency = getCurrentSimTime() - op.start_time;
        stat_total_latency->addData(latency);
        stat_ops_completed->addData(1);
        
        // Remove from pending operations
        pending_ops.erase(req_id);
    }
    // Otherwise, operation will complete after locks are released
}

uint64_t ComputeServer::get_child_index_for_key(const BTreeNode& node, uint64_t key) {
    uint32_t i = 0;
    while (i < node.num_keys && key >= node.keys[i]) {
        i++;
    }
    return i;
}

bool ComputeServer::search_key_in_node(const BTreeNode& node, uint64_t key) {
    // Binary search would be more efficient for large fanouts
    // But linear search is fine for typical B+tree fanouts (16-256)
    for (uint32_t i = 0; i < node.num_keys; i++) {
        if (node.keys[i] == key) {
            return true;
        }
    }
    return false;
}

void ComputeServer::handle_btree_traversal(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op,
    BTreeNode& node) {
    
    // This function handles traversal through internal nodes
    // Leaf node handling is done in handle_read_response
    
    // Internal node - find appropriate child to traverse to
    uint64_t child_idx = get_child_index_for_key(node, op.key);
    uint64_t child_addr = node.children[child_idx];
    
    if (verbose_level >= 2) {
        out.output("   → Traverse to child[%lu] = 0x%lx (level %u, op=%s)\n", 
                  child_idx, child_addr, op.current_level + 1,
                  (op.type == AsyncOperation::SEARCH) ? "SEARCH" : "INSERT");
    }
    
    // Update operation state for next level
    op.current_level++;
    op.current_address = child_addr;
    
    // Delegate back to btree_ops to continue the operation
    // Note: btree_ops will call lock_manager->try_acquire_lock_async() which creates
    // a NEW request with a NEW req_id and inserts it into pending_ops.
    // We must erase the old req_id AFTER to avoid losing the operation if something fails.
    auto interface_getter = [this](uint64_t addr) { return get_interface_for_address(addr); };
    
    if (op.type == AsyncOperation::SEARCH) {
        btree_ops->btree_search_async(op.key, op, pending_ops, lock_manager, interface_getter, stat_network_reads);
    } else if (op.type == AsyncOperation::INSERT) {
        btree_ops->btree_insert_async(op.key, op.value, op, pending_ops, lock_manager, interface_getter, stat_network_reads);
    }
    
    // Remove old request ID now that new one has been created
    pending_ops.erase(req_id);
}

void ComputeServer::handle_leaf_search(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op,
    BTreeNode& node) {
    
    // Validate we're at a leaf node
    assert(node.is_leaf && "handle_leaf_search called on non-leaf node");
    assert(op.type == AsyncOperation::SEARCH && "handle_leaf_search called with non-SEARCH operation");
    
    // Perform search operation in leaf node
    bool found = search_key_in_node(node, op.key);
    
    if (verbose_level >= 2) {
        out.output("   SEARCH key=%lu: %s\n", op.key, found ? "FOUND" : "NOT FOUND");
    }
    
    stat_searches->addData(1);
    
    // Mark operation as ready to complete
    op.ready_to_complete = true;
    
    // Release all locks held during traversal (async with LL/SC)
    auto interface_getter = [this](uint64_t addr) { return get_interface_for_address(addr); };
    lock_manager->release_all_locks(req_id, op, interface_getter, pending_ops);
    
    // Check if operation can complete immediately (no locks to release)
    if (lock_manager->is_operation_complete(op)) {
        // Record latency and completion
        SimTime_t latency = getCurrentSimTime() - op.start_time;
        stat_total_latency->addData(latency);
        stat_ops_completed->addData(1);
        
        // Operation complete - remove from pending
        pending_ops.erase(req_id);
    }
    // Otherwise, operation will complete after locks are released
}

void ComputeServer::handle_leaf_insert(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op,
    BTreeNode& node) {
    
    // Validate we're at a leaf node
    assert(node.is_leaf && "handle_leaf_insert called on non-leaf node");
    assert(op.type == AsyncOperation::INSERT && "handle_leaf_insert called with non-INSERT operation");
    
    if (verbose_level >= 1) {
        out.output("   INSERT key=%lu, value=%lu - at leaf (addr=0x%lx, num_keys=%u/%u)\n",
                  op.key, op.value, node.node_address, node.num_keys, btree_fanout - 1);
    }
    
    // Phase 2: Check if key already exists - update value if found
    for (uint32_t i = 0; i < node.num_keys; i++) {
        if (node.keys[i] == op.key) {
            if (verbose_level >= 1) {
                out.output("   INSERT key=%lu - key exists, updating value %lu -> %lu\n", 
                          op.key, node.values[i], op.value);
            }
            
            // Update the value
            node.values[i] = op.value;
            
            // Write modified leaf back to memory
            write_leaf_and_complete(req_id, op, node);
            return;
        }
    }
    
    // Phase 2: Check if leaf has space for new key
    if (node.num_keys >= btree_fanout - 1) {
        // Node is full - needs split
        if (verbose_level >= 1) {
            out.output("   INSERT key=%lu - leaf FULL, needs split\n", op.key);
        }
        
        // Check if we're in optimistic mode (holding only shared locks on path)
        if (!op.pessimistic_mode) {
            // We need to split but only have shared locks on the PATH (not leaf)!
            // Leaf has exclusive lock (acquired preemptively), but path has shared locks
            // Must restart with exclusive locks on entire path
            if (verbose_level >= 1) {
                out.output("   ⚠️  OPTIMISTIC MODE failed - restarting with EXCLUSIVE locks on path\n");
            }
            restart_insert_with_exclusive_locks(req_id, op);
            return;
        }
        
        // Already in pessimistic mode with exclusive locks on entire path - proceed with split
        if (verbose_level >= 1) {
            out.output("   PESSIMISTIC MODE - proceeding with split\n");
        }
        handle_leaf_split(req_id, op, node);
        return;
    }
    
    // Phase 2: Leaf is safe for insert - has space
    // We have exclusive lock on leaf (acquired preemptively in optimistic mode)
    if (verbose_level >= 2) {
        out.output("   ✅ Leaf is SAFE for insert (has space: %u/%u keys, have EXCLUSIVE lock)\n",
                  node.num_keys, btree_fanout - 1);
    }
    
    // Phase 2: Insert key/value into leaf in sorted order
    insert_into_leaf(node, op.key, op.value);
    
    if (verbose_level >= 1) {
        out.output("   INSERT key=%lu - inserted at leaf, now has %u keys\n", 
                  op.key, node.num_keys);
    }
    
    // Write modified leaf back to memory
    write_leaf_and_complete(req_id, op, node);
}

void ComputeServer::restart_insert_with_exclusive_locks(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op) {
    
    if (verbose_level >= 1) {
        out.output("\n🔄 RESTART INSERT with PESSIMISTIC locking: key=%lu, value=%lu\n", 
                  op.key, op.value);
    }
    
    // Mark operation for restart AFTER lock release completes
    // Store restart parameters in the operation
    op.type = AsyncOperation::RESTART_SIGNAL;
    op.restart_pending = true;
    op.restart_key = op.key;
    op.restart_value = op.value;
    op.restart_start_time = op.start_time;
    
    if (verbose_level >= 2) {
        out.output("   Will restart after releasing %zu held locks\n", op.held_locks.size());
    }
    
    // Release all locks acquired during optimistic traversal
    // When release completes, the operation will have ready_to_complete=true
    // and we'll trigger a write to RESTART_SIGNAL_ADDRESS to invoke the restart handler
    auto interface_getter = [this](uint64_t addr) { return get_interface_for_address(addr); };
    lock_manager->release_all_locks(req_id, op, interface_getter, pending_ops);
    
    // Note: Lock release is async. When complete, the operation will still be in pending_ops.
    // We need to detect this in a response handler and trigger the restart.
}

void ComputeServer::handle_restart_after_lock_release(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op) {
    
    if (verbose_level >= 2) {
        out.output("   🔄 RESTART triggered: Starting new pessimistic INSERT for key=%lu\n", op.restart_key);
    }
    
    // Create new operation starting from root metadata read with PESSIMISTIC mode
    // We need to re-read metadata since root may have changed during our operation
    AsyncOperation new_op;
    new_op.type = AsyncOperation::READ_ROOT_METADATA;  // Start with metadata read
    new_op.intended_operation_type = AsyncOperation::INSERT;  // This will be an INSERT operation
    new_op.key = op.restart_key;
    new_op.value = op.restart_value;
    new_op.current_level = 0;
    new_op.start_time = op.restart_start_time;      // Keep original timestamp for latency tracking
    new_op.restart_start_time = op.restart_start_time;  // ← CRITICAL: Mark this as a restarted operation!
    new_op.pessimistic_mode = true;                 // ← KEY: This forces exclusive locks!
    
    if (verbose_level >= 2) {
        out.output("   Restarting from root metadata read with EXCLUSIVE locks (pessimistic mode)\n");
    }
    
    // Erase the old RESTART_SIGNAL operation
    pending_ops.erase(req_id);
    
    // Read root metadata again (root may have changed)
    read_root_metadata_async(new_op);
}


void ComputeServer::insert_into_leaf(BTreeNode& leaf, uint64_t key, uint64_t value) {
    // Validate assumptions
    assert(leaf.is_leaf && "insert_into_leaf called on non-leaf node");
    // Note: We allow insertion into full leaves (num_keys == btree_fanout - 1) because
    // handle_leaf_split() temporarily inserts into a full node to determine split point
    assert(leaf.num_keys <= btree_fanout - 1 && "insert_into_leaf called on overfull leaf");
    
    // Find insertion position (keys are sorted)
    uint32_t insert_pos = 0;
    while (insert_pos < leaf.num_keys && leaf.keys[insert_pos] < key) {
        insert_pos++;
    }
    
    // Ensure key doesn't already exist (should have been checked)
    assert((insert_pos >= leaf.num_keys || leaf.keys[insert_pos] != key) && 
           "Duplicate key in insert_into_leaf");
    
    // Shift keys and values to make space
    for (uint32_t i = leaf.num_keys; i > insert_pos; i--) {
        leaf.keys[i] = leaf.keys[i - 1];
        leaf.values[i] = leaf.values[i - 1];
    }
    
    // Insert new key/value
    leaf.keys[insert_pos] = key;
    leaf.values[insert_pos] = value;
    leaf.num_keys++;
    
    if (verbose_level >= 2) {
        out.output("      Inserted at position %u, leaf now has %u keys\n", 
                  insert_pos, leaf.num_keys);
    }
}

void ComputeServer::insert_into_internal_node(BTreeNode& internal, uint64_t key, uint64_t right_child) {
    // Validate assumptions
    assert(!internal.is_leaf && "insert_into_internal_node called on leaf node");
    // Note: We allow insertion into full nodes (num_keys == btree_fanout - 1) because
    // handle_internal_split() temporarily inserts into a full node to determine split point
    assert(internal.num_keys <= btree_fanout - 1 && "insert_into_internal_node called on overfull node");
    
    if (verbose_level >= 2) {
        out.output("      Inserting separator key=%lu into internal node 0x%lx\n",
                  key, internal.node_address);
    }
    
    // Find insertion position
    // In internal node: keys[i] is the separator between children[i] and children[i+1]
    uint32_t insert_pos = 0;
    while (insert_pos < internal.num_keys && internal.keys[insert_pos] < key) {
        insert_pos++;
    }
    
    // Shift keys to make space
    for (uint32_t i = internal.num_keys; i > insert_pos; i--) {
        internal.keys[i] = internal.keys[i - 1];
    }
    
    // Shift children pointers (note: children has num_keys+1 elements)
    // We need to shift children[insert_pos+1..num_keys] to the right
    for (uint32_t i = internal.num_keys + 1; i > insert_pos + 1; i--) {
        internal.children[i] = internal.children[i - 1];
    }
    
    // Insert new key and right child pointer
    internal.keys[insert_pos] = key;
    internal.children[insert_pos + 1] = right_child;
    internal.num_keys++;
    
    if (verbose_level >= 2) {
        out.output("      Inserted at position %u, internal node now has %u keys\n",
                  insert_pos, internal.num_keys);
    }
}

void ComputeServer::handle_internal_split(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op,
    BTreeNode& node) {
    
    assert(!node.is_leaf && "handle_internal_split called on leaf node");
    assert(node.num_keys >= btree_fanout - 1 && "handle_internal_split called on non-full node");
    
    if (verbose_level >= 1) {
        out.output("   SPLIT: Internal node 0x%lx is full (%u keys), splitting...\n",
                  node.node_address, node.num_keys);
    }
    
    // First, we need to insert the pending separator into this node temporarily
    // to determine the proper split
    BTreeNode temp_node = node;
    insert_into_internal_node(temp_node, op.separator_key, op.new_node.node_address);
    
    // Now split the temporary node
    // Calculate middle point - the middle key will be promoted
    uint32_t mid = temp_node.num_keys / 2;
    uint64_t promoted_key = temp_node.keys[mid];
    
    if (verbose_level >= 2) {
        out.output("      Split point: %u, promoted key: %lu\n", mid, promoted_key);
    }
    
    // Create right sibling internal node
    next_node_id++;  // Increment node counter
    uint64_t right_internal_address = allocate_node_address();
    BTreeNode right_internal(btree_fanout);
    right_internal.is_leaf = false;
    right_internal.node_address = right_internal_address;
    right_internal.num_keys = 0;
    
    // Left internal keeps original address
    BTreeNode& left_internal = node;
    
    // Redistribute keys and children
    // Left gets: keys[0..mid-1], children[0..mid]
    // Note: keys[mid] is the promoted key that goes to parent (not stored in left or right)
    left_internal.num_keys = mid;
    for (uint32_t i = 0; i < mid; i++) {
        left_internal.keys[i] = temp_node.keys[i];
    }
    for (uint32_t i = 0; i <= mid; i++) {
        left_internal.children[i] = temp_node.children[i];
    }
    
    // Right gets: keys[mid+1..n], children[mid+1..n+1]
    for (uint32_t i = mid + 1; i < temp_node.num_keys; i++) {
        right_internal.keys[right_internal.num_keys] = temp_node.keys[i];
        right_internal.num_keys++;
    }
    
    // Copy children to right node (children[mid+1] onwards)
    for (uint32_t i = 0; i <= right_internal.num_keys; i++) {
        right_internal.children[i] = temp_node.children[mid + 1 + i];
    }
    
    if (verbose_level >= 1) {
        out.output("      Left internal: %u keys, Right internal: %u keys, Promoted: %lu\n",
                  left_internal.num_keys, right_internal.num_keys, promoted_key);
    }
    
    // Store split information - this will propagate up
    op.old_node = left_internal;
    op.new_node = right_internal;
    op.separator_key = promoted_key;
    op.split_phase = AsyncOperation::WRITE_OLD_NODE;
    
    // Write both internal nodes
    write_split_nodes(req_id, op);
}

void ComputeServer::handle_root_split(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op) {
    
    // Calculate new tree height
    uint32_t new_tree_height = op.tree_height + 1;
    
    if (verbose_level >= 1) {
        out.output("   SPLIT: Creating new root, tree height %u -> %u\n",
                  op.tree_height, new_tree_height);
    }
    
    // Create new root node (internal node)
    next_node_id++;  // Increment node counter
    uint64_t new_root_address = allocate_node_address();
    BTreeNode new_root(btree_fanout);
    new_root.is_leaf = false;
    new_root.node_address = new_root_address;
    new_root.num_keys = 1;
    
    // New root has one key (separator) and two children (old root and new sibling)
    new_root.keys[0] = op.separator_key;
    new_root.children[0] = op.old_node.node_address;  // Left child (old root)
    new_root.children[1] = op.new_node.node_address;  // Right child (new sibling)
    
    if (verbose_level >= 1) {
        out.output("      New root: 0x%lx, key=%lu, left=0x%lx, right=0x%lx\n",
                  new_root_address, op.separator_key, 
                  op.old_node.node_address, op.new_node.node_address);
    }
    
    // Write the new root
    auto data = serializer->serialize(new_root);
    auto write_req = new SST::Interfaces::StandardMem::Write(
        new_root_address + LOCK_HEADER_SIZE,  // Write after lock header
        data.size(),
        data
    );
    
    SST::Interfaces::StandardMem::Request::id_t write_id = write_req->getID();
    pending_ops[write_id] = op;
    pending_ops[write_id].waiting_for_write = true;
    pending_ops[write_id].is_root_split = true;
    pending_ops[write_id].split_phase = AsyncOperation::NONE;
    
    // Store new root address and height temporarily in operation
    // These will be written to ROOT_METADATA_ADDRESS after the new root node write completes
    pending_ops[write_id].parent_address = new_root_address;
    pending_ops[write_id].tree_height = new_tree_height;  // Store new height
    
    SST::Interfaces::StandardMem* interface = get_interface_for_address(new_root_address);
    interface->send(write_req);
    stat_network_writes->addData(1);
    
    pending_ops.erase(req_id);
}

void ComputeServer::write_parent_and_complete(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op,
    BTreeNode& parent) {
    
    if (verbose_level >= 2) {
        out.output("      Writing updated parent 0x%lx to memory\n", parent.node_address);
    }
    
    // Serialize the modified parent
    auto data = serializer->serialize(parent);
    
    // Create write request - write after lock header
    // Create write request
    auto write_req = new SST::Interfaces::StandardMem::Write(
        parent.node_address + LOCK_HEADER_SIZE,
        data.size(),
        data
    );
    
    // Store operation info so we can complete it when write finishes
    SST::Interfaces::StandardMem::Request::id_t write_id = write_req->getID();
    pending_ops[write_id] = op;
    pending_ops[write_id].waiting_for_write = true;
    pending_ops[write_id].split_phase = AsyncOperation::NONE;  // Split complete after this write
    
    // Send write to appropriate memory server
    SST::Interfaces::StandardMem* interface = get_interface_for_address(parent.node_address);
    interface->send(write_req);
    stat_network_writes->addData(1);
    
    // Remove the original request
    pending_ops.erase(req_id);
}

void ComputeServer::handle_leaf_split(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op,
    BTreeNode& node) {
    
    assert(node.is_leaf && "handle_leaf_split called on non-leaf node");
    assert(node.num_keys >= btree_fanout - 1 && "handle_leaf_split called on non-full leaf");
    
    if (verbose_level >= 1) {
        out.output("   SPLIT: Leaf 0x%lx is full (%u keys), splitting...\n", 
                  node.node_address, node.num_keys);
    }
    
    // First, insert the new key into a temporary copy to get balanced split
    // This is better than: split then insert, which can lead to imbalanced nodes
    BTreeNode temp_node = node;
    insert_into_leaf(temp_node, op.key, op.value);
    
    // Now split the temporary node (which has fanout keys after insertion)
    // Calculate split point (middle) - this gives us a balanced split
    uint32_t split_point = temp_node.num_keys / 2;  // Left gets smaller half if odd
    
    // Create right sibling leaf (new node)
    next_node_id++;  // Increment node counter
    uint64_t right_leaf_address = allocate_node_address();
    BTreeNode right_leaf(btree_fanout);
    right_leaf.is_leaf = true;
    right_leaf.node_address = right_leaf_address;
    right_leaf.num_keys = 0;
    
    // Left leaf keeps original address, will be modified in place
    BTreeNode& left_leaf = node;
    
    if (verbose_level >= 2) {
        out.output("      Split point: %u (after inserting new key), left will have %u keys, right will have %u keys\n",
                  split_point, split_point, (temp_node.num_keys - split_point));
    }
    
    // Redistribute keys from temp_node
    // Left gets: keys[0..split_point-1]
    left_leaf.num_keys = split_point;
    for (uint32_t i = 0; i < split_point; i++) {
        left_leaf.keys[i] = temp_node.keys[i];
        left_leaf.values[i] = temp_node.values[i];
    }
    
    // Right gets: keys[split_point..n]
    for (uint32_t i = split_point; i < temp_node.num_keys; i++) {
        right_leaf.keys[right_leaf.num_keys] = temp_node.keys[i];
        right_leaf.values[right_leaf.num_keys] = temp_node.values[i];
        right_leaf.num_keys++;
    }
    
    // Update sibling pointers (leaves form linked list)
    right_leaf.next_leaf = left_leaf.next_leaf;
    left_leaf.next_leaf = right_leaf_address;
    
    // Determine separator key (smallest key in right leaf)
    uint64_t separator_key = right_leaf.keys[0];
    
    if (verbose_level >= 1) {
        out.output("      Left leaf: %u keys, Right leaf: %u keys, Separator: %lu (balanced split)\n",
                  left_leaf.num_keys, right_leaf.num_keys, separator_key);
    }
    
    // Store split information in operation for parent update
    op.split_phase = AsyncOperation::WRITE_OLD_NODE;
    op.old_node = left_leaf;
    op.new_node = right_leaf;
    op.separator_key = separator_key;
    
    // Write both leaves to memory (left first, then right)
    write_split_nodes(req_id, op);
}

void ComputeServer::write_split_nodes(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op) {
    
    if (op.split_phase == AsyncOperation::WRITE_OLD_NODE) {
        // Phase 1: Write left leaf (old node)
        auto data = serializer->serialize(op.old_node);
        auto write_req = new SST::Interfaces::StandardMem::Write(
            op.old_node.node_address + LOCK_HEADER_SIZE,  // Write after lock header
            data.size(), 
            data
        );
        
        SST::Interfaces::StandardMem::Request::id_t write_id = write_req->getID();
        pending_ops[write_id] = op;
        pending_ops[write_id].waiting_for_write = true;
        
        SST::Interfaces::StandardMem* interface = get_interface_for_address(op.old_node.node_address);
        interface->send(write_req);
        stat_network_writes->addData(1);
        
        if (verbose_level >= 2) {
            out.output("      Wrote left leaf 0x%lx\n", op.old_node.node_address);
        }
        
        pending_ops.erase(req_id);
        
    } else if (op.split_phase == AsyncOperation::WRITE_NEW_NODE) {
        // Phase 2: Write right leaf (new sibling)
        auto data = serializer->serialize(op.new_node);
        auto write_req = new SST::Interfaces::StandardMem::Write(
            op.new_node.node_address + LOCK_HEADER_SIZE,  // Write after lock header
            data.size(), 
            data
        );
        
        SST::Interfaces::StandardMem::Request::id_t write_id = write_req->getID();
        pending_ops[write_id] = op;
        pending_ops[write_id].waiting_for_write = true;
        
        SST::Interfaces::StandardMem* interface = get_interface_for_address(op.new_node.node_address);
        interface->send(write_req);
        stat_network_writes->addData(1);
        
        if (verbose_level >= 2) {
            out.output("      Wrote right leaf 0x%lx\n", op.new_node.node_address);
        }
        
        pending_ops.erase(req_id);
    }
}

void ComputeServer::update_parent_after_split(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op) {
    
    if (verbose_level >= 2) {
        out.output("      Updating parent with separator_key=%lu, right_child=0x%lx\n",
                  op.separator_key, op.new_node.node_address);
    }
    
    // Remove the split node from path first
    // Path structure before pop: [..., parent, split_node]  (or just [root] if splitting root)
    // Path structure after pop:  [..., parent]              (or empty [] if split root)
    op.path.pop_back();
    
    // Check if path is now empty - this means we split the root
    if (op.path.empty()) {
        // Root split - after removing the split node, path is empty (no parent exists)
        if (verbose_level >= 1) {
            out.output("   SPLIT: Root node split detected, creating new root\n");
        }
        
        // Phase 5: Handle root split
        handle_root_split(req_id, op);
        return;
    }
    
    // Get parent node (now the last element in path after removing split node)
    BTreeNode& parent = op.path.back();
    
    if (verbose_level >= 2) {
        out.output("      Parent node 0x%lx has %u keys (max %u)\n",
                  parent.node_address, parent.num_keys, btree_fanout - 1);
    }
    
    // Check if parent has space for new entry
    if (parent.num_keys >= btree_fanout - 1) {
        // Parent is also full - needs recursive split
        if (verbose_level >= 1) {
            out.output("   SPLIT: Parent is full, splitting internal node recursively\n");
        }
        
        // Phase 5: Split the internal node
        handle_internal_split(req_id, op, parent);
        return;
    }
    
    // Parent has space - insert the separator key
    insert_into_internal_node(parent, op.separator_key, op.new_node.node_address);
    
    // Write updated parent back to memory
    write_parent_and_complete(req_id, op, parent);
}

void ComputeServer::write_leaf_and_complete(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op,
    BTreeNode& leaf) {
    
    // Serialize the modified leaf
    auto data = serializer->serialize(leaf);
    
    // Create write request - write at address + LOCK_HEADER_SIZE (after lock header)
    auto write_req = new SST::Interfaces::StandardMem::Write(
        leaf.node_address + LOCK_HEADER_SIZE, 
        data.size(), 
        data
    );
    
    // Store operation info so we can complete it when write finishes
    SST::Interfaces::StandardMem::Request::id_t write_id = write_req->getID();
    pending_ops[write_id] = op;  // Copy operation to track write completion
    pending_ops[write_id].waiting_for_write = true;
    
    // Send write to appropriate memory server
    SST::Interfaces::StandardMem* interface = get_interface_for_address(leaf.node_address);
    interface->send(write_req);
    stat_network_writes->addData(1);
    
    if (verbose_level >= 2) {
        out.output("      Wrote leaf 0x%lx to memory, waiting for write response\n", 
                  leaf.node_address);
    }
    
    // Remove the original read request (write request will complete the operation)
    pending_ops.erase(req_id);
}

SST::Interfaces::StandardMem* ComputeServer::get_interface_for_address(uint64_t address) {
    // Calculate which memory server owns this address
    // Memory layout: Each server owns ADDRESS_SPACE_PER_SERVER bytes
    //   Server 0: [MEMORY_BASE_ADDRESS, MEMORY_BASE_ADDRESS + ADDRESS_SPACE_PER_SERVER)
    //   Server 1: [MEMORY_BASE_ADDRESS + ADDRESS_SPACE_PER_SERVER, ...)
    //   etc.
    uint64_t server_id = GET_MEMORY_SERVER(address);
    
    // Validate server_id is within range
    if (server_id >= memory_interfaces.size()) {
        out.fatal(CALL_INFO, -1, 
                 "FATAL: Address 0x%lx maps to memory server %lu, but only %zu servers exist!\n"
                 "Address space per server: 0x%lx (%lu bytes)\n"
                 "This indicates an address calculation error.\n",
                 address, server_id, memory_interfaces.size(),
                 MemoryServer::ADDRESS_SPACE_PER_SERVER, MemoryServer::ADDRESS_SPACE_PER_SERVER);
    }
    
    return memory_interfaces[server_id];
}

uint64_t ComputeServer::allocate_node_address() {
    // Use round-robin memory server selection
    uint32_t memory_server_id = current_memory_server;
    
    // Calculate how many nodes fit in a chunk
    // Each node occupies: LOCK_HEADER_SIZE (8 bytes) + serialized node data
    size_t node_size = get_serialized_node_size();
    size_t total_node_size = LOCK_HEADER_SIZE + node_size;
    uint32_t max_nodes_per_chunk = MemoryServer::CHUNK_SIZE / total_node_size;
    const uint32_t PREALLOCATE_THRESHOLD = 10;  // Request new chunk when 10 nodes left
    
    // Check if we have any chunks for this memory server
    // Note: This should only be true if the initial chunk allocation is still pending
    // or if there was a failure in the chunk allocation system
    if (allocated_chunks.find(memory_server_id) == allocated_chunks.end() ||
        allocated_chunks[memory_server_id].empty()) {
        
        out.output("⚠️ Compute %d: No chunks available for server %u yet!\n",
                  node_id, memory_server_id);
        out.fatal(CALL_INFO, -1, 
                 "FATAL: No chunks available for memory server %u.\n"
                 "This should not happen since setup() requests initial chunk.\n"
                 "Possible causes:\n"
                 "  1. Chunk allocation response not received yet (timing issue)\n"
                 "  2. Chunk allocation failed\n"
                 "  3. Memory server is full\n",
                 memory_server_id);
        assert(false && "No chunks available for memory server");
    }
    
    // Get the most recent chunk (last in vector)
    std::vector<ChunkInfo>& chunks = allocated_chunks[memory_server_id];
    ChunkInfo& current_chunk = chunks.back();
    
    // Check if current chunk is completely full
    if (current_chunk.nodes_used >= max_nodes_per_chunk) {
        if (verbose_level >= 1) {
            out.output("⚠️ Compute %d: Chunk %u FULL (%u/%u nodes), moving to next memory server\n",
                      node_id, current_chunk.chunk_id, current_chunk.nodes_used, max_nodes_per_chunk);
        }
        
        // Move to next memory server in round-robin fashion
        current_memory_server = (current_memory_server + 1) % num_memory_nodes;
        memory_server_id = current_memory_server;
        
        if (verbose_level >= 1) {
            out.output("   Round-robin: Now using memory server %u\n", memory_server_id);
        }
        
        // Get chunks from new memory server
        if (allocated_chunks.find(memory_server_id) == allocated_chunks.end() ||
            allocated_chunks[memory_server_id].empty()) {
            out.output("⚠️ Compute %d: Memory server %u has no chunks available!\n",
                      node_id, memory_server_id);
            out.fatal(CALL_INFO, -1, 
                     "FATAL: Next memory server %u has no chunks.\n"
                     "Pre-allocation of next chunk should have happened earlier!\n",
                     memory_server_id);
            assert(false && "No chunks available for next memory server");
        }
        
        // Use the most recent chunk from new server
        chunks = allocated_chunks[memory_server_id];
        current_chunk = chunks.back();
    }
    
    // Pre-allocate next chunk when current chunk is almost full (10 nodes left)
    uint32_t nodes_remaining = max_nodes_per_chunk - current_chunk.nodes_used;
    if (nodes_remaining == PREALLOCATE_THRESHOLD) {
        // Calculate which memory server will be next
        uint32_t next_memory_server = (current_memory_server + 1) % num_memory_nodes;
        
        if (verbose_level >= 1) {
            out.output("📦 Compute %d: Only %u nodes left in current chunk, pre-allocating from server %u\n",
                      node_id, nodes_remaining, next_memory_server);
        }
        
        // Request chunk from next memory server (async, won't block)
        request_chunk_allocation(next_memory_server);
    }
    
    // Allocate from current chunk
    // Each node occupies: LOCK_HEADER_SIZE + node_size
    uint64_t node_address = current_chunk.chunk_address + (current_chunk.nodes_used * total_node_size);
    current_chunk.nodes_used++;
    next_node_id++;  // Track total nodes allocated (for statistics)
    
    if (verbose_level >= 3) {
        out.output("✅ Compute %d: Allocated 0x%lx from chunk %u (server %u, %u/%u nodes used, node_size=%zu+%d=%zu bytes)\n",
                  node_id, node_address, current_chunk.chunk_id, memory_server_id, 
                  current_chunk.nodes_used, max_nodes_per_chunk, node_size, LOCK_HEADER_SIZE, total_node_size);
    }
    
    return node_address;
}

void ComputeServer::initialize_btree() {
    // Only node_id 0 initializes the B+tree to avoid conflicts
    if (node_id != 0) {
        out.output("Node %d: Skipping tree initialization (only node 0 initializes), will check validity bit\n", node_id);
        // Non-initializing nodes will check validity bit in tick()
        return;
    }
    
    out.output("Node %d: Initializing B+tree with root metadata at 0x%llx\n", node_id, (unsigned long long)ROOT_METADATA_ADDRESS);
    
    // Step 1: Write validity_bit = 0 (tree not ready)
    std::vector<uint8_t> invalid_bit = {0};
    auto validity_req1 = new SST::Interfaces::StandardMem::Write(VALIDITY_BIT_ADDRESS, 1, invalid_bit);
    SST::Interfaces::StandardMem* validity_interface = get_interface_for_address(VALIDITY_BIT_ADDRESS);
    validity_interface->send(validity_req1);
    out.output("Node %d: Wrote validity_bit=0 (tree not ready)\n", node_id);
    
    // Step 2: Allocate and write the initial root node
    // Initial root is a leaf node (tree height = 1)
    uint64_t initial_root_address = allocate_node_address();
    BTreeNode root(btree_fanout);
    root.is_leaf = true;
    root.num_keys = 0;
    root.node_address = initial_root_address;
    
    auto data = serializer->serialize(root);
    
    // IMPORTANT: The lock system reads node data from address + LOCK_HEADER_SIZE (after lock header)
    // So we must write the node data at root_address + LOCK_HEADER_SIZE
    // The first LOCK_HEADER_SIZE bytes (lock header) are initialized to zero by the memory server
    auto root_req = new SST::Interfaces::StandardMem::Write(initial_root_address + LOCK_HEADER_SIZE, data.size(), data);
    
    // Track this write request so we know when to write metadata
    AsyncOperation init_op;
    init_op.type = AsyncOperation::INIT_WRITE;
    init_op.waiting_for_write = true;  // Changed to true - we're waiting for this write
    init_op.parent_address = initial_root_address;  // Store root address for metadata write
    init_op.tree_height = 1;  // Initial tree height
    pending_ops[root_req->getID()] = init_op;
    
    SST::Interfaces::StandardMem* target_interface = get_interface_for_address(initial_root_address);
    target_interface->send(root_req);
    
    out.output("Node %d: Wrote initial root node at address 0x%lx (height=1)\n", node_id, initial_root_address);
    
    // Step 3: Write root metadata - this will be done in handle_write_response
    // after the root node write completes
    // Step 4: Write validity_bit = 1 - this will be done after metadata write completes
}

void ComputeServer::check_tree_initialization() {
    // Non-initializing nodes check the validity bit to see if Node 0 has completed initialization
    if (checking_validity_bit) {
        // Already waiting for a read response
        return;
    }
    
    out.output("Node %d: Checking validity bit at address 0x%llx\n", node_id, VALIDITY_BIT_ADDRESS);
    
    // Read the validity bit
    auto req = new SST::Interfaces::StandardMem::Read(VALIDITY_BIT_ADDRESS, 1);
    
    AsyncOperation check_op;
    check_op.type = AsyncOperation::VALIDITY_CHECK;
    check_op.waiting_for_write = false;
    pending_ops[req->getID()] = check_op;
    
    SST::Interfaces::StandardMem* validity_interface = get_interface_for_address(VALIDITY_BIT_ADDRESS);
    validity_interface->send(req);
    
    checking_validity_bit = true;
}

void ComputeServer::handle_validity_check_response(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    SST::Interfaces::StandardMem::ReadResp* resp) {
    
    if (!pending_ops.count(req_id)) {
        out.output("WARNING: Received validity check response for unknown request ID %lu\n", req_id);
        return;
    }
    
    checking_validity_bit = false;
    uint8_t validity_bit = resp->data[0];
    
    if (validity_bit == 1) {
        out.output("Node %d: Validity bit is 1, tree is ready!\n", node_id);
        tree_initialized = true;
    } else {
        out.output("Node %d: Validity bit is 0, tree not ready yet, will retry\n", node_id);
        // Will check again in next tick
    }
    
    // Remove operation from pending
    pending_ops.erase(req_id);
}

void ComputeServer::process_btree_operation(const WorkloadOp& op) {
    if (op.op_type == BTREE_SEARCH) {
        btree_search_async(op.key);
    } else if (op.op_type == BTREE_INSERT) {
        btree_insert_async(op.key, op.value);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// ROOT METADATA MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> ComputeServer::serialize_root_metadata(const RootMetadata& metadata) {
    std::vector<uint8_t> data(16, 0);  // 8 (root_address) + 4 (tree_height) + 4 (reserved)
    
    // Serialize root_address (8 bytes)
    std::memcpy(data.data(), &metadata.root_address, sizeof(uint64_t));
    
    // Serialize tree_height (4 bytes)
    std::memcpy(data.data() + 8, &metadata.tree_height, sizeof(uint32_t));
    
    // Serialize reserved (4 bytes) - all zeros
    std::memcpy(data.data() + 12, &metadata.reserved, sizeof(uint32_t));
    
    return data;
}

ComputeServer::RootMetadata ComputeServer::deserialize_root_metadata(const std::vector<uint8_t>& data) {
    RootMetadata metadata;
    
    if (data.size() < 16) {
        out.output("ERROR: Root metadata too small: %zu bytes\n", data.size());
        return metadata;  // Return default
    }
    
    // Debug: print first 16 bytes of data
    if (verbose_level >= 2) {
        out.output("DEBUG: First 16 bytes of metadata: ");
        for (size_t i = 0; i < std::min(size_t(16), data.size()); i++) {
            out.output("%02x ", data[i]);
        }
        out.output("\n");
    }
    
    // Deserialize root_address (8 bytes)
    std::memcpy(&metadata.root_address, data.data(), sizeof(uint64_t));
    
    // Deserialize tree_height (4 bytes)
    std::memcpy(&metadata.tree_height, data.data() + 8, sizeof(uint32_t));
    
    // Deserialize reserved (4 bytes)
    std::memcpy(&metadata.reserved, data.data() + 12, sizeof(uint32_t));
    
    if (verbose_level >= 3) {
        out.output("Deserialized root metadata: root=0x%lx, height=%u\n",
                  metadata.root_address, metadata.tree_height);
    }
    
    return metadata;
}

void ComputeServer::read_root_metadata_async(AsyncOperation& op) {
    // Read root metadata with lock (LL/SC protocol)
    // - SHARED lock for optimistic operations (reads only)
    // - EXCLUSIVE lock for pessimistic operations (may need to update metadata on root split)
    
    if (verbose_level >= 2) {
        out.output("Reading root metadata from 0x%llx (pessimistic_mode=%d)\n", 
                  (unsigned long long)ROOT_METADATA_ADDRESS, op.pessimistic_mode);
    }
    
    // Use lock manager to acquire lock on metadata
    // Lock manager will handle the LL/SC protocol
    op.lock_target_address = ROOT_METADATA_ADDRESS;
    
    // CRITICAL: Use EXCLUSIVE lock if in pessimistic mode (operation may split root and update metadata)
    // Use SHARED lock for optimistic mode (read-only access to metadata)
    op.need_exclusive_lock = op.pessimistic_mode;  
    
    // Get the interface for the root metadata address
    auto interface = get_interface_for_address(ROOT_METADATA_ADDRESS);
    
    // Try to acquire lock on root metadata node (shared or exclusive based on pessimistic_mode)
    lock_manager->try_acquire_lock_async(op, ROOT_METADATA_ADDRESS, op.pessimistic_mode, interface, pending_ops);
}

void ComputeServer::handle_root_metadata_response(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    const std::vector<uint8_t>& data) {
    
    if (!pending_ops.count(req_id)) return;
    
    auto& op = pending_ops[req_id];
    
    // Deserialize root metadata
    RootMetadata metadata = deserialize_root_metadata(data);
    
    if (verbose_level >= 2) {
        out.output("Root metadata read: root=0x%lx, height=%u\n",
                  metadata.root_address, metadata.tree_height);
    }
    
    // Store in operation for use during traversal
    op.current_address = metadata.root_address;
    op.current_level = 0;
    op.tree_height = metadata.tree_height;  // Store tree height in operation
    
    // Set the real operation type from intended_operation_type
    op.type = op.intended_operation_type;
    
    // Note: pessimistic_mode is already set correctly:
    // - false for new operations (default from constructor)
    // - true for restarted operations (set in handle_restart_after_lock_release)
    
    if (verbose_level >= 2) {
        out.output("DEBUG: After metadata read - type=%s, pessimistic_mode=%d\n",
                  (op.type == AsyncOperation::SEARCH) ? "SEARCH" : "INSERT",
                  op.pessimistic_mode);
    }
    
    // Continue with the actual B+tree operation
    // Delegate to btree_ops which will continue traversal
    auto interface_getter = [this](uint64_t addr) { return get_interface_for_address(addr); };
    
    if (op.type == AsyncOperation::SEARCH) {
        btree_ops->btree_search_async(op.key, op, pending_ops, lock_manager, interface_getter, stat_network_reads);
    } else if (op.type == AsyncOperation::INSERT) {
        btree_ops->btree_insert_async(op.key, op.value, op, pending_ops, lock_manager, interface_getter, stat_network_reads);
    }
    
    // Remove the metadata read operation (new operation created above)
    pending_ops.erase(req_id);
}

void ComputeServer::handle_btree_initialization_write(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op) {
    
    // Check if this is the root node write or metadata write
    if (op.current_address == ROOT_METADATA_ADDRESS) {
        // Step 4: Metadata write complete - set validity bit
        out.output("Node %d: Root metadata write complete, setting validity_bit=1\n", node_id);
        
        std::vector<uint8_t> valid_bit = {1};
        auto validity_req = new SST::Interfaces::StandardMem::Write(VALIDITY_BIT_ADDRESS, 1, valid_bit);
        SST::Interfaces::StandardMem* validity_interface = get_interface_for_address(VALIDITY_BIT_ADDRESS);
        validity_interface->send(validity_req);
        
        out.output("Node %d: B+tree initialization complete, validity_bit=1, tree ready\n", node_id);
        tree_initialized = true;
        pending_ops.erase(req_id);
        
    } else {
        // Step 3: Root node write complete - write metadata
        out.output("Node %d: Root node write complete, writing root metadata\n", node_id);
        
        RootMetadata initial_metadata;
        initial_metadata.root_address = op.parent_address;  // Stored from initialize_btree()
        initial_metadata.tree_height = op.tree_height;      // Stored from initialize_btree()
        initial_metadata.reserved = 0;
        
        auto metadata_data = serialize_root_metadata(initial_metadata);
        
        // Pad metadata to full node size (get_serialized_node_size())
        // This ensures the memory block has the expected size when read back
        size_t full_node_size = get_serialized_node_size();
        if (metadata_data.size() < full_node_size) {
            metadata_data.resize(full_node_size, 0);  // Pad with zeros
        }
        
        // Debug: print what we're writing
        if (verbose_level >= 2) {
            out.output("DEBUG: Writing metadata: root=0x%lx, height=%u, padded to %zu bytes\n",
                      initial_metadata.root_address, initial_metadata.tree_height, metadata_data.size());
            out.output("       First 16 bytes: ");
            for (size_t i = 0; i < std::min(size_t(16), metadata_data.size()); i++) {
                out.output("%02x ", metadata_data[i]);
            }
            out.output("\n");
        }
        
        auto metadata_req = new SST::Interfaces::StandardMem::Write(
            ROOT_METADATA_ADDRESS + LOCK_HEADER_SIZE,
            metadata_data.size(),
            metadata_data
        );
        
        // Track this write - we'll set validity bit after metadata is written
        AsyncOperation metadata_op;
        metadata_op.type = AsyncOperation::INIT_WRITE;
        metadata_op.waiting_for_write = true;
        metadata_op.current_address = ROOT_METADATA_ADDRESS;  // Mark as metadata write
        pending_ops[metadata_req->getID()] = metadata_op;
        
        SST::Interfaces::StandardMem* metadata_interface = get_interface_for_address(ROOT_METADATA_ADDRESS);
        metadata_interface->send(metadata_req);
        stat_network_writes->addData(1);
        
        out.output("Node %d: Wrote root metadata (root=0x%lx, height=%u)\n", 
                  node_id, initial_metadata.root_address, initial_metadata.tree_height);
        
        pending_ops.erase(req_id);
    }
}

void ComputeServer::update_root_metadata_async(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    AsyncOperation& op,
    uint64_t new_root_address,
    uint32_t new_tree_height) {
    
    // Update root metadata with EXCLUSIVE lock (LL/SC protocol)
    // This is called during root splits
    
    if (verbose_level >= 1) {
        out.output("Updating root metadata: new_root=0x%lx, new_height=%u\n",
                  new_root_address, new_tree_height);
    }
    
    // Create new metadata
    RootMetadata new_metadata;
    new_metadata.root_address = new_root_address;
    new_metadata.tree_height = new_tree_height;
    
    // Serialize metadata
    auto metadata_data = serialize_root_metadata(new_metadata);
    
    // Pad metadata to full node size for consistency
    size_t full_node_size = get_serialized_node_size();
    if (metadata_data.size() < full_node_size) {
        metadata_data.resize(full_node_size, 0);  // Pad with zeros
    }
    
    // Write to root metadata address (after lock header)
    auto write_req = new SST::Interfaces::StandardMem::Write(
        ROOT_METADATA_ADDRESS + LOCK_HEADER_SIZE,
        metadata_data.size(),
        metadata_data
    );
    
    SST::Interfaces::StandardMem::Request::id_t write_id = write_req->getID();
    pending_ops[write_id] = op;
    pending_ops[write_id].waiting_for_write = true;
    // Mark this as a metadata write (only operations with this address are metadata updates)
    pending_ops[write_id].current_address = ROOT_METADATA_ADDRESS;
    
    SST::Interfaces::StandardMem* interface = get_interface_for_address(ROOT_METADATA_ADDRESS);
    interface->send(write_req);
    stat_network_writes->addData(1);
    
    if (verbose_level >= 2) {
        out.output("Sent root metadata update request\n");
    }
    
    // Remove old operation
    pending_ops.erase(req_id);
}

// ═══════════════════════════════════════════════════════════════════════════
// CHUNK ALLOCATION VIA MAGIC ADDRESS PROTOCOL
// ═══════════════════════════════════════════════════════════════════════════

void ComputeServer::request_chunk_allocation(uint32_t memory_server_id) {
    if (memory_server_id >= num_memory_nodes) {
        out.output("ERROR: Invalid memory_server_id %u (max %u)\n", 
                  memory_server_id, num_memory_nodes - 1);
        return;
    }
    
    if (verbose_level >= 1) {
        out.output("📦 Compute %d → Memory %d: REQUEST_CHUNK (magic address protocol)\n",
                  node_id, memory_server_id);
    }
    
    // Construct magic address: MAGIC_ALLOCATE_CHUNK_BASE | memory_server_id
    uint64_t magic_address = MAGIC_ALLOCATE_CHUNK_BASE | memory_server_id;
    
    // Create READ request to magic address
    // Request 12 bytes: [chunk_id (4 bytes)] [chunk_address (8 bytes)]
    auto read_req = new SST::Interfaces::StandardMem::Read(magic_address, 12);
    
    // Create AsyncOperation to track this request
    AsyncOperation chunk_op;
    chunk_op.type = AsyncOperation::CHUNK_ALLOCATE;  // ← Properly set the type!
    chunk_op.chunk_allocation_complete = false;
    chunk_op.chunk_allocation_failed = false;
    chunk_op.memory_server_id = memory_server_id;    // ← Store which memory server
    chunk_op.start_time = getCurrentSimTime();
    
    SST::Interfaces::StandardMem::Request::id_t req_id = read_req->getID();
    pending_ops[req_id] = chunk_op;
    
    // Send to the appropriate memory server interface
    SST::Interfaces::StandardMem* interface = memory_interfaces[memory_server_id];
    interface->send(read_req);
    
    if (verbose_level >= 2) {
        out.output("   Sent READ to magic address 0x%lx (req_id=%lu)\n", 
                  magic_address, req_id);
    }
}

void ComputeServer::handle_chunk_allocation_response(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    SST::Interfaces::StandardMem::ReadResp* resp) {
    
    if (!pending_ops.count(req_id)) {
        out.output("WARNING: Received chunk allocation response for unknown request ID %lu\n", req_id);
        return;
    }
    
    auto& op = pending_ops[req_id];
    
    // Verify this is a chunk allocation operation
    if (op.type != AsyncOperation::CHUNK_ALLOCATE) {
        out.output("ERROR: handle_chunk_allocation_response called for non-CHUNK_ALLOCATE operation (type=%d)\n", 
                  op.type);
        return;
    }
    
    // Parse response: [chunk_id (4 bytes)] [chunk_address (8 bytes)]
    if (resp->data.size() < 4) {
        out.output("ERROR: Chunk allocation response has insufficient data (%zu bytes)\n", 
                  resp->data.size());
        op.chunk_allocation_failed = true;
        pending_ops.erase(req_id);
        return;
    }
    
    uint32_t chunk_id;
    memcpy(&chunk_id, resp->data.data(), sizeof(uint32_t));
    
    if (chunk_id == 0xFFFFFFFF) {
        // Allocation failed
        out.output("❌ Compute %d: Chunk allocation FAILED from memory server\n", node_id);
        op.chunk_allocation_failed = true;
    } else {
        // Success! Extract chunk address from response
        uint64_t chunk_address = 0;
        assert(resp->data.size() == 12 && "Chunk allocation response too small for address");
        memcpy(&chunk_address, resp->data.data() + 4, sizeof(uint64_t));

        out.output("✅ Compute %d: Chunk allocation SUCCESS\n", node_id);
        out.output("   chunk_id=%u, address=0x%lx\n", chunk_id, chunk_address);
        
        op.chunk_allocation_complete = true;
        op.allocated_chunk_id = chunk_id;
        op.allocated_chunk_address = chunk_address;
        
        // Record latency
        SimTime_t latency = getCurrentSimTime() - op.start_time;
        if (verbose_level >= 2) {
            out.output("   Allocation latency: %lu ns\n", latency);
        }
        
        // **PERSIST THE CHUNK INFO**
        // Add this chunk to the list of chunks for this memory server
        uint32_t memory_server_id = op.memory_server_id;
        allocated_chunks[memory_server_id].push_back(ChunkInfo(chunk_id, chunk_address));
        
        size_t num_chunks = allocated_chunks[memory_server_id].size();
        out.output("   Added chunk to memory server %u (now has %zu chunks)\n", 
                  memory_server_id, num_chunks);
    }
    
    // Remove operation from pending
    pending_ops.erase(req_id);
}
