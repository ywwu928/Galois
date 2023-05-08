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
#include <fstream>
#include <sstream>
#include <limits>
#include <vector>
#include <string>
#include <unordered_map>

#ifdef GALOIS_ENABLE_GPU
#include "pagerank_push_cuda.h"
struct CUDA_Context* cuda_ctx;
#else
enum { CPU, GPU_CUDA };
int personality = CPU;
#endif

constexpr static const char* const REGION_NAME = "PageRank";

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

static cll::opt<uint32_t> cacheSize("cacheSize",
                                    cll::desc("Size of the cache. Default is 1000"),
                                    cll::init(1000));

static cll::opt<uint32_t> prime1("prime1",
                                    cll::desc("Prime number for the first modular hash function. Dafault is 8527"),
                                    cll::init(8527));
static cll::opt<uint32_t> threshold1("threshold1",
                                    cll::desc("Threshold for the first modular hash function. Dafault is 150"),
                                    cll::init(150));

static cll::opt<uint32_t> prime2("prime2",
                                    cll::desc("Prime number for the second modular hash function. Dafault is 67339"),
                                    cll::init(67339));
static cll::opt<uint32_t> threshold2("threshold2",
                                    cll::desc("Threshold for the second modular hash function. Dafault is 20"),
                                    cll::init(20));

static cll::opt<uint32_t> prime3("prime3",
                                    cll::desc("Prime number for the third modular hash function. Dafault is 213491"),
                                    cll::init(213491));
static cll::opt<uint32_t> threshold3("threshold3",
                                    cll::desc("Threshold for the third modular hash function. Dafault is 6"),
                                    cll::init(6));

static cll::opt<std::string> graphName("graphName",
                                    cll::desc("Name of the input graph"),
                                    cll::init("temp"));

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

static const float alpha = (1.0 - 0.85);
struct NodeData {
  float value;
  std::atomic<uint32_t> nout;
  float delta;
  std::atomic<float> residual;
};

galois::DynamicBitSet bitset_residual;
galois::DynamicBitSet bitset_nout;
galois::DynamicBitSet bitset_touched;

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;
typedef GNode WorkItem;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph>> syncSubstrate;
  
std::vector <uint32_t> hash_counter1(prime1+1);
std::vector <uint32_t> hash_counter2(prime2+1);
std::vector <uint32_t> hash_counter3(prime3+1);
std::unordered_map <uint64_t, bool> id_curr(cacheSize);
std::unordered_map <uint64_t, bool> id_prev(cacheSize);

#include "pagerank_push_sync.hh"

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/
bool check_id (
    uint64_t node_ID,
    std::unordered_map <uint64_t, bool>& cached_ID
) {
    // find if node is cached
    auto it = cached_ID.find(node_ID);
    if (it != cached_ID.end()) { // exists
        return true;
    }
    else { // does not exist
        return false;
    }
}

void increment_counter (uint64_t node_ID) {
    int index1 = node_ID % prime1;
    int index2 = node_ID % prime2;
    int index3 = node_ID % prime3;

    hash_counter1[index1] += 1;
    hash_counter2[index2] += 1;
    hash_counter3[index3] += 1;

    if (hash_counter1[index1] >= threshold1) {
        if (hash_counter2[index2] >= threshold2) {
            if (hash_counter3[index3] >= threshold3) {
                // find if already cached
                auto it = id_curr.find(node_ID);
                if (it == id_curr.end()) { // does not exist
                    if (id_curr.size() < cacheSize) { // there is still empty space
                        id_curr[node_ID] = true;
                    }
                }
            }
        }
    }
}

// Reset all fields of all nodes to 0
struct ResetGraph {
  Graph* graph;

  ResetGraph(Graph* _graph) : graph(_graph) {}
  void static go(Graph& _graph) {
    const auto& allNodes = _graph.allNodesRange();
    if (personality == GPU_CUDA) {
#ifdef GALOIS_ENABLE_GPU
      std::string impl_str("ResetGraph_" +
                           (syncSubstrate->get_run_identifier()));
      galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
      StatTimer_cuda.start();
      ResetGraph_allNodes_cuda(cuda_ctx);
      StatTimer_cuda.stop();
#else
      abort();
#endif
    } else if (personality == CPU) {
      galois::do_all(
          galois::iterate(allNodes.begin(), allNodes.end()),
          ResetGraph{&_graph}, galois::no_stats(),
          galois::loopname(
              syncSubstrate->get_run_identifier("ResetGraph").c_str()));
    }
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    sdata.value     = 0;
    sdata.nout      = 0;
    sdata.residual  = 0;
    sdata.delta     = 0;
  }
};

// Initialize residual at nodes with outgoing edges + find nout for
// nodes with outgoing edges
struct InitializeGraph {
  const float& local_alpha;
  Graph* graph;

  InitializeGraph(const float& _alpha, Graph* _graph)
      : local_alpha(_alpha), graph(_graph) {}

  void static go(Graph& _graph) {
    // first initialize all fields to 0 via ResetGraph (can't assume all zero
    // at start)
    ResetGraph::go(_graph);

    const auto& nodesWithEdges = _graph.allNodesWithEdgesRange();

    if (personality == GPU_CUDA) {
#ifdef GALOIS_ENABLE_GPU
      std::string impl_str("InitializeGraph_" +
                           (syncSubstrate->get_run_identifier()));
      galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
      StatTimer_cuda.start();
      InitializeGraph_nodesWithEdges_cuda(alpha, cuda_ctx);
      StatTimer_cuda.stop();
#else
      abort();
#endif
    } else if (personality == CPU) {
      // regular do all without stealing; just initialization of nodes with
      // outgoing edges
      galois::do_all(
          galois::iterate(nodesWithEdges.begin(), nodesWithEdges.end()),
          InitializeGraph{alpha, &_graph}, galois::steal(), galois::no_stats(),
          galois::loopname(
              syncSubstrate->get_run_identifier("InitializeGraph").c_str()));
    }

    syncSubstrate->sync<writeSource, readSource, Reduce_add_nout, Bitset_nout>(
        "InitializeGraphNout");
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    sdata.residual  = local_alpha;
    uint32_t num_edges =
        std::distance(graph->edge_begin(src), graph->edge_end(src));
    galois::atomicAdd(sdata.nout, num_edges);
    bitset_nout.set(src);
  }
};

struct PageRank_delta {
  const float& local_alpha;
  cll::opt<float>& local_tolerance;
  Graph* graph;
  
  PageRank_delta(const float& _local_alpha,
                  cll::opt<float>& _local_tolerance,
                  Graph* _graph)
                  : local_alpha(_local_alpha),
                  local_tolerance(_local_tolerance),
                  graph(_graph) {}

  void static go(Graph& _graph) {
    const auto& nodesWithEdges = _graph.allNodesWithEdgesRange();
    
    if (personality == GPU_CUDA) {
#ifdef GALOIS_ENABLE_GPU
      std::string impl_str("PageRank_" + (syncSubstrate->get_run_identifier()));
      galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
      StatTimer_cuda.start();
      PageRank_delta_nodesWithEdges_cuda(alpha, tolerance, cuda_ctx);
      StatTimer_cuda.stop();
#else
      abort();
#endif
    } else if (personality == CPU) {
      galois::do_all(
          galois::iterate(nodesWithEdges.begin(), nodesWithEdges.end()),
          PageRank_delta{alpha, 
                        tolerance, 
                        &_graph}, 
          galois::no_stats(),
          galois::loopname(syncSubstrate->get_run_identifier("PageRank_delta").c_str()));
    }
  }

  void operator()(WorkItem src) const {
    NodeData& sdata = graph->getData(src);
    
    /*
    if (graph->isOwned(graph->getGID(src))) {
      master_round += 1;
    }
    else {
      mirror_round += 1;
    }
    */

    if (sdata.residual > 0) {
      float residual_old = sdata.residual;
      sdata.residual     = 0;
      sdata.value += residual_old;
      if (residual_old > this->local_tolerance) {
        if (sdata.nout > 0) {
          sdata.delta = residual_old * (1 - local_alpha) / sdata.nout;
        }
      }
    }
  }
};

template <bool async>
struct PageRank {
  Graph* graph;
  using DGTerminatorDetector =
      typename std::conditional<async, galois::DGTerminator<unsigned int>,
                                galois::DGAccumulator<unsigned int>>::type;

  DGTerminatorDetector& active_vertices;
  
  galois::DGAccumulator<uint64_t>& master_total;
  galois::DGAccumulator<uint64_t>& master_write_total;
  galois::DGAccumulator<uint64_t>& master_round;
  galois::DGAccumulator<uint64_t>& master_write_round;
  galois::DGAccumulator<uint64_t>& mirror_total;
  galois::DGAccumulator<uint64_t>& mirror_write_total;
  galois::DGAccumulator<uint64_t>& mirror_round;
  galois::DGAccumulator<uint64_t>& mirror_write_round;
  galois::DGAccumulator<uint64_t>* mirror_to_host;
  galois::DGAccumulator<uint64_t>* mirror_write_to_host;
  galois::DGAccumulator<uint64_t>* num_mirror_touched_to_host;
  galois::DGAccumulator<uint64_t>* num_mirror_write_to_host;
  galois::DGAccumulator<uint64_t>& local_round;
  galois::DGAccumulator<uint64_t>& local_write_round;
  galois::DGAccumulator<uint64_t>& remote_round;
  galois::DGAccumulator<uint64_t>& remote_write_round;
  galois::DGAccumulator<uint64_t>* remote_to_host;
  galois::DGAccumulator<uint64_t>* remote_write_to_host;
  
  std::ofstream& file;

  PageRank(Graph* _g,
          DGTerminatorDetector& _dga,
          galois::DGAccumulator<uint64_t>& _master_total,
          galois::DGAccumulator<uint64_t>& _master_write_total,
          galois::DGAccumulator<uint64_t>& _master_round,
          galois::DGAccumulator<uint64_t>& _master_write_round,
          galois::DGAccumulator<uint64_t>& _mirror_total,
          galois::DGAccumulator<uint64_t>& _mirror_write_total,
          galois::DGAccumulator<uint64_t>& _mirror_round,
          galois::DGAccumulator<uint64_t>& _mirror_write_round,
          galois::DGAccumulator<uint64_t>* _mirror_to_host,
          galois::DGAccumulator<uint64_t>* _mirror_write_to_host,
          galois::DGAccumulator<uint64_t>* _num_mirror_touched_to_host,
          galois::DGAccumulator<uint64_t>* _num_mirror_write_to_host,
          galois::DGAccumulator<uint64_t>& _local_round,
          galois::DGAccumulator<uint64_t>& _local_write_round,
          galois::DGAccumulator<uint64_t>& _remote_round,
          galois::DGAccumulator<uint64_t>& _remote_write_round,
          galois::DGAccumulator<uint64_t>* _remote_to_host,
          galois::DGAccumulator<uint64_t>* _remote_write_to_host,
          std::ofstream& _file)
          : graph(_g),
          active_vertices(_dga),
          master_total(_master_total), 
          master_write_total(_master_write_total),
          master_round(_master_round), 
          master_write_round(_master_write_round),
          mirror_total(_mirror_total), 
          mirror_write_total(_mirror_write_total),
          mirror_round(_mirror_round), 
          mirror_write_round(_mirror_write_round),
          mirror_to_host(_mirror_to_host),
          mirror_write_to_host(_mirror_write_to_host),
          num_mirror_touched_to_host(_num_mirror_touched_to_host),
          num_mirror_write_to_host(_num_mirror_write_to_host),
          local_round(_local_round), 
          local_write_round(_local_write_round),
          remote_round(_remote_round), 
          remote_write_round(_remote_write_round),
          remote_to_host(_remote_to_host),
          remote_write_to_host(_remote_write_to_host),
          file(_file) {}

  void static go(Graph& _graph, 
                  galois::DGAccumulator<uint64_t>& master_total,
                  galois::DGAccumulator<uint64_t>& master_write_total,
                  galois::DGAccumulator<uint64_t>& master_round,
                  galois::DGAccumulator<uint64_t>& master_write_round,
                  galois::DGAccumulator<uint64_t>& mirror_total,
                  galois::DGAccumulator<uint64_t>& mirror_write_total,
                  galois::DGAccumulator<uint64_t>& mirror_round,
                  galois::DGAccumulator<uint64_t>& mirror_write_round,
                  galois::DGAccumulator<uint64_t>* mirror_to_host,
                  galois::DGAccumulator<uint64_t>* mirror_write_to_host,
                  galois::DGAccumulator<uint64_t>* num_mirror_touched_to_host,
                  galois::DGAccumulator<uint64_t>* num_mirror_write_to_host,
                  galois::DGAccumulator<uint64_t>& local_round,
                  galois::DGAccumulator<uint64_t>& local_write_round,
                  galois::DGAccumulator<uint64_t>& remote_round,
                  galois::DGAccumulator<uint64_t>& remote_write_round,
                  galois::DGAccumulator<uint64_t>* remote_to_host,
                  galois::DGAccumulator<uint64_t>* remote_write_to_host,
                  std::ofstream& file) {
    unsigned _num_iterations   = 0;
    const auto& nodesWithEdges = _graph.allNodesWithEdgesRange();
    DGTerminatorDetector dga;
    
    master_total.reset();
    master_write_total.reset();
    mirror_total.reset();
    mirror_write_total.reset();
    
    uint32_t num_hosts = _graph.getNumHosts();
    uint64_t host_id = galois::runtime::getSystemNetworkInterface().ID;
    
    do {
      syncSubstrate->set_num_round(_num_iterations);
      
      master_round.reset();
      master_write_round.reset();
      mirror_round.reset();
      mirror_write_round.reset();
      local_round.reset();
      local_write_round.reset();
      remote_round.reset();
      remote_write_round.reset();
      
      bitset_touched.reset();
      
      for (uint32_t i=0; i<num_hosts; i++) {
          mirror_to_host[i].reset();
          mirror_write_to_host[i].reset();
          num_mirror_touched_to_host[i].reset();
          num_mirror_write_to_host[i].reset();
          remote_to_host[i].reset();
          remote_write_to_host[i].reset();
      }
    
      std::fill(hash_counter1.begin(), hash_counter1.end(), 0);
      std::fill(hash_counter2.begin(), hash_counter2.end(), 0);
      std::fill(hash_counter3.begin(), hash_counter3.end(), 0);
      
      PageRank_delta::go(_graph); 
      dga.reset();
      // reset residual on mirrors
      syncSubstrate->reset_mirrorField<Reduce_add_residual>();

      if (personality == GPU_CUDA) {
#ifdef GALOIS_ENABLE_GPU
        std::string impl_str("PageRank_" +
                             (syncSubstrate->get_run_identifier()));
        galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
        StatTimer_cuda.start();
        unsigned int __retval = 0;
        PageRank_nodesWithEdges_cuda(__retval, cuda_ctx);
        dga += __retval;
        StatTimer_cuda.stop();
#else
        abort();
#endif
      } else if (personality == CPU) {
        galois::do_all(
            galois::iterate(nodesWithEdges), 
            PageRank{&_graph, 
                    dga, 
                    master_total, 
                    master_write_total, 
                    master_round, 
                    master_write_round,
                    mirror_total, 
                    mirror_write_total,
                    mirror_round, 
                    mirror_write_round,
                    mirror_to_host,
                    mirror_write_to_host,
                    num_mirror_touched_to_host,
                    num_mirror_write_to_host,
                    local_round, 
                    local_write_round,
                    remote_round, 
                    remote_write_round,
                    remote_to_host,
                    remote_write_to_host,
                    file},
            galois::no_stats(), 
            galois::steal(),
            galois::loopname(syncSubstrate->get_run_identifier("PageRank").c_str()));
      }
     
      file << "#####   Round " << _num_iterations << "   #####\n";
      file << "host " << host_id << " round master accesses: " << master_round.read_local() << "\n";
      file << "host " << host_id << " round master writes: " << master_write_round.read_local() << "\n";
      file << "host " << host_id << " round mirror accesses: " << mirror_round.read_local() << "\n";
      file << "host " << host_id << " round mirror writes: " << mirror_write_round.read_local() << "\n";
      file << "host " << host_id << " round local accesses: " << local_round.read_local() << "\n";
      file << "host " << host_id << " round local writes: " << local_write_round.read_local() << "\n";
      file << "host " << host_id << " round remote accesses: " << remote_round.read_local() << "\n";
      file << "host " << host_id << " round remote writes: " << remote_write_round.read_local() << "\n";
      
      uint64_t dirty_count = 0;
      uint64_t touched_count = 0;
      for (uint64_t i=_graph.numMasters(); i<_graph.size(); i++) {
        if (bitset_residual.test(i)) {
          dirty_count += 1;
        }
        
        if (bitset_touched.test(i)) {
          touched_count += 1;
        }
      }
      file << "host " << host_id << " number of dirty mirrors: " << dirty_count << "\n";
      file << "host " << host_id << " number of touched mirrors: " << touched_count << "\n";
      
      uint64_t cached_count = id_prev.size();

      uint64_t cached_from_host[num_hosts];
      for (uint32_t i=0; i<num_hosts; i++) {
          cached_from_host[i] = 0;
      }
      for (auto& i: id_prev) {
          unsigned temp_id = _graph.getHostID(i.first);
          cached_from_host[temp_id] += 1;
      }
      file << "host " << host_id << " number of cached mirrors: " << cached_count << "\n";
      
      for (uint32_t i=0; i<num_hosts; i++) {
          file << "host " << host_id << " mirror access to host " << i << ": " << mirror_to_host[i].read_local() << "\n";
          file << "host " << host_id << " mirror write to host " << i << ": " << mirror_write_to_host[i].read_local() << "\n";
          file << "host " << host_id << " dirty mirrors for host " << i << ": " << num_mirror_write_to_host[i].read_local() << "\n";
          file << "host " << host_id << " touched mirrors for host " << i << ": " << num_mirror_touched_to_host[i].read_local() << "\n";
          file << "host " << host_id << " remote access to host " << i << ": " << remote_to_host[i].read_local() << "\n";
          file << "host " << host_id << " remote write to host " << i << ": " << remote_write_to_host[i].read_local() << "\n";
          file << "host " << host_id << " cached mirrors from host " << i << ": " << cached_from_host[i] << "\n";
      }

      syncSubstrate->sync<writeDestination, readSource, Reduce_add_residual,
                          Bitset_residual, async>("PageRank");
      
      master_total += master_round.read_local();
      master_write_total += master_write_round.read_local();
      mirror_total += mirror_round.read_local();
      mirror_write_total += mirror_write_round.read_local();
      
      id_prev = id_curr;
      id_curr.clear();
      
      galois::runtime::reportStat_Tsum(
          REGION_NAME, "NumWorkItems_" + (syncSubstrate->get_run_identifier()),
          (unsigned long)dga.read_local());

      ++_num_iterations;
    } while ((async || (_num_iterations < maxIterations)) &&
             dga.reduce(syncSubstrate->get_run_identifier()));

    if (galois::runtime::getSystemNetworkInterface().ID == 0) {
      galois::runtime::reportStat_Single(
          REGION_NAME,
          "NumIterations_" + std::to_string(syncSubstrate->get_run_num()),
          (unsigned long)_num_iterations);
    }
    
    file << "#####   Summary   #####" << "\n";
    file << "host " << host_id << " total master accesses: " << master_total.read_local() << "\n";
    file << "host " << host_id << " total master writes: " << master_write_total.read_local() << "\n";
    file << "host " << host_id << " total mirror accesses: " << mirror_total.read_local() << "\n";
    file << "host " << host_id << " total mirror writes: " << mirror_write_total.read_local() << "\n";
  }

  void operator()(WorkItem src) const {
    NodeData& sdata = graph->getData(src);
    
    /*
    if (graph->isOwned(graph->getGID(src))) {
      master_round += 1;
    }
    else {
      mirror_round += 1;
    }
    */
    
    if (sdata.delta > 0) {
      float _delta = sdata.delta;
      sdata.delta  = 0;
      
      active_vertices += 1; // this should be moved to Pagerank_delta operator

      for (auto nbr : graph->edges(src)) {
        GNode dst       = graph->getEdgeDst(nbr);
        NodeData& ddata = graph->getData(dst);
          
        uint64_t dst_GID = graph->getGID(dst);

        bool owned = graph->isOwned(dst_GID);
        unsigned host_id = graph->getHostID(dst_GID);
          
        if (owned) {
          master_round += 1;
          local_round += 1;
        }
        else {
          mirror_round += 1;

          mirror_to_host[host_id] += 1;
            
          if (!bitset_touched.test(dst)) {
              num_mirror_touched_to_host[host_id] += 1;
          }
            
          if (check_id(dst_GID, id_prev)) {
              local_round += 1;
          }
          else {
              remote_round += 1;
              remote_to_host[host_id] += 1;
          }

          increment_counter(dst_GID);
        }
        
        bitset_touched.set(dst);
        
        galois::atomicAdd(ddata.residual, _delta);
            
        if (owned) {
          master_round += 1;
          master_write_round += 1;
          local_round += 1;
          local_write_round += 1;
        }
        else {
          mirror_round += 1;
          mirror_write_round += 1;
        
          mirror_to_host[host_id] += 1;
          mirror_write_to_host[host_id] += 1;

          if (!bitset_residual.test(dst)) {
              num_mirror_write_to_host[host_id] += 1;
          }
          
          if (check_id(dst_GID, id_prev)) {
              local_round += 1;
              local_write_round += 1;
          }
          else {
              remote_round += 1;
              remote_write_round += 1;
              remote_to_host[host_id] += 1;
              remote_write_to_host[host_id] += 1;
          }

          increment_counter(dst_GID);
        }

        bitset_residual.set(dst);
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
#else
      abort();
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

/******************************************************************************/
/* Make results */
/******************************************************************************/

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
constexpr static const char* const desc = "Residual PageRank on Distributed "
                                          "Galois.";
constexpr static const char* const url = 0;

int main(int argc, char** argv) {
  galois::DistMemSys G;
  DistBenchStart(argc, argv, name, desc, url);

  auto& net = galois::runtime::getSystemNetworkInterface();

  if (net.ID == 0) {
    galois::runtime::reportParam(REGION_NAME, "Max Iterations", maxIterations);
    std::ostringstream ss;
    ss << tolerance;
    galois::runtime::reportParam(REGION_NAME, "Tolerance", ss.str());
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

  bitset_residual.resize(hg->size());
  bitset_nout.resize(hg->size());
  bitset_touched.resize(hg->size());

  galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");

  InitializeGraph::go((*hg));
  galois::runtime::getHostBarrier().wait();
  
  uint32_t num_hosts = hg->getNumHosts();
  uint64_t host_id = galois::runtime::getSystemNetworkInterface().ID;

  galois::DGAccumulator<float> DGA_sum;
  galois::DGAccumulator<float> DGA_sum_residual;
  galois::DGAccumulator<uint64_t> DGA_residual_over_tolerance;
  galois::DGReduceMax<float> max_value;
  galois::DGReduceMin<float> min_value;
  galois::DGReduceMax<float> max_residual;
  galois::DGReduceMin<float> min_residual;
  galois::DGAccumulator<uint64_t> master_total;
  galois::DGAccumulator<uint64_t> master_write_total;
  galois::DGAccumulator<uint64_t> master_round;
  galois::DGAccumulator<uint64_t> master_write_round;
  galois::DGAccumulator<uint64_t> mirror_total;
  galois::DGAccumulator<uint64_t> mirror_write_total;
  galois::DGAccumulator<uint64_t> mirror_round;
  galois::DGAccumulator<uint64_t> mirror_write_round;
  galois::DGAccumulator<uint64_t> mirror_to_host[num_hosts];
  galois::DGAccumulator<uint64_t> mirror_write_to_host[num_hosts];
  galois::DGAccumulator<uint64_t> num_mirror_touched_to_host[num_hosts];
  galois::DGAccumulator<uint64_t> num_mirror_write_to_host[num_hosts];
  galois::DGAccumulator<uint64_t> local_round;
  galois::DGAccumulator<uint64_t> local_write_round;
  galois::DGAccumulator<uint64_t> remote_round;
  galois::DGAccumulator<uint64_t> remote_write_round;
  galois::DGAccumulator<uint64_t> remote_to_host[num_hosts];
  galois::DGAccumulator<uint64_t> remote_write_to_host[num_hosts];
  
  std::ofstream file;
  file.open(graphName + "_" + std::to_string(num_hosts) + "procs_cache" + std::to_string(cacheSize) + "_id" + std::to_string(host_id));
  file << "#####   Stat   #####" << std::endl;
  file << "host " << host_id << " total edges: " << hg->sizeEdges() << std::endl;

  for (auto run = 0; run < numRuns; ++run) {
    file << "#####   Run " << run << "   #####" << std::endl;
    
    galois::gPrint("[", net.ID, "] PageRank::go run ", run, " called\n");
    std::string timer_str("Timer_" + std::to_string(run));
    galois::StatTimer StatTimer_main(timer_str.c_str(), REGION_NAME);

    StatTimer_main.start();
    if (execution == Async) {
      PageRank<true>::go(*hg, 
                        master_total, 
                        master_write_total, 
                        master_round, 
                        master_write_round,
                        mirror_total, 
                        mirror_write_total,
                        mirror_round, 
                        mirror_write_round,
                        mirror_to_host,
                        num_mirror_touched_to_host,
                        mirror_write_to_host,
                        num_mirror_write_to_host,
                        local_round, 
                        local_write_round,
                        remote_round, 
                        remote_write_round,
                        remote_to_host,
                        remote_write_to_host,
                        file);
    } else {
      PageRank<false>::go(*hg, 
                        master_total, 
                        master_write_total, 
                        master_round, 
                        master_write_round,
                        mirror_total, 
                        mirror_write_total,
                        mirror_round, 
                        mirror_write_round,
                        mirror_to_host,
                        num_mirror_touched_to_host,
                        mirror_write_to_host,
                        num_mirror_write_to_host,
                        local_round, 
                        local_write_round,
                        remote_round, 
                        remote_write_round,
                        remote_to_host,
                        remote_write_to_host,
                        file);
    }
    StatTimer_main.stop();

    // sanity check
    PageRankSanity::go(*hg, DGA_sum, DGA_sum_residual,
                       DGA_residual_over_tolerance, max_value, min_value,
                       max_residual, min_residual);

    if ((run + 1) != numRuns) {
      if (personality == GPU_CUDA) {
#ifdef GALOIS_ENABLE_GPU
        bitset_residual_reset_cuda(cuda_ctx);
        bitset_nout_reset_cuda(cuda_ctx);
        bitset_touched_reset_cuda(cuda_ctx);
#else
        abort();
#endif
      } else {
        bitset_residual.reset();
        bitset_nout.reset();
        bitset_touched.reset();
      }

      (*syncSubstrate).set_num_run(run + 1);
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
