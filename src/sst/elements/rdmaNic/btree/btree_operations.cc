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

#include "btree_operations.h"
#include <algorithm>

namespace SST {
namespace MemHierarchy {

BTreeOperations::BTreeOperations(uint32_t fanout, int verbose, SST::Output* output)
    : btree_fanout_(fanout), verbose_level_(verbose), out_(output) {
    // NOTE: root_address and tree_height are NO LONGER stored locally!
    // They are read from ROOT_METADATA_ADDRESS at the start of each operation.
}

void BTreeOperations::btree_insert_async(
    uint64_t key,
    uint64_t value,
    AsyncOperation& op,
    std::map<SST::Interfaces::StandardMem::Request::id_t, AsyncOperation>& pending_ops,
    BTreeLockManager* lock_manager,
    std::function<SST::Interfaces::StandardMem*(uint64_t)> interface_getter,
    SST::Statistics::Statistic<uint64_t>* stat_reads) {
    
    if (verbose_level_ >= 1 && out_) {
        out_->output("\n🔹 btree_insert_async (async): key=%lu, value=%lu\n", key, value);
    }
    
    // Don't modify op.current_level or op.current_address here
    // They are set by the caller (ComputeServer::btree_insert_async for new ops,
    // or handle_btree_traversal for continuation)
    
    // Optimistic locking: Use shared locks during traversal, exclusive at leaf
    // If pessimistic_mode is set (after restart), use exclusive locks throughout
    
    // Detect if we're about to access a leaf node
    // op.current_level points to the level we're ABOUT TO access (already incremented)
    // NOTE: tree_height is stored in op.tree_height (read from metadata at operation start)
    bool accessing_leaf = (op.current_level >= op.tree_height - 1) || 
                         (op.current_level == 0 && op.tree_height == 1);
    
    bool use_exclusive_lock = op.pessimistic_mode || accessing_leaf;
    
    if (out_) {
        if (op.pessimistic_mode) {
            out_->output("   Step 1: Acquiring EXCLUSIVE lock on node=0x%lx (level=%u) [PESSIMISTIC MODE]\n", 
                        op.current_address, op.current_level);
        } else if (accessing_leaf) {
            out_->output("   Step 1: Acquiring EXCLUSIVE lock on node=0x%lx (level=%u) [LEAF - need write access]\n", 
                        op.current_address, op.current_level);
        } else {
            out_->output("   Step 1: Acquiring SHARED lock on node=0x%lx (level=%u) [OPTIMISTIC MODE]\n", 
                        op.current_address, op.current_level);
        }
    }
    
    SST::Interfaces::StandardMem* interface = interface_getter(op.current_address);
    lock_manager->try_acquire_lock_async(op, op.current_address, use_exclusive_lock, interface, pending_ops);
    stat_reads->addData(1);
}

void BTreeOperations::btree_search_async(
    uint64_t key,
    AsyncOperation& op,
    std::map<SST::Interfaces::StandardMem::Request::id_t, AsyncOperation>& pending_ops,
    BTreeLockManager* lock_manager,
    std::function<SST::Interfaces::StandardMem*(uint64_t)> interface_getter,
    SST::Statistics::Statistic<uint64_t>* stat_reads) {
    
    if (verbose_level_ >= 1 && out_) {
        out_->output("\n🔍 SEARCH Operation (async): key=%lu\n", key);
    }
    
    // Don't modify op.current_level or op.current_address here
    // They are set by the caller (ComputeServer::btree_search_async for new ops,
    // or handle_btree_traversal for continuation)
    
    // Acquire SHARED lock on the current node (SEARCH only needs read access)
    if (out_) {
        out_->output("   Step 1: Acquiring SHARED lock on node=0x%lx (level=%u)\n", 
                    op.current_address, op.current_level);
    }
    
    SST::Interfaces::StandardMem* interface = interface_getter(op.current_address);
    lock_manager->try_acquire_lock_async(op, op.current_address, false, interface, pending_ops);
    stat_reads->addData(1);
}

} // namespace MemHierarchy
} // namespace SST
