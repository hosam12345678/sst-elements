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

#ifndef _H_BTREE_NODE
#define _H_BTREE_NODE

#include <vector>
#include <cstdint>

namespace SST {
namespace MemHierarchy {

// ═══════════════════════════════════════════════════════════════════════════
// B+TREE NODE STRUCTURE
// ═══════════════════════════════════════════════════════════════════════════

/**
 * B+tree node structure with dynamic fanout
 * 
 * Memory Layout:
 * ┌─────────────────────────────────────────────────────────────┐
 * │ Lock Header (8 bytes)                                        │
 * ├─────────────────────────────────────────────────────────────┤
 * │ Node Metadata (num_keys, fanout, is_leaf, address)          │
 * ├─────────────────────────────────────────────────────────────┤
 * │ Keys Array (fanout * 8 bytes)                               │
 * ├─────────────────────────────────────────────────────────────┤
 * │ Values Array (fanout * 8 bytes) - for leaf nodes            │
 * ├─────────────────────────────────────────────────────────────┤
 * │ Children Pointers ((fanout+1) * 8 bytes) - for internal     │
 * └─────────────────────────────────────────────────────────────┘
 */
struct BTreeNode {
    std::vector<uint64_t> keys;        // Keys (size = fanout)
    std::vector<uint64_t> values;      // Values for leaf nodes (size = fanout)
    std::vector<uint64_t> children;    // Child pointers for internal nodes (size = fanout+1)
    uint32_t num_keys;                 // Number of keys currently in node
    uint32_t fanout;                   // Maximum keys per node
    bool is_leaf;                      // Leaf or internal node
    uint64_t node_address;             // Address in memory server
    uint64_t next_leaf;                // Next leaf pointer (for leaf nodes only)
    
    // Constructor
    BTreeNode(uint32_t fanout_size = 16) : fanout(fanout_size) {
        keys.resize(fanout);
        values.resize(fanout);
        children.resize(fanout + 1);
        num_keys = 0;
        is_leaf = true;
        node_address = 0;
        next_leaf = 0;  // 0 means no next leaf
    }
    
    // Lock header size (prepended to serialized node)
    static constexpr size_t LOCK_HEADER_SIZE = 8;
};

// ═══════════════════════════════════════════════════════════════════════════
// ASYNC OPERATION STATE MACHINE
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Tracks state for multi-step asynchronous B+tree operations
 * 
 * Operations flow through state machines:
 * - INSERT: Traversal → Leaf Insert → (Split if needed)
 * - SEARCH: Traversal → Leaf Search
 * - SPLIT: Write Old → Write New → Acquire Parent Lock → Update Parent
 */
struct AsyncOperation {
    // Operation types
    enum Type { 
        TRAVERSAL,        // Walking down the tree
        INSERT,           // Inserting a key-value pair
        SEARCH,           // Searching for a key
        SPLIT_LEAF,       // Splitting a leaf node
        SPLIT_INTERNAL,   // Splitting an internal node
        UPDATE_PARENT     // Updating parent after split
    };
    
    // Split operation phases
    enum SplitPhase { 
        NONE,                   // Not in a split
        WRITE_OLD_NODE,         // Phase 1: Write split old node
        WRITE_NEW_NODE,         // Phase 2: Write split new node
        ACQUIRE_PARENT_LOCK,    // Phase 3: Acquire exclusive lock on parent
        UPDATE_PARENT_NODE      // Phase 4: Update parent with separator key
    };
    
    // Operation state
    Type type;                          // What operation is this?
    uint64_t key;                       // Key being operated on
    uint64_t value;                     // Value (for inserts)
    uint32_t current_level;             // Which tree level we're at
    uint64_t current_address;           // Current node address
    std::vector<BTreeNode> path;        // Nodes visited so far (for splits)
    uint64_t start_time;                // When operation started (SimTime_t)
    
    // Split operation state
    SplitPhase split_phase;             // Which phase of split we're in
    BTreeNode old_node;                 // Node being split
    BTreeNode new_node;                 // New node created from split
    uint64_t separator_key;             // Key to insert into parent
    uint64_t parent_address;            // Address of parent node
    bool is_root_split;                 // Is this splitting the root?
    
    // Lock tracking for distributed locking protocol
    std::vector<uint64_t> held_locks;   // Addresses of nodes we currently hold locks on
    bool waiting_for_lock;              // Are we waiting to retry lock acquisition?
    uint64_t lock_target_address;       // Address we're trying to lock
    
    // Write tracking
    bool waiting_for_write;             // Are we waiting for a write to complete?
    bool need_exclusive_lock;           // true = exclusive lock, false = shared lock
    uint32_t lock_retry_count;          // How many times we've retried
    
    // Constructor
    AsyncOperation() : type(TRAVERSAL), key(0), value(0), current_level(0), 
                      current_address(0), start_time(0), split_phase(NONE),
                      separator_key(0), parent_address(0), is_root_split(false),
                      waiting_for_lock(false), lock_target_address(0), 
                      waiting_for_write(false),
                      need_exclusive_lock(false), lock_retry_count(0) {}
};

} // namespace MemHierarchy
} // namespace SST

#endif // _H_BTREE_NODE
