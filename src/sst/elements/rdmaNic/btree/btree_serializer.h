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

#ifndef _H_BTREE_SERIALIZER
#define _H_BTREE_SERIALIZER

#include "btree_node.h"
#include <vector>
#include <cstdint>
#include <sst/core/output.h>

namespace SST {
namespace MemHierarchy {

/**
 * BTreeSerializer - Handles serialization and deserialization of B+tree nodes
 * 
 * Serialization Format:
 * ┌─────────────────────────────────────────────────────────────┐
 * │ Metadata: num_keys (4) + fanout (4) + is_leaf (1) + addr (8)│
 * ├─────────────────────────────────────────────────────────────┤
 * │ Keys Array: fanout * 8 bytes                                │
 * ├─────────────────────────────────────────────────────────────┤
 * │ Values Array: fanout * 8 bytes (leaf nodes only)            │
 * ├─────────────────────────────────────────────────────────────┤
 * │ Children Pointers: (fanout+1) * 8 bytes (internal nodes)    │
 * └─────────────────────────────────────────────────────────────┘
 * 
 * Note: Always serializes full arrays for fixed-size layout, regardless of num_keys
 */
class BTreeSerializer {
public:
    /**
     * Constructor
     * @param fanout The B+tree fanout (maximum keys per node)
     * @param output SST Output object for debug logging
     */
    BTreeSerializer(uint32_t fanout, SST::Output* output = nullptr);
    
    /**
     * Serialize a B+tree node to raw bytes
     * @param node The node to serialize
     * @return Vector of bytes containing serialized node
     */
    std::vector<uint8_t> serialize(const BTreeNode& node);
    
    /**
     * Deserialize raw bytes into a B+tree node
     * @param data Raw bytes to deserialize
     * @return Deserialized B+tree node
     */
    BTreeNode deserialize(const std::vector<uint8_t>& data);
    
    /**
     * Calculate the size of a serialized node
     * @return Size in bytes
     */
    size_t get_serialized_size() const;
    
private:
    uint32_t fanout_;           // B+tree fanout
    SST::Output* out_;          // Output for logging
    size_t serialized_size_;    // Cached serialized size
    
    // Calculate serialized size based on fanout
    size_t calculate_serialized_size() const;
};

} // namespace MemHierarchy
} // namespace SST

#endif // _H_BTREE_SERIALIZER
