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

/******************************************************************************/
/* Graph structure declarations + other inits */
/******************************************************************************/

struct NodeData {
  uint32_t current_degree;
  std::atomic<uint32_t> trim;
};

galois::DynamicBitSet bitset_exclude;
galois::DynamicBitSet bitset_active;
galois::DynamicBitSet bitset_trim;

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph, uint32_t>> syncSubstrate;

#include "kcore_sync.hh"

/******************************************************************************/
/* Functors for running the algorithm */
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
    NodeData& sdata      = graph->getData(src);
    sdata.current_degree = std::distance(graph->edge_begin(src), graph->edge_end(src));
    sdata.trim           = 0;
  }
};

struct KCore_trim {
  Graph* graph;

  KCore_trim(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
    const auto& masterNodes = _graph.masterNodesRange();
    
    galois::do_all(
        galois::iterate(masterNodes),
        KCore_trim{&_graph}, galois::no_stats());
  }

  void operator()(GNode src) const {
    if (!bitset_exclude.test(src)) {
        NodeData& sdata = graph->getData(src);

        if (bitset_trim.test(src)) {
            sdata.current_degree = sdata.current_degree - sdata.trim;
            sdata.trim = 0;
        }

        if (sdata.current_degree < k_core_num) {
            bitset_exclude.set(src);
            bitset_active.set(src);
        }
    }
  }
};

struct KCore {
  Graph* graph;

  galois::runtime::NetworkInterface& net;

  KCore(Graph* _graph)
      : graph(_graph), net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph) {
#ifdef GALOIS_USER_STATS
    constexpr bool USER_STATS = true;
#else
    constexpr bool USER_STATS = false;
#endif

    unsigned _num_iterations   = 0;

    const auto& masterNodes = _graph.masterNodesRange();
  
    auto& _net = galois::runtime::getSystemNetworkInterface();
    
    KCore_trim::go(_graph);

    uint64_t local_active_vertices = bitset_active.count();
    uint64_t global_active_vertices;

    do {
      std::string total_str("Total_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_total(total_str.c_str(), REGION_NAME_RUN.c_str());
      std::string compute_str("Compute_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_compute(compute_str.c_str(), REGION_NAME_RUN.c_str());
      std::string comm_str("Communication_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_comm(comm_str.c_str(), REGION_NAME_RUN.c_str());
      std::string reset_str("Reset_Mirror_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_reset(reset_str.c_str(), REGION_NAME_RUN.c_str());
      std::string trim_str("Trim_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_trim(trim_str.c_str(), REGION_NAME_RUN.c_str());
      std::string active_str("Active_Reduce_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_active(active_str.c_str(), REGION_NAME_RUN.c_str());

#ifdef GALOIS_PRINT_PROCESS
      galois::gPrint("Host ", _net.ID, " : iteration ", _num_iterations, "\n");
#endif

      syncSubstrate->set_num_round(_num_iterations);

      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "NumWorkItems_Round_" + std::to_string(_num_iterations), local_active_vertices);

      StatTimer_total.start();
      bitset_trim.reset();

      _net.prefetchBuffers();

      StatTimer_compute.start();
      galois::do_all(galois::iterate(masterNodes),
                     KCore{&_graph}, galois::steal(),
                     galois::no_stats());
      _net.flushRemoteWork();
      StatTimer_compute.stop();

      StatTimer_comm.start();
      syncSubstrate->reduce<Reduce_add_trim, Bitset_trim>();
      StatTimer_comm.stop();
      
      _net.resetWorkTermination();
      
      StatTimer_reset.start();
      syncSubstrate->reset_mirrorField<Reduce_add_trim>();
      StatTimer_reset.stop();
      
      bitset_active.reset();

      StatTimer_trim.start();
      KCore_trim::go(_graph);
      StatTimer_trim.stop();
      
      local_active_vertices = bitset_active.count();

      _num_iterations++;
      
      StatTimer_active.start();
      global_active_vertices = 0;
      MPI_Allreduce(&local_active_vertices, &global_active_vertices, 1,
                    MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);
      StatTimer_active.stop();
      
      StatTimer_total.stop();
    } while ((_num_iterations < maxIterations) && global_active_vertices);
  }

  void operator()(GNode src) const {
    if (bitset_active.test(src)) {
        for (auto current_edge : graph->outEdges(src)) {
            GNode dst = graph->getOutEdgeDst(current_edge);
#ifndef GALOIS_FULL_MIRRORING
            if (graph->isPhantom(dst)) {
                net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getRemoteLID(dst), (uint32_t)1);
            }
            else {
#endif
                if (!bitset_exclude.test(dst)) {
                    auto& ddata = graph->getData(dst);
                    galois::atomicAddVoid(ddata.trim, (uint32_t)1);
                    bitset_trim.set(dst);
                }
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

/* Gets the total number of nodes that are still alive */
struct KCoreSanityCheck {
  void static go(Graph& _graph, galois::DGAccumulator<uint64_t>& dga) {
    dga.reset();

    uint64_t local_num_nodes = _graph.numMasters() - bitset_exclude.count();
    dga += local_num_nodes;

    uint64_t global_num_nodes = dga.reduce();

    // Only node 0 will print data
    if (galois::runtime::getSystemNetworkInterface().ID == 0) {
      galois::gPrint("Number of nodes in the ", k_core_num, "-core is ", global_num_nodes, "\n");
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
      if (bitset_exclude.test(node)) {
          values.push_back(0);
      }
      else {
          values.push_back(1);
      }
  }

  return values;
}

/******************************************************************************/
/* Main method for running */
/******************************************************************************/

constexpr static const char* const name = "Distributed KCore Extraction (Push)";
constexpr static const char* const desc = "Distributed KCore Extraction (Push)";
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

  std::unique_ptr<Graph> hg;
  std::tie(hg, syncSubstrate) = symmetricDistGraphInitialization<NodeData, void, uint32_t>();

  net.allocateBufferPool();
  
  hg->sortEdgesByDestination();

  galois::runtime::getHostBarrier().wait();
  net.partitionDone();

  bitset_exclude.resize(hg->actualSize());
  bitset_active.resize(hg->numMasters());
  bitset_trim.resize(hg->actualSize());

  galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");
  
  InitializeGraph::go((*hg));
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
    KCore::go(*hg);
    StatTimer_main.stop();
    galois::gPrint("Host ", net.ID, " KCore run ", run, " time: ", StatTimer_main.get(), " ms\n");

    KCoreSanityCheck::go(*hg, dga);

    if ((run + 1) != numRuns) {
      bitset_exclude.reset();
      bitset_active.reset();
      bitset_trim.reset();

      (*syncSubstrate).set_num_run(run + 1);
      InitializeGraph::go(*hg);
    }
  }

  StatTimer_total.stop();
  
  net.applicationDone();

  if (output) {
    std::vector<unsigned> results = makeResults(hg);
    auto globalIDs                = hg->getMasterGlobalIDs();
    assert(results.size() == globalIDs.size());

    writeOutput(outputLocation, "in_kcore", results.data(), results.size(),
                globalIDs.data());
  }

  return 0;
}
