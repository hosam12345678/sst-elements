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

#ifndef _H_COMPUTE_SERVER
#define _H_COMPUTE_SERVER

#include <sst/core/component.h>
#include <sst/core/link.h>
#include <sst/core/rng/mersenne.h>
#include <sst/core/statapi/stataccumulator.h>
#include <sst/core/interfaces/stdMem.h>
#include <random>
#include <map>
#include <vector>
#include <string>

// Include refactored B+tree and workload modules
#include "btree/btree_node.h"
#include "btree/btree_serializer.h"
#include "btree/btree_locks.h"
#include "btree/btree_operations.h"
#include "workload/workload_types.h"
#include "workload/workload_generator.h"

namespace SST {
namespace MemHierarchy {

class ComputeServer : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(
        ComputeServer,
        "rdmaNic",
        "computeServer",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Disaggregated memory compute server - generates B+tree workloads",
        COMPONENT_CATEGORY_PROCESSOR
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"node_id", "Compute server node ID", "0"},
        {"num_memory_nodes", "Total number of memory servers to connect to", "4"},
        {"workload_type", "Workload pattern (ycsb_a, ycsb_b, sherman_mixed)", "ycsb_a"},
        {"operations_per_second", "Target operations per second", "10000"},
        {"simulation_duration_us", "How long to run simulation", "1000000"},  // 1 second
        {"zipfian_alpha", "Zipfian distribution parameter", "0.9"},
        {"read_ratio", "Percentage of read operations (0.0-1.0)", "0.95"},
        {"btree_fanout", "B+tree fanout (keys per node)", "16"},
        {"key_range", "Range of keys (0 to key_range)", "1000000"},
        {"verbose", "Verbose debug output", "0"}
    )

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"mem_interface_0", "Memory interface to memory server 0", "SST::Interfaces::StandardMem"},
        {"mem_interface_1", "Memory interface to memory server 1", "SST::Interfaces::StandardMem"},
        {"mem_interface_2", "Memory interface to memory server 2", "SST::Interfaces::StandardMem"},
        {"mem_interface_3", "Memory interface to memory server 3", "SST::Interfaces::StandardMem"},
        {"mem_interface_4", "Memory interface to memory server 4", "SST::Interfaces::StandardMem"},
        {"mem_interface_5", "Memory interface to memory server 5", "SST::Interfaces::StandardMem"},
        {"mem_interface_6", "Memory interface to memory server 6", "SST::Interfaces::StandardMem"},
        {"mem_interface_7", "Memory interface to memory server 7", "SST::Interfaces::StandardMem"}
    )
    
    SST_ELI_DOCUMENT_STATISTICS(
        {"btree_inserts", "Number of B+tree insert operations", "operations", 1},
        {"btree_searches", "Number of B+tree search operations", "operations", 1},
        {"network_reads", "Number of remote memory read operations", "operations", 1},
        {"network_writes", "Number of remote memory write operations", "operations", 1},
        {"total_latency", "Total operation latency", "ns", 1},
        {"operations_completed", "Total operations completed", "operations", 1}
    )

    // Constructor
    ComputeServer(ComponentId_t id, Params& params);
    ~ComputeServer();

    // SST Component interface
    virtual void init(unsigned int phase) override;
    virtual void setup() override;
    virtual void finish() override;

    // Main simulation loop
    bool tick(SST::Cycle_t);
    
    // Memory event handler
    void handleMemoryEvent(SST::Interfaces::StandardMem::Request* req);

    // ===== Application-level B+tree operations =====
    // These initiate async B+tree operations
    void btree_insert_async(uint64_t key, uint64_t value);
    void btree_search_async(uint64_t key);

private:
    // Configuration
    uint32_t node_id;
    uint32_t num_memory_nodes;       // How many memory servers to connect to
    std::string workload_type;
    uint32_t ops_per_second;
    SimTime_t simulation_duration;
    double zipfian_alpha;
    double read_ratio;
    uint32_t btree_fanout;
    uint64_t key_range;
    int verbose_level;

    // Workload state
    std::queue<WorkloadOp> pending_operations;
    WorkloadGenerator* workload_gen;  // Handles workload generation
    bool tree_initialized;  // Tracks if B+tree initialization is complete
    bool checking_validity_bit;  // Tracks if we're waiting for validity bit check

    // B+tree Root Metadata (stored in memory, NOT local!)
    // The ONLY local state is whether we're currently reading metadata
    // NO local caching of root_address or tree_height - always read from memory!
    struct RootMetadata {
        uint64_t root_address;   // Address of current root node
        uint32_t tree_height;    // Current height of the tree
        uint32_t reserved;       // Reserved for future use (padding to 16 bytes)
        
        RootMetadata() : root_address(0), tree_height(1), reserved(0) {}
    };
    
    // B+tree state
    uint64_t next_node_id;                       // Counter for allocating node IDs
    std::map<uint64_t, uint64_t> parent_map;     // Maps child_address → parent_address (for split operations)
    
    // Chunk allocation tracking (per memory server)
    struct ChunkInfo {
        uint32_t chunk_id;          // Chunk ID from memory server
        uint64_t chunk_address;     // Base address of this chunk
        uint32_t nodes_used;        // Number of nodes allocated from this chunk
        
        ChunkInfo() : chunk_id(0), chunk_address(0), nodes_used(0) {}
        ChunkInfo(uint32_t id, uint64_t addr) : chunk_id(id), chunk_address(addr), nodes_used(0) {}
    };
    
    // Store all allocated chunks per memory server
    // Key: memory_server_id, Value: vector of chunks (in allocation order)
    std::map<uint32_t, std::vector<ChunkInfo>> allocated_chunks;
    
    // Round-robin memory server selection
    uint32_t current_memory_server;  // Next memory server to allocate from

    // Network interfaces (multiple for connecting to different memory servers)
    SST::Interfaces::StandardMem* memory_interface;  // Primary interface
    std::vector<SST::Interfaces::StandardMem*> memory_interfaces;  // Additional interfaces
    
    // Async operation tracking - state machine
    std::map<SST::Interfaces::StandardMem::Request::id_t, AsyncOperation> pending_ops;
    
    // B+tree serializer
    BTreeSerializer* serializer;
    
    // B+tree lock manager
    BTreeLockManager* lock_manager;
    
    // B+tree operations
    BTreeOperations* btree_ops;
    
    // Statistics
    Statistic<uint64_t>* stat_inserts;
    Statistic<uint64_t>* stat_searches;
    Statistic<uint64_t>* stat_network_reads;
    Statistic<uint64_t>* stat_network_writes;
    Statistic<uint64_t>* stat_total_latency;
    Statistic<uint64_t>* stat_ops_completed;

    // Timing
    SST::Clock::HandlerBase* clock_handler;
    SimTime_t last_op_time;
    
    // Helper functions
    uint64_t allocate_node_address();  // Allocate node address using round-robin chunk allocation
    SST::Interfaces::StandardMem* get_interface_for_address(uint64_t address);
    void process_btree_operation(const WorkloadOp& op);
    
    // ===== CHUNK ALLOCATION VIA MAGIC ADDRESS =====
    // Magic address for chunk allocation requests (matches MemoryServer constant)
    static constexpr uint64_t MAGIC_ALLOCATE_CHUNK_BASE = 0xFFFFFFFF00000000ULL;
    
    // Request chunk allocation from a specific memory server
    // Uses READ operation to magic address: (MAGIC_ALLOCATE_CHUNK_BASE | memory_server_id)
    // Response: ReadResp with [chunk_id (4 bytes)] [chunk_address (8 bytes)]
    //           or chunk_id=0xFFFFFFFF if allocation failed
    void request_chunk_allocation(uint32_t memory_server_id);
    
    // Handle chunk allocation response
    void handle_chunk_allocation_response(SST::Interfaces::StandardMem::Request::id_t req_id,
                                         SST::Interfaces::StandardMem::ReadResp* resp);
    
    // Check if a request is a special operation (chunk allocation, etc.)
    bool handle_special_operation_response(SST::Interfaces::StandardMem::Request* req);
  
    // ===== ROOT METADATA MANAGEMENT =====
    // Read root metadata (root pointer + tree height) from memory
    // Acquires SHARED lock, reads metadata, then releases lock
    void read_root_metadata_async(AsyncOperation& op);
    
    // Handle root metadata read response
    void handle_root_metadata_response(SST::Interfaces::StandardMem::Request::id_t req_id,
                                       const std::vector<uint8_t>& data);
    
    // Update root metadata (for root splits)
    // Acquires EXCLUSIVE lock, updates metadata atomically with LL/SC, then releases
    void update_root_metadata_async(SST::Interfaces::StandardMem::Request::id_t req_id,
                                    AsyncOperation& op,
                                    uint64_t new_root_address,
                                    uint32_t new_tree_height);
    
    // Handle B+tree initialization writes (root node, metadata, validity bit)
    void handle_btree_initialization_write(SST::Interfaces::StandardMem::Request::id_t req_id,
                                           AsyncOperation& op);
    
    // Serialize/deserialize root metadata
    std::vector<uint8_t> serialize_root_metadata(const RootMetadata& metadata);
    RootMetadata deserialize_root_metadata(const std::vector<uint8_t>& data);
  
    // B+tree structure management
    void initialize_btree();
    void check_tree_initialization();  // Check validity bit before starting operations
    void handle_validity_check_response(SST::Interfaces::StandardMem::Request::id_t req_id,
                                        SST::Interfaces::StandardMem::ReadResp* resp);
    uint64_t calculate_tree_height(uint64_t num_keys);
    uint64_t get_child_index_for_key(const BTreeNode& node, uint64_t key);
    
    // Async operation handlers
    void handle_read_response(SST::Interfaces::StandardMem::Request::id_t req_id, 
                             const std::vector<uint8_t>& data);
    void handle_write_response(SST::Interfaces::StandardMem::Request::id_t req_id,
                               SST::Interfaces::StandardMem::WriteResp* resp);
    void handle_leaf_operation(AsyncOperation& op, BTreeNode& leaf);
    
    // Lock protocol handlers (LL/SC)
    bool handle_lock_operations(SST::Interfaces::StandardMem::Request* req);
    bool handle_lock_acquisition(SST::Interfaces::StandardMem::Request::id_t req_id,
                                 AsyncOperation& op, SST::Interfaces::StandardMem::Request* req);
    bool handle_lock_release(SST::Interfaces::StandardMem::Request::id_t req_id,
                            AsyncOperation& op, SST::Interfaces::StandardMem::Request* req);
    
    // B+tree traversal helpers
    void handle_btree_traversal(SST::Interfaces::StandardMem::Request::id_t req_id,
                                AsyncOperation& op, BTreeNode& node);
    bool search_key_in_node(const BTreeNode& node, uint64_t key);
    
    // Leaf node operation handlers
    void handle_leaf_search(SST::Interfaces::StandardMem::Request::id_t req_id,
                           AsyncOperation& op, BTreeNode& node);
    void handle_leaf_insert(SST::Interfaces::StandardMem::Request::id_t req_id,
                           AsyncOperation& op, BTreeNode& node);
    
    // Leaf modification helpers
    void insert_into_leaf(BTreeNode& leaf, uint64_t key, uint64_t value);
    void write_leaf_and_complete(SST::Interfaces::StandardMem::Request::id_t req_id,
                                 AsyncOperation& op, BTreeNode& leaf);
    
    // Optimistic locking protocol
    void restart_insert_with_exclusive_locks(SST::Interfaces::StandardMem::Request::id_t req_id,
                                            AsyncOperation& op);
    void handle_restart_after_lock_release(SST::Interfaces::StandardMem::Request::id_t req_id,
                                           AsyncOperation& op);
    
    // Split operation handlers
    void handle_leaf_split(SST::Interfaces::StandardMem::Request::id_t req_id,
                          AsyncOperation& op, BTreeNode& node);
    void handle_internal_split(SST::Interfaces::StandardMem::Request::id_t req_id,
                               AsyncOperation& op, BTreeNode& node);
    void handle_root_split(SST::Interfaces::StandardMem::Request::id_t req_id,
                          AsyncOperation& op);
    void write_split_nodes(SST::Interfaces::StandardMem::Request::id_t req_id,
                          AsyncOperation& op);
    void update_parent_after_split(SST::Interfaces::StandardMem::Request::id_t req_id,
                                   AsyncOperation& op);
    
    // Write response handlers (split into modular functions)
    void handle_split_write_response(SST::Interfaces::StandardMem::Request::id_t req_id,
                                     AsyncOperation& op);
    void handle_root_split_write_response(SST::Interfaces::StandardMem::Request::id_t req_id,
                                          AsyncOperation& op);
    void handle_simple_write_completion(SST::Interfaces::StandardMem::Request::id_t req_id,
                                        AsyncOperation& op);
    
    // Internal node modification helpers
    void insert_into_internal_node(BTreeNode& internal, uint64_t key, uint64_t right_child);
    void write_parent_and_complete(SST::Interfaces::StandardMem::Request::id_t req_id,
                                   AsyncOperation& op, BTreeNode& parent);
    
    // Async split operations
    void split_leaf_async(AsyncOperation& op, BTreeNode& leaf, uint64_t new_key, uint64_t new_value);
    void split_internal_async(AsyncOperation& op, BTreeNode& internal, uint64_t new_key, uint64_t new_child);
    void handle_split_response(AsyncOperation& op);
    void update_parent_async(uint64_t old_node_addr, uint64_t separator_key, uint64_t new_node_addr, uint32_t level);
    
    // Node writing helpers
    void write_node_back(const BTreeNode& node);
    void write_node_back_with_callback(const BTreeNode& node, AsyncOperation& op);
    
    // Helper to get serialized size (delegates to serializer)
    inline size_t get_serialized_node_size() const {
        return serializer ? serializer->get_serialized_size() : 0;
    }
    
    // Debug output
    Output dbg;
    Output out;
};

} // namespace MemHierarchy
} // namespace SST

#endif // _H_COMPUTE_SERVER