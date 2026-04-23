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

#include <iostream>
#include <sstream>
#include <limits>
#include <random>
#include <vector>

#include "snb_data_structure.h"

static std::string REGION_NAME = "Property";
static std::string REGION_NAME_RUN;

/******************************************************************************/
/* Declaration of command line arguments */
/******************************************************************************/

namespace cll = llvm::cl;
static cll::opt<unsigned> rseed("rseed", cll::desc("The random seed for choosing the hosts (default value 0)"), cll::init(0));

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

struct NodeData {
    uint32_t type;
    uint32_t index;
    std::atomic<uint32_t> dist_current;
};

struct EdgeData {
    uint32_t type;
    uint32_t index;
};

typedef galois::graphs::DistGraph<NodeData, EdgeData> Graph;
typedef typename Graph::GraphNode GNode;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph, uint32_t>> syncSubstrate;

// Setup Seeding Information
std::mt19937 generator(rseed);

std::vector<double> master_weights = {0.0025, 0.0005, 0.0051, 0.0, 0.645, 0.0284, 0.0031, 0.3154};
std::discrete_distribution<> master_distribution(master_weights.begin(), master_weights.end());

std::vector<double> mirror_weights = {0.00417, 0.00353, 0.0681, 0.00022, 0.33865, 0.0, 0.05803, 0.5273};
std::discrete_distribution<> mirror_distribution(mirror_weights.begin(), mirror_weights.end());

std::vector<double> edge_weights = {
    0.0005, 0.0001, 0.0009, 0.0, 0.1189,
    0.1564, 0.1189, 0.0603, 0.0586, 0.0582,
    0.0987, 0.0180, 0.0133, 0.0006, 0.0105,
    0.0834, 0.0436, 0.0017, 0.0582, 0.0413,
    0.0582
};
std::discrete_distribution<> edge_distribution(edge_weights.begin(), edge_weights.end());

std::vector<Organization> organization_memory;
std::vector<Place> place_memory;
std::vector<Tag> tag_memory;
std::vector<TagClass> tagclass_memory;
std::vector<Comment> comment_memory;
std::vector<Forum> forum_memory;
std::vector<Person> person_memory;
std::vector<Post> post_memory;

std::vector<Forum_hasMemberOrModerator_Person> forum_person_memory;
std::vector<Person_knows_Person> person_person_memory;
std::vector<Person_likes_Comment> person_comment_memory;
std::vector<Person_likes_Post> person_post_memory;
std::vector<Person_workOrStudyAt_Organization> person_organization_memory;

uint32_t vertex_index[8];
uint32_t edge_index[21];

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/

void AssignProperty(Graph& graph) {
    uint32_t type;

    for (int i=0; i<8; i++) {
        vertex_index[i] = 0;
    }

    for (int i=0; i<21; i++) {
        edge_index[i] = 0;
    }

    // masters and edges
    for (uint32_t lid=0; lid<graph.numMasters(); lid++) {
        NodeData& node_data = graph.getData(lid);

        type = master_distribution(generator);
        node_data.type = type;
        node_data.index = vertex_index[type];
        vertex_index[type]++;
    
        for (auto edge : graph.outEdges(lid)) {
            EdgeData& edge_data = graph.getEdgeData(edge);
            
            type = edge_distribution(generator);
            edge_data.type = type;
            edge_data.index = edge_index[type];
            edge_index[type]++;
        }
    }
    
    // mirrors
    for (uint32_t offset=0; offset<graph.numMirrors(); offset++) {
        uint32_t lid = graph.numMasters() + offset;
        NodeData& node_data = graph.getData(lid);

        type = mirror_distribution(generator);
        node_data.type = type;
        node_data.index = vertex_index[type];
        vertex_index[type]++;
    }
}

/******************************************************************************/
/* Main */
/******************************************************************************/

constexpr static const char* const name = "Construct Labeled Property Graph";
constexpr static const char* const desc = "Construct Labeled Property Graph";
constexpr static const char* const url  = nullptr;

int main(int argc, char** argv) {
    galois::DistMemSys G;
    DistBenchStart(argc, argv, name, desc, url);

    auto& net = galois::runtime::getSystemNetworkInterface();

    if (partitionScheme != OEC) {
        galois::gPrint("This repo only supports OEC\n");
        return 1;
    }

    std::unique_ptr<Graph> hg;
    std::tie(hg, syncSubstrate) = distGraphInitialization<NodeData, EdgeData, uint32_t>();

    net.allocateBufferPool();

    galois::runtime::getHostBarrier().wait();
    net.partitionDone();
    
    galois::gPrint("[", net.ID, "] AssignProperty begin\n");
    AssignProperty(*hg);
    galois::gPrint("[", net.ID, "] AssignProperty end\n");
  
    galois::gPrint("[", net.ID, "] ConstructProperty begin\n");
    organization_memory.resize(vertex_index[0]);
    place_memory.resize(vertex_index[1]);
    tag_memory.resize(vertex_index[2]);
    tagclass_memory.resize(vertex_index[3]);
    comment_memory.resize(vertex_index[4]);
    forum_memory.resize(vertex_index[5]);
    person_memory.resize(vertex_index[6]);
    post_memory.resize(vertex_index[7]);
    
    forum_person_memory.resize(edge_index[10]);
    person_person_memory.resize(edge_index[14]);
    person_comment_memory.resize(edge_index[15]);
    person_post_memory.resize(edge_index[16]);
    person_organization_memory.resize(edge_index[17]);
    galois::gPrint("[", net.ID, "] ConstructProperty end\n");

    galois::runtime::getHostBarrier().wait();

    if (net.ID == 0) {
#ifdef GALOIS_FULL_MIRRORING
        galois::gPrint("LPG construction succeed : Full Mirroring\n");
#endif

#ifdef GALOIS_STATIC_PARTIAL_MIRRORING
        galois::gPrint("LPG construction succeed : Static Partial Mirroring\n");
#endif

#ifdef GALOIS_MIRROR_FREE
        galois::gPrint("LPG construction succeed : Mirror-Free\n");
#endif
    }
  
    net.applicationDone();

    return 0;
}
