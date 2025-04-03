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
#include "galois/DTerminationDetector.h"
#include "galois/runtime/Tracer.h"
#include "galois/runtime/Profile.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <vector>

static std::string REGION_NAME = "PageRank";
static std::string REGION_NAME_RUN;

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
  std::atomic<uint32_t> nout;
  float delta;
  std::atomic<float> residual;
};

galois::DynamicBitSet bitset_residual;

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;
typedef GNode WorkItem;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph, float>> syncSubstrate;

#include "pagerank_push_sync.hh"

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/

// Reset all fields of all nodes to 0
struct ResetGraph {
  Graph* graph;

  ResetGraph(Graph* _graph) : graph(_graph) {}
  void static go(Graph& _graph) {
    const auto& presentNodes = _graph.presentNodesRange();
    galois::do_all(
        galois::iterate(presentNodes.begin(), presentNodes.end()),
        ResetGraph{&_graph}, galois::no_stats());
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    sdata.value     = 0;
    sdata.nout      = 0;
    sdata.residual  = 0;
    sdata.delta     = 0;
  }
};

// Initialize residual at nodes with outgoing edges + find nout for
// nodes with outgoing edges
struct InitializeGraph {
  const float& local_alpha;
  Graph* graph;

  InitializeGraph(const float& _alpha, Graph* _graph)
      : local_alpha(_alpha), graph(_graph) {}

  void static go(Graph& _graph) {
    // first initialize all fields to 0 via ResetGraph (can't assume all zero
    // at start)
    ResetGraph::go(_graph);

    const auto& presentNodes = _graph.presentNodesRange();

    // regular do all without stealing; just initialization of nodes with
    // outgoing edges
    galois::do_all(
        galois::iterate(presentNodes.begin(), presentNodes.end()),
        InitializeGraph{alpha, &_graph}, galois::steal(), galois::no_stats());
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    sdata.residual  = local_alpha;
    uint32_t num_edges =
        std::distance(graph->edge_begin(src), graph->edge_end(src));
    galois::atomicAdd(sdata.nout, num_edges);
  }
};

struct PageRank_delta {
  const float& local_alpha;
  cll::opt<float>& local_tolerance;
  Graph* graph;

  PageRank_delta(const float& _local_alpha, cll::opt<float>& _local_tolerance,
                 Graph* _graph)
      : local_alpha(_local_alpha), local_tolerance(_local_tolerance),
        graph(_graph) {}

  void static go(Graph& _graph) {
    const auto& masterNodes = _graph.masterNodesRange();

    galois::do_all(
        galois::iterate(masterNodes),
        PageRank_delta{alpha, tolerance, &_graph}, galois::no_stats());
  }

  void operator()(WorkItem src) const {
    NodeData& sdata = graph->getData(src);

    if (sdata.residual > 0) {
      float residual_old = sdata.residual;
      sdata.residual     = 0;
      sdata.value += residual_old;
      if (residual_old > this->local_tolerance) {
        if (sdata.nout > 0) {
          sdata.delta = residual_old * (1 - local_alpha) / sdata.nout;
        }
      }
    }
  }
};

struct PageRank {
  Graph* graph;
  using DGTerminatorDetector = galois::DGAccumulator<unsigned int>;

  DGTerminatorDetector& active_vertices;

  galois::runtime::NetworkInterface& net;

  PageRank(Graph* _g, DGTerminatorDetector& _dga)
      : graph(_g), active_vertices(_dga), net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph) {
    unsigned _num_iterations   = 0;

    const auto& masterNodes = _graph.masterNodesRange();

    DGTerminatorDetector dga;
  
    auto& _net = galois::runtime::getSystemNetworkInterface();

    do {
#ifdef GALOIS_PRINT_PROCESS
      galois::gPrint("Host ", _net.ID, " : iteration ", _num_iterations, "\n");
#endif

      syncSubstrate->set_num_round(_num_iterations);
      dga.reset();

      // reset residual on mirrors
      syncSubstrate->reset_mirrorField<Reduce_add_residual>();
      
      PageRank_delta::go(_graph);

      // launch all other threads to compute
      galois::do_all(galois::iterate(masterNodes), PageRank{&_graph, dga},
                     galois::no_stats(), galois::steal(),
                     galois::loopname(syncSubstrate->get_run_identifier("PageRank").c_str()));

#ifndef GALOIS_FULL_MIRRORING     
      // inform all other hosts that this host has finished sending messages
      // force all messages to be processed before continuing
       _net.flushRemoteWork();
       _net.broadcastWorkTermination();
#endif

       dga.ireduce_send();

#ifdef GALOIS_NO_MIRRORING     
      syncSubstrate->poll_for_remote_work<Reduce_add_residual>();
#else
      syncSubstrate->sync<writeDestination, readSource, Reduce_add_residual, Bitset_residual>("PageRank");
#endif
      
      _net.resetWorkTermination();

      ++_num_iterations;
    } while((_num_iterations < maxIterations) && dga.ireduce_wait());
  }

  void operator()(WorkItem src) const {
    NodeData& sdata = graph->getData(src);
    if (sdata.delta > 0) {
      float _delta = sdata.delta;
      sdata.delta  = 0;

      active_vertices += 1; // this should be moved to Pagerank_delta operator

      for (auto nbr : graph->edges(src)) {
        GNode dst       = graph->getEdgeDst(nbr);
#ifndef GALOIS_FULL_MIRRORING     
        if (graph->isPhantom(dst)) {
            //uint32_t& hostID = graph->getHostIDForLocal(dst);
            //uint32_t& remoteLID = graph->getPhantomRemoteLID(dst);
            //unsigned tid = galois::substrate::ThreadPool::getTID();
            net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getPhantomRemoteLID(dst), _delta);
        }
        else {
#endif
            NodeData& ddata = graph->getData(dst);

            galois::atomicAddVoid(ddata.residual, _delta);

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
  cll::opt<float>& local_tolerance;
  Graph* graph;

  galois::DGAccumulator<float>& DGAccumulator_sum;
  galois::DGAccumulator<float>& DGAccumulator_sum_residual;
  galois::DGAccumulator<uint64_t>& DGAccumulator_residual_over_tolerance;

  galois::DGReduceMax<float>& max_value;
  galois::DGReduceMin<float>& min_value;
  galois::DGReduceMax<float>& max_residual;
  galois::DGReduceMin<float>& min_residual;

  PageRankSanity(
      cll::opt<float>& _local_tolerance, Graph* _graph,
      galois::DGAccumulator<float>& _DGAccumulator_sum,
      galois::DGAccumulator<float>& _DGAccumulator_sum_residual,
      galois::DGAccumulator<uint64_t>& _DGAccumulator_residual_over_tolerance,
      galois::DGReduceMax<float>& _max_value,
      galois::DGReduceMin<float>& _min_value,
      galois::DGReduceMax<float>& _max_residual,
      galois::DGReduceMin<float>& _min_residual)
      : local_tolerance(_local_tolerance), graph(_graph),
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

    galois::do_all(galois::iterate(_graph.masterNodesRange().begin(),
                                   _graph.masterNodesRange().end()),
                   PageRankSanity(tolerance, &_graph, DGA_sum,
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

    if (sdata.residual > local_tolerance) {
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

constexpr static const char* const name = "PageRank - Compiler Generated "
                                          "Distributed Heterogeneous";
constexpr static const char* const desc = "Residual PageRank on Distributed "
                                          "Galois.";
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
  
  galois::StatTimer StatTimer_total("TimerTotal", REGION_NAME.c_str());
  StatTimer_total.start();
  galois::StatTimer StatTimer_preprocess("TimerPreProcess", REGION_NAME.c_str());
  StatTimer_preprocess.start();

  std::unique_ptr<Graph> hg;
  std::tie(hg, syncSubstrate) = distGraphInitialization<NodeData, void, float>();

  hg->sortEdgesByDestination();

  bitset_residual.resize(hg->size());

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
    std::string timer_str("Timer_" + std::to_string(run));
    galois::StatTimer StatTimer_main(timer_str.c_str(), REGION_NAME_RUN.c_str());

    net.touchBufferPool();

    StatTimer_main.start();
    PageRank::go(*hg);
    StatTimer_main.stop();
    galois::gPrint("Host ", net.ID, " PageRank run ", run, " time: ", StatTimer_main.get(), " ms\n");

    // sanity check
    PageRankSanity::go(*hg, DGA_sum, DGA_sum_residual,
                       DGA_residual_over_tolerance, max_value, min_value,
                       max_residual, min_residual);

    if ((run + 1) != numRuns) {
      bitset_residual.reset();

      (*syncSubstrate).set_num_run(run + 1);
      galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");
      InitializeGraph::go(*hg);
      galois::runtime::getHostBarrier().wait();
    }
  }

  StatTimer_total.stop();

  if (output) {
    std::vector<float> results = makeResults(hg);
    auto globalIDs             = hg->getMasterGlobalIDs();
    assert(results.size() == globalIDs.size());

    writeOutput(outputLocation, "pagerank", results.data(), results.size(),
                globalIDs.data());
  }

  return 0;
}
