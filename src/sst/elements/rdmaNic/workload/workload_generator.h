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

#ifndef _H_WORKLOAD_GENERATOR
#define _H_WORKLOAD_GENERATOR

#include <sst/core/output.h>
#include <random>
#include <vector>
#include <queue>
#include "workload_types.h"

namespace SST {
namespace MemHierarchy {

/**
 * @class WorkloadGenerator
 * @brief Generates B+tree workload operations with configurable distributions
 * 
 * Supports:
 * - Uniform or Zipfian key distributions
 * - Configurable read/write ratios
 * - Time-based operation scheduling
 */
class WorkloadGenerator {
public:
    /**
     * Constructor
     * @param node_id Node identifier
     * @param key_range Range of keys (0 to key_range-1)
     * @param zipfian_alpha Zipfian distribution parameter (0.0 = uniform)
     * @param read_ratio Fraction of read operations (0.0 to 1.0)
     * @param ops_per_second Target operations per second
     * @param simulation_duration_ns Simulation duration in nanoseconds
     * @param output Output object for logging
     * @param seed Random seed
     */
    WorkloadGenerator(
        uint32_t node_id,
        uint64_t key_range,
        double zipfian_alpha,
        double read_ratio,
        uint32_t ops_per_second,
        SimTime_t simulation_duration_ns,
        SST::Output* output,
        unsigned int seed = 0);
    
    /**
     * Generate all workload operations
     * @param workload Queue to fill with operations
     */
    void generate_workload(std::queue<WorkloadOp>& workload);
    
    /**
     * Generate a single operation
     * @return Generated operation
     */
    WorkloadOp generate_next_operation();
    
    /**
     * Generate a key using configured distribution
     * @return Generated key
     */
    uint64_t get_key();
    
    /**
     * Get key frequency statistics
     * @return Vector of key access counts
     */
    const std::vector<uint64_t>& get_key_frequencies() const { return key_frequencies_; }

private:
    uint32_t node_id_;
    uint64_t key_range_;
    double zipfian_alpha_;
    double read_ratio_;
    uint32_t ops_per_second_;
    SimTime_t simulation_duration_ns_;
    SST::Output* out_;
    
    // Random number generation
    std::mt19937 rng_;
    std::uniform_real_distribution<double> uniform_dist_;
    
    // Statistics
    std::vector<uint64_t> key_frequencies_;
};

} // namespace MemHierarchy
} // namespace SST

#endif // _H_WORKLOAD_GENERATOR
