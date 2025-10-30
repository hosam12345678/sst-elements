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

#ifndef _H_MEMORY_SERVER
#define _H_MEMORY_SERVER

#include <sst/core/component.h>
#include <sst/core/event.h>
#include <sst/core/sst_types.h>
#include <sst/core/interfaces/stdMem.h>
#include <unordered_map>
#include <vector>

namespace SST {
namespace MemHierarchy {

// Memory block structure for storing B+tree nodes
struct MemoryBlock {
    uint64_t address;
    std::vector<uint8_t> data;
    SimTime_t last_access;
    uint64_t access_count;
    bool is_locked;
    uint64_t lock_owner;
};

// Lock structure for B+tree node locking
struct NodeLock {
    uint64_t lock_address;
    bool is_locked;
    uint64_t owner_id;  // Which compute node owns the lock
    SimTime_t lock_time;
    std::queue<uint64_t> waiting_queue;  // Nodes waiting for lock
};

class MemoryServer : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(
        MemoryServer,
        "rdmaNic",
        "memoryServer",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Disaggregated memory server - stores B+tree nodes and data",
        COMPONENT_CATEGORY_MEMORY
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"memory_server_id", "Memory server node ID", "0"},
        {"num_compute_nodes", "Total number of compute nodes to accept connections from", "8"},
        {"memory_capacity_gb", "Memory capacity in GB", "16"},
        {"memory_latency_ns", "Memory access latency in nanoseconds", "100"},
        {"btree_node_size", "Size of B+tree nodes in bytes", "4096"},
        {"enable_locking", "Enable B+tree node locking", "true"},
        {"lock_timeout_us", "Lock timeout in microseconds", "10000"},
        {"verbose", "Verbose debug output", "0"}
    )

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"mem_interface", "Memory interface - single interface per memory server instance", "SST::Interfaces::StandardMem"}
    )

    SST_ELI_DOCUMENT_STATISTICS(
        {"network_reads_received", "Number of remote read requests received", "requests", 1},
        {"network_writes_received", "Number of remote write requests received", "requests", 1},
        {"memory_reads", "Number of local memory reads", "reads", 1},
        {"memory_writes", "Number of local memory writes", "writes", 1},
        {"lock_acquisitions", "Number of lock acquisition requests", "locks", 1},
        {"lock_releases", "Number of lock release requests", "locks", 1},
        {"lock_conflicts", "Number of lock conflicts/waits", "conflicts", 1},
        {"bytes_read", "Total bytes read from memory", "bytes", 1},
        {"bytes_written", "Total bytes written to memory", "bytes", 1},
        {"memory_utilization", "Memory utilization percentage", "percent", 1}
    )

    // Constructor
    MemoryServer(ComponentId_t id, Params& params);
    ~MemoryServer();

    // SST Component interface
    virtual void init(unsigned int phase) override;
    virtual void setup() override;
    virtual void finish() override;
    
    // Memory event handler
    void handleMemoryEvent(SST::Interfaces::StandardMem::Request* req);
    void handleMemoryEventFromInterface(SST::Interfaces::StandardMem::Request* req, int interface_id);

    // Remote memory request handlers
    void handle_remote_read(SST::Interfaces::StandardMem::Read* req, int interface_id);
    void handle_remote_write(SST::Interfaces::StandardMem::Write* req, int interface_id);
    void handle_remote_request(SST::Interfaces::StandardMem::Request* req);

    // Memory operations
    std::vector<uint8_t> read_memory(uint64_t address, size_t size);
    void write_memory(uint64_t address, const std::vector<uint8_t>& data);

    // Lock management
    bool acquire_lock(uint64_t lock_address, uint64_t requester_id);
    void release_lock(uint64_t lock_address, uint64_t requester_id);
    bool is_lock_address(uint64_t address);

    // B+tree node management
    void store_btree_node(uint64_t address, const std::vector<uint8_t>& node_data);
    std::vector<uint8_t> load_btree_node(uint64_t address);

private:
    // Configuration
    uint32_t memory_server_id;
    uint32_t num_compute_nodes;      // How many compute nodes to accept connections from
    uint64_t memory_capacity;        // In bytes
    SimTime_t memory_latency;        // Access latency
    size_t btree_node_size;
    bool enable_locking;
    SimTime_t lock_timeout;
    int verbose_level;

    // Memory storage
    std::unordered_map<uint64_t, MemoryBlock> memory_blocks;
    uint64_t memory_used;            // Bytes currently used
    uint64_t base_address;           // Base address for this memory server

    // Lock management
    std::unordered_map<uint64_t, NodeLock> node_locks;
    
    // Memory interfaces (multiple for accepting connections from different compute servers)
    SST::Interfaces::StandardMem* mem_interface;  // Primary interface
    std::vector<SST::Interfaces::StandardMem*> mem_interfaces;  // Additional interfaces
    std::vector<SST::Interfaces::StandardMem*> all_mem_interfaces;  // All interfaces for easy lookup
    std::unordered_map<SST::Interfaces::StandardMem*, int> interface_to_id;  // Map interface pointer to ID

    // Statistics
    Statistic<uint64_t>* stat_network_reads;
    Statistic<uint64_t>* stat_network_writes;
    Statistic<uint64_t>* stat_memory_reads;
    Statistic<uint64_t>* stat_memory_writes;
    Statistic<uint64_t>* stat_lock_acquisitions;
    Statistic<uint64_t>* stat_lock_releases;
    Statistic<uint64_t>* stat_lock_conflicts;
    Statistic<uint64_t>* stat_bytes_read;
    Statistic<uint64_t>* stat_bytes_written;
    Statistic<uint64_t>* stat_memory_utilization;

    // Helper functions
    uint64_t get_lock_address_for_node(uint64_t node_address);
    bool is_address_in_range(uint64_t address);
    void update_memory_stats();
    void cleanup_expired_locks();
    void send_response(SST::Interfaces::StandardMem::Request* req, bool success, int interface_id = -1);
    
    // Debug output
    Output dbg;
    Output out;
};

} // namespace MemHierarchy
} // namespace SST

#endif // _H_MEMORY_SERVER // SHERMAN_MEMORY_SERVER_H