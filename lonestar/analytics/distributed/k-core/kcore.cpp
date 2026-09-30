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

/******************************************************************************/
/* Sync code/calls was manually written, not compiler generated */
/******************************************************************************/

#include "DistBench/Output.h"
#include "DistBench/Start.h"
#include "galois/DistGalois.h"
#include "galois/DReducible.h"
#include "galois/gstl.h"
#include "galois/runtime/Tracer.h"

#include <iostream>
#include <limits>

static std::string REGION_NAME = "KCore";
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
static cll::opt<unsigned int>
    maxIterations("maxIterations",
                  cll::desc("Maximum iterations: Default 10000"),
                  cll::init(10000));
// required k specification for k-core
static cll::opt<unsigned int> k_core_num("kcore", cll::desc("KCore value"),
                                         cll::Required);

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
/* Graph structure declarations + other inits */
/******************************************************************************/

struct NodeData {
  uint32_t current_degree;
  std::atomic<uint32_t> trim;
};

galois::DynamicBitSet bitset_exclude;
galois::DynamicBitSet bitset_active;
galois::DynamicBitSet bitset_trim;

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph, uint32_t>> syncSubstrate;

#include "kcore_sync.hh"

/******************************************************************************/
/* Functors for running the algorithm */
/******************************************************************************/

struct InitializeGraph {
  Graph* graph;

  InitializeGraph(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
    const auto& presentNodes = _graph.presentNodesRangeIn();

    galois::do_all(
        galois::iterate(presentNodes.begin(), presentNodes.end()),
        InitializeGraph{&_graph}, galois::no_stats());
  }

  void operator()(GNode src) const {
    NodeData& sdata      = graph->getData(src);
    sdata.current_degree = std::distance(graph->edge_begin(src), graph->edge_end(src));
    sdata.trim           = 0;
  }
};

struct KCore_trim {
  Graph* graph;

  galois::GAccumulator<uint64_t>& active_vertices;
  galois::GAccumulator<uint64_t>& active_edges;

  KCore_trim(Graph* _graph, galois::GAccumulator<uint64_t>& _active_vertices, galois::GAccumulator<uint64_t>& _active_edges)
      : graph(_graph),
        active_vertices (_active_vertices),
        active_edges (_active_edges) {}

  void static go(Graph& _graph, galois::GAccumulator<uint64_t>& _active_vertices, galois::GAccumulator<uint64_t>& _active_edges) {
    const auto& masterNodes = _graph.masterNodesRangeIn();
    
    galois::do_all(
        galois::iterate(masterNodes),
        KCore_trim{&_graph, _active_vertices, _active_edges}, galois::no_stats());
  }

  void operator()(GNode src) const {
    if (!bitset_exclude.test(src)) {
        NodeData& sdata = graph->getData(src);

        if (bitset_trim.test(src)) {
            sdata.current_degree = sdata.current_degree - sdata.trim;
            sdata.trim = 0;
        }

        if (sdata.current_degree < k_core_num) {
            active_vertices += 1;

            bitset_exclude.set(src);
            bitset_active.set(src);
            
            uint64_t nout = std::distance(graph->out_edge_begin(src), graph->out_edge_end(src));
            active_edges += nout;
        }
    }
  }
};

struct PullRemote {
  Graph* graph;

  galois::runtime::NetworkInterface& net;

  PullRemote(Graph* _graph) : graph(_graph), net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph) {
      const auto& remoteNodes = _graph.remoteNodesRangeIn();

      galois::do_all(galois::iterate(remoteNodes), PullRemote{&_graph},
                     galois::steal(), galois::no_stats());
  }

  void operator()(GNode dst) const {
    uint32_t dtrim = 0;
      
    for (auto current_edge : graph->inEdges(dst)) {
        GNode src = graph->getInEdgeSrc(current_edge);
        if (bitset_active.test(src)) {
            dtrim += 1;
        }
    }
    
    if (dtrim != 0) {
        net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getRemoteLID(dst), dtrim);
    }
  }
};

struct PullMaster {
  Graph* graph;

  PullMaster(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
      const auto& masterNodes = _graph.masterNodesRangeIn();

      galois::do_all(galois::iterate(masterNodes), PullMaster{&_graph},
                     galois::steal(), galois::no_stats());
  }

  void operator()(GNode dst) const {
    if (!bitset_exclude.test(dst)) {
        NodeData& ddata = graph->getData(dst);
      
        bool dirty = false;
        for (auto current_edge : graph->inEdges(dst)) {
            GNode src = graph->getInEdgeSrc(current_edge);
            if (bitset_active.test(src)) {
                galois::addVoid(ddata.trim, (uint32_t)1);
                dirty = true;
            }
        }

        if (dirty) {
            bitset_trim.set(dst);
        }
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
      
      galois::do_all(galois::iterate(masterNodes), Push{&_graph},
                     galois::steal(), galois::no_stats());
  }

  void operator()(GNode src) const {
    if (bitset_active.test(src)) {
        for (auto current_edge : graph->outEdges(src)) {
            GNode dst = graph->getOutEdgeDst(current_edge);
            if (graph->isPhantom(dst)) {
                net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getRemoteLID(dst), (uint32_t)1);
            }
            else {
                if (!bitset_exclude.test(dst)) {
                    auto& ddata = graph->getData(dst);
                    galois::atomicAddVoid(ddata.trim, (uint32_t)1);
                    bitset_trim.set(dst);
                }
            }
        }
    }
  }
};

struct KCore {
  Graph* graph;

  galois::runtime::NetworkInterface& net;

  KCore(Graph* _graph)
      : graph(_graph),
        net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph, Exp exp) {
#ifdef GALOIS_USER_STATS
    constexpr bool USER_STATS = true;
#else
    constexpr bool USER_STATS = false;
#endif

    unsigned _num_iterations   = 0;
  
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
      
      std::string total_str("Total_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_total(total_str.c_str(), REGION_NAME_RUN.c_str());
      std::string trim_str("Trim_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_trim(trim_str.c_str(), REGION_NAME_RUN.c_str());
      std::string active_str("Active_Reduce_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_active(active_str.c_str(), REGION_NAME_RUN.c_str());
      
      StatTimer_total.start();
      bitset_active.reset();
      active_v.reset();
      active_e.reset();

      StatTimer_trim.start();
      KCore_trim::go(_graph, active_v, active_e);
      StatTimer_trim.stop();
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
          if (hybrid) {
              if (local_active_v >= vertex_threshold_high) {
                  pull = true;
              }
              else if (local_active_v <= vertex_threshold_low) {
                  pull = false;
              }
              else {
                  if (local_active_e >= edge_threshold_high) {
                      pull = true;
                  }
                  else if (local_active_e <= edge_threshold_low) {
                      pull = false;
                  }
                  else {
                      if (degree) {
                          if ((local_active_e/local_active_v) >= degree_threshold) {
                              pull = true;
                          }
                          else {
                              pull = false;
                          }
                      }
                  }
              }
          }
          else {
              if (local) {
                  active_edges = local_active_e;
              }
              else {
                  active_edges = global_active_e;
              }

              if (hysteresis) {
                  if (active_edges >= edge_threshold_high) {
                      pull = true;
                  }
                  else if (active_edges <= edge_threshold_low) {
                      pull = false;
                  }
              }
              else {
                  if (active_edges > edge_threshold_low) {
                      pull = true;
                  }
                  else {
                      pull = false;
                  }
              }
          }
      }

      bitset_trim.reset();
      
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
      syncSubstrate->reduce<Reduce_add_trim>(&bitset_trim);
      StatTimer_comm.stop();
      
      _net.resetWorkTermination();
      
      if (!pull) {
          std::string reset_str("Reset_Mirror_Round_" + std::to_string(_num_iterations));
          galois::CondStatTimer<USER_STATS> StatTimer_reset(reset_str.c_str(), REGION_NAME_RUN.c_str());

          StatTimer_reset.start();
          syncSubstrate->reset_mirrorField<Reduce_add_trim>();
          StatTimer_reset.stop();
      }

      _num_iterations++;
      StatTimer_total.stop();
    }
  }
};

/******************************************************************************/
/* Sanity check operators */
/******************************************************************************/

/* Gets the total number of nodes that are still alive */
struct KCoreSanityCheck {
  void static go(Graph& _graph, galois::DGAccumulator<uint64_t>& dga) {
    dga.reset();

    uint64_t local_num_nodes = _graph.numMasters() - bitset_exclude.count();
    dga += local_num_nodes;

    uint64_t global_num_nodes = dga.reduce();

    // Only node 0 will print data
    if (galois::runtime::getSystemNetworkInterface().ID == 0) {
      galois::gPrint("Number of nodes in the ", k_core_num, "-core is ", global_num_nodes, "\n");
    }
  }
};

/******************************************************************************/
/* Make results */
/******************************************************************************/

std::vector<unsigned> makeResults(std::unique_ptr<Graph>& hg) {
  std::vector<unsigned> values;

  values.reserve(hg->numMasters());
  for (auto node : hg->masterNodesRange()) {
      if (bitset_exclude.test(node)) {
          values.push_back(0);
      }
      else {
          values.push_back(1);
      }
  }

  return values;
}

/******************************************************************************/
/* Main method for running */
/******************************************************************************/

constexpr static const char* const name = "Distributed KCore Extraction (Push)";
constexpr static const char* const desc = "Distributed KCore Extraction (Push)";
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
  std::tie(hg, syncSubstrate) = symmetricDistGraphInitialization<NodeData, void, uint32_t>();

  net.allocateBufferPool();
  
  hg->sortEdgesByDestination();
  hg->sortInEdgesBySource();

  galois::runtime::getHostBarrier().wait();
  net.partitionDone();

  bitset_exclude.resize(hg->actualSize());
  bitset_active.resize(hg->numMasters());
  bitset_trim.resize(hg->actualSize());

  galois::runtime::getHostBarrier().wait();
  StatTimer_preprocess.stop();

  galois::DGAccumulator<uint64_t> dga;

  for (int i=0; i<Exp_Count; i++) {
    TYPE_NAME = exp_names[i];

    for (auto run = 0; run < numRuns; ++run) {
        galois::gPrint("[", net.ID, "] KCore (", TYPE_NAME, ") run ", run, " start\n");
      
        bitset_exclude.reset();
        bitset_active.reset();
        bitset_trim.reset();
        InitializeGraph::go(*hg);
        galois::runtime::getHostBarrier().wait();

        REGION_NAME_RUN = REGION_NAME + "_" + TYPE_NAME + "_" + std::to_string(run);
        std::string main_timer_str("Timer_" + std::to_string(run));
        galois::StatTimer StatTimer_main(main_timer_str.c_str(), REGION_NAME_RUN.c_str());

        StatTimer_main.start();
        KCore::go(*hg, static_cast<Exp>(i));
        StatTimer_main.stop();
        galois::gPrint("Host ", net.ID, " KCore (", TYPE_NAME, ") run ", run, " time: ", StatTimer_main.get(), " ms\n");

        KCoreSanityCheck::go(*hg, dga);
        
        (*syncSubstrate).set_num_run(run + 1);
    }
  }

  StatTimer_total.stop();
  
  net.applicationDone();

  if (output) {
    std::vector<unsigned> results = makeResults(hg);
    auto globalIDs                = hg->getMasterGlobalIDs();
    assert(results.size() == globalIDs.size());

    writeOutput(outputLocation, "in_kcore", results.data(), results.size(),
                globalIDs.data());
  }

  return 0;
}
