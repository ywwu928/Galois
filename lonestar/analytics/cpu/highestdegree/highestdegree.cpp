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

#include "Lonestar/BoilerPlate.h"
#include "MinHeap.h"
#include "galois/Bag.h"
#include "galois/Galois.h"
#include "galois/Timer.h"
#include "galois/graphs/LCGraph.h"
#include "galois/graphs/TypeTraits.h"

#include <iostream>
#include <fstream>
#include <sstream>

namespace cll = llvm::cl;

static const char* name = "Highest Degree";

static const char* desc = "Prints out the highest degree node IDs of a graph";

static const char* url = "highest_degree";

static cll::opt<std::string> inputFile(cll::Positional, cll::desc("<input file>"), cll::Required);

constexpr static const unsigned CHUNK_SIZE = 16;

static cll::opt<int> size("size", cll::desc("Number of highest degree nodes to be printed"), cll::init(10));

static cll::opt<std::string> fileName("fileName", cll::desc("Name of the output file"), cll::init("temp"));

typedef galois::graphs::LC_CSR_Graph<uint32_t, void>::with_no_lockable<true>::type Graph;
typedef typename Graph::GraphNode GNode;

// MinHeap heap(size);

void highestdegree(Graph& graph, MinHeap& heap) {
/*  
  galois::do_all(
	  galois::iterate(graph),
	  [&](const GNode& src) {
	      auto& sdata = graph.getData(src);

	      sdata = graph.getDegree(src);
		  //! For each out-going neighbors.
		  // for (auto jj: graph.out_edges(src)) {
			  // GNode dst    = graph.getEdgeDst(jj);
			  // auto& ddata  = graph.getData(dst);
			  // sdata += 1;
		  // }
	  },
	  galois::steal(), galois::chunk_size<CHUNK_SIZE>(),
	  galois::loopname("CountDegree"), galois::no_stats());
*/  
  galois::do_all(
	  galois::iterate(graph),
	  [&](const GNode& src) {
	      auto& sdata = graph.getData(src);
		  if (heap.size() < size) {
			  heap.insertKey(sdata, graph.getId(src));
		  }
		  else {
			  if (heap.getMin() < sdata) {
				  heap.extractMin();
				  heap.insertKey(sdata, graph.getId(src));
			  }
		  }
	  },
	  galois::steal(), galois::chunk_size<CHUNK_SIZE>(),
	  galois::loopname("FormHeap"), galois::no_stats());

}

int main(int argc, char** argv) {
  galois::SharedMemSys G;
  LonestarStart(argc, argv, name, desc, url, &inputFile);

  galois::StatTimer totalTime("TimerTotal");
  totalTime.start();

  Graph graph;
  
  std::cout << "Reading from file: " << inputFile << "\n";
  galois::graphs::readGraph(graph, inputFile);
  std::cout << "Read " << graph.size() << " nodes, " << graph.sizeEdges() << " edges\n";

  size_t approxNodeData = 4 * (graph.size() + graph.sizeEdges());
  galois::preAlloc(8 * numThreads + approxNodeData / galois::runtime::pagePoolSize());
  galois::reportPageAlloc("MeminfoPre");
  
  galois::do_all(galois::iterate(graph),
                 [&graph](GNode n) { graph.getData(n) = graph.getDegree(n); });

  MinHeap heap(size);

  galois::StatTimer execTime("Timer_0");
  execTime.start();

  highestdegree(graph, heap);

  execTime.stop();

  heap.printCount();
  heap.printId();
  
  std::ofstream file;
  file.open(fileName + "_top" + std::to_string(size));
  heap.writeId(file);

  galois::reportPageAlloc("MeminfoPost");

  totalTime.stop();

  return 0;
}
