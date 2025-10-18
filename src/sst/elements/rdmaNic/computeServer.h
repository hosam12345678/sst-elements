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

namespace SST {
namespace MemHierarchy {

// B+tree operation types
enum BTreeOp {
    BTREE_INSERT,
    BTREE_SEARCH, 
    BTREE_DELETE
};

// Workload operation structure
struct WorkloadOp {
    BTreeOp op_type;
    uint64_t key;
    uint64_t value;
    SimTime_t timestamp;
    uint64_t node_id;  // Which compute node issued this
};

// B+tree node structure (dynamic fanout)
struct BTreeNode {
    std::vector<uint64_t> keys;        // Keys (size = fanout)
    std::vector<uint64_t> values;      // Values for leaf nodes (size = fanout)
    std::vector<uint64_t> children;    // Child pointers for internal nodes (size = fanout+1)
    uint32_t num_keys;                 // Number of keys currently in node
    uint32_t fanout;                   // Maximum keys per node
    bool is_leaf;                      // Leaf or internal node
    uint64_t node_address;             // Address in memory server
    
    // Constructor
    BTreeNode(uint32_t fanout_size = 16) : fanout(fanout_size) {
        keys.resize(fanout);
        values.resize(fanout);
        children.resize(fanout + 1);
        num_keys = 0;
        is_leaf = true;
        node_address = 0;
    }
    
    // Lock is located at the beginning of the node (no separate address)
    // Lock offset: 0
    // Data offset: sizeof(lock)
};

class ComputeServer : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(
        ComputeServer,
        "rdmaNic",
        "computeServer",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Disaggregated memory compute server - generates B+tree workloads using RDMA",
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

    SST_ELI_DOCUMENT_PORTS(
        {"rdma_nic_0", "RDMA NIC connection to memory server 0 (dedicated instance)", {}},
        {"rdma_nic_1", "RDMA NIC connection to memory server 1 (dedicated instance)", {}},
        {"rdma_nic_2", "RDMA NIC connection to memory server 2 (dedicated instance)", {}},
        {"rdma_nic_3", "RDMA NIC connection to memory server 3 (dedicated instance)", {}},
        {"rdma_nic_4", "RDMA NIC connection to memory server 4 (dedicated instance)", {}},
        {"rdma_nic_5", "RDMA NIC connection to memory server 5 (dedicated instance)", {}},
        {"rdma_nic_6", "RDMA NIC connection to memory server 6 (dedicated instance)", {}},
        {"rdma_nic_7", "RDMA NIC connection to memory server 7 (dedicated instance)", {}}
    )
    SST_ELI_DOCUMENT_STATISTICS(
        {"btree_inserts", "Number of B+tree insert operations", "operations", 1},
        {"btree_searches", "Number of B+tree search operations", "operations", 1},
        {"btree_deletes", "Number of B+tree delete operations", "operations", 1},
        {"rdma_reads", "Number of RDMA read operations", "operations", 1},
        {"rdma_writes", "Number of RDMA write operations", "operations", 1},
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
    // These implement B+tree logic using RDMA operations below
    void btree_insert(uint64_t key, uint64_t value);
    void btree_search(uint64_t key);
    void btree_delete(uint64_t key);

    // ===== Low-level RDMA operations =====
    // These issue actual RDMA requests to memory servers
    void rdma_read(uint64_t remote_address, size_t size, uint64_t local_buffer);
    void rdma_write(uint64_t remote_address, void* data, size_t size);

    // Workload generation
    void generate_workload();
    WorkloadOp generate_next_operation();
    uint64_t get_zipfian_key();

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
    std::mt19937 rng;
    std::uniform_real_distribution<double> uniform_dist;
    std::vector<uint64_t> key_frequencies;  // Track key access frequency

    // B+tree state (local cache of tree structure)
    uint64_t root_address;
    uint32_t tree_height;                        // Current height of the tree
    uint64_t next_node_id;                       // Counter for allocating node IDs
    std::map<uint64_t, BTreeNode> cached_nodes;  // Simple cache
    std::map<uint64_t, uint64_t> parent_map;     // Maps child_address → parent_address (for split operations)

    // RDMA interfaces (multiple for connecting to different memory servers)
    SST::Interfaces::StandardMem* rdma_interface;  // Primary interface
    std::vector<SST::Interfaces::StandardMem*> rdma_interfaces;  // interfaces
    
    // Statistics
    Statistic<uint64_t>* stat_inserts;
    Statistic<uint64_t>* stat_searches;
    Statistic<uint64_t>* stat_deletes;
    Statistic<uint64_t>* stat_rdma_reads;
    Statistic<uint64_t>* stat_rdma_writes;
    Statistic<uint64_t>* stat_total_latency;
    Statistic<uint64_t>* stat_ops_completed;

    // Timing
    SST::Clock::HandlerBase* clock_handler;
    SimTime_t last_op_time;
    
    // Outstanding operations tracking
    std::map<uint64_t, SimTime_t> outstanding_reads;
    std::map<uint64_t, SimTime_t> outstanding_writes;
    
    // Helper functions
    uint64_t hash_key_to_memory_server(uint64_t key);
    uint64_t allocate_node_address(uint64_t node_id, uint32_t level);
    uint64_t get_lock_offset();  // Lock is at offset 0 within the node
    SST::Interfaces::StandardMem* get_rdma_interface_for_address(uint64_t address);
    void process_btree_operation(const WorkloadOp& op);
    
    // B+tree structure management
    void initialize_btree();
    uint64_t calculate_tree_height(uint64_t num_keys);
    uint64_t get_child_index_for_key(const BTreeNode& node, uint64_t key);
    BTreeNode traverse_to_leaf(uint64_t key);  // Returns leaf node (with data) for key
    BTreeNode parse_node_from_buffer(uint64_t address, uint64_t buffer_address);  // Deserialize node from RDMA buffer
    void split_leaf(BTreeNode& old_leaf, uint64_t new_key, uint64_t new_value);  // Handle leaf split
    void insert_into_parent(uint64_t old_node_addr, uint64_t separator_key, uint64_t new_node_addr, uint32_t level);  // Update parent after split
    void split_internal(BTreeNode& old_internal, uint64_t new_key, uint64_t new_child_addr, uint32_t level);  // Handle internal node split
    uint64_t find_parent_address(uint64_t child_address, uint32_t child_level);  // Find parent of a node
    
    // Debug output
    Output dbg;
    Output out;
};

} // namespace MemHierarchy
} // namespace SST

#endif // _H_COMPUTE_SERVER