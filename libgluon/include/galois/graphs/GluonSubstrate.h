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
 * @file GluonSubstrate.h
 *
 * Contains the implementation for GluonSubstrate.
 */

#ifndef _GALOIS_GLUONSUB_H_
#define _GALOIS_GLUONSUB_H_

#include <fstream>
#include <atomic>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <chrono>
#include <sstream>

#include "galois/runtime/GlobalObj.h"
#include "galois/runtime/DistStats.h"
#include "galois/runtime/SyncStructures.h"
#include "galois/runtime/DataCommMode.h"
#include "galois/DynamicBitset.h"

// TODO find a better way to do this without globals
//! Specifies what format to send metadata in
extern DataCommMode enforcedDataMode;

//! Enumeration for specifiying write location for sync calls
enum WriteLocation {
  //! write at source
  writeSource,
  //! write at destination
  writeDestination,
  //! write at source and/or destination
  writeAny
};
//! Enumeration for specifiying read location for sync calls
enum ReadLocation {
  //! read at source
  readSource,
  //! read at destination
  readDestination,
  //! read at source and/or destination
  readAny
};

namespace galois {
namespace graphs {

/**
 * Gluon communication substrate that handles communication given a user graph.
 * User graph should provide certain things the substrate expects.
 *
 * TODO documentation on expected things
 *
 * @tparam GraphTy User graph to handle communication for
 */
template <typename GraphTy, typename ValTy>
class GluonSubstrate : public galois::runtime::GlobalObject {
private:
  //! Synchronization type
  enum SyncType {
    syncReduce,   //!< Reduction sync
    syncBroadcast //!< Broadcast sync
  };

  //! Graph name used for printing things
  constexpr static const char* const RNAME = "Gluon";

  //! The graph to handle communication for
  GraphTy& userGraph;
  galois::runtime::NetworkInterface& net;
  const unsigned id; //!< Copy of net.ID, which is the ID of the machine.
  bool transposed;   //!< Marks if passed in graph is transposed or not.
  bool isVertexCut;  //!< Marks if passed in graph's partitioning is vertex cut.
  DataCommMode substrateDataMode; //!< datamode to enforce
  const uint32_t numHosts;     //!< Copy of net.Num, which is the total number of machines
  uint32_t num_run; //!< Keep track of number of runs.
  uint32_t num_round; //!< Keep track of number of rounds.
  unsigned numT;

  // memoization optimization
  //! Master nodes of mirrors on different hosts. For broadcast;
  std::vector<std::vector<size_t>> masterNodes;
  //! Mirror nodes on different hosts. For reduce; comes from the user graph
  //! during initialization (we expect user to give to us)
  std::vector<std::vector<size_t>>& mirrorNodes;
  //! Phantom nodes on different hosts. For reduce; comes from the user graph
  //! during initialization (we expect user to give to us)
  std::vector<std::vector<size_t>>& phantomNodes;
  
  std::vector<uint8_t*> sendCommBuffer;
  std::vector<size_t> sendCommBufferLen;

  size_t recvCommBufferOffset;

  size_t maxSharedSize;

  // Used for efficient comms
  DataCommMode data_mode;
  size_t syncBitsetLen;
  size_t syncOffsetsLen;

  /**
   * Reset a provided bitset given the type of synchronization performed
   *
   * @param syncType Type of synchronization to consider when doing reset
   * @param bitset_reset_range Function to reset range with
   */
  void reset_bitset(SyncType syncType, void (*bitset_reset_range)(size_t, size_t)) {
    size_t numMasters = userGraph.numMasters();
    if (numMasters > 0) {
      // note this assumes masters are from 0 -> a number; CuSP should
      // do this automatically
      if (syncType == syncBroadcast) { // reset masters
        bitset_reset_range(0, numMasters - 1);
      } else {
        assert(syncType == syncReduce);
        // mirrors occur after masters
        if (numMasters < userGraph.size()) {
          bitset_reset_range(numMasters, userGraph.size() - 1);
        }
      }
    } else { // all things are mirrors
      // only need to reset if reduce
      if (syncType == syncReduce) {
        if (userGraph.size() > 0) {
          bitset_reset_range(0, userGraph.size() - 1);
        }
      }
    }
  }

  //! Increments evilPhase, a phase counter used by communication.
  void inline incrementEvilPhase() {
    ++galois::runtime::evilPhase;
    // limit defined by MPI or LCI
    if (galois::runtime::evilPhase >= static_cast<uint32_t>(std::numeric_limits<int16_t>::max())) {
      galois::runtime::evilPhase = 1;
    }
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Proxy communication setup
  ////////////////////////////////////////////////////////////////////////////////
  /**
   * Let other hosts know about which host has what mirrors/masters;
   * used for later communication of mirrors/masters.
   */

  void exchangeProxyInfo() {
    // send off the mirror nodes
    for (unsigned x = 0; x < numHosts; ++x) {
      if (x == id)
        continue;

      galois::runtime::SendBuffer b;
      gSerialize(b, mirrorNodes[x]);
      net.sendTagged(x, galois::runtime::evilPhase, b);
    }

    // receive the mirror nodes
    for (unsigned x = 0; x < numHosts; ++x) {
      if (x == id)
        continue;

      decltype(net.receiveTagged(galois::runtime::evilPhase)) p;
      do {
        p = net.receiveTagged(galois::runtime::evilPhase);
      } while (!p);

      galois::runtime::gDeserialize(p->second, masterNodes[p->first]);
    }
    
    incrementEvilPhase();
    
    // convert the global ids stored in the master/mirror nodes arrays to local
    // ids
    // TODO: use 32-bit distinct vectors for masters and mirrors from here on
    for (uint32_t h = 0; h < masterNodes.size(); ++h) {
      galois::do_all(
          galois::iterate(size_t{0}, masterNodes[h].size()),
          [&](size_t n) {
            masterNodes[h][n] = userGraph.getLID(masterNodes[h][n]);
          },
          galois::no_stats());
    }

    for (uint32_t h = 0; h < mirrorNodes.size(); ++h) {
      galois::do_all(
          galois::iterate(size_t{0}, mirrorNodes[h].size()),
          [&](size_t n) {
            mirrorNodes[h][n] = userGraph.getLID(mirrorNodes[h][n]);
          },
          galois::no_stats());
    }
    
#ifndef GALOIS_FULL_MIRRORING     
    // send off the phantom nodes
    for (unsigned x = 0; x < numHosts; ++x) {
      if (x == id)
        continue;

      galois::runtime::SendBuffer b;
      gSerialize(b, phantomNodes[x]);
      net.sendTagged(x, galois::runtime::evilPhase, b);

      if (phantomNodes[x].size() == 0) {
          net.excludeSendWorkTermination(x);
      }
    }

    // receive the phantom master nodes
    std::vector<std::vector<size_t>> phantomMasterNodes;
    phantomMasterNodes.resize(numHosts);
    for (unsigned x = 0; x < numHosts; ++x) {
      if (x == id)
        continue;

      decltype(net.receiveTagged(galois::runtime::evilPhase)) p;
      do {
        p = net.receiveTagged(galois::runtime::evilPhase);
      } while (!p);

      galois::runtime::gDeserialize(p->second, phantomMasterNodes[p->first]);
    }
    
    incrementEvilPhase();
    
    // convert the global ids stored in the phantom (master) nodes arrays to local ids
    for (uint32_t h = 0; h < numHosts; ++h) {
      galois::do_all(
          galois::iterate(size_t{0}, phantomMasterNodes[h].size()),
          [&](size_t n) {
            phantomMasterNodes[h][n] = userGraph.getLID(phantomMasterNodes[h][n]);
          },
          galois::no_stats());

      if (phantomMasterNodes[h].size() == 0) {
          net.excludeHostWorkTermination();
      }
    }
    
    for (uint32_t h = 0; h < phantomNodes.size(); ++h) {
      galois::do_all(
          galois::iterate(size_t{0}, phantomNodes[h].size()),
          [&](size_t n) {
            phantomNodes[h][n] = userGraph.getLID(phantomNodes[h][n]);
          },
          galois::no_stats());
    }

    // send off the phantom master nodes
    for (unsigned x = 0; x < numHosts; ++x) {
      if (x == id)
        continue;

      galois::runtime::SendBuffer b;
      gSerialize(b, phantomMasterNodes[x]);
      net.sendTagged(x, galois::runtime::evilPhase, b);
    }
    
    // receive the phantom remote nodes
    std::vector<std::vector<size_t>> phantomRemoteNodes;
    phantomRemoteNodes.resize(numHosts);
    for (unsigned x = 0; x < numHosts; ++x) {
      if (x == id)
        continue;

      decltype(net.receiveTagged(galois::runtime::evilPhase)) p;
      do {
        p = net.receiveTagged(galois::runtime::evilPhase);
      } while (!p);

      galois::runtime::gDeserialize(p->second, phantomRemoteNodes[p->first]);
    }
    
    incrementEvilPhase();

    userGraph.constructPhantomLocalToRemoteVector(phantomRemoteNodes);
#endif
  }

  /**
   * Send statistics about master/mirror nodes to each host, and
   * report the statistics.
   */
  void sendInfoToHost() {
    uint64_t host_master_nodes = userGraph.numMasters();
    uint64_t host_mirror_nodes = userGraph.numMirrors();
    uint64_t host_phantom_nodes = userGraph.numPhantoms();
    uint64_t host_edges = userGraph.sizeEdges();
  
#ifdef GALOIS_HOST_STATS
    constexpr bool HOST_STATS = true;
#else
    constexpr bool HOST_STATS = false;
#endif

    std::string master_nodes_str = "MasterNodes_Host_" + std::to_string(id);
    galois::runtime::reportStatCond_Single<HOST_STATS>(RNAME, master_nodes_str, host_master_nodes);
    std::string mirror_nodes_str = "MirrorNodes_Host_" + std::to_string(id);
    galois::runtime::reportStatCond_Single<HOST_STATS>(RNAME, mirror_nodes_str, host_mirror_nodes);
    std::string phantom_nodes_str = "PhantomNodes_Host_" + std::to_string(id);
    galois::runtime::reportStatCond_Single<HOST_STATS>(RNAME, phantom_nodes_str, host_phantom_nodes);
    std::string edges_str = "Edges_Host_" + std::to_string(id);
    galois::runtime::reportStatCond_Single<HOST_STATS>(RNAME, edges_str, host_edges);
        
    if (net.ID == 0) {
        uint64_t global_total_mirror_nodes = host_mirror_nodes;
        uint64_t global_total_phantom_nodes = host_phantom_nodes;

        // receive
        for (unsigned x = 0; x < numHosts; ++x) {
          if (x == id)
            continue;

          decltype(net.receiveTagged(galois::runtime::evilPhase)) p;
          do {
            p = net.receiveTagged(galois::runtime::evilPhase);
          } while (!p);

          uint64_t mirror_nodes_from_others;
          uint64_t phantom_nodes_from_others;
          galois::runtime::gDeserialize(p->second, mirror_nodes_from_others, phantom_nodes_from_others);
          global_total_mirror_nodes += mirror_nodes_from_others;
          global_total_phantom_nodes += phantom_nodes_from_others;
      }

      reportProxyStats(global_total_mirror_nodes, global_total_phantom_nodes);
    }
    else {
        // send info to host
        galois::runtime::SendBuffer b;
        gSerialize(b, host_mirror_nodes, host_phantom_nodes);
        net.sendTagged(0, galois::runtime::evilPhase, b);
    }

    incrementEvilPhase();
  }

  /**
   * Reports master/mirror stats.
   * Assumes that communication has already occured so that the host
   * calling it actually has the info required.
   *
   * @param global_total_mirror_nodes number of mirror nodes on all hosts
   * @param global_total_owned_nodes number of "owned" nodes on all hosts
   */
  void reportProxyStats(uint64_t global_total_mirror_nodes, uint64_t global_total_phantom_nodes) {
    float replication_factor = (float)global_total_mirror_nodes / (float)userGraph.globalSize();
    galois::runtime::reportStat_Single(RNAME, "ReplicationFactor", replication_factor);
  
#ifdef GALOIS_HOST_STATS
    constexpr bool HOST_STATS = true;
#else
    constexpr bool HOST_STATS = false;
#endif

    galois::runtime::reportStatCond_Single<HOST_STATS>(RNAME, "TotalMasterNodes", userGraph.globalSize());
    galois::runtime::reportStatCond_Single<HOST_STATS>(RNAME, "TotalMirrorNodes", global_total_mirror_nodes);
    galois::runtime::reportStatCond_Single<HOST_STATS>(RNAME, "TotalPhantomNodes", global_total_phantom_nodes);
    galois::runtime::reportStatCond_Single<HOST_STATS>(RNAME, "TotalEdges", userGraph.globalSizeEdges());
  }

  /**
   * Sets up the communication between the different hosts that contain
   * different parts of the graph by exchanging master/mirror information.
   */
  void setupCommunication() {
    // Exchange information for memoization optimization.
    exchangeProxyInfo();

#ifdef GALOIS_HOST_STATS
    constexpr bool HOST_STATS = true;
#else
    constexpr bool HOST_STATS = false;
#endif

    maxSharedSize = 0;
    // report masters/mirrors/phantoms to/from other hosts as statistics
    for (auto x = 0U; x < masterNodes.size(); ++x) {
      if (x == id)
        continue;
      std::string master_nodes_str =
          "MasterNodesFrom_" + std::to_string(id) + "_To_" + std::to_string(x);
      galois::runtime::reportStatCond_Tsum<HOST_STATS>(
          RNAME, master_nodes_str, masterNodes[x].size());
      if (masterNodes[x].size() > maxSharedSize) {
        maxSharedSize = masterNodes[x].size();
      }
    }

    for (auto x = 0U; x < mirrorNodes.size(); ++x) {
      if (x == id)
        continue;
      std::string mirror_nodes_str =
          "MirrorNodesFrom_" + std::to_string(x) + "_To_" + std::to_string(id);
      galois::runtime::reportStatCond_Tsum<HOST_STATS>(
          RNAME, mirror_nodes_str, mirrorNodes[x].size());
      if (mirrorNodes[x].size() > maxSharedSize) {
        maxSharedSize = mirrorNodes[x].size();
      }
    }

    for (auto x = 0U; x < phantomNodes.size(); ++x) {
      if (x == id)
        continue;
      std::string phantom_nodes_str =
          "PhantomNodesFrom_" + std::to_string(x) + "_To_" + std::to_string(id);
      galois::runtime::reportStatCond_Tsum<HOST_STATS>(
          RNAME, phantom_nodes_str, phantomNodes[x].size());
    }

    sendInfoToHost();
  }

public:
  /**
   * Delete default constructor: this class NEEDS to have a graph passed into
   * it.
   */
  GluonSubstrate() = delete;

  /**
   * Constructor for GluonSubstrate. Initializes metadata fields.
   *
   * @param _userGraph graph to build substrate on
   * @param host host number that this graph resides on
   * @param numHosts total number of hosts in the currently executing program
   * @param _transposed True if the graph is transposed
   * or not
   * @param _enforcedDataMode Forced data comm mode for sync
   */
  GluonSubstrate(
      GraphTy& _userGraph, unsigned host, unsigned numHosts, bool _transposed,
      DataCommMode _enforcedDataMode               = DataCommMode::noData)
      : galois::runtime::GlobalObject(this), userGraph(_userGraph), net(galois::runtime::getSystemNetworkInterface()), id(host),
        transposed(_transposed), isVertexCut(userGraph.is_vertex_cut()),
        substrateDataMode(_enforcedDataMode), numHosts(numHosts), num_run(0),
        num_round(0),
        mirrorNodes(userGraph.getMirrorNodes()),
        phantomNodes(userGraph.getPhantomNodes()),
        recvCommBufferOffset(0) {

    // set this global value for use on GPUs mostly
    enforcedDataMode = _enforcedDataMode;

    // master setup from mirrors done by setupCommunication call
    masterNodes.resize(numHosts);
    // setup proxy communication
    setupCommunication();

    numT = galois::getActiveThreads();

    // allocate communication buffer
    sendCommBuffer.resize(numHosts, nullptr);
    sendCommBufferLen.resize(numHosts, 0);

    // noData : data_mode
    // bitsetData : data_mode + syncBitset + dirty data
    // offsetsData : data_mode + syncOffsetsLen + syncOffsets + dirty data
    // onlyData : data_mode + dirty data
    size_t total_alloc_size =
        sizeof(DataCommMode) +
        sizeof(size_t) +
        (maxSharedSize * sizeof(uint32_t)) + // syncOffsets or syncBitset
        (maxSharedSize * sizeof(ValTy)); // dirty data

    // send buffer
    for (unsigned i=0; i<numHosts; i++) {
        if (i == id) {
            continue;
        }
          
        void* ptr = malloc(total_alloc_size);
        if (ptr == nullptr) {
            galois::gError("Failed to allocate memory for the communication phase send buffer\n");
        }
        sendCommBuffer[i] = static_cast<uint8_t*>(ptr);
    }

    // receive buffer
    net.allocateRecvCommBuffer(total_alloc_size);
  }

  ~GluonSubstrate() {
      for (unsigned i=0; i<numHosts; i++) {
          if (sendCommBuffer[i] != nullptr) {
              free(sendCommBuffer[i]);
          }
      }
  }

private:
  ////////////////////////////////////////////////////////////////////////////////
  // Extract data from nodes (for reduce and broadcast)
  ////////////////////////////////////////////////////////////////////////////////
  /**
   * Extracts data at provided lid.
   *
   * This version (reduce) resets the value after extract.
   *
   * @tparam FnTy structure that specifies how synchronization is to be done
   * @tparam syncType either reduce or broadcast; determines if reset is
   * necessary
   *
   * @param lid local id of node to get data from
   * @returns data (specified by FnTy) of node with local id lid
   */
  /* Reduction extract resets the value afterwards */
  template <typename FnTy, SyncType syncType>
  inline ValTy extractWrapper(size_t lid) {
    if (syncType == syncReduce) {
      auto val = FnTy::extract(lid, userGraph.getData(lid));
      FnTy::reset(lid, userGraph.getData(lid));
      return val;
    } else {
      return FnTy::extract(lid, userGraph.getData(lid));
    }
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Reduce/sets on node (for broadcast)
  ////////////////////////////////////////////////////////////////////////////////
  /**
   * Reduce variant. Takes a value and reduces it according to the sync
   * structure provided to the function.
   *
   * @tparam FnTy structure that specifies how synchronization is to be done
   * @tparam syncType Reduce sync or broadcast sync
   *
   * @param lid local id of node to reduce to
   * @param val value to reduce to
   * @param bit_set_compute bitset indicating which nodes have changed; updated
   * if reduction causes a change
   */
  template <typename FnTy, SyncType syncType>
  inline void setWrapper(size_t lid, ValTy val, galois::DynamicBitSet& bit_set_compute) {
    if (syncType == syncReduce) {
      if (FnTy::reduce(lid, userGraph.getData(lid), val)) {
          if (bit_set_compute.size() != 0)
              bit_set_compute.set(lid);
      }
    } else {
        FnTy::setVal(lid, userGraph.getData(lid), val);
    }
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Sends
  ////////////////////////////////////////////////////////////////////////////////
  /**
   * Extracts the data that will be sent to a host in this round of
   * synchronization based on the passed in bitset and saves it to a
   * send buffer.
   *
   * @tparam syncType either reduce or broadcast
   * @tparam syncFnTy struct that has info on how to do synchronization
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   * being used for the extraction
   *
   * @param from_id
   * @param indices Vector that contains node ids of nodes that we will
   * potentially send things to
   * @param b OUTPUT: buffer that will be sent over the network; contains data
   * based on set bits in bitset
   */
  template <SyncType syncType, typename SyncFnTy, typename BitsetFnTy>
  void syncExtract(unsigned from_id, std::vector<size_t>& indices) {
    uint32_t num = indices.size();

    const galois::DynamicBitSet& bitset_compute = BitsetFnTy::get();
    
    // total num of set bits
    auto activeThreads = galois::getActiveThreads();
    std::vector<unsigned int> t_prefix_bit_counts(activeThreads);

    // count how many bits are set on each thread
    galois::on_each([&](unsigned tid, unsigned nthreads) {
        unsigned int block_size = num / nthreads;
        if ((num % nthreads) > 0)
            ++block_size;
        assert((block_size * nthreads) >= num);

        unsigned int start = tid * block_size;
        unsigned int end   = (tid + 1) * block_size;
        if (end > num)
            end = num;

        unsigned int count = 0;
        for (unsigned int i = start; i < end; ++i) {
            size_t lid = indices[i];
            if (bitset_compute.test(lid)) {
                ++count;
            }
        }

        t_prefix_bit_counts[tid] = count;
    });

    // calculate prefix sum of bits per thread
    for (unsigned int i = 1; i < activeThreads; ++i) {
        t_prefix_bit_counts[i] += t_prefix_bit_counts[i - 1];
    }

    syncBitsetLen = num;
    syncOffsetsLen = t_prefix_bit_counts[activeThreads - 1];
        
    data_mode = get_data_mode<ValTy>(syncOffsetsLen, indices.size());
    
    uint8_t* bufPtr = sendCommBuffer[from_id];
    size_t& bufOffset = sendCommBufferLen[from_id];

    // noData : data_mode
    // bitsetData : data_mode + syncBitset + dirty data
    // offsetsData : data_mode + syncOffsetsLen + syncOffsets + dirty data
    // onlyData : data_mode + dirty data
    *((DataCommMode*)(bufPtr + bufOffset)) = data_mode;
    bufOffset += sizeof(DataCommMode);

    if (data_mode == bitsetData) {
        galois::on_each([&](unsigned tid, unsigned nthreads) {
            unsigned int block_size = syncBitsetLen / nthreads;
            if ((syncBitsetLen % nthreads) > 0)
                ++block_size;
            assert((block_size * nthreads) >= syncBitsetLen);

            unsigned int start = tid * block_size;
            unsigned int end   = (tid + 1) * block_size;
            if (end > syncBitsetLen)
                end = syncBitsetLen;
                
            size_t threadIndexOffset = bufOffset + start * sizeof(uint8_t);
            unsigned int t_prefix_bit_count;
            if (tid == 0) {
                t_prefix_bit_count = 0;
            } else {
                t_prefix_bit_count = t_prefix_bit_counts[tid - 1];
            }
            size_t threadValOffset = bufOffset + syncBitsetLen * sizeof(uint8_t) + t_prefix_bit_count * sizeof(ValTy);

            for (unsigned int i = start; i < end; ++i) {
                size_t lid = indices[i];
                if (bitset_compute.test(lid)) {
                    *(bufPtr + threadIndexOffset) = (uint8_t)1;
                    ValTy val = extractWrapper<SyncFnTy, syncType>(lid);
                    *((ValTy*)(bufPtr + threadValOffset)) = val;
                    threadValOffset += sizeof(ValTy);
                } else {
                    *(bufPtr + threadIndexOffset) = (uint8_t)0;
                }
                threadIndexOffset += sizeof(uint8_t);
            }
        });

        bufOffset += (syncBitsetLen * sizeof(uint8_t) + syncOffsetsLen * sizeof(ValTy));
    } else if (data_mode == offsetsData) {
        *((size_t*)(bufPtr + bufOffset)) = syncOffsetsLen;
        bufOffset += sizeof(size_t);

        // calculate the indices of the set bits and save them to the offset vector
        if (syncOffsetsLen > 0) {
            galois::on_each([&](unsigned tid, unsigned nthreads) {
                unsigned int block_size = syncBitsetLen / nthreads;
                if ((syncBitsetLen % nthreads) > 0)
                    ++block_size;
                assert((block_size * nthreads) >= syncBitsetLen);

                unsigned int start = tid * block_size;
                unsigned int end   = (tid + 1) * block_size;
                if (end > syncBitsetLen)
                    end = syncBitsetLen;

                unsigned int t_prefix_bit_count;
                if (tid == 0) {
                    t_prefix_bit_count = 0;
                } else {
                    t_prefix_bit_count = t_prefix_bit_counts[tid - 1];
                }
                size_t threadOffset = bufOffset + t_prefix_bit_count * (sizeof(uint32_t) + sizeof(ValTy));

                for (unsigned int i = start; i < end; ++i) {
                    size_t lid = indices[i];
                    if (bitset_compute.test(lid)) {
                        *((uint32_t*)(bufPtr + threadOffset)) = (uint32_t)i;
                        threadOffset += sizeof(uint32_t);
                        ValTy val = extractWrapper<SyncFnTy, syncType>(lid);
                        *((ValTy*)(bufPtr + threadOffset)) = val;
                        threadOffset += sizeof(ValTy);
                    }
                }
            });
            
            bufOffset += (syncOffsetsLen * (sizeof(uint32_t) + sizeof(ValTy)));
      }
    } else if (data_mode == onlyData) {
        galois::on_each([&](unsigned tid, unsigned nthreads) {
            unsigned int block_size = syncBitsetLen / nthreads;
            if ((syncBitsetLen % nthreads) > 0)
                ++block_size;
            assert((block_size * nthreads) >= syncBitsetLen);

            unsigned int start = tid * block_size;
            unsigned int end   = (tid + 1) * block_size;
            if (end > syncBitsetLen)
                end = syncBitsetLen;

            size_t threadOffset = bufOffset + start * sizeof(ValTy);
            for (unsigned int i = start; i < end; ++i) {
                size_t lid = indices[i];
                ValTy val = extractWrapper<SyncFnTy, syncType>(lid);
                *((ValTy*)(bufPtr + threadOffset)) = val;
                threadOffset += sizeof(ValTy);
            }
        });
            
        bufOffset += syncBitsetLen * sizeof(ValTy);
    }
  }

  /**
   * Sends data to all hosts (if there is anything that needs to be sent
   * to that particular host) and adjusts bitset according to sync type.
   *
   * @tparam syncType either reduce or broadcast
   * @tparam SyncFnTy synchronization structure with info needed to synchronize
   * @tparam BitsetFnTy struct that has information needed to access bitset
   */
  template <SyncType syncType, typename SyncFnTy, typename BitsetFnTy>
  void syncSend() {
    std::string syncTypeStr = (syncType == syncReduce) ? "Reduce" : "Broadcast";
    std::string statSendBytes_str(syncTypeStr + "SendBytes_" + get_run_identifier());
    
    auto& sharedNodes = (syncType == syncReduce) ? mirrorNodes : masterNodes;
    
    for (unsigned h = 1; h < numHosts; ++h) {
      unsigned x = (id + h) % numHosts;

      if (sharedNodes[x].size() == 0)
        continue;

      syncExtract<syncType, SyncFnTy, BitsetFnTy>(x, sharedNodes[x]);
      galois::runtime::reportStatCond_Tsum<MORE_DIST_STATS>(RNAME, statSendBytes_str, sendCommBufferLen[x]);

      net.sendComm(x, sendCommBuffer[x], sendCommBufferLen[x]);
      sendCommBufferLen[x] = 0;
    }

    reset_bitset(syncType, &BitsetFnTy::reset_range);
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Receives
  ////////////////////////////////////////////////////////////////////////////////

  /**
   * Deserializes messages from other hosts and applies them to update local
   * data based on the provided sync structures.
   *
   * @tparam syncType either reduce or broadcast
   * @tparam SyncFnTy synchronization structure with info needed to synchronize
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   *
   * @param from_id ID of host which the message we are processing was received
   * from
   * @param buf Buffer that contains received message from other host
   */
  template <SyncType syncType, typename SyncFnTy, typename BitsetFnTy>
  void syncRecvApply(std::vector<size_t>& indices, uint8_t* bufPtr) {
    uint32_t num = indices.size();

    size_t& bufOffset = recvCommBufferOffset;
    // 1st deserialize gets data mode
    data_mode = *((DataCommMode*)(bufPtr + bufOffset));
    bufOffset += sizeof(DataCommMode);

    galois::DynamicBitSet& bitset_compute = BitsetFnTy::get();

    syncBitsetLen = num;
  
    // noData : data_mode
    // bitsetData : data_mode + syncBitset + dirty data
    // offsetsData : data_mode + syncOffsetsLen + syncOffsets + dirty data
    // onlyData : data_mode + dirty data
    if (data_mode == bitsetData) {
        auto activeThreads = galois::getActiveThreads();
        std::vector<unsigned int> t_prefix_bit_counts(activeThreads);

        // count how many bits are set on each thread
        galois::on_each([&](unsigned tid, unsigned nthreads) {
            unsigned int block_size = syncBitsetLen / nthreads;
            if ((syncBitsetLen % nthreads) > 0)
                ++block_size;
            assert((block_size * nthreads) >= syncBitsetLen);

            unsigned int start = tid * block_size;
            unsigned int end   = (tid + 1) * block_size;
            if (end > syncBitsetLen)
                end = syncBitsetLen;

            unsigned int count = 0;
            size_t threadOffset = bufOffset + start * sizeof(uint8_t);
            for (unsigned int i = start; i < end; ++i) {
                uint8_t bit = *((uint8_t*)(bufPtr + threadOffset));
                threadOffset += sizeof(uint8_t);
                if (bit == (uint8_t)1)
                    ++count;
            }

            t_prefix_bit_counts[tid] = count;
        });

        // calculate prefix sum of bits per thread
        for (unsigned int i = 1; i < activeThreads; ++i) {
            t_prefix_bit_counts[i] += t_prefix_bit_counts[i - 1];
        }
        // total num of set bits
        syncOffsetsLen = t_prefix_bit_counts[activeThreads - 1];

        // calculate the indices of the set bits and save them to the offset vector
        if (syncOffsetsLen > 0) {
            // count how many bits are set on each thread
            galois::on_each([&](unsigned tid, unsigned nthreads) {
                unsigned int block_size = syncBitsetLen / nthreads;
                if ((syncBitsetLen % nthreads) > 0)
                    ++block_size;
                assert((block_size * nthreads) >= syncBitsetLen);

                unsigned int start = tid * block_size;
                unsigned int end   = (tid + 1) * block_size;
                if (end > syncBitsetLen)
                    end = syncBitsetLen;

                size_t threadIndexOffset = bufOffset + start * sizeof(uint8_t);
                unsigned int t_prefix_bit_count;
                if (tid == 0) {
                    t_prefix_bit_count = 0;
                } else {
                    t_prefix_bit_count = t_prefix_bit_counts[tid - 1];
                }
                size_t threadValOffset = bufOffset + syncBitsetLen * sizeof(uint8_t) + t_prefix_bit_count * sizeof(ValTy);
              
                for (unsigned int i = start; i < end; ++i) {
                    uint8_t bit = *((uint8_t*)(bufPtr + threadIndexOffset));
                    threadIndexOffset += sizeof(uint8_t);
                    if (bit == (uint8_t)1) {
                        size_t lid = indices[i];
                        ValTy val = *((ValTy*)(bufPtr + threadValOffset));
                        threadValOffset += sizeof(ValTy);
                        setWrapper<SyncFnTy, syncType>(lid, val, bitset_compute);
                    }
                }
            });
        }
            
        bufOffset += (syncBitsetLen * sizeof(uint8_t) + syncOffsetsLen * sizeof(ValTy));
    } else if (data_mode == offsetsData) {
        syncOffsetsLen = *((size_t*)(bufPtr + bufOffset));
        bufOffset += sizeof(size_t);
        galois::on_each([&](unsigned tid, unsigned nthreads) {
            unsigned int block_size = syncOffsetsLen / nthreads;
            if ((syncOffsetsLen % nthreads) > 0)
                ++block_size;
            assert((block_size * nthreads) >= syncOffsetsLen);

            unsigned int start = tid * block_size;
            unsigned int end   = (tid + 1) * block_size;
            if (end > syncOffsetsLen)
                end = syncOffsetsLen;

            size_t threadOffset = bufOffset + start * (sizeof(uint32_t) + sizeof(ValTy));
            for (unsigned int i = start; i < end; ++i) {
                uint32_t indexOffset = *((uint32_t*)(bufPtr + threadOffset));
                threadOffset += sizeof(uint32_t);
                size_t lid = indices[indexOffset];
                ValTy val = *((ValTy*)(bufPtr + threadOffset));
                threadOffset += sizeof(ValTy);
                setWrapper<SyncFnTy, syncType>(lid, val, bitset_compute);
            }
        });
            
        bufOffset += (syncOffsetsLen * (sizeof(uint32_t) + sizeof(ValTy)));
    } else if (data_mode == onlyData) {
        galois::on_each([&](unsigned tid, unsigned nthreads) {
            unsigned int block_size = syncBitsetLen / nthreads;
            if ((syncBitsetLen % nthreads) > 0)
                ++block_size;
            assert((block_size * nthreads) >= syncBitsetLen);

            unsigned int start = tid * block_size;
            unsigned int end   = (tid + 1) * block_size;
            if (end > syncBitsetLen)
                end = syncBitsetLen;

            size_t threadOffset = bufOffset + start * sizeof(ValTy);
            for (unsigned int i = start; i < end; ++i) {
                size_t lid = indices[i];
                ValTy val = *((ValTy*)(bufPtr + threadOffset));
                threadOffset += sizeof(ValTy);
                setWrapper<SyncFnTy, syncType>(lid, val, bitset_compute);
            }
        });
            
        bufOffset += syncBitsetLen * sizeof(ValTy);
    }
  }

  /**
   * Determines if there is anything to receive from a host and receives/applies
   * the messages.
   *
   * @tparam syncType either reduce or broadcast
   * @tparam SyncFnTy synchronization structure with info needed to synchronize
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   */
  template <SyncType syncType, typename SyncFnTy, typename BitsetFnTy>
  void syncRecv() {
      auto& sharedNodes = (syncType == syncReduce) ? masterNodes : mirrorNodes;
      
      uint32_t host;
      uint8_t* work;
      for (unsigned x = 0; x < numHosts; ++x) {
          if (x == id)
              continue;
          if (sharedNodes[x].size() == 0)
              continue;

          net.receiveComm(host, work);

          recvCommBufferOffset = 0;
          syncRecvApply<syncType, SyncFnTy, BitsetFnTy>(sharedNodes[host], work);
      }
      incrementEvilPhase();
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Higher Level Sync Calls (broadcast/reduce, etc)
  ////////////////////////////////////////////////////////////////////////////////

  /**
   * Does a reduction of data from mirror nodes to master nodes.
   *
   * @tparam ReduceFnTy reduce sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   */
  template <typename ReduceFnTy, typename BitsetFnTy>
  inline void reduce() {
    syncSend<syncReduce, ReduceFnTy, BitsetFnTy>();

#ifndef GALOIS_FULL_MIRRORING
    poll_for_remote_work<ReduceFnTy>();
#endif

    syncRecv<syncReduce, ReduceFnTy, BitsetFnTy>();
  }

  /**
   * Does a broadcast of data from master to mirror nodes.
   *
   * @tparam BroadcastFnTy broadcast sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   */
  template <typename BroadcastFnTy, typename BitsetFnTy>
  inline void broadcast() {
      syncSend<syncBroadcast, BroadcastFnTy, BitsetFnTy>();
      syncRecv<syncBroadcast, BroadcastFnTy, BitsetFnTy>();
  }

  /**
   * Do sync necessary for write source, read source.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   */
  template <typename SyncFnTy, typename BitsetFnTy>
  inline void sync_src_to_src() {
    // do nothing for OEC
    // reduce and broadcast for IEC, CVC, UVC
    if (transposed || isVertexCut) {
      reduce<SyncFnTy, BitsetFnTy>();
      broadcast<SyncFnTy, BitsetFnTy>();
    }
  }

  /**
   * Do sync necessary for write source, read destination.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   */
  template <typename SyncFnTy, typename BitsetFnTy>
  inline void sync_src_to_dst() {
    // only broadcast for OEC
    // only reduce for IEC
    // reduce and broadcast for CVC, UVC
    if (transposed) {
      reduce<SyncFnTy, BitsetFnTy>();
      if (isVertexCut) {
        broadcast<SyncFnTy, BitsetFnTy>();
      }
    } else {
      if (isVertexCut) {
        reduce<SyncFnTy, BitsetFnTy>();
      }
      broadcast<SyncFnTy, BitsetFnTy>();
    }
  }

  /**
   * Do sync necessary for write source, read any.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   */
  template <typename SyncFnTy, typename BitsetFnTy>
  inline void sync_src_to_any() {
    // only broadcast for OEC
    // reduce and broadcast for IEC, CVC, UVC
    if (transposed || isVertexCut) {
      reduce<SyncFnTy, BitsetFnTy>();
    }
    broadcast<SyncFnTy, BitsetFnTy>();
  }

  /**
   * Do sync necessary for write dest, read source.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   */
  template <typename SyncFnTy, typename BitsetFnTy>
  inline void sync_dst_to_src() {
    // only reduce for OEC
    // only broadcast for IEC
    // reduce and broadcast for CVC, UVC
    if (transposed) {
      if (isVertexCut) {
        reduce<SyncFnTy, BitsetFnTy>();
      }
      broadcast<SyncFnTy, BitsetFnTy>();
    } else {
      reduce<SyncFnTy, BitsetFnTy>();
      if (isVertexCut) {
        broadcast<SyncFnTy, BitsetFnTy>();
      }
    }
  }

  /**
   * Do sync necessary for write dest, read dest.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   */
  template <typename SyncFnTy, typename BitsetFnTy>
  inline void sync_dst_to_dst() {
    // do nothing for IEC
    // reduce and broadcast for OEC, CVC, UVC
    if (!transposed || isVertexCut) {
      reduce<SyncFnTy, BitsetFnTy>();
      broadcast<SyncFnTy, BitsetFnTy>();
    }
  }

  /**
   * Do sync necessary for write dest, read any.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   */
  template <typename SyncFnTy, typename BitsetFnTy>
  inline void sync_dst_to_any() {
    // only broadcast for IEC
    // reduce and broadcast for OEC, CVC, UVC
    if (!transposed || isVertexCut) {
      reduce<SyncFnTy, BitsetFnTy>();
    }
    broadcast<SyncFnTy, BitsetFnTy>();
  }

  /**
   * Do sync necessary for write any, read src.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   */
  template <typename SyncFnTy, typename BitsetFnTy>
  inline void sync_any_to_src() {
    // only reduce for OEC
    // reduce and broadcast for IEC, CVC, UVC
    reduce<SyncFnTy, BitsetFnTy>();
    if (transposed || isVertexCut) {
      broadcast<SyncFnTy, BitsetFnTy>();
    }
  }

  /**
   * Do sync necessary for write any, read dst.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   */
  template <typename SyncFnTy, typename BitsetFnTy>
  inline void sync_any_to_dst() {
    // only reduce for IEC
    // reduce and broadcast for OEC, CVC, UVC
    reduce<SyncFnTy, BitsetFnTy>();

    if (!transposed || isVertexCut) {
      broadcast<SyncFnTy, BitsetFnTy>();
    }
  }

  /**
   * Do sync necessary for write any, read any.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   */
  template <typename SyncFnTy, typename BitsetFnTy>
  inline void sync_any_to_any() {
    // reduce and broadcast for OEC, IEC, CVC, UVC
    reduce<SyncFnTy, BitsetFnTy>();
    broadcast<SyncFnTy, BitsetFnTy>();
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Public iterface: sync
  ////////////////////////////////////////////////////////////////////////////////

public:
  /**
   * Main sync call exposed to the user that calls the correct sync function
   * based on provided template arguments. Must provide information through
   * structures on how to do synchronization/which fields to synchronize.
   *
   * @tparam writeLocation Location data is written (src or dst)
   * @tparam readLocation Location data is read (src or dst)
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   */
  template <WriteLocation writeLocation, ReadLocation readLocation,
            typename SyncFnTy, typename BitsetFnTy = galois::InvalidBitsetFnTy>
  inline void sync() {
      if (writeLocation == writeSource) {
        if (readLocation == readSource) {
          sync_src_to_src<SyncFnTy, BitsetFnTy>();
        } else if (readLocation == readDestination) {
          sync_src_to_dst<SyncFnTy, BitsetFnTy>();
        } else { // readAny
          sync_src_to_any<SyncFnTy, BitsetFnTy>();
        }
      } else if (writeLocation == writeDestination) {
        if (readLocation == readSource) {
          sync_dst_to_src<SyncFnTy, BitsetFnTy>();
        } else if (readLocation == readDestination) {
          sync_dst_to_dst<SyncFnTy, BitsetFnTy>();
        } else { // readAny
          sync_dst_to_any<SyncFnTy, BitsetFnTy>();
        }
      } else { // writeAny
        if (readLocation == readSource) {
          sync_any_to_src<SyncFnTy, BitsetFnTy>();
        } else if (readLocation == readDestination) {
          sync_any_to_dst<SyncFnTy, BitsetFnTy>();
        } else { // readAny
          sync_any_to_any<SyncFnTy, BitsetFnTy>();
        }
      }
  }

public:
  ////////////////////////////////////////////////////////////////////////////////
  // Metadata settings/getters
  ////////////////////////////////////////////////////////////////////////////////
  /**
   * Set the run number.
   *
   * @param runNum Number to set the run to
   */
  inline void set_num_run(const uint32_t runNum) { num_run = runNum; }

  /**
   * Get the set run number.
   *
   * @returns The set run number saved in the graph
   */
  inline uint32_t get_run_num() const { return num_run; }

  /**
   * Set the round number for use in the run identifier.
   *
   * @param round round number to set to
   */
  inline void set_num_round(const uint32_t round) { num_round = round; }

  /**
   * Get a run identifier using the set run and set round.
   *
   * @returns a string run identifier
   * @deprecated We want to move away from calling this by itself; use ones
   * that take an argument; will be removed once we eliminate all instances
   * of its use from code
   */
  inline std::string get_run_identifier() const {
#if GALOIS_PER_ROUND_STATS
    return std::string(std::to_string(num_run) + "_" +
                       std::to_string(num_round));
#else
    return std::string(std::to_string(num_run));
#endif
  }

  /**
   * Get a run identifier using the set run and set round and
   * append to the passed in string.
   *
   * @param loop_name String to append the run identifier
   * @returns String with run identifier appended to passed in loop name
   */
  inline std::string get_run_identifier(std::string loop_name) const {
#if GALOIS_PER_ROUND_STATS
    return std::string(std::string(loop_name) + "_" + std::to_string(num_run) +
                       "_" + std::to_string(num_round));
#else
    return std::string(std::string(loop_name) + "_" + std::to_string(num_run));
#endif
  }

  /**
   * Given a sync structure, reset the field specified by the structure
   * to the 0 of the reduction on mirrors.
   *
   * @tparam FnTy structure that specifies how synchronization is to be done
   */
  template <typename FnTy>
  void reset_mirrorField() {
    // TODO make sure this is correct still
    auto mirrorRanges = userGraph.getMirrorRanges();
    for (auto r : mirrorRanges) {
      if (r.first == r.second)
        continue;
      assert(r.first < r.second);
      
      // CPU always enters this block
      galois::do_all(
          galois::iterate(r.first, r.second),
          [&](uint32_t lid) { FnTy::reset(lid, userGraph.getData(lid)); },
          galois::no_stats());
    }
  }

    template<typename FnTy>
    void poll_for_remote_work() {
        std::atomic<bool> terminateFlag = false;

        galois::on_each(
            [&](unsigned, unsigned) {
                bool success;
                bool fullFlag;
                uint8_t* buf;
                size_t bufLen;

                uint32_t msgCount;
                
                uint32_t lid;
                ValTy val;

                while(!terminateFlag) {
                    success = net.receiveRemoteWork(terminateFlag, fullFlag, buf, bufLen);

                    if (success) { // received message
                        if (fullFlag) {
                            msgCount = net.workCount;
                        }
                        else {
                            msgCount = *((uint32_t*)(buf + bufLen - sizeof(uint32_t)));
                        }
                        
                        for (uint32_t i=0; i<msgCount; i++) {
                            lid = *((uint32_t*)buf + (i << 1));
                            val = *((ValTy*)buf + (i << 1) + 1);
                            FnTy::reduce_atomic_void(userGraph.getData(lid), val);
                        }
                        
                        net.deallocateRecvBuffer(buf);
                    }
                }
            }
        );
    }

};

template <typename GraphTy, typename ValTy>
constexpr const char* const galois::graphs::GluonSubstrate<GraphTy, ValTy>::RNAME;
} // end namespace graphs
} // end namespace galois

#endif // header guard
