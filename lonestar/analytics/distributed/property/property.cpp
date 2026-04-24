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

#include <random>
#include <vector>
#include <algorithm>

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
    uint64_t index;
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

std::unique_ptr<Organization[]> organization_memory;
std::unique_ptr<Place[]> place_memory;
std::unique_ptr<Tag[]> tag_memory;
std::unique_ptr<TagClass[]> tagclass_memory;
std::unique_ptr<Comment[]> comment_memory;
std::unique_ptr<Forum[]> forum_memory;
std::unique_ptr<Person[]> person_memory;
std::unique_ptr<Post[]> post_memory;

std::unique_ptr<Forum_hasMemberOrModerator_Person[]> forum_person_memory;
std::unique_ptr<Person_knows_Person[]> person_person_memory;
std::unique_ptr<Person_likes_Comment[]> person_comment_memory;
std::unique_ptr<Person_likes_Post[]> person_post_memory;
std::unique_ptr<Person_workOrStudyAt_Organization[]> person_organization_memory;

uint32_t vertex_counter[8];
uint64_t edge_counter[21];

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/

void TypeAssignment(Graph& graph) {
    std::unique_ptr<uint32_t[]> types;
    uint64_t max_size = std::max({graph.numMasters(), graph.numMirrors(), graph.sizeEdges()});
    types = std::make_unique<uint32_t[]>(max_size);

    // master
    for (uint32_t i=0; i<graph.numMasters(); i++) {
        types[i] = master_distribution(generator);
    }
    
    galois::on_each([&](unsigned tid, unsigned nthreads) {
        uint32_t total = graph.numMasters();
        uint32_t quotient = total / nthreads;
        uint32_t start = tid * quotient;
        uint32_t end = (tid + 1) * quotient;
        if (tid == nthreads - 1) {
            end = total;
        }

        std::vector<uint32_t> type_index;
        for (uint32_t i=0; i<8; i++) {
            type_index.emplace_back(
                std::count(types.get(), types.get() + start, i)
            );
        }
        
        for (uint32_t lid=start; lid<end; lid++) {
            NodeData& master_data = graph.getData(lid);
            master_data.type = types[lid];
            master_data.index = type_index[master_data.type];
            type_index[master_data.type]++;
        }
    });

    for (uint32_t i=0; i<8; i++) {
        vertex_counter[i] = std::count(types.get(), types.get() + graph.numMasters(), i);
    }
    
    // mirror
    for (uint32_t i=0; i<graph.numMirrors(); i++) {
        types[i] = mirror_distribution(generator);
    }
    
    galois::on_each([&](unsigned tid, unsigned nthreads) {
        uint32_t total = graph.numMirrors();
        uint32_t quotient = total / nthreads;
        uint32_t start = tid * quotient;
        uint32_t end = (tid + 1) * quotient;
        if (tid == nthreads - 1) {
            end = total;
        }

        std::vector<uint32_t> type_index;
        for (uint32_t i=0; i<8; i++) {
            type_index.emplace_back(
                vertex_counter[i] + std::count(types.get(), types.get() + start, i)
            );
        }
        
        for (uint32_t index=start; index<end; index++) {
            uint32_t lid = graph.numMasters() + index;
            NodeData& mirror_data = graph.getData(lid);
            mirror_data.type = types[index];
            mirror_data.index = type_index[mirror_data.type];
            type_index[mirror_data.type]++;
        }
    });

    for (uint32_t i=0; i<8; i++) {
        vertex_counter[i] += std::count(types.get(), types.get() + graph.numMirrors(), i);
    }
    
    // edge
    for (uint32_t i=0; i<graph.sizeEdges(); i++) {
        types[i] = edge_distribution(generator);
    }
    
    galois::on_each([&](unsigned tid, unsigned nthreads) {
        uint64_t total = graph.sizeEdges();
        uint64_t quotient = total / nthreads;
        uint64_t start = tid * quotient;
        uint64_t end = (tid + 1) * quotient;
        if (tid == nthreads - 1) {
            end = total;
        }

        std::vector<uint64_t> type_index;
        for (uint32_t i=0; i<21; i++) {
            type_index.emplace_back(
                std::count(types.get(), types.get() + start, i)
            );
        }
        
        for (uint64_t index=start; index<end; index++) {
            EdgeData& edge_data = graph.getEdgeDataDirect(index);
            edge_data.type = types[index];
            edge_data.index = type_index[edge_data.type];
            type_index[edge_data.type]++;
        }
    });

    for (uint32_t i=0; i<21; i++) {
        edge_counter[i] = std::count(types.get(), types.get() + graph.sizeEdges(), i);
    }
}

void PropertyConstruction (Graph& graph) {
    // sanity check
    uint64_t sum = 0;
    for (uint32_t i=0; i<8; i++) {
        sum += vertex_counter[i];
    }
    if (sum != (graph.numMasters() + graph.numMirrors())) {
        galois::gPrint("Vertex counts do not match!\n");
        abort();
    }
    
    sum = 0;
    for (uint32_t i=0; i<21; i++) {
        sum += edge_counter[i];
    }
    if (sum != graph.sizeEdges()) {
        galois::gPrint("Edge counts do not match!\n");
        abort();
    }

    organization_memory = std::make_unique<Organization[]>(vertex_counter[0]);
    place_memory = std::make_unique<Place[]>(vertex_counter[1]);
    tag_memory = std::make_unique<Tag[]>(vertex_counter[2]);
    tagclass_memory = std::make_unique<TagClass[]>(vertex_counter[3]);
    comment_memory = std::make_unique<Comment[]>(vertex_counter[4]);
    forum_memory = std::make_unique<Forum[]>(vertex_counter[5]);
    person_memory = std::make_unique<Person[]>(vertex_counter[6]);
    post_memory = std::make_unique<Post[]>(vertex_counter[7]);
    
    forum_person_memory = std::make_unique<Forum_hasMemberOrModerator_Person[]>(edge_counter[10]);
    person_person_memory = std::make_unique<Person_knows_Person[]>(edge_counter[14]);
    person_comment_memory = std::make_unique<Person_likes_Comment[]>(edge_counter[15]);
    person_post_memory = std::make_unique<Person_likes_Post[]>(edge_counter[16]);
    person_organization_memory = std::make_unique<Person_workOrStudyAt_Organization[]>(edge_counter[17]);
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
    
    galois::gPrint("[", net.ID, "] TypeAssignment begin\n");
    TypeAssignment(*hg);
    galois::gPrint("[", net.ID, "] TypeAssignment end\n");
  
    galois::gPrint("[", net.ID, "] PropertyConstruction begin\n");
    PropertyConstruction(*hg);
    galois::gPrint("[", net.ID, "] PropertyConstruction end\n");

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
