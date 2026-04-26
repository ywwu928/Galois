/*
 * This file belongs to the Galois project, a C++ library for exploiting parallelism.
 * The code is being released under the terms of the 3-Clause BSD License (a
 * copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */

#include "galois/runtime/SyncStructures.h"

//GALOIS_SYNC_STRUCTURE_REDUCE_MIN(dist_current, unsigned int);
GALOIS_SYNC_STRUCTURE_BITSET(dist_current_odd);
GALOIS_SYNC_STRUCTURE_BITSET(dist_current_even);

struct CommData {
    int64_t id;
    int32_t classYear;
    uint32_t dist;
};

struct Reduce_min_dist {
    static CommData extract(uint32_t, const struct NodeData& node) {
        CommData result{0, 0, node.dist_current.load()};
        return result;
    }

    static bool reduce(uint32_t, struct NodeData& node, CommData y) {
        if (y.dist < node.dist_current.load()) {
            node.dist_current.store(y.dist);
            return true;
        }
        else {
            return false;
        }
    }

    static void reduce_void(struct NodeData& node, CommData y) {
        if (y.dist < node.dist_current.load()) {
            node.dist_current.store(y.dist);
        }
    }

    static bool reduce_atomic(struct NodeData& node, int32_t, int32_t, uint32_t dist) {
        uint32_t old_dist = node.dist_current.load(std::memory_order_relaxed);
        while (old_dist > dist && !node.dist_current.compare_exchange_weak(old_dist, dist, std::memory_order_relaxed)) ;
        return dist < old_dist;
    }

    static void reduce_atomic_void(struct NodeData& node, int32_t, int32_t, uint32_t dist) {
        uint32_t old_dist = node.dist_current.load(std::memory_order_relaxed);
        while (old_dist > dist && !node.dist_current.compare_exchange_weak(old_dist, dist, std::memory_order_relaxed)) ;
    }

    static void reset(uint32_t, struct NodeData&) {}

    static void setVal(uint32_t, struct NodeData& node, CommData y) {
        node.dist_current.store(y.dist);
    }
};
