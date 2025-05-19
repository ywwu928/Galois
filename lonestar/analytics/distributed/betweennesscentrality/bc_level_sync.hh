/*
 * This file belongs to the Galois project, a C++ library for exploiting parallelism.
 * The code is being released under the terms of the 3-Clause BSD License (a
 * copy is located in LICENSE.txt at the top-level directory).
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

#include "galois/runtime/SyncStructures.h"

////////////////////////////////////////////////////////////////////////////
// # short paths
////////////////////////////////////////////////////////////////////////////

GALOIS_SYNC_STRUCTURE_REDUCE_ADD(num_shortest_paths, double);
// used for middle sync only
GALOIS_SYNC_STRUCTURE_REDUCE_SET(num_shortest_paths, double);
GALOIS_SYNC_STRUCTURE_BITSET(num_shortest_paths);

////////////////////////////////////////////////////////////////////////////
// Current Lengths
////////////////////////////////////////////////////////////////////////////

GALOIS_SYNC_STRUCTURE_REDUCE_MIN(current_length, uint32_t);
GALOIS_SYNC_STRUCTURE_BITSET(current_length);

////////////////////////////////////////////////////////////////////////////
// Dependency
////////////////////////////////////////////////////////////////////////////

GALOIS_SYNC_STRUCTURE_REDUCE_ADD(dependency, float);
GALOIS_SYNC_STRUCTURE_BITSET(dependency);

struct ForwardReduce {
  using ValTy1 = uint32_t;
  using ValTy2 = double;

  static void operate(uint32_t lid, struct NodeData& node, ValTy1 val1, ValTy2 val2, galois::DynamicBitSet& bitset_val1, galois::DynamicBitSet& bitset_val2, galois::DGAccumulator<uint32_t>& dga) {
      uint32_t old = galois::atomicMin(node.current_length, val1);

      if (old > val1) {
          bitset_val1.set(lid);
          galois::atomicAddVoid(node.num_shortest_paths, val2);
          bitset_val2.set(lid);
          dga += 1;
      }
      else if (old == val1) {
          galois::atomicAddVoid(node.num_shortest_paths, val2);
          bitset_val2.set(lid);
          dga += 1;
      }
  }
};
