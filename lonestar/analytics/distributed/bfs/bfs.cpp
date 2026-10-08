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
static std::string TYPE_NAME;

enum Exp {
    Pull,
    Push,
    Global_Heuristic,
    Global_Hysteresis,
    Local_Heuristic,
    Local_Hysteresis,
    Hybrid_Edge,
    Hybrid_Degree,
    Exp_Count
};

std::string exp_names[] = {
    "Pull",
    "Push",
    "Global_Heuristic",
    "Global_Hysteresis",
    "Local_Heuristic",
    "Local_Hysteresis",
    "Hybrid_Edge",
    "Hybrid_Degree"
};

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

static cll::opt<float> lower_bound("lower_bound",
                                   cll::desc("edge density lower bound for switching"),
                                   cll::init(0.05));

static cll::opt<float> upper_bound("upper_bound",
                                   cll::desc("edge density upper bound for switching"),
                                   cll::init(0.8));

static cll::opt<int> degree_density_bound("degree_density_bound",
                                           cll::desc("degree density bound for switching"),
                                           cll::init(50));

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

const uint32_t infinity = std::numeric_limits<uint32_t>::max();

struct NodeData {
  std::atomic<uint32_t> dist_current;
  uint32_t dist_old;
};

galois::DynamicBitSet bitset_active;
galois::DynamicBitSet bitset_dirty;

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
        sdata.dist_old = 0;
        bitset_dirty.set(src);
    }
    else {
        sdata.dist_current = infinity;
        sdata.dist_old = infinity;
    }
  }
};

struct BFS_sync {
  Graph* graph;

  galois::GAccumulator<uint64_t>& active_vertices;
  galois::GAccumulator<uint64_t>& active_edges;

  BFS_sync(Graph* _graph, galois::GAccumulator<uint64_t>& _active_vertices, galois::GAccumulator<uint64_t>& _active_edges)
      : graph(_graph),
        active_vertices (_active_vertices),
        active_edges (_active_edges) {}

  void static go(Graph& _graph, galois::GAccumulator<uint64_t>& _active_vertices, galois::GAccumulator<uint64_t>& _active_edges) {
    const auto& masterNodes = _graph.masterNodesRange();

    galois::do_all(
        galois::iterate(masterNodes),
        BFS_sync{&_graph, _active_vertices, _active_edges}, galois::no_stats());
  }

  void operator()(GNode src) const {
    if (bitset_dirty.test(src)) {
      bitset_active.set(src);
      auto& sdata = graph->getData(src);
      sdata.dist_old = sdata.dist_current;

      active_vertices += 1;
      uint64_t nout = std::distance(graph->edge_begin(src), graph->edge_end(src));
      active_edges += nout;
    }
  }
};

struct PullRemote {
  Graph* graph;

  galois::runtime::NetworkInterface& net;

  PullRemote(Graph* _graph)
      : graph(_graph),
        net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph) {
      const auto& remoteNodes = _graph.remoteNodesRangeIn();

      galois::do_all(
          galois::iterate(remoteNodes), PullRemote{&_graph},
          galois::steal(), galois::no_stats());
  }

  void operator()(GNode dst) const {
    uint32_t ddist = UINT32_MAX;
    for (auto jj : graph->inEdges(dst)) {
        GNode src         = graph->getInEdgeSrc(jj);
        if (bitset_active.test(src)) {
            auto& snode       = graph->getData(src);
            if (snode.dist_current + 1 < ddist) {
                ddist = snode.dist_old + 1;
            }
        }
    }
    
    if (ddist != UINT32_MAX) {
        net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getRemoteLID(dst), ddist);
    }
  }
};

struct PullMaster {
  Graph* graph;
  
  PullMaster(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
      const auto& masterNodes = _graph.masterNodesRangeIn();
      
      galois::do_all(
          galois::iterate(masterNodes), PullMaster{&_graph},
          galois::steal(), galois::no_stats());
  }

  void operator()(GNode dst) const {
    NodeData& dnode = graph->getData(dst);

    for (auto jj : graph->inEdges(dst)) {
        GNode src         = graph->getInEdgeSrc(jj);
        if (bitset_active.test(src)) {
            auto& snode       = graph->getData(src);
            galois::minVoid(dnode.dist_current, snode.dist_old + 1);
        }
    }

    if (dnode.dist_old > dnode.dist_current) {
        bitset_dirty.set(dst);
    }
  }
};

struct Push {
  Graph* graph;

  galois::runtime::NetworkInterface& net;
  
  Push(Graph* _graph)
      : graph(_graph),
        net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph) {
      const auto& masterNodes = _graph.masterNodesRange();
    
      galois::do_all(
          galois::iterate(masterNodes), Push(&_graph),
          galois::no_stats(), galois::steal());
  }

  void operator()(GNode src) const {
    if (bitset_active.test(src)) {
        NodeData& snode = graph->getData(src);
    
        for (auto jj : graph->outEdges(src)) {
          GNode dst         = graph->getOutEdgeDst(jj);
          if (graph->isPhantom(dst)) {
            net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getRemoteLID(dst), snode.dist_old + 1);
          }
          else {
            auto& dnode       = graph->getData(dst);     
            bool dirty = galois::atomicMinBool(dnode.dist_current, snode.dist_old + 1);
          
            if (dirty) {
              bitset_dirty.set(dst);
            }
          }
        }
    }
  }
};

struct BFS {
  Graph* graph;
  
  BFS(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph, Exp exp) {
#ifdef GALOIS_USER_STATS
    constexpr bool USER_STATS = true;
#else
    constexpr bool USER_STATS = false;
#endif

    unsigned _num_iterations = 0;
    
    auto& _net = galois::runtime::getSystemNetworkInterface();

    galois::GAccumulator<uint64_t> active_v, active_e;
    uint64_t local_active_v;
    uint64_t local_active_e;
    uint64_t global_active_e;
  
    bool pull = true;
    bool dual = false;
    bool local = false;
    bool hysteresis = false;
    bool hybrid = false;
    bool degree = false;

    switch (exp) {
        case Pull: {
            pull = true;
            break;
        }
        case Push: {
            pull = false;
            break;
        }
        case Global_Heuristic: {
            dual = true;
            hysteresis = false;
            break;
        }
        case Global_Hysteresis: {
            pull = true;
            dual = true;
            hysteresis = true;
            break;
        }
        case Local_Heuristic: {
            dual = true;
            local = true;
            hysteresis = false;
            break;
        }
        case Local_Hysteresis: {
            pull = true;
            dual = true;
            local = true;
            hysteresis = true;
            break;
        }
        case Hybrid_Edge: {
            pull = true;
            dual = true;
            local = true;
            hysteresis = true;
            hybrid = true;
            break;
        }
        case Hybrid_Degree: {
            pull = true;
            dual = true;
            local = true;
            hysteresis = true;
            hybrid = true;
            degree = true;
            break;
        }
        case Exp_Count: {
            galois::gPrint("Error: Unknown experiment!\n");
            return;
        }
    }

    bool forcePush = _graph.forcePush();

    uint64_t active_edges;
    uint64_t edge_threshold_low, edge_threshold_high;
    if (local) {
        edge_threshold_low = _graph.sizeEdges() * lower_bound;
        edge_threshold_high = _graph.sizeEdges() * upper_bound;
    }
    else {
        edge_threshold_low = _graph.globalSizeEdges() * lower_bound;
        edge_threshold_high = _graph.globalSizeEdges() * upper_bound;
    }
    uint64_t vertex_threshold_low = _graph.numMasters() * lower_bound;
    uint64_t vertex_threshold_high = _graph.numMasters() * upper_bound;
    float degree_threshold = (_graph.sizeEdges() / _graph.numMasters()) * degree_density_bound;

    while (true) {
      if (_num_iterations >= maxIterations) {
          break;
      }

#ifdef GALOIS_PRINT_PROCESS
      galois::gPrint("Host ", _net.ID, " : iteration ", _num_iterations, "\n");
#endif
      
      std::string total_str(TYPE_NAME + "_Total_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_total(total_str.c_str(), REGION_NAME_RUN.c_str());
      std::string sync_str("Sync_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_sync(sync_str.c_str(), REGION_NAME_RUN.c_str());
      std::string active_str(TYPE_NAME + "_Active_Reduce_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_active(active_str.c_str(), REGION_NAME_RUN.c_str());
      
      StatTimer_total.start();
      bitset_active.reset();
      active_v.reset();
      active_e.reset();
      
      StatTimer_sync.start();
      BFS_sync::go(_graph, active_v, active_e);
      StatTimer_sync.stop();
      local_active_v = active_v.reduce();
      local_active_e = active_e.reduce();

      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "Active_Vertices_Round_" + std::to_string(_num_iterations), local_active_v);
      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "Active_Edges_Round_" + std::to_string(_num_iterations), local_active_e);
      
      StatTimer_active.start();
      global_active_e = 0;
      MPI_Allreduce(&local_active_e, &global_active_e, 1,
                    MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
      StatTimer_active.stop();

      if (global_active_e == 0) {
          StatTimer_total.stop();
          break;
      }
      
      std::string compute_str("Compute_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_compute(compute_str.c_str(), REGION_NAME_RUN.c_str());
      std::string comm_str("Communication_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_comm(comm_str.c_str(), REGION_NAME_RUN.c_str());

      syncSubstrate->set_num_round(_num_iterations);

      if (dual) {
          if (local && forcePush) {
              pull = false;
          }
          else if (hybrid) {
              if (local_active_v >= vertex_threshold_high) {
                  pull = true;
              }
              else if (local_active_v <= vertex_threshold_low) {
                  pull = false;
              }
              else if (local_active_e >= edge_threshold_high) {
                  pull = true;
              }
              else if (local_active_e <= edge_threshold_low) {
                  pull = false;
              }
              else if (degree) {
                  pull = (local_active_e / local_active_v) >= degree_threshold;
              }
          }
          else {
              active_edges = local ? local_active_e : global_active_e;

              if (hysteresis) {
                  if (active_edges >= edge_threshold_high) {
                      pull = true;
                  }
                  else if (active_edges <= edge_threshold_low) {
                      pull = false;
                  }
              }
              else {
                  pull = active_edges > edge_threshold_low;
              }
          }
      }
      
      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "Pull_Round_" + std::to_string(_num_iterations), pull);
      
      bitset_dirty.reset();

      if (pull) {
          StatTimer_compute.start();
          PullRemote::go(_graph);
          _net.flushRemoteWork();
          PullMaster::go(_graph);
          StatTimer_compute.stop();
      }
      else {
          StatTimer_compute.start();
          Push::go(_graph);
          _net.flushRemoteWork();
          StatTimer_compute.stop();
      }

      StatTimer_comm.start();
      syncSubstrate->reduce<Reduce_min_dist_current>(&bitset_dirty);
      StatTimer_comm.stop();
      
      _net.resetWorkTermination();

      ++_num_iterations;
      StatTimer_total.stop();
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
  hg->sortInEdgesBySource();

  galois::runtime::getHostBarrier().wait();
  net.partitionDone();

  bitset_active.resize(hg->numMasters());
  bitset_dirty.resize(hg->actualSize());

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
  StatTimer_preprocess.stop();
    
  for (int i=0; i<Exp_Count; i++) {
    TYPE_NAME = exp_names[i];

    for (auto run = 0; run < numRuns; ++run) {
        galois::gPrint("[", net.ID, "] BFS (", TYPE_NAME, ") run ", run, " start\n");
      
        bitset_active.reset();
        bitset_dirty.reset();
        InitializeGraph::go((*hg));
        galois::runtime::getHostBarrier().wait();

        REGION_NAME_RUN = REGION_NAME + "_" + TYPE_NAME + "_" + std::to_string(run);
        std::string main_timer_str("Timer_" + std::to_string(run));
        galois::StatTimer StatTimer_main(main_timer_str.c_str(), REGION_NAME_RUN.c_str());

        StatTimer_main.start();
        BFS::go(*hg, static_cast<Exp>(i));
        StatTimer_main.stop();
        galois::gPrint("Host ", net.ID, " BFS (", TYPE_NAME, ") run ", run, " time: ", StatTimer_main.get(), " ms\n");

        BFSSanityCheck::go(*hg, DGAccumulator_sum, m);

        (*syncSubstrate).set_num_run(run + 1);
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
