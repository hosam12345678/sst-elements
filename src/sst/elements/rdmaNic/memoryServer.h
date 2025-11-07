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
#include <cstring>  // For memcpy of lock data

namespace SST {
namespace MemHierarchy {

// Forward declarations
class LockRequestEvent;
class LockResponseEvent;

// Memory block structure for storing B+tree nodes
struct MemoryBlock {
    uint64_t address;
    std::vector<uint8_t> data;
    SimTime_t last_access;
    uint64_t access_count;
};

// ===== IN-MEMORY LOCK STRUCTURE =====
// Lock is embedded at the beginning of each B+tree node
// Memory layout: [Lock (8 bytes)] [Node Data (remaining bytes)]
//
// Lock encoding (64-bit value):
// - 0                : Unlocked
// - 1-0x7FFFFFFFFFFFFFFF : Shared lock (reader count)
// - 0x8000000000000000 | compute_id : Exclusive lock (high bit + owner ID)
struct NodeLock {
    uint64_t state;
    
    NodeLock() : state(0) {}
    
    static constexpr uint64_t EXCLUSIVE_BIT = 0x8000000000000000ULL;
    static constexpr uint64_t OWNER_MASK = ~EXCLUSIVE_BIT;
    
    bool is_unlocked() const { return state == 0; }
    bool is_shared() const { return state > 0 && !(state & EXCLUSIVE_BIT); }
    bool is_exclusive() const { return (state & EXCLUSIVE_BIT) != 0; }
    uint64_t get_owner() const { return state & OWNER_MASK; }
    uint64_t get_reader_count() const { return is_shared() ? state : 0; }
    
    static uint64_t make_exclusive(uint64_t compute_id) { 
        return EXCLUSIVE_BIT | compute_id; 
    }
};

// Size of lock header in bytes
constexpr size_t LOCK_HEADER_SIZE = sizeof(uint64_t);  // 8 bytes

// Lock operation types
enum LockOperation {
    LOCK_TRY_ACQUIRE_SHARED,      // Try to acquire shared lock (for reads)
    LOCK_TRY_ACQUIRE_EXCLUSIVE,   // Try to acquire exclusive lock (for writes)
    LOCK_RELEASE_SHARED,          // Release shared lock
    LOCK_RELEASE_EXCLUSIVE,       // Release exclusive lock
    LOCK_SUCCESS,                 // Lock operation succeeded (response)
    LOCK_FAILED                   // Lock operation failed (response)
};

class MemoryServer : public SST::Component {
public:
    // ===== PUBLIC MEMORY LAYOUT CONSTANTS =====
    static constexpr uint64_t RESERVED_METADATA_SIZE = 0x1000;  // 4KB reserved at start of each memory server
    static constexpr uint64_t CHUNK_SIZE = 8 * 1024 * 1024;     // 8MB per chunk
    static constexpr uint64_t CHUNKS_PER_SERVER = 128;          // 128 chunks = 1GB
    static constexpr uint64_t ADDRESS_SPACE_PER_SERVER = RESERVED_METADATA_SIZE + (CHUNKS_PER_SERVER * CHUNK_SIZE);
    
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
        {"verbose", "Verbose debug output", "0"}
    )

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"mem_interface_0", "Memory interface from compute server 0", "SST::Interfaces::StandardMem"},
        {"mem_interface_1", "Memory interface from compute server 1", "SST::Interfaces::StandardMem"},
        {"mem_interface_2", "Memory interface from compute server 2", "SST::Interfaces::StandardMem"},
        {"mem_interface_3", "Memory interface from compute server 3", "SST::Interfaces::StandardMem"},
        {"mem_interface_4", "Memory interface from compute server 4", "SST::Interfaces::StandardMem"},
        {"mem_interface_5", "Memory interface from compute server 5", "SST::Interfaces::StandardMem"},
        {"mem_interface_6", "Memory interface from compute server 6", "SST::Interfaces::StandardMem"},
        {"mem_interface_7", "Memory interface from compute server 7", "SST::Interfaces::StandardMem"}
    )

    SST_ELI_DOCUMENT_STATISTICS(
        {"network_reads_received", "Number of remote read requests received", "requests", 1},
        {"network_writes_received", "Number of remote write requests received", "requests", 1},
        {"memory_reads", "Number of local memory reads", "reads", 1},
        {"memory_writes", "Number of local memory writes", "writes", 1},
        {"bytes_read", "Total bytes read from memory", "bytes", 1},
        {"bytes_written", "Total bytes written to memory", "bytes", 1},
        {"memory_utilization", "Memory utilization percentage", "percent", 1},
        {"locks_acquired", "Number of locks acquired", "locks", 1},
        {"locks_released", "Number of locks released", "locks", 1},
        {"lock_conflicts", "Number of lock conflicts (already held)", "conflicts", 1},
        {"lock_wait_time", "Total time spent waiting for locks", "nanoseconds", 1}
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
    void handle_loadlink(SST::Interfaces::StandardMem::LoadLink* req, int interface_id);
    void handle_storeconditional(SST::Interfaces::StandardMem::StoreConditional* req, int interface_id);

    // ===== IN-MEMORY LOCK OPERATIONS =====
    // Shared lock operations (for SEARCH/reads)
    bool try_acquire_shared_lock(uint64_t node_address);
    bool release_shared_lock(uint64_t node_address);
    
    // Exclusive lock operations (for INSERT/writes)
    bool try_acquire_exclusive_lock(uint64_t node_address, uint64_t compute_id);
    bool release_exclusive_lock(uint64_t node_address, uint64_t compute_id);
    
    // Lock state queries (for debugging/verification)
    uint64_t read_lock_state(uint64_t node_address);
    bool is_locked_shared(uint64_t node_address);
    bool is_locked_exclusive(uint64_t node_address);
    
    // Helper: get lock address for a node
    uint64_t get_lock_address(uint64_t node_address) { return node_address; }
    uint64_t get_data_address(uint64_t node_address) { return node_address + LOCK_HEADER_SIZE; }

    // Memory operations
    std::vector<uint8_t> read_memory(uint64_t address, size_t size);
    void write_memory(uint64_t address, const std::vector<uint8_t>& data);
    
    // Chunk allocation
    int32_t allocate_chunk();  // Returns chunk_id or -1 if no free chunks
    void free_chunk(uint32_t chunk_id);
    uint64_t chunk_id_to_address(uint32_t chunk_id);  // Convert chunk_id to base address
    
    // ===== MAGIC ADDRESS PROTOCOL =====
    // Special addresses for chunk management operations
    static constexpr uint64_t MAGIC_ALLOCATE_CHUNK_BASE = 0xFFFFFFFF00000000ULL;
    // Request: READ from (MAGIC_ALLOCATE_CHUNK_BASE | memory_server_id)
    // Response: ReadResp contains chunk_id (32-bit) or 0xFFFFFFFF if failed
    
    void handle_magic_allocate_chunk(SST::Interfaces::StandardMem::Read* req, int interface_id);

private:
    // Configuration
    uint32_t memory_server_id;
    uint32_t num_compute_nodes;      // How many compute nodes to accept connections from
    int verbose_level;

    // Memory storage
    std::unordered_map<uint64_t, MemoryBlock> memory_blocks;
    uint64_t memory_used;            // Bytes currently used
    uint64_t base_address;           // Base address for this memory server
    
    // ===== CHUNK ALLOCATION =====
    // Each memory server manages chunks (8MB each) for B+tree node allocation
    std::vector<bool> chunk_allocated;  // Bitmap: true if chunk is allocated
    uint64_t next_free_chunk_hint;      // Hint for next free chunk (optimization)
    
    // ===== LOCK STATISTICS =====
    // No separate lock table - locks are embedded in memory
    uint64_t total_locks_acquired;   // Statistics
    uint64_t total_locks_released;
    uint64_t total_lock_conflicts;   // Times lock acquisition failed (already held)
    
    // ===== LOAD-LINK/STORE-CONDITIONAL TRACKING =====
    // Track LoadLink reservations for atomic operations
    // Maps address -> map of (interface_id -> reservation_value)
    // Multiple nodes can have LL reservations on the same address
    // Any write to an address invalidates ALL reservations except the writer's
    std::unordered_map<uint64_t, std::unordered_map<int, uint64_t>> ll_reservations;
    
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
    Statistic<uint64_t>* stat_bytes_read;
    Statistic<uint64_t>* stat_bytes_written;
    Statistic<uint64_t>* stat_memory_utilization;
    
    // Lock statistics
    Statistic<uint64_t>* stat_locks_acquired;
    Statistic<uint64_t>* stat_locks_released;
    Statistic<uint64_t>* stat_lock_conflicts;
    Statistic<uint64_t>* stat_lock_wait_time;

    // Helper functions
    bool is_address_in_range(uint64_t address);
    void update_memory_stats();
    void send_response(SST::Interfaces::StandardMem::Request* req, bool success, int interface_id = -1);
    
    // Debug output
    Output dbg;
    Output out;
};

} // namespace MemHierarchy
} // namespace SST

#endif // _H_MEMORY_SERVER // SHERMAN_MEMORY_SERVER_H