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
static std::string TYPE_NAME;

enum Exp {
    Pull,
    Push,
    Global_Heuristic,
    Global_Hysteresis,
    Local_Heuristic,
    Local_Hysteresis,
    Hybrid_Edge,
    Hybrid_Degree,
    Exp_Count
};

std::string exp_names[] = {
    "Pull",
    "Push",
    "Global_Heuristic",
    "Global_Hysteresis",
    "Local_Heuristic",
    "Local_Hysteresis",
    "Hybrid_Edge",
    "Hybrid_Degree"
};

/******************************************************************************/
/* Declaration of command line arguments */
/******************************************************************************/

namespace cll = llvm::cl;
static cll::opt<unsigned int> maxIterations("maxIterations",
                                            cll::desc("Maximum iterations: "
                                                      "Default 1000"),
                                            cll::init(1000));

static cll::opt<float> lower_bound("lower_bound",
                                   cll::desc("edge density lower bound for switching"),
                                   cll::init(0.05));

static cll::opt<float> upper_bound("upper_bound",
                                   cll::desc("edge density upper bound for switching"),
                                   cll::init(0.8));

static cll::opt<int> degree_density_bound("degree_density_bound",
                                           cll::desc("degree density bound for switching"),
                                           cll::init(50));

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

struct NodeData {
  std::atomic<uint32_t> comp_current;
};

galois::DynamicBitSet bitset_comp_current_odd;
galois::DynamicBitSet bitset_comp_current_even;

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph, uint32_t>> syncSubstrate;

#include "cc_sync.hh"

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/

struct InitializeGraph {
  Graph* graph;

  InitializeGraph(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
    const auto& presentNodes = _graph.presentNodesRange();

    galois::do_all(
        galois::iterate(presentNodes),
        InitializeGraph{&_graph}, galois::no_stats());
  }

  void operator()(GNode src) const {
    NodeData& sdata    = graph->getData(src);
    sdata.comp_current = graph->getGID(src);
  }
};

struct PullRemote {
  Graph* graph;
  
  galois::DynamicBitSet* active_bitset_ptr;

  galois::runtime::NetworkInterface& net;

  PullRemote(Graph* _graph, galois::DynamicBitSet* _active_bitset_ptr)
      : graph(_graph),
        active_bitset_ptr(_active_bitset_ptr),
        net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph, galois::DynamicBitSet* _active_bitset_ptr) {
      const auto& remoteNodes = _graph.remoteNodesRange();

      galois::do_all(
          galois::iterate(remoteNodes), PullRemote{&_graph, _active_bitset_ptr},
          galois::steal(), galois::no_stats());
  }

  void operator()(GNode dst) const {
    uint32_t dcomp = UINT32_MAX;
    for (auto jj : graph->inEdges(dst)) {
        GNode src         = graph->getInEdgeSrc(jj);
        if (active_bitset_ptr->test(src)) {
            auto& snode       = graph->getData(src);
            if (snode.comp_current < dcomp) {
                dcomp = snode.comp_current;
            }
        }
    }
    
    if (dcomp != UINT32_MAX) {
        net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getRemoteLID(dst), dcomp);
    }
  }
};

struct PullMaster {
  Graph* graph;
  
  galois::DynamicBitSet* dirty_bitset_ptr;

  PullMaster(Graph* _graph, galois::DynamicBitSet* _dirty_bitset_ptr)
      : graph(_graph),
        dirty_bitset_ptr(_dirty_bitset_ptr) {}

  void static go(Graph& _graph, galois::DynamicBitSet* _dirty_bitset_ptr) {
      const auto& masterNodes = _graph.masterNodesRange();
      
      galois::do_all(
          galois::iterate(masterNodes), PullMaster{&_graph, _dirty_bitset_ptr},
          galois::steal(), galois::no_stats());
  }

  void operator()(GNode dst) const {
    NodeData& dnode = graph->getData(dst);

    uint32_t old_comp = dnode.comp_current;
    for (auto jj : graph->inEdges(dst)) {
        GNode src         = graph->getInEdgeSrc(jj);
        auto& snode       = graph->getData(src);
        uint32_t new_comp = snode.comp_current;
        galois::minVoid(dnode.comp_current, new_comp);
    }
    
    if (old_comp > dnode.comp_current) {
        dirty_bitset_ptr->set(dst);
    }
  }
};

struct Push {
  Graph* graph;
  
  galois::DynamicBitSet* active_bitset_ptr;
  galois::DynamicBitSet* dirty_bitset_ptr;

  galois::runtime::NetworkInterface& net;

  Push(Graph* _graph, galois::DynamicBitSet* _active_bitset_ptr, galois::DynamicBitSet* _dirty_bitset_ptr)
      : graph(_graph),
        active_bitset_ptr(_active_bitset_ptr),
        dirty_bitset_ptr(_dirty_bitset_ptr),
        net(galois::runtime::getSystemNetworkInterface()) {}

  void static go(Graph& _graph, galois::DynamicBitSet* _active_bitset_ptr, galois::DynamicBitSet* _dirty_bitset_ptr) {
      const auto& masterNodes = _graph.masterNodesRange();
      
      galois::do_all(
          galois::iterate(masterNodes), Push(&_graph, _active_bitset_ptr, _dirty_bitset_ptr),
          galois::no_stats(), galois::steal());
  }

  void operator()(GNode src) const {
    if (active_bitset_ptr->test(src)) {
      NodeData& snode = graph->getData(src);
      uint32_t new_comp = snode.comp_current;
      
      for (auto jj : graph->outEdges(src)) {
        GNode dst         = graph->getOutEdgeDst(jj);
        if (graph->isPhantom(dst)) {
            net.sendWork(galois::substrate::ThreadPool::getTID(), graph->getHostIDForLocal(dst), graph->getRemoteLID(dst), new_comp);
        }
        else {
            auto& dnode       = graph->getData(dst);
            bool dirty = galois::atomicMinBool(dnode.comp_current, new_comp);
            if (dirty) {
                dirty_bitset_ptr->set(dst);
            }
        }
      }
    }
  }
};

void CountActive(Graph& _graph, galois::DynamicBitSet* _active_bitset_ptr, galois::GAccumulator<uint64_t>& _active_vertices, galois::GAccumulator<uint64_t>& _active_edges) {
    galois::on_each([&](unsigned tid, unsigned nthreads) {
        auto& bitset_vec = _active_bitset_ptr->get_vec();
        uint64_t bitset_vec_size = bitset_vec.size();
        uint64_t quotient = bitset_vec_size / nthreads;
        uint64_t remainder = bitset_vec_size % nthreads;

        uint64_t start, end;
        if (tid < remainder) {
            start = tid * (quotient + 1);
            end = (tid + 1) * (quotient + 1);
        }
        else {
            start = tid * quotient + remainder;
            end = (tid + 1) * quotient + remainder;
        }

        for (uint64_t i = start; i < end; ++i) {
            uint64_t value = bitset_vec[i].load(std::memory_order_relaxed);
            uint64_t count = std::popcount(value);
            if (count != 0) {
                _active_vertices += count;
                while (value != 0) {
                    unsigned index = std::countr_zero(value);
                    uint32_t lid = 64 * i + index;
                    uint64_t nout = std::distance(_graph.out_edge_begin(lid), _graph.out_edge_end(lid));
                    _active_edges += nout;
                    value &= value - 1;
                }
            }
        }
    });
}

struct ConnectedComp {
  Graph* graph;

  ConnectedComp(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph, Exp exp) {
#ifdef GALOIS_USER_STATS
    constexpr bool USER_STATS = true;
#else
    constexpr bool USER_STATS = false;
#endif

    unsigned _num_iterations   = 0;
  
    auto& _net = galois::runtime::getSystemNetworkInterface();

    galois::GAccumulator<uint64_t> active_v, active_e;
    uint64_t local_active_v = _graph.numMasters();
    uint64_t local_active_e = _graph.sizeEdges();
    uint64_t global_active_e = _graph.globalSizeEdges();

    bool odd = false;
    bitset_comp_current_even.set_all();
  
    galois::DynamicBitSet* active_bitset_ptr;
    galois::DynamicBitSet* dirty_bitset_ptr;

    bool pull = true;
    bool dual = false;
    bool local = false;
    bool hysteresis = false;
    bool hybrid = false;
    bool degree = false;

    uint64_t active_edges;

    uint64_t edge_threshold_low = _graph.globalSizeEdges() * lower_bound;
    uint64_t edge_threshold_high = _graph.globalSizeEdges() * upper_bound;
    uint64_t vertex_threshold_low = _graph.numMasters() * lower_bound;
    uint64_t vertex_threshold_high = _graph.numMasters() * upper_bound;
    float degree_threshold = (_graph.sizeEdges() / _graph.numMasters()) * degree_density_bound;

    switch (exp) {
        case Pull: {
            pull = true;
            break;
        }
        case Push: {
            pull = false;
            break;
        }
        case Global_Heuristic: {
            dual = true;
            hysteresis = false;
            break;
        }
        case Global_Hysteresis: {
            pull = true;
            dual = true;
            hysteresis = true;
            break;
        }
        case Local_Heuristic: {
            dual = true;
            local = true;
            hysteresis = false;
            break;
        }
        case Local_Hysteresis: {
            pull = true;
            dual = true;
            local = true;
            hysteresis = true;
            break;
        }
        case Hybrid_Edge: {
            pull = true;
            dual = true;
            local = true;
            hysteresis = true;
            hybrid = true;
            break;
        }
        case Hybrid_Degree: {
            pull = true;
            dual = true;
            local = true;
            hysteresis = true;
            hybrid = true;
            degree = true;
            break;
        }
        case Exp_Count: {
            galois::gPrint("Error: Unknown experiment!\n");
            return;
        }
    }

    do {
      std::string total_str("Total_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_total(total_str.c_str(), REGION_NAME_RUN.c_str());
      std::string compute_str("Compute_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_compute(compute_str.c_str(), REGION_NAME_RUN.c_str());
      std::string comm_str("Communication_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_comm(comm_str.c_str(), REGION_NAME_RUN.c_str());
      std::string active_str("Active_Reduce_Round_" + std::to_string(_num_iterations));
      galois::CondStatTimer<USER_STATS> StatTimer_active(active_str.c_str(), REGION_NAME_RUN.c_str());

#ifdef GALOIS_PRINT_PROCESS
      galois::gPrint("Host ", _net.ID, " : iteration ", _num_iterations, "\n");
#endif

      syncSubstrate->set_num_round(_num_iterations);

      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "Active_Vertices_Round_" + std::to_string(_num_iterations), local_active_v);
      galois::runtime::reportStatCond_Single<USER_STATS>(REGION_NAME_RUN.c_str(), "Active_Edges_Round_" + std::to_string(_num_iterations), local_active_e);
      
      StatTimer_total.start();
      if (dual) {
          if (hybrid) {
              if (local_active_v >= vertex_threshold_high) {
                  pull = true;
              }
              else if (local_active_v <= vertex_threshold_low) {
                  pull = false;
              }
              else {
                  if (local_active_e >= edge_threshold_high) {
                      pull = true;
                  }
                  else if (local_active_e <= edge_threshold_low) {
                      pull = false;
                  }
                  else {
                      if (degree) {
                          if ((local_active_e/local_active_v) >= degree_threshold) {
                              pull = true;
                          }
                          else {
                              pull = false;
                          }
                      }
                  }
              }
          }
          else {
              if (local) {
                  active_edges = local_active_e;
              }
              else {
                  active_edges = global_active_e;
              }

              if (hysteresis) {
                  if (active_edges >= edge_threshold_high) {
                      pull = true;
                  }
                  else if (active_edges <= edge_threshold_low) {
                      pull = false;
                  }
              }
              else {
                  if (active_edges > edge_threshold_low) {
                      pull = true;
                  }
                  else {
                      pull = false;
                  }
              }
          }
      }

      if (odd) {
          active_bitset_ptr = &bitset_comp_current_odd;
          dirty_bitset_ptr = &bitset_comp_current_even;
      }
      else {
          active_bitset_ptr = &bitset_comp_current_even;
          dirty_bitset_ptr = &bitset_comp_current_odd;
      }
      
      dirty_bitset_ptr->reset();
      
      _net.prefetchBuffers();

      if (pull) {
          StatTimer_compute.start();
          PullRemote::go(_graph, active_bitset_ptr);
          _net.flushRemoteWork();
          PullMaster::go(_graph, dirty_bitset_ptr);
          StatTimer_compute.stop();
      }
      else {
          StatTimer_compute.start();
          Push::go(_graph, active_bitset_ptr, dirty_bitset_ptr);
          _net.flushRemoteWork();
          StatTimer_compute.stop();
      }

      StatTimer_comm.start();
      syncSubstrate->reduce<Reduce_min_comp_current>(dirty_bitset_ptr);
      StatTimer_comm.stop();
      
      active_v.reset();
      active_e.reset();
      CountActive(_graph, dirty_bitset_ptr, active_v, active_e);
      local_active_v = active_v.reduce();
      local_active_e = active_e.reduce();
      
      odd = !odd;
      
      _net.resetWorkTermination();

      ++_num_iterations;
      
      StatTimer_active.start();
      global_active_e = 0;
      MPI_Allreduce(&local_active_e, &global_active_e, 1,
                    MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);
      StatTimer_active.stop();

      StatTimer_total.stop();
    } while ((_num_iterations < maxIterations) && global_active_e);
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

    galois::do_all(galois::iterate(_graph.masterNodesRange()),
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

constexpr static const char* const name = "Distributed Connected Components (Pull)";
constexpr static const char* const desc = "Distributed Connected Components (Pull)";
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

  net.allocateBufferPool();

  hg->sortEdgesByDestination();

  galois::runtime::getHostBarrier().wait();
  net.partitionDone();

  bitset_comp_current_odd.resize(hg->actualSize());
  bitset_comp_current_even.resize(hg->actualSize());

  galois::runtime::getHostBarrier().wait();
  StatTimer_preprocess.stop();

  galois::DGAccumulator<uint64_t> active_vertices64;

  for (auto run = 0; run < numRuns; ++run) {
    galois::gPrint("[", net.ID, "] ConnectedComp::go run ", run, " called\n");

    for (int i=0; i<Exp_Count; i++) {
        bitset_comp_current_odd.reset();
        bitset_comp_current_even.reset();
        InitializeGraph::go((*hg));

        TYPE_NAME = exp_names[i];
        REGION_NAME_RUN = REGION_NAME + "_" + TYPE_NAME + "_" + std::to_string(run);
        std::string main_timer_str("Timer_" + std::to_string(run));
        galois::StatTimer StatTimer_main(main_timer_str.c_str(), REGION_NAME_RUN.c_str());

        net.touchBufferPool();
        galois::runtime::getHostBarrier().wait();

        StatTimer_main.start();
        ConnectedComp::go(*hg, static_cast<Exp>(i));
        StatTimer_main.stop();
        galois::gPrint("Host ", net.ID, " ConnectedComp run ", run, " (", TYPE_NAME, ") time: ", StatTimer_main.get(), " ms\n");

        ConnectedCompSanityCheck::go(*hg, active_vertices64);
    }

    (*syncSubstrate).set_num_run(run + 1);
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
