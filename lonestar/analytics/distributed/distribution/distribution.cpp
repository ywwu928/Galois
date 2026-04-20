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
#include "galois/runtime/Tracer.h"
#include "galois/runtime/Profile.h"

#include <fstream>
#include <vector>
#include <cstdint>
#include <iostream>

static std::string REGION_NAME = "Distribution";

/******************************************************************************/
/* Declaration of command line arguments */
/******************************************************************************/
namespace cll = llvm::cl;

static cll::opt<std::string> nodeTypeFile("nodeTypeFile",
                                 cll::desc("path to the node type file"));

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

struct NodeData {
  uint32_t type;
};

typedef galois::graphs::DistGraph<NodeData, uint32_t> Graph;
typedef typename Graph::GraphNode GNode;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph, uint32_t>> syncSubstrate;

std::vector<uint32_t> node_types;

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
        uint64_t gid = graph->getGID(src);
        NodeData& sdata = graph->getData(src);
        sdata.type = node_types[gid];
    }
};

struct DistributionVertex {
    Graph* graph;

    std::vector<galois::GAccumulator<uint64_t>>& vertex_type;

    DistributionVertex(
        Graph* _graph,
        std::vector<galois::GAccumulator<uint64_t>>& _vertex_type
    ) : graph(_graph),
        vertex_type(_vertex_type) {}

    void static go(
        Graph& _graph,
        std::vector<galois::GAccumulator<uint64_t>>& master_type,
        std::vector<galois::GAccumulator<uint64_t>>& mirror_type
    ) {
      
        for (int i=0; i<9; i++) {
            master_type[i].reset();
            mirror_type[i].reset();
        }

        const auto& masterNodes = _graph.masterNodesRange();

        galois::do_all(
            galois::iterate(masterNodes),
            DistributionVertex(
                &_graph,
                master_type
            ),
            galois::no_stats()
        );

        uint64_t count = 0;
    
        for (int i=0; i<9; i++) {
            count = master_type[i].reduce();
            galois::runtime::reportStat_Tsum(REGION_NAME.c_str(), "MasterCount_Type_" + std::to_string(i), count);
        }
        
        const auto& mirrorNodes = _graph.mirrorNodesRange();

        galois::do_all(
            galois::iterate(mirrorNodes),
            DistributionVertex(
                &_graph,
                mirror_type
            ),
            galois::no_stats()
        );
    
        for (int i=0; i<9; i++) {
            count = mirror_type[i].reduce();
            galois::runtime::reportStat_Tsum(REGION_NAME.c_str(), "MirrorCount_Type_" + std::to_string(i), count);
        }
    }

    void operator()(GNode src) const {
        NodeData& sdata = graph->getData(src);
        vertex_type[sdata.type] += 1;
    }
};

struct DistributionEdge {
    Graph* graph;
    
    std::vector<galois::GAccumulator<uint64_t>>& edge_type;

    DistributionEdge(
        Graph* _graph,
        std::vector<galois::GAccumulator<uint64_t>>& _edge_type
    ) : graph(_graph),
        edge_type(_edge_type) {}

    void static go(
        Graph& _graph,
        std::vector<galois::GAccumulator<uint64_t>>& edge_type
    ) {
    
        for (int i=0; i<22; i++) {
            edge_type[i].reset();
        }

        const auto& masterNodes = _graph.masterNodesRange();

        galois::do_all(
            galois::iterate(masterNodes),
            DistributionEdge(
                &_graph,
                edge_type
            ),
            galois::no_stats()
        );

        uint64_t count = 0;
    
        for (int i=0; i<22; i++) {
            count = edge_type[i].reduce();
            galois::runtime::reportStat_Tsum(REGION_NAME.c_str(), "EdgeCount_Type_" + std::to_string(i), count);
        }
    }

    void operator()(GNode src) const {
        for (auto edge : graph->outEdges(src)) {
            uint32_t edgeType = graph->getEdgeData(edge);
            edge_type[edgeType] += 1;
        }
    }
};

/******************************************************************************/
/* Main */
/******************************************************************************/

constexpr static const char* const name = "Distribution";
constexpr static const char* const desc = "Distribution";
constexpr static const char* const url = 0;

int main(int argc, char** argv) {
    galois::DistMemSys G;
    DistBenchStart(argc, argv, name, desc, url);

    auto& net = galois::runtime::getSystemNetworkInterface();
  
    std::unique_ptr<Graph> hg;
    std::tie(hg, syncSubstrate) = distGraphInitialization<NodeData, uint32_t, uint32_t>();

    galois::runtime::getHostBarrier().wait();
    net.partitionDone();

    galois::gPrint("[", net.ID, "] Load node type file\n");
    std::ifstream file(nodeTypeFile);
    if (!file) {
        galois::gPrint("Failed to open node type file\n");
        return 1;
    }
    node_types.reserve(hg->globalSize());
    uint32_t x;
    while (file >> x) {   // reads numbers separated by whitespace/newlines
        node_types.push_back(x);
    }

    galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");
    InitializeGraph::go((*hg));
    galois::runtime::getHostBarrier().wait();

    std::vector<galois::GAccumulator<uint64_t>> master_type;
    std::vector<galois::GAccumulator<uint64_t>> mirror_type;
    std::vector<galois::GAccumulator<uint64_t>> edge_type;

    master_type.resize(9);
    mirror_type.resize(9);
    edge_type.resize(22);

    galois::gPrint("[", net.ID, "] Distribution::go called\n");
    
    galois::runtime::getHostBarrier().wait();

    DistributionVertex::go(*hg, master_type, mirror_type);
    DistributionEdge::go(*hg, edge_type);
  
    net.applicationDone();

    return 0;
}
