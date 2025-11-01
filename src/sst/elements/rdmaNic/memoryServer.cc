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
#include "memoryServer.h"
#include <cassert>

using namespace SST;
using namespace SST::MemHierarchy;

MemoryServer::MemoryServer(ComponentId_t id, Params& params) :
    SST::Component(id),
    memory_used(0)
{
    // Parse configuration parameters
    memory_server_id = params.find<uint32_t>("memory_server_id", 0);
    num_compute_nodes = params.find<uint32_t>("num_compute_nodes", 8);
    memory_capacity = params.find<uint64_t>("memory_capacity_gb", 16) * 1024 * 1024 * 1024; // Convert to bytes
    memory_latency = params.find<SimTime_t>("memory_latency_ns", 100);
    btree_node_size = params.find<size_t>("btree_node_size", 4096);
    verbose_level = params.find<int>("verbose", 0);

    // Calculate base address for this memory server - each server gets 16MB address space
    base_address = 0x10000000 + memory_server_id * 0x1000000;  // 16MB per server

    // Setup debug output with maximum verbosity for address visibility
    dbg.init("", 5, 0, (Output::output_location_t)1);  // Force high verbosity
    out.init("MemoryServer[@p:@l]: ", 1, 0, Output::STDOUT);

    // Initialize statistics
    stat_network_reads = registerStatistic<uint64_t>("network_reads_received");
    stat_network_writes = registerStatistic<uint64_t>("network_writes_received");
    stat_memory_reads = registerStatistic<uint64_t>("memory_reads");
    stat_memory_writes = registerStatistic<uint64_t>("memory_writes");
    stat_bytes_read = registerStatistic<uint64_t>("bytes_read");
    stat_bytes_written = registerStatistic<uint64_t>("bytes_written");
    stat_memory_utilization = registerStatistic<uint64_t>("memory_utilization");
    
    // Lock statistics
    stat_locks_acquired = registerStatistic<uint64_t>("locks_acquired");
    stat_locks_released = registerStatistic<uint64_t>("locks_released");
    stat_lock_conflicts = registerStatistic<uint64_t>("lock_conflicts");
    stat_lock_wait_time = registerStatistic<uint64_t>("lock_wait_time");
    
    // Initialize lock counters
    total_locks_acquired = 0;
    total_locks_released = 0;
    total_lock_conflicts = 0;

    // Setup multiple memory interfaces for many-to-many connectivity
    // Create SEPARATE handler for EACH interface so we know which interface to respond through
    out.output("Loading memory interfaces (N interfaces, N handlers approach)...\n");
    
    for (int i = 0; i < num_compute_nodes; i++) {
        std::string interface_name = "mem_interface_" + std::to_string(i);
        
        // Create handler with data parameter for interface ID
        auto handler_for_interface_i = new SST::Interfaces::StandardMem::Handler2<MemoryServer, &MemoryServer::handleMemoryEventFromInterface, int>(this, i);
        
        auto mem_interface_i = loadUserSubComponent<SST::Interfaces::StandardMem>(interface_name, SST::ComponentInfo::SHARE_NONE,
                                                                                   registerTimeBase("1ns"), handler_for_interface_i);
        if (mem_interface_i) {
            if (i == 0) {
                mem_interface = mem_interface_i;  // Store first interface as primary
            } else {
                mem_interfaces.push_back(mem_interface_i);
            }
            all_mem_interfaces.push_back(mem_interface_i);  // Store all interfaces for lookup
            interface_to_id[mem_interface_i] = i;  // Map interface pointer to ID
            out.output("  Loaded memory interface from Compute Server %d: %s\n", i, interface_name.c_str());
        } else {
            // Interface not configured - this is OK, not all compute servers need to connect
            out.output("  Interface %s not configured (optional)\n", interface_name.c_str());
        }
    }
    
    if (all_mem_interfaces.empty()) {
        out.fatal(CALL_INFO, -1, "No memory interfaces found! At least one interface must be configured.\n");
    }
    
    out.output("  Many-to-Many connectivity: %d interfaces loaded\n", (int)mem_interfaces.size() + 1);
    out.output("  Can accept connections from ALL compute servers\n");

    out.output("Memory Server %d initialized\n", memory_server_id);
    out.output("  Capacity: %lu GB, Base address: 0x%lx\n", 
               memory_capacity / (1024*1024*1024), base_address);
}

MemoryServer::~MemoryServer() {
    // Cleanup will be automatic for smart pointers
}

void MemoryServer::init(unsigned int phase) {
    mem_interface->init(phase);
    
    // Initialize all additional interfaces
    for (auto& interface : mem_interfaces) {
        interface->init(phase);
    }
    
    if (phase == 0) {
        // Initialize some sample B+tree nodes for testing
        std::vector<uint8_t> sample_node(btree_node_size, 0);
        
        // Root node - only initialize on Memory Server 0
        if (memory_server_id == 0) {
            store_btree_node(base_address, sample_node);  // Root at memory server 0's base address
        }
        
        // Sample leaf nodes within this server's address space
        for (int i = 0; i < 10; i++) {
            uint64_t leaf_addr = base_address + 0x1000 + (i * btree_node_size);  // Offset from base
            store_btree_node(leaf_addr, sample_node);
        }
        
        out.output("Initialized sample B+tree nodes\n");
    }
}

void MemoryServer::setup() {
    mem_interface->setup();
    
    // Setup all additional interfaces
    for (auto& interface : mem_interfaces) {
        interface->setup();
    }
}

void MemoryServer::finish() {
    mem_interface->finish();
    
    // Finish all additional interfaces
    for (auto& interface : mem_interfaces) {
        interface->finish();
    }
    
    // Output final statistics
    out.output("Memory Server %d completed:\n", memory_server_id);
    out.output("  Remote reads: %lu, Remote writes: %lu\n", 
               stat_network_reads->getCollectionCount(), stat_network_writes->getCollectionCount());
    out.output("  Memory utilization: %lu / %lu bytes (%.2f%%)\n", 
               memory_used, memory_capacity, (double)memory_used / memory_capacity * 100.0);
}

void MemoryServer::handleMemoryEvent(SST::Interfaces::StandardMem::Request* req) {
    dbg.debug(CALL_INFO, 2, 0, "Received memory event: %s (ID=%lu)\n", 
              req->getString().c_str(), req->getID());
    
    // Call handleMemoryEventFromInterface with interface_id = 0
    // This works because responses are broadcast to all interfaces anyway
    handleMemoryEventFromInterface(req, 0);
}

void MemoryServer::handleMemoryEventFromInterface(SST::Interfaces::StandardMem::Request* req, int interface_id) {
    dbg.debug(CALL_INFO, 2, 0, "Received memory event from interface %d: %s (ID=%lu)\n", 
              interface_id, req->getString().c_str(), req->getID());
    
    // Handle incoming remote memory requests with interface ID for proper response routing
    if (auto read_req = dynamic_cast<SST::Interfaces::StandardMem::Read*>(req)) {
        handle_remote_read(read_req, interface_id);
    } else if (auto write_req = dynamic_cast<SST::Interfaces::StandardMem::Write*>(req)) {
        handle_remote_write(write_req, interface_id);
    }
}

void MemoryServer::handle_remote_read(SST::Interfaces::StandardMem::Read* req, int interface_id) {
    // Assert to verify function is called
    assert(req != nullptr && "handle_remote_read called with valid request");
    
    uint64_t address = req->pAddr;
    size_t size = req->size;
    
    // Assert valid address and size
    assert(address != 0 && "Remote read request has valid address");
    assert(size > 0 && "Remote read request has valid size");
    
    dbg.debug(CALL_INFO, 2, 0, "REMOTE READ: addr=0x%lx, size=%zu from interface %d\n", address, size, interface_id);
    
    // Always print address information showing many-to-many connectivity
    out.output("🔍 Memory %d ← Any Compute: REMOTE READ from address 0x%lx (size=%zu bytes) [Many-to-Many]\n", 
               memory_server_id, address, size);
    
    stat_network_reads->addData(1);
    stat_bytes_read->addData(size);
    
    if (!is_address_in_range(address)) {
        out.output("WARNING: Memory Server %d - Remote read to invalid address 0x%lx (range: 0x%lx-0x%lx)\n", 
                   memory_server_id, address, base_address, base_address + 0x1000000);
        send_response(req, false, interface_id);
        return;
    }
    
    // Read data from memory
    std::vector<uint8_t> data = read_memory(address, size);
    
    // Create and send read response 
    // Route response back through the correct interface using interface_id
    auto resp = new SST::Interfaces::StandardMem::ReadResp(req, data);
    
    // Get the correct interface for response based on interface_id
    SST::Interfaces::StandardMem* response_interface = nullptr;
    if (interface_id >= 0 && interface_id < (int)all_mem_interfaces.size()) {
        response_interface = all_mem_interfaces[interface_id];
        dbg.debug(CALL_INFO, 2, 0, "Sending ReadResp for request ID %lu through interface %d\n", 
                  req->getID(), interface_id);
    } else {
        response_interface = mem_interface;  // Fallback to primary interface
        dbg.debug(CALL_INFO, 1, 0, "WARNING: Invalid interface_id %d, using primary interface\n", interface_id);
    }
    
    // Send response through the correct interface
    response_interface->send(resp);
}

void MemoryServer::handle_remote_write(SST::Interfaces::StandardMem::Write* req, int interface_id) {
    // Assert to verify function is called
    assert(req != nullptr && "handle_remote_write called with valid request");
    
    uint64_t address = req->pAddr;
    const std::vector<uint8_t>& data = req->data;
    
    // Assert valid address and data
    assert(address != 0 && "Remote write request has valid address");
    assert(!data.empty() && "Remote write request has valid data");
    
    dbg.debug(CALL_INFO, 2, 0, "REMOTE WRITE: addr=0x%lx, size=%zu\n", address, data.size());
    
    // Always print address information showing many-to-many connectivity
    out.output("🔍 Memory %d ← Any Compute: REMOTE WRITE to address 0x%lx (size=%zu bytes) [Many-to-Many]\n", 
               memory_server_id, address, data.size());
    
    stat_network_writes->addData(1);
    stat_bytes_written->addData(data.size());
    
    if (!is_address_in_range(address)) {
        out.output("WARNING: Memory Server %d - Remote write to invalid address 0x%lx (range: 0x%lx-0x%lx)\n", 
                   memory_server_id, address, base_address, base_address + 0x1000000);
        send_response(req, false);
        return;
    }
    
    // Regular memory write
    write_memory(address, data);
    
    // Send write response through the CORRECT interface
    auto resp = new SST::Interfaces::StandardMem::WriteResp(req);
    
    // Get the correct interface for response based on interface_id
    SST::Interfaces::StandardMem* response_interface = nullptr;
    if (interface_id >= 0 && interface_id < (int)all_mem_interfaces.size()) {
        response_interface = all_mem_interfaces[interface_id];
        dbg.debug(CALL_INFO, 2, 0, "Sending WriteResp for request ID %lu through interface %d\n", 
                  req->getID(), interface_id);
    } else {
        response_interface = mem_interface;  // Fallback to primary interface
        dbg.debug(CALL_INFO, 1, 0, "WARNING: Invalid interface_id %d, using primary interface\n", interface_id);
    }
    
    // Send response through the correct interface
    response_interface->send(resp);
}

std::vector<uint8_t> MemoryServer::read_memory(uint64_t address, size_t size) {
    stat_memory_reads->addData(1);
    
    auto it = memory_blocks.find(address);
    if (it != memory_blocks.end()) {
        // Update access statistics
        it->second.last_access = getCurrentSimTime();
        it->second.access_count++;
        
        // Return requested data
        const auto& block_data = it->second.data;
        if (size <= block_data.size()) {
            return std::vector<uint8_t>(block_data.begin(), block_data.begin() + size);
        }
    }
    
    // Return zeros if block doesn't exist
    return std::vector<uint8_t>(size, 0);
}

void MemoryServer::write_memory(uint64_t address, const std::vector<uint8_t>& data) {
    stat_memory_writes->addData(1);
    
    // Find or create memory block
    auto it = memory_blocks.find(address);
    if (it != memory_blocks.end()) {
        // Update existing block
        it->second.data = data;
        it->second.last_access = getCurrentSimTime();
        it->second.access_count++;
    } else {
        // Create new block
        MemoryBlock new_block;
        new_block.address = address;
        new_block.data = data;
        new_block.last_access = getCurrentSimTime();
        new_block.access_count = 1;
        
        memory_blocks[address] = new_block;
        memory_used += data.size();
    }
    
    update_memory_stats();
}

void MemoryServer::store_btree_node(uint64_t address, const std::vector<uint8_t>& node_data) {
    write_memory(address, node_data);
}

std::vector<uint8_t> MemoryServer::load_btree_node(uint64_t address) {
    return read_memory(address, btree_node_size);
}

bool MemoryServer::is_address_in_range(uint64_t address) {
    // Check if address is within this memory server's allocated range
    uint64_t range_start = base_address;
    uint64_t range_end = base_address + 0x1000000;  // 16MB per server
    
    bool in_range = (address >= range_start) && (address < range_end);
    
    // Debug output for invalid addresses
    if (!in_range && verbose_level >= 2) {
        out.output("Address validation: 0x%lx not in range [0x%lx, 0x%lx) for server %d\n",
                   address, range_start, range_end, memory_server_id);
    }
    
    return in_range;
}

void MemoryServer::update_memory_stats() {
    if (memory_capacity > 0) {
        uint64_t utilization = (memory_used * 100) / memory_capacity;
        stat_memory_utilization->addData(utilization);
    }
}

void MemoryServer::send_response(SST::Interfaces::StandardMem::Request* req, bool success, int interface_id) {
    // Send error response through the correct interface if needed
    // For now, we'll just delete the request
    delete req;
}

// ===== SHARED/EXCLUSIVE LOCK IMPLEMENTATIONS =====

bool MemoryServer::try_acquire_shared_lock(uint64_t node_address) {
    if (!is_address_in_range(node_address)) {
        out.output("ERROR: try_acquire_shared_lock on invalid address 0x%lx\n", node_address);
        return false;
    }
    
    // Read current lock state (first 8 bytes of node)
    std::vector<uint8_t> lock_data = read_memory(node_address, LOCK_HEADER_SIZE);
    uint64_t current_state;
    memcpy(&current_state, lock_data.data(), sizeof(uint64_t));
    
    NodeLock lock;
    lock.state = current_state;
    
    if (lock.is_unlocked()) {
        // Unlocked → transition to shared with count=1
        uint64_t new_state = 1;
        std::vector<uint8_t> new_lock_data(LOCK_HEADER_SIZE);
        memcpy(new_lock_data.data(), &new_state, sizeof(uint64_t));
        write_memory(node_address, new_lock_data);
        
        total_locks_acquired++;
        stat_locks_acquired->addData(1);
        
        if (verbose_level >= 3) {
            out.output("🔓→🔒 Memory %d: Acquired SHARED lock on node 0x%lx (count=1)\n", 
                       memory_server_id, node_address);
        }
        return true;
        
    } else if (lock.is_shared()) {
        // Already shared → increment reader count
        uint64_t new_state = current_state + 1;
        std::vector<uint8_t> new_lock_data(LOCK_HEADER_SIZE);
        memcpy(new_lock_data.data(), &new_state, sizeof(uint64_t));
        write_memory(node_address, new_lock_data);
        
        total_locks_acquired++;
        stat_locks_acquired->addData(1);
        
        if (verbose_level >= 3) {
            out.output("🔒+ Memory %d: Acquired SHARED lock on node 0x%lx (count=%lu)\n", 
                       memory_server_id, node_address, new_state);
        }
        return true;
        
    } else {
        // Exclusive lock held - cannot acquire shared
        total_lock_conflicts++;
        stat_lock_conflicts->addData(1);
        
        if (verbose_level >= 3) {
            uint64_t owner = lock.get_owner();
            out.output("❌ Memory %d: DENIED SHARED lock on node 0x%lx (exclusive by compute %lu)\n", 
                       memory_server_id, node_address, owner);
        }
        return false;
    }
}

bool MemoryServer::release_shared_lock(uint64_t node_address) {
    if (!is_address_in_range(node_address)) {
        out.output("ERROR: release_shared_lock on invalid address 0x%lx\n", node_address);
        return false;
    }
    
    // Read current lock state
    std::vector<uint8_t> lock_data = read_memory(node_address, LOCK_HEADER_SIZE);
    uint64_t current_state;
    memcpy(&current_state, lock_data.data(), sizeof(uint64_t));
    
    NodeLock lock;
    lock.state = current_state;
    
    if (!lock.is_shared()) {
        out.output("ERROR: Trying to release shared lock on node 0x%lx but it's not shared (state=0x%lx)\n", 
                   node_address, current_state);
        return false;
    }
    
    // Decrement reader count
    uint64_t new_state;
    if (current_state == 1) {
        // Last reader - unlock completely
        new_state = 0;
        if (verbose_level >= 3) {
            out.output("🔒→🔓 Memory %d: Released SHARED lock on node 0x%lx (unlocked)\n", 
                       memory_server_id, node_address);
        }
    } else {
        // Still other readers
        new_state = current_state - 1;
        if (verbose_level >= 3) {
            out.output("🔒- Memory %d: Released SHARED lock on node 0x%lx (count=%lu)\n", 
                       memory_server_id, node_address, new_state);
        }
    }
    
    std::vector<uint8_t> new_lock_data(LOCK_HEADER_SIZE);
    memcpy(new_lock_data.data(), &new_state, sizeof(uint64_t));
    write_memory(node_address, new_lock_data);
    
    total_locks_released++;
    stat_locks_released->addData(1);
    
    return true;
}

bool MemoryServer::try_acquire_exclusive_lock(uint64_t node_address, uint64_t compute_id) {
    if (!is_address_in_range(node_address)) {
        out.output("ERROR: try_acquire_exclusive_lock on invalid address 0x%lx\n", node_address);
        return false;
    }
    
    // Read current lock state
    std::vector<uint8_t> lock_data = read_memory(node_address, LOCK_HEADER_SIZE);
    uint64_t current_state;
    memcpy(&current_state, lock_data.data(), sizeof(uint64_t));
    
    NodeLock lock;
    lock.state = current_state;
    
    if (lock.is_unlocked()) {
        // Unlocked → acquire exclusive
        uint64_t new_state = NodeLock::make_exclusive(compute_id);
        std::vector<uint8_t> new_lock_data(LOCK_HEADER_SIZE);
        memcpy(new_lock_data.data(), &new_state, sizeof(uint64_t));
        write_memory(node_address, new_lock_data);
        
        // Verify we got it (check for race)
        std::vector<uint8_t> verify_data = read_memory(node_address, LOCK_HEADER_SIZE);
        uint64_t verify_state;
        memcpy(&verify_state, verify_data.data(), sizeof(uint64_t));
        
        if (verify_state == new_state) {
            total_locks_acquired++;
            stat_locks_acquired->addData(1);
            
            if (verbose_level >= 3) {
                out.output("🔓→🔐 Memory %d: Acquired EXCLUSIVE lock on node 0x%lx by compute %lu\n", 
                           memory_server_id, node_address, compute_id);
            }
            return true;
        } else {
            // Race condition - someone else got it
            total_lock_conflicts++;
            stat_lock_conflicts->addData(1);
            
            if (verbose_level >= 3) {
                out.output("❌ Memory %d: RACE on EXCLUSIVE lock for node 0x%lx (lost to another compute)\n", 
                           memory_server_id, node_address);
            }
            return false;
        }
    } else {
        // Already locked (shared or exclusive)
        total_lock_conflicts++;
        stat_lock_conflicts->addData(1);
        
        if (verbose_level >= 3) {
            if (lock.is_shared()) {
                out.output("❌ Memory %d: DENIED EXCLUSIVE lock on node 0x%lx (shared by %lu readers)\n", 
                           memory_server_id, node_address, lock.get_reader_count());
            } else {
                out.output("❌ Memory %d: DENIED EXCLUSIVE lock on node 0x%lx (exclusive by compute %lu)\n", 
                           memory_server_id, node_address, lock.get_owner());
            }
        }
        return false;
    }
}

bool MemoryServer::release_exclusive_lock(uint64_t node_address, uint64_t compute_id) {
    if (!is_address_in_range(node_address)) {
        out.output("ERROR: release_exclusive_lock on invalid address 0x%lx\n", node_address);
        return false;
    }
    
    // Read current lock state
    std::vector<uint8_t> lock_data = read_memory(node_address, LOCK_HEADER_SIZE);
    uint64_t current_state;
    memcpy(&current_state, lock_data.data(), sizeof(uint64_t));
    
    NodeLock lock;
    lock.state = current_state;
    
    // Verify we own it
    if (!lock.is_exclusive() || lock.get_owner() != compute_id) {
        out.output("ERROR: Compute %lu trying to release exclusive lock on node 0x%lx but doesn't own it (state=0x%lx)\n", 
                   compute_id, node_address, current_state);
        return false;
    }
    
    // Release (write 0)
    uint64_t new_state = 0;
    std::vector<uint8_t> new_lock_data(LOCK_HEADER_SIZE);
    memcpy(new_lock_data.data(), &new_state, sizeof(uint64_t));
    write_memory(node_address, new_lock_data);
    
    total_locks_released++;
    stat_locks_released->addData(1);
    
    if (verbose_level >= 3) {
        out.output("🔐→🔓 Memory %d: Released EXCLUSIVE lock on node 0x%lx by compute %lu\n", 
                   memory_server_id, node_address, compute_id);
    }
    
    return true;
}

// Lock state queries
uint64_t MemoryServer::read_lock_state(uint64_t node_address) {
    if (!is_address_in_range(node_address)) {
        return 0;
    }
    
    std::vector<uint8_t> lock_data = read_memory(node_address, LOCK_HEADER_SIZE);
    uint64_t state;
    memcpy(&state, lock_data.data(), sizeof(uint64_t));
    return state;
}

bool MemoryServer::is_locked_shared(uint64_t node_address) {
    uint64_t state = read_lock_state(node_address);
    NodeLock lock;
    lock.state = state;
    return lock.is_shared();
}

bool MemoryServer::is_locked_exclusive(uint64_t node_address) {
    uint64_t state = read_lock_state(node_address);
    NodeLock lock;
    lock.state = state;
    return lock.is_exclusive();
}