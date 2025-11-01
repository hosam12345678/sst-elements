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

#include "btree_locks.h"
#include <algorithm>
#include <cstring>

namespace SST {
namespace MemHierarchy {

BTreeLockManager::BTreeLockManager(uint32_t node_id, int verbose_level, SST::Output* output)
    : node_id_(node_id), verbose_level_(verbose_level), out_(output) {
}

SST::Interfaces::StandardMem::Request::id_t BTreeLockManager::try_acquire_lock_async(
    AsyncOperation& op,
    uint64_t node_address,
    bool exclusive,
    SST::Interfaces::StandardMem* interface,
    std::map<SST::Interfaces::StandardMem::Request::id_t, AsyncOperation>& pending_ops) {
    
    if (verbose_level_ >= 2 && out_) {
        out_->output("   🔐 Trying to acquire %s lock on node 0x%lx (retry #%u)\n",
                     exclusive ? "EXCLUSIVE" : "SHARED", node_address, op.lock_retry_count);
    }
    
    // Set up the operation state for lock acquisition
    op.waiting_for_lock = true;
    op.lock_target_address = node_address;
    op.need_exclusive_lock = exclusive;
    op.lock_retry_count++;
    
    // Read the lock header (first 8 bytes) to attempt acquisition
    // The memory server will atomically try to acquire the lock and return the result
    auto req = new SST::Interfaces::StandardMem::Read(node_address, LOCK_HEADER_SIZE);
    auto req_id = req->getID();
    
    // Store this operation in pending_ops so we can retrieve it when response arrives
    pending_ops[req_id] = op;
    
    // Send the lock acquisition request
    interface->send(req);
    
    return req_id;
}

bool BTreeLockManager::check_lock_acquired(uint64_t lock_state, bool need_exclusive) const {
    if (need_exclusive) {
        // For exclusive lock: lock_state should have high bit set with our node_id
        uint64_t expected_exclusive = 0x8000000000000000ULL | node_id_;
        return (lock_state == expected_exclusive);
    } else {
        // For shared lock: lock_state should be > 0 and < 0x8000000000000000ULL
        return (lock_state > 0 && lock_state < 0x8000000000000000ULL);
    }
}

bool BTreeLockManager::handle_lock_response(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    const std::vector<uint8_t>& data,
    std::map<SST::Interfaces::StandardMem::Request::id_t, AsyncOperation>& pending_ops,
    SST::Interfaces::StandardMem* interface,
    size_t serialized_node_size) {
    
    auto it = pending_ops.find(req_id);
    if (it == pending_ops.end()) {
        if (out_) {
            out_->fatal(CALL_INFO, -1, "Lock response for unknown request ID\n");
        }
        return false;
    }
    
    AsyncOperation& op = it->second;
    
    // Parse the lock state from the response (8 bytes)
    uint64_t lock_state = 0;
    if (data.size() >= 8) {
        memcpy(&lock_state, data.data(), 8);
    }
    
    // Check if lock was acquired
    bool lock_acquired = check_lock_acquired(lock_state, op.need_exclusive_lock);
    
    if (lock_acquired) {
        // Success! Lock acquired
        if (verbose_level_ >= 2 && out_) {
            out_->output("   ✓ %s lock acquired on node 0x%lx\n",
                         op.need_exclusive_lock ? "EXCLUSIVE" : "SHARED", op.lock_target_address);
        }
        
        // Add to held locks list
        op.held_locks.push_back(op.lock_target_address);
        
        // Clear waiting state
        op.waiting_for_lock = false;
        op.lock_retry_count = 0;
        
        // Check if this is a parent lock acquisition during split
        if (op.split_phase == AsyncOperation::ACQUIRE_PARENT_LOCK) {
            // Split operation - acquired parent lock, now read parent node
            if (out_) {
                out_->output("   ✓ Parent lock acquired, reading parent data at 0x%lx\n", 
                            op.lock_target_address);
            }
            
            // Transition to UPDATE_PARENT_NODE phase
            op.split_phase = AsyncOperation::UPDATE_PARENT_NODE;
            
            // Read parent node (skip lock header)
            auto req = new SST::Interfaces::StandardMem::Read(
                op.lock_target_address + LOCK_HEADER_SIZE,
                serialized_node_size
            );
            auto req_id_read = req->getID();
            
            pending_ops[req_id_read] = op;
            pending_ops.erase(it);
            
            interface->send(req);
            
        } else {
            // Normal traversal lock acquisition - read node data
            if (out_) {
                out_->output("   Step 2: Reading node data at 0x%lx (lock held)\n", 
                            op.lock_target_address);
            }
            
            // Read the full node (skip the lock header)
            auto req = new SST::Interfaces::StandardMem::Read(
                op.lock_target_address + LOCK_HEADER_SIZE,  // Skip lock header
                serialized_node_size
            );
            auto req_id_read = req->getID();
            
            // Update operation in pending_ops
            pending_ops[req_id_read] = op;
            pending_ops.erase(it);  // Remove old request ID
            
            // Send read request
            interface->send(req);
        }
        
        return true;  // Lock acquired successfully
        
    } else {
        // Lock acquisition failed - need to retry
        if (verbose_level_ >= 2 && out_) {
            out_->output("   ✗ Lock denied on node 0x%lx, will retry\n", op.lock_target_address);
        }
        
        // Remove from pending_ops temporarily - we'll re-add when we retry
        AsyncOperation retry_op = op;
        pending_ops.erase(it);
        
        // Schedule a retry (immediately retry for now, could add exponential backoff)
        try_acquire_lock_async(retry_op, retry_op.lock_target_address, 
                              retry_op.need_exclusive_lock, interface, pending_ops);
        
        return false;  // Lock not acquired, retrying
    }
}

void BTreeLockManager::release_all_locks(
    AsyncOperation& op,
    std::function<SST::Interfaces::StandardMem*(uint64_t)> interface_getter) {
    
    if (op.held_locks.empty()) {
        return;  // No locks to release
    }
    
    if (verbose_level_ >= 2 && out_) {
        out_->output("   🔓 Releasing %zu held locks\n", op.held_locks.size());
    }
    
    // Release each lock by writing 0 to the lock header
    for (uint64_t lock_addr : op.held_locks) {
        std::vector<uint8_t> unlock_data(LOCK_HEADER_SIZE, 0);  // All zeros = unlock
        
        auto req = new SST::Interfaces::StandardMem::Write(lock_addr, LOCK_HEADER_SIZE, unlock_data);
        
        SST::Interfaces::StandardMem* target_interface = interface_getter(lock_addr);
        target_interface->send(req);
        
        if (verbose_level_ >= 3 && out_) {
            out_->output("      Released lock on 0x%lx\n", lock_addr);
        }
    }
    
    // Clear the held locks list
    op.held_locks.clear();
}

void BTreeLockManager::release_parent_lock_during_crabbing(
    AsyncOperation& op,
    uint64_t parent_address,
    std::function<SST::Interfaces::StandardMem*(uint64_t)> interface_getter) {
    
    // During lock crabbing, release a single parent lock after child is acquired
    // This is the "hand-over-hand" part of the protocol
    
    if (op.held_locks.empty()) {
        return;  // No locks to release
    }
    
    // Find the parent lock in our held locks list
    auto lock_it = std::find(op.held_locks.begin(), op.held_locks.end(), parent_address);
    if (lock_it == op.held_locks.end()) {
        // Parent lock not found - might have already been released
        if (verbose_level_ >= 3 && out_) {
            out_->output("   ⚠️  Parent lock 0x%lx not in held_locks (already released?)\n", 
                        parent_address);
        }
        return;
    }
    
    // Release the parent lock
    if (verbose_level_ >= 2 && out_) {
        out_->output("   🔓 Lock crabbing: releasing parent lock on 0x%lx\n", parent_address);
    }
    
    std::vector<uint8_t> unlock_data(LOCK_HEADER_SIZE, 0);
    auto unlock_req = new SST::Interfaces::StandardMem::Write(parent_address, LOCK_HEADER_SIZE, unlock_data);
    
    SST::Interfaces::StandardMem* target_interface = interface_getter(parent_address);
    target_interface->send(unlock_req);
    
    // Remove from held locks list
    op.held_locks.erase(lock_it);
}

} // namespace MemHierarchy
} // namespace SST
