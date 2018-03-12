/** Graph helper functions -*- C++ -*-
 * @file GraphHelpers.h
 * @section License
 *
 * This file is part of Galois.  Galois is a framework to exploit
 * amorphous data-parallelism in irregular programs.
 *
 * Galois is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, version 2.1 of the
 * License.
 *
 * Galois is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Galois.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * @section Copyright
 *
 * Copyright (C) 2017, The University of Texas at Austin. All rights
 * reserved.
 *
 * @section Description
 *
 * Contains functions that can be done on various graphs with a particular
 * interface.
 *
 * @author Loc Hoang <l_hoang@utexas.edu>
 */
#include <galois/graphs/GraphHelpers.h>

namespace galois {
namespace graphs {
namespace internal {

uint32_t determine_block_division(uint32_t numDivisions,
                                  std::vector<unsigned>& scaleFactor) {
  uint32_t numBlocks = 0;

  if (scaleFactor.empty()) {
    // if scale factor isn't specified, everyone gets the same amount
    numBlocks = numDivisions;

    // scale factor holds a prefix sum of the scale factor
    for (uint32_t i = 0; i < numDivisions; i++) {
      scaleFactor.push_back(i + 1);
    }
  } else {
    assert(scaleFactor.size() == numDivisions);
    assert(numDivisions >= 1);

    // get numDivisions number of blocks we need + save a prefix sum of the scale
    // factor vector to scaleFactor
    for (uint32_t i = 0; i < numDivisions; i++) {
      numBlocks += scaleFactor[i];
      scaleFactor[i] = numBlocks;
    }
  }

  return numBlocks;
}

bool unitRangeCornerCaseHandle(uint32_t unitsToSplit, uint32_t beginNode,
                               uint32_t endNode, 
                               std::vector<uint32_t>& returnRanges) {
  uint32_t totalNodes = endNode - beginNode;                                

  // check corner cases
  // no nodes = assign nothing to all units
  if (beginNode == endNode) {
    returnRanges[0] = beginNode;

    for (uint32_t i = 0; i < unitsToSplit; i++) {
      returnRanges[i + 1] = beginNode;
    }

    return true;
  }

  // single unit case; 1 unit gets all
  if (unitsToSplit == 1) {
    returnRanges[0] = beginNode;
    returnRanges[1] = endNode;
    return true;
  // more units than nodes
  } else if (unitsToSplit > totalNodes) {
    uint32_t current_node = beginNode;
    returnRanges[0] = current_node;
    // 1 node for units until out of units
    for (uint32_t i = 0; i < totalNodes; i++) {
      returnRanges[i + 1] = ++current_node;
    }
    // deal with remainder units; they get nothing
    for (uint32_t i = totalNodes; i < unitsToSplit; i++) {
      returnRanges[i + 1] = totalNodes;
    }

    return true;
  }

  return false;
}

void unitRangeSanity(uint32_t unitsToSplit, uint32_t beginNode, 
                     uint32_t endNode, std::vector<uint32_t>& returnRanges) {
  #ifndef NDEBUG
  // sanity checks
  assert(returnRanges[0] == beginNode &&
         "return ranges begin not the begin node");
  assert(returnRanges[unitsToSplit] == endNode &&
         "return ranges end not end node");

  for (uint32_t i = 1; i < unitsToSplit; i++) {
    assert(returnRanges[i] >= beginNode && returnRanges[i] <= endNode);
    assert(returnRanges[i] >= returnRanges[i - 1]);
  }
  #endif
}

} // end internal namespace
} // end graphs namespace
} // end galois namespace
