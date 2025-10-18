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
    enable_locking = params.find<bool>("enable_locking", true);
    lock_timeout = params.find<SimTime_t>("lock_timeout_us", 10000) * 1000; // Convert to ns
    verbose_level = params.find<int>("verbose", 0);

    // Calculate base address for this memory server - each server gets 16MB address space
    base_address = 0x10000000 + memory_server_id * 0x1000000;  // 16MB per server

    // Setup debug output with maximum verbosity for address visibility
    dbg.init("", 5, 0, (Output::output_location_t)1);  // Force high verbosity
    out.init("MemoryServer[@p:@l]: ", 1, 0, Output::STDOUT);

    // Initialize statistics
    stat_rdma_reads = registerStatistic<uint64_t>("rdma_reads_received");
    stat_rdma_writes = registerStatistic<uint64_t>("rdma_writes_received");
    stat_memory_reads = registerStatistic<uint64_t>("memory_reads");
    stat_memory_writes = registerStatistic<uint64_t>("memory_writes");
    stat_lock_acquisitions = registerStatistic<uint64_t>("lock_acquisitions");
    stat_lock_releases = registerStatistic<uint64_t>("lock_releases");
    stat_lock_conflicts = registerStatistic<uint64_t>("lock_conflicts");
    stat_bytes_read = registerStatistic<uint64_t>("bytes_read");
    stat_bytes_written = registerStatistic<uint64_t>("bytes_written");
    stat_memory_utilization = registerStatistic<uint64_t>("memory_utilization");

    // Setup multiple RDMA interfaces 
    // Support both architectures:
    // 1. Multi-interface: rdma_nic_0, rdma_nic_1, ... (shared handler)
    // 2. Single-interface: rdma_nic (dedicated handler per instance)
    
    // Check if we're using single-interface architecture (dedicated instances)
    auto single_handler = new SST::Interfaces::StandardMem::Handler2<MemoryServer,&MemoryServer::handleMemoryEvent>(this);
    auto single_interface = loadUserSubComponent<SST::Interfaces::StandardMem>("rdma_nic", SST::ComponentInfo::SHARE_NONE,
                                                                               registerTimeBase("1ns"), single_handler);
    if (single_interface) {
        // Single interface architecture - dedicated memory server instance
        out.output("Using single-interface architecture (dedicated instance)\n");
        rdma_interface = single_interface;
        all_rdma_interfaces.push_back(rdma_interface);
        interface_to_id[rdma_interface] = 0;  // Single interface uses ID 0
        out.output("  Loaded single RDMA interface: rdma_nic\n");
    } else {
        // Multi-interface architecture - shared handler
        out.output("Using multi-interface architecture (shared handler)\n");
        auto shared_handler = new SST::Interfaces::StandardMem::Handler2<MemoryServer,&MemoryServer::handleMemoryEvent>(this);
        
        // Load RDMA interfaces for ALL compute servers (many-to-many connectivity)
        for (int i = 0; i < num_compute_nodes; i++) {
            std::string interface_name = "rdma_nic_" + std::to_string(i);
            
            auto rdma_interface_i = loadUserSubComponent<SST::Interfaces::StandardMem>(interface_name, SST::ComponentInfo::SHARE_NONE,
                                                                                       registerTimeBase("1ns"), shared_handler);
            if (rdma_interface_i) {
                if (i == 0) {
                    rdma_interface = rdma_interface_i;  // Store first interface as primary
                } else {
                    rdma_interfaces.push_back(rdma_interface_i);
                }
                all_rdma_interfaces.push_back(rdma_interface_i);  // Store all interfaces for lookup
                interface_to_id[rdma_interface_i] = i;  // Map interface pointer to ID
                out.output("  Loaded RDMA interface from Compute Server %d: %s\n", i, interface_name.c_str());
            } else {
                out.fatal(CALL_INFO, -1, "Failed to load RDMA interface %s\n", interface_name.c_str());
            }
        }
        
        if (all_rdma_interfaces.empty()) {
            out.fatal(CALL_INFO, -1, "No RDMA interfaces found! Check interface configuration.\n");
        }
    }
    
    out.output("  Many-to-Many RDMA connectivity: %d interfaces loaded\n", (int)rdma_interfaces.size() + 1);
    out.output("  Can accept connections from ALL compute servers\n");

    out.output("Memory Server %d initialized\n", memory_server_id);
    out.output("  Capacity: %lu GB, Base address: 0x%lx\n", 
               memory_capacity / (1024*1024*1024), base_address);
}

MemoryServer::~MemoryServer() {
    // Cleanup will be automatic for smart pointers
}

void MemoryServer::init(unsigned int phase) {
    rdma_interface->init(phase);
    
    // Initialize all additional interfaces
    for (auto& interface : rdma_interfaces) {
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
    rdma_interface->setup();
    
    // Setup all additional interfaces
    for (auto& interface : rdma_interfaces) {
        interface->setup();
    }
}

void MemoryServer::finish() {
    rdma_interface->finish();
    
    // Finish all additional interfaces
    for (auto& interface : rdma_interfaces) {
        interface->finish();
    }
    
    // Output final statistics
    out.output("Memory Server %d completed:\n", memory_server_id);
    out.output("  RDMA reads: %lu, RDMA writes: %lu\n", 
               stat_rdma_reads->getCollectionCount(), stat_rdma_writes->getCollectionCount());
    out.output("  Memory utilization: %lu / %lu bytes (%.2f%%)\n", 
               memory_used, memory_capacity, (double)memory_used / memory_capacity * 100.0);
}

void MemoryServer::handleMemoryEvent(SST::Interfaces::StandardMem::Request* req) {
    dbg.debug(CALL_INFO, 2, 0, "Received memory event: %s (ID=%lu)\n", 
              req->getString().c_str(), req->getID());
    
    // We need to determine which interface this request came from
    // Unfortunately, the StandardMem handler doesn't provide this directly
    // For now, we'll use a heuristic or store interface information differently
    
    // TODO: This is a limitation - we can't easily determine the source interface
    // For now, default to interface 0 (will work for single interface, fail for multi)
    int interface_id = 0;
    
    // Handle incoming RDMA requests
    if (auto read_req = dynamic_cast<SST::Interfaces::StandardMem::Read*>(req)) {
        handle_rdma_read(read_req, interface_id);
    } else if (auto write_req = dynamic_cast<SST::Interfaces::StandardMem::Write*>(req)) {
        handle_rdma_write(write_req, interface_id);
    }
}

void MemoryServer::handleMemoryEventFromInterface(SST::Interfaces::StandardMem::Request* req, int interface_id) {
    dbg.debug(CALL_INFO, 2, 0, "Received memory event from interface %d: %s (ID=%lu)\n", 
              interface_id, req->getString().c_str(), req->getID());
    
    // Handle incoming RDMA requests with interface ID for proper response routing
    if (auto read_req = dynamic_cast<SST::Interfaces::StandardMem::Read*>(req)) {
        handle_rdma_read(read_req, interface_id);
    } else if (auto write_req = dynamic_cast<SST::Interfaces::StandardMem::Write*>(req)) {
        handle_rdma_write(write_req, interface_id);
    }
}

void MemoryServer::handle_rdma_read(SST::Interfaces::StandardMem::Read* req, int interface_id) {
    // Assert to verify function is called
    assert(req != nullptr && "handle_rdma_read called with valid request");
    
    uint64_t address = req->pAddr;
    size_t size = req->size;
    
    // Assert valid address and size
    assert(address != 0 && "RDMA read request has valid address");
    assert(size > 0 && "RDMA read request has valid size");
    
    dbg.debug(CALL_INFO, 2, 0, "RDMA READ: addr=0x%lx, size=%zu from interface %d\n", address, size, interface_id);
    
    // Always print address information showing many-to-many connectivity
    out.output("🔍 Memory %d ← Any Compute: RDMA READ from address 0x%lx (size=%zu bytes) [Many-to-Many]\n", 
               memory_server_id, address, size);
    
    stat_rdma_reads->addData(1);
    stat_bytes_read->addData(size);
    
    if (!is_address_in_range(address)) {
        out.output("WARNING: Memory Server %d - RDMA read to invalid address 0x%lx (range: 0x%lx-0x%lx)\n", 
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
    if (interface_id >= 0 && interface_id < (int)all_rdma_interfaces.size()) {
        response_interface = all_rdma_interfaces[interface_id];
        dbg.debug(CALL_INFO, 2, 0, "Sending ReadResp for request ID %lu through interface %d\n", 
                  req->getID(), interface_id);
    } else {
        response_interface = rdma_interface;  // Fallback to primary interface
        dbg.debug(CALL_INFO, 1, 0, "WARNING: Invalid interface_id %d, using primary interface\n", interface_id);
    }
    
    // Send response through the correct interface
    response_interface->send(resp);
}

void MemoryServer::handle_rdma_write(SST::Interfaces::StandardMem::Write* req, int interface_id) {
    // Assert to verify function is called
    assert(req != nullptr && "handle_rdma_write called with valid request");
    
    uint64_t address = req->pAddr;
    const std::vector<uint8_t>& data = req->data;
    
    // Assert valid address and data
    assert(address != 0 && "RDMA write request has valid address");
    assert(!data.empty() && "RDMA write request has valid data");
    
    dbg.debug(CALL_INFO, 2, 0, "RDMA WRITE: addr=0x%lx, size=%zu\n", address, data.size());
    
    // Always print address information showing many-to-many connectivity
    out.output("🔍 Memory %d ← Any Compute: RDMA WRITE to address 0x%lx (size=%zu bytes) [Many-to-Many]\n", 
               memory_server_id, address, data.size());
    
    stat_rdma_writes->addData(1);
    stat_bytes_written->addData(data.size());
    
    if (!is_address_in_range(address)) {
        out.output("WARNING: Memory Server %d - RDMA write to invalid address 0x%lx (range: 0x%lx-0x%lx)\n", 
                   memory_server_id, address, base_address, base_address + 0x1000000);
        send_response(req, false);
        return;
    }
    
    // Check if this is a lock operation
    if (enable_locking && is_lock_address(address)) {
        // Handle lock operation
        uint64_t lock_value = *reinterpret_cast<const uint64_t*>(data.data());
        uint64_t requester_id = (lock_value == 0) ? 0 : lock_value; // 0 = release, non-zero = acquire
        
        if (lock_value == 0) {
            release_lock(address, requester_id);
        } else {
            acquire_lock(address, requester_id);
        }
    } else {
        // Regular memory write
        write_memory(address, data);
    }
    
    // Send write response
    auto resp = new SST::Interfaces::StandardMem::WriteResp(req);
    
    // Send response with simulated latency  
    rdma_interface->send(resp);
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
        new_block.is_locked = false;
        new_block.lock_owner = 0;
        
        memory_blocks[address] = new_block;
        memory_used += data.size();
    }
    
    update_memory_stats();
}

bool MemoryServer::acquire_lock(uint64_t lock_address, uint64_t requester_id) {
    dbg.debug(CALL_INFO, 3, 0, "Lock acquire: addr=0x%lx, requester=%lu\n", lock_address, requester_id);
    
    stat_lock_acquisitions->addData(1);
    
    auto it = node_locks.find(lock_address);
    if (it == node_locks.end()) {
        // Create new lock
        NodeLock new_lock;
        new_lock.lock_address = lock_address;
        new_lock.is_locked = true;
        new_lock.owner_id = requester_id;
        new_lock.lock_time = getCurrentSimTime();
        
        node_locks[lock_address] = new_lock;
        return true;
    } else {
        // Lock exists, check if available
        NodeLock& lock = it->second;
        if (!lock.is_locked) {
            // Lock is available
            lock.is_locked = true;
            lock.owner_id = requester_id;
            lock.lock_time = getCurrentSimTime();
            return true;
        } else {
            // Lock is held by someone else
            stat_lock_conflicts->addData(1);
            lock.waiting_queue.push(requester_id);
            return false;
        }
    }
}

void MemoryServer::release_lock(uint64_t lock_address, uint64_t requester_id) {
    dbg.debug(CALL_INFO, 3, 0, "Lock release: addr=0x%lx, requester=%lu\n", lock_address, requester_id);
    
    stat_lock_releases->addData(1);
    
    auto it = node_locks.find(lock_address);
    if (it != node_locks.end()) {
        NodeLock& lock = it->second;
        if (lock.is_locked && lock.owner_id == requester_id) {
            // Release the lock
            lock.is_locked = false;
            lock.owner_id = 0;
            
            // Check if anyone is waiting
            if (!lock.waiting_queue.empty()) {
                uint64_t next_owner = lock.waiting_queue.front();
                lock.waiting_queue.pop();
                
                // Grant lock to next waiter
                lock.is_locked = true;
                lock.owner_id = next_owner;
                lock.lock_time = getCurrentSimTime();
            }
        }
    }
}

bool MemoryServer::is_lock_address(uint64_t address) {
    // Lock addresses are at a fixed offset from node addresses
    return (address >= base_address + 0x100000) && (address < base_address + 0x200000);
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

void MemoryServer::cleanup_expired_locks() {
    SimTime_t current_time = getCurrentSimTime();
    
    for (auto& pair : node_locks) {
        NodeLock& lock = pair.second;
        if (lock.is_locked && (current_time - lock.lock_time) > lock_timeout) {
            // Lock has expired, release it
            out.output("WARNING: Lock 0x%lx expired for owner %lu\n", 
                       lock.lock_address, lock.owner_id);
            lock.is_locked = false;
            lock.owner_id = 0;
        }
    }
}

void MemoryServer::send_response(SST::Interfaces::StandardMem::Request* req, bool success, int interface_id) {
    // Send error response through the correct interface if needed
    // For now, we'll just delete the request
    delete req;
}