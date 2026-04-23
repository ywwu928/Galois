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

#include <iostream>
#include <sstream>
#include <limits>
#include <random>
#include <vector>
#include <unordered_set>

#include "snb_data_structure.h"

static std::string REGION_NAME = "SSSP";
static std::string REGION_NAME_RUN;

/******************************************************************************/
/* Declaration of command line arguments */
/******************************************************************************/

namespace cll = llvm::cl;

static cll::opt<unsigned int> maxIterations("maxIterations",
                                            cll::desc("Maximum iterations: "
                                                      "Default 1000"),
                                            cll::init(1000));

enum selectionMode { randomValue, explicitValue };

static cll::opt<selectionMode> srcSelection(
    "srcSelection", cll::desc("Start Node Selection Mode"),
    cll::values(clEnumVal(randomValue, "Selected by random number generator with seed"),
                clEnumVal(explicitValue, "User explicitly specify the starting node ID")),
    cll::init(explicitValue));

static uint64_t src_node;
static cll::opt<unsigned> rseed("rseed", cll::desc("The random seed for choosing the hosts (default value 0)"), cll::init(0));
static cll::opt<uint64_t> startNode("startNode", cll::desc("ID of the start node"), cll::init(0));
      

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

const uint32_t infinity = std::numeric_limits<uint32_t>::max();

struct NodeData {
    uint32_t type;
    uint32_t index;
    std::atomic<uint32_t> dist_current;
};

struct EdgeData {
    uint32_t type;
    uint32_t index;
    bool flag;
    uint32_t weight;
};

galois::DynamicBitSet bitset_dist_current_odd;
galois::DynamicBitSet bitset_dist_current_even;

typedef galois::graphs::DistGraph<NodeData, EdgeData> Graph;
typedef typename Graph::GraphNode GNode;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph, uint32_t>> syncSubstrate;

#include "sssp_sync.hh"

// Setup Seeding Information
std::mt19937 generator(rseed);

std::vector<double> master_weights = {0.0025, 0.0005, 0.0051, 0.0, 0.645, 0.0284, 0.0031, 0.3154};
std::discrete_distribution<> master_distribution(master_weights.begin(), master_weights.end());

std::vector<double> mirror_weights = {0.00417, 0.00353, 0.0681, 0.00022, 0.33865, 0.0, 0.05803, 0.5273};
std::discrete_distribution<> mirror_distribution(mirror_weights.begin(), mirror_weights.end());

std::vector<double> edge_weights = {
    0.0005, 0.0001, 0.0009, 0.0, 0.1189,
    0.1564, 0.1189, 0.0603, 0.0586, 0.0582,
    0.0987, 0.0180, 0.0133, 0.0006, 0.0105,
    0.0834, 0.0436, 0.0017, 0.0582, 0.0413,
    0.0582
};
std::discrete_distribution<> edge_distribution(edge_weights.begin(), edge_weights.end());

std::vector<Organization> organization_memory;
std::vector<Place> place_memory;
std::vector<Tag> tag_memory;
std::vector<TagClass> tagclass_memory;
std::vector<Comment> comment_memory;
std::vector<Forum> forum_memory;
std::vector<Person> person_memory;
std::vector<Post> post_memory;

std::vector<Forum_hasMemberOrModerator_Person> forum_person_memory;
std::vector<Person_knows_Person> person_person_memory;
std::vector<Person_likes_Comment> person_comment_memory;
std::vector<Person_likes_Post> person_post_memory;
std::vector<Person_workOrStudyAt_Organization> person_organization_memory;

uint32_t vertex_index[8];
uint32_t edge_index[21];

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/

void AssignProperty(Graph& graph) {
    uint32_t type;

    for (int i=0; i<8; i++) {
        vertex_index[i] = 0;
    }

    for (int i=0; i<21; i++) {
        edge_index[i] = 0;
    }

    // masters and edges
    for (uint32_t lid=0; lid<graph.numMasters(); lid++) {
        NodeData& node_data = graph.getData(lid);

        type = master_distribution(generator);
        node_data.type = type;
        node_data.index = vertex_index[type];
        vertex_index[type]++;
    
        for (auto edge : graph.outEdges(lid)) {
            EdgeData& edge_data = graph.getEdgeData(edge);
            
            type = edge_distribution(generator);
            edge_data.type = type;
            edge_data.index = edge_index[type];
            edge_index[type]++;
        }
    }
    
    // mirrors
    for (uint32_t offset=0; offset<graph.numMirrors(); offset++) {
        uint32_t lid = graph.numMasters() + offset;
        NodeData& node_data = graph.getData(lid);

        type = mirror_distribution(generator);
        node_data.type = type;
        node_data.index = vertex_index[type];
        vertex_index[type]++;
    }
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
        
        for (auto edge : graph->outEdges(src)) {
            EdgeData& edata = graph->getEdgeData(edge);
            edata.flag = true;
            edata.weight = 1;
        }
    }
};

struct GraphProjection {
    Graph* graph;

    GraphProjection(Graph* _graph) : graph(_graph) {}

    void static go(Graph& _graph) {
        const auto& masterNodes = _graph.masterNodesRange();
    
        galois::do_all(
            galois::iterate(masterNodes),
            GraphProjection(&_graph), galois::no_stats());
    }

    void operator()(GNode src) const {
        NodeData& sdata = graph->getData(src);
        if (sdata.type == 0) { // organization
            std::unordered_set<GNode> destinations;
            
            for (auto edge : graph->outEdges(src)) {
                GNode dst = graph->getOutEdgeDst(edge);
                destinations.insert(dst);
                
                // dummy edge property read
                EdgeData& edata = graph->getEdgeData(edge);
                if (edata.type == 10) {
                    volatile char temp = forum_person_memory[edata.index].creationDate[0];
                } else if (edata.type == 14) {
                    volatile char temp = person_person_memory[edata.index].creationDate[0];
                } else if (edata.type == 15) {
                    volatile char temp = person_comment_memory[edata.index].creationDate[0];
                } else if (edata.type == 16) {
                    volatile char temp = person_post_memory[edata.index].creationDate[0];
                } else if (edata.type == 17) {
                    volatile int32_t temp = person_organization_memory[edata.index].classYear;
                }
            }

            for (auto vertex : destinations) {
                for (auto edge : graph->outEdges(vertex)) {
                    GNode dst = graph->getOutEdgeDst(edge);
                    if (destinations.find(dst) != destinations.end()) {
                        EdgeData& edata = graph->getEdgeData(edge);
                        edata.flag = true;
                        edata.weight = 1;
                    }
                }
            }
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
                syncSubstrate->reduce<Reduce_min_dist_current, Bitset_dist_current_even>();
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
                syncSubstrate->reduce<Reduce_min_dist_current, Bitset_dist_current_odd>();
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
            NodeData& snode = graph->getData(src);
    
            for (auto edge : graph->outEdges(src)) {
                EdgeData& edata = graph->getEdgeData(edge);

                if (edata.flag == true) {
                    uint32_t new_dist = snode.dist_current + edata.weight;
                    GNode dst = graph->getOutEdgeDst(edge);
#ifndef GALOIS_FULL_MIRRORING
                    if (graph->isPhantom(dst)) {
                        net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getRemoteLID(dst), new_dist);
                    }
                    else {
#endif
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

/* Prints total number of nodes visited + max distance */
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

        // Only host 0 will print the info
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
    std::tie(hg, syncSubstrate) = distGraphInitialization<NodeData, EdgeData, uint32_t>();

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
    
    galois::gPrint("[", net.ID, "] AssignProperty begin\n");
    AssignProperty(*hg);
    galois::gPrint("[", net.ID, "] AssignProperty end\n");
  
    galois::gPrint("[", net.ID, "] ConstructProperty begin\n");
    organization_memory.resize(vertex_index[0]);
    place_memory.resize(vertex_index[1]);
    tag_memory.resize(vertex_index[2]);
    tagclass_memory.resize(vertex_index[3]);
    comment_memory.resize(vertex_index[4]);
    forum_memory.resize(vertex_index[5]);
    person_memory.resize(vertex_index[6]);
    post_memory.resize(vertex_index[7]);
    
    forum_person_memory.resize(edge_index[10]);
    person_person_memory.resize(edge_index[14]);
    person_comment_memory.resize(edge_index[15]);
    person_post_memory.resize(edge_index[16]);
    person_organization_memory.resize(edge_index[17]);
    galois::gPrint("[", net.ID, "] ConstructProperty end\n");

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
        GraphProjection::go(*hg);
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
