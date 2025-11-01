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

#ifndef _H_BTREE_LOCKS
#define _H_BTREE_LOCKS

#include "btree_node.h"
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/output.h>
#include <map>
#include <vector>
#include <cstdint>

namespace SST {
namespace MemHierarchy {

/**
 * BTreeLockManager - Manages distributed locks for B+tree nodes
 * 
 * Lock Protocol:
 * - Each node has an 8-byte lock header at offset 0
 * - Lock states:
 *   - 0x0000000000000000 = Unlocked
 *   - 0x0000000000000001 - 0x7FFFFFFFFFFFFFFF = Shared lock (count)
 *   - 0x8000000000000000 | node_id = Exclusive lock (owned by node_id)
 * 
 * Lock Crabbing:
 * - Acquire lock on child before releasing parent
 * - Hand-over-hand traversal ensures deadlock freedom
 * - At most 2 locks held during transition
 * 
 * Lock Types:
 * - Shared (read): Multiple nodes can hold simultaneously
 * - Exclusive (write): Only one node can hold
 */
class BTreeLockManager {
public:
    /**
     * Constructor
     * @param node_id ID of this compute node
     * @param verbose_level Debug output verbosity (0-3)
     * @param output SST Output object for logging
     */
    BTreeLockManager(uint32_t node_id, int verbose_level, SST::Output* output);
    
    /**
     * Attempt to acquire a lock on a node (initiates async request)
     * @param op AsyncOperation to track lock acquisition
     * @param node_address Address of node to lock
     * @param exclusive true for exclusive lock, false for shared lock
     * @param interface Network interface to send request through
     * @param pending_ops Map of pending operations (to store this request)
     * @return Request ID for tracking
     */
    SST::Interfaces::StandardMem::Request::id_t try_acquire_lock_async(
        AsyncOperation& op,
        uint64_t node_address,
        bool exclusive,
        SST::Interfaces::StandardMem* interface,
        std::map<SST::Interfaces::StandardMem::Request::id_t, AsyncOperation>& pending_ops);
    
    /**
     * Handle response to a lock acquisition attempt
     * @param req_id Request ID of the lock attempt
     * @param data Response data (lock state)
     * @param pending_ops Map of pending operations
     * @param interface Network interface for subsequent requests
     * @param serialized_node_size Size of serialized node (for reading after lock)
     * @return true if lock acquired, false if retry needed
     */
    bool handle_lock_response(
        SST::Interfaces::StandardMem::Request::id_t req_id,
        const std::vector<uint8_t>& data,
        std::map<SST::Interfaces::StandardMem::Request::id_t, AsyncOperation>& pending_ops,
        SST::Interfaces::StandardMem* interface,
        size_t serialized_node_size);
    
    /**
     * Release all locks held by an operation
     * @param op Operation whose locks should be released
     * @param interface_getter Function to get network interface for an address
     */
    void release_all_locks(
        AsyncOperation& op,
        std::function<SST::Interfaces::StandardMem*(uint64_t)> interface_getter);
    
    /**
     * Release a single parent lock during lock crabbing
     * @param op Operation performing lock crabbing
     * @param parent_address Address of parent lock to release
     * @param interface_getter Function to get network interface for an address
     */
    void release_parent_lock_during_crabbing(
        AsyncOperation& op,
        uint64_t parent_address,
        std::function<SST::Interfaces::StandardMem*(uint64_t)> interface_getter);
    
    /**
     * Get lock header size
     * @return Size of lock header in bytes
     */
    static constexpr size_t get_lock_header_size() {
        return LOCK_HEADER_SIZE;
    }
    
private:
    uint32_t node_id_;           // ID of this compute node
    int verbose_level_;          // Debug output verbosity
    SST::Output* out_;           // Output for logging
    
    static constexpr size_t LOCK_HEADER_SIZE = 8;  // 8-byte lock header
    
    // Helper to check if lock was acquired
    bool check_lock_acquired(uint64_t lock_state, bool need_exclusive) const;
};

} // namespace MemHierarchy
} // namespace SST

#endif // _H_BTREE_LOCKS
