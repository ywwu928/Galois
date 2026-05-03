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

#include "snb_data_structure.h"

//GALOIS_SYNC_STRUCTURE_REDUCE_MIN(dist_current, unsigned int);
GALOIS_SYNC_STRUCTURE_BITSET(dist_current_odd);
GALOIS_SYNC_STRUCTURE_BITSET(dist_current_even);

struct NodeData {
    uint32_t type;
    uint32_t index;
    std::atomic<uint32_t> dist_current;
};

struct EdgeData {
    uint32_t type;
    uint32_t index;
};

struct CommData {
    int64_t id;
    int32_t classYear;
    uint32_t dist;
};

typedef galois::graphs::DistGraph<NodeData, EdgeData> Graph;
typedef typename Graph::GraphNode GNode;

struct Reduce_min_dist {
    static CommData extract(uint32_t, const struct NodeData& node) {
        CommData result{0, 0, node.dist_current.load()};
        return result;
    }

    static bool reduce(Graph& graph, uint32_t lid, CommData comm) {
        uint32_t new_dist = comm.dist;

        for (auto edge : graph.outEdges(lid)) {
            volatile EdgeData& edge_data = graph.getEdgeData(edge);
            uint32_t edge_type = edge_data.type;
            (void) edge_type;

            if (person_university_distribution(generator) == 0) {
                GNode dst = graph.getOutEdgeDst(edge);
                NodeData& dst_data = graph.getData(dst);
                uint32_t dst_index = dst_data.index;
                (void) dst_index;
                volatile Organization& dst_property = organization_memory[1];
                int64_t dst_university_id = dst_property.id;

                if (dst_university_id == comm.id) {
                    if (same_university_distribution(generator) == 0) {
                        uint32_t edge_index = edge_data.index;
                        (void) edge_index;
                        volatile Person_workOrStudyAt_Organization& edge_property = person_organization_memory[1];
                        int32_t class_year = edge_property.classYear;
                        
                        int64_t class_year_diff = static_cast<int64_t>(comm.classYear) - static_cast<int64_t>(class_year);
                        uint32_t abs_class_year_diff = static_cast<uint32_t>(std::abs(class_year_diff));
                        new_dist += abs_class_year_diff;
                        break;
                    }
                }
            }
        }

        NodeData& node = graph.getData(lid);
        if (new_dist < node.dist_current.load()) {
            node.dist_current.store(new_dist);
            return true;
        }
        else {
            return false;
        }
    }

    static void reduce_void(Graph& graph, uint32_t lid, CommData comm) {
        uint32_t new_dist = comm.dist;

        for (auto edge : graph.outEdges(lid)) {
            volatile EdgeData& edge_data = graph.getEdgeData(edge);
            uint32_t edge_type = edge_data.type;
            (void) edge_type;

            if (person_university_distribution(generator) == 0) {
                GNode dst = graph.getOutEdgeDst(edge);
                NodeData& dst_data = graph.getData(dst);
                uint32_t dst_index = dst_data.index;
                (void) dst_index;
                volatile Organization& dst_property = organization_memory[1];
                int64_t dst_university_id = dst_property.id;

                if (dst_university_id == comm.id) {
                    if (same_university_distribution(generator) == 0) {
                        uint32_t edge_index = edge_data.index;
                        (void) edge_index;
                        volatile Person_workOrStudyAt_Organization& edge_property = person_organization_memory[1];
                        int32_t class_year = edge_property.classYear;
                        
                        int64_t class_year_diff = static_cast<int64_t>(comm.classYear) - static_cast<int64_t>(class_year);
                        uint32_t abs_class_year_diff = static_cast<uint32_t>(std::abs(class_year_diff));
                        new_dist += abs_class_year_diff;
                        break;
                    }
                }
            }
        }

        NodeData& node = graph.getData(lid);
        if (new_dist < node.dist_current.load()) {
            node.dist_current.store(new_dist);
        }
    }

    static bool reduce_atomic(Graph& graph, uint32_t lid, int64_t work_id, int32_t work_class_year, uint32_t new_dist) {
        for (auto edge : graph.outEdges(lid)) {
            volatile EdgeData& edge_data = graph.getEdgeData(edge);
            uint32_t edge_type = edge_data.type;
            (void) edge_type;

            if (person_university_distribution(generator) == 0) {
                GNode dst = graph.getOutEdgeDst(edge);
                NodeData& dst_data = graph.getData(dst);
                uint32_t dst_index = dst_data.index;
                (void) dst_index;
                volatile Organization& dst_property = organization_memory[1];
                int64_t dst_university_id = dst_property.id;

                if (dst_university_id == work_id) {
                    if (same_university_distribution(generator) == 0) {
                        uint32_t edge_index = edge_data.index;
                        (void) edge_index;
                        volatile Person_workOrStudyAt_Organization& edge_property = person_organization_memory[1];
                        int32_t class_year = edge_property.classYear;
                        
                        int64_t class_year_diff = static_cast<int64_t>(work_class_year) - static_cast<int64_t>(class_year);
                        uint32_t abs_class_year_diff = static_cast<uint32_t>(std::abs(class_year_diff));
                        new_dist += abs_class_year_diff;
                        break;
                    }
                }
            }
        }

        NodeData& node = graph.getData(lid);
        uint32_t old_dist = node.dist_current.load(std::memory_order_relaxed);
        while (old_dist > new_dist && !node.dist_current.compare_exchange_weak(old_dist, new_dist, std::memory_order_relaxed)) ;
        return new_dist < old_dist;
    }

    static void reduce_atomic_void(Graph& graph, uint32_t lid, int64_t work_id, int32_t work_class_year, uint32_t new_dist) {
        for (auto edge : graph.outEdges(lid)) {
            volatile EdgeData& edge_data = graph.getEdgeData(edge);
            uint32_t edge_type = edge_data.type;
            (void) edge_type;

            if (person_university_distribution(generator) == 0) {
                GNode dst = graph.getOutEdgeDst(edge);
                NodeData& dst_data = graph.getData(dst);
                uint32_t dst_index = dst_data.index;
                (void) dst_index;
                volatile Organization& dst_property = organization_memory[1];
                int64_t dst_university_id = dst_property.id;

                if (dst_university_id == work_id) {
                    if (same_university_distribution(generator) == 0) {
                        uint32_t edge_index = edge_data.index;
                        (void) edge_index;
                        volatile Person_workOrStudyAt_Organization& edge_property = person_organization_memory[1];
                        int32_t class_year = edge_property.classYear;
                        
                        int64_t class_year_diff = static_cast<int64_t>(work_class_year) - static_cast<int64_t>(class_year);
                        uint32_t abs_class_year_diff = static_cast<uint32_t>(std::abs(class_year_diff));
                        new_dist += abs_class_year_diff;
                        break;
                    }
                }
            }
        }

        NodeData& node = graph.getData(lid);
        uint32_t old_dist = node.dist_current.load(std::memory_order_relaxed);
        while (old_dist > new_dist && !node.dist_current.compare_exchange_weak(old_dist, new_dist, std::memory_order_relaxed)) ;
    }

    static void reset(uint32_t, struct NodeData&) {}

    static void setVal(uint32_t, struct NodeData& node, CommData y) {
        node.dist_current.store(y.dist);
    }
};
