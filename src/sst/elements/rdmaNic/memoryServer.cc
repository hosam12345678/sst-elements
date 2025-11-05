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

    // Calculate base address for this memory server
    // Address space per server = 64KB (metadata) + (CHUNKS_PER_SERVER * CHUNK_SIZE)
    uint64_t address_space_per_server = 0x10000 + (CHUNKS_PER_SERVER * CHUNK_SIZE);
    base_address = 0x10000000 + memory_server_id * address_space_per_server;

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

    // Initialize chunk allocation state
    chunk_allocated.resize(CHUNKS_PER_SERVER, false);  // All chunks initially free
    next_free_chunk_hint = 0;  // Start searching from chunk 0
    
    out.output("Memory Server %d initialized\n", memory_server_id);
    out.output("  Capacity: %lu GB, Base address: 0x%lx\n", 
               memory_capacity / (1024*1024*1024), base_address);
    out.output("  Chunk allocation: %lu chunks of %lu MB each (%lu GB total)\n",
               CHUNKS_PER_SERVER, CHUNK_SIZE / (1024*1024), 
               (CHUNKS_PER_SERVER * CHUNK_SIZE) / (1024*1024*1024));
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
    
    // Check for magic address operations first
    uint64_t address = 0;
    if (auto ll_req = dynamic_cast<SST::Interfaces::StandardMem::LoadLink*>(req)) {
        address = ll_req->pAddr;
    } else if (auto sc_req = dynamic_cast<SST::Interfaces::StandardMem::StoreConditional*>(req)) {
        address = sc_req->pAddr;
    } else if (auto read_req = dynamic_cast<SST::Interfaces::StandardMem::Read*>(req)) {
        address = read_req->pAddr;
    } else if (auto write_req = dynamic_cast<SST::Interfaces::StandardMem::Write*>(req)) {
        address = write_req->pAddr;
    }
    
    // Check if this is a magic address operation
    uint64_t magic_base = MAGIC_ALLOCATE_CHUNK_BASE;
    if ((address & 0xFFFFFFFF00000000ULL) == magic_base) {
        // This is a chunk allocation request
        if (auto read_req = dynamic_cast<SST::Interfaces::StandardMem::Read*>(req)) {
            handle_magic_allocate_chunk(read_req, interface_id);
            return;
        } else {
            out.output("⚠️  Memory %d: Magic address 0x%lx accessed with non-Read operation\n", 
                      memory_server_id, address);
            delete req;
            return;
        }
    }
    
    // Handle incoming remote memory requests with interface ID for proper response routing
    if (auto ll_req = dynamic_cast<SST::Interfaces::StandardMem::LoadLink*>(req)) {
        handle_loadlink(ll_req, interface_id);
    } else if (auto sc_req = dynamic_cast<SST::Interfaces::StandardMem::StoreConditional*>(req)) {
        handle_storeconditional(sc_req, interface_id);
    } else if (auto read_req = dynamic_cast<SST::Interfaces::StandardMem::Read*>(req)) {
        handle_remote_read(read_req, interface_id);
    } else if (auto write_req = dynamic_cast<SST::Interfaces::StandardMem::Write*>(req)) {
        handle_remote_write(write_req, interface_id);
    } else {
        out.output("⚠️  Memory %d: Unhandled request type: %s\n", memory_server_id, req->getString().c_str());
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
    
    // CRITICAL: Invalidate any LL reservations for this address
    // Any write (not just SC) must clear LL reservations to maintain atomicity
    auto ll_it = ll_reservations.find(address);
    if (ll_it != ll_reservations.end()) {
        dbg.debug(CALL_INFO, 2, 0, "Regular Write invalidated LL reservation at 0x%lx\n", address);
        ll_reservations.erase(ll_it);
    }
    
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

void MemoryServer::handle_loadlink(SST::Interfaces::StandardMem::LoadLink* req, int interface_id) {
    uint64_t address = req->pAddr;
    size_t size = req->size;
    
    dbg.debug(CALL_INFO, 2, 0, "LOADLINK: addr=0x%lx, size=%zu from interface %d\n", address, size, interface_id);
    
    out.output("🔗 Memory %d ← Compute: LOADLINK from address 0x%lx (size=%zu bytes) [LL/SC Protocol]\n", 
               memory_server_id, address, size);
    
    stat_network_reads->addData(1);
    
    if (!is_address_in_range(address)) {
        out.output("WARNING: Memory Server %d - LoadLink to invalid address 0x%lx\n", 
                   memory_server_id, address);
        send_response(req, false, interface_id);
        return;
    }
    
    // Read current value from memory
    std::vector<uint8_t> data = read_memory(address, size);
    
    // Store reservation: track this address and the value read
    // This allows StoreConditional to detect if the value changed
    uint64_t value = 0;
    if (data.size() >= 8) {
        memcpy(&value, data.data(), 8);
    }
    
    // Store LL reservation (allows multiple nodes to have reservations on same address)
    // Each interface gets its own reservation tracking the value it read
    ll_reservations[address][interface_id] = value;
    
    dbg.debug(CALL_INFO, 2, 0, "LL reservation set: addr=0x%lx, value=%lu, interface=%d (total reservations at this addr: %zu)\n", 
              address, value, interface_id, ll_reservations[address].size());
    
    // Send ReadResp (LoadLink responds like Read, but use full constructor since LoadLink != Read)
    auto resp = new SST::Interfaces::StandardMem::ReadResp(
        req->getID(),           // Request ID
        req->pAddr,             // Physical address
        req->size,              // Size
        data,                   // Response data
        req->getAllFlags(),     // Flags
        req->vAddr,             // Virtual address
        req->iPtr,              // Instruction pointer
        req->tid                // Thread ID
    );
    
    SST::Interfaces::StandardMem* response_interface = nullptr;
    if (interface_id >= 0 && interface_id < (int)all_mem_interfaces.size()) {
        response_interface = all_mem_interfaces[interface_id];
    } else {
        response_interface = mem_interface;
    }
    
    response_interface->send(resp);
}

void MemoryServer::handle_storeconditional(SST::Interfaces::StandardMem::StoreConditional* req, int interface_id) {
    uint64_t address = req->pAddr;
    const std::vector<uint8_t>& data = req->data;
    
    dbg.debug(CALL_INFO, 2, 0, "STORECONDITIONAL: addr=0x%lx, size=%zu from interface %d\n", 
              address, data.size(), interface_id);
    
    out.output("🔐 Memory %d ← Compute: STORECONDITIONAL to address 0x%lx (size=%zu bytes) [LL/SC Protocol]\n", 
               memory_server_id, address, data.size());
    
    stat_network_writes->addData(1);
    
    if (!is_address_in_range(address)) {
        out.output("WARNING: Memory Server %d - StoreConditional to invalid address 0x%lx\n", 
                   memory_server_id, address);
        send_response(req, false, interface_id);
        return;
    }
    
    // Check if there's a valid LL reservation for this interface at this address
    auto addr_it = ll_reservations.find(address);
    bool sc_success = false;
    
    if (addr_it != ll_reservations.end()) {
        // There are reservations at this address, check if this interface has one
        auto& reservations_at_addr = addr_it->second;
        auto res_it = reservations_at_addr.find(interface_id);
        
        if (res_it != reservations_at_addr.end()) {
            // This interface has an LL reservation - check if value changed
            uint64_t reserved_value = res_it->second;
            
            std::vector<uint8_t> current_data = read_memory(address, 8);
            uint64_t current_value = 0;
            if (current_data.size() >= 8) {
                memcpy(&current_value, current_data.data(), 8);
            }
            
            if (current_value == reserved_value) {
                // Value hasn't changed - SC SUCCEEDS!
                // This is the FIRST StoreConditional to execute successfully
                write_memory(address, data);
                sc_success = true;
                
                dbg.debug(CALL_INFO, 2, 0, "SC SUCCESS: addr=0x%lx, value unchanged (%lu), invalidating %zu other reservations\n", 
                          address, current_value, reservations_at_addr.size() - 1);
                out.output("   ✓ SC SUCCESS at 0x%lx (value unchanged: %lu) - invalidating %zu other reservations\n", 
                          address, current_value, reservations_at_addr.size() - 1);
                
                // CRITICAL: Invalidate ALL reservations at this address (including ours)
                // Any subsequent SC from other nodes will fail because:
                // 1. Their reservation is gone, OR
                // 2. The value changed from what they read
                ll_reservations.erase(addr_it);
                
            } else {
                // Value changed since our LL - SC FAILS!
                // Another node already wrote to this address
                sc_success = false;
                
                dbg.debug(CALL_INFO, 2, 0, "SC FAILED: addr=0x%lx, value changed (%lu -> %lu)\n", 
                          address, reserved_value, current_value);
                out.output("   ✗ SC FAILED at 0x%lx (value changed: %lu -> %lu) [RACE DETECTED!]\n", 
                          address, reserved_value, current_value);
                
                // Remove only this interface's reservation (value already changed)
                reservations_at_addr.erase(res_it);
                if (reservations_at_addr.empty()) {
                    ll_reservations.erase(addr_it);
                }
            }
            
        } else {
            // This interface has no reservation at this address - SC FAILS
            sc_success = false;
            dbg.debug(CALL_INFO, 2, 0, "SC FAILED: addr=0x%lx, no LL reservation for interface %d\n", address, interface_id);
            out.output("   ✗ SC FAILED at 0x%lx (no LL reservation for this interface)\n", address);
        }
    } else {
        // No reservations at this address at all - SC FAILS
        sc_success = false;
        dbg.debug(CALL_INFO, 2, 0, "SC FAILED: addr=0x%lx, no LL reservations at this address\n", address);
        out.output("   ✗ SC FAILED at 0x%lx (no LL reservations at this address)\n", address);
    }
    
    // Send WriteResp with success/failure flag (use full constructor since StoreConditional != Write)
    auto resp = new SST::Interfaces::StandardMem::WriteResp(
        req->getID(),           // Request ID
        req->pAddr,             // Physical address
        req->size,              // Size
        req->getAllFlags(),     // Flags
        req->vAddr,             // Virtual address
        req->iPtr,              // Instruction pointer
        req->tid                // Thread ID
    );
    if (!sc_success) {
        resp->setFail();  // Set fail flag if SC failed
    }
    
    SST::Interfaces::StandardMem* response_interface = nullptr;
    if (interface_id >= 0 && interface_id < (int)all_mem_interfaces.size()) {
        response_interface = all_mem_interfaces[interface_id];
    } else {
        response_interface = mem_interface;
    }
    
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

// ===== CHUNK ALLOCATION METHODS =====

int32_t MemoryServer::allocate_chunk() {
    // Linear search for first free chunk, starting from hint
    for (uint64_t i = 0; i < CHUNKS_PER_SERVER; i++) {
        uint32_t chunk_id = (next_free_chunk_hint + i) % CHUNKS_PER_SERVER;
        
        if (!chunk_allocated[chunk_id]) {
            // Found a free chunk
            chunk_allocated[chunk_id] = true;
            next_free_chunk_hint = (chunk_id + 1) % CHUNKS_PER_SERVER;  // Update hint for next allocation
            
            if (verbose_level >= 2) {
                out.output("Allocated chunk %u (address 0x%lx)\n", 
                          chunk_id, chunk_id_to_address(chunk_id));
            }
            
            return chunk_id;
        }
    }
    
    // No free chunks available
    if (verbose_level >= 1) {
        out.output("ERROR: No free chunks available (all %lu chunks allocated)\n", CHUNKS_PER_SERVER);
    }
    return -1;
}

void MemoryServer::free_chunk(uint32_t chunk_id) {
    if (chunk_id >= CHUNKS_PER_SERVER) {
        out.output("ERROR: Invalid chunk_id %u (max %lu)\n", chunk_id, CHUNKS_PER_SERVER);
        return;
    }
    
    if (!chunk_allocated[chunk_id]) {
        out.output("WARNING: Attempted to free already-free chunk %u\n", chunk_id);
        return;
    }
    
    chunk_allocated[chunk_id] = false;
    next_free_chunk_hint = chunk_id;  // Next allocation can start from this freed chunk
    
    if (verbose_level >= 2) {
        out.output("Freed chunk %u (address 0x%lx)\n", 
                  chunk_id, chunk_id_to_address(chunk_id));
    }
}

uint64_t MemoryServer::chunk_id_to_address(uint32_t chunk_id) {
    // Calculate base address of chunk within this memory server's address space
    // Start chunks at offset 0x10000 (64KB) from base to leave room for root and metadata
    uint64_t chunk_pool_base = base_address + 0x10000;
    return chunk_pool_base + (chunk_id * CHUNK_SIZE);
}

bool MemoryServer::is_address_in_range(uint64_t address) {
    // Check if address is within this memory server's allocated range
    // Range = base_address to (base_address + chunk_pool + all chunks)
    // chunk_pool starts at base_address + 0x10000 (64KB for root/metadata)
    // total range = 64KB metadata + (CHUNKS_PER_SERVER * CHUNK_SIZE)
    uint64_t range_start = base_address;
    uint64_t range_end = base_address + 0x10000 + (CHUNKS_PER_SERVER * CHUNK_SIZE);
    
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

// ===== MAGIC ADDRESS HANDLER FOR CHUNK ALLOCATION =====

void MemoryServer::handle_magic_allocate_chunk(SST::Interfaces::StandardMem::Read* req, int interface_id) {
    uint64_t address = req->pAddr;
    uint32_t requested_server_id = address & 0xFFFFFFFF;
    
    dbg.debug(CALL_INFO, 2, 0, "MAGIC: Allocate chunk request for server %u from interface %d\n", 
              requested_server_id, interface_id);
    
    out.output("🪄 Memory %d ← Compute: ALLOCATE_CHUNK request (magic address 0x%lx) [READ operation]\n", 
               memory_server_id, address);
    
    // Verify the request is for this memory server
    if (requested_server_id != memory_server_id) {
        out.output("WARNING: Memory Server %d received chunk allocation request for server %u\n", 
                   memory_server_id, requested_server_id);
        
        // Send failure response (0xFFFFFFFF)
        std::vector<uint8_t> failure_data(4);
        uint32_t failure_value = 0xFFFFFFFF;
        memcpy(failure_data.data(), &failure_value, sizeof(uint32_t));
        
        auto read_resp = new SST::Interfaces::StandardMem::ReadResp(req, failure_data);
        
        SST::Interfaces::StandardMem* response_interface = nullptr;
        if (interface_id >= 0 && interface_id < (int)all_mem_interfaces.size()) {
            response_interface = all_mem_interfaces[interface_id];
        } else {
            response_interface = mem_interface;
        }
        response_interface->send(read_resp);
        delete req;
        return;
    }
    
    // Allocate a chunk
    int32_t chunk_id = allocate_chunk();
    
    if (chunk_id < 0) {
        // Allocation failed - no free chunks
        out.output("   ✗ ALLOCATE_CHUNK FAILED: No free chunks available\n");
        
        std::vector<uint8_t> failure_data(4);
        uint32_t failure_value = 0xFFFFFFFF;
        memcpy(failure_data.data(), &failure_value, sizeof(uint32_t));
        
        auto read_resp = new SST::Interfaces::StandardMem::ReadResp(req, failure_data);
        
        SST::Interfaces::StandardMem* response_interface = nullptr;
        if (interface_id >= 0 && interface_id < (int)all_mem_interfaces.size()) {
            response_interface = all_mem_interfaces[interface_id];
        } else {
            response_interface = mem_interface;
        }
        response_interface->send(read_resp);
        
    } else {
        // Allocation succeeded
        uint64_t chunk_address = chunk_id_to_address(chunk_id);
        out.output("   ✓ ALLOCATE_CHUNK SUCCESS: chunk_id=%d, address=0x%lx\n", 
                  chunk_id, chunk_address);
        
        // Send response with chunk_id and address
        // Response format: [chunk_id (4 bytes)] [chunk_address (8 bytes)]
        std::vector<uint8_t> success_data(12);
        memcpy(success_data.data(), &chunk_id, sizeof(uint32_t));
        memcpy(success_data.data() + 4, &chunk_address, sizeof(uint64_t));
        
        auto read_resp = new SST::Interfaces::StandardMem::ReadResp(req, success_data);
        
        SST::Interfaces::StandardMem* response_interface = nullptr;
        if (interface_id >= 0 && interface_id < (int)all_mem_interfaces.size()) {
            response_interface = all_mem_interfaces[interface_id];
        } else {
            response_interface = mem_interface;
        }
        response_interface->send(read_resp);
    }
    
    delete req;
}