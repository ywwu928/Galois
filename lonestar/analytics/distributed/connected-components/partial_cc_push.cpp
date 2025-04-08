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
          InitializeGraph{&_graph}, galois::no_stats());
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    sdata.comp_current = graph->getGID(src);
    sdata.comp_old = graph->getGID(src);
  }
};

struct FirstItr_ConnectedComp {
  Graph* graph;

  galois::runtime::NetworkInterface& net;
  
  FirstItr_ConnectedComp(Graph* _graph) : graph(_graph), net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph) {
#ifdef GALOIS_USER_STATS
    constexpr bool USER_STATS = true;
#else
    constexpr bool USER_STATS = false;
#endif

#ifndef GALOIS_FULL_MIRRORING     
    const auto& masterNodes = _graph.masterNodesRangeReserved();
#else
    const auto& masterNodes = _graph.masterNodesRange();
#endif
    
#ifdef GALOIS_PRINT_PROCESS
    auto& _net = galois::runtime::getSystemNetworkInterface();
#endif
     
    std::string total_str("Total_Round_0");
    galois::CondStatTimer<USER_STATS> StatTimer_total(total_str.c_str(), REGION_NAME_RUN.c_str());
    std::string reset_buf_str("ResetBuf_Round_0");
    galois::CondStatTimer<USER_STATS> StatTimer_reset_buf(reset_buf_str.c_str(), REGION_NAME_RUN.c_str());
    std::string compute_str("Compute_Round_0");
    galois::CondStatTimer<USER_STATS> StatTimer_compute(compute_str.c_str(), REGION_NAME_RUN.c_str());
    std::string sync_buf_str("SyncBuf_Round_0");
    galois::CondStatTimer<USER_STATS> StatTimer_sync_buf(sync_buf_str.c_str(), REGION_NAME_RUN.c_str());
    std::string comm_str("Communication_Round_0");
      galois::CondStatTimer<USER_STATS> StatTimer_comm(comm_str.c_str(), REGION_NAME_RUN.c_str());
    
#ifdef GALOIS_PRINT_PROCESS
      galois::gPrint("Host ", _net.ID, " : iteration 0\n");
#endif

    StatTimer_total.start();
    syncSubstrate->set_num_round(0);
      
#ifndef GALOIS_FULL_MIRRORING     
      StatTimer_reset_buf.start();
      syncSubstrate->set_update_buf_to_identity(UINT32_MAX);
      StatTimer_reset_buf.stop();
      // dedicate a thread to poll for remote messages
      std::function<void(void)> func = [&]() {
              syncSubstrate->poll_for_remote_work_dedicated<Reduce_min_comp_current>();
      };
      galois::substrate::getThreadPool().runDedicated(func);
#endif
    
    // launch all other threads to compute
    StatTimer_compute.start();
    galois::do_all(
        galois::iterate(masterNodes), FirstItr_ConnectedComp{&_graph},
        galois::steal(), galois::no_stats(),
        galois::loopname(syncSubstrate->get_run_identifier("ConnectedComp").c_str()));
    StatTimer_compute.stop();

#ifndef GALOIS_FULL_MIRRORING     
    // inform all other hosts that this host has finished sending messages
    // force all messages to be processed before continuing
    syncSubstrate->net_flush();

    galois::substrate::getThreadPool().waitDedicated();

    StatTimer_sync_buf.start();
    syncSubstrate->sync_update_buf<Reduce_min_comp_current>(UINT32_MAX);
    StatTimer_sync_buf.stop();
#endif

    StatTimer_comm.start();
#ifdef GALOIS_NO_MIRRORING     
    syncSubstrate->poll_for_remote_work<Reduce_min_comp_current>();
#else
    syncSubstrate->sync<writeDestination, readSource, Reduce_min_comp_current, Bitset_comp_current>("ConnectedComp");
#endif
    StatTimer_comm.stop();
      
    syncSubstrate->reset_termination();

    galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "NumWorkItems_Round_0", _graph.masterNodesRange().end() - _graph.masterNodesRange().begin());
    
    StatTimer_total.stop();
  }

  void operator()(GNode src) const {
    NodeData& snode = graph->getData(src);
    snode.comp_old  = snode.comp_current;

    for (auto jj : graph->edges(src)) {
        GNode dst         = graph->getEdgeDst(jj);
#ifndef GALOIS_FULL_MIRRORING     
        if (graph->isPhantom(dst)) {
            uint32_t new_dist = snode.comp_current;
            //uint32_t& hostID = graph->getHostIDForLocal(dst);
            //uint32_t& remoteLID = graph->getPhantomRemoteLID(dst);
            //unsigned tid = galois::substrate::ThreadPool::getTID();
            net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getPhantomRemoteLID(dst), new_dist);
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

struct ConnectedComp {
  Graph* graph;
  using DGTerminatorDetector = galois::DGAccumulator<unsigned int>;

  DGTerminatorDetector& active_vertices;

  galois::runtime::NetworkInterface& net;

  ConnectedComp(Graph* _graph, DGTerminatorDetector& _dga)
      : graph(_graph), active_vertices(_dga), net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph) {
    using namespace galois::worklists;

#ifdef GALOIS_USER_STATS
    constexpr bool USER_STATS = true;
#else
    constexpr bool USER_STATS = false;
#endif

    FirstItr_ConnectedComp::go(_graph);
    galois::runtime::getHostBarrier().wait();

    unsigned _num_iterations = 1;
    
    DGTerminatorDetector dga;

#ifndef GALOIS_FULL_MIRRORING     
    const auto& masterNodes = _graph.masterNodesRangeReserved();
#else
    const auto& masterNodes = _graph.masterNodesRange();
#endif
  
#ifdef GALOIS_PRINT_PROCESS
    auto& _net = galois::runtime::getSystemNetworkInterface();
#endif

    do {
      std::string total_str("Total_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_total(total_str.c_str(), REGION_NAME_RUN.c_str());
      std::string reset_buf_str("ResetBuf_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_reset_buf(reset_buf_str.c_str(), REGION_NAME_RUN.c_str());
      std::string compute_str("Compute_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_compute(compute_str.c_str(), REGION_NAME_RUN.c_str());
      std::string sync_buf_str("SyncBuf_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_sync_buf(sync_buf_str.c_str(), REGION_NAME_RUN.c_str());
      std::string comm_str("Communication_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_comm(comm_str.c_str(), REGION_NAME_RUN.c_str());

#ifdef GALOIS_PRINT_PROCESS
      galois::gPrint("Host ", _net.ID, " : iteration ", _num_iterations, "\n");
#endif

      StatTimer_total.start();
      syncSubstrate->set_num_round(_num_iterations);

      dga.reset();
      
#ifndef GALOIS_FULL_MIRRORING     
      StatTimer_reset_buf.start();
      syncSubstrate->set_update_buf_to_identity(UINT32_MAX);
      StatTimer_reset_buf.stop();
      // dedicate a thread to poll for remote messages
      std::function<void(void)> func = [&]() {
              syncSubstrate->poll_for_remote_work_dedicated<Reduce_min_comp_current>();
      };
      galois::substrate::getThreadPool().runDedicated(func);
#endif
      
      // launch all other threads to compute
      StatTimer_compute.start();
      galois::do_all(
          galois::iterate(masterNodes), ConnectedComp(&_graph, dga),
          galois::no_stats(), galois::steal(),
          galois::loopname(syncSubstrate->get_run_identifier("ConnectedComp").c_str()));
      StatTimer_compute.stop();

#ifndef GALOIS_FULL_MIRRORING     
      // inform all other hosts that this host has finished sending messages
      // force all messages to be processed before continuing
      syncSubstrate->net_flush();

      galois::substrate::getThreadPool().waitDedicated();

      StatTimer_sync_buf.start();
      syncSubstrate->sync_update_buf<Reduce_min_comp_current>(UINT32_MAX);
      StatTimer_sync_buf.stop();
#endif

      StatTimer_comm.start();
#ifdef GALOIS_NO_MIRRORING     
      syncSubstrate->poll_for_remote_work<Reduce_min_comp_current>();
#else
      syncSubstrate->sync<writeDestination, readSource, Reduce_min_comp_current, Bitset_comp_current>("ConnectedComp");
#endif
      StatTimer_comm.stop();
      
      syncSubstrate->reset_termination();

      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "NumWorkItems_Round_" + std::to_string(_num_iterations), (unsigned long)dga.read_local());

      ++_num_iterations;
      
      StatTimer_total.stop();
    } while ((_num_iterations < maxIterations) && dga.reduce(syncSubstrate->get_run_identifier()));
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
            //uint32_t& hostID = graph->getHostIDForLocal(dst);
            //uint32_t& remoteLID = graph->getPhantomRemoteLID(dst);
            //unsigned tid = galois::substrate::ThreadPool::getTID();
            net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getPhantomRemoteLID(dst), new_dist);
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
                   ConnectedCompSanityCheck(&_graph, dga), galois::no_stats());

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
  galois::StatTimer StatTimer_preprocess("TimerPreProcess", REGION_NAME.c_str());
  StatTimer_preprocess.start();

  std::unique_ptr<Graph> hg;
  std::tie(hg, syncSubstrate) = symmetricDistGraphInitialization<NodeData, void, uint32_t>();

  hg->sortEdgesByDestination();

  bitset_comp_current.resize(hg->size());

  galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");

  InitializeGraph::go((*hg));
  galois::runtime::getHostBarrier().wait();
  StatTimer_preprocess.stop();

  galois::DGAccumulator<uint64_t> active_vertices64;

  for (auto run = 0; run < numRuns; ++run) {
    REGION_NAME_RUN = REGION_NAME + "_" + std::to_string(run);
    galois::gPrint("[", net.ID, "] ConnectedComp::go run ", run, " called\n");
    std::string timer_str("Timer_" + std::to_string(run));
    galois::StatTimer StatTimer_main(timer_str.c_str(), REGION_NAME.c_str());

    StatTimer_main.start();
    ConnectedComp::go(*hg);
    StatTimer_main.stop();
    galois::gPrint("Host ", net.ID, " ConnectedComp run ", run, " time: ", StatTimer_main.get(), " ms\n");

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
