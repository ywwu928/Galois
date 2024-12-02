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

constexpr static const char* const REGION_NAME = "Max Degree";

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

namespace cll = llvm::cl;
struct NodeData {
  uint32_t degree;
};

typedef galois::graphs::DistGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;

std::unique_ptr<galois::DGReduceMax<uint32_t>> max_degree;

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/
struct InitializeGraph {
  Graph* graph;

  InitializeGraph(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
    const auto& allNodes = _graph.allNodesRange();

    galois::do_all(
        galois::iterate(allNodes.begin(), allNodes.end()),
        InitializeGraph{&_graph}, galois::no_stats());
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
	sdata.degree = graph->getDegree(src);
  }
};

struct MaxDegree {
  Graph* graph;

  MaxDegree(Graph* _graph) : graph(_graph) {}

  void static go(Graph& _graph) {
    max_degree->reset();

    const auto& allNodes = _graph.allNodesRange();

    galois::do_all(
        galois::iterate(allNodes),
        MaxDegree(&_graph),
        galois::steal(),
        galois::no_stats());

    max_degree->reduce();

    if (galois::runtime::getSystemNetworkInterface().ID == 0) {
      galois::gPrint("maximum degree of graph =  ", max_degree->read(), "\n");
    }
  }

  void operator()(GNode src) const {
    NodeData& snode = graph->getData(src);
    max_degree->update(snode.degree);
  }
};

/******************************************************************************/
/* Main */
/******************************************************************************/

constexpr static const char* const name = "Max Degree - Compiler Generated "
                                          "Distributed Heterogeneous";
constexpr static const char* const desc = "Max Degree on Distributed "
                                          "Galois.";
constexpr static const char* const url = 0;

int main(int argc, char** argv) {
  galois::DistMemSys G;
  DistBenchStart(argc, argv, name, desc, url);

  const auto& net = galois::runtime::getSystemNetworkInterface();

  galois::StatTimer StatTimer_total("TimerTotal", REGION_NAME);

  StatTimer_total.start();

  std::vector<unsigned> scaleFactor;
  auto hg = loadDistGraph<NodeData, void, false>(scaleFactor);
    
  max_degree = std::make_unique<galois::DGReduceMax<uint32_t>>();

  galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");

  InitializeGraph::go((*hg));
  galois::runtime::getHostBarrier().wait();

  galois::gPrint("[", net.ID, "] Max Degree called\n");
  std::string timer_str("Timer");
  galois::StatTimer StatTimer_main(timer_str.c_str(), REGION_NAME);

  StatTimer_main.start();
  MaxDegree::go(*hg);
  StatTimer_main.stop();

  StatTimer_total.stop();

  return 0;
}
