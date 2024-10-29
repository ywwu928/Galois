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

static std::string REGION_NAME = "TouchSubset";
static std::string REGION_NAME_RUN;

/******************************************************************************/
/* Declaration of command line arguments */
/******************************************************************************/
namespace cll = llvm::cl;

static cll::opt<unsigned int>
    maxIterations("maxIterations",
                  cll::desc("Maximum iterations: Default 1000"),
                  cll::init(1000));

static cll::opt<unsigned int>
    factor("factor",
                  cll::desc("Number to do modulo for filtering nodes"),
                  cll::init(1));

enum Exec { Sync, Async };

static cll::opt<Exec> execution(
    "exec", cll::desc("Distributed Execution Model (default value Async):"),
    cll::values(clEnumVal(Sync, "Bulk-synchronous Parallel (BSP)"),
                clEnumVal(Async, "Bulk-asynchronous Parallel (BASP)")),
    cll::init(Async));

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

struct NodeData {
  std::atomic<uint32_t> value;
  uint32_t sum;
};

galois::DynamicBitSet bitset_value;

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;
typedef GNode WorkItem;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph, uint32_t>> syncSubstrate;

#include "touch_subset_push_sync.hh"

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/

// Reset (Initialize) all fields of all nodes to 0
struct InitializeGraph {
  Graph* graph;

  InitializeGraph(Graph* _graph) : graph(_graph) {}
  void static go(Graph& _graph) {
    const auto& presentNodes = _graph.presentNodesRange();
    galois::do_all(
        galois::iterate(presentNodes.begin(), presentNodes.end()),
        InitializeGraph{&_graph}, galois::no_stats(),
        galois::loopname(syncSubstrate->get_run_identifier("InitializeGraph").c_str()));
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    sdata.value = 1;
    sdata.sum = 1;
  }
};

struct TouchSubset_sync {
  Graph* graph;

  TouchSubset_sync(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
    const auto& masterNodes = _graph.masterNodesRange();

    galois::do_all(
        galois::iterate(masterNodes.begin(), masterNodes.end()),
        TouchSubset_sync{&_graph}, galois::no_stats(),
        galois::loopname(syncSubstrate->get_run_identifier("TouchSubset_sync").c_str()));
  }

  void operator()(WorkItem src) const {
    NodeData& sdata = graph->getData(src);
    sdata.sum += sdata.value;
    sdata.value = 0;
  }
};

template <bool async>
struct TouchSubset {
  Graph* graph;

  TouchSubset(Graph* _g) : graph(_g) {}

  void static go(Graph& _graph) {
    unsigned _num_iterations   = 0;
#ifndef GALOIS_FULL_MIRRORING     
    const auto& masterNodes = _graph.masterNodesRangeReserved();
#else
    const auto& masterNodes = _graph.masterNodesRange();
#endif
  
    auto& net = galois::runtime::getSystemNetworkInterface();

    do {
      syncSubstrate->set_num_round(_num_iterations);
      // reset value on mirrors
      syncSubstrate->reset_mirrorField<Reduce_add_value>();
      
      std::string compute_str("Host_" + std::to_string(net.ID) + "_Compute_Round_" + std::to_string(_num_iterations));
      galois::StatTimer StatTimer_compute(compute_str.c_str(), REGION_NAME_RUN.c_str());
      
      StatTimer_compute.start();
      TouchSubset_sync::go(_graph);

#ifndef GALOIS_FULL_MIRRORING
      syncSubstrate->set_update_buf_to_identity(0);
      // dedicate a thread to poll for remote messages
      std::function<void(void)> func = [&]() {
              syncSubstrate->poll_for_remote_work_dedicated(galois::add<uint32_t>);
      };
      galois::substrate::getThreadPool().runDedicated(func);
#endif
      // launch all other threads to compute
      galois::do_all(galois::iterate(masterNodes), TouchSubset{&_graph},
                     galois::no_stats(), galois::steal(),
                     galois::loopname(syncSubstrate->get_run_identifier("TouchSubset").c_str()));

#ifndef GALOIS_FULL_MIRRORING     
      // inform all other hosts that this host has finished sending messages
      // force all messages to be processed before continuing
      syncSubstrate->net_flush();
      galois::substrate::getThreadPool().waitDedicated();
      syncSubstrate->sync_update_buf<Reduce_add_value>(0);
#endif
      StatTimer_compute.stop();
      
      std::string comm_str("Host_" + std::to_string(net.ID) + "_Communication_Round_" + std::to_string(_num_iterations));
      galois::StatTimer StatTimer_comm(comm_str.c_str(), REGION_NAME_RUN.c_str());

      StatTimer_comm.start();
#ifndef GALOIS_NO_MIRRORING     
      syncSubstrate->sync<writeDestination, readSource, Reduce_add_value,
                          Bitset_value, async>("TouchSubset");
#else
      syncSubstrate->poll_for_remote_work<Reduce_add_value>();
#endif
      StatTimer_comm.stop();
      
      syncSubstrate->reset_termination();

      ++_num_iterations;
      galois::runtime::getHostBarrier().wait();
    } while (_num_iterations < maxIterations);

    if (galois::runtime::getSystemNetworkInterface().ID == 0) {
      galois::runtime::reportStat_Single(
          REGION_NAME_RUN.c_str(),
          "NumIterations_" + std::to_string(syncSubstrate->get_run_num()),
          (unsigned long)_num_iterations);
    }
  }

  void operator()(WorkItem src) const {
      NodeData& sdata = graph->getData(src);
      if (src % factor == 0) { // filter out nodes
          for (auto nbr : graph->edges(src)) {
              GNode dst       = graph->getEdgeDst(nbr);
#ifndef GALOIS_FULL_MIRRORING     
              if (graph->isGhost(dst)) {
                  syncSubstrate->send_data_to_remote(graph->getHostIDForLocal(dst), graph->getGhostRemoteLID(dst), sdata.sum);
              }
              else {
#endif
                  NodeData& ddata = graph->getData(dst);
                  galois::atomicAdd(ddata.value, sdata.sum);
                  bitset_value.set(dst);
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

struct TouchSubsetSanity {
  Graph* graph;

  galois::DGAccumulator<float>& DGAccumulator_value;
  galois::DGReduceMax<float>& max_value;
  galois::DGReduceMin<float>& min_value;
  galois::DGAccumulator<float>& DGAccumulator_sum;
  galois::DGReduceMax<float>& max_sum;
  galois::DGReduceMin<float>& min_sum;

  TouchSubsetSanity(
      Graph* _graph,
      galois::DGAccumulator<float>& _DGAccumulator_value,
      galois::DGReduceMax<float>& _max_value,
      galois::DGReduceMin<float>& _min_value,
      galois::DGAccumulator<float>& _DGAccumulator_sum,
      galois::DGReduceMax<float>& _max_sum,
      galois::DGReduceMin<float>& _min_sum)
      : graph(_graph),
        DGAccumulator_value(_DGAccumulator_value),
        max_value(_max_value), min_value(_min_value),
        DGAccumulator_sum(_DGAccumulator_sum),
        max_sum(_max_sum), min_sum(_min_sum) {}

  void static go(Graph& _graph,
                 galois::DGAccumulator<float>& DGA_value,
                 galois::DGReduceMax<float>& max_value,
                 galois::DGReduceMin<float>& min_value,
                 galois::DGAccumulator<float>& DGA_sum,
                 galois::DGReduceMax<float>& max_sum,
                 galois::DGReduceMin<float>& min_sum) {
    DGA_value.reset();
    max_value.reset();
    min_value.reset();
    DGA_sum.reset();
    max_sum.reset();
    min_sum.reset();

    galois::do_all(galois::iterate(_graph.masterNodesRange().begin(),
                                   _graph.masterNodesRange().end()),
                   TouchSubsetSanity(&_graph, DGA_value, max_value, min_value, DGA_sum, max_sum, min_sum),
                   galois::no_stats(), galois::loopname("TouchSubsetSanity"));

    float max_rank          = max_value.reduce();
    float min_rank          = min_value.reduce();
    float rank_sum          = DGA_value.reduce();
    float max_acc          = max_sum.reduce();
    float min_acc          = min_sum.reduce();
    float acc_sum          = DGA_sum.reduce();

    // Only node 0 will print data
    if (galois::runtime::getSystemNetworkInterface().ID == 0) {
      galois::gPrint("Max value is ", max_rank, "\n");
      galois::gPrint("Min value is ", min_rank, "\n");
      galois::gPrint("Accumulated value is ", rank_sum, "\n");
      galois::gPrint("Max sum is ", max_acc, "\n");
      galois::gPrint("Min sum is ", min_acc, "\n");
      galois::gPrint("Accumulated sum is ", acc_sum, "\n");
    }
  }

  /* Gets the max, min rank from all owned nodes and
   * also the sum of ranks */
  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);

    max_value.update(sdata.value);
    min_value.update(sdata.value);
    DGAccumulator_value += sdata.value;
    max_sum.update(sdata.sum);
    min_sum.update(sdata.sum);
    DGAccumulator_sum += sdata.sum;
  }
};

/******************************************************************************/
/* Main */
/******************************************************************************/

constexpr static const char* const name = "TouchSubset - Compiler Generated "
                                          "Distributed Heterogeneous";
constexpr static const char* const desc = "TouchSubset Benchmark on Distributed "
                                          "Galois.";
constexpr static const char* const url = 0;

int main(int argc, char** argv) {
  galois::DistMemSys G;
  DistBenchStart(argc, argv, name, desc, url);

  auto& net = galois::runtime::getSystemNetworkInterface();

  if (net.ID == 0) {
    galois::runtime::reportParam(REGION_NAME.c_str(), "Max Iterations", maxIterations);
    galois::runtime::reportParam(REGION_NAME.c_str(), "Factor", factor);
  }

  galois::StatTimer StatTimer_total("TimerTotal", REGION_NAME.c_str());
  StatTimer_total.start();

  std::unique_ptr<Graph> hg;
  std::tie(hg, syncSubstrate) = distGraphInitialization<NodeData, void, uint32_t>();

  hg->sortEdgesByDestination();

  bitset_value.resize(hg->size());

  galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");

  InitializeGraph::go((*hg));
  galois::runtime::getHostBarrier().wait();

  galois::DGAccumulator<float> DGA_value;
  galois::DGReduceMax<float> max_value;
  galois::DGReduceMin<float> min_value;
  galois::DGAccumulator<float> DGA_sum;
  galois::DGReduceMax<float> max_sum;
  galois::DGReduceMin<float> min_sum;

  for (auto run = 0; run < numRuns; ++run) {
    REGION_NAME_RUN = REGION_NAME + "_" + std::to_string(run);
    galois::gPrint("[", net.ID, "] TouchSubset::go run ", run, " called\n");
    std::string timer_str("Timer_" + std::to_string(run));
    galois::StatTimer StatTimer_main(timer_str.c_str(), REGION_NAME_RUN.c_str());

    StatTimer_main.start();
    if (execution == Async) {
      TouchSubset<true>::go(*hg);
    } else {
      TouchSubset<false>::go(*hg);
    }
    StatTimer_main.stop();

    // sanity check
    TouchSubsetSanity::go(*hg, DGA_value, max_value, min_value, DGA_sum, max_sum, min_sum);

    if ((run + 1) != numRuns) {
      bitset_value.reset();

      (*syncSubstrate).set_num_run(run + 1);
      galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");
      InitializeGraph::go(*hg);
      galois::runtime::getHostBarrier().wait();
    }
  }

  StatTimer_total.stop();

  return 0;
}
