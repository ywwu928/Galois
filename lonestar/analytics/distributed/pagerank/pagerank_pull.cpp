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

#include <algorithm>
#include <iostream>
#include <limits>
#include <vector>

#ifdef GALOIS_ENABLE_GPU
#include "pagerank_pull_cuda.h"
struct CUDA_Context* cuda_ctx;
#else
enum { CPU, GPU_CUDA };
int personality = CPU;
#endif

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

enum Exec { Sync, Async };

static cll::opt<Exec> execution(
    "exec", cll::desc("Distributed Execution Model (default value Async):"),
    cll::values(clEnumVal(Sync, "Bulk-synchronous Parallel (BSP)"),
                clEnumVal(Async, "Bulk-asynchronous Parallel (BASP)")),
    cll::init(Async));

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

static const float alpha = (1.0 - 0.85);
struct NodeData {
  float value;
  std::atomic<uint32_t> nout;
  float residual;
  float delta;
};

galois::DynamicBitSet bitset_residual;
galois::DynamicBitSet bitset_nout;

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph>> syncSubstrate;

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
    const auto& allNodes = _graph.allNodesRange();
    if (personality == GPU_CUDA) {
#ifdef GALOIS_ENABLE_GPU
      std::string impl_str("ResetGraph_" +
                           (syncSubstrate->get_run_identifier()));
      galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME.c_str());
      StatTimer_cuda.start();
      ResetGraph_allNodes_cuda(alpha, cuda_ctx);
      StatTimer_cuda.stop();
#else
      abort();
#endif
    } else if (personality == CPU) {
      galois::do_all(
          galois::iterate(allNodes.begin(), allNodes.end()),
          ResetGraph{alpha, &_graph}, galois::no_stats(),
          galois::loopname(
              syncSubstrate->get_run_identifier("ResetGraph").c_str()));
    }
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

    const auto& allNodes = _graph.allNodesRange();

    if (personality == GPU_CUDA) {
#ifdef GALOIS_ENABLE_GPU
      std::string impl_str("InitializeGraph_" +
                           (syncSubstrate->get_run_identifier()));
      galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME.c_str());
      StatTimer_cuda.start();
      InitializeGraph_allNodes_cuda(cuda_ctx);
      StatTimer_cuda.stop();
#else
      abort();
#endif
    } else if (personality == CPU) {
      // doing a local do all because we are looping over edges
      galois::do_all(
          galois::iterate(allNodes), InitializeGraph{&_graph},
          galois::steal(), galois::no_stats(),
          galois::loopname(
              syncSubstrate->get_run_identifier("InitializeGraph").c_str()));
    }

    syncSubstrate
        ->sync<writeDestination, readAny, Reduce_add_nout, Bitset_nout>(
            "InitializeGraph");
  }

  // Calculate "outgoing" edges for destination nodes (note we are using
  // the tranpose graph for pull algorithms)
  void operator()(GNode src) const {
    for (auto nbr : graph->edges(src)) {
      GNode dst   = graph->getEdgeDst(nbr);
      auto& ddata = graph->getData(dst);
      galois::atomicAdd(ddata.nout, (uint32_t)1);
      bitset_nout.set(dst);
    }
  }
};

template <bool async>
struct PageRank_delta {
  const float& local_alpha;
  cll::opt<float>& local_tolerance;
  Graph* graph;

  using DGTerminatorDetector =
      typename std::conditional<async, galois::DGTerminator<unsigned int>,
                                galois::DGAccumulator<unsigned int>>::type;

  DGTerminatorDetector& active_vertices;

  PageRank_delta(const float& _local_alpha, cll::opt<float>& _local_tolerance,
                 Graph* _graph, DGTerminatorDetector& _dga)
      : local_alpha(_local_alpha), local_tolerance(_local_tolerance),
        graph(_graph), active_vertices(_dga) {}

  void static go(Graph& _graph, DGTerminatorDetector& dga) {
    const auto& allNodes = _graph.allNodesRange();

    if (personality == GPU_CUDA) {
#ifdef GALOIS_ENABLE_GPU
      std::string impl_str("PageRank_" + (syncSubstrate->get_run_identifier()));
      galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME.c_str());
      StatTimer_cuda.start();
      unsigned int __retval = 0;
      PageRank_delta_allNodes_cuda(__retval, alpha, tolerance, cuda_ctx);
      dga += __retval;
      StatTimer_cuda.stop();
#else
      abort();
#endif
    } else if (personality == CPU) {
      galois::do_all(
          galois::iterate(allNodes.begin(), allNodes.end()),
          PageRank_delta{alpha, tolerance, &_graph, dga}, galois::no_stats(),
          galois::loopname(
              syncSubstrate->get_run_identifier("PageRank_delta").c_str()));
    }
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

// TODO: GPU code operator does not match CPU's operator (cpu accumulates sum
// and adds all at once, GPU adds each pulled value individually/atomically)
template <bool async>
struct PageRank {
  Graph* graph;

  using DGTerminatorDetector =
      typename std::conditional<async, galois::DGTerminator<unsigned int>,
                                galois::DGAccumulator<unsigned int>>::type;

  PageRank(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
    unsigned _num_iterations   = 0;
    const auto& allNodes = _graph.allNodesRange();
    DGTerminatorDetector dga;

    // unsigned int reduced = 0;
    auto& net = galois::runtime::getSystemNetworkInterface();

    do {
      syncSubstrate->set_num_round(_num_iterations);
      dga.reset();
      // reset residual on mirrors
      if (partitionScheme == OEC) {
          syncSubstrate->reset_mirrorField<Reduce_add_residual>();
      }
      
      std::string compute_str("Host_" + std::to_string(net.ID) + "_Compute_Round_" + std::to_string(_num_iterations));
      galois::StatTimer StatTimer_compute(compute_str.c_str(), REGION_NAME_RUN.c_str());

      StatTimer_compute.start();
      PageRank_delta<async>::go(_graph, dga);

      if (personality == GPU_CUDA) {
#ifdef GALOIS_ENABLE_GPU
        std::string impl_str("PageRank_" +
                             (syncSubstrate->get_run_identifier()));
        galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME.c_str());
        StatTimer_cuda.start();
        PageRank_allNodes_cuda(cuda_ctx);
        StatTimer_cuda.stop();
#else
        abort();
#endif
      } else if (personality == CPU) {
        galois::do_all(
            galois::iterate(allNodes), PageRank{&_graph}, galois::steal(),
            galois::no_stats(),
            galois::loopname(
                syncSubstrate->get_run_identifier("PageRank").c_str()));
      }
      StatTimer_compute.stop();

      std::string comm_str("Host_" + std::to_string(net.ID) + "_Communication_Round_" + std::to_string(_num_iterations));
      galois::StatTimer StatTimer_comm(comm_str.c_str(), REGION_NAME_RUN.c_str());

      StatTimer_comm.start();
      syncSubstrate->sync<writeSource, readDestination, Reduce_add_residual,
                          Bitset_residual, async>("PageRank");
      StatTimer_comm.stop();

      galois::runtime::reportStat_Tsum(
          REGION_NAME.c_str(), "NumWorkItems_" + (syncSubstrate->get_run_identifier()),
          (unsigned long)_graph.sizeEdges());

      ++_num_iterations;
    } while ((async || (_num_iterations < maxIterations)) &&
             dga.reduce(syncSubstrate->get_run_identifier()));

    galois::runtime::reportStat_Tmax(
        REGION_NAME.c_str(),
        "NumIterations_" + std::to_string(syncSubstrate->get_run_num()),
        (unsigned long)_num_iterations);
  }

  // Pull deltas from neighbor nodes, then add to self-residual
  void operator()(GNode src) const {
    auto& sdata = graph->getData(src);

    for (auto nbr : graph->edges(src)) {
      GNode dst   = graph->getEdgeDst(nbr);
      auto& ddata = graph->getData(dst);

      if (ddata.delta > 0) {
        galois::add(sdata.residual, ddata.delta);

        bitset_residual.set(src);
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

    if (personality == GPU_CUDA) {
#ifdef GALOIS_ENABLE_GPU
      float _max_value;
      float _min_value;
      float _sum_value;
      float _sum_residual;
      uint64_t num_residual_over_tolerance;
      float _max_residual;
      float _min_residual;
      PageRankSanity_masterNodes_cuda(
          num_residual_over_tolerance, _sum_value, _sum_residual, _max_residual,
          _max_value, _min_residual, _min_value, tolerance, cuda_ctx);
      DGA_sum += _sum_value;
      DGA_sum_residual += _sum_residual;
      DGA_residual_over_tolerance += num_residual_over_tolerance;
      max_value.update(_max_value);
      max_residual.update(_max_residual);
      min_value.update(_min_value);
      min_residual.update(_min_residual);
#endif
    } else {
      galois::do_all(galois::iterate(_graph.masterNodesRange().begin(),
                                     _graph.masterNodesRange().end()),
                     PageRankSanity(tolerance, &_graph, DGA_sum,
                                    DGA_sum_residual,
                                    DGA_residual_over_tolerance, max_value,
                                    min_value, max_residual, min_residual),
                     galois::no_stats(), galois::loopname("PageRankSanity"));
    }

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

std::vector<float> makeResultsCPU(std::unique_ptr<Graph>& hg) {
  std::vector<float> values;

  values.reserve(hg->numMasters());
  for (auto node : hg->masterNodesRange()) {
    values.push_back(hg->getData(node).value);
  }

  return values;
}

#ifdef GALOIS_ENABLE_GPU
std::vector<float> makeResultsGPU(std::unique_ptr<Graph>& hg) {
  std::vector<float> values;

  values.reserve(hg->numMasters());
  for (auto node : hg->masterNodesRange()) {
    values.push_back(get_node_value_cuda(cuda_ctx, node));
  }

  return values;
}
#else
std::vector<float> makeResultsGPU(std::unique_ptr<Graph>& /*unused*/) {
  abort();
}
#endif

std::vector<float> makeResults(std::unique_ptr<Graph>& hg) {
  switch (personality) {
  case CPU:
    return makeResultsCPU(hg);
  case GPU_CUDA:
    return makeResultsGPU(hg);
  default:
    abort();
  }
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
    galois::runtime::reportParam(REGION_NAME.c_str(), "Max Iterations", maxIterations);
    std::ostringstream ss;
    ss << tolerance;
    galois::runtime::reportParam(REGION_NAME.c_str(), "Tolerance", ss.str());
  }

  galois::StatTimer StatTimer_total("TimerTotal", REGION_NAME.c_str());

  StatTimer_total.start();

  std::unique_ptr<Graph> hg;
#ifdef GALOIS_ENABLE_GPU
  std::tie(hg, syncSubstrate) =
      distGraphInitialization<NodeData, void, false>(&cuda_ctx);
#else
  std::tie(hg, syncSubstrate) =
      distGraphInitialization<NodeData, void, false>();
#endif

  bitset_residual.resize(hg->size());
  bitset_nout.resize(hg->size());

  galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");

  InitializeGraph::go(*hg);
  galois::runtime::getHostBarrier().wait();

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

    StatTimer_main.start();
    if (execution == Async) {
      PageRank<true>::go(*hg);
    } else {
      PageRank<false>::go(*hg);
    }
    StatTimer_main.stop();
    galois::gPrint("Host ", net.ID, " PageRank run ", run, " time: ", StatTimer_main.get(), " ms\n");

    // sanity check
    PageRankSanity::go(*hg, DGA_sum, DGA_sum_residual,
                       DGA_residual_over_tolerance, max_value, min_value,
                       max_residual, min_residual);

    if ((run + 1) != numRuns) {
      if (personality == GPU_CUDA) {
#ifdef GALOIS_ENABLE_GPU
        bitset_residual_reset_cuda(cuda_ctx);
        bitset_nout_reset_cuda(cuda_ctx);
#else
        abort();
#endif
      } else {
        bitset_residual.reset();
        bitset_nout.reset();
      }

      syncSubstrate->set_num_run(run + 1);
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
