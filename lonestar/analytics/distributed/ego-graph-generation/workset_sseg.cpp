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

#include "DistBench/Start.h"
#include "galois/DistGalois.h"
#include "galois/DReducible.h"
#include "galois/AtomicHelpers.h"

#include <memory>
#include <iostream>
#include <vector>

/** include for debug printing **/
#include <boost/functional/hash.hpp>
#include <unordered_set>
/********************************/

static cll::opt<uint64_t>
    start_node("startNode", cll::desc("ID of the source node"), cll::init(0));

std::vector<uint64_t> levels = {5, 3, 2, 1, 0};

constexpr uint64_t NaN = std::numeric_limits<uint64_t>::max();

struct NodeData {
  std::atomic<uint64_t> level_id{NaN};
  uint64_t lid{NaN};
  std::atomic<uint64_t> read;
  std::atomic<uint64_t> write;
  std::atomic<bool> touched;
  std::atomic<uint64_t> workset_touched;
  std::atomic<uint64_t> last_access_touched;
};
using EdgeData  = void;
using Graph     = galois::graphs::DistGraph<NodeData, EdgeData>;
using Substrate = galois::graphs::GluonSubstrate<Graph>;
std::unique_ptr<Substrate> syncSubstrate;

galois::DynamicBitSet bitset_level_id;

#include "galois/runtime/SyncStructures.h"
GALOIS_SYNC_STRUCTURE_REDUCE_MIN(level_id, uint64_t);
GALOIS_SYNC_STRUCTURE_BITSET(level_id);

#include "instrument_workset.h"
std::unique_ptr<Instrument<Graph>> inst;

void initializeWorkset(Graph& graph) {
  galois::do_all(
      galois::iterate(graph.allNodesRange()),
      [&](Graph::GraphNode node) {
        NodeData& data = graph.getData(node);
        data.workset_touched = 0;
      },
      galois::loopname(syncSubstrate->get_run_identifier("InitWS").c_str()),
      galois::no_stats());
}

void initializeGraph(Graph& graph) {
  galois::StatTimer initTimer("TimerInit");
  initTimer.start();
  galois::do_all(
      galois::iterate(graph.allNodesRange()),
      [&](Graph::GraphNode node) {
        NodeData& data = graph.getData(node);
        data.level_id.store(NaN, std::memory_order_relaxed);
        data.lid = NaN;
        data.read = 0;
        data.write = 0;
        data.touched = false;
        data.last_access_touched = 0;
      },
      galois::loopname(syncSubstrate->get_run_identifier("Initalization").c_str()),
      galois::no_stats());
  initTimer.stop();
}

void initializeWorkList(Graph& graph, galois::InsertBag<Graph::GraphNode>& wl, galois::DGAccumulator<uint64_t>& counter) {
  if (!graph.isLocal(start_node))
    return;
  Graph::GraphNode start_lid = graph.getLID(start_node);
  galois::do_all(
      galois::iterate(graph.edge_begin(start_lid), graph.edge_end(start_lid)),
      [&](auto& edge) {
        auto dst = graph.getEdgeDst(edge);
        if (start_lid == dst)
          return; // ignore self loops
        wl.emplace(dst);
        counter += 1;
      },
      galois::loopname(syncSubstrate->get_run_identifier("InitWL").c_str()),
      galois::steal(), galois::no_stats());
}

int main(int argc, char* argv[]) {
  galois::DistMemSys G;
  DistBenchStart(argc, argv, "Ego Graph Generation", nullptr, nullptr);
  std::string dataFile = argv[1];

  auto& net = galois::runtime::getSystemNetworkInterface();

  galois::StatTimer totalTimer("TimerTotal");
  totalTimer.start();

  std::unique_ptr<Graph> graph;
  std::tie(graph, syncSubstrate) =
      distGraphInitialization<NodeData, EdgeData>();
  if (net.ID == 0) {
    galois::gPrint("Global #Nodes = ", graph->globalSize(), "\n");
    galois::gPrint("Global #Edges = ", graph->globalSizeEdges(), "\n");
  }
  galois::gPrint(net.ID, "#Nodes = ", graph->size(), "\n");
  galois::gPrint(net.ID, "#Edges = ", graph->sizeEdges(), "\n");

  bitset_level_id.resize(graph->size());

  inst = std::make_unique<Instrument<Graph>>();
  inst->init(net.ID, net.Num, graph);

  using NodeBag = galois::InsertBag<Graph::GraphNode>;
  using EdgeBag = galois::InsertBag<std::pair<Graph::GraphNode, Graph::GraphNode>>;
  std::unique_ptr<NodeBag> ego_nodes = std::make_unique<NodeBag>();
  std::unique_ptr<EdgeBag> ego_edges = std::make_unique<EdgeBag>();
  std::vector<uint32_t> ego_nodes_vec;
  std::shared_ptr<galois::substrate::SimpleLock> vector_lock = std::make_shared<galois::substrate::SimpleLock>();

  galois::DGAccumulator<uint64_t> perHostSourceCounter;
  perHostSourceCounter.reset();
  std::unique_ptr<NodeBag> sources = std::make_unique<NodeBag>();
  galois::do_all(
      galois::iterate(graph->allNodesWithEdgesRange()),
      [&](auto& node) {
        if (node > graph->numMasters()) { // mirror node
          sources->emplace(node);
          return;
        }
        // bfs to detect mirror accessibility
        std::unique_ptr<std::set<uint64_t>> next = std::make_unique<std::set<uint64_t>>();
        std::unique_ptr<std::set<uint64_t>> curr = std::make_unique<std::set<uint64_t>>();
        curr->insert(node);
        for (auto i = 0ul; i < levels.size(); i++) {
          for (auto&& src : *curr) {
            auto j = levels[i];
            for (auto&& e : graph->edges(src)) {
              if (j == 0) {
                break;
              }
              auto dst = graph->getEdgeDst(e);
              if (dst > graph->numMasters()) { // mirror node accessible
                sources->emplace(node);
                return;
              }
              next->insert(dst);
              j--;
            }
          }
          if (next->empty()) {
            return;
          }
          std::swap(curr, next);
          next->clear();
        }
      },
      galois::loopname("sources"));
  std::unordered_set<Graph::GraphNode> source_set(sources->begin(), sources->end());
  perHostSourceCounter += source_set.size();
  galois::gPrint(net.ID, " ", perHostSourceCounter.read_local(), "\n");
  auto totalNumSources = perHostSourceCounter.reduce();
  if (net.ID == 0) {
    galois::gPrint("TOTAL SOURCES: ", totalNumSources, "\n");
  }

  std::unique_ptr<NodeBag> curr = std::make_unique<NodeBag>();
  std::unique_ptr<NodeBag> next = std::make_unique<NodeBag>();

  std::vector<galois::DGAccumulator<uint64_t>> perHostNodeCounter(net.Num);

  auto totalNumThreads = galois::runtime::activeThreads;
  galois::substrate::PerThreadStorage<uint64_t> perThreadNum;

  // std::random_device rd;  // a seed source for the random number engine
  // std::mt19937 gen(rd()); // mersenne_twister_engine seeded with rd()
  std::mt19937 gen(2); // mersenne_twister_engine seeded with rd()
  // std::uniform_int_distribution<> distrib(0, numRuns);
  std::vector<Graph::GraphNode> sample_sources;
  std::sample(source_set.begin(), source_set.end(), std::back_inserter(sample_sources), (int)numRuns, gen);
  
  initializeWorkset(*graph);
  
  // galois::DGAccumulator<int> skippedRuns;
  // skippedRuns.reset();
  if (net.ID == 0 && sample_sources.size() < (unsigned long int)numRuns) {
      galois::gPrint("Host 0 does not have enough source candidates!\n");
      return 1;
  }

  for (auto run = 0; run < numRuns; ++run) {
    // start_node = distrib(gen);

    // if (graph->isLocal(start_node)) {
    //   ego_nodes->emplace(graph->getLID(start_node));
    // }
    if (net.ID == 0) {
        ego_nodes->emplace(sample_sources[run]);
        vector_lock->lock();
        ego_nodes_vec.push_back(sample_sources[run]);
        vector_lock->unlock();
    }
    
    galois::gPrint("[", net.ID, "] Run ", run, " started\n");
    inst->log_run(run);

    curr->clear();
    next->clear();

    for (uint64_t i = 0; i < net.Num; i++) {
      perHostNodeCounter[i].reset();
    }

    initializeGraph(*graph);
    galois::runtime::getHostBarrier().wait();

    std::string timer_str("Timer_" + std::to_string(run));
    galois::StatTimer mainTimer(timer_str.c_str());
    mainTimer.start();
    if (net.ID == 0) {
      start_node = graph->getGID(sample_sources[run]);
      initializeWorkList(*graph, *next, perHostNodeCounter[net.ID]);
    }

    inst->init_reuse();

    for (uint64_t level = 0; level < levels.size(); level++) {
      // compute how many work items on this host
      uint64_t totalNumCandidates  = 0;
      uint64_t prefixNumCandidates = 0;
      uint64_t myNumCandidates     = perHostNodeCounter[net.ID].read_local();
      for (uint64_t i = 0; i < perHostNodeCounter.size(); i++) {
        uint64_t numNodes = perHostNodeCounter[i].reduce();
        totalNumCandidates += numNodes;
        if (i < net.ID) {
          prefixNumCandidates += numNodes;
        }
        perHostNodeCounter[i].reset();
      }

      if (totalNumCandidates == 0) {
        galois::gDebug("No neighboring nodes found at level ", level - 1,
                       "; early stop\n");
        break;
      }

      // balance workload
      const uint64_t hostStart = levels[level] * prefixNumCandidates / totalNumCandidates,
                     hostStop = levels[level] * (prefixNumCandidates + myNumCandidates) / totalNumCandidates;
      const uint64_t hostNumWorkItems = hostStop - hostStart;

      galois::on_each([&](const unsigned tid, const unsigned) {
        *perThreadNum.getLocal() = tid;
      });

      std::swap(curr, next);
      next->clear();

      auto curr_end = curr->begin();
      for (uint64_t _ = 0; _ < hostNumWorkItems; _++, curr_end++) {
        if (curr_end == curr->end()) {
          break;
        }
      }

      galois::do_all(
          galois::iterate(curr->begin(), curr_end),
          [&](auto& node) {
            uint64_t& n = *perThreadNum.getLocal();
            if (n >= hostNumWorkItems)
              return;

            auto& data = graph->getData(node);
            auto old_level = galois::atomicMin(data.level_id, level);
            if (old_level > level) {
              inst->record_write_random(node);
              bitset_level_id.set(node);
              n += totalNumThreads;
              ego_nodes->emplace(node);
              vector_lock->lock();
              ego_nodes_vec.push_back(node);
              vector_lock->unlock();
            }
          },
          galois::loopname("LevelAssignment"));

      syncSubstrate->sync<writeDestination, readSource, Reduce_min_level_id, Bitset_level_id, /*async*/ false>("LevelAssignment");

      galois::do_all(
          galois::iterate(curr->begin(), curr_end),
          [&](auto& node) {
            auto& data = graph->getData(node);
            if (data.level_id.load(std::memory_order_relaxed) == level) {
              for (auto it = graph->edge_begin(node); it != graph->edge_end(node); it++) {
                Graph::GraphNode dst = graph->getEdgeDst(it);
                if (node == dst)
                  continue; // ignore self loops
                auto& ddata = graph->getData(dst);
                inst->record_read_random(dst);
                if (ddata.level_id.load(std::memory_order_relaxed) <= level)
                  continue; // deduplicate
                next->emplace(dst);
                perHostNodeCounter[net.ID] += 1;
              }
            }
          },
          galois::loopname("NextWL"));
    }

    // assign ego graph id
    for (uint64_t i = 0; i < net.Num; i++) {
      perHostNodeCounter[i].reset();
    }
    galois::do_all(galois::iterate(*ego_nodes), [&](Graph::GraphNode) { perHostNodeCounter[net.ID] += 1; });
    std::vector<uint64_t> numNodesPrefix(1 + net.Num, 0);
    for (uint64_t i = 1; i <= net.Num; i++) {
      numNodesPrefix[i] = numNodesPrefix[i - 1] + perHostNodeCounter[i - 1].reduce();
    }
    uint64_t numLocalNodes = perHostNodeCounter[net.ID].read_local();

    galois::on_each([&](const unsigned tid, const unsigned numT) {
      auto start = numLocalNodes < numT ? tid : tid * numLocalNodes / numT,
           stop  = numLocalNodes < numT ? tid + 1 : (tid + 1) * numLocalNodes / numT;
      if (start >= numLocalNodes) {
        return;
      }
      auto begin  = std::next(ego_nodes->begin(), start);
      uint64_t id = start;
      for (auto it = begin; it != ego_nodes->end() && id != stop; it++, id++) {
        auto& nodeData = graph->getData(*it);
        nodeData.lid = numNodesPrefix[net.ID] + id;
      }
    });

    std::sort(ego_nodes_vec.begin(), ego_nodes_vec.end());

    galois::do_all(galois::iterate(ego_nodes_vec), [&](auto src) {
      auto& sdata = graph->getData(src);
      for (auto it = graph->edge_begin(src); it != graph->edge_end(src); it++) {
        auto dst = graph->getEdgeDst(it);
        if (dst < src) {
            auto& ddata = graph->getData(dst);
            inst->record_read_random(dst);
            if (ddata.lid != NaN) {
              ego_edges->emplace(sdata.lid, ddata.lid);
            }
        } else {
            break;
        }
      }
    });
/*
    galois::do_all(galois::iterate(*ego_nodes), [&](auto src) {
      auto& sdata = graph->getData(src);
      for (auto it = graph->edge_begin(src); it != graph->edge_end(src); it++) {
        auto dst = graph->getEdgeDst(it);
        auto& ddata = graph->getData(dst);
        inst->record_read_random(dst);
        if (src != dst && ddata.lid != NaN) {
          ego_edges->emplace(sdata.lid, ddata.lid);
        }
      }
    });
*/
    inst->log_access();
    inst->log_distance();

    mainTimer.stop();

    // if ((run + 1) != numRuns) {
    syncSubstrate->set_num_run(run + 1);
    bitset_level_id.reset();
    initializeGraph(*graph);
    ego_nodes->clear();
    ego_edges->clear();
    // }
  }

  inst->log_workset();

  totalTimer.stop();
  
  // auto totalSkippedRuns = skippedRuns.reduce();
  // if (net.ID == 0) {
    // galois::gPrint("Actual number of runs : ", numRuns - totalSkippedRuns, "\n");
  // }

  // uint64_t numNodes = 0, numEdges = 0;
  // for (auto i = ego_nodes->begin(); i != ego_nodes->end(); i++) {
  //   numNodes++;
  //   auto& data = graph->getData(*i);
  //   galois::gPrint("node ", data.lid, " at level ",
  //                  data.level_id.load(std::memory_order_relaxed),
  //                  " was originally node ", *i, "\n");
  // }

  // std::unordered_set<std::pair<Graph::GraphNode, Graph::GraphNode>,
  //                    boost::hash<std::pair<Graph::GraphNode,
  //                    Graph::GraphNode>>>
  //     ego_edges_set(ego_edges->begin(), ego_edges->end()); // ugly
  //     deduplication
  // for (auto i = ego_edges_set.begin(); i != ego_edges_set.end(); i++) {
  //   numEdges++;
  //   auto [src, dst] = *i;
  //   galois::gPrint(src, "->", dst, "\n");
  // }
  // galois::gPrint("#Ego graph nodes = ", numNodes, "\n");
  // galois::gPrint("#Ego graph edges = ", numEdges, "\n");

  inst.reset();

  return 0;
}
