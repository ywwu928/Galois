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
#include "MinHeap.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <limits>

enum { CPU, GPU_CUDA };
int personality = CPU;

constexpr static const char* const REGION_NAME = "Highest Degree";

/******************************************************************************/
/* Declaration of command line arguments */
/******************************************************************************/

namespace cll = llvm::cl;

static cll::opt<int> size("size", cll::desc("Number of highest degree nodes to be printed"), cll::init(10));

static cll::opt<std::string> fileName("fileName", cll::desc("Name of the output file"), cll::init("temp"));

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

const uint32_t infinity = std::numeric_limits<uint32_t>::max() / 4;

struct NodeData {
  std::atomic<uint32_t> degree;
};

galois::DynamicBitSet bitset_degree;

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph>> syncSubstrate;

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/
struct InitializeGraph {
  Graph* graph;

  InitializeGraph(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
    const auto& allNodes = _graph.allNodesRange();

    if (personality == GPU_CUDA) {
      abort();
    } else if (personality == CPU) {
      galois::do_all(
          galois::iterate(allNodes.begin(), allNodes.end()),
          InitializeGraph{&_graph}, galois::no_stats(),
          galois::loopname(
              syncSubstrate->get_run_identifier("InitializeGraph").c_str()));
    }
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
	sdata.degree = graph->getDegree(src);
  }
};

struct HighestDegree {
  Graph* graph;

  MinHeap& heap;
  
  std::ofstream& file;

  HighestDegree(Graph* _graph,
	  MinHeap& _heap,
      std::ofstream& _file)
      : graph(_graph),
	  heap(_heap),
      file(_file) {}

  void static go(Graph& _graph,
		  		  MinHeap& heap,
                  std::ofstream& file) {

    const auto& nodesWithEdges = _graph.allNodesWithEdgesRange();

    // syncSubstrate->set_num_round(_num_iterations);
    
    if (personality == GPU_CUDA) {
        abort();
    } else if (personality == CPU) {
        galois::do_all(
            galois::iterate(nodesWithEdges),
            HighestDegree(&_graph,
				heap,
                file),
            galois::steal(),
            galois::no_stats(),
            galois::loopname(syncSubstrate->get_run_identifier("HighestDegree").c_str()));
    }

	heap.printCount();
	heap.printId();

	heap.writeId(file);
      
      // syncSubstrate->sync<writeDestination, readSource, Reduce_min_dist_current, Bitset_dist_current, async>("BFS");
      
  }

  void operator()(GNode src) const {
    NodeData& snode = graph->getData(src);

    if (heap.size() < size) {
	    heap.insertKey(snode.degree, graph->getGID(src));
    }
    else {
	    if (heap.getMin() < snode.degree) {
		    heap.extractMin();
		    heap.insertKey(snode.degree, graph->getGID(src));
	    }
    }
  }
};

/******************************************************************************/
/* Main */
/******************************************************************************/

constexpr static const char* const name = "Highest Degree";

constexpr static const char* const desc = "Prints out the highest degree node IDs of a graph";

constexpr static const char* const url = "highest_degree";

int main(int argc, char** argv) {
  galois::DistMemSys G;
  DistBenchStart(argc, argv, name, desc, url);

  const auto& net = galois::runtime::getSystemNetworkInterface();

  galois::StatTimer StatTimer_total("TimerTotal", REGION_NAME);

  StatTimer_total.start();

  std::unique_ptr<Graph> hg;
  std::tie(hg, syncSubstrate) = distGraphInitialization<NodeData, void>();

  // bitset comm setup
  bitset_degree.resize(hg->size());

  galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");

  InitializeGraph::go((*hg));
  galois::runtime::getHostBarrier().wait();
  
  uint64_t host_id = galois::runtime::getSystemNetworkInterface().ID;

  MinHeap heap(size);
  
  std::ofstream file;
  file.open(fileName + "_top" + std::to_string(size) + "_id" + std::to_string(host_id));

  galois::gPrint("[", net.ID, "] Highest Degree called\n");
  std::string timer_str("Timer");
  galois::StatTimer StatTimer_main(timer_str.c_str(), REGION_NAME);

  StatTimer_main.start();
  HighestDegree::go(*hg, heap, file);
  StatTimer_main.stop();

  StatTimer_total.stop();

  return 0;
}
