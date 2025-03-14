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

/**
 * @file DReducible.h
 *
 * Implements distributed reducible objects for easy reduction of values
 * across a distributed system.
 */
#ifndef GALOIS_DISTTERMINATOR_H
#define GALOIS_DISTTERMINATOR_H

#include <limits>
#include "galois/Galois.h"
#include "galois/Reduction.h"
#include "galois/AtomicHelpers.h"
#include "galois/runtime/DistStats.h"

namespace galois {

/**
 * Distributed sum-reducer for getting the sum of some value across multiple
 * hosts.
 *
 * @tparam Ty type of value to max-reduce
 */
template <typename Ty>
class DGTerminator {
  galois::runtime::NetworkInterface& net = galois::runtime::getSystemNetworkInterface();

  galois::GAccumulator<Ty> mdata;
  Ty local_mdata;
  int local_active, global_active;

  MPI_Request reduce_request;

public:
  //! Default constructor
  DGTerminator() {
    reset();
    local_active = 0;
    global_active = 0;
    MPI_Iallreduce(&local_active, &global_active, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD, &reduce_request);
  }

  /**
   * Adds to accumulated value
   *
   * @param rhs Value to add
   * @returns reference to this object
   */
  DGTerminator& operator+=(const Ty& rhs) {
    mdata += rhs;
    return *this;
  }

  /**
   * Sets current value stored in accumulator.
   *
   * @param rhs Value to set
   */
  void operator=(const Ty rhs) {
    mdata.reset();
    mdata += rhs;
  }

  /**
   * Sets current value stored in accumulator.
   *
   * @param rhs Value to set
   */
  void set(const Ty rhs) {
    mdata.reset();
    mdata += rhs;
  }

  /**
   * Read local accumulated value.
   *
   * @returns locally accumulated value
   */
  Ty read_local() {
    if (local_mdata == 0)
      local_mdata = mdata.reduce();
    return local_mdata;
  }

  /**
   * Reset the entire accumulator.
   *
   * @returns the value of the last reduce call
   */
  void reset() {
    mdata.reset();
    local_mdata = 0;
  }

  bool terminate() {
      // make sure the previous reduce operation has finished
      int reduce_received = 0;
      while (reduce_received == 0) {
          MPI_Test(&reduce_request, &reduce_received, MPI_STATUS_IGNORE);
      }
      
      if (local_mdata != 0) {
          local_active = 1;
      }
      else {
          local_active = 0;
      }
      global_active = 0;
      
      MPI_Iallreduce(&local_active, &global_active, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD, &reduce_request);

      if (local_active != 0) {
          return false;
      }
      else {
          reduce_received = 0;
          while (reduce_received == 0) {
              MPI_Test(&reduce_request, &reduce_received, MPI_STATUS_IGNORE);
          }

          if (global_active != 0) {
              return false;
          }
          else {
              return true;
          }
      }
  }

  /**
   * Reduce data across all hosts, saves the value, and returns the
   * reduced value
   *
   * @param runID optional argument used to create a statistics timer
   * for later reporting
   *
   * @returns The reduced value
   */
  Ty reduce(std::string runID = std::string()) {
      std::string timer_str("ReduceDGAccum_" + runID);
      galois::CondStatTimer<GALOIS_COMM_STATS> reduceTimer(timer_str.c_str(), "DGReducible");
      reduceTimer.start();

      if (local_mdata == 0)
          local_mdata = mdata.reduce();

      bool halt = terminate();
      reduceTimer.stop();
      
      if (halt) {
          galois::runtime::evilPhase += 2; // one for reduce and one for broadcast
          if (galois::runtime::evilPhase >= static_cast<uint32_t>(std::numeric_limits<int16_t>::max())) { // limit defined by MPI or LCI
              galois::runtime::evilPhase = 1;
          }

          return false;
      }
      else {
          return true;
      }
  }
};

} // namespace galois
#endif
