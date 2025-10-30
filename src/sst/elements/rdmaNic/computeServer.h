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

// B+tree node structure (dynamic fanout) - MUST be defined before AsyncOperation
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

// Async operation tracking - state machine for multi-step operations
struct AsyncOperation {
    enum Type { TRAVERSAL, INSERT, SEARCH, DELETE, SPLIT_LEAF, SPLIT_INTERNAL, UPDATE_PARENT };
    enum SplitPhase { NONE, WRITE_OLD_NODE, WRITE_NEW_NODE, READ_PARENT, UPDATE_PARENT_NODE };
    
    Type type;                          // What operation is this?
    uint64_t key;                       // Key being operated on
    uint64_t value;                     // Value (for inserts)
    uint32_t current_level;             // Which tree level we're at
    uint64_t current_address;           // Current node address
    std::vector<BTreeNode> path;        // Nodes visited so far (for splits)
    SimTime_t start_time;               // When operation started
    
    // Split operation state
    SplitPhase split_phase;             // Which phase of split we're in
    BTreeNode old_node;                 // Node being split
    BTreeNode new_node;                 // New node created from split
    uint64_t separator_key;             // Key to insert into parent
    uint64_t parent_address;            // Address of parent node
    bool is_root_split;                 // Is this splitting the root?
    
    // Constructor
    AsyncOperation() : type(TRAVERSAL), key(0), value(0), current_level(0), 
                      current_address(0), start_time(0), split_phase(NONE),
                      separator_key(0), parent_address(0), is_root_split(false) {}
};

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
        {"btree_deletes", "Number of B+tree delete operations", "operations", 1},
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
    void btree_delete_async(uint64_t key);

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

    // B+tree state
    uint64_t root_address;
    uint32_t tree_height;                        // Current height of the tree
    uint64_t next_node_id;                       // Counter for allocating node IDs
    std::map<uint64_t, uint64_t> parent_map;     // Maps child_address → parent_address (for split operations)

    // Network interfaces (multiple for connecting to different memory servers)
    SST::Interfaces::StandardMem* memory_interface;  // Primary interface
    std::vector<SST::Interfaces::StandardMem*> memory_interfaces;  // Additional interfaces
    
    // Async operation tracking - state machine
    std::map<SST::Interfaces::StandardMem::Request::id_t, AsyncOperation> pending_ops;
    
    // Statistics
    Statistic<uint64_t>* stat_inserts;
    Statistic<uint64_t>* stat_searches;
    Statistic<uint64_t>* stat_deletes;
    Statistic<uint64_t>* stat_network_reads;
    Statistic<uint64_t>* stat_network_writes;
    Statistic<uint64_t>* stat_total_latency;
    Statistic<uint64_t>* stat_ops_completed;

    // Timing
    SST::Clock::HandlerBase* clock_handler;
    SimTime_t last_op_time;
    
    // Helper functions
    uint64_t allocate_node_address(uint64_t node_id, uint32_t level);
    SST::Interfaces::StandardMem* get_interface_for_address(uint64_t address);
    void process_btree_operation(const WorkloadOp& op);
    
    // B+tree structure management
    void initialize_btree();
    uint64_t calculate_tree_height(uint64_t num_keys);
    uint64_t get_child_index_for_key(const BTreeNode& node, uint64_t key);
    
    // Async operation handlers
    void handle_read_response(SST::Interfaces::StandardMem::Request::id_t req_id, 
                             const std::vector<uint8_t>& data);
    void handle_write_response(SST::Interfaces::StandardMem::Request::id_t req_id);
    void handle_leaf_operation(AsyncOperation& op, BTreeNode& leaf);
    
    // Async split operations
    void split_leaf_async(AsyncOperation& op, BTreeNode& leaf, uint64_t new_key, uint64_t new_value);
    void split_internal_async(AsyncOperation& op, BTreeNode& internal, uint64_t new_key, uint64_t new_child);
    void handle_split_response(AsyncOperation& op);
    void update_parent_async(uint64_t old_node_addr, uint64_t separator_key, uint64_t new_node_addr, uint32_t level);
    
    // Data serialization/deserialization
    BTreeNode deserialize_node(const std::vector<uint8_t>& data);
    std::vector<uint8_t> serialize_node(const BTreeNode& node);
    size_t get_serialized_node_size() const;
    void write_node_back(const BTreeNode& node);
    void write_node_back_with_callback(const BTreeNode& node, AsyncOperation& op);
    
    // Debug output
    Output dbg;
    Output out;
};

} // namespace MemHierarchy
} // namespace SST

#endif // _H_COMPUTE_SERVER