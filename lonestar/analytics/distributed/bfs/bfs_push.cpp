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

#include <iostream>
#include <sstream>
#include <limits>

#ifdef GALOIS_ENABLE_GPU
#include "bfs_push_cuda.h"
struct CUDA_Context* cuda_ctx;
#else
enum { CPU, GPU_CUDA };
int personality = CPU;
#endif

constexpr static const char* const REGION_NAME = "BFS";

/******************************************************************************/
/* Declaration of command line arguments */
/******************************************************************************/

namespace cll = llvm::cl;

static cll::opt<unsigned int> maxIterations("maxIterations",
                                            cll::desc("Maximum iterations: "
                                                      "Default 1000"),
                                            cll::init(1000));

// static cll::opt<uint64_t> src_node("startNode", cll::desc("ID of the source node"), cll::init(0));
static cll::list<uint64_t> src_node("startNode", cll::desc("ID of the source nodes"), cll::OneOrMore);

static cll::opt<uint32_t>
    delta("delta",
          cll::desc("Shift value for the delta step (default value 0)"),
          cll::init(0));

enum Exec { Sync, Async };

static cll::opt<Exec> execution(
    "exec", cll::desc("Distributed Execution Model (default value Async):"),
    cll::values(clEnumVal(Sync, "Bulk-synchronous Parallel (BSP)"),
                clEnumVal(Async, "Bulk-asynchronous Parallel (BASP)")),
    cll::init(Async));

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

const uint32_t infinity = std::numeric_limits<uint32_t>::max() / 4;

struct NodeData {
  std::atomic<uint32_t> dist_current;
  uint32_t dist_old;
};

galois::DynamicBitSet bitset_dist_current;

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph>> syncSubstrate;

#include "bfs_push_sync.hh"

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/
struct InitializeGraph {
  const uint32_t& local_infinity;
  const uint64_t& local_src_node;
  Graph* graph;

  InitializeGraph(const uint64_t& _src_node, const uint32_t& _infinity,
                  Graph* _graph)
      : local_infinity(_infinity), local_src_node(_src_node), graph(_graph) {}

  void static go(Graph& _graph, const uint64_t& start_node) {
    const auto& allNodes = _graph.allNodesRange();

    if (personality == GPU_CUDA) {
#ifdef GALOIS_ENABLE_GPU
      std::string impl_str(
          syncSubstrate->get_run_identifier("InitializeGraph_"));
      galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
      StatTimer_cuda.start();
      InitializeGraph_allNodes_cuda(infinity, start_node, cuda_ctx);
      StatTimer_cuda.stop();
#else
      abort();
#endif
    } else if (personality == CPU) {
      galois::do_all(
          galois::iterate(allNodes.begin(), allNodes.end()),
          InitializeGraph{start_node, infinity, &_graph}, galois::no_stats(),
          galois::loopname(
              syncSubstrate->get_run_identifier("InitializeGraph").c_str()));
    }
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
	uint64_t src_GID = graph->getGID(src);
    sdata.dist_current = (src_GID == local_src_node) ? 0 : local_infinity;
    sdata.dist_old = (src_GID == local_src_node) ? 0 : local_infinity;
  }
};

template <bool async>
struct FirstItr_BFS {
  Graph* graph;
  
  FirstItr_BFS(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph,
                  const uint64_t& start_node) {
    uint32_t __begin, __end;
    if (_graph.isLocal(start_node)) {
      __begin = _graph.getLID(start_node);
      __end   = __begin + 1;
    } else {
      __begin = 0;
      __end   = 0;
    }
    syncSubstrate->set_num_round(0);

    if (personality == GPU_CUDA) {
#ifdef GALOIS_ENABLE_GPU
      std::string impl_str(syncSubstrate->get_run_identifier("BFS"));
      galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
      StatTimer_cuda.start();
      FirstItr_BFS_cuda(__begin, __end, cuda_ctx);
      StatTimer_cuda.stop();
#else
      abort();
#endif
    } else if (personality == CPU) {
      // one node
      galois::do_all(
          galois::iterate(__begin, __end), 
          FirstItr_BFS{&_graph},
          galois::no_stats(),
          galois::loopname(syncSubstrate->get_run_identifier("BFS").c_str()));
    }

	syncSubstrate->sync<writeDestination, readSource, Reduce_min_dist_current, Bitset_dist_current, async>("BFS");
      
    // just a barrier to synchronize output
    // master_round.reduce();
    
    galois::runtime::reportStat_Tsum(
        REGION_NAME, syncSubstrate->get_run_identifier("NumWorkItems"),
        __end - __begin);
  }

  void operator()(GNode src) const {
    NodeData& snode = graph->getData(src);
    snode.dist_old  = snode.dist_current;

    for (auto jj : graph->edges(src)) {
      GNode dst         = graph->getEdgeDst(jj);
      auto& dnode       = graph->getData(dst);

      uint32_t new_dist = 1 + snode.dist_current;
      uint32_t old_dist = galois::atomicMin(dnode.dist_current, new_dist);
      if (old_dist > new_dist) {
        bitset_dist_current.set(dst);
      }
    }
  }
};

template <bool async>
struct BFS {
  uint32_t local_priority;
  Graph* graph;
  using DGTerminatorDetector =
      typename std::conditional<async, galois::DGTerminator<unsigned int>,
                                galois::DGAccumulator<unsigned int>>::type;
  using DGAccumulatorTy = galois::DGAccumulator<unsigned int>;

  DGTerminatorDetector& active_vertices;
  DGAccumulatorTy& work_edges;
  
  BFS(uint32_t _local_priority,
      Graph* _graph,
      DGTerminatorDetector& _dga,
      DGAccumulatorTy& _work_edges)
      : local_priority(_local_priority),
      graph(_graph),
      active_vertices(_dga),
      work_edges(_work_edges) {}

  void static go(Graph& _graph,
                  const uint64_t& start_node) {

    FirstItr_BFS<async>::go(_graph,
                            start_node);

    unsigned _num_iterations = 1;

    const auto& nodesWithEdges = _graph.allNodesWithEdgesRange();

    uint32_t priority;
    if (delta == 0)
      priority = std::numeric_limits<uint32_t>::max();
    else
      priority = 0;
    DGTerminatorDetector dga;
    DGAccumulatorTy work_edges;
    
    do {

      priority += delta;

      syncSubstrate->set_num_round(_num_iterations);
      dga.reset();
      work_edges.reset();
    
      if (personality == GPU_CUDA) {
#ifdef GALOIS_ENABLE_GPU
        std::string impl_str(syncSubstrate->get_run_identifier("BFS"));
        galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
        StatTimer_cuda.start();
        unsigned int __retval  = 0;
        unsigned int __retval2 = 0;
        BFS_nodesWithEdges_cuda(__retval, __retval2, priority, cuda_ctx);
        dga += __retval;
        work_edges += __retval2;
        StatTimer_cuda.stop();
#else
        abort();
#endif
      } else if (personality == CPU) {
        galois::do_all(
            galois::iterate(nodesWithEdges),
            BFS(priority, 
                &_graph, 
                dga, 
                work_edges), 
            galois::steal(),
            galois::no_stats(),
            galois::loopname(syncSubstrate->get_run_identifier("BFS").c_str()));
      }

      syncSubstrate->sync<writeDestination, readSource, Reduce_min_dist_current, Bitset_dist_current, async>("BFS");
      
      galois::runtime::reportStat_Tsum(
          REGION_NAME, syncSubstrate->get_run_identifier("NumWorkItems"),
          (unsigned long)work_edges.read_local());

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

    if (snode.dist_old > snode.dist_current) {
      active_vertices += 1;

      if (local_priority > snode.dist_current) {
        snode.dist_old = snode.dist_current;

        for (auto jj : graph->edges(src)) {
		  work_edges += 1;

          GNode dst         = graph->getEdgeDst(jj);
          auto& dnode       = graph->getData(dst);
          
          uint32_t new_dist = 1 + snode.dist_current;
          uint32_t old_dist = galois::atomicMin(dnode.dist_current, new_dist);
          
          if (old_dist > new_dist) {
            bitset_dist_current.set(dst);
          }
        }
      }
    }
  }
};

/******************************************************************************/
/* Sanity check operators */
/******************************************************************************/

/* Prints total number of nodes visited + max distance */
struct BFSSanityCheck {
  const uint32_t& local_infinity;
  Graph* graph;

  galois::DGAccumulator<uint64_t>& DGAccumulator_sum;
  galois::DGReduceMax<uint32_t>& DGMax;

  BFSSanityCheck(const uint32_t& _infinity, Graph* _graph,
                 galois::DGAccumulator<uint64_t>& dgas,
                 galois::DGReduceMax<uint32_t>& dgm)
      : local_infinity(_infinity), graph(_graph), DGAccumulator_sum(dgas),
        DGMax(dgm) {}

  void static go(Graph& _graph, galois::DGAccumulator<uint64_t>& dgas,
                 galois::DGReduceMax<uint32_t>& dgm, const uint64_t& start_node) {
    dgas.reset();
    dgm.reset();

    if (personality == GPU_CUDA) {
#ifdef GALOIS_ENABLE_GPU
      uint64_t sum;
      uint32_t max;
      BFSSanityCheck_masterNodes_cuda(sum, max, infinity, cuda_ctx);
      dgas += sum;
      dgm.update(max);
#else
      abort();
#endif
    } else {
      galois::do_all(galois::iterate(_graph.masterNodesRange().begin(),
                                     _graph.masterNodesRange().end()),
                     BFSSanityCheck(infinity, &_graph, dgas, dgm),
                     galois::no_stats(), galois::loopname("BFSSanityCheck"));
    }

    uint64_t num_visited  = dgas.reduce();
    uint32_t max_distance = dgm.reduce();

    // Only host 0 will print the info
    if (galois::runtime::getSystemNetworkInterface().ID == 0) {
      galois::gPrint("Number of nodes visited from source ", start_node, " is ",
                     num_visited, "\n");
      galois::gPrint("Max distance from source ", start_node, " is ",
                     max_distance, "\n");
    }
  }

  void operator()(GNode src) const {
    NodeData& src_data = graph->getData(src);

    if (src_data.dist_current < local_infinity) {
      DGAccumulator_sum += 1;
      DGMax.update(src_data.dist_current);
    }
  }
};

/******************************************************************************/
/* Make results */
/******************************************************************************/

std::vector<uint32_t> makeResultsCPU(std::unique_ptr<Graph>& hg) {
  std::vector<uint32_t> values;

  values.reserve(hg->numMasters());
  for (auto node : hg->masterNodesRange()) {
    values.push_back(hg->getData(node).dist_current);
  }

  return values;
}

#ifdef GALOIS_ENABLE_GPU
std::vector<uint32_t> makeResultsGPU(std::unique_ptr<Graph>& hg) {
  std::vector<uint32_t> values;

  values.reserve(hg->numMasters());
  for (auto node : hg->masterNodesRange()) {
    values.push_back(get_node_dist_current_cuda(cuda_ctx, node));
  }

  return values;
}
#else
std::vector<uint32_t> makeResultsGPU(std::unique_ptr<Graph>& /*unused*/) {
  abort();
}
#endif

std::vector<uint32_t> makeResults(std::unique_ptr<Graph>& hg) {
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

constexpr static const char* const name =
    "BFS - Distributed Heterogeneous with "
    "worklist.";
constexpr static const char* const desc = "BFS on Distributed Galois.";
constexpr static const char* const url  = nullptr;

int main(int argc, char** argv) {
  galois::DistMemSys G;
  DistBenchStart(argc, argv, name, desc, url);

  const auto& net = galois::runtime::getSystemNetworkInterface();
  if (net.ID == 0) {
    galois::runtime::reportParam(REGION_NAME, "Max Iterations", maxIterations);
    galois::runtime::reportParam(REGION_NAME, "Source Node ID", src_node[0]);
  }

  galois::StatTimer StatTimer_total("TimerTotal", REGION_NAME);

  StatTimer_total.start();

  std::unique_ptr<Graph> hg;
#ifdef GALOIS_ENABLE_GPU
  std::tie(hg, syncSubstrate) =
      distGraphInitialization<NodeData, void>(&cuda_ctx);
#else
  std::tie(hg, syncSubstrate) = distGraphInitialization<NodeData, void>();
#endif

  // bitset comm setup
  bitset_dist_current.resize(hg->size());

  galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");

  // InitializeGraph::go((*hg));
  InitializeGraph::go((*hg), src_node[0]);
  galois::runtime::getHostBarrier().wait();
  
  // accumulators for use in operators
  galois::DGAccumulator<uint64_t> DGAccumulator_sum;
  galois::DGReduceMax<uint32_t> m;
    
  for (auto run = 0; run < numRuns; ++run) {
    galois::gPrint("[", net.ID, "] BFS::go run ", run, " called\n");
    
    std::string timer_str("Timer_" + std::to_string(run));
    galois::StatTimer StatTimer_main(timer_str.c_str(), REGION_NAME);

    StatTimer_main.start();
    if (execution == Async) {
      BFS<true>::go(*hg, 
                    src_node[run]);
    } else {
      BFS<false>::go(*hg, 
                    src_node[run]);
    }
    StatTimer_main.stop();

    // sanity check
    BFSSanityCheck::go(*hg, DGAccumulator_sum, m, src_node[run]);

    if ((run + 1) != numRuns) {
      if (personality == GPU_CUDA) {
#ifdef GALOIS_ENABLE_GPU
        bitset_dist_current_reset_cuda(cuda_ctx);
#else
        abort();
#endif
      } else {
        bitset_dist_current.reset();
      }

      syncSubstrate->set_num_run(run + 1);
      // InitializeGraph::go((*hg));
      InitializeGraph::go((*hg), src_node[run+1]);
      galois::runtime::getHostBarrier().wait();
    }
  }

  StatTimer_total.stop();

  if (output) {
    std::vector<uint32_t> results = makeResults(hg);
    auto globalIDs                = hg->getMasterGlobalIDs();
    assert(results.size() == globalIDs.size());

    writeOutput(outputLocation, "level", results.data(), results.size(),
                globalIDs.data());
  }

  return 0;
}
