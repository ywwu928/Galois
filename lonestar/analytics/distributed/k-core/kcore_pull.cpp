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

enum IterMode { All, Separate };

static cll::opt<IterMode> iterMode(
    "iterMode", cll::desc("Iterate Mode (default value Separate):"),
    cll::values(clEnumVal(All, "iterate through all nodes"),
                clEnumVal(Separate, "iterate through present nodes first and then phantom nodes")),
    cll::init(Separate));

/******************************************************************************/
/* Graph structure declarations + other inits */
/******************************************************************************/

struct NodeData {
  std::atomic<uint32_t> current_degree;
  std::atomic<uint32_t> trim;
  uint8_t flag;
  uint8_t pull_flag;
};

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph, uint32_t>> syncSubstrate;

#include "kcore_pull_sync.hh"

/******************************************************************************/
/* Functors for running the algorithm */
/******************************************************************************/

/* Degree counting
 * Called by InitializeGraph1 */
struct DegreeCounting {
  Graph* graph;

  DegreeCounting(Graph* _graph) : graph(_graph) {}

  /* Initialize the entire graph node-by-node */
  void static go(Graph& _graph) {
    const auto& allNodes = _graph.allNodesRange();

    galois::do_all(
        galois::iterate(allNodes), DegreeCounting{&_graph},
        galois::steal(), galois::no_stats());
  }

  /* Calculate degree of nodes by checking how many nodes have it as a dest and
   * adding for every dest (works same way in pull version since it's a
   * symmetric graph) */
  void operator()(GNode src) const {
    for (auto current_edge : graph->edges(src)) {
      GNode dst   = graph->getEdgeDst(current_edge);
      auto& ddata = graph->getData(dst);
      galois::atomicAdd(ddata.current_degree, (uint32_t)1);
    }
  }
};

/* Initialize: initial field setup */
struct InitializeGraph {
  Graph* graph;

  InitializeGraph(Graph* _graph) : graph(_graph) {}

  /* Initialize the entire graph node-by-node */
  void static go(Graph& _graph) {
    const auto& presentNodes = _graph.presentNodesRange();
    
    galois::do_all(
        galois::iterate(presentNodes.begin(), presentNodes.end()),
        InitializeGraph{&_graph}, galois::no_stats());

    // degree calculation
    DegreeCounting::go(_graph);
  }

  /* Setup intial fields */
  void operator()(GNode src) const {
    NodeData& src_data      = graph->getData(src);
    src_data.flag           = true;
    src_data.trim           = 0;
    src_data.current_degree = 0;
    src_data.pull_flag      = false;
  }
};

/* Updates liveness of a node + updates flag that says if node has been pulled
 * from */
struct LiveUpdate {
  cll::opt<uint32_t>& local_k_core_num;
  Graph* graph;

  using DGTerminatorDetector = galois::DGAccumulator<unsigned int>;

  DGTerminatorDetector& active_vertices;

  LiveUpdate(cll::opt<uint32_t>& _kcore, Graph* _graph,
             DGTerminatorDetector& _dga)
      : local_k_core_num(_kcore), graph(_graph), active_vertices(_dga) {}

  void static go(Graph& _graph, DGTerminatorDetector& dga) {
    const auto& presentNodes = _graph.presentNodesRange();
    dga.reset();

    galois::do_all(
        galois::iterate(presentNodes.begin(), presentNodes.end()),
        LiveUpdate{k_core_num, &_graph, dga}, galois::no_stats());
  }

  /**
   * Mark a node dead if degree is under kcore number and mark it
   * available for pulling from.
   *
   * If dead, and pull flag is on, then turn off flag as you don't want to
   * be pulled from more than once.
   */
  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);

    if (sdata.flag) {
      if (sdata.trim > 0) {
        sdata.current_degree = sdata.current_degree - sdata.trim;
      }

      if (sdata.current_degree < local_k_core_num) {
        sdata.flag = false;
        active_vertices += 1;

        // let neighbors pull from me next round
        // assert(sdata.pull_flag == false);
        sdata.pull_flag = true;
      }
    } else {
      // dead
      if (sdata.pull_flag) {
        // do not allow neighbors to pull value from this node anymore
        sdata.pull_flag = false;
      }
    }

    // always reset trim
    sdata.trim = 0;
  }
};

/* Step that determines if a node is dead and updates its neighbors' trim
 * if it is */
struct KCorePresent {
  Graph* graph;

  KCorePresent(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
    const auto& presentNodes = _graph.presentNodesRange();
    galois::do_all(galois::iterate(presentNodes), KCorePresent{&_graph},
                   galois::no_stats(), galois::steal());
  }

  void operator()(GNode src) const {
    NodeData& src_data = graph->getData(src);

    // only if node is alive we do things
    if (src_data.flag) {
      // if dst node is dead, increment trim by one so we can decrement
      // our degree later
      for (auto current_edge : graph->edges(src)) {
        GNode dst          = graph->getEdgeDst(current_edge);
        NodeData& dst_data = graph->getData(dst);

        if (dst_data.pull_flag) {
          galois::add(src_data.trim, (uint32_t)1);
        }
      }
    }
  }
};

/* Step that determines if a node is dead and updates its neighbors' trim
 * if it is */
struct KCorePhantom {
  Graph* graph;

  galois::runtime::NetworkInterface& net;

  KCorePhantom(Graph* _graph) : graph(_graph), net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph) {
    const auto& phantomNodes = _graph.phantomNodesRange();
    galois::do_all(galois::iterate(phantomNodes), KCorePhantom{&_graph},
                   galois::no_stats(), galois::steal());
  }

  void operator()(GNode src) const {
    // if dst node is dead, increment trim by one so we can decrement our degree later
    uint32_t strim = 0;

    for (auto current_edge : graph->edges(src)) {
        GNode dst          = graph->getEdgeDst(current_edge);
        NodeData& dst_data = graph->getData(dst);

        if (dst_data.pull_flag) {
            strim += 1;
        }
    }
    
    if (strim != 0) {
        net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(src), graph->getPhantomRemoteLID(src), strim);
    }
  }
};

/* Step that determines if a node is dead and updates its neighbors' trim
 * if it is */
struct KCoreSep {
  Graph* graph;

  using DGTerminatorDetector = galois::DGAccumulator<unsigned int>;

  KCoreSep(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
#ifdef GALOIS_USER_STATS
    constexpr bool USER_STATS = true;
#else
    constexpr bool USER_STATS = false;
#endif

    unsigned _num_iterations = 0;

    DGTerminatorDetector dga;
  
    auto& _net = galois::runtime::getSystemNetworkInterface();

    do {
      std::string total_str("Total_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_total(total_str.c_str(), REGION_NAME_RUN.c_str());
      std::string compute_str("Compute_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_compute(compute_str.c_str(), REGION_NAME_RUN.c_str());
      std::string flush_str("Flush_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_flush(flush_str.c_str(), REGION_NAME_RUN.c_str());
      std::string live_str("LiveUpdate_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_live(live_str.c_str(), REGION_NAME_RUN.c_str());
      std::string comm_str("Communication_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_comm(comm_str.c_str(), REGION_NAME_RUN.c_str());

#ifdef GALOIS_PRINT_PROCESS
      galois::gPrint("Host ", _net.ID, " : iteration ", _num_iterations, "\n");
#endif

      StatTimer_total.start();
      syncSubstrate->set_num_round(_num_iterations);

      _net.prefetchBuffers();

      StatTimer_compute.start();
      KCorePresent::go(_graph);
      KCorePhantom::go(_graph);
      StatTimer_compute.stop();
      
      // inform all other hosts that this host has finished sending messages
      // force all messages to be processed before continuing
      StatTimer_flush.start();
      _net.flushRemoteWork();
      StatTimer_flush.stop();

      StatTimer_comm.start();
      syncSubstrate->poll_for_remote_work<Reduce_add_trim>();
      StatTimer_comm.stop();

      // update live/deadness
      StatTimer_live.start();
      LiveUpdate::go(_graph, dga);
      StatTimer_live.stop();
      
      _net.resetWorkTermination();

      _num_iterations++;

      StatTimer_total.stop();
    } while ((_num_iterations < maxIterations) && dga.reduce(syncSubstrate->get_run_identifier()));
  }
};

struct KCoreAll {
  Graph* graph;
  using DGTerminatorDetector = galois::DGAccumulator<unsigned int>;
  
  galois::runtime::NetworkInterface& net;

  KCoreAll(Graph* _graph) : graph(_graph), net(galois::runtime::getSystemNetworkInterface()) {}

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
      std::string live_str("LiveUpdate_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_live(live_str.c_str(), REGION_NAME_RUN.c_str());
      std::string comm_str("Communication_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_comm(comm_str.c_str(), REGION_NAME_RUN.c_str());

#ifdef GALOIS_PRINT_PROCESS
      galois::gPrint("Host ", _net.ID, " : iteration ", _num_iterations, "\n");
#endif

      StatTimer_total.start();
      syncSubstrate->set_num_round(_num_iterations);

      _net.prefetchBuffers();

      // launch all other threads to compute
      StatTimer_compute.start();
      galois::do_all(galois::iterate(allNodes), KCoreAll{&_graph},
                     galois::no_stats(), galois::steal());
      StatTimer_compute.stop();

      // inform all other hosts that this host has finished sending messages
      // force all messages to be processed before continuing
      StatTimer_flush.start();
      _net.flushRemoteWork();
      StatTimer_flush.stop();

      StatTimer_comm.start();
      syncSubstrate->poll_for_remote_work<Reduce_add_trim>();
      StatTimer_comm.stop();

      // update live/deadness
      StatTimer_live.start();
      LiveUpdate::go(_graph, dga);
      StatTimer_live.stop();
      
      _net.resetWorkTermination();

      _num_iterations++;

      StatTimer_total.stop();
    } while ((_num_iterations < maxIterations) && dga.reduce(syncSubstrate->get_run_identifier()));
  }

  void operator()(GNode src) const {
    // source node can be master, mirror or phantom
    if (graph->isPhantom(src)) {
        // create register for phantom node data
        uint32_t strim = 0;

        for (auto current_edge : graph->edges(src)) {
            GNode dst          = graph->getEdgeDst(current_edge);
            NodeData& dst_data = graph->getData(dst);

            if (dst_data.pull_flag) {
                strim += 1;
            }
        }
        
        if (strim != 0) {
            net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(src), graph->getPhantomRemoteLID(src), strim);
        }
    } else {
        NodeData& src_data = graph->getData(src);

        // only if node is alive we do things
        if (src_data.flag) {
            // if dst node is dead, increment trim by one so we can decrement
            // our degree later
            for (auto current_edge : graph->edges(src)) {
                GNode dst          = graph->getEdgeDst(current_edge);
                NodeData& dst_data = graph->getData(dst);

                if (dst_data.pull_flag) {
                    galois::add(src_data.trim, (uint32_t)1);
                }
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
                                          "Pull Topological.";
constexpr static const char* const desc = "KCore on Distributed Galois.";
constexpr static const char* const url  = nullptr;

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

  std::unique_ptr<Graph> h_graph;
  std::tie(h_graph, syncSubstrate) = symmetricDistGraphInitialization<NodeData, void, uint32_t>();

  net.allocateBufferPool();

  galois::runtime::getHostBarrier().wait();
  net.forwardPass();

  galois::gPrint("[", net.ID, "] InitializeGraph::go functions called\n");

  InitializeGraph::go((*h_graph));
  galois::runtime::getHostBarrier().wait();
  StatTimer_preprocess.stop();

  galois::DGAccumulator<uint64_t> dga;

  for (auto run = 0; run < numRuns; ++run) {
    REGION_NAME_RUN = REGION_NAME + "_" + std::to_string(run);
    galois::gPrint("[", net.ID, "] KCore::go run ", run, " called\n");
    std::string timer_str("Timer_" + std::to_string(run));
    galois::StatTimer StatTimer_main(timer_str.c_str(), REGION_NAME_RUN.c_str());

    net.touchBufferPool();
    galois::runtime::getHostBarrier().wait();
    
    StatTimer_main.start();
    if (iterMode == All) {
      KCoreAll::go(*h_graph);
    } else {
      KCoreSep::go(*h_graph);
    }
    StatTimer_main.stop();
    galois::gPrint("Host ", net.ID, " KCore run ", run, " time: ", StatTimer_main.get(), " ms\n");

    // sanity check
    KCoreSanityCheck::go(*h_graph, dga);

    // re-init graph for next run
    if ((run + 1) != numRuns) {
      (*syncSubstrate).set_num_run(run + 1);
      InitializeGraph::go(*h_graph);
    }
  }

  StatTimer_total.stop();

  net.applicationDone();

  if (output) {
    std::vector<unsigned> results = makeResults(h_graph);
    auto globalIDs                = h_graph->getMasterGlobalIDs();
    assert(results.size() == globalIDs.size());

    writeOutput(outputLocation, "in_kcore", results.data(), results.size(),
                globalIDs.data());
  }

  return 0;
}
