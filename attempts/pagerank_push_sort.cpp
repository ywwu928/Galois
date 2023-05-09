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

static cll::opt<uint32_t> counterSize("topk",
                                    cll::desc("Top k frequently accessed nodes: Default 100"),
                                    cll::init(100));

static cll::opt<uint32_t> roundRatio("rRatio",
                                    cll::desc("Ratio of elements in a counter reset round to k: Default 40"),
                                    cll::init(40));

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

#include "pagerank_push_sync.hh"

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/
bool check_counter (
    uint64_t node_ID,
    std::vector <std::pair<uint64_t, int>>& counter
) {
    // find if node is already in counter
    auto curr_it = std::find_if(counter.begin(), counter.end(), [&node_ID](const std::pair<uint64_t, int>& element) {return element.first == node_ID;});
    if (curr_it != counter.end()) { // exists
        return true;
    }
    else { // does not exist
        return false;
    }
}

void increment_counter (
    uint64_t node_ID,
    uint64_t total_nodes,
    std::vector <std::pair<uint64_t, int>>& counter
) {
    // find if node is already in counter
    auto curr_it = std::find_if(counter.begin(), counter.end(), [&node_ID](const std::pair<uint64_t, int>& element) {return element.first == node_ID;});
    if (curr_it != counter.end()) { // exists
        curr_it->second++; // increment the counter value
    }
    else { // does not exist
        // check if there is still empty space in counter
        auto curr_it = std::find_if(counter.begin(), counter.end(), [&total_nodes](const std::pair<uint64_t, int>& element) {return element.first == total_nodes;});
        if (curr_it != counter.end()) { // exists empty space
            curr_it->first = node_ID;
            curr_it->second = 1;
        }
    }
}

void update_counters (
    uint64_t total_nodes,
    std::vector <std::pair<uint64_t, int>>& counter_reserved,
    std::vector <std::pair<uint64_t, int>>& counter_unreserved
) {
/*
    std::cout << "Before Update!" << std::endl;
    std::cout << "Reserved Counter:" << std::endl;
    for (auto i: counter_reserved) {
        std::cout << "(" << i.first << ", " << i.second << ") ";
    }
    std::cout << std::endl;
    std::cout << std::endl;
      
    std::cout << "Unreserved Counter" << std::endl;
    for (auto i: counter_unreserved) {
        std::cout << "(" << i.first << ", " << i.second << ") ";
    }
    std::cout << std::endl;
    std::cout << std::endl;
*/
    std::vector <std::pair<uint64_t, int>> counter_merged = counter_reserved;
    for (auto i: counter_unreserved) {
        uint64_t temp_id = i.first;
        auto it = std::find_if(counter_merged.begin(), counter_merged.end(), [&temp_id](const std::pair<uint64_t, int>& element) {return element.first == temp_id;});
        if (it != counter_merged.end()) { // exists duplicate
            if (i.second > it->second) { // unreserved counter has higher counts
                it->second = i.second;
            }
        }
        else {
            counter_merged.push_back(i);
        }
    }
    // counter_merged.insert(counter_merged.end(), counter_unreserved.begin(), counter_unreserved.end());
    std::sort(counter_merged.begin(), counter_merged.end(), [](const std::pair<uint64_t, int>& left, const std::pair<uint64_t, int>& right) {return left.second > right.second;});
    counter_reserved.assign(counter_merged.begin(), counter_merged.begin() + counterSize);
    std::pair<uint64_t, int> init_pair(total_nodes, 0); 
    std::fill(counter_unreserved.begin(), counter_unreserved.end(), init_pair);
/*    
    std::cout << "After Update!" << std::endl;
    std::cout << "Reserved Counter:" << std::endl;
    for (auto i: counter_reserved) {
        std::cout << "(" << i.first << ", " << i.second << ") ";
    }
    std::cout << std::endl;
    std::cout << std::endl;
      
    std::cout << "Unreserved Counter" << std::endl;
    for (auto i: counter_unreserved) {
        std::cout << "(" << i.first << ", " << i.second << ") ";
    }
    std::cout << std::endl;
    std::cout << std::endl;
*/
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
  
  uint32_t& element_cnt;
  std::vector <std::pair<uint64_t, int>>& cnt_reserved;
  std::vector <std::pair<uint64_t, int>>& cnt_unreserved;
  std::vector <std::pair<uint64_t, int>>& cnt_prev;

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
          uint32_t& _element_cnt,
          std::vector <std::pair<uint64_t, int>>& _cnt_reserved,
          std::vector <std::pair<uint64_t, int>>& _cnt_unreserved,
          std::vector <std::pair<uint64_t, int>>& _cnt_prev,
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
          element_cnt(_element_cnt), 
          cnt_reserved(_cnt_reserved),
          cnt_unreserved(_cnt_unreserved),
          cnt_prev(_cnt_prev),
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
                  uint32_t& element_cnt,
                  std::vector <std::pair<uint64_t, int>>& cnt_reserved,
                  std::vector <std::pair<uint64_t, int>>& cnt_unreserved,
                  std::vector <std::pair<uint64_t, int>>& cnt_prev,
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
    uint64_t total_nodes = _graph.globalSize();
    std::pair<uint64_t, int> init_pair(total_nodes, 0); 
    
    std::fill(cnt_reserved.begin(), cnt_reserved.end(), init_pair);
    std::fill(cnt_unreserved.begin(), cnt_unreserved.end(), init_pair);
    std::fill(cnt_prev.begin(), cnt_prev.end(), init_pair);
    
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
                    element_cnt,
                    cnt_reserved,
                    cnt_unreserved,
                    cnt_prev,
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
      
      uint64_t cached_count = 0;
      uint64_t cached_one_count = 0;
      uint64_t cached_two_count = 0;
      uint64_t cached_from_host[num_hosts];
      for (uint32_t i=0; i<num_hosts; i++) {
          cached_from_host[i] = 0;
      }

      for (auto i: cnt_prev) {
          if (i.first != total_nodes) {
              cached_count += 1;
              if (i.second == 1) {
                  cached_one_count += 1;
              }
              else if (i.second == 2) {
                  cached_two_count += 1;
              }
              unsigned temp_id = _graph.getHostID(i.first);
              cached_from_host[temp_id] += 1;
          }
      }
      file << "host " << host_id << " number of cached mirrors: " << cached_count << "\n";
      file << "host " << host_id << " number of caches with count 1: " << cached_one_count << "\n";
      file << "host " << host_id << " number of caches with count 2: " << cached_two_count << "\n";
      
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
/*   
      std::cout << "Reserved Counter:" << std::endl;
      for (auto i: cnt_reserved) {
          std::cout << "(" << i.first << ", " << i.second << ") ";
      }
      std::cout << std::endl;
      std::cout << std::endl;
      
      std::cout << "Unreserved Counter" << std::endl;
      for (auto i: cnt_unreserved) {
          std::cout << "(" << i.first << ", " << i.second << ") ";
      }
      std::cout << std::endl;
      std::cout << std::endl;
      
      std::cout << "Previous Counter:" << std::endl;
      for (auto i: cnt_prev) {
          std::cout << "(" << i.first << ", " << i.second << ") ";
      }
      std::cout << std::endl;
      std::cout << std::endl;
*/
      cnt_prev.assign(cnt_reserved.begin(), cnt_reserved.end());
      // std::fill(cnt_reserved.begin(), cnt_reserved.end(), init_pair);
      // std::fill(cnt_unreserved.begin(), cnt_unreserved.end(), init_pair);
      
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
        uint64_t total_nodes = graph->globalSize();

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
            
          if (check_counter(dst_GID, cnt_prev)) {
              local_round += 1;
          }
          else {
              remote_round += 1;
              remote_to_host[host_id] += 1;
          }

          increment_counter(dst_GID, total_nodes, cnt_unreserved);
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
          
          if (check_counter(dst_GID, cnt_prev)) {
              local_round += 1;
              local_write_round += 1;
          }
          else {
              remote_round += 1;
              remote_write_round += 1;
              remote_to_host[host_id] += 1;
              remote_write_to_host[host_id] += 1;
          }

          increment_counter(dst_GID, total_nodes, cnt_unreserved);
        }

        bitset_residual.set(dst);

        element_cnt += 1;

        if (element_cnt == roundRatio * counterSize) {
            update_counters(total_nodes, cnt_reserved, cnt_unreserved);

            element_cnt = 0;
        }
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
  
  uint32_t element_cnt = 0;
  std::vector <std::pair<uint64_t, int>> cnt_reserved(counterSize);
  std::vector <std::pair<uint64_t, int>> cnt_unreserved(counterSize);
  std::vector <std::pair<uint64_t, int>> cnt_prev(counterSize);
  
  std::ofstream file;
  file.open(graphName + "_" + std::to_string(num_hosts) + "procs_k" + std::to_string(counterSize) + "_r" + std::to_string(roundRatio) + "_id" + std::to_string(host_id));
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
                        element_cnt,
                        cnt_reserved,
                        cnt_unreserved,
                        cnt_prev,
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
                        element_cnt,
                        cnt_reserved,
                        cnt_unreserved,
                        cnt_prev,
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
