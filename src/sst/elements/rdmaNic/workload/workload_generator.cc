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

#include "workload_generator.h"
#include <cmath>
#include <algorithm>

namespace SST {
namespace MemHierarchy {

WorkloadGenerator::WorkloadGenerator(
    uint32_t node_id,
    uint64_t key_range,
    double zipfian_alpha,
    double read_ratio,
    uint32_t ops_per_second,
    SimTime_t simulation_duration_ns,
    SST::Output* output,
    unsigned int seed)
    : node_id_(node_id),
      key_range_(key_range),
      zipfian_alpha_(zipfian_alpha),
      read_ratio_(read_ratio),
      ops_per_second_(ops_per_second),
      simulation_duration_ns_(simulation_duration_ns),
      out_(output),
      rng_(seed),
      uniform_dist_(0.0, 1.0) {
    
    // Initialize key frequency tracking (first 100 keys)
    key_frequencies_.resize(std::min(key_range_, static_cast<uint64_t>(100)), 0);
    
    if (out_) {
        out_->output("WorkloadGenerator: node=%d, key_range=%lu, alpha=%.2f, read_ratio=%.2f\n",
                     node_id_, key_range_, zipfian_alpha_, read_ratio_);
    }
}

void WorkloadGenerator::generate_workload(std::queue<WorkloadOp>& workload) {
    if (out_) {
        out_->output("Generating workload: node=%d, ops/sec=%d, duration=%lu ns\n",
                     node_id_, ops_per_second_, simulation_duration_ns_);
    }
    
    // Calculate time interval between operations (nanoseconds)
    SimTime_t op_interval = 1000000000ULL / ops_per_second_;
    SimTime_t current_time = 0;
    
    // Generate operations for the simulation duration
    while (current_time < simulation_duration_ns_) {
        WorkloadOp op = generate_next_operation();
        op.timestamp = current_time;
        op.node_id = node_id_;
        
        workload.push(op);
        current_time += op_interval;
    }
    
    if (out_) {
        out_->output("Generated %zu operations for node %d\n", workload.size(), node_id_);
    }
}

WorkloadOp WorkloadGenerator::generate_next_operation() {
    WorkloadOp op;
    
    // Determine operation type based on read ratio
    double rand_val = uniform_dist_(rng_);
    if (rand_val < read_ratio_) {
        op.op_type = BTREE_SEARCH;
    } else {
        op.op_type = BTREE_INSERT;
    }
    
    // Generate key using configured distribution
    op.key = get_key();
    op.value = op.key * 1000 + node_id_;  // Simple value generation
    
    return op;
}

uint64_t WorkloadGenerator::get_key() {
    uint64_t key;
    double rand_val = uniform_dist_(rng_);
    
    if (zipfian_alpha_ <= 0.0) {
        // UNIFORM distribution
        key = static_cast<uint64_t>(rand_val * key_range_);
    } else {
        // Zipfian distribution using inverse power method
        // Avoid rand_val = 0 to prevent pow(0, negative) = infinity
        if (rand_val == 0.0) rand_val = 1e-10;
        
        key = static_cast<uint64_t>(std::pow(rand_val, -1.0 / zipfian_alpha_)) % key_range_;
    }
    
    // Track frequency for first 100 keys
    if (key < key_frequencies_.size()) {
        key_frequencies_[key]++;
    }
    
    return key;
}

} // namespace MemHierarchy
} // namespace SST
