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
#include "galois/gstl.h"
#include "galois/runtime/Tracer.h"

#include <iostream>
#include <limits>
#include <algorithm>

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

enum IterMode { All, Separate };

static cll::opt<IterMode> iterMode(
    "iterMode", cll::desc("Iterate Mode (default value Separate):"),
    cll::values(clEnumVal(All, "iterate through all nodes"),
                clEnumVal(Separate, "iterate through present nodes first and then phantom nodes")),
    cll::init(Separate));

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

struct NodeData {
  std::atomic<uint32_t> comp_current;
};

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph, uint32_t>> syncSubstrate;

#include "cc_pull_sync.hh"

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
    NodeData& sdata    = graph->getData(src);
    sdata.comp_current = graph->getGID(src);
  }
};

struct ConnectedCompPresent_First {
  Graph* graph;

  using DGTerminatorDetector = galois::DGAccumulator<unsigned int>;

  DGTerminatorDetector& active_vertices;

  ConnectedCompPresent_First(Graph* _graph, DGTerminatorDetector& _dga) : graph(_graph), active_vertices(_dga) {}

  void static go(Graph& _graph, DGTerminatorDetector& dga) {
      const auto& presentNodes = _graph.presentNodesRange();
      
      // launch all other threads to compute
      galois::do_all(
          galois::iterate(presentNodes), ConnectedCompPresent_First{&_graph, dga},
          galois::steal(), galois::no_stats());
  }

  // Pull from neighbor nodes, then add to self
  void operator()(GNode src) const {
    NodeData& snode = graph->getData(src);

    bool updated = false;
    for (auto jj : graph->edges(src)) {
        GNode dst         = graph->getEdgeDst(jj);
        auto& dnode       = graph->getData(dst);
        uint32_t new_comp = dnode.comp_current;
        uint32_t old_comp = galois::min(snode.comp_current, new_comp);
        if (old_comp > new_comp) {
            updated = true;
        }
    }
    
    if (updated) {
        active_vertices += 1;
    }
  }
};

struct ConnectedCompPhantom_First {
  Graph* graph;

  using DGTerminatorDetector = galois::DGAccumulator<unsigned int>;

  DGTerminatorDetector& active_vertices;

  galois::runtime::NetworkInterface& net;

  ConnectedCompPhantom_First(Graph* _graph, DGTerminatorDetector& _dga) : graph(_graph), active_vertices(_dga), net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph, DGTerminatorDetector& dga) {
      const auto& phantomNodes = _graph.phantomNodesRange();

      // launch all other threads to compute
      galois::do_all(
          galois::iterate(phantomNodes), ConnectedCompPhantom_First{&_graph, dga},
          galois::steal(), galois::no_stats());
  }

  // Pull from neighbor nodes, then add to self
  void operator()(GNode src) const {
    // source node must be phantom
    // create register for phantom node data
    uint32_t scomp = UINT32_MAX;

    for (auto jj : graph->edges(src)) {
        GNode dst         = graph->getEdgeDst(jj);
        auto& dnode       = graph->getData(dst);

        if (dnode.comp_current < scomp) {
            scomp = dnode.comp_current;
        }
    }
    
    if (scomp != UINT32_MAX) {
        active_vertices += 1;
        net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(src), graph->getPhantomRemoteLID(src), scomp);
    }
  }
};

struct ConnectedCompPresent {
  Graph* graph;

  using DGTerminatorDetector = galois::DGAccumulator<unsigned int>;

  DGTerminatorDetector& active_vertices;

  ConnectedCompPresent(Graph* _graph, DGTerminatorDetector& _dga) : graph(_graph), active_vertices(_dga) {}

  void static go(Graph& _graph, DGTerminatorDetector& dga) {
      const auto& presentNodes = _graph.presentNodesRange();
      
      // launch all other threads to compute
      galois::do_all(
          galois::iterate(presentNodes), ConnectedCompPresent{&_graph, dga},
          galois::steal(), galois::no_stats());
  }

  // Pull from neighbor nodes, then add to self
  void operator()(GNode src) const {
    NodeData& snode = graph->getData(src);

    bool updated = false;
    for (auto jj : graph->edges(src)) {
        GNode dst         = graph->getEdgeDst(jj);
        auto& dnode       = graph->getData(dst);
        uint32_t new_comp = dnode.comp_current;
        uint32_t old_comp = galois::min(snode.comp_current, new_comp);
        if (old_comp > new_comp) {
            updated = true;
        }
    }
    
    if (updated) {
        active_vertices += 1;
    }
  }
};

struct ConnectedCompPhantom {
  Graph* graph;

  using DGTerminatorDetector = galois::DGAccumulator<unsigned int>;

  DGTerminatorDetector& active_vertices;

  galois::runtime::NetworkInterface& net;

  ConnectedCompPhantom(Graph* _graph, DGTerminatorDetector& _dga) : graph(_graph), active_vertices(_dga), net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph, DGTerminatorDetector& dga) {
      const auto& phantomNodes = _graph.phantomNodesRange();

      // launch all other threads to compute
      galois::do_all(
          galois::iterate(phantomNodes), ConnectedCompPhantom{&_graph, dga},
          galois::steal(), galois::no_stats());
  }

  // Pull from neighbor nodes, then add to self
  void operator()(GNode src) const {
    // source node must be phantom
    // create register for phantom node data
    uint32_t scomp = UINT32_MAX;

    for (auto jj : graph->edges(src)) {
        GNode dst         = graph->getEdgeDst(jj);
        auto& dnode       = graph->getData(dst);

        if (dnode.comp_current < scomp) {
            scomp = dnode.comp_current;
        }
    }
    
    if (scomp != UINT32_MAX) {
        net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(src), graph->getPhantomRemoteLID(src), scomp);
    }
  }
};

struct ConnectedCompSep {
  Graph* graph;

  using DGTerminatorDetector = galois::DGAccumulator<unsigned int>;

  ConnectedCompSep(Graph* _graph) : graph(_graph) {}

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

      _net.prefetchBuffers();

      StatTimer_compute.start();
      if (_num_iterations == 0) {
          ConnectedCompPresent_First::go(_graph, dga);
          ConnectedCompPhantom_First::go(_graph, dga);
      }
      else {
          ConnectedCompPresent::go(_graph, dga);
          ConnectedCompPhantom::go(_graph, dga);
      }
      StatTimer_compute.stop();

      // inform all other hosts that this host has finished sending messages
      // force all messages to be processed before continuing
      StatTimer_flush.start();
      _net.flushRemoteWork();
      StatTimer_flush.stop();

      StatTimer_comm.start();
      syncSubstrate->poll_for_remote_work_active<Reduce_min_comp_current>(dga);
      StatTimer_comm.stop();
      
      _net.resetWorkTermination();

      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "NumWorkItems_Round_" + std::to_string(_num_iterations), (unsigned long)dga.read_local());

      ++_num_iterations;

      StatTimer_total.stop();
    } while ((_num_iterations < maxIterations) && dga.reduce(syncSubstrate->get_run_identifier()));
  }
};

struct ConnectedCompAll {
  Graph* graph;
  using DGTerminatorDetector = galois::DGAccumulator<unsigned int>;

  DGTerminatorDetector& active_vertices;
  
  galois::runtime::NetworkInterface& net;

  ConnectedCompAll(Graph* _graph, DGTerminatorDetector& _dga)
      : graph(_graph), active_vertices(_dga), net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph) {
#ifdef GALOIS_USER_STATS
    constexpr bool USER_STATS = true;
#else
    constexpr bool USER_STATS = false;
#endif

    unsigned _num_iterations = 0;

    DGTerminatorDetector dga;

    const auto& allNodes = _graph.allNodesRange();
  
    auto& _net = galois::runtime::getSystemNetworkInterface();

    do {
      std::string total_str("Total_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_total(total_str.c_str(), REGION_NAME_RUN.c_str());
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

      _net.prefetchBuffers();

      // launch all other threads to compute
      StatTimer_compute.start();
      galois::do_all(
          galois::iterate(allNodes), ConnectedCompAll(&_graph, dga),
          galois::steal(), galois::no_stats());
      StatTimer_compute.stop();

      // inform all other hosts that this host has finished sending messages
      // force all messages to be processed before continuing
      StatTimer_flush.start();
      _net.flushRemoteWork();
      StatTimer_flush.stop();

      StatTimer_comm.start();
      syncSubstrate->poll_for_remote_work_active<Reduce_min_comp_current>(dga);
      StatTimer_comm.stop();
      
      _net.resetWorkTermination();

      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "NumWorkItems_Round_" + std::to_string(_num_iterations), (unsigned long)dga.read_local());

      ++_num_iterations;

      StatTimer_total.stop();
    } while ((_num_iterations < maxIterations) && dga.reduce(syncSubstrate->get_run_identifier()));
  }

  void operator()(GNode src) const {
    // source node can be master, mirror or phantom
    if (graph->isPhantom(src)) {
        // create register for phantom node data
        uint32_t scomp = UINT32_MAX;

        for (auto jj : graph->edges(src)) {
            GNode dst         = graph->getEdgeDst(jj);
            auto& dnode       = graph->getData(dst);

            if (dnode.comp_current < scomp) {
                scomp = dnode.comp_current;
            }
        }
    
        if (scomp != UINT32_MAX) {
            net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(src), graph->getPhantomRemoteLID(src), scomp);
        }
    } else {
        NodeData& snode = graph->getData(src);

        bool updated = false;
        for (auto jj : graph->edges(src)) {
            GNode dst         = graph->getEdgeDst(jj);
            auto& dnode       = graph->getData(dst);
            uint32_t new_comp = dnode.comp_current;
            uint32_t old_comp = galois::min(snode.comp_current, new_comp);
            if (old_comp > new_comp) {
                updated = true;
            }
        }

        if (updated) {
            active_vertices += 1;
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

    galois::do_all(galois::iterate(_graph.masterNodesRange().begin(),
                                   _graph.masterNodesRange().end()),
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

constexpr static const char* const name = "ConnectedComp Pull - Distributed "
                                          "Heterogeneous";
constexpr static const char* const desc = "ConnectedComp pull on Distributed "
                                          "Galois.";
constexpr static const char* const url = nullptr;

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
  //std::tie(hg, syncSubstrate) = distGraphInitialization<NodeData, void, uint32_t, false>();

  galois::runtime::getHostBarrier().wait();
  net.forwardPass();

  galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");

  InitializeGraph::go((*hg));
  galois::runtime::getHostBarrier().wait();
  StatTimer_preprocess.stop();

  galois::DGAccumulator<uint64_t> active_vertices64;

  for (auto run = 0; run < numRuns; ++run) {
    REGION_NAME_RUN = REGION_NAME + "_" + std::to_string(run);
    galois::gPrint("[", net.ID, "] ConnectedComp::go run ", run, " called\n");
    std::string timer_str("Timer_" + std::to_string(run));
    galois::StatTimer StatTimer_main(timer_str.c_str(), REGION_NAME_RUN.c_str());

    net.touchBufferPool();
    galois::runtime::getHostBarrier().wait();

    StatTimer_main.start();
    if (iterMode == All) {
      ConnectedCompAll::go(*hg);
    } else {
      ConnectedCompSep::go(*hg);
    }
    StatTimer_main.stop();
    galois::gPrint("Host ", net.ID, " ConnectedComp run ", run, " time: ", StatTimer_main.get(), " ms\n");

    ConnectedCompSanityCheck::go(*hg, active_vertices64);

    if ((run + 1) != numRuns) {
      (*syncSubstrate).set_num_run(run + 1);
      InitializeGraph::go((*hg));
    }
  }

  StatTimer_total.stop();

  net.applicationDone();

  if (output) {
    std::vector<uint32_t> results = makeResults(hg);
    auto globalIDs                = hg->getMasterGlobalIDs();
    assert(results.size() == globalIDs.size());

    writeOutput(outputLocation, "component", results.data(), results.size(),
                globalIDs.data());
  }

  return 0;
}
