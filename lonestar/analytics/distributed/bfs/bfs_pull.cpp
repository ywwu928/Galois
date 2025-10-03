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
#include "galois/DReducible.h"
#include "galois/gstl.h"
#include "galois/runtime/Tracer.h"

#include <iostream>
#include <limits>
#include <random>

static std::string REGION_NAME = "BFS";
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

enum IterMode { All, Separate };

static cll::opt<IterMode> iterMode(
    "iterMode", cll::desc("Iterate Mode (default value All):"),
    cll::values(clEnumVal(All, "iterate through all nodes"),
                clEnumVal(Separate, "iterate through present nodes first and then phantom nodes")),
    cll::init(All));

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

const uint32_t infinity = std::numeric_limits<uint32_t>::max() / 4;

struct NodeData {
    std::atomic<uint32_t> dist_current;
};

galois::DynamicBitSet bitset_dist_current;

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph, uint32_t>> syncSubstrate;

#include "bfs_pull_sync.hh"

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/

struct InitializeGraph {
  const uint32_t& local_infinity;
  uint64_t local_src_node;
  Graph* graph;

  InitializeGraph(uint64_t& _src_node, const uint32_t& _infinity,
                  Graph* _graph)
      : local_infinity(_infinity), local_src_node(_src_node), graph(_graph) {}

  void static go(Graph& _graph) {
    const auto& presentNodes = _graph.presentNodesRange();
    galois::do_all(
        galois::iterate(presentNodes),
        InitializeGraph(src_node, infinity, &_graph), galois::no_stats());
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    sdata.dist_current = (graph->getGID(src) == local_src_node) ? 0 : local_infinity;
    bitset_dist_current.set(src);
  }
};

struct BFSPresent {
  Graph* graph;

  using DGTerminatorDetector = galois::DGAccumulator<unsigned int>;

  DGTerminatorDetector& active_vertices;

  BFSPresent(Graph* _graph, DGTerminatorDetector& _dga) : graph(_graph), active_vertices(_dga) {}

  void static go(Graph& _graph, DGTerminatorDetector& dga) {
      const auto& presentNodes = _graph.presentNodesRange();
      
      // launch all other threads to compute
      galois::do_all(
          galois::iterate(presentNodes), BFSPresent{&_graph, dga},
          galois::steal(), galois::no_stats());
  }

  // Pull from neighbor nodes, then add to self
  void operator()(GNode src) const {
    NodeData& snode = graph->getData(src);

    bool updated = false;
    for (auto jj : graph->edges(src)) {
        GNode dst         = graph->getEdgeDst(jj);
        auto& dnode       = graph->getData(dst);
        uint32_t new_dist = dnode.dist_current + 1;
        uint32_t old_dist = galois::min(snode.dist_current, new_dist);
        if (old_dist > new_dist) {
            updated = true;
        }
    }

    if (updated) {
        bitset_dist_current.set(src);
        active_vertices += 1;
    }
  }
};

struct BFSPhantom {
  Graph* graph;

  galois::runtime::NetworkInterface& net;

  BFSPhantom(Graph* _graph) : graph(_graph), net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph) {
      const auto& phantomNodes = _graph.phantomNodesRange();

      // launch all other threads to compute
      galois::do_all(
          galois::iterate(phantomNodes), BFSPhantom{&_graph},
          galois::steal(), galois::no_stats());
  }

  // Pull from neighbor nodes, then add to self
  void operator()(GNode src) const {
    // source node must be phantom
    // create register for phantom node data
    uint32_t sdist = UINT32_MAX;

    bool send = false;
    for (auto jj : graph->edges(src)) {
        GNode dst         = graph->getEdgeDst(jj);
        auto& dnode       = graph->getData(dst);

        if (dnode.dist_current + 1 < sdist) {
            sdist = dnode.dist_current + 1;
            if (bitset_dist_current.test(dst)) {
                send = true;
            }
            else {
                send = false;
            }
        }
    }

    if (send) {
        net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(src), graph->getPhantomRemoteLID(src), sdist);
    }
  }
};

struct BFSSep {
  Graph* graph;

  using DGTerminatorDetector = galois::DGAccumulator<unsigned int>;

  BFSSep(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
#ifdef GALOIS_USER_STATS
    constexpr bool USER_STATS = true;
#else
    constexpr bool USER_STATS = false;
#endif

    unsigned _num_iterations   = 0;

    DGTerminatorDetector dga;
  
    auto& _net = galois::runtime::getSystemNetworkInterface();

    do {
      std::string total_str("Total_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_total(total_str.c_str(), REGION_NAME_RUN.c_str());
      std::string compute_str("Compute_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_compute(compute_str.c_str(), REGION_NAME_RUN.c_str());
      std::string flush_str("Flush_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_flush(flush_str.c_str(), REGION_NAME_RUN.c_str());
      std::string comm_str("Communication_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_comm(comm_str.c_str(), REGION_NAME_RUN.c_str());

#ifdef GALOIS_PRINT_PROCESS
      galois::gPrint("Host ", _net.ID, " : iteration ", _num_iterations, "\n");
#endif

      StatTimer_total.start();
      syncSubstrate->set_num_round(_num_iterations);

      dga.reset();

      _net.prefetchBuffers();

      StatTimer_compute.start();
      BFSPresent::go(_graph, dga);
      BFSPhantom::go(_graph);
      StatTimer_compute.stop();

      // inform all other hosts that this host has finished sending messages
      // force all messages to be processed before continuing
      StatTimer_flush.start();
      _net.flushRemoteWork();
      StatTimer_flush.stop();

      bitset_dist_current.reset();

      StatTimer_comm.start();
      syncSubstrate->poll_for_remote_work_bitset<Reduce_min_dist_current>(dga, bitset_dist_current);
      StatTimer_comm.stop();
      
      _net.resetWorkTermination();

      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "NumWorkItems_Round_" + std::to_string(_num_iterations), (unsigned long)dga.read_local());

      ++_num_iterations;

      StatTimer_total.stop();
    } while ((_num_iterations < maxIterations) && dga.reduce(syncSubstrate->get_run_identifier()));
  }
};

struct BFSAll {
  Graph* graph;
  using DGTerminatorDetector = galois::DGAccumulator<unsigned int>;

  DGTerminatorDetector& active_vertices;
  
  galois::runtime::NetworkInterface& net;

  BFSAll(Graph* _graph, DGTerminatorDetector& _dga)
      : graph(_graph), active_vertices(_dga), net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph) {
#ifdef GALOIS_USER_STATS
    constexpr bool USER_STATS = true;
#else
    constexpr bool USER_STATS = false;
#endif

    unsigned _num_iterations = 0;

    const auto& allNodes = _graph.allNodesRange();
    
    DGTerminatorDetector dga;
  
    auto& _net = galois::runtime::getSystemNetworkInterface();

    do {
      std::string total_str("Total_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_total(total_str.c_str(), REGION_NAME_RUN.c_str());
      std::string compute_str("Compute_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_compute(compute_str.c_str(), REGION_NAME_RUN.c_str());
      std::string flush_str("Flush_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_flush(flush_str.c_str(), REGION_NAME_RUN.c_str());
      std::string comm_str("Communication_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_comm(comm_str.c_str(), REGION_NAME_RUN.c_str());

#ifdef GALOIS_PRINT_PROCESS
      galois::gPrint("Host ", _net.ID, " : iteration ", _num_iterations, "\n");
#endif

      StatTimer_total.start();
      syncSubstrate->set_num_round(_num_iterations);

      dga.reset();

      _net.prefetchBuffers();

      // launch all other threads to compute
      StatTimer_compute.start();
      galois::do_all(
          galois::iterate(allNodes), BFSAll(&_graph, dga),
          galois::no_stats(), galois::steal());
      StatTimer_compute.stop();
      
      // inform all other hosts that this host has finished sending messages
      // force all messages to be processed before continuing
      StatTimer_flush.start();
      _net.flushRemoteWork();
      StatTimer_flush.stop();

      StatTimer_comm.start();
      syncSubstrate->poll_for_remote_work_active<Reduce_min_dist_current>(dga);
      StatTimer_comm.stop();
      
      _net.resetWorkTermination();

      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "NumWorkItems_Round_" + std::to_string(_num_iterations), (unsigned long)dga.read_local());

      ++_num_iterations;

      StatTimer_total.stop();
    } while ((_num_iterations < maxIterations) && dga.reduce(syncSubstrate->get_run_identifier()));
  }

  void operator()(GNode src) const {
    // source node can be master, mirror or phantom
    if (graph->isPhantom(src)) {
        // create register for phantom node data
        uint32_t sdist = UINT32_MAX;

        for (auto jj : graph->edges(src)) {
            GNode dst         = graph->getEdgeDst(jj);
            auto& dnode       = graph->getData(dst);

            uint32_t new_dist = dnode.dist_current + 1;
            if (new_dist < sdist) {
                sdist = new_dist;
            }
        }
    
        if (sdist != UINT32_MAX) {
            net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(src), graph->getPhantomRemoteLID(src), sdist);
        }
    } else {
        NodeData& snode = graph->getData(src);

        bool updated = false;
        for (auto jj : graph->edges(src)) {
            GNode dst         = graph->getEdgeDst(jj);
            auto& dnode       = graph->getData(dst);
            uint32_t new_dist = dnode.dist_current + 1;
            uint32_t old_dist = galois::min(snode.dist_current, new_dist);
            if (old_dist > new_dist) {
                updated = true;
            }
        }

        if (updated) {
            active_vertices += 1;
        }
    }
  }
};

/******************************************************************************/
/* Sanity check operators */
/******************************************************************************/

/* Prints total number of nodes visited + max distance */
struct BFSSanityCheck {
  const uint32_t& local_infinity;
  Graph* graph;

  galois::DGAccumulator<uint64_t>& DGAccumulator_sum;
  galois::DGReduceMax<uint32_t>& DGMax;

  BFSSanityCheck(const uint32_t& _infinity, Graph* _graph,
                 galois::DGAccumulator<uint64_t>& dgas,
                 galois::DGReduceMax<uint32_t>& dgm)
      : local_infinity(_infinity), graph(_graph), DGAccumulator_sum(dgas), DGMax(dgm) {}

  void static go(Graph& _graph, galois::DGAccumulator<uint64_t>& dgas, galois::DGReduceMax<uint32_t>& dgm) {
    dgas.reset();
    dgm.reset();

    galois::do_all(galois::iterate(_graph.masterNodesRange().begin(),
                                   _graph.masterNodesRange().end()),
                     BFSSanityCheck(infinity, &_graph, dgas, dgm),
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

    if (src_data.dist_current < local_infinity) {
      DGAccumulator_sum += 1;
      DGMax.update(src_data.dist_current);
    }
  }
};

/******************************************************************************/
/* Make results */
/******************************************************************************/

std::vector<uint32_t> makeResults(std::unique_ptr<Graph>& hg) {
  std::vector<uint32_t> values;

  values.reserve(hg->numMasters());
  for (auto node : hg->masterNodesRange()) {
    values.push_back(hg->getData(node).dist_current);
  }

  return values;
}

/******************************************************************************/
/* Main */
/******************************************************************************/

constexpr static const char* const name = "BFS pull - Distributed Heterogeneous";
constexpr static const char* const desc = "BFS pull on Distributed Galois.";
constexpr static const char* const url  = nullptr;

int main(int argc, char** argv) {
  galois::DistMemSys G;
  DistBenchStart(argc, argv, name, desc, url);

  auto& net = galois::runtime::getSystemNetworkInterface();

  if (net.ID == 0) {
    galois::runtime::reportParam(REGION_NAME, "Source Node ID", src_node);
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
  std::tie(hg, syncSubstrate) = distGraphInitialization<NodeData, void, uint32_t, false>();

  bitset_dist_current.resize(hg->size());

  // accumulators for use in operators
  galois::DGAccumulator<uint64_t> DGAccumulator_sum;
  galois::DGReduceMax<uint32_t> m;
  
  if (srcSelection == randomValue) {
      // Setup Seeding Information
      std::mt19937 generator(rseed);
      
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

  galois::runtime::getHostBarrier().wait();
  net.forwardPass();
  StatTimer_preprocess.stop();

  for (auto run = 0; run < numRuns; ++run) {
    REGION_NAME_RUN = REGION_NAME + "_" + std::to_string(run);
    
    syncSubstrate->set_num_run(run);
      
    bitset_dist_current.reset();
  
    galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");

    InitializeGraph::go((*hg));
    galois::runtime::getHostBarrier().wait();

    galois::gPrint("[", net.ID, "] BFS::go run ", run, " called\n");
    
    std::string timer_str("Timer_" + std::to_string(run));
    galois::StatTimer StatTimer_main(timer_str.c_str(), REGION_NAME_RUN.c_str());

    net.touchBufferPool();
    galois::runtime::getHostBarrier().wait();
    
    StatTimer_main.start();
    if (iterMode == All) {
      BFSAll::go(*hg);
    } else {
      BFSSep::go(*hg);
    }
    StatTimer_main.stop();
    galois::gPrint("Host ", net.ID, " BFS run ", run, " time: ", StatTimer_main.get(), " ms\n");

    // sanity check
    BFSSanityCheck::go(*hg, DGAccumulator_sum, m);
  
    if (output) {
        std::vector<uint32_t> results = makeResults(hg);
        auto globalIDs                = hg->getMasterGlobalIDs();
        assert(results.size() == globalIDs.size());

        writeOutput(outputLocation, "level", results.data(), results.size(), globalIDs.data());
    }
  }

  StatTimer_total.stop();

  net.applicationDone();

  return 0;
}
