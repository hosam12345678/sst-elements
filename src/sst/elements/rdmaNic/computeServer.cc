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
#include "workload/workload_generator.h"
#include <cassert>

using namespace SST;
using namespace SST::MemHierarchy;

// Memory address macros for disaggregated memory
#define MEMORY_BASE_ADDRESS 0x10000000ULL
#define MEMORY_SERVER_SIZE 0x1000000ULL
#define GET_MEMORY_SERVER(addr) (((addr) - MEMORY_BASE_ADDRESS) / MEMORY_SERVER_SIZE)

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

    // Initialize B+tree state
    tree_height = 1;
    next_node_id = 0;
    root_address = MEMORY_BASE_ADDRESS;

    // Setup debug output
    dbg.init("", 5, 0, (Output::output_location_t)1);
    out.init("ComputeServer[@p:@l]: ", 1, 0, Output::STDOUT);
    
    // Initialize B+tree components
    serializer = new BTreeSerializer(btree_fanout, &out);
    lock_manager = new BTreeLockManager(node_id, verbose_level, &out);
    btree_ops = new BTreeOperations(root_address, btree_fanout, verbose_level, &out);
    
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
    memory_interface->setup();
    
    // Setup all additional interfaces
    for (auto& interface : memory_interfaces) {
        interface->setup();
    }
    
    // Initialize B+tree after address routing is established
    initialize_btree();
}

void ComputeServer::finish() {
    memory_interface->finish();
    
    // Finish all additional interfaces
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
    if (!pending_operations.empty()) {
        if (pending_ops.size() < 10) {
            WorkloadOp next_op = pending_operations.front();
            pending_operations.pop();
            process_btree_operation(next_op);
        }
    }
    
    if (getCurrentSimTime() >= simulation_duration && 
        pending_operations.empty() && pending_ops.empty()) {
        return true;
    }
    
    return false;
}

void ComputeServer::handleMemoryEvent(Interfaces::StandardMem::Request* req) {
    auto req_id = req->getID();
    
    if (auto* read_resp = dynamic_cast<Interfaces::StandardMem::ReadResp*>(req)) {
        handle_read_response(req_id, read_resp->data);
    } else if (dynamic_cast<Interfaces::StandardMem::WriteResp*>(req)) {
        handle_write_response(req_id);
    }
    
    delete req;
}

void ComputeServer::btree_search_async(uint64_t key) {
    AsyncOperation op;
    op.type = AsyncOperation::SEARCH;
    op.key = key;
    op.current_level = 0;
    op.current_address = root_address;
    op.start_time = getCurrentSimTime();
    
    auto interface_getter = [this](uint64_t addr) { return get_interface_for_address(addr); };
    btree_ops->btree_search_async(key, op, pending_ops, lock_manager, interface_getter, stat_network_reads);
}

void ComputeServer::btree_insert_async(uint64_t key, uint64_t value) {
    AsyncOperation op;
    op.type = AsyncOperation::INSERT;
    op.key = key;
    op.value = value;
    op.current_level = 0;
    op.current_address = root_address;
    op.start_time = getCurrentSimTime();
    
    if (verbose_level >= 1) {
        out.output("INSERT key=%lu, value=%lu - starting traversal\n", key, value);
    }
    
    auto interface_getter = [this](uint64_t addr) { return get_interface_for_address(addr); };
    btree_ops->btree_insert_async(key, value, op, pending_ops, lock_manager, interface_getter, stat_network_reads);
}

void ComputeServer::handle_read_response(Interfaces::StandardMem::Request::id_t req_id,
                                         const std::vector<uint8_t>& data) {
    if (!pending_ops.count(req_id)) return;
    
    auto& op = pending_ops[req_id];
    
    // Handle lock acquisition responses
    if (op.waiting_for_lock) {
        lock_manager->handle_lock_response(req_id, data, pending_ops, 
                                          get_interface_for_address(op.lock_target_address),
                                          get_serialized_node_size());
        stat_network_reads->addData(1);
        return;
    }
    
    // Deserialize the node we just read
    BTreeNode node = serializer->deserialize(data);
    node.node_address = op.current_address;
    op.path.push_back(node);
    
    // Check if we've reached a leaf node
    bool at_leaf = (node.is_leaf || op.current_level >= tree_height - 1);
    
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

void ComputeServer::handle_write_response(Interfaces::StandardMem::Request::id_t req_id) {
    if (!pending_ops.count(req_id)) return;
    
    auto& op = pending_ops[req_id];
    
    if (op.waiting_for_write) {
        // Check if this is part of a split operation
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
            
        } else {
            // Simple write (no split) or final write - finish the insert operation
            
            // Check if this was a root split
            if (op.is_root_split) {
                if (verbose_level >= 1) {
                    out.output("   SPLIT: Root split complete, updating tree metadata\n");
                }
                
                // Update root address and tree height
                root_address = op.parent_address;
                tree_height++;
                
                if (verbose_level >= 1) {
                    out.output("   New root: 0x%lx, Tree height: %u\n", root_address, tree_height);
                }
            }
            
            if (verbose_level >= 1) {
                out.output("   INSERT key=%lu - write complete, operation finished\n", op.key);
            }
            
            stat_inserts->addData(1);
            
            // Release all locks held during operation
            auto interface_getter = [this](uint64_t addr) { return get_interface_for_address(addr); };
            lock_manager->release_all_locks(op, interface_getter);
            
            // Record latency and completion
            SimTime_t latency = getCurrentSimTime() - op.start_time;
            stat_total_latency->addData(latency);
            stat_ops_completed->addData(1);
            
            // Remove from pending operations
            pending_ops.erase(req_id);
        }
    }
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
    
    // Remove current request (btree_ops will create new one)
    pending_ops.erase(req_id);
    
    // Delegate back to btree_ops to continue the operation
    auto interface_getter = [this](uint64_t addr) { return get_interface_for_address(addr); };
    
    if (op.type == AsyncOperation::SEARCH) {
        btree_ops->btree_search_async(op.key, op, pending_ops, lock_manager, interface_getter, stat_network_reads);
    } else if (op.type == AsyncOperation::INSERT) {
        btree_ops->btree_insert_async(op.key, op.value, op, pending_ops, lock_manager, interface_getter, stat_network_reads);
    }
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
    
    // Release all locks held during traversal
    auto interface_getter = [this](uint64_t addr) { return get_interface_for_address(addr); };
    lock_manager->release_all_locks(op, interface_getter);
    
    // Record latency and completion
    SimTime_t latency = getCurrentSimTime() - op.start_time;
    stat_total_latency->addData(latency);
    stat_ops_completed->addData(1);
    
    // Operation complete - remove from pending
    pending_ops.erase(req_id);
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
            out.output("   INSERT key=%lu - leaf FULL, initiating split\n", op.key);
        }
        
        // Phase 3: Split the leaf
        handle_leaf_split(req_id, op, node);
        return;
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

void ComputeServer::insert_into_leaf(BTreeNode& leaf, uint64_t key, uint64_t value) {
    // Validate assumptions
    assert(leaf.is_leaf && "insert_into_leaf called on non-leaf node");
    assert(leaf.num_keys < btree_fanout - 1 && "insert_into_leaf called on full leaf");
    
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
    assert(internal.num_keys < btree_fanout - 1 && "insert_into_internal_node called on full node");
    
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
    uint64_t right_internal_address = allocate_node_address(next_node_id++, op.current_level - 1);
    BTreeNode right_internal(btree_fanout);
    right_internal.is_leaf = false;
    right_internal.node_address = right_internal_address;
    right_internal.num_keys = 0;
    
    // Left internal keeps original address
    BTreeNode& left_internal = node;
    
    // Redistribute keys and children
    // Left gets: keys[0..mid-1], children[0..mid]
    left_internal.num_keys = mid;
    
    // Right gets: keys[mid+1..n], children[mid+1..n+1]
    for (uint32_t i = mid + 1; i < temp_node.num_keys; i++) {
        right_internal.keys[right_internal.num_keys] = temp_node.keys[i];
        right_internal.num_keys++;
    }
    
    // Copy children to right node (children[mid+1] onwards)
    for (uint32_t i = 0; i <= right_internal.num_keys; i++) {
        right_internal.children[i] = temp_node.children[mid + 1 + i];
    }
    
    // Update left node's children (keep only children[0..mid])
    // (keys already correct from temp_node copy)
    for (uint32_t i = 0; i <= left_internal.num_keys; i++) {
        left_internal.children[i] = temp_node.children[i];
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
    
    if (verbose_level >= 1) {
        out.output("   SPLIT: Creating new root, tree height %u -> %u\n",
                  tree_height, tree_height + 1);
    }
    
    // Create new root node (internal node)
    uint64_t new_root_address = allocate_node_address(next_node_id++, 0);
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
        new_root_address,
        data.size(),
        data
    );
    
    SST::Interfaces::StandardMem::Request::id_t write_id = write_req->getID();
    pending_ops[write_id] = op;
    pending_ops[write_id].waiting_for_write = true;
    pending_ops[write_id].is_root_split = true;
    pending_ops[write_id].split_phase = AsyncOperation::NONE;
    
    // Store new root address temporarily in operation
    pending_ops[write_id].parent_address = new_root_address;
    
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
    
    // Create write request
    auto write_req = new SST::Interfaces::StandardMem::Write(
        parent.node_address,
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
    
    // Calculate split point (middle)
    uint32_t split_point = (btree_fanout - 1) / 2;  // Left gets smaller half if odd
    
    // Create right sibling leaf (new node)
    uint64_t right_leaf_address = allocate_node_address(next_node_id++, op.current_level);
    BTreeNode right_leaf(btree_fanout);
    right_leaf.is_leaf = true;
    right_leaf.node_address = right_leaf_address;
    right_leaf.num_keys = 0;
    
    // Left leaf keeps original address, will be modified in place
    BTreeNode& left_leaf = node;
    
    if (verbose_level >= 2) {
        out.output("      Split point: %u, left will have %u keys, right will have %u keys\n",
                  split_point, split_point, (node.num_keys - split_point));
    }
    
    // Move upper half of keys to right sibling
    for (uint32_t i = split_point; i < node.num_keys; i++) {
        right_leaf.keys[right_leaf.num_keys] = left_leaf.keys[i];
        right_leaf.values[right_leaf.num_keys] = left_leaf.values[i];
        right_leaf.num_keys++;
    }
    
    // Update left leaf to have only lower half
    left_leaf.num_keys = split_point;
    
    // Update sibling pointers (leaves form linked list)
    right_leaf.next_leaf = left_leaf.next_leaf;
    left_leaf.next_leaf = right_leaf_address;
    
    // Determine separator key (smallest key in right leaf)
    uint64_t separator_key = right_leaf.keys[0];
    
    if (verbose_level >= 1) {
        out.output("      Left leaf: %u keys, Right leaf: %u keys, Separator: %lu\n",
                  left_leaf.num_keys, right_leaf.num_keys, separator_key);
    }
    
    // Now insert the new key into the appropriate leaf
    if (op.key < separator_key) {
        if (verbose_level >= 2) {
            out.output("      Inserting key=%lu into left leaf\n", op.key);
        }
        insert_into_leaf(left_leaf, op.key, op.value);
    } else {
        if (verbose_level >= 2) {
            out.output("      Inserting key=%lu into right leaf\n", op.key);
        }
        insert_into_leaf(right_leaf, op.key, op.value);
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
            op.old_node.node_address, 
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
            op.new_node.node_address, 
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
    
    // Find parent node in the path (one level up from the split node)
    if (op.path.size() < 2) {
        // No parent exists - this was a root split!
        if (verbose_level >= 1) {
            out.output("   SPLIT: Root node split detected, creating new root\n");
        }
        
        // Phase 5: Handle root split
        handle_root_split(req_id, op);
        return;
    }
    
    // Get parent node (second-to-last in path)
    BTreeNode& parent = op.path[op.path.size() - 2];
    
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
    
    // Create write request
    auto write_req = new SST::Interfaces::StandardMem::Write(
        leaf.node_address, 
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
    uint64_t server_id = GET_MEMORY_SERVER(address);
    return memory_interfaces[server_id];
}

uint64_t ComputeServer::allocate_node_address(uint64_t node_id, uint32_t level) {
    uint64_t server_id = node_id % num_memory_nodes;
    uint64_t level_base = level * 0x10000;
    uint64_t node_offset = (node_id / num_memory_nodes) * get_serialized_node_size();
    
    return MEMORY_BASE_ADDRESS + (server_id * MEMORY_SERVER_SIZE) + level_base + node_offset;
}

void ComputeServer::initialize_btree() {
    BTreeNode root(btree_fanout);
    root.is_leaf = true;
    root.num_keys = 0;
    root.node_address = root_address;
    
    auto data = serializer->serialize(root);
    auto req = new SST::Interfaces::StandardMem::Write(root_address, data.size(), data);
    
    SST::Interfaces::StandardMem* target_interface = get_interface_for_address(root_address);
    target_interface->send(req);
}



void ComputeServer::process_btree_operation(const WorkloadOp& op) {
    if (op.op_type == BTREE_SEARCH) {
        btree_search_async(op.key);
    } else if (op.op_type == BTREE_INSERT) {
        btree_insert_async(op.key, op.value);
    }
}
