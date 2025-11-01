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

BTreeOperations::BTreeOperations(uint64_t& root_addr, uint32_t fanout, int verbose, SST::Output* output)
    : root_address_(root_addr), btree_fanout_(fanout), verbose_level_(verbose), out_(output) {
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
        out_->output("\n🔹 INSERT Operation (async): key=%lu, value=%lu\n", key, value);
    }
    
    // Setup operation
    op.type = AsyncOperation::INSERT;
    op.key = key;
    op.value = value;
    op.current_level = 0;
    op.current_address = root_address_;
    
    // Acquire EXCLUSIVE lock on root node (INSERT requires write access)
    if (out_) {
        out_->output("   Step 1: Acquiring EXCLUSIVE lock on root=0x%lx\n", root_address_);
    }
    
    SST::Interfaces::StandardMem* interface = interface_getter(root_address_);
    lock_manager->try_acquire_lock_async(op, root_address_, true, interface, pending_ops);
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
    
    // Setup operation
    op.type = AsyncOperation::SEARCH;
    op.key = key;
    op.current_level = 0;
    op.current_address = root_address_;
    
    // Acquire SHARED lock on root node (SEARCH only needs read access)
    if (out_) {
        out_->output("   Step 1: Acquiring SHARED lock on root=0x%lx\n", root_address_);
    }
    
    SST::Interfaces::StandardMem* interface = interface_getter(root_address_);
    lock_manager->try_acquire_lock_async(op, root_address_, false, interface, pending_ops);
    stat_reads->addData(1);
}

} // namespace MemHierarchy
} // namespace SST
