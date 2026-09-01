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
static std::string WORKLIST_TYPE;

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
    const auto& presentNodes = _graph.presentNodesRange();

    galois::do_all(
        galois::iterate(presentNodes),
        InitializeGraph{&_graph}, galois::no_stats());
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

struct PageRank_delta_edge {
  Graph* graph;

  galois::GAccumulator<unsigned int>& active_edges;

  PageRank_delta_edge(Graph* _graph, galois::GAccumulator<unsigned int>& _active_edges)
      : graph(_graph),
        active_edges (_active_edges) {}

  void static go(Graph& _graph, galois::GAccumulator<unsigned int>& _active_edges) {
    const auto& masterNodes = _graph.masterNodesRange();

    galois::do_all(
        galois::iterate(masterNodes),
        PageRank_delta_edge{&_graph, _active_edges}, galois::no_stats());
  }

  void operator()(GNode src) const {
    if (bitset_residual.test(src)) {
      auto& sdata = graph->getData(src);

      sdata.value += sdata.residual;
      if (sdata.residual > tolerance) {
        uint32_t nout = std::distance(graph->edge_begin(src), graph->edge_end(src));
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

struct PageRank_delta_vertex {
  Graph* graph;

  PageRank_delta_vertex(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
    const auto& masterNodes = _graph.masterNodesRange();

    galois::do_all(
        galois::iterate(masterNodes),
        PageRank_delta_vertex{&_graph}, galois::no_stats());
  }

  void operator()(GNode src) const {
    if (bitset_residual.test(src)) {
      auto& sdata = graph->getData(src);

      sdata.value += sdata.residual;
      if (sdata.residual > tolerance) {
        uint32_t nout = std::distance(graph->edge_begin(src), graph->edge_end(src));
        if (nout > 0) {
          sdata.delta = sdata.residual * (1 - alpha) / nout;
          bitset_delta.set(src);
        }
      }
      sdata.residual = 0;
    }
  }
};

struct PageRank {
  Graph* graph;
  
  galois::runtime::NetworkInterface& net;

  PageRank(Graph* _graph)
      : graph(_graph),
        net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph, bool edge_worklist) {
#ifdef GALOIS_USER_STATS
    constexpr bool USER_STATS = true;
#else
    constexpr bool USER_STATS = false;
#endif

    unsigned _num_iterations   = 0;

    const auto& masterNodes = _graph.masterNodesRange();
  
    auto& _net = galois::runtime::getSystemNetworkInterface();

    galois::GAccumulator<unsigned int> active_count;
    uint64_t local_active_count, global_active_count;

    bitset_residual.set_all();

    do {
      std::string total_str(WORKLIST_TYPE + "_Total_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_total(total_str.c_str(), REGION_NAME_RUN.c_str());
      std::string delta_str(WORKLIST_TYPE + "_Delta_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_delta(delta_str.c_str(), REGION_NAME_RUN.c_str());
      std::string compute_str(WORKLIST_TYPE + "_Compute_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_compute(compute_str.c_str(), REGION_NAME_RUN.c_str());
      std::string comm_str(WORKLIST_TYPE + "_Communication_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_comm(comm_str.c_str(), REGION_NAME_RUN.c_str());
      std::string reset_str(WORKLIST_TYPE + "_Reset_Mirror_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_reset(reset_str.c_str(), REGION_NAME_RUN.c_str());
      std::string active_str(WORKLIST_TYPE + "_Active_Reduce_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_active(active_str.c_str(), REGION_NAME_RUN.c_str());

#ifdef GALOIS_PRINT_PROCESS
      galois::gPrint("Host ", _net.ID, " : iteration ", _num_iterations, "\n");
#endif

      syncSubstrate->set_num_round(_num_iterations);

      StatTimer_total.start();
      bitset_delta.reset();

      StatTimer_delta.start();
      if (edge_worklist) {
          active_count.reset();
          PageRank_delta_edge::go(_graph, active_count);
          local_active_count = active_count.reduce();
      }
      else {
          PageRank_delta_vertex::go(_graph);
          local_active_count = bitset_delta.count();
      }
      StatTimer_delta.stop();

      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), WORKLIST_TYPE + "_NumWorkItems_Round_" + std::to_string(_num_iterations), local_active_count);

      bitset_residual.reset();

      _net.prefetchBuffers();

      StatTimer_compute.start();
      galois::do_all(galois::iterate(masterNodes), PageRank{&_graph},
                     galois::no_stats(), galois::steal());
      _net.flushRemoteWork();
      StatTimer_compute.stop();

      StatTimer_comm.start();
      syncSubstrate->reduce<Reduce_add_residual, Bitset_residual>();
      StatTimer_comm.stop();
      
      _net.resetWorkTermination();
      
      StatTimer_reset.start();
      syncSubstrate->reset_mirrorField<Reduce_add_residual>();
      StatTimer_reset.stop();

      ++_num_iterations;
      
      StatTimer_active.start();
      global_active_count = 0;
      MPI_Allreduce(&local_active_count, &global_active_count, 1,
                    MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);
      StatTimer_active.stop();
      
      StatTimer_total.stop();
    } while ((_num_iterations < maxIterations) && global_active_count);
  }

  void operator()(WorkItem src) const {
    if (bitset_delta.test(src)) {
        NodeData& sdata = graph->getData(src);

        for (auto nbr : graph->outEdges(src)) {
            GNode dst       = graph->getOutEdgeDst(nbr);
#ifndef GALOIS_FULL_MIRRORING
            if (graph->isPhantom(dst)) {
                net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getRemoteLID(dst), sdata.delta);
            }
            else {
#endif
                NodeData& ddata = graph->getData(dst);
                galois::atomicAddVoid(ddata.residual, sdata.delta);
                bitset_residual.set(dst);
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
                                  min_value, max_residual, min_residual),
                   galois::no_stats());

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

  galois::runtime::getHostBarrier().wait();
  net.partitionDone();

  bitset_residual.resize(hg->actualSize());
  bitset_delta.resize(hg->numMasters());

  galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");

  InitializeGraph::go((*hg));
  galois::runtime::getHostBarrier().wait();
  StatTimer_preprocess.stop();

  galois::DGAccumulator<float> DGA_sum;
  galois::DGAccumulator<float> DGA_sum_residual;
  galois::DGAccumulator<uint64_t> DGA_residual_over_tolerance;
  galois::DGReduceMax<float> max_value;
  galois::DGReduceMin<float> min_value;
  galois::DGReduceMax<float> max_residual;
  galois::DGReduceMin<float> min_residual;

  for (auto run = 0; run < numRuns; ++run) {
    REGION_NAME_RUN = REGION_NAME + "_" + std::to_string(run);
    galois::gPrint("[", net.ID, "] PageRank::go run ", run, " called\n");

    // edge worklist
    WORKLIST_TYPE = "Edge";
    std::string edge_timer_str("Edge_Timer_" + std::to_string(run));
    galois::StatTimer StatTimer_edge_main(edge_timer_str.c_str(), REGION_NAME_RUN.c_str());

    net.touchBufferPool();
    galois::runtime::getHostBarrier().wait();

    StatTimer_edge_main.start();
    PageRank::go(*hg, true);
    StatTimer_edge_main.stop();
    galois::gPrint("Host ", net.ID, " PageRank run ", run, " (edge) time: ", StatTimer_edge_main.get(), " ms\n");

    PageRankSanity::go(*hg, DGA_sum, DGA_sum_residual,
                       DGA_residual_over_tolerance, max_value, min_value,
                       max_residual, min_residual);
    
    bitset_residual.reset();
    bitset_delta.reset();

    InitializeGraph::go(*hg);

    // vertex worklist
    WORKLIST_TYPE = "Vertex";
    std::string vertex_timer_str("Vertex_Timer_" + std::to_string(run));
    galois::StatTimer StatTimer_vertex_main(vertex_timer_str.c_str(), REGION_NAME_RUN.c_str());

    net.touchBufferPool();
    galois::runtime::getHostBarrier().wait();

    StatTimer_vertex_main.start();
    PageRank::go(*hg, false);
    StatTimer_vertex_main.stop();
    galois::gPrint("Host ", net.ID, " PageRank run ", run, " (vertex) time: ", StatTimer_vertex_main.get(), " ms\n");

    PageRankSanity::go(*hg, DGA_sum, DGA_sum_residual,
                       DGA_residual_over_tolerance, max_value, min_value,
                       max_residual, min_residual);

    if ((run + 1) != numRuns) {
      bitset_residual.reset();
      bitset_delta.reset();

      (*syncSubstrate).set_num_run(run + 1);
      InitializeGraph::go(*hg);
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
