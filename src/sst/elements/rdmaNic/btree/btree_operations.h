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

#ifndef _H_BTREE_OPERATIONS
#define _H_BTREE_OPERATIONS

#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/statapi/stataccumulator.h>
#include <functional>
#include <map>
#include <vector>

#include "btree_node.h"
#include "btree_serializer.h"
#include "btree_locks.h"

namespace SST {
namespace MemHierarchy {

/**
 * @class BTreeOperations
 * @brief Encapsulates all B+tree operations (insert, search, split) for disaggregated memory
 * 
 * This class implements the core B+tree algorithms for a distributed B+tree stored across
 * multiple remote memory servers. All operations are asynchronous and use lock crabbing
 * for safe concurrent access.
 * 
 * B+Tree Properties:
 * - Internal nodes: Store keys and child pointers for routing
 * - Leaf nodes: Store key-value pairs (actual data)
 * - All leaves at same level (balanced tree)
 * - Lock crabbing: Hold max 2 locks during traversal (parent + child)
 * 
 * Split Protocol:
 * 1. WRITE_NEW_NODE: Write new sibling node to memory
 * 2. ACQUIRE_PARENT_LOCK: Acquire exclusive lock on parent
 * 3. UPDATE_PARENT_NODE: Read parent, add separator key + new child pointer
 * 4. Parent may recursively split if full
 */
class BTreeOperations {
public:
    /**
     * Constructor
     * @param root_addr Initial root node address
     * @param fanout Number of keys per node
     * @param verbose Verbosity level for debug output
     * @param output Output object for logging
     * NOTE: root_address and tree_height are NO LONGER stored locally!
     * They are read from ROOT_METADATA_ADDRESS at the start of each operation.
     */
    BTreeOperations(uint32_t fanout, int verbose, SST::Output* output);
    
    /**
     * Initiate async B+tree insert operation
     * @param key Key to insert
     * @param value Value to insert
     * @param op AsyncOperation to track this insert
     * @param pending_ops Map of pending operations
     * @param lock_manager Lock manager for acquiring root lock
     * @param interface_getter Callback to get memory interface for address
     * @param stat_reads Statistic counter for network reads
     */
    void btree_insert_async(
        uint64_t key,
        uint64_t value,
        AsyncOperation& op,
        std::map<SST::Interfaces::StandardMem::Request::id_t, AsyncOperation>& pending_ops,
        BTreeLockManager* lock_manager,
        std::function<SST::Interfaces::StandardMem*(uint64_t)> interface_getter,
        SST::Statistics::Statistic<uint64_t>* stat_reads);
    
    /**
     * Initiate async B+tree search operation
     * @param key Key to search for
     * @param op AsyncOperation to track this search
     * @param pending_ops Map of pending operations
     * @param lock_manager Lock manager for acquiring root lock
     * @param interface_getter Callback to get memory interface for address
     * @param stat_reads Statistic counter for network reads
     */
    void btree_search_async(
        uint64_t key,
        AsyncOperation& op,
        std::map<SST::Interfaces::StandardMem::Request::id_t, AsyncOperation>& pending_ops,
        BTreeLockManager* lock_manager,
        std::function<SST::Interfaces::StandardMem*(uint64_t)> interface_getter,
        SST::Statistics::Statistic<uint64_t>* stat_reads);

private:
    uint32_t btree_fanout_;        // Number of keys per node
    int verbose_level_;            // Verbosity for debug output
    SST::Output* out_;             // Output object for logging
    
    // NOTE: root_address and tree_height are NO LONGER stored here!
    // They are read from ROOT_METADATA_ADDRESS at the start of each operation.
};

} // namespace MemHierarchy
} // namespace SST

#endif // _H_BTREE_OPERATIONS
