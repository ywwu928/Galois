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
#include "galois/DReducible.h"
#include "galois/DTerminationDetector.h"
#include "galois/gstl.h"
#include "galois/runtime/Tracer.h"

#include <iostream>
#include <limits>

static std::string REGION_NAME = "ConnectedComp";
static std::string REGION_NAME_RUN;

/******************************************************************************/
/* Declaration of command line arguments */
/******************************************************************************/

namespace cll = llvm::cl;
static cll::opt<unsigned int> maxIterations("maxIterations",
                                            cll::desc("Maximum iterations: "
                                                      "Default 1000"),
                                            cll::init(1000));

enum Exec { Sync, Async };

static cll::opt<Exec> execution(
    "exec", cll::desc("Distributed Execution Model (default value Async):"),
    cll::values(clEnumVal(Sync, "Bulk-synchronous Parallel (BSP)"),
                clEnumVal(Async, "Bulk-asynchronous Parallel (BASP)")),
    cll::init(Sync));

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

struct NodeData {
  std::atomic<uint32_t> comp_current;
  uint32_t comp_old;
};

galois::DynamicBitSet bitset_comp_current;

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph, uint32_t>> syncSubstrate;

#include "cc_push_sync.hh"

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/

struct InitializeGraph {
  Graph* graph;

  InitializeGraph(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
      const auto& presentNodes = _graph.presentNodesRange();

      galois::do_all(
          galois::iterate(presentNodes.begin(), presentNodes.end()),
          InitializeGraph{&_graph}, galois::no_stats(),
          galois::loopname(
              syncSubstrate->get_run_identifier("InitializeGraph").c_str()));
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    sdata.comp_current = graph->getGID(src);
    sdata.comp_old = graph->getGID(src);
  }
};

template <bool async>
struct FirstItr_ConnectedComp {
  Graph* graph;
  FirstItr_ConnectedComp(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
#ifndef GALOIS_FULL_MIRRORING     
    const auto& masterNodes = _graph.masterNodesRangeReserved();
#else
    const auto& masterNodes = _graph.masterNodesRange();
#endif
    
    auto& net = galois::runtime::getSystemNetworkInterface();
    
    galois::gPrint("Host ", net.ID, " : iteration 0\n");
    syncSubstrate->set_num_round(0);
    
    std::string compute_str("Host_" + std::to_string(net.ID) + "_Compute_Round_" + std::to_string(0));
    galois::StatTimer StatTimer_compute(compute_str.c_str(), REGION_NAME_RUN.c_str());
      
    StatTimer_compute.start();
#ifndef GALOIS_FULL_MIRRORING     
      syncSubstrate->set_update_buf_to_identity(UINT32_MAX);
      // dedicate a thread to poll for remote messages
      std::function<void(void)> func = [&]() {
              syncSubstrate->poll_for_remote_work_dedicated<Reduce_min_comp_current>(galois::min<uint32_t>);
      };
      galois::substrate::getThreadPool().runDedicated(func);
#endif
    // launch all other threads to compute
    galois::do_all(
        galois::iterate(masterNodes), FirstItr_ConnectedComp{&_graph},
        galois::steal(), galois::no_stats(),
        galois::loopname(syncSubstrate->get_run_identifier("ConnectedComp").c_str()));

#ifndef GALOIS_FULL_MIRRORING     
    // inform all other hosts that this host has finished sending messages
    // force all messages to be processed before continuing
    syncSubstrate->net_flush();
#endif
    StatTimer_compute.stop();
    
    std::string comm_str("Host_" + std::to_string(net.ID) + "_Communication_Round_" + std::to_string(0));
    galois::StatTimer StatTimer_comm(comm_str.c_str(), REGION_NAME_RUN.c_str());

    StatTimer_comm.start();
#ifndef GALOIS_FULL_MIRRORING     
    syncSubstrate->sync_update_buf<Reduce_min_comp_current>(UINT32_MAX);
    galois::substrate::getThreadPool().waitDedicated();
#endif

#ifdef GALOIS_FULL_MIRRORING     
    syncSubstrate->sync<writeDestination, readSource, Reduce_min_comp_current, Bitset_comp_current, async>("ConnectedComp");
#elif defined(GALOIS_NO_MIRRORING)
    syncSubstrate->poll_for_remote_work<Reduce_min_comp_current>();
#else
    syncSubstrate->sync<writeDestination, readSource, Reduce_min_comp_current, Bitset_comp_current, async>("ConnectedComp");
#endif
    StatTimer_comm.stop();
      
    syncSubstrate->reset_termination();

    galois::runtime::reportStat_Tsum(
        REGION_NAME, "NumWorkItems_" + (syncSubstrate->get_run_identifier()),
        _graph.masterNodesRange().end() - _graph.masterNodesRange().begin());
  }

  void operator()(GNode src) const {
    NodeData& snode = graph->getData(src);
    snode.comp_old  = snode.comp_current;

    for (auto jj : graph->edges(src)) {
        GNode dst         = graph->getEdgeDst(jj);
#ifndef GALOIS_FULL_MIRRORING     
        if (graph->isPhantom(dst)) {
            uint32_t new_dist = snode.comp_current;
#ifdef GALOIS_EXCHANGE_PHANTOM_LID
            syncSubstrate->send_data_to_remote(graph->getHostIDForLocal(dst), graph->getPhantomRemoteLID(dst), new_dist);
#else
            syncSubstrate->send_data_to_remote(graph->getHostIDForLocal(dst), graph->getGID(dst), new_dist);
#endif
        }
        else {
#endif
            auto& dnode       = graph->getData(dst);
            uint32_t new_dist = snode.comp_current;
            uint32_t old_dist = galois::atomicMin(dnode.comp_current, new_dist);
            if (old_dist > new_dist)
                bitset_comp_current.set(dst);
#ifndef GALOIS_FULL_MIRRORING     
        }
#endif
    }
  }
};

template <bool async>
struct ConnectedComp {
  Graph* graph;
  using DGTerminatorDetector =
      typename std::conditional<async, galois::DGTerminator<unsigned int>,
                                galois::DGAccumulator<unsigned int>>::type;

  DGTerminatorDetector& active_vertices;

  ConnectedComp(Graph* _graph, DGTerminatorDetector& _dga)
      : graph(_graph), active_vertices(_dga) {}

  void static go(Graph& _graph) {
    using namespace galois::worklists;

    FirstItr_ConnectedComp<async>::go(_graph);
    galois::runtime::getHostBarrier().wait();


    unsigned _num_iterations = 1;
    DGTerminatorDetector dga;

#ifndef GALOIS_FULL_MIRRORING     
    const auto& masterNodes = _graph.masterNodesRangeReserved();
#else
    const auto& masterNodes = _graph.masterNodesRange();
#endif
  
    auto& net = galois::runtime::getSystemNetworkInterface();

    do {
      galois::gPrint("Host ", net.ID, " : iteration ", _num_iterations, "\n");
      syncSubstrate->set_num_round(_num_iterations);
      dga.reset();
      
      std::string compute_str("Host_" + std::to_string(net.ID) + "_Compute_Round_" + std::to_string(_num_iterations));
      galois::StatTimer StatTimer_compute(compute_str.c_str(), REGION_NAME_RUN.c_str());
      
      StatTimer_compute.start();
#ifndef GALOIS_FULL_MIRRORING     
      syncSubstrate->set_update_buf_to_identity(UINT32_MAX);
      // dedicate a thread to poll for remote messages
      std::function<void(void)> func = [&]() {
              syncSubstrate->poll_for_remote_work_dedicated<Reduce_min_comp_current>(galois::min<uint32_t>);
      };
      galois::substrate::getThreadPool().runDedicated(func);
#endif
      // launch all other threads to compute
      galois::do_all(
          galois::iterate(masterNodes), ConnectedComp(&_graph, dga),
          galois::no_stats(), galois::steal(),
          galois::loopname(syncSubstrate->get_run_identifier("ConnectedComp").c_str()));

#ifndef GALOIS_FULL_MIRRORING     
      // inform all other hosts that this host has finished sending messages
      // force all messages to be processed before continuing
      syncSubstrate->net_flush();
#endif
      StatTimer_compute.stop();
      
      std::string comm_str("Host_" + std::to_string(net.ID) + "_Communication_Round_" + std::to_string(_num_iterations));
      galois::StatTimer StatTimer_comm(comm_str.c_str(), REGION_NAME_RUN.c_str());

      StatTimer_comm.start();
#ifndef GALOIS_FULL_MIRRORING     
      syncSubstrate->sync_update_buf<Reduce_min_comp_current>(UINT32_MAX);
      galois::substrate::getThreadPool().waitDedicated();
#endif

#ifdef GALOIS_FULL_MIRRORING     
      syncSubstrate->sync<writeDestination, readSource, Reduce_min_comp_current, Bitset_comp_current, async>("ConnectedComp");
#elif defined(GALOIS_NO_MIRRORING)
      syncSubstrate->poll_for_remote_work<Reduce_min_comp_current>();
#else
      syncSubstrate->sync<writeDestination, readSource, Reduce_min_comp_current, Bitset_comp_current, async>("ConnectedComp");
#endif
      
      StatTimer_comm.stop();
      
      syncSubstrate->reset_termination();

      galois::runtime::reportStat_Tsum(
          REGION_NAME, "NumWorkItems_" + (syncSubstrate->get_run_identifier()),
          (unsigned long)dga.read_local());
      galois::runtime::reportStat_Single(
          REGION_NAME, "Host_" + std::to_string(net.ID) + "_Run_" + (syncSubstrate->get_run_identifier()) + "_Round_" + std::to_string(_num_iterations) + "_ActiveVertices",
          (unsigned long)dga.read_local());

      ++_num_iterations;
    } while ((async || (_num_iterations < maxIterations)) &&
             dga.reduce(syncSubstrate->get_run_identifier()));

    galois::runtime::reportStat_Tmax(
        REGION_NAME,
        "NumIterations_" + std::to_string(syncSubstrate->get_run_num()),
        (unsigned long)_num_iterations);
  }

  void operator()(GNode src) const {
    NodeData& snode = graph->getData(src);

    if (snode.comp_old > snode.comp_current) {
      snode.comp_old = snode.comp_current;

      for (auto jj : graph->edges(src)) {
        active_vertices += 1;

        GNode dst         = graph->getEdgeDst(jj);
#ifndef GALOIS_FULL_MIRRORING     
        if (graph->isPhantom(dst)) {
            uint32_t new_dist = snode.comp_current;
#ifdef GALOIS_EXCHANGE_PHANTOM_LID
            syncSubstrate->send_data_to_remote(graph->getHostIDForLocal(dst), graph->getPhantomRemoteLID(dst), new_dist);
#else
            syncSubstrate->send_data_to_remote(graph->getHostIDForLocal(dst), graph->getGID(dst), new_dist);
#endif
        }
        else {
#endif
            auto& dnode       = graph->getData(dst);
            uint32_t new_dist = snode.comp_current;
            uint32_t old_dist = galois::atomicMin(dnode.comp_current, new_dist);
            if (old_dist > new_dist)
                bitset_comp_current.set(dst);
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

/* Get/print the number of components */
struct ConnectedCompSanityCheck {
  Graph* graph;

  galois::DGAccumulator<uint64_t>& active_vertices;

  ConnectedCompSanityCheck(Graph* _graph, galois::DGAccumulator<uint64_t>& _dga)
      : graph(_graph), active_vertices(_dga) {}

  void static go(Graph& _graph, galois::DGAccumulator<uint64_t>& dga) {
    dga.reset();

    galois::do_all(galois::iterate(_graph.masterNodesRange().begin(), _graph.masterNodesRange().end()),
                   ConnectedCompSanityCheck(&_graph, dga), galois::no_stats(),
                   galois::loopname("ConnectedCompSanityCheck"));

    uint64_t num_components = dga.reduce();

    // Only node 0 will print the number visited
    if (galois::runtime::getSystemNetworkInterface().ID == 0) {
      galois::gPrint("Number of components is ", num_components, "\n");
    }
  }

  /* Check if a node's component is the same as its ID.
   * if yes, then increment an accumulator */
  void operator()(GNode src) const {
    NodeData& src_data = graph->getData(src);

    if (src_data.comp_current == graph->getGID(src)) {
      active_vertices += 1;
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
    values.push_back(hg->getData(node).comp_current);
  }

  return values;
}

/******************************************************************************/
/* Main */
/******************************************************************************/

constexpr static const char* const name = "ConnectedComp - Distributed "
                                          "Heterogeneous with filter.";
constexpr static const char* const desc =
    "ConnectedComp on Distributed Galois.";
constexpr static const char* const url = nullptr;

int main(int argc, char** argv) {
  galois::DistMemSys G;
  DistBenchStart(argc, argv, name, desc, url);

  auto& net = galois::runtime::getSystemNetworkInterface();

  if (net.ID == 0) {
    galois::runtime::reportParam(REGION_NAME, "Max Iterations", maxIterations);
  }

  galois::StatTimer StatTimer_total("TimerTotal", REGION_NAME.c_str());

  StatTimer_total.start();

  std::unique_ptr<Graph> hg;
  std::tie(hg, syncSubstrate) = symmetricDistGraphInitialization<NodeData, void, uint32_t>();

  hg->sortEdgesByDestination();

  bitset_comp_current.resize(hg->size());

  galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");

  InitializeGraph::go((*hg));
  galois::runtime::getHostBarrier().wait();

  galois::DGAccumulator<uint64_t> active_vertices64;

  for (auto run = 0; run < numRuns; ++run) {
    REGION_NAME_RUN = REGION_NAME + "_" + std::to_string(run);
    galois::gPrint("[", net.ID, "] ConnectedComp::go run ", run, " called\n");
    std::string timer_str("Timer_" + std::to_string(run));
    galois::StatTimer StatTimer_main(timer_str.c_str(), REGION_NAME.c_str());

    StatTimer_main.start();
    if (execution == Async) {
      ConnectedComp<true>::go(*hg);
    } else {
      ConnectedComp<false>::go(*hg);
    }
    StatTimer_main.stop();

    ConnectedCompSanityCheck::go(*hg, active_vertices64);

    if ((run + 1) != numRuns) {
      bitset_comp_current.reset();

      (*syncSubstrate).set_num_run(run + 1);
      galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");
      InitializeGraph::go((*hg));
      galois::runtime::getHostBarrier().wait();
    }
  }

  StatTimer_total.stop();

  if (output) {
    std::vector<uint32_t> results = makeResults(hg);
    auto globalIDs                = hg->getMasterGlobalIDs();
    assert(results.size() == globalIDs.size());

    writeOutput(outputLocation, "component", results.data(), results.size(),
                globalIDs.data());
  }

  return 0;
}
