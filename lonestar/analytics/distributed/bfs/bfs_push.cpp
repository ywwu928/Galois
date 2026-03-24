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

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

const uint32_t infinity = std::numeric_limits<uint32_t>::max();

struct NodeData {
  std::atomic<uint32_t> dist_current;
};

galois::DynamicBitSet bitset_dist_current_odd;
galois::DynamicBitSet bitset_dist_current_even;

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph, uint32_t>> syncSubstrate;

#include "bfs_sync.hh"

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/

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

struct BFS {
  Graph* graph;
  
  galois::DynamicBitSet* active_bitset_ptr;
  galois::DynamicBitSet* dirty_bitset_ptr;

  galois::runtime::NetworkInterface& net;
  
  BFS(Graph* _graph, galois::DynamicBitSet* _active_bitset_ptr, galois::DynamicBitSet* _dirty_bitset_ptr)
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
              galois::iterate(masterNodes), BFS(&_graph, &bitset_dist_current_odd, &bitset_dist_current_even),
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
              galois::iterate(masterNodes), BFS(&_graph, &bitset_dist_current_even, &bitset_dist_current_odd),
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
        uint32_t new_dist = snode.dist_current + 1;
    
        for (auto jj : graph->outEdges(src)) {
          GNode dst         = graph->getOutEdgeDst(jj);
#ifndef GALOIS_FULL_MIRRORING
          if (graph->isPhantom(dst)) {
            net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getRemoteLID(dst), new_dist);
          }
          else {
#endif
            auto& dnode       = graph->getData(dst);     
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
};

/******************************************************************************/
/* Sanity check operators */
/******************************************************************************/

/* Prints total number of nodes visited + max distance */
struct BFSSanityCheck {
  Graph* graph;

  galois::DGAccumulator<uint64_t>& DGAccumulator_sum;
  galois::DGReduceMax<uint32_t>& DGMax;

  BFSSanityCheck(Graph* _graph,
                 galois::DGAccumulator<uint64_t>& dgas,
                 galois::DGReduceMax<uint32_t>& dgm)
      : graph(_graph), DGAccumulator_sum(dgas), DGMax(dgm) {}

  void static go(Graph& _graph, galois::DGAccumulator<uint64_t>& dgas, galois::DGReduceMax<uint32_t>& dgm) {
    dgas.reset();
    dgm.reset();

    galois::do_all(galois::iterate(_graph.masterNodesRange()),
                     BFSSanityCheck(&_graph, dgas, dgm),
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

constexpr static const char* const name = "Distributed Breadth-First Search (Push)";
constexpr static const char* const desc = "Distributed Breadth-First Search (Push)";
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
  std::tie(hg, syncSubstrate) = distGraphInitialization<NodeData, void, uint32_t>();

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

  galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");

  InitializeGraph::go((*hg));
  galois::runtime::getHostBarrier().wait();
  StatTimer_preprocess.stop();
    
  for (auto run = 0; run < numRuns; ++run) {
    REGION_NAME_RUN = REGION_NAME + "_" + std::to_string(run);
    galois::gPrint("[", net.ID, "] BFS::go run ", run, " called\n");
    std::string timer_str("Timer_" + std::to_string(run));
    galois::StatTimer StatTimer_main(timer_str.c_str(), REGION_NAME_RUN.c_str());

    net.touchBufferPool();
    galois::runtime::getHostBarrier().wait();

    StatTimer_main.start();
    BFS::go(*hg); 
    StatTimer_main.stop();
    galois::gPrint("Host ", net.ID, " BFS run ", run, " time: ", StatTimer_main.get(), " ms\n");

    BFSSanityCheck::go(*hg, DGAccumulator_sum, m);

    if ((run + 1) != numRuns) {
      bitset_dist_current_odd.reset();
      bitset_dist_current_even.reset();

      (*syncSubstrate).set_num_run(run + 1);
      InitializeGraph::go((*hg));
    }
  }

  StatTimer_total.stop();
  
  net.applicationDone();
  
  if (output) {
      std::vector<uint32_t> results = makeResults(hg);
      auto globalIDs                = hg->getMasterGlobalIDs();
      assert(results.size() == globalIDs.size());

      writeOutput(outputLocation, "level", results.data(), results.size(), globalIDs.data());
  }

  return 0;
}
