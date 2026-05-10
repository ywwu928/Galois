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

#include <vector>
#include <algorithm>

#include "property.h"

static std::string REGION_NAME = "Property";
static std::string REGION_NAME_RUN;

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

struct NodeData {
    uint32_t type;
    uint32_t index;
    uint32_t dummy;
};

struct EdgeData {
    uint32_t type;
    uint64_t index;
};

galois::DynamicBitSet bitset_dummy;

typedef galois::graphs::DistGraph<struct NodeData, struct EdgeData> Graph;
typedef typename Graph::GraphNode GNode;

std::unique_ptr<galois::graphs::GluonSubstrate<Graph, uint32_t>> syncSubstrate;

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

    bitset_dummy.resize(hg->actualSize());
    
    galois::gPrint("[", net.ID, "] TypeAssignment begin\n");
    TypeAssignment(*hg);
    galois::gPrint("[", net.ID, "] TypeAssignment end\n");
  
    galois::gPrint("[", net.ID, "] PropertyConstruction begin\n");
    PropertyConstruction();
    galois::gPrint("[", net.ID, "] PropertyConstruction end\n");

    galois::runtime::getHostBarrier().wait();

    if (net.ID == 0) {
        galois::gPrint("LPG construction succeed!\n");
    }
  
    net.applicationDone();

    return 0;
}
