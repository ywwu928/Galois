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
#include "galois/runtime/Profile.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <vector>

static std::string REGION_NAME = "PageRank";
static std::string REGION_NAME_RUN;
static std::string TYPE_NAME;

enum Exp {
    Pull,
    Push,
    Global_Threshold,
    Local_Threshold,
    Hybrid_Threshold,
    Exp_Count
};

std::string exp_names[] = {
    "Pull",
    "Push",
    "Global_Threshold",
    "Local_Threshold",
    "Hybrid_Threshold"
};

/******************************************************************************/
/* Declaration of command line arguments */
/******************************************************************************/
namespace cll = llvm::cl;

static cll::opt<float> tolerance("tolerance",
                                 cll::desc("tolerance for residual"),
                                 cll::init(0.000001));
static cll::opt<unsigned int>
    maxIterations("maxIterations",
                  cll::desc("Maximum iterations: Default 1000"),
                  cll::init(1000));

static cll::opt<float> edge_bound("edge_bound",
                                   cll::desc("edge density bound for switching"),
                                   cll::init(0.05));

static cll::opt<float> vertex_bound("vertex_bound",
                                   cll::desc("vertex density bound for switching"),
                                   cll::init(0.8));

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

static const float alpha = (1.0 - 0.85);
struct NodeData {
  float value;
  float delta;
  std::atomic<float> residual;
};

galois::DynamicBitSet bitset_residual;
galois::DynamicBitSet bitset_delta;

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;
typedef GNode WorkItem;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph, float>> syncSubstrate;

#include "pagerank_sync.hh"

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/

struct InitializeGraph {
  Graph* graph;

  InitializeGraph(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
    const auto& presentNodes = _graph.presentNodesRangeIn();

    galois::do_all(
        galois::iterate(presentNodes),
        InitializeGraph{&_graph});
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    sdata.value     = 0;
    sdata.delta    = 0;
    if (graph->isMaster(src)) {
        sdata.residual  = alpha;
    }
    else {
        sdata.residual = 0;
    }
  }
};

struct PageRank_delta {
  Graph* graph;

  galois::GAccumulator<uint64_t>& active_vertices;
  galois::GAccumulator<uint64_t>& active_edges;

  PageRank_delta(Graph* _graph, galois::GAccumulator<uint64_t>& _active_vertices, galois::GAccumulator<uint64_t>& _active_edges)
      : graph(_graph),
        active_vertices (_active_vertices),
        active_edges (_active_edges) {}

  void static go(Graph& _graph, galois::GAccumulator<uint64_t>& _active_vertices, galois::GAccumulator<uint64_t>& _active_edges) {
    const auto& masterNodes = _graph.masterNodesRangeIn();

    galois::do_all(
        galois::iterate(masterNodes),
        PageRank_delta{&_graph, _active_vertices, _active_edges});
  }

  void operator()(GNode src) const {
    if (bitset_residual.test(src)) {
      auto& sdata = graph->getData(src);

      sdata.value += sdata.residual;
      if (sdata.residual > tolerance) {
        active_vertices += 1;
        uint64_t nout = std::distance(graph->edge_begin(src), graph->edge_end(src));
        if (nout > 0) {
          sdata.delta = sdata.residual * (1 - alpha) / nout;
          bitset_delta.set(src);
          active_edges += nout;
        }
      }
      sdata.residual = 0;
    }
  }
};

struct PullRemote {
  Graph* graph;

  galois::runtime::NetworkInterface& net;

  PullRemote(Graph* _graph) : graph(_graph), net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph) {
      const auto& remoteNodes = _graph.remoteNodesRangeIn();

      galois::do_all(
          galois::iterate(remoteNodes), PullRemote{&_graph},
          galois::steal());
  }

  void operator()(GNode dst) const {
    float dresidual = 0;
    
    for (auto nbr : graph->inEdges(dst)) {
        GNode src   = graph->getInEdgeSrc(nbr);

        if (bitset_delta.test(src)) {
            auto& sdata = graph->getData(src);
            dresidual = dresidual + sdata.delta;
        }
    }

    if (dresidual != 0) {
        net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getRemoteLID(dst), dresidual);
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
          galois::steal());
  }

  void operator()(GNode dst) const {
    auto& ddata = graph->getData(dst);

    bool dirty = false;
    for (auto nbr : graph->inEdges(dst)) {
        GNode src   = graph->getInEdgeSrc(nbr);

        if (bitset_delta.test(src)) {
            auto& sdata = graph->getData(src);
            galois::addVoid(ddata.residual, sdata.delta);
            dirty = true;
        }
    }

    if (dirty) {
        bitset_residual.set(dst);
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
                     galois::steal());
  }

  void operator()(WorkItem src) const {
    if (bitset_delta.test(src)) {
        NodeData& sdata = graph->getData(src);

        for (auto nbr : graph->outEdges(src)) {
            GNode dst       = graph->getOutEdgeDst(nbr);
            if (graph->isPhantom(dst)) {
                net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getRemoteLID(dst), sdata.delta);
            }
            else {
                NodeData& ddata = graph->getData(dst);
                galois::atomicAddVoid(ddata.residual, sdata.delta);
                bitset_residual.set(dst);
            }
        }
    }
  }
};

struct PageRank {
  Graph* graph;
  
  galois::runtime::NetworkInterface& net;

  PageRank(Graph* _graph)
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
    bool hybrid = false;

    switch (exp) {
        case Pull: {
            pull = true;
            break;
        }
        case Push: {
            pull = false;
            break;
        }
        case Global_Threshold: {
            dual = true;
            break;
        }
        case Local_Threshold: {
            dual = true;
            local = true;
            break;
        }
        case Hybrid_Threshold: {
            dual = true;
            local = true;
            hybrid = true;
            break;
        }
        case Exp_Count: {
            galois::gPrint("Error: Unknown experiment!\n");
            return;
        }
    }

    uint64_t active_edges;
    uint64_t edge_threshold;
    if (local) {
        edge_threshold = _graph.sizeEdges() * edge_bound;
    }
    else {
        edge_threshold = _graph.globalSizeEdges() * edge_bound;
    }
    uint64_t vertex_threshold = _graph.numMasters() * vertex_bound;

    bitset_residual.set_all();
    PageRank_delta::go(_graph, active_v, active_e);
    local_active_v = active_v.reduce();
    local_active_e = active_e.reduce();

    do {
#ifdef GALOIS_PRINT_PROCESS
      galois::gPrint("Host ", _net.ID, " : iteration ", _num_iterations, "\n");
#endif

      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "Active_Vertices_Round_" + std::to_string(_num_iterations), local_active_v);
      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "Active_Edges_Round_" + std::to_string(_num_iterations), local_active_e);
      
      std::string total_str("Total_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_total(total_str.c_str(), REGION_NAME_RUN.c_str());
      std::string compute_str("Compute_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_compute(compute_str.c_str(), REGION_NAME_RUN.c_str());
      std::string comm_str("Communication_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_comm(comm_str.c_str(), REGION_NAME_RUN.c_str());
      std::string delta_str("Delta_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_delta(delta_str.c_str(), REGION_NAME_RUN.c_str());
      std::string active_str("Active_Reduce_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_active(active_str.c_str(), REGION_NAME_RUN.c_str());

      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "Active_Vertices_Round_" + std::to_string(_num_iterations), local_active_v);
      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "Active_Edges_Round_" + std::to_string(_num_iterations), local_active_e);

      StatTimer_total.start();
      if (dual) {
          active_edges = local ? local_active_e : global_active_e;

          pull = active_edges > edge_threshold;

          if (hybrid && local_active_v >= vertex_threshold) {
              pull = true;
          }
      }
      
      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "Pull_Round_" + std::to_string(_num_iterations), pull);

      bitset_residual.reset();
      
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
      syncSubstrate->reduce<Reduce_add_residual>(&bitset_residual);
      StatTimer_comm.stop();
      
      _net.resetWorkTermination();
      
      if (!pull) {
          std::string reset_str("Reset_Mirror_Round_" + std::to_string(_num_iterations));
          galois::CondStatTimer<USER_STATS> StatTimer_reset(reset_str.c_str(), REGION_NAME_RUN.c_str());

          StatTimer_reset.start();
          syncSubstrate->reset_mirrorField<Reduce_add_residual>();
          StatTimer_reset.stop();
      }
      
      bitset_delta.reset();
      active_v.reset();
      active_e.reset();

      StatTimer_delta.start();
      PageRank_delta::go(_graph, active_v, active_e);
      StatTimer_delta.stop();
      local_active_v = active_v.reduce();
      local_active_e = active_e.reduce();
      
      StatTimer_active.start();
      global_active_e = 0;
      MPI_Allreduce(&local_active_e, &global_active_e, 1,
                    MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
      StatTimer_active.stop();
      
      StatTimer_total.stop();

      ++_num_iterations;
    } while (global_active_e && (_num_iterations < maxIterations));
  }
};

/******************************************************************************/
/* Sanity check operators */
/******************************************************************************/

// Gets various values from the pageranks values/residuals of the graph
struct PageRankSanity {
  Graph* graph;

  galois::DGAccumulator<float>& DGAccumulator_sum;
  galois::DGAccumulator<float>& DGAccumulator_sum_residual;
  galois::DGAccumulator<uint64_t>& DGAccumulator_residual_over_tolerance;

  galois::DGReduceMax<float>& max_value;
  galois::DGReduceMin<float>& min_value;
  galois::DGReduceMax<float>& max_residual;
  galois::DGReduceMin<float>& min_residual;

  PageRankSanity(
      Graph* _graph,
      galois::DGAccumulator<float>& _DGAccumulator_sum,
      galois::DGAccumulator<float>& _DGAccumulator_sum_residual,
      galois::DGAccumulator<uint64_t>& _DGAccumulator_residual_over_tolerance,
      galois::DGReduceMax<float>& _max_value,
      galois::DGReduceMin<float>& _min_value,
      galois::DGReduceMax<float>& _max_residual,
      galois::DGReduceMin<float>& _min_residual)
      : graph(_graph),
        DGAccumulator_sum(_DGAccumulator_sum),
        DGAccumulator_sum_residual(_DGAccumulator_sum_residual),
        DGAccumulator_residual_over_tolerance(
            _DGAccumulator_residual_over_tolerance),
        max_value(_max_value), min_value(_min_value),
        max_residual(_max_residual), min_residual(_min_residual) {}

  void static go(Graph& _graph, galois::DGAccumulator<float>& DGA_sum,
                 galois::DGAccumulator<float>& DGA_sum_residual,
                 galois::DGAccumulator<uint64_t>& DGA_residual_over_tolerance,
                 galois::DGReduceMax<float>& max_value,
                 galois::DGReduceMin<float>& min_value,
                 galois::DGReduceMax<float>& max_residual,
                 galois::DGReduceMin<float>& min_residual) {
    DGA_sum.reset();
    DGA_sum_residual.reset();
    max_value.reset();
    max_residual.reset();
    min_value.reset();
    min_residual.reset();
    DGA_residual_over_tolerance.reset();

    galois::do_all(galois::iterate(_graph.masterNodesRange()),
                   PageRankSanity(&_graph, DGA_sum,
                                  DGA_sum_residual,
                                  DGA_residual_over_tolerance, max_value,
                                  min_value, max_residual, min_residual));

    float max_rank          = max_value.reduce();
    float min_rank          = min_value.reduce();
    float rank_sum          = DGA_sum.reduce();
    float residual_sum      = DGA_sum_residual.reduce();
    uint64_t over_tolerance = DGA_residual_over_tolerance.reduce();
    float max_res           = max_residual.reduce();
    float min_res           = min_residual.reduce();

    // Only node 0 will print data
    if (galois::runtime::getSystemNetworkInterface().ID == 0) {
      galois::gPrint("Max rank is ", max_rank, "\n");
      galois::gPrint("Min rank is ", min_rank, "\n");
      galois::gPrint("Rank sum is ", rank_sum, "\n");
      galois::gPrint("Residual sum is ", residual_sum, "\n");
      galois::gPrint("# nodes with residual over ", tolerance,
                     " (tolerance) is ", over_tolerance, "\n");
      galois::gPrint("Max residual is ", max_res, "\n");
      galois::gPrint("Min residual is ", min_res, "\n");
    }
  }

  /* Gets the max, min rank from all owned nodes and
   * also the sum of ranks */
  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);

    max_value.update(sdata.value);
    min_value.update(sdata.value);
    max_residual.update(sdata.residual);
    min_residual.update(sdata.residual);

    DGAccumulator_sum += sdata.value;
    DGAccumulator_sum_residual += sdata.residual;

    if (sdata.residual > tolerance) {
      DGAccumulator_residual_over_tolerance += 1;
    }
  }
};

/******************************************************************************/
/* Make results */
/******************************************************************************/

std::vector<float> makeResults(std::unique_ptr<Graph>& hg) {
  std::vector<float> values;

  values.reserve(hg->numMasters());
  for (auto node : hg->masterNodesRange()) {
    values.push_back(hg->getData(node).value);
  }

  return values;
}

/******************************************************************************/
/* Main */
/******************************************************************************/

constexpr static const char* const name = "Distributed Pagerank (Push)";
constexpr static const char* const desc = "Distributed Pagerank (Push)";
constexpr static const char* const url = 0;

int main(int argc, char** argv) {
  galois::DistMemSys G;
  DistBenchStart(argc, argv, name, desc, url);

  auto& net = galois::runtime::getSystemNetworkInterface();

  if (net.ID == 0) {
    galois::runtime::reportParam(REGION_NAME.c_str(), "Max Iterations", maxIterations);
    std::ostringstream ss;
    ss << tolerance;
    galois::runtime::reportParam(REGION_NAME.c_str(), "Tolerance", ss.str());
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
  std::tie(hg, syncSubstrate) = distGraphInitialization<NodeData, void, float>();

  net.allocateBufferPool();
  
  hg->sortEdgesByDestination();
  hg->sortInEdgesBySource();

  galois::runtime::getHostBarrier().wait();
  net.partitionDone();

  bitset_residual.resize(hg->actualSize());
  bitset_delta.resize(hg->numMasters());

  galois::runtime::getHostBarrier().wait();
  StatTimer_preprocess.stop();

  galois::DGAccumulator<float> DGA_sum;
  galois::DGAccumulator<float> DGA_sum_residual;
  galois::DGAccumulator<uint64_t> DGA_residual_over_tolerance;
  galois::DGReduceMax<float> max_value;
  galois::DGReduceMin<float> min_value;
  galois::DGReduceMax<float> max_residual;
  galois::DGReduceMin<float> min_residual;

  for (int i=0; i<Exp_Count; i++) {
    TYPE_NAME = exp_names[i];

    for (auto run = 0; run < numRuns; ++run) {
        galois::gPrint("[", net.ID, "] PageRank (", TYPE_NAME, ") run ", run, " start\n");
        
        bitset_residual.reset();
        bitset_delta.reset();
        InitializeGraph::go(*hg);
        galois::runtime::getHostBarrier().wait();

        REGION_NAME_RUN = REGION_NAME + "_" + TYPE_NAME + "_" + std::to_string(run);
        std::string main_timer_str("Timer_" + std::to_string(run));
        galois::StatTimer StatTimer_main(main_timer_str.c_str(), REGION_NAME_RUN.c_str());

        StatTimer_main.start();
        PageRank::go(*hg, static_cast<Exp>(i));
        StatTimer_main.stop();
        galois::gPrint("Host ", net.ID, " PageRank (", TYPE_NAME, ") run ", run, " time: ", StatTimer_main.get(), " ms\n");


        PageRankSanity::go(*hg, DGA_sum, DGA_sum_residual,
                           DGA_residual_over_tolerance, max_value, min_value,
                           max_residual, min_residual);
    }
  }

  StatTimer_total.stop();
  
  net.applicationDone();

  if (output) {
    std::vector<float> results = makeResults(hg);
    auto globalIDs             = hg->getMasterGlobalIDs();
    assert(results.size() == globalIDs.size());

    writeOutput(outputLocation, "pagerank", results.data(), results.size(),
                globalIDs.data());
  }

  return 0;
}
