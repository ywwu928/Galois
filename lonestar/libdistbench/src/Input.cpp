/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2019, The University of Texas at Austin. All rights reserved.
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

/**
 * @file Reader.cpp
 *
 * Contains definitions for command line arguments related to distributed
 * graph loading.
 */

#include "DistBench/Input.h"

using namespace galois::graphs;

namespace cll = llvm::cl;

cll::opt<std::string> inputFile(cll::Positional, cll::desc("<input file>"),
                                cll::Required);
cll::opt<std::string> inputFileTranspose("graphTranspose",
                                         cll::desc("<input file, transposed>"),
                                         cll::init(""));
cll::opt<bool>
    symmetricGraph("symmetricGraph",
                   cll::desc("Specify that the input graph is symmetric"),
                   cll::init(false));

cll::opt<PARTITIONING_SCHEME> partitionScheme(
    "partition", cll::desc("Type of partitioning."),
    cll::values(
        clEnumValN(HASH, "hash", "Hashing"),
        clEnumValN(OEC, "oec", "Outgoing Edge-Cut (default)"),
        clEnumValN(IEC, "iec", "Incoming Edge-Cut"),
        clEnumValN(HOVC, "hovc", "Outgoing Hybrid Vertex-Cut"),
        clEnumValN(HIVC, "hivc", "Incoming Hybrid Vertex-Cut"),
        clEnumValN(CART_VCUT, "cvc", "Cartesian Vertex-Cut of oec"),
        clEnumValN(CART_VCUT_IEC, "cvc-iec", "Cartesian Vertex-Cut of iec"),
        // clEnumValN(CEC, "cec", "Custom edge cut from vertexID mapping"),
        clEnumValN(GINGER_O, "ginger-o", "ginger, outgiong edges, using CuSP"),
        clEnumValN(GINGER_I, "ginger-i", "ginger, incoming edges, using CuSP"),
        clEnumValN(FENNEL_O, "fennel-o",
                   "fennel, outgoing edge cut, using CuSP"),
        clEnumValN(FENNEL_I, "fennel-i",
                   "fennel, incoming edge cut, using CuSP"),
        clEnumValN(SUGAR_O, "sugar-o",
                   "fennel, incoming edge cut, using CuSP")),
    cll::init(OEC));

cll::opt<bool> readFromFile("readFromFile",
                            cll::desc("Set this flag if graph is to be "
                                      "constructed from file (file must be "
                                      "created by Abelian CSR)"),
                            cll::init(false), cll::Hidden);

cll::opt<std::string>
    localGraphFileName("localGraphFileName",
                       cll::desc("Name of the local file to construct "
                                 "local graph (file must be created by "
                                 "Abelian CSR)"),
                       cll::init("local_graph"), cll::Hidden);

cll::opt<bool> saveLocalGraph("saveLocalGraph",
                              cll::desc("Set to save the local CSR graph"),
                              cll::init(false), cll::Hidden);

cll::opt<std::string> mastersFile("mastersFile",
                                  cll::desc("File specifying masters blocking"),
                                  cll::init(""), cll::Hidden);

#ifdef GALOIS_FULL_MIRRORING
int mirrorThreshold = 0;
#elif defined(GALOIS_NO_MIRRORING)
int mirrorThreshold = -1;
#else
cll::opt<int> mirrorThreshold("mirrorThreshold",
                                  cll::desc("Threshold for the incoming degree of a proxy to be actually mirrored (0 means full mirroring & -1 means no mirroring)"),
                                  cll::init(0));
#endif

#ifdef GALOIS_FULL_MIRRORING
uint32_t highDegreeFactor = 0;
#elif defined(GALOIS_NO_MIRRORING)
uint32_t highDegreeFactor = 0;
#else
cll::opt<uint32_t> highDegreeFactor("highDegreeFactor",
                                  cll::desc("Factor to determine the threshold for high incoming degree mirrors to not be mirrored"),
                                  cll::init(0));
#endif

cll::opt<uint32_t> dataSizeRatio("dataSizeRatio",
                                  cll::desc("The ratio of the node data size to the edge data size"),
                                  cll::init(1));

cll::opt<uint32_t> stopThreshold("stopThreshold",
                                 cll::desc("Threshold to stop sweeping memory overhead"),
                                 cll::init(1));
