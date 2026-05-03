/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
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

#include "DistBench/Output.h"
#include "DistBench/Start.h"
#include "galois/DistGalois.h"
#include "galois/gstl.h"
#include "galois/DReducible.h"
#include "galois/runtime/Tracer.h"

#include <limits>
#include <vector>
#include <algorithm>
#include <cstdlib>

static std::string REGION_NAME = "SSSP";
static std::string REGION_NAME_RUN;

/******************************************************************************/
/* Declaration of command line arguments */
/******************************************************************************/

namespace cll = llvm::cl;

static cll::opt<unsigned int> maxIterations(
    "maxIterations",
    cll::desc("Maximum iterations: Default 1000"),
    cll::init(1000)
);

enum selectionMode {randomValue, explicitValue};

static cll::opt<selectionMode> srcSelection(
    "srcSelection",
    cll::desc("Start Node Selection Mode"),
    cll::values(clEnumVal(randomValue, "Selected by random number generator with seed"),
                clEnumVal(explicitValue, "User explicitly specify the starting node ID")),
    cll::init(explicitValue)
);

static uint64_t src_node;
static cll::opt<uint64_t> startNode("startNode", cll::desc("ID of the start node"), cll::init(0));
      

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

const uint32_t infinity = std::numeric_limits<uint32_t>::max();

galois::DynamicBitSet bitset_dist_current_odd;
galois::DynamicBitSet bitset_dist_current_even;

#include "sssp_sync.hh"

std::unique_ptr<galois::graphs::GluonSubstrate<Graph, CommData>> syncSubstrate;

uint32_t vertex_counter[8];
uint64_t edge_counter[21];

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/

void TypeAssignment(Graph& graph) {
    std::unique_ptr<uint32_t[]> types;
    uint64_t max_size = std::max({graph.numMasters(), graph.numMirrors(), graph.sizeEdges()});
    types = std::make_unique<uint32_t[]>(max_size);

    // master
    for (uint32_t i=0; i<graph.numMasters(); i++) {
        types[i] = master_distribution(generator);
    }

    galois::on_each([&](unsigned tid, unsigned nthreads) {
        uint32_t total = graph.numMasters();
        uint32_t quotient = total / nthreads;
        uint32_t start = tid * quotient;
        uint32_t end = (tid + 1) * quotient;
        if (tid == nthreads - 1) {
            end = total;
        }

        std::vector<uint32_t> type_index;
        for (uint32_t i=0; i<8; i++) {
            type_index.emplace_back(
                std::count(types.get(), types.get() + start, i)
            );
        }

        for (uint32_t lid=start; lid<end; lid++) {
            NodeData& master_data = graph.getData(lid);
            master_data.type = types[lid];
            master_data.index = type_index[master_data.type];
            type_index[master_data.type]++;
        }
    });

    for (uint32_t i=0; i<8; i++) {
        vertex_counter[i] = std::count(types.get(), types.get() + graph.numMasters(), i);
    }

    // mirror
    for (uint32_t i=0; i<graph.numMirrors(); i++) {
        types[i] = mirror_distribution(generator);
    }

    galois::on_each([&](unsigned tid, unsigned nthreads) {
        uint32_t total = graph.numMirrors();
        uint32_t quotient = total / nthreads;
        uint32_t start = tid * quotient;
        uint32_t end = (tid + 1) * quotient;
        if (tid == nthreads - 1) {
            end = total;
        }

        std::vector<uint32_t> type_index;
        for (uint32_t i=0; i<8; i++) {
            type_index.emplace_back(
                vertex_counter[i] + std::count(types.get(), types.get() + start, i)
            );
        }

        for (uint32_t index=start; index<end; index++) {
            uint32_t lid = graph.numMasters() + index;
            NodeData& mirror_data = graph.getData(lid);
            mirror_data.type = types[index];
            mirror_data.index = type_index[mirror_data.type];
            type_index[mirror_data.type]++;
        }
    });

    for (uint32_t i=0; i<8; i++) {
        vertex_counter[i] += std::count(types.get(), types.get() + graph.numMirrors(), i);
    }

    // edge
    for (uint32_t i=0; i<graph.sizeEdges(); i++) {
        types[i] = edge_distribution(generator);
    }

    galois::on_each([&](unsigned tid, unsigned nthreads) {
        uint64_t total = graph.sizeEdges();
        uint64_t quotient = total / nthreads;
        uint64_t start = tid * quotient;
        uint64_t end = (tid + 1) * quotient;
        if (tid == nthreads - 1) {
            end = total;
        }

        std::vector<uint64_t> type_index;
        for (uint32_t i=0; i<21; i++) {
            type_index.emplace_back(
                std::count(types.get(), types.get() + start, i)
            );
        }

        for (uint64_t index=start; index<end; index++) {
            EdgeData& edge_data = graph.getEdgeDataDirect(index);
            edge_data.type = types[index];
            edge_data.index = type_index[edge_data.type];
            type_index[edge_data.type]++;
        }
    });

    for (uint32_t i=0; i<21; i++) {
        edge_counter[i] = std::count(types.get(), types.get() + graph.sizeEdges(), i);
    }
}

void PropertyConstruction () {
    organization_memory = std::make_unique<Organization[]>(vertex_counter[0]);
    place_memory = std::make_unique<Place[]>(vertex_counter[1]);
    tag_memory = std::make_unique<Tag[]>(vertex_counter[2]);
    tagclass_memory = std::make_unique<TagClass[]>(vertex_counter[3]);
    comment_memory = std::make_unique<Comment[]>(vertex_counter[4]);
    forum_memory = std::make_unique<Forum[]>(vertex_counter[5]);
    person_memory = std::make_unique<Person[]>(vertex_counter[6]);
    post_memory = std::make_unique<Post[]>(vertex_counter[7]);

    forum_person_memory = std::make_unique<Forum_hasMemberOrModerator_Person[]>(edge_counter[10]);
    person_person_memory = std::make_unique<Person_knows_Person[]>(edge_counter[14]);
    person_comment_memory = std::make_unique<Person_likes_Comment[]>(edge_counter[15]);
    person_post_memory = std::make_unique<Person_likes_Post[]>(edge_counter[16]);
    person_organization_memory = std::make_unique<Person_workOrStudyAt_Organization[]>(edge_counter[17]);
}

struct InitializeGraph {
    Graph* graph;

    InitializeGraph(Graph* _graph) : graph(_graph) {}

    void static go(Graph& _graph) {
        const auto& presentNodes = _graph.presentNodesRange();
    
        galois::do_all(
            galois::iterate(presentNodes),
            InitializeGraph(&_graph), galois::no_stats());
    }

    void operator()(GNode src) const {
        NodeData& sdata = graph->getData(src);
        if (graph->getGID(src) == src_node) {
            sdata.dist_current = 0;
            bitset_dist_current_even.set(src);
        }
        else {
            sdata.dist_current = infinity;
        }
        
    }
};

struct SSSP {
    Graph* graph;
  
    galois::DynamicBitSet* active_bitset_ptr;
    galois::DynamicBitSet* dirty_bitset_ptr;

    galois::runtime::NetworkInterface& net;
  
    SSSP(Graph* _graph, galois::DynamicBitSet* _active_bitset_ptr, galois::DynamicBitSet* _dirty_bitset_ptr)
        : graph(_graph),
          active_bitset_ptr(_active_bitset_ptr),
          dirty_bitset_ptr(_dirty_bitset_ptr),
          net(galois::runtime::getSystemNetworkInterface()) {}

    void static go(Graph& _graph) {
#ifdef GALOIS_USER_STATS
        constexpr bool USER_STATS = true;
#else
        constexpr bool USER_STATS = false;
#endif

        unsigned _num_iterations = 0;

        const auto& masterNodes = _graph.masterNodesRange();
    
        auto& _net = galois::runtime::getSystemNetworkInterface();

        uint64_t local_active_vertices;
        if (_graph.isOwned(src_node)) {
            local_active_vertices = 1;
        }
        else {
            local_active_vertices = 0;
        }
        uint64_t global_active_vertices;

        bool odd = false;

        do {
            std::string total_str("Total_Round_" + std::to_string(_num_iterations));
            galois::CondStatTimer<USER_STATS> StatTimer_total(total_str.c_str(), REGION_NAME_RUN.c_str());
            std::string compute_str("Compute_Round_" + std::to_string(_num_iterations));
            galois::CondStatTimer<USER_STATS> StatTimer_compute(compute_str.c_str(), REGION_NAME_RUN.c_str());
            std::string comm_str("Communication_Round_" + std::to_string(_num_iterations));
            galois::CondStatTimer<USER_STATS> StatTimer_comm(comm_str.c_str(), REGION_NAME_RUN.c_str());
            std::string active_str("Active_Reduce_Round_" + std::to_string(_num_iterations));
            galois::CondStatTimer<USER_STATS> StatTimer_active(active_str.c_str(), REGION_NAME_RUN.c_str());

#ifdef GALOIS_PRINT_PROCESS
            galois::gPrint("Host ", _net.ID, " : iteration ", _num_iterations, "\n");
#endif

            syncSubstrate->set_num_round(_num_iterations);

            galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "NumWorkItems_Round_" + std::to_string(_num_iterations), local_active_vertices);

            StatTimer_total.start();
            _net.prefetchBuffers();
      
            if (odd) {
                bitset_dist_current_even.reset();

                StatTimer_compute.start();
                galois::do_all(
                    galois::iterate(masterNodes), SSSP(&_graph, &bitset_dist_current_odd, &bitset_dist_current_even),
                    galois::no_stats(), galois::steal());
                _net.flushRemoteWork();
                StatTimer_compute.stop();
          
                StatTimer_comm.start();
                syncSubstrate->reduce<Reduce_min_dist, Bitset_dist_current_even>();
                StatTimer_comm.stop();

                local_active_vertices = bitset_dist_current_even.count();
            }
            else {
                bitset_dist_current_odd.reset();

                StatTimer_compute.start();
                galois::do_all(
                    galois::iterate(masterNodes), SSSP(&_graph, &bitset_dist_current_even, &bitset_dist_current_odd),
                    galois::no_stats(), galois::steal());
                _net.flushRemoteWork();
                StatTimer_compute.stop();
          
                StatTimer_comm.start();
                syncSubstrate->reduce<Reduce_min_dist, Bitset_dist_current_odd>();
                StatTimer_comm.stop();

                local_active_vertices = bitset_dist_current_odd.count();
            }

            odd = !odd;
      
            _net.resetWorkTermination();

            ++_num_iterations;
      
            StatTimer_active.start();
            global_active_vertices = 0;
            MPI_Allreduce(&local_active_vertices, &global_active_vertices, 1,
                          MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);
            StatTimer_active.stop();
      
            StatTimer_total.stop();
        } while ((_num_iterations < maxIterations) && global_active_vertices);
    }

    void operator()(GNode src) const {
        if (active_bitset_ptr->test(src)) {
            // find university
            int64_t university_id = 0;
            int32_t class_year = 0;
            
            auto edges = graph->outEdges(src);
            for (auto it = edges.begin(); it != edges.end(); ++it) {
                auto edge = *it;
                volatile EdgeData& edge_data = graph->getEdgeData(edge);
                uint32_t edge_type = edge_data.type;
                (void) edge_type;

                if (person_university_distribution(generator) == 0 || std::next(it) == edges.end()) {
                    GNode dst = graph->getOutEdgeDst(edge);
                    NodeData& dst_data = graph->getData(dst);
                    uint32_t dst_index = dst_data.index;
                    (void) dst_index;
                    volatile Organization& dst_property = organization_memory[0];
                    university_id = dst_property.id;

                    uint32_t edge_index = edge_data.index;
                    (void) edge_index;
                    volatile Person_workOrStudyAt_Organization& edge_property = person_organization_memory[0];
                    class_year = edge_property.classYear;
                    break;
                }
            }
    
            for (auto src_edge : graph->outEdges(src)) {
                volatile EdgeData& src_edge_data = graph->getEdgeData(src_edge);
                uint32_t src_edge_type = src_edge_data.type;
                (void) src_edge_type;

                if (person_person_distribution(generator) == 0) {
                    NodeData& src_data = graph->getData(src);
                    uint32_t new_dist = src_data.dist_current + 1;
                        
                    GNode dst = graph->getOutEdgeDst(src_edge);

#ifndef GALOIS_FULL_MIRRORING
                    if (graph->isPhantom(dst)) {
                        net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getRemoteLID(dst), university_id, class_year, new_dist);
                    }
                    else {
#endif
                        if (!graph->isMaster(dst)) { // mirror
                            for (auto dst_edge : graph->outEdges(dst)) {
                                volatile EdgeData& dst_edge_data = graph->getEdgeData(dst_edge);
                                uint32_t dst_edge_type = dst_edge_data.type;
                                (void) dst_edge_type;

                                if (person_university_distribution(generator) == 0) {
                                    GNode dst_dst = graph->getOutEdgeDst(dst_edge);
                                    NodeData& dst_dst_data = graph->getData(dst_dst);
                                    uint32_t dst_dst_index = dst_dst_data.index;
                                    (void) dst_dst_index;
                                    volatile Organization& dst_dst_property = organization_memory[1];
                                    int64_t dst_dst_university_id = dst_dst_property.id;

                                    if (dst_dst_university_id == university_id) {
                                        if (same_university_distribution(generator) == 0) {
                                            uint32_t dst_edge_index = dst_edge_data.index;
                                            (void) dst_edge_index;
                                            volatile Person_workOrStudyAt_Organization& dst_edge_property = person_organization_memory[1];
                                            int32_t dst_class_year = dst_edge_property.classYear;
                                            
                                            int64_t class_year_diff = static_cast<int64_t>(class_year) - static_cast<int64_t>(dst_class_year);
                                            uint32_t abs_class_year_diff = static_cast<uint32_t>(std::abs(class_year_diff));
                                            new_dist += abs_class_year_diff;
                                            break;
                                        }
                                    }
                                }
                            }
                        }

                        auto& dnode = graph->getData(dst);     
                        bool dirty = galois::atomicMinBool(dnode.dist_current, new_dist);
      
                        if (dirty) {
                            dirty_bitset_ptr->set(dst);
                        }
#ifndef GALOIS_FULL_MIRRORING
                    }
#endif
                }
            }
        }
    }
};

/******************************************************************************/
/* Sanity check operators */
/******************************************************************************/

struct SSSPSanityCheck {
    Graph* graph;

    galois::DGAccumulator<uint64_t>& DGAccumulator_sum;
    galois::DGReduceMax<uint32_t>& DGMax;

    SSSPSanityCheck(
        Graph* _graph,
        galois::DGAccumulator<uint64_t>& dgas,
        galois::DGReduceMax<uint32_t>& dgm
    ) : graph(_graph), DGAccumulator_sum(dgas), DGMax(dgm) {}

    void static go(Graph& _graph, galois::DGAccumulator<uint64_t>& dgas, galois::DGReduceMax<uint32_t>& dgm) {
        dgas.reset();
        dgm.reset();

        galois::do_all(
            galois::iterate(_graph.masterNodesRange()),
            SSSPSanityCheck(&_graph, dgas, dgm),
            galois::no_stats());

        uint64_t num_visited  = dgas.reduce();
        uint32_t max_distance = dgm.reduce();

        if (galois::runtime::getSystemNetworkInterface().ID == 0) {
            galois::gPrint("Number of nodes visited from source ", src_node, " is ", num_visited, "\n");
            galois::gPrint("Max distance from source ", src_node, " is ", max_distance, "\n");
        }
    }

    void operator()(GNode src) const {
        NodeData& src_data = graph->getData(src);

        if (src_data.dist_current < infinity) {
            DGAccumulator_sum += 1;
            DGMax.update(src_data.dist_current);
        }
    }
};

/******************************************************************************/
/* Main */
/******************************************************************************/

constexpr static const char* const name = "Distributed Single Source Shortest Path (Push)";
constexpr static const char* const desc = "Distributed Single Source Shortest Path (Push)";
constexpr static const char* const url  = nullptr;

int main(int argc, char** argv) {
    galois::DistMemSys G;
    DistBenchStart(argc, argv, name, desc, url);

    auto& net = galois::runtime::getSystemNetworkInterface();
  
    if (net.ID == 0) {
        galois::runtime::reportParam(REGION_NAME, "Max Iterations", maxIterations);
    }

    if (partitionScheme != OEC) {
        galois::gPrint("This repo only supports OEC\n");
        return 1;
    }

    galois::StatTimer StatTimer_total("TimerTotal", REGION_NAME.c_str());
    StatTimer_total.start();
    galois::StatTimer StatTimer_preprocess("TimerPreProcess", REGION_NAME.c_str());
    StatTimer_preprocess.start();

    std::unique_ptr<Graph> hg;
    std::tie(hg, syncSubstrate) = distGraphInitialization<NodeData, EdgeData, CommData>();

    net.allocateBufferPool();
  
    hg->sortEdgesByDestination();

    galois::runtime::getHostBarrier().wait();
    net.partitionDone();

    bitset_dist_current_odd.resize(hg->actualSize());
    bitset_dist_current_even.resize(hg->actualSize());

    // accumulators for use in operators
    galois::DGAccumulator<uint64_t> DGAccumulator_sum;
    galois::DGReduceMax<uint32_t> m;
  
    if (srcSelection == randomValue) {
        // Get the src_nodes of the runs
        galois::StatTimer StatTimer_select("VertexSelection", REGION_NAME.c_str());
        StatTimer_select.start();
        uint64_t degree = 0;
        auto num_nodes = hg->globalSize();
        uint64_t cand = 0;
        while (degree < 1) {
            DGAccumulator_sum.reset();
            cand = generator() % num_nodes;

            if (hg->isOwned(cand) || hg->isLocal(cand)) {
                auto lcand = hg->getLID(cand);
                DGAccumulator_sum += hg->localDegree(lcand);
            }

            degree = DGAccumulator_sum.reduce();
        }
        src_node = cand;
        StatTimer_select.stop();
    }
    else if (srcSelection == explicitValue) {
        src_node = startNode;
    }
  
    DGAccumulator_sum.reset();
    
    galois::gPrint("[", net.ID, "] TypeAssignment begin\n");
    TypeAssignment(*hg);
    galois::gPrint("[", net.ID, "] TypeAssignment end\n");

    galois::gPrint("[", net.ID, "] PropertyConstruction begin\n");
    PropertyConstruction();
    galois::gPrint("[", net.ID, "] PropertyConstruction end\n");
    galois::runtime::getHostBarrier().wait();

    galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");
    InitializeGraph::go((*hg));
    galois::runtime::getHostBarrier().wait();
    StatTimer_preprocess.stop();
    
    for (auto run = 0; run < numRuns; ++run) {
        REGION_NAME_RUN = REGION_NAME + "_" + std::to_string(run);
        galois::gPrint("[", net.ID, "] SSSP::go run ", run, " called\n");
        std::string timer_str("Timer_" + std::to_string(run));
        galois::StatTimer StatTimer_main(timer_str.c_str(), REGION_NAME_RUN.c_str());

        net.touchBufferPool();
        galois::runtime::getHostBarrier().wait();

        StatTimer_main.start();
        SSSP::go(*hg); 
        StatTimer_main.stop();
        galois::gPrint("Host ", net.ID, " SSSP run ", run, " time: ", StatTimer_main.get(), " ms\n");

        SSSPSanityCheck::go(*hg, DGAccumulator_sum, m);

        if ((run + 1) != numRuns) {
        bitset_dist_current_odd.reset();
        bitset_dist_current_even.reset();

        (*syncSubstrate).set_num_run(run + 1);
        InitializeGraph::go((*hg));
        }
    }

    StatTimer_total.stop();
  
    net.applicationDone();

    return 0;
}
