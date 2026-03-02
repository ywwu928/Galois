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

#include <algorithm>
#include <iostream>
#include <limits>
#include <vector>
#include <variant>

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

enum IterMode { All, Separate };

static cll::opt<IterMode> iterMode(
    "iterMode", cll::desc("Iterate Mode (default value Separate):"),
    cll::values(clEnumVal(All, "iterate through all nodes"),
                clEnumVal(Separate, "iterate through present nodes first and then phantom nodes")),
    cll::init(Separate));

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

static const float alpha = (1.0 - 0.85);
struct NodeData {
  float value;
  std::atomic<uint32_t> nout;
  std::atomic<float> residual;
  float delta;
};

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph, float>> syncSubstrate;

#include "pagerank_pull_sync.hh"

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/

/* (Re)initialize all fields to 0 except for residual which needs to be 0.15
 * everywhere */
struct ResetGraph {
  const float& local_alpha;
  Graph* graph;

  ResetGraph(const float& _local_alpha, Graph* _graph)
      : local_alpha(_local_alpha), graph(_graph) {}

  void static go(Graph& _graph) {
    const auto& presentNodes = _graph.presentNodesRange();
    galois::do_all(
        galois::iterate(presentNodes.begin(), presentNodes.end()),
        ResetGraph{alpha, &_graph}, galois::no_stats());
  }

  void operator()(GNode src) const {
    auto& sdata    = graph->getData(src);
    sdata.value    = 0;
    sdata.nout     = 0;
    sdata.delta    = 0;
    sdata.residual = local_alpha;
  }
};

struct InitializeGraph {
  Graph* graph;

  InitializeGraph(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
    // init graph
    ResetGraph::go(_graph);

    const auto& presentNodes = _graph.presentNodesRange();

    // doing a local do all because we are looping over edges
    galois::do_all(
        galois::iterate(presentNodes), InitializeGraph{&_graph},
        galois::steal(), galois::no_stats());
  }

  // Calculate "outgoing" edges for destination nodes (note we are using
  // the tranpose graph for pull algorithms)
  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    uint32_t num_edges =
        std::distance(graph->out_edge_begin(src), graph->out_edge_end(src));
    galois::atomicAdd(sdata.nout, num_edges);
  }
};

struct PageRank_delta {
  const float& local_alpha;
  cll::opt<float>& local_tolerance;
  Graph* graph;

  using DGTerminatorDetector = galois::DGAccumulator<unsigned int>;

  DGTerminatorDetector& active_vertices;

  PageRank_delta(const float& _local_alpha, cll::opt<float>& _local_tolerance,
                 Graph* _graph, DGTerminatorDetector& _dga)
      : local_alpha(_local_alpha), local_tolerance(_local_tolerance),
        graph(_graph), active_vertices(_dga) {}

  void static go(Graph& _graph, DGTerminatorDetector& dga) {
    const auto& presentNodes = _graph.presentNodesRange();
    galois::do_all(
        galois::iterate(presentNodes.begin(), presentNodes.end()),
        PageRank_delta{alpha, tolerance, &_graph, dga}, galois::no_stats());
  }

  void operator()(GNode src) const {
    auto& sdata = graph->getData(src);
    sdata.delta = 0;

    if (sdata.residual > 0) {
      sdata.value += sdata.residual;
      if (sdata.residual > this->local_tolerance) {
        if (sdata.nout > 0) {
          sdata.delta = sdata.residual * (1 - local_alpha) / sdata.nout;
          active_vertices += 1;
        }
      }
      sdata.residual = 0;
    }
  }
};

struct PageRankPresent {
  Graph* graph;

  PageRankPresent(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
      const auto& presentNodes = _graph.presentNodesRange();
      // launch all other threads to compute
      galois::do_all(
          galois::iterate(presentNodes), PageRankPresent{&_graph},
          galois::steal(), galois::no_stats());
  }

  // Pull deltas from neighbor nodes, then add to self-residual
  void operator()(GNode dst) const {
    // destination node must be master or mirror
    auto& ddata = graph->getData(dst);

    for (auto nbr : graph->inEdges(dst)) {
        GNode src   = graph->getInEdgeSrc(nbr);
        // source node must be masters
        auto& sdata = graph->getData(src);

        if (sdata.delta > 0) {
            galois::add(ddata.residual, sdata.delta);
        }
    }
  }
};

struct PageRankPhantom {
  Graph* graph;

  galois::runtime::NetworkInterface& net;

  PageRankPhantom(Graph* _graph) : graph(_graph), net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph) {
      const auto& phantomNodes = _graph.phantomNodesRange();

      // launch all other threads to compute
      galois::do_all(
          galois::iterate(phantomNodes), PageRankPhantom{&_graph},
          galois::steal(), galois::no_stats());
  }

  // Pull deltas from neighbor nodes, then add to self-residual
  void operator()(GNode dst) const {
    // destination node must be phantom
    // create register for phantom node data
    float dresidual = 0;
    
    for (auto nbr : graph->inEdges(dst)) {
        GNode src   = graph->getInEdgeSrc(nbr);
        // source node must be masters
        auto& sdata = graph->getData(src);

        if (sdata.delta > 0) {
            dresidual = dresidual + sdata.delta;
        }
    }

    if (dresidual != 0) {
        net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getPhantomRemoteLID(dst), dresidual);
    }
  }
};

struct PageRankSep {
  Graph* graph;

  using DGTerminatorDetector = galois::DGAccumulator<unsigned int>;

  PageRankSep(Graph* _graph) : graph(_graph) {}

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
      std::string delta_str("Delta_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_delta(delta_str.c_str(), REGION_NAME_RUN.c_str());
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

      galois::gPrint("Host ", _net.ID, " : point 1\n");
      
      StatTimer_delta.start();
      PageRank_delta::go(_graph, dga);
      StatTimer_delta.stop();
      galois::gPrint("Host ", _net.ID, " : point 2\n");

      _net.prefetchBuffers();
      galois::gPrint("Host ", _net.ID, " : point 3\n");

      StatTimer_compute.start();
      PageRankPresent::go(_graph);
      galois::gPrint("Host ", _net.ID, " : point 4\n");
      PageRankPhantom::go(_graph);
      galois::gPrint("Host ", _net.ID, " : point 5\n");
      StatTimer_compute.stop();

      // inform all other hosts that this host has finished sending messages
      // force all messages to be processed before continuing
      StatTimer_flush.start();
      _net.flushRemoteWork();
      StatTimer_flush.stop();
      galois::gPrint("Host ", _net.ID, " : point 6\n");

      StatTimer_comm.start();
      syncSubstrate->poll_for_remote_work<Reduce_add_residual>();
      StatTimer_comm.stop();
      galois::gPrint("Host ", _net.ID, " : point 7\n");
      
      _net.resetWorkTermination();
      galois::gPrint("Host ", _net.ID, " : point 8\n");

      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "NumWorkItems_Round_" + std::to_string(_num_iterations), (unsigned long)_graph.sizeEdges());

      ++_num_iterations;

      StatTimer_total.stop();
    } while ((_num_iterations < maxIterations) && dga.reduce(syncSubstrate->get_run_identifier()));
  }
};

struct PageRankAll {
  Graph* graph;
  using DGTerminatorDetector = galois::DGAccumulator<unsigned int>;
  
  galois::runtime::NetworkInterface& net;

  PageRankAll(Graph* _graph) : graph(_graph), net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph) {
#ifdef GALOIS_USER_STATS
    constexpr bool USER_STATS = true;
#else
    constexpr bool USER_STATS = false;
#endif

    unsigned _num_iterations   = 0;

    const auto& allNodes = _graph.allNodesRange();

    DGTerminatorDetector dga;
  
    auto& _net = galois::runtime::getSystemNetworkInterface();

    do {
      std::string total_str("Total_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_total(total_str.c_str(), REGION_NAME_RUN.c_str());
      std::string delta_str("Delta_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_delta(delta_str.c_str(), REGION_NAME_RUN.c_str());
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

      StatTimer_delta.start();
      PageRank_delta::go(_graph, dga);
      StatTimer_delta.stop();

      _net.prefetchBuffers();

      // launch all other threads to compute
      StatTimer_compute.start();
      galois::do_all(
          galois::iterate(allNodes), PageRankAll{&_graph},
          galois::steal(), galois::no_stats());
      StatTimer_compute.stop();

      // inform all other hosts that this host has finished sending messages
      // force all messages to be processed before continuing
      StatTimer_flush.start();
      _net.flushRemoteWork();
      StatTimer_flush.stop();

      StatTimer_comm.start();
      syncSubstrate->poll_for_remote_work<Reduce_add_residual>();
      StatTimer_comm.stop();
      
      _net.resetWorkTermination();

      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "NumWorkItems_Round_" + std::to_string(_num_iterations), (unsigned long)_graph.sizeEdges());

      ++_num_iterations;

      StatTimer_total.stop();
    } while ((_num_iterations < maxIterations) && dga.reduce(syncSubstrate->get_run_identifier()));
  }

  // Pull deltas from neighbor nodes, then add to self-residual
  void operator()(GNode dst) const {
    // source node can be master, mirror or phantom
    if (graph->isPhantom(dst)) {
        // create register for phantom node data
        float dresidual = 0;
        
        for (auto nbr : graph->inEdges(dst)) {
            GNode src   = graph->getInEdgeSrc(nbr);
            // destination node must be masters
            auto& sdata = graph->getData(src);

            if (sdata.delta > 0) {
                dresidual = dresidual + sdata.delta;
            }
        }

        if (dresidual != 0) {
            net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getPhantomRemoteLID(dst), dresidual);
        }
    } else {
        auto& ddata = graph->getData(dst);

        for (auto nbr : graph->inEdges(dst)) {
            GNode src   = graph->getInEdgeSrc(nbr);
            // destination node must be masters
            auto& sdata = graph->getData(src);

            if (sdata.delta > 0) {
                galois::add(ddata.residual, sdata.delta);
            }
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
      galois::gPrint("# nodes with residual over ", tolerance, " (tolerance) is ", over_tolerance, "\n");
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
constexpr static const char* const desc = "PageRank Residual Pull version on "
                                          "Distributed Galois.";
constexpr static const char* const url = nullptr;

int main(int argc, char** argv) {
  galois::DistMemSys G;
  DistBenchStart(argc, argv, name, desc, url);

  auto& net = galois::runtime::getSystemNetworkInterface();

  if (net.ID == 0) {
    galois::runtime::reportParam(REGION_NAME, "Max Iterations", maxIterations);
    std::ostringstream ss;
    ss << tolerance;
    galois::runtime::reportParam(REGION_NAME, "Tolerance", ss.str());
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
  std::tie(hg, syncSubstrate) = distGraphInitialization<NodeData, void, float, false>();

  net.allocateBufferPool();

  galois::runtime::getHostBarrier().wait();
  net.partitionDone();

  galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");

  InitializeGraph::go(*hg);
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
    galois::runtime::getHostBarrier().wait();

    StatTimer_main.start();
    if (iterMode == All) {
        PageRankAll::go(*hg);
    } else {
        PageRankSep::go(*hg);
    }
    StatTimer_main.stop();
    galois::gPrint("Host ", net.ID, " PageRank run ", run, " time: ", StatTimer_main.get(), " ms\n");

    // sanity check
    PageRankSanity::go(*hg, DGA_sum, DGA_sum_residual,
                       DGA_residual_over_tolerance, max_value, min_value,
                       max_residual, min_residual);

    if ((run + 1) != numRuns) {
      syncSubstrate->set_num_run(run + 1);
      galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");
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
