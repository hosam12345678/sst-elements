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

#include "btree_serializer.h"
#include <cstring>

namespace SST {
namespace MemHierarchy {

BTreeSerializer::BTreeSerializer(uint32_t fanout, SST::Output* output)
    : fanout_(fanout), out_(output) {
    serialized_size_ = calculate_serialized_size();
}

size_t BTreeSerializer::calculate_serialized_size() const {
    // Metadata: num_keys (4) + fanout (4) + is_leaf (1) + address (8)
    size_t metadata_size = sizeof(uint32_t) * 2 + sizeof(bool) + sizeof(uint64_t);
    
    // Keys array
    size_t keys_size = fanout_ * sizeof(uint64_t);
    
    // Values array (for leaf nodes)
    size_t values_size = fanout_ * sizeof(uint64_t);
    
    // Children pointers (for internal nodes)
    size_t children_size = (fanout_ + 1) * sizeof(uint64_t);
    
    return metadata_size + keys_size + values_size + children_size;
}

size_t BTreeSerializer::get_serialized_size() const {
    return serialized_size_;
}

std::vector<uint8_t> BTreeSerializer::serialize(const BTreeNode& node) {
    std::vector<uint8_t> data(serialized_size_, 0);
    size_t offset = 0;
    
    // Serialize metadata
    std::memcpy(data.data() + offset, &node.num_keys, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    
    std::memcpy(data.data() + offset, &node.fanout, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    
    std::memcpy(data.data() + offset, &node.is_leaf, sizeof(bool));
    offset += sizeof(bool);
    
    std::memcpy(data.data() + offset, &node.node_address, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    
    // Serialize keys array (always serialize full fanout size for fixed layout)
    std::memcpy(data.data() + offset, node.keys.data(), fanout_ * sizeof(uint64_t));
    offset += fanout_ * sizeof(uint64_t);
    
    // Serialize values array (for leaf nodes)
    std::memcpy(data.data() + offset, node.values.data(), fanout_ * sizeof(uint64_t));
    offset += fanout_ * sizeof(uint64_t);
    
    // Serialize children array (for internal nodes)
    std::memcpy(data.data() + offset, node.children.data(), (fanout_ + 1) * sizeof(uint64_t));
    
    // Debug output
    if (out_) {
        out_->output("   📦 Serialized node: num_keys=%u, is_leaf=%d, addr=0x%lx",
                     node.num_keys, node.is_leaf, node.node_address);
        if (node.num_keys > 0) {
            out_->output(", keys[0]=%lu", node.keys[0]);
        }
        out_->output("\n");
    }
    
    return data;
}

BTreeNode BTreeSerializer::deserialize(const std::vector<uint8_t>& data) {
    BTreeNode node(fanout_);
    
    // Check minimum size
    if (data.size() < sizeof(uint32_t) * 2 + sizeof(bool) + sizeof(uint64_t)) {
        if (out_) {
            out_->output("   ⚠️  WARNING: Data too small: %zu bytes\n", data.size());
        }
        return node;
    }
    
    size_t offset = 0;
    
    // Deserialize metadata
    std::memcpy(&node.num_keys, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    
    std::memcpy(&node.fanout, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    
    std::memcpy(&node.is_leaf, data.data() + offset, sizeof(bool));
    offset += sizeof(bool);
    
    std::memcpy(&node.node_address, data.data() + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    
    // Deserialize keys array
    if (node.num_keys > 0 && offset + node.num_keys * sizeof(uint64_t) <= data.size()) {
        std::memcpy(node.keys.data(), data.data() + offset, node.num_keys * sizeof(uint64_t));
    }
    offset += fanout_ * sizeof(uint64_t);  // Always advance by full fanout size
    
    // Deserialize values array (for leaf nodes)
    if (node.is_leaf && node.num_keys > 0 && offset + node.num_keys * sizeof(uint64_t) <= data.size()) {
        std::memcpy(node.values.data(), data.data() + offset, node.num_keys * sizeof(uint64_t));
    }
    offset += fanout_ * sizeof(uint64_t);  // Always advance by full fanout size
    
    // Deserialize children array (for internal nodes)
    if (!node.is_leaf && offset + (node.num_keys + 1) * sizeof(uint64_t) <= data.size()) {
        std::memcpy(node.children.data(), data.data() + offset, (node.num_keys + 1) * sizeof(uint64_t));
        
        if (out_) {
            out_->output("   DEBUG DESER: Copied %u children from offset %zu\n", 
                        node.num_keys + 1, offset);
        }
    } else if (!node.is_leaf && out_) {
        out_->output("   DEBUG DESER: SKIPPED children! is_leaf=%d, offset=%zu, need=%zu, data_size=%zu\n",
                    node.is_leaf, offset, (node.num_keys + 1) * sizeof(uint64_t), data.size());
    }
    
    // Debug output
    if (out_) {
        out_->output("   📦 Deserialized node: num_keys=%u, is_leaf=%d, addr=0x%lx",
                     node.num_keys, node.is_leaf, node.node_address);
        if (node.num_keys > 0) {
            out_->output(", keys[0]=%lu", node.keys[0]);
        }
        out_->output("\n");
    }
    
    return node;
}

} // namespace MemHierarchy
} // namespace SST
