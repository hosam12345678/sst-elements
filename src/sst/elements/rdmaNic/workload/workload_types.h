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

#ifndef _H_WORKLOAD_TYPES
#define _H_WORKLOAD_TYPES

#include <cstdint>
#include <sst/core/sst_types.h>

namespace SST {
namespace MemHierarchy {

// ═══════════════════════════════════════════════════════════════════════════
// WORKLOAD OPERATION TYPES
// ═══════════════════════════════════════════════════════════════════════════

/**
 * B+tree operation types
 */
enum BTreeOp {
    BTREE_INSERT,   // Insert key-value pair
    BTREE_SEARCH    // Search for key
};

/**
 * Workload operation structure
 * Represents a single operation to be executed at a specific time
 */
struct WorkloadOp {
    BTreeOp op_type;        // Type of operation (INSERT or SEARCH)
    uint64_t key;           // Key to operate on
    uint64_t value;         // Value (for inserts)
    SimTime_t timestamp;    // When to execute (in nanoseconds)
    uint64_t node_id;       // Which compute node issued this
};

} // namespace MemHierarchy
} // namespace SST

#endif // _H_WORKLOAD_TYPES
