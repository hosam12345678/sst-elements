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

    
    // Set up the operation state for lock acquisition
    op.waiting_for_loadlink_response = true;
    op.lock_target_address = node_address;
    op.need_exclusive_lock = exclusive;
    op.llsc_retry_count++;
    
    if (verbose_level_ >= 2 && out_) {
        out_->output("   🔐 Trying to acquire %s lock on node 0x%lx (LL/SC attempt #%u)\n",
                     exclusive ? "EXCLUSIVE" : "SHARED", node_address, op.llsc_retry_count);
    }
    
    // Step 1: Use LoadLink to atomically read lock state and track for interference
    // LoadLink returns ReadResp (like Read) but enables StoreConditional to detect races
    auto req = new SST::Interfaces::StandardMem::LoadLink(node_address, LOCK_HEADER_SIZE);
    auto req_id = req->getID();
    
    // Store this operation in pending_ops so we can retrieve it when response arrives
    pending_ops[req_id] = op;
    
    // Send the LoadLink request
    interface->send(req);
    
    return req_id;
}

bool BTreeLockManager::check_lock_acquired(uint64_t lock_state, bool need_exclusive) const {
    if (need_exclusive) {
        // For exclusive lock: lock_state should have high bit set with our node_id
        uint64_t expected_exclusive = 0x8000000000000000ULL | node_id_;
        return (lock_state == expected_exclusive);
    } else {
        // For shared lock: lock_state should be > 0 and < 0x8000000000000000ULL (reference count)
        // Any positive value without exclusive bit means shared lock(s) exist
        return (lock_state > 0 && (lock_state & 0x8000000000000000ULL) == 0);
    }
}

bool BTreeLockManager::handle_loadlink_response(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    const std::vector<uint8_t>& data,
    std::map<SST::Interfaces::StandardMem::Request::id_t, AsyncOperation>& pending_ops,
    SST::Interfaces::StandardMem* interface,
    size_t serialized_node_size) {
    
    auto it = pending_ops.find(req_id);
    if (it == pending_ops.end()) {
        if (out_) {
            out_->fatal(CALL_INFO, -1, "LoadLink response for unknown request ID\n");
        }
        return false;
    }
    
    AsyncOperation& op = it->second;
    
    // Parse the current lock state from LoadLink response (8 bytes)
    uint64_t lock_state = 0;
    if (data.size() >= 8) {
        memcpy(&lock_state, data.data(), 8);
    }
    
    // Store the lock value for debugging and verification
    op.llsc_lock_value = lock_state;
    
    // Check if lock is FREE or can be acquired
    bool lock_is_free = (lock_state == 0);
    bool is_exclusive_lock = (lock_state & 0x8000000000000000ULL) != 0;
    bool can_acquire_shared = !is_exclusive_lock && !op.need_exclusive_lock;
    
    if (lock_is_free || can_acquire_shared) {
        // We can acquire the lock!
        if (verbose_level_ >= 2 && out_) {
            if (lock_is_free) {
                out_->output("   🔓 Lock is free (LL=%lu), acquiring %s lock on node 0x%lx\n",
                             lock_state, op.need_exclusive_lock ? "EXCLUSIVE" : "SHARED", op.lock_target_address);
            } else {
                out_->output("   🔓 Lock is shared (LL count=%lu), adding shared lock on node 0x%lx\n",
                             lock_state, op.lock_target_address);
            }
        }
        
        // Prepare lock state to write
        uint64_t new_lock_state;
        if (op.need_exclusive_lock) {
            // Exclusive lock: set high bit + node_id
            new_lock_state = 0x8000000000000000ULL | node_id_;
        } else {
            // Shared lock: increment reference count
            if (lock_is_free) {
                new_lock_state = 1;  // First shared holder
            } else {
                new_lock_state = lock_state + 1;  // Increment reference count
            }
        }
        
        // Use StoreConditional to atomically write ONLY if no interference since LoadLink
        // SC will fail if another node modified the lock between our LL and SC
        std::vector<uint8_t> lock_data(8);
        memcpy(lock_data.data(), &new_lock_state, 8);
        auto sc_req = new SST::Interfaces::StandardMem::StoreConditional(op.lock_target_address, 8, lock_data);
        
        // Mark that we're waiting for StoreConditional response
        op.waiting_for_loadlink_response = false;
        op.waiting_for_sc_response = true;
        auto sc_req_id = sc_req->getID();
        pending_ops[sc_req_id] = op;
        pending_ops.erase(it);  // Remove the LoadLink request ID
        
        interface->send(sc_req);
        return false;  // Not yet complete, waiting for SC response
    } else if (check_lock_acquired(lock_state, op.need_exclusive_lock)) {
        // Lock was already acquired by us (from a previous write)
        if (verbose_level_ >= 2 && out_) {
            out_->output("   ✓ %s lock confirmed on node 0x%lx\n",
                         op.need_exclusive_lock ? "EXCLUSIVE" : "SHARED", op.lock_target_address);
        }
        
        // NOTE: We do NOT add to held_locks here because check at start of
        // try_acquire_lock_async() should have caught this case.
        // If we reach here, it means the lock was already held from outside this operation.
        // This should be safe - just proceed with reading the node data.
        
        // Clear waiting state - we just completed LoadLink response handling
        op.waiting_for_loadlink_response = false;
        op.lock_retry_count = 0;
          
        {
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
        // Lock is busy (held exclusively or we need exclusive but it's shared)
        // Retry with exponential backoff
        if (verbose_level_ >= 2 && out_) {
            if (is_exclusive_lock) {
                uint32_t holder = lock_state & 0x7FFFFFFFFFFFFFFFULL;
                out_->output("   ✗ Lock held EXCLUSIVELY by node %u on 0x%lx, will retry\n", 
                           holder, op.lock_target_address);
            } else {
                out_->output("   ✗ Lock held SHARED (count=%lu) but we need EXCLUSIVE on 0x%lx, will retry\n",
                           lock_state, op.lock_target_address);
            }
        }
        
        // Remove from pending_ops temporarily - we'll re-add when we retry
        AsyncOperation retry_op = op;
        pending_ops.erase(it);
        
        // Reset LoadLink wait state for retry
        retry_op.waiting_for_loadlink_response = false;
        
        // Schedule a retry (immediately retry for now, could add exponential backoff)
        try_acquire_lock_async(retry_op, retry_op.lock_target_address, 
                              retry_op.need_exclusive_lock, interface, pending_ops);
        
        return false;  // Lock not acquired, retrying
    }
}

void BTreeLockManager::handle_storeconditional_response(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    SST::Interfaces::StandardMem::WriteResp* resp,
    std::map<SST::Interfaces::StandardMem::Request::id_t, AsyncOperation>& pending_ops,
    SST::Interfaces::StandardMem* interface,
    size_t serialized_node_size) {
    
    auto it = pending_ops.find(req_id);
    if (it == pending_ops.end()) {
        if (out_) {
            out_->fatal(CALL_INFO, -1, "StoreConditional response for unknown request ID\n");
        }
        return;
    }
    
    AsyncOperation& op = it->second;
    
    // Check if StoreConditional succeeded or failed
    bool sc_failed = resp->getFail();
    
    if (sc_failed) {
        // SC FAILED - another node modified the lock between our LL and SC
        // This is the race condition we're preventing!
        if (verbose_level_ >= 2 && out_) {
            out_->output("   ⚠ StoreConditional FAILED on node 0x%lx (interference detected), retrying LL/SC...\n",
                       op.lock_target_address);
        }
        
        // Remove from pending_ops temporarily - we'll re-add when we retry
        AsyncOperation retry_op = op;
        pending_ops.erase(it);
        
        // Reset SC wait state for retry
        retry_op.waiting_for_sc_response = false;
        
        // Retry the entire LL/SC sequence from LoadLink
        try_acquire_lock_async(retry_op, retry_op.lock_target_address, 
                              retry_op.need_exclusive_lock, interface, pending_ops);
        return;
    }
    
    // SC SUCCEEDED - we atomically acquired the lock!
    if (verbose_level_ >= 2 && out_) {
        out_->output("   ✓ %s lock acquired on node 0x%lx (SC success, count/state=%lu)\n",
                     op.need_exclusive_lock ? "EXCLUSIVE" : "SHARED", 
                     op.lock_target_address, op.llsc_lock_value);
    }
    
    // Add to held locks list with lock type
    op.held_locks.push_back(op.lock_target_address);
    op.held_locks_exclusive.push_back(op.need_exclusive_lock);
    
    // Clear waiting states
    op.waiting_for_sc_response = false;
    op.llsc_retry_count = 0;
    
    // Now read the node data (skip the lock header)
    if (out_) {
        out_->output("   Step 2: Reading node data at 0x%lx (lock held)\n", 
                    op.lock_target_address);
    }
    
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

void BTreeLockManager::release_all_locks(
    SST::Interfaces::StandardMem::Request::id_t old_req_id,
    AsyncOperation& op,
    std::function<SST::Interfaces::StandardMem*(uint64_t)> interface_getter,
    std::map<SST::Interfaces::StandardMem::Request::id_t, AsyncOperation>& pending_ops) {
    
    if (op.held_locks.empty()) {
        return;  // No locks to release
    }
    
    if (verbose_level_ >= 2 && out_) {
        out_->output("   🔓 Releasing %zu held locks using LL/SC (bottom-to-top order)\n", op.held_locks.size());
    }
    
    // Start releasing locks one by one using LL/SC protocol
    // This ensures proper reference count decrement for shared locks
    // 
    // IMPORTANT: Release locks in REVERSE order (LIFO - Last In, First Out)
    // Locks were acquired top-to-bottom (root → leaf), so release bottom-to-top (leaf → root)
    // This minimizes contention: leaf locks are released first, allowing other operations to proceed
    op.release_lock_index = op.held_locks.size() - 1;  // Start from LAST lock (leaf)
    op.waiting_for_release_ll = true;
    
    // Initiate release of first lock (actually the LAST acquired lock - the leaf)
    release_single_lock_async(old_req_id, op, interface_getter, pending_ops);
}

void BTreeLockManager::release_single_lock_async(
    SST::Interfaces::StandardMem::Request::id_t old_req_id,
    AsyncOperation& op,
    std::function<SST::Interfaces::StandardMem*(uint64_t)> interface_getter,
    std::map<SST::Interfaces::StandardMem::Request::id_t, AsyncOperation>& pending_ops) {
    
    // Check if all locks have been released
    // Note: release_lock_index starts at held_locks.size()-1 and decrements to -1
    if (op.release_lock_index < 0) {
        // All locks released!
        if (verbose_level_ >= 2 && out_) {
            out_->output("   ✓ All %zu locks released\n", op.held_locks.size());
        }
        op.held_locks.clear();
        op.held_locks_exclusive.clear();
        op.waiting_for_release_ll = false;
        return;
    }
    
    uint64_t lock_addr = op.held_locks[op.release_lock_index];
    bool is_exclusive = op.held_locks_exclusive[op.release_lock_index];
    
    if (verbose_level_ >= 3 && out_) {
        // Display position in reverse: lock 1/N is the last lock (leaf)
        uint32_t display_position = op.held_locks.size() - op.release_lock_index;
        out_->output("      Releasing %s lock on 0x%lx (lock %u/%zu, bottom-to-top)\n",
                    is_exclusive ? "EXCLUSIVE" : "SHARED", lock_addr,
                    display_position, op.held_locks.size());
    }
    
    // Use LoadLink to read current lock state atomically
    auto ll_req = new SST::Interfaces::StandardMem::LoadLink(lock_addr, LOCK_HEADER_SIZE);
    auto req_id = ll_req->getID();
    
    // Store operation in pending_ops to retrieve during response (creates NEW entry)
    pending_ops[req_id] = op;
    
    // Erase the old req_id now that we've copied to the new one (if old_req_id != 0)
    pending_ops.erase(old_req_id);
    
    
    SST::Interfaces::StandardMem* target_interface = interface_getter(lock_addr);
    target_interface->send(ll_req);
    
    // Note: Response will be handled by handle_release_loadlink_response()
}

void BTreeLockManager::handle_release_loadlink_response(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    const std::vector<uint8_t>& data,
    std::map<SST::Interfaces::StandardMem::Request::id_t, AsyncOperation>& pending_ops,
    std::function<SST::Interfaces::StandardMem*(uint64_t)> interface_getter) {
    
    auto it = pending_ops.find(req_id);
    if (it == pending_ops.end()) {
        if (out_) {
            out_->fatal(CALL_INFO, -1, "Release LoadLink response for unknown request ID\n");
        }
        return;
    }
    
    AsyncOperation& op = it->second;
    
    // Parse current lock state
    uint64_t lock_state = 0;
    if (data.size() >= 8) {
        memcpy(&lock_state, data.data(), 8);
    }
    
    op.release_lock_value = lock_state;
    
    uint64_t lock_addr = op.held_locks[op.release_lock_index];
    bool is_exclusive = op.held_locks_exclusive[op.release_lock_index];
    
    // Compute new lock value based on lock type
    uint64_t new_lock_value;
    
    if (is_exclusive) {
        // Exclusive lock: just write 0 (only we hold it)
        uint64_t expected_exclusive = 0x8000000000000000ULL | node_id_;
        if (lock_state != expected_exclusive) {
            if (out_) {
                out_->output("   ⚠ WARNING: Releasing exclusive lock 0x%lx but state is 0x%lx (expected 0x%lx)\n",
                           lock_addr, lock_state, expected_exclusive);
            }
        }
        new_lock_value = 0;
        
    } else {
        // Shared lock: decrement reference count
        if (lock_state == 0) {
            if (out_) {
                out_->fatal(CALL_INFO, -1, "ERROR: Attempting to release shared lock 0x%lx but it's already free!\n",
                           lock_addr);
                assert(0);
            }
        } else if (lock_state & 0x8000000000000000ULL) {
            if (out_) {
                out_->fatal(CALL_INFO, -1, "ERROR: Attempting to release shared lock 0x%lx but it's held exclusively!\n",
                           lock_addr);
            }
        }
        
        // Decrement reference count (or free if we're last holder)
        new_lock_value = (lock_state == 1) ? 0 : lock_state - 1;
        
        if (verbose_level_ >= 3 && out_) {
            out_->output("      Shared lock count: %lu -> %lu\n", lock_state, new_lock_value);
        }
    }
    
    // Use StoreConditional to atomically write new value
    std::vector<uint8_t> lock_data(8);
    memcpy(lock_data.data(), &new_lock_value, 8);
    auto sc_req = new SST::Interfaces::StandardMem::StoreConditional(lock_addr, 8, lock_data);
    auto sc_req_id = sc_req->getID();
    
    op.waiting_for_release_ll = false;
    op.waiting_for_release_sc = true;
    
    // Update operation in pending_ops
    pending_ops[sc_req_id] = op;
    pending_ops.erase(it);  // Remove LoadLink request ID
    
    SST::Interfaces::StandardMem* target_interface = interface_getter(lock_addr);
    target_interface->send(sc_req);
    
    // Note: Response will be handled by handle_release_storeconditional_response()
}

void BTreeLockManager::handle_release_storeconditional_response(
    SST::Interfaces::StandardMem::Request::id_t req_id,
    SST::Interfaces::StandardMem::WriteResp* resp,
    std::map<SST::Interfaces::StandardMem::Request::id_t, AsyncOperation>& pending_ops,
    std::function<SST::Interfaces::StandardMem*(uint64_t)> interface_getter,
    uint64_t restart_signal_address) {
    
    auto it = pending_ops.find(req_id);
    if (it == pending_ops.end()) {
        if (out_) {
            out_->fatal(CALL_INFO, -1, "Release StoreConditional response for unknown request ID\n");
        }
        return;
    }
    
    AsyncOperation& op = it->second;
    bool sc_failed = resp->getFail();
    
    uint64_t lock_addr = op.held_locks[op.release_lock_index];
    bool is_exclusive = op.held_locks_exclusive[op.release_lock_index];
    
    if (sc_failed) {
        // SC failed - another node interfered, retry from LoadLink
        if (verbose_level_ >= 2 && out_) {
            out_->output("   ⚠ Release StoreConditional FAILED on 0x%lx (interference), retrying...\n",
                       lock_addr);
        }
        
        AsyncOperation retry_op = op;
        auto old_req_id = it->first;
        pending_ops.erase(it);
        
        retry_op.waiting_for_release_sc = false;
        retry_op.waiting_for_release_ll = true;
        
        // Retry releasing this lock (old_req_id already erased, pass 0)
        release_single_lock_async(0, retry_op, interface_getter, pending_ops);
        return;
    }
    
    // SC succeeded - lock released!
    if (verbose_level_ >= 3 && out_) {
        out_->output("      ✓ Released %s lock on 0x%lx (SC success)\n",
                    is_exclusive ? "EXCLUSIVE" : "SHARED", lock_addr);
    }
    
    // Move to next lock (decrement to release in reverse order - bottom to top)
    op.release_lock_index--;
    op.waiting_for_release_sc = false;
    
    // Check if more locks remain (index goes from size-1 down to 0, then -1 means done)
    if (op.release_lock_index >= 0) {
        // More locks to release
        op.waiting_for_release_ll = true;
        auto old_req_id = it->first;
        AsyncOperation next_op = op;  // Copy before erase!
        pending_ops.erase(it);  // Remove current request ID (invalidates 'op' reference)
        release_single_lock_async(0, next_op, interface_getter, pending_ops);
    } else {
        // All locks released! 
        if (verbose_level_ >= 2 && out_) {
            out_->output("   ✓ All %zu locks released successfully\n", op.held_locks.size());
        }
        op.held_locks.clear();
        op.held_locks_exclusive.clear();
        op.waiting_for_release_ll = false;
        
        // Check if this operation should restart after lock release
        if (op.restart_pending) {
            if (verbose_level_ >= 2 && out_) {
                out_->output("   🔄 Locks released, triggering restart by writing to RESTART_SIGNAL_ADDRESS\n");
            }
            
            // Mark operation type as RESTART_SIGNAL so computeServer knows to restart it
            op.type = AsyncOperation::RESTART_SIGNAL;
            
            // Trigger restart by writing to special RESTART_SIGNAL_ADDRESS
            // This will invoke the write response handler in computeServer
            // which will detect the RESTART_SIGNAL type and start the new operation
            std::vector<uint8_t> signal_data(8, 0);  // Dummy data
            auto write_req = new SST::Interfaces::StandardMem::Write(
                restart_signal_address, 8, signal_data);
            auto write_req_id = write_req->getID();
            
            // Update operation in pending_ops before sending write
            pending_ops[write_req_id] = op;
            pending_ops.erase(it);  // Remove old req_id
            
            // Send write to trigger restart handler
            SST::Interfaces::StandardMem* restart_interface = interface_getter(restart_signal_address);
            restart_interface->send(write_req);
            
        } else {
            // Normal completion path
            op.ready_to_complete = true;
            // Operation stays in pending_ops with ready_to_complete=true, 
            // will be finalized and erased by computeServer in tick()
        }
    }
}

} // namespace MemHierarchy
} // namespace SST
