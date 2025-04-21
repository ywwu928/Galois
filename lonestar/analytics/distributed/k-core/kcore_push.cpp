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
#include "galois/DTerminationDetector.h"
#include "galois/gstl.h"
#include "galois/runtime/Tracer.h"

#include <iostream>
#include <limits>

static std::string REGION_NAME = "KCore";
static std::string REGION_NAME_RUN;

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

enum Exec { Sync, Async };

static cll::opt<Exec> execution(
    "exec", cll::desc("Distributed Execution Model (default value Async):"),
    cll::values(clEnumVal(Sync, "Bulk-synchronous Parallel (BSP)"),
                clEnumVal(Async, "Bulk-asynchronous Parallel (BASP)")),
    cll::init(Async));

/******************************************************************************/
/* Graph structure declarations + other inits */
/******************************************************************************/

struct NodeData {
  std::atomic<uint32_t> current_degree;
  std::atomic<uint32_t> trim;
  uint8_t flag;
};

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;

// bitset for tracking updates
galois::DynamicBitSet bitset_current_degree;
galois::DynamicBitSet bitset_trim;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph>> syncSubstrate;

// add all sync/bitset structs (needs above declarations)
#include "kcore_push_sync.hh"

/******************************************************************************/
/* Functors for running the algorithm */
/******************************************************************************/

/* Degree counting
 * Called by InitializeGraph1 */
struct InitializeGraph2 {
  Graph* graph;

  InitializeGraph2(Graph* _graph) : graph(_graph) {}

  /* Initialize the entire graph node-by-node */
  void static go(Graph& _graph) {
    const auto& masterNodes = _graph.masterNodesRange();

    galois::do_all(
        galois::iterate(masterNodes), InitializeGraph2{&_graph},
        galois::steal(), galois::no_stats());

    syncSubstrate->sync<writeDestination, readSource, Reduce_add_current_degree, Bitset_current_degree>("InitializeGraph2");
  }

  /* Calculate degree of nodes by checking how many nodes have it as a dest and
   * adding for every dest */
  void operator()(GNode src) const {
    for (auto current_edge : graph->edges(src)) {
      GNode dest_node = graph->getEdgeDst(current_edge);

      NodeData& dest_data = graph->getData(dest_node);
      galois::atomicAdd(dest_data.current_degree, (uint32_t)1);

      bitset_current_degree.set(dest_node);
    }
  }
};

/* Initialize: initial field setup */
struct InitializeGraph1 {
  Graph* graph;

  InitializeGraph1(Graph* _graph) : graph(_graph) {}

  /* Initialize the entire graph node-by-node */
  void static go(Graph& _graph) {
    const auto& allNodes = _graph.allNodesRange();

    galois::do_all(
        galois::iterate(allNodes.begin(), allNodes.end()),
        InitializeGraph1{&_graph}, galois::no_stats());

    galois::runtime::getHostBarrier().wait();

    // degree calculation
    InitializeGraph2::go(_graph);
  }

  /* Setup intial fields */
  void operator()(GNode src) const {
    NodeData& src_data      = graph->getData(src);
    src_data.flag           = true;
    src_data.trim           = 0;
    src_data.current_degree = 0;
  }
};

/* Use the trim value (i.e. number of incident nodes that have been removed)
 * to update degrees.
 * Called by KCoreStep1 */
struct KCoreStep2 {
  Graph* graph;

  KCoreStep2(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
    const auto& allNodes = _graph.allNodesRange();
    
    galois::do_all(
        galois::iterate(allNodes),
        KCoreStep2{&_graph}, galois::no_stats(),
        galois::loopname(syncSubstrate->get_run_identifier("KCore").c_str()));
  }

  void operator()(GNode src) const {
    NodeData& src_data = graph->getData(src);

    // we currently do not care about degree for dead nodes,
    // so we ignore those (i.e. if flag isn't set, do nothing)
    if (src_data.flag) {
      if (src_data.trim > 0) {
        src_data.current_degree = src_data.current_degree - src_data.trim;
      }
    }

    src_data.trim = 0;
  }
};

/* Step that determines if a node is dead and updates its neighbors' trim
 * if it is */
template <bool async>
struct KCoreStep1 {
  cll::opt<uint32_t>& local_k_core_num;
  Graph* graph;

  using DGTerminatorDetector =
      typename std::conditional<async, galois::DGTerminator<unsigned int>,
                                galois::DGAccumulator<unsigned int>>::type;

  DGTerminatorDetector& active_vertices;

  KCoreStep1(cll::opt<uint32_t>& _kcore, Graph* _graph,
             DGTerminatorDetector& _dga)
      : local_k_core_num(_kcore), graph(_graph), active_vertices(_dga) {}

  void static go(Graph& _graph) {
#ifdef GALOIS_USER_STATS
    constexpr bool USER_STATS = true;
#else
    constexpr bool USER_STATS = false;
#endif

    unsigned iterations = 0;

    DGTerminatorDetector dga;

    const auto& masterNodes = _graph.masterNodesRange();

    do {
      std::string total_str("Total_Round_" + std::to_string(iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_total(total_str.c_str(), REGION_NAME_RUN.c_str());
      std::string step2_str("Step2_Round_" + std::to_string(iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_step2(step2_str.c_str(), REGION_NAME_RUN.c_str());
      std::string compute_str("Compute_Round_" + std::to_string(iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_compute(compute_str.c_str(), REGION_NAME_RUN.c_str());
      std::string comm_str("Communication_Round_" + std::to_string(iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_comm(comm_str.c_str(), REGION_NAME_RUN.c_str());

#ifdef GALOIS_PRINT_PROCESS
      galois::gPrint("Host ", _net.ID, " : iteration ", _num_iterations, "\n");
#endif

      StatTimer_total.start();
      syncSubstrate->set_num_round(iterations);

      dga.reset();

      StatTimer_compute.start();
      galois::do_all(galois::iterate(masterNodes),
                     KCoreStep1{k_core_num, &_graph, dga}, galois::steal(),
                     galois::no_stats(),
                     galois::loopname(syncSubstrate->get_run_identifier("KCore").c_str()));
      StatTimer_compute.stop();

      // do the trim sync; readSource because in symmetric graph
      // source=destination; not a readAny because any will grab non
      // source/dest nodes (which have degree 0, so they won't have a trim
      // anyways)
      StatTimer_comm.start();
      syncSubstrate->sync<writeDestination, readSource, Reduce_add_trim, Bitset_trim, async>("KCore");
      StatTimer_comm.stop();

      // handle trimming (locally)
      StatTimer_step2.start();
      KCoreStep2::go(_graph);
      StatTimer_step2.stop();

      iterations++;

      StatTimer_total.stop();
    } while ((async || (iterations < maxIterations)) &&
             dga.reduce(syncSubstrate->get_run_identifier()));
  }

  void operator()(GNode src) const {
    NodeData& src_data = graph->getData(src);

    // only if node is alive we do things
    if (src_data.flag) {
      if (src_data.current_degree < local_k_core_num) {
        // set flag to 0 (false) and increment trim on outgoing neighbors
        // (if they exist)
        src_data.flag = false;
        active_vertices += 1; // can be optimized: node may not have edges

        for (auto current_edge : graph->edges(src)) {
          GNode dst = graph->getEdgeDst(current_edge);

          auto& dst_data = graph->getData(dst);

          galois::atomicAdd(dst_data.trim, (uint32_t)1);
          bitset_trim.set(dst);
        }
      }
    }
  }
};

/******************************************************************************/
/* Sanity check operators */
/******************************************************************************/

/* Gets the total number of nodes that are still alive */
struct KCoreSanityCheck {
  Graph* graph;
  galois::DGAccumulator<uint64_t>& active_vertices;

  KCoreSanityCheck(Graph* _graph,
                   galois::DGAccumulator<uint64_t>& _active_vertices)
      : graph(_graph), active_vertices(_active_vertices) {}

  void static go(Graph& _graph, galois::DGAccumulator<uint64_t>& dga) {
    dga.reset();

    galois::do_all(galois::iterate(_graph.masterNodesRange().begin(),
                                   _graph.masterNodesRange().end()),
                   KCoreSanityCheck(&_graph, dga), galois::no_stats());

    uint64_t num_nodes = dga.reduce();

    // Only node 0 will print data
    if (galois::runtime::getSystemNetworkInterface().ID == 0) {
      galois::gPrint("Number of nodes in the ", k_core_num, "-core is ",
                     num_nodes, "\n");
    }
  }

  /* Check if an owned node is alive/dead: increment appropriate accumulator */
  void operator()(GNode src) const {
    NodeData& src_data = graph->getData(src);

    if (src_data.flag) {
      active_vertices += 1;
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
    values.push_back(hg->getData(node).flag);
  }

  return values;
}

/******************************************************************************/
/* Main method for running */
/******************************************************************************/

constexpr static const char* const name = "KCore - Distributed Heterogeneous "
                                          "Push Filter.";
constexpr static const char* const desc = "KCore on Distributed Galois.";
constexpr static const char* const url  = nullptr;

int main(int argc, char** argv) {
  galois::DistMemSys G;
  DistBenchStart(argc, argv, name, desc, url);

  auto& net = galois::runtime::getSystemNetworkInterface();

  if (net.ID == 0) {
    galois::runtime::reportParam(REGION_NAME, "Max Iterations", maxIterations);
  }

  galois::StatTimer StatTimer_total("TimerTotal", REGION_NAME.c_str());
  StatTimer_total.start();
  galois::StatTimer StatTimer_preprocess("TimerPreProcess", REGION_NAME.c_str());
  StatTimer_preprocess.start();

  std::unique_ptr<Graph> h_graph;
  std::tie(h_graph, syncSubstrate) = symmetricDistGraphInitialization<NodeData, void>();

  bitset_current_degree.resize(h_graph->size());
  bitset_trim.resize(h_graph->size());

  galois::gPrint("[", net.ID, "] InitializeGraph::go functions called\n");

  InitializeGraph1::go((*h_graph));
  galois::runtime::getHostBarrier().wait();
  StatTimer_preprocess.stop();

  galois::DGAccumulator<uint64_t> dga;

  for (auto run = 0; run < numRuns; ++run) {
    REGION_NAME_RUN = REGION_NAME + "_" + std::to_string(run);
    galois::gPrint("[", net.ID, "] KCore::go run ", run, " called\n");
    std::string timer_str("Timer_" + std::to_string(run));
    galois::StatTimer StatTimer_main(timer_str.c_str(), REGION_NAME_RUN.c_str());

    StatTimer_main.start();
    if (execution == Async) {
      KCoreStep1<true>::go(*h_graph);
    } else {
      KCoreStep1<false>::go(*h_graph);
    }
    StatTimer_main.stop();
    galois::gPrint("Host ", net.ID, " KCore run ", run, " time: ", StatTimer_main.get(), " ms\n");

    // sanity check
    KCoreSanityCheck::go(*h_graph, dga);

    // re-init graph for next run
    if ((run + 1) != numRuns) {
      (*syncSubstrate).set_num_run(run + 1);

      bitset_current_degree.reset();
      bitset_trim.reset();

      galois::gPrint("[", net.ID, "] InitializeGraph::go functions called\n");
      InitializeGraph1::go(*h_graph);
      galois::runtime::getHostBarrier().wait();
    }
  }

  StatTimer_total.stop();

  if (output) {
    std::vector<unsigned> results = makeResults(h_graph);
    auto globalIDs                = h_graph->getMasterGlobalIDs();
    assert(results.size() == globalIDs.size());

    writeOutput(outputLocation, "in_kcore", results.data(), results.size(),
                globalIDs.data());
  }

  return 0;
}
