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
  std::pair<unsigned, unsigned> cartesianGrid; //!< cartesian grid (if any)
  bool partitionAgnostic; //!< true if communication should ignore partitioning
  DataCommMode substrateDataMode; //!< datamode to enforce
  const uint32_t
      numHosts;     //!< Copy of net.Num, which is the total number of machines
  uint32_t num_run; //!< Keep track of number of runs.
  uint32_t num_round; //!< Keep track of number of rounds.
  bool isCartCut;     //!< True if graph is a cartesian cut
  unsigned numT;

  // bitvector status hasn't been maintained
  //! Typedef used so galois::runtime::BITVECTOR_STATUS doesn't have to be
  //! written
  using BITVECTOR_STATUS = galois::runtime::BITVECTOR_STATUS;
  //! A pointer set during syncOnDemand calls that points to the status
  //! of a bitvector with regard to where data has been synchronized
  //! @todo pass the flag as function paramater instead
  BITVECTOR_STATUS* currentBVFlag;

  // memoization optimization
  //! Master nodes of mirrors on different hosts. For broadcast;
  std::vector<std::vector<size_t>> masterNodes;
  //! Mirror nodes on different hosts. For reduce; comes from the user graph
  //! during initialization (we expect user to give to us)
  std::vector<std::vector<size_t>>& mirrorNodes;
  //! Phantom nodes on different hosts. For reduce; comes from the user graph
  //! during initialization (we expect user to give to us)
  std::vector<std::vector<size_t>>& phantomNodes;

  uint64_t phantomMasterCount;
  
  std::vector<uint8_t*> sendCommBuffer;
  std::vector<size_t> sendCommBufferLen;

  size_t recvCommBufferOffset;

  size_t maxSharedSize;

  uint32_t dataSizeRatio;

  // double buffering storage to enforce synchronization for partial or no mirroring
  std::vector<std::unique_ptr<ValTy>> phantomMasterUpdateBuffer;

  // Used for efficient comms
  DataCommMode data_mode;
  size_t bit_set_count;
  uint8_t* syncBitset;
  size_t syncBitsetLen;
  uint32_t* syncOffsets;
  size_t syncOffsetsLen;

  /**
   * Reset a provided bitset given the type of synchronization performed
   *
   * @param syncType Type of synchronization to consider when doing reset
   * @param bitset_reset_range Function to reset range with
   */
  void reset_bitset(SyncType syncType,
                    void (*bitset_reset_range)(size_t, size_t)) {
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
    if (galois::runtime::evilPhase >=
        static_cast<uint32_t>(std::numeric_limits<int16_t>::max())) {
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

    // force all messages to be processed before continuing
    net.flushData();

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
#if GALOIS_COMM_STATS
          galois::loopname(get_run_identifier("MasterNodes").c_str()),
#endif
          galois::no_stats());
    }

    for (uint32_t h = 0; h < mirrorNodes.size(); ++h) {
      galois::do_all(
          galois::iterate(size_t{0}, mirrorNodes[h].size()),
          [&](size_t n) {
            mirrorNodes[h][n] = userGraph.getLID(mirrorNodes[h][n]);
          },
#if GALOIS_COMM_STATS
          galois::loopname(get_run_identifier("MirrorNodes").c_str()),
#endif
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
    }

    // force all messages to be processed before continuing
    net.flushData();

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
#if GALOIS_COMM_STATS
          galois::loopname(get_run_identifier("PhantomMasterNodes").c_str()),
#endif
          galois::no_stats());
    }
    
    // count the number of phantom masters and allocat memory for the update buffers
    phantomMasterCount = 0;
    phantomMasterUpdateBuffer.resize(userGraph.numMasters());
    for (uint32_t h = 0; h < numHosts; ++h) {
        for (size_t i=0; i<phantomMasterNodes[h].size(); i++) {
            if (!phantomMasterUpdateBuffer[phantomMasterNodes[h][i]]) {
                phantomMasterUpdateBuffer[phantomMasterNodes[h][i]] = std::make_unique<ValTy>();
                phantomMasterCount++;
            }
        }
    }
    
    for (uint32_t h = 0; h < phantomNodes.size(); ++h) {
      galois::do_all(
          galois::iterate(size_t{0}, phantomNodes[h].size()),
          [&](size_t n) {
            phantomNodes[h][n] = userGraph.getLID(phantomNodes[h][n]);
          },
#if GALOIS_COMM_STATS
          galois::loopname(get_run_identifier("PhantomNodes").c_str()),
#endif
          galois::no_stats());
    }

#ifdef GALOIS_EXCHANGE_PHANTOM_LID    
    // send off the phantom master nodes
    for (unsigned x = 0; x < numHosts; ++x) {
      if (x == id)
        continue;

      galois::runtime::SendBuffer b;
      gSerialize(b, phantomMasterNodes[x]);
      net.sendTagged(x, galois::runtime::evilPhase, b);
    }

    // force all messages to be processed before continuing
    net.flushData();
    
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
  
    std::string master_nodes_str = "MasterNodes_Host_" + std::to_string(id);
    galois::runtime::reportStatCond_Single<GALOIS_HOST_STATS>(RNAME, master_nodes_str, host_master_nodes);
    std::string mirror_nodes_str = "MirrorNodes_Host_" + std::to_string(id);
    galois::runtime::reportStatCond_Single<GALOIS_HOST_STATS>(RNAME, mirror_nodes_str, host_mirror_nodes);
    std::string phantom_nodes_str = "PhantomNodes_Host_" + std::to_string(id);
    galois::runtime::reportStatCond_Single<GALOIS_HOST_STATS>(RNAME, phantom_nodes_str, host_phantom_nodes);
    std::string phantom_master_nodes_str = "PhantomMasterNodes_Host_" + std::to_string(id);
    galois::runtime::reportStatCond_Single<GALOIS_HOST_STATS>(RNAME, phantom_master_nodes_str, phantomMasterCount);
        
    if (net.ID == 0) {
        uint64_t global_total_mirror_nodes = host_mirror_nodes;
        uint64_t global_total_phantom_nodes = host_phantom_nodes;
        uint64_t global_total_phantom_master_nodes = phantomMasterCount;

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
          uint64_t phantom_master_nodes_from_others;
          galois::runtime::gDeserialize(p->second, mirror_nodes_from_others, phantom_nodes_from_others, phantom_master_nodes_from_others);
          global_total_mirror_nodes += mirror_nodes_from_others;
          global_total_phantom_nodes += phantom_nodes_from_others;
          global_total_phantom_master_nodes += phantom_master_nodes_from_others;
      }

      reportProxyStats(global_total_mirror_nodes, global_total_phantom_nodes, global_total_phantom_master_nodes);
    }
    else {
        // send info to host
        galois::runtime::SendBuffer b;
        gSerialize(b, host_mirror_nodes, host_phantom_nodes, phantomMasterCount);
        net.sendTagged(0, galois::runtime::evilPhase, b);
        
        // force all messages to be processed before continuing
        net.flushData();
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
  void reportProxyStats(uint64_t global_total_mirror_nodes, uint64_t global_total_phantom_nodes, uint64_t global_total_phantom_master_nodes) {
    float replication_factor = (float)(global_total_mirror_nodes + global_total_phantom_master_nodes) / (float)userGraph.globalSize();
    galois::runtime::reportStat_Single(RNAME, "ReplicationFactor", replication_factor);
    float memory_overhead = (float)(dataSizeRatio * (userGraph.globalSize() + global_total_mirror_nodes + global_total_phantom_master_nodes) + userGraph.globalSizeEdges()) / (float)(dataSizeRatio * userGraph.globalSize() + userGraph.globalSizeEdges());
    galois::runtime::reportStat_Single(RNAME, "AggregatedMemoryOverhead", memory_overhead);

    galois::runtime::reportStatCond_Single<GALOIS_HOST_STATS>(RNAME, "TotalMasterNodes", userGraph.globalSize());
    galois::runtime::reportStatCond_Single<GALOIS_HOST_STATS>(RNAME, "TotalMirrorNodes", global_total_mirror_nodes);
    galois::runtime::reportStatCond_Single<GALOIS_HOST_STATS>(RNAME, "TotalPhantomNodes", global_total_phantom_nodes);
    galois::runtime::reportStatCond_Single<GALOIS_HOST_STATS>(RNAME, "TotalEdges", userGraph.globalSizeEdges());
  }

  /**
   * Sets up the communication between the different hosts that contain
   * different parts of the graph by exchanging master/mirror information.
   */
  void setupCommunication() {
    galois::CondStatTimer<MORE_DIST_STATS> Tcomm_setup("CommunicationSetupTime",
                                                       RNAME);

    // barrier so that all hosts start the timer together
    galois::runtime::getHostBarrier().wait();

    Tcomm_setup.start();

    // Exchange information for memoization optimization.
    exchangeProxyInfo();

    Tcomm_setup.stop();

    maxSharedSize = 0;
    // report masters/mirrors/phantoms to/from other hosts as statistics
    for (auto x = 0U; x < masterNodes.size(); ++x) {
      if (x == id)
        continue;
      if (masterNodes[x].size() > maxSharedSize) {
        maxSharedSize = masterNodes[x].size();
      }
    }

    for (auto x = 0U; x < mirrorNodes.size(); ++x) {
      if (x == id)
        continue;
      if (mirrorNodes[x].size() > maxSharedSize) {
        maxSharedSize = mirrorNodes[x].size();
      }
    }

    sendInfoToHost();

    // do not track memory usage of partitioning
    net.resetMemUsage();
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
   * @param _cartesianGrid cartesian grid for sync
   * @param _partitionAgnostic determines if sync should be partition agnostic
   * or not
   * @param _enforcedDataMode Forced data comm mode for sync
   */
  GluonSubstrate(
      GraphTy& _userGraph, unsigned host, unsigned numHosts, bool _transposed,
      uint32_t dataSizeRatio = 1,
      std::pair<unsigned, unsigned> _cartesianGrid = std::make_pair(0u, 0u),
      bool _partitionAgnostic                      = false,
      DataCommMode _enforcedDataMode               = DataCommMode::noData)
      : galois::runtime::GlobalObject(this), userGraph(_userGraph), net(galois::runtime::getSystemNetworkInterface()), id(host),
        transposed(_transposed), isVertexCut(userGraph.is_vertex_cut()),
        cartesianGrid(_cartesianGrid), partitionAgnostic(_partitionAgnostic),
        substrateDataMode(_enforcedDataMode), numHosts(numHosts), num_run(0),
        num_round(0), currentBVFlag(nullptr),
        mirrorNodes(userGraph.getMirrorNodes()),
        phantomNodes(userGraph.getPhantomNodes()),
        recvCommBufferOffset(0),
        dataSizeRatio(dataSizeRatio) {
    if (cartesianGrid.first != 0 && cartesianGrid.second != 0) {
      GALOIS_ASSERT(cartesianGrid.first * cartesianGrid.second == numHosts,
                    "Cartesian split doesn't equal number of hosts");
      if (id == 0) {
        galois::gInfo("Gluon optimizing communication for 2-D cartesian cut: ",
                      cartesianGrid.first, " x ", cartesianGrid.second);
      }
      isCartCut = true;
    } else {
      assert(cartesianGrid.first == 0 && cartesianGrid.second == 0);
      isCartCut = false;
    }

    // set this global value for use on GPUs mostly
    enforcedDataMode = _enforcedDataMode;

    // master setup from mirrors done by setupCommunication call
    masterNodes.resize(numHosts);
    // setup proxy communication
    galois::CondStatTimer<MORE_DIST_STATS> Tgraph_construct_comm(
        "GraphCommSetupTime", RNAME);
    Tgraph_construct_comm.start();
    setupCommunication();
    Tgraph_construct_comm.stop();

    numT = galois::getActiveThreads();

    // allocate send work buffer
    sendWorkBuffer.resize(numT, nullptr);
    for (unsigned t=0; t<numT; t++) {
#ifdef GALOIS_EXCHANGE_PHANTOM_LID
        void* ptr = malloc(sizeof(uint32_t) + sizeof(ValTy));
#else
        void* ptr = malloc(sizeof(uint64_t) + sizeof(ValTy));
#endif
        if (ptr == nullptr) {
            galois::gError("Failed to allocate memory for the thread send work buffer\n");
        }
        sendWorkBuffer[t] = static_cast<uint8_t*>(ptr);
    }

    // allocate communication buffer
    sendCommBuffer.resize(numHosts, nullptr);
    sendCommBufferLen.resize(numHosts, 0);
    syncBitset = (uint8_t*)malloc(maxSharedSize * sizeof(uint8_t));
    if (syncBitset == nullptr) {
        galois::gError("Failed to allocate memory for the communication bitset\n");
    }

    syncOffsets = (uint32_t*)malloc(maxSharedSize * sizeof(uint32_t));
    if (syncOffsets == nullptr) {
        galois::gError("Failed to allocate memory for the communication offset array\n");
    }

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
      for (unsigned t=0; t<numT; t++) {
          if (sendWorkBuffer[t] != nullptr) {
              free(sendWorkBuffer[t]);
          }
      }
      
      for (unsigned i=0; i<numHosts; i++) {
          if (sendCommBuffer[i] != nullptr) {
              free(sendCommBuffer[i]);
          }
      }
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Data extraction from bitsets
  ////////////////////////////////////////////////////////////////////////////////

private:
  /**
   * Given a bitset, determine the indices of the bitset that are currently
   * set.
   *
   * @tparam syncType either reduce or broadcast; only used to name the timer
   *
   * @param loopName string used to name the timer for this function
   * @param bitset_comm the bitset to get the offsets of
   * @param offsets output: the offset vector that will contain indices into
   * the bitset that are set
   * @param bit_set_count output: will be set to the number of bits set in the
   * bitset
   */
  template <SyncType syncType>
  void getOffsetsFromBitset(const std::string& loopName) {
    // timer creation
    std::string syncTypeStr = (syncType == syncReduce) ? "Reduce" : "Broadcast";
    std::string offsets_timer_str(syncTypeStr + "Offsets_" + get_run_identifier(loopName));
    galois::CondStatTimer<GALOIS_COMM_STATS> Toffsets(offsets_timer_str.c_str(), RNAME);

    Toffsets.start();

    auto activeThreads = galois::getActiveThreads();
    std::vector<unsigned int> t_prefix_bit_counts(activeThreads);

    // count how many bits are set on each thread
    galois::on_each([&](unsigned tid, unsigned nthreads) {
      // TODO use block_range instead
      unsigned int block_size = syncBitsetLen / nthreads;
      if ((syncBitsetLen % nthreads) > 0)
        ++block_size;
      assert((block_size * nthreads) >= syncBitsetLen);

      unsigned int start = tid * block_size;
      unsigned int end   = (tid + 1) * block_size;
      if (end > syncBitsetLen)
        end = syncBitsetLen;

      unsigned int count = 0;
      for (unsigned int i = start; i < end; ++i) {
        if (syncBitset[i] == 1)
          ++count;
      }

      t_prefix_bit_counts[tid] = count;
    });

    // calculate prefix sum of bits per thread
    for (unsigned int i = 1; i < activeThreads; ++i) {
      t_prefix_bit_counts[i] += t_prefix_bit_counts[i - 1];
    }
    // total num of set bits
    bit_set_count = t_prefix_bit_counts[activeThreads - 1];

    syncOffsetsLen = bit_set_count;
    
    // calculate the indices of the set bits and save them to the offset vector
    if (bit_set_count > 0) {
      galois::on_each([&](unsigned tid, unsigned nthreads) {
        // TODO use block_range instead
        // TODO this is same calculation as above; maybe refactor it
        // into function?
        unsigned int block_size = syncBitsetLen / nthreads;
        if ((syncBitsetLen % nthreads) > 0)
          ++block_size;
        assert((block_size * nthreads) >= syncBitsetLen);

        unsigned int start = tid * block_size;
        unsigned int end   = (tid + 1) * block_size;
        if (end > syncBitsetLen)
          end = syncBitsetLen;

        unsigned int count = 0;
        unsigned int t_prefix_bit_count;
        if (tid == 0) {
          t_prefix_bit_count = 0;
        } else {
          t_prefix_bit_count = t_prefix_bit_counts[tid - 1];
        }

        for (unsigned int i = start; i < end; ++i) {
          if (syncBitset[i] == 1) {
            syncOffsets[t_prefix_bit_count + count] = i;
            ++count;
          }
        }
      });
    }
    Toffsets.stop();
  }

  /**
   * Determine what data needs to be synchronized based on the passed in
   * bitset_compute and returns information regarding these need-to-be-sync'd
   * nodes.
   *
   * @tparam FnTy structure that specifies how synchronization is to be done;
   * only used to get the size of the type being synchronized in this function
   * @tparam syncType type of synchronization this function is being called
   * for; only used to name a timer
   *
   * @param loopName loopname used to name the timer for the function
   * @param indices A vector that contains the local ids of the nodes that
   * you want to potentially synchronize
   * @param bitset_compute Contains the full bitset of all nodes in this
   * graph
   * @param bitset_comm OUTPUT: bitset that marks which indices in the passed
   * in indices array need to be synchronized
   * @param offsets OUTPUT: contains indices into bitset_comm that are set
   * @param bit_set_count OUTPUT: contains number of bits set in bitset_comm
   * @param data_mode OUTPUT: the way that this data should be communicated
   * based on how much data needs to be sent out
   */
  template <SyncType syncType>
  void getBitsetAndOffsets(const std::string& loopName,
                           const std::vector<size_t>& indices,
                           const galois::DynamicBitSet& bitset_compute) {
    if (substrateDataMode != onlyData) {
      std::string syncTypeStr = (syncType == syncReduce) ? "Reduce" : "Broadcast";
      std::string doall_str(syncTypeStr + "Bitset_" + loopName);

      //reset syncBitset
      std::fill(syncBitset, syncBitset + syncBitsetLen, static_cast<uint8_t>(0));

      // determine which local nodes in the indices array need to be
      // sychronized
      galois::do_all(
          galois::iterate(size_t{0}, indices.size()),
          [&](size_t n) {
            // assumes each lid is unique as test is not thread safe
            size_t lid = indices[n];
            if (bitset_compute.test(lid)) {
              syncBitset[n] = 1;
            }
          },
#if GALOIS_COMM_STATS
          galois::loopname(get_run_identifier(doall_str).c_str()),
#endif
          galois::no_stats());

      // get the number of set bits and the offsets into the comm bitset
      getOffsetsFromBitset<syncType>(loopName);
    }

    data_mode = get_data_mode<ValTy>(bit_set_count, indices.size());
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Message prep functions (buffering, send buffer getting, etc.)
  ////////////////////////////////////////////////////////////////////////////////
  /**
   * Get data that is going to be sent for synchronization and returns
   * it in a send buffer.
   *
   * @tparam syncType synchronization type
   * @tparam SyncFnTy synchronization structure with info needed to synchronize
   * @tparam BitsetFnTy struct that has information needed to access bitset
   *
   * @param loopName Name to give timer
   * @param x Host to send to
   * @param b OUTPUT: Buffer that will hold data to send
   */
  template <
      SyncType syncType, typename SyncFnTy, typename BitsetFnTy, bool async,
      typename std::enable_if<!BitsetFnTy::is_vector_bitset()>::type* = nullptr>
  void getSendBuffer(std::string loopName, unsigned x) {
    auto& sharedNodes = (syncType == syncReduce) ? mirrorNodes : masterNodes;

    syncExtract<syncType, SyncFnTy, BitsetFnTy, async>(loopName, x, sharedNodes[x]);

    std::string syncTypeStr = (syncType == syncReduce) ? "Reduce" : "Broadcast";
    std::string statSendBytes_str(syncTypeStr + "SendBytes_" + get_run_identifier(loopName));

    galois::runtime::reportStatCond_Tsum<MORE_DIST_STATS>(RNAME, statSendBytes_str, sendCommBufferLen[x]);
  }

  /**
   * Given data to serialize in val_vec, serialize it into the send buffer
   * depending on the mode of data communication selected for the data.
   *
   * @tparam syncType either reduce or broadcast
   * @tparam VecType type of val_vec, which stores the data to send
   *
   * @param loopName loop name used for timers
   * @param data_mode the way that the data should be communicated
   * @param bit_set_count the number of items we are sending in this message
   * @param indices list of all nodes that we are potentially interested in
   * sending things to
   * @param offsets contains indicies into "indices" that we are interested in
   * @param val_vec contains the data that we are serializing to send
   * @param b the buffer in which to serialize the message we are sending
   * to
   */
  template <bool async, SyncType syncType>
  void serializeMessage(std::string loopName, unsigned host) {
    std::string syncTypeStr = (syncType == syncReduce) ? "Reduce" : "Broadcast";
    std::string serialize_timer_str(syncTypeStr + "SerializeMessage_" + get_run_identifier(loopName));
    galois::CondStatTimer<GALOIS_COMM_STATS> Tserialize(serialize_timer_str.c_str(), RNAME);
    
    uint8_t* bufPtr = sendCommBuffer[host];
    size_t& bufOffset = sendCommBufferLen[host];

    // noData : data_mode
    // bitsetData : data_mode + syncBitset + dirty data
    // offsetsData : data_mode + syncOffsetsLen + syncOffsets + dirty data
    // onlyData : data_mode + dirty data
    if (data_mode == noData) {
      if (!async) {
        Tserialize.start();
        std::memcpy(bufPtr + bufOffset, &data_mode, sizeof(data_mode));
        bufOffset += sizeof(data_mode);
        Tserialize.stop();
      }
    } else if (data_mode == bitsetData) {
      Tserialize.start();
      std::memcpy(bufPtr + bufOffset, &data_mode, sizeof(data_mode));
      bufOffset += sizeof(data_mode);
      std::memcpy(bufPtr + bufOffset, syncBitset, syncBitsetLen * sizeof(uint8_t));
      bufOffset += syncBitsetLen * sizeof(uint8_t);
      Tserialize.stop();
    } else if (data_mode == offsetsData) {
      Tserialize.start();
      std::memcpy(bufPtr + bufOffset, &data_mode, sizeof(data_mode));
      bufOffset += sizeof(data_mode);
      std::memcpy(bufPtr + bufOffset, &syncOffsetsLen, sizeof(syncOffsetsLen));
      bufOffset += sizeof(syncOffsetsLen);
      std::memcpy(bufPtr + bufOffset, syncOffsets, syncOffsetsLen * sizeof(uint32_t));
      bufOffset += syncOffsetsLen * sizeof(uint32_t);
      Tserialize.stop();
    } else { // onlyData
      Tserialize.start();
      std::memcpy(bufPtr + bufOffset, &data_mode, sizeof(data_mode));
      bufOffset += sizeof(data_mode);
      Tserialize.stop();
    }
  }

  /**
   * Given the data mode, deserialize the rest of a message in a Receive Buffer.
   *
   * @tparam syncType either reduce or broadcast
   * @tparam VecType type of val_vec, which data will be deserialized into
   *
   * @param loopName used to name timers for statistics
   * @param data_mode data mode with which the original message was sent;
   * determines how to deserialize the rest of the message
   * @param buf buffer which contains the received message to deserialize
   *
   * The rest of the arguments are output arguments (they are passed by
   * reference)
   *
   * @param bit_set_count Var that holds number of bits set (i.e. number of
   * node changed) after deserialization
   * @param offsets holds offsets data after deserialization if data mode is
   * offsets + data
   * @param bit_set_comm holds the bitset representing changed nodes after
   * deserialization of data mode is bitset + data
   * @param buf_start
   * @param retval
   * @param val_vec The data proper will be deserialized into this vector
   */
  template <SyncType syncType>
  void deserializeMessage(std::string loopName, DataCommMode data_mode,
                          uint32_t num, uint8_t* bufPtr) {
    std::string syncTypeStr = (syncType == syncReduce) ? "Reduce" : "Broadcast";
    std::string serialize_timer_str(syncTypeStr + "DeserializeMessage_" + get_run_identifier(loopName));
    galois::CondStatTimer<GALOIS_COMM_STATS> Tdeserialize(serialize_timer_str.c_str(), RNAME);
    Tdeserialize.start();

    // noData : data_mode
    // bitsetData : data_mode + syncBitset + dirty data
    // offsetsData : data_mode + syncOffsetsLen + syncOffsets + dirty data
    // onlyData : data_mode + dirty data
    if (data_mode == bitsetData) {
        syncBitsetLen = num;
        std::memcpy(syncBitset, bufPtr + recvCommBufferOffset, syncBitsetLen * sizeof(uint8_t));
        recvCommBufferOffset += syncBitsetLen * sizeof(uint8_t);
    } else if (data_mode == offsetsData) {
        std::memcpy(&syncOffsetsLen, bufPtr + recvCommBufferOffset, sizeof(syncOffsetsLen));
        recvCommBufferOffset += sizeof(syncOffsetsLen);
        bit_set_count = syncOffsetsLen;
        std::memcpy(syncOffsets, bufPtr + recvCommBufferOffset, syncOffsetsLen * sizeof(uint32_t));
        recvCommBufferOffset += syncOffsetsLen * sizeof(uint32_t);
    }

    Tdeserialize.stop();
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Other helper functions
  ////////////////////////////////////////////////////////////////////////////////

  //! Returns the grid row ID of this host
  unsigned gridRowID() const { return (id / cartesianGrid.second); }
  //! Returns the grid row ID of the specified host
  unsigned gridRowID(unsigned hid) const {
    return (hid / cartesianGrid.second);
  }
  //! Returns the grid column ID of this host
  unsigned gridColumnID() const { return (id % cartesianGrid.second); }
  //! Returns the grid column ID of the specified host
  unsigned gridColumnID(unsigned hid) const {
    return (hid % cartesianGrid.second);
  }

  /**
   * Determine if a host is a communication partner using cartesian grid.
   */
  bool isNotCommPartnerCVC(unsigned host, SyncType syncType,
                           WriteLocation writeLocation,
                           ReadLocation readLocation) {
    assert(cartesianGrid.first != 0);
    assert(cartesianGrid.second != 0);

    if (transposed) {
      if (syncType == syncReduce) {
        switch (writeLocation) {
        case writeSource:
          return (gridColumnID() != gridColumnID(host));
        case writeDestination:
          return (gridRowID() != gridRowID(host));
        case writeAny:
          assert((gridRowID() == gridRowID(host)) ||
                 (gridColumnID() == gridColumnID(host)));
          return ((gridRowID() != gridRowID(host)) &&
                  (gridColumnID() != gridColumnID(host))); // false
        default:
          GALOIS_DIE("unreachable");
        }
      } else { // syncBroadcast
        switch (readLocation) {
        case readSource:
          return (gridColumnID() != gridColumnID(host));
        case readDestination:
          return (gridRowID() != gridRowID(host));
        case readAny:
          assert((gridRowID() == gridRowID(host)) ||
                 (gridColumnID() == gridColumnID(host)));
          return ((gridRowID() != gridRowID(host)) &&
                  (gridColumnID() != gridColumnID(host))); // false
        default:
          GALOIS_DIE("unreachable");
        }
      }
    } else {
      if (syncType == syncReduce) {
        switch (writeLocation) {
        case writeSource:
          return (gridRowID() != gridRowID(host));
        case writeDestination:
          return (gridColumnID() != gridColumnID(host));
        case writeAny:
          assert((gridRowID() == gridRowID(host)) ||
                 (gridColumnID() == gridColumnID(host)));
          return ((gridRowID() != gridRowID(host)) &&
                  (gridColumnID() != gridColumnID(host))); // false
        default:
          GALOIS_DIE("unreachable");
        }
      } else { // syncBroadcast, 1
        switch (readLocation) {
        case readSource:
          return (gridRowID() != gridRowID(host));
        case readDestination:
          return (gridColumnID() != gridColumnID(host));
        case readAny:
          assert((gridRowID() == gridRowID(host)) ||
                 (gridColumnID() == gridColumnID(host)));
          return ((gridRowID() != gridRowID(host)) &&
                  (gridColumnID() != gridColumnID(host))); // false
        default:
          GALOIS_DIE("unreachable");
        }
      }
      return false;
    }
  }

  // Requirement: For all X and Y,
  // On X, nothingToSend(Y) <=> On Y, nothingToRecv(X)
  /**
   * Determine if we have anything that we need to send to a particular host
   *
   * @param host Host number that we may or may not send to
   * @param syncType Synchronization type to determine which nodes on a
   * host need to be considered
   * @param writeLocation If data is being written to on source or
   * destination (or both)
   * @param readLocation If data is being read from on source or
   * destination (or both)
   * @returns true if there is nothing to send to a host, false otherwise
   */
  bool nothingToSend(unsigned host, SyncType syncType,
                     WriteLocation writeLocation, ReadLocation readLocation) {
    auto& sharedNodes = (syncType == syncReduce) ? mirrorNodes : masterNodes;
    // TODO refactor (below)
    if (!isCartCut) {
      return (sharedNodes[host].size() == 0);
    } else {
      // TODO If CVC, call is not comm partner else use default above
      if (sharedNodes[host].size() > 0) {
        return isNotCommPartnerCVC(host, syncType, writeLocation, readLocation);
      } else {
        return true;
      }
    }
  }

  /**
   * Determine if we have anything that we need to receive from a particular
   * host
   *
   * @param host Host number that we may or may not receive from
   * @param syncType Synchronization type to determine which nodes on a
   * host need to be considered
   * @param writeLocation If data is being written to on source or
   * destination (or both)
   * @param readLocation If data is being read from on source or
   * destination (or both)
   * @returns true if there is nothing to receive from a host, false otherwise
   */
  bool nothingToRecv(unsigned host, SyncType syncType,
                     WriteLocation writeLocation, ReadLocation readLocation) {
    auto& sharedNodes = (syncType == syncReduce) ? masterNodes : mirrorNodes;
    // TODO refactor (above)
    if (!isCartCut) {
      return (sharedNodes[host].size() == 0);
    } else {
      if (sharedNodes[host].size() > 0) {
        return isNotCommPartnerCVC(host, syncType, writeLocation, readLocation);
      } else {
        return true;
      }
    }
  }

  /**
   * Reports bytes saved by using the bitset to only selectively load data
   * to send.
   *
   * @tparam SyncFnTy synchronization structure with info needed to synchronize;
   * used for size calculation
   *
   * @param loopName loop name used for timers
   * @param syncTypeStr String used to name timers
   * @param totalToSend Total amount of nodes that are potentially sent (not
   * necessarily all nodees will be sent)
   * @param bitSetCount Number of nodes that will actually be sent
   * @param bitSetComm bitset used to send data
   */
  void reportRedundantSize(std::string loopName, std::string syncTypeStr,
                           uint32_t totalToSend) {
    size_t redundant_size = (totalToSend - bit_set_count) * sizeof(ValTy);
    size_t bit_set_size = (syncBitsetLen * sizeof(uint8_t));

    if (redundant_size > bit_set_size) {
      std::string statSavedBytes_str(syncTypeStr + "SavedBytes_" + get_run_identifier(loopName));

      galois::runtime::reportStatCond_Tsum<MORE_DIST_STATS>(RNAME, statSavedBytes_str, (redundant_size - bit_set_size));
    }
  }

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

  /**
   * Based on provided arguments, extracts the data that we are interested
   * in sending into val_vec.
   *
   * @tparam FnTy structure that specifies how synchronization is to be done
   * @tparam syncType either reduce or broadcast; used to determine if reseting
   * the extracted field is necessary
   * @tparam identity_offsets If this is true, then ignore the offsets
   * array and just grab directly from indices (i.e. don't pick out
   * particular elements, just grab contiguous chunk)
   * @tparam parallelize Determines if parallelizing the extraction is done or
   * not
   *
   * @param loopName name of loop used to name timer
   * @param indices Local ids of nodes that we are interested in
   * @param size Number of elements to extract
   * @param offsets Holds offsets into "indices" of the data that we are
   * interested in
   * @param val_vec OUTPUT: holds the extracted data
   * @param start Offset into val_vec to start saving data to
   */
  template <typename FnTy, SyncType syncType,
            bool identity_offsets = false, bool parallelize = true>
  void extractSubset(const std::string& loopName, unsigned host,
                     const std::vector<size_t>& indices, size_t size) {
    uint8_t* bufPtr = sendCommBuffer[host];
    size_t& bufOffset = sendCommBufferLen[host];
    
    if (parallelize) {
      std::string syncTypeStr = (syncType == syncReduce) ? "Reduce" : "Broadcast";
      std::string doall_str(syncTypeStr + "ExtractVal_" + loopName);

      galois::do_all(
          galois::iterate(size_t{0}, size),
          [&](unsigned int n) {
            unsigned int offset;
            if (identity_offsets)
              offset = n;
            else
              offset = syncOffsets[n];
            size_t lid         = indices[offset];
            ValTy val = extractWrapper<FnTy, syncType>(lid);
            size_t threadOffset = bufOffset + n * sizeof(val);
            std::memcpy(bufPtr + threadOffset, &val, sizeof(val));
          },
#if GALOIS_COMM_STATS
          galois::loopname(get_run_identifier(doall_str).c_str()),
#endif
          galois::no_stats());
      
      bufOffset += size * sizeof(ValTy);
    } else { // non-parallel version
      for (unsigned n = 0; n < size; ++n) {
        unsigned int offset;
        if (identity_offsets)
          offset = n;
        else
          offset = syncOffsets[n];

        size_t lid         = indices[offset];
        ValTy val = extractWrapper<FnTy, syncType>(lid);
        std::memcpy(bufPtr + bufOffset, &val, sizeof(val));
        bufOffset += sizeof(val);
      }
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
  template <typename FnTy, SyncType syncType, bool async>
  inline void setWrapper(size_t lid, ValTy val,
                         galois::DynamicBitSet& bit_set_compute) {
    if (syncType == syncReduce) {
      if (FnTy::reduce(lid, userGraph.getData(lid), val)) {
        if (bit_set_compute.size() != 0)
          bit_set_compute.set(lid);
      }
    } else {
      if (async)
        FnTy::reduce(lid, userGraph.getData(lid), val);
      else
        FnTy::setVal(lid, userGraph.getData(lid), val);
    }
  }

  /**
   * Given data received from another host and information on which nodes
   * to update, do the reduce/set of the received data to update local nodes.
   *
   * Complement function, in some sense, of extractSubset.
   *
   * @tparam VecTy type of indices variable
   * @tparam FnTy structure that specifies how synchronization is to be done
   * @tparam SyncType Reduce or broadcast
   * @tparam identity_offsets If this is true, then ignore the offsets
   * array and just grab directly from indices (i.e. don't pick out
   * particular elements, just grab contiguous chunk)
   * @tparam parallelize True if updates to nodes are to be parallelized
   *
   * @param loopName name of loop used to name timer
   * @param indices Local ids of nodes that we are interested in
   * @param size Number of elements to set
   * @param offsets Holds offsets into "indices" of the data that we are
   * interested in
   * @param val_vec holds data we will use to set
   * @param bit_set_compute bitset indicating which nodes have changed
   * @param start Offset into val_vec to start saving data to
   */
  template <typename IndicesVecTy, typename FnTy, SyncType syncType,
            bool async, bool identity_offsets = false,
            bool parallelize = true>
  void setSubset(const std::string& loopName, const IndicesVecTy& indices,
                 size_t size, uint8_t* bufPtr, galois::DynamicBitSet& bit_set_compute) {
    std::string syncTypeStr = (syncType == syncReduce) ? "Reduce" : "Broadcast";
    std::string doall_str(syncTypeStr + "SetVal_" + get_run_identifier(loopName));

    if (parallelize) {
      galois::do_all(
          galois::iterate(size_t{0}, size),
          [&](unsigned int n) {
            unsigned int offset;
            if (identity_offsets)
              offset = n;
            else
              offset = syncOffsets[n];
            auto lid = indices[offset];
            ValTy val;
            size_t threadOffset = recvCommBufferOffset + n * sizeof(val);
            std::memcpy(&val, bufPtr + threadOffset, sizeof(val));
            setWrapper<FnTy, syncType, async>(lid, val, bit_set_compute);
          },
#if GALOIS_COMM_STATS
          galois::loopname(get_run_identifier(doall_str).c_str()),
#endif
          galois::no_stats());

      recvCommBufferOffset += size * sizeof(ValTy);
    } else {
      for (unsigned int n = 0; n < size; ++n) {
        unsigned int offset;
        if (identity_offsets)
          offset = n;
        else
          offset = syncOffsets[n];
        auto lid = indices[offset];
        ValTy val;
        std::memcpy(&val, bufPtr + recvCommBufferOffset, sizeof(val));
        recvCommBufferOffset += sizeof(val);
        setWrapper<FnTy, syncType, async>(lid, val, bit_set_compute);
      }
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
   * @param loopName loop name used for timers
   * @param from_id
   * @param indices Vector that contains node ids of nodes that we will
   * potentially send things to
   * @param b OUTPUT: buffer that will be sent over the network; contains data
   * based on set bits in bitset
   */
  template <
      SyncType syncType, typename SyncFnTy, typename BitsetFnTy, bool async,
      typename std::enable_if<!BitsetFnTy::is_vector_bitset()>::type* = nullptr>
  void syncExtract(std::string loopName, unsigned from_id, std::vector<size_t>& indices) {
    uint32_t num = indices.size();

    std::string syncTypeStr = (syncType == syncReduce) ? "Reduce" : "Broadcast";
    std::string extract_timer_str(syncTypeStr + "Extract_" + get_run_identifier(loopName));
    galois::CondStatTimer<GALOIS_COMM_STATS> Textract(extract_timer_str.c_str(), RNAME);

    Textract.start();

    if (num > 0) {
        syncBitsetLen = num;
        
        const galois::DynamicBitSet& bit_set_compute = BitsetFnTy::get();
        bit_set_count = 0;

        getBitsetAndOffsets<syncType>(loopName, indices, bit_set_compute);

        serializeMessage<async, syncType>(loopName, from_id);
        if (data_mode == onlyData) {
            bit_set_count = indices.size();
            extractSubset<SyncFnTy, syncType, true, true>(loopName, from_id, indices, num);
        } else if (data_mode != noData) { // bitsetData or offsetsData
            extractSubset<SyncFnTy, syncType, false, true>(loopName, from_id, indices, bit_set_count);
        }
        reportRedundantSize(loopName, syncTypeStr, num);
    } else {
        data_mode = noData;
        if (!async) {
            std::memcpy(sendCommBuffer[from_id] + sendCommBufferLen[from_id], &data_mode, sizeof(data_mode));
            sendCommBufferLen[from_id] += sizeof(data_mode);
        }
    }

    Textract.stop();

    std::string metadata_str(syncTypeStr + "MetadataMode_" + std::to_string(data_mode) + "_" + get_run_identifier(loopName));
    galois::runtime::reportStatCond_Single<MORE_DIST_STATS>(RNAME, metadata_str, 1);
  }

  /**
   * Sends data to all hosts (if there is anything that needs to be sent
   * to that particular host) and adjusts bitset according to sync type.
   *
   * @tparam writeLocation Location data is written (src or dst)
   * @tparam readLocation Location data is read (src or dst)
   * @tparam syncType either reduce or broadcast
   * @tparam SyncFnTy synchronization structure with info needed to synchronize
   * @tparam BitsetFnTy struct that has information needed to access bitset
   *
   * @param loopName used to name timers created by this sync send
   */
  template <WriteLocation writeLocation, ReadLocation readLocation,
            SyncType syncType, typename SyncFnTy, typename BitsetFnTy,
            bool async>
  void syncNetSend(std::string loopName) {
    std::string syncTypeStr = (syncType == syncReduce) ? "Reduce" : "Broadcast";
    std::string statNumMessages_str(syncTypeStr + "NumMessages_" +
                                    get_run_identifier(loopName));

    size_t numMessages = 0;
    for (unsigned h = 1; h < numHosts; ++h) {
      unsigned x = (id + h) % numHosts;

      if (nothingToSend(x, syncType, writeLocation, readLocation))
        continue;

      getSendBuffer<syncType, SyncFnTy, BitsetFnTy, async>(loopName, x);

      if ((!async) || (sendCommBufferLen[x] > 0)) {
        net.sendComm(x, sendCommBuffer[x], sendCommBufferLen[x]);
        sendCommBufferLen[x] = 0;
        ++numMessages;
      }
    }
    if (!async) {
      // Will force all messages to be processed before continuing
      net.flushComm();
    }

    if (BitsetFnTy::is_valid()) {
      reset_bitset(syncType, &BitsetFnTy::reset_range);
    }

    galois::runtime::reportStatCond_Tsum<MORE_DIST_STATS>(RNAME, statNumMessages_str, numMessages);
  }

  /**
   * Sends data over the network to other hosts based on the provided template
   * arguments.
   *
   * @tparam writeLocation Location data is written (src or dst)
   * @tparam readLocation Location data is read (src or dst)
   * @tparam syncType either reduce or broadcast
   * @tparam SyncFnTy synchronization structure with info needed to synchronize
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   *
   * @param loopName used to name timers for statistics
   */
  template <WriteLocation writeLocation, ReadLocation readLocation,
            SyncType syncType, typename SyncFnTy, typename BitsetFnTy,
            bool async>
  void syncSend(std::string loopName) {
    std::string syncTypeStr = (syncType == syncReduce) ? "Reduce" : "Broadcast";
    galois::CondStatTimer<GALOIS_COMM_STATS> TSendTime(
        (syncTypeStr + "Send_" + get_run_identifier(loopName)).c_str(), RNAME);

    TSendTime.start();
    syncNetSend<writeLocation, readLocation, syncType, SyncFnTy, BitsetFnTy, async>(loopName);
    TSendTime.stop();
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Receives
  ////////////////////////////////////////////////////////////////////////////////

  /**
   * Deserializes messages from other hosts and applies them to update local
   * data based on the provided sync structures.
   *
   * Complement of syncExtract.
   *
   * @tparam syncType either reduce or broadcast
   * @tparam SyncFnTy synchronization structure with info needed to synchronize
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   *
   * @param from_id ID of host which the message we are processing was received
   * from
   * @param buf Buffer that contains received message from other host
   * @param loopName used to name timers for statistics
   */
  template <
      SyncType syncType, typename SyncFnTy, typename BitsetFnTy, bool async,
      typename std::enable_if<!BitsetFnTy::is_vector_bitset()>::type* = nullptr>
  void syncRecvApply(uint32_t from_id, uint8_t* bufPtr, std::string loopName) {
    std::string syncTypeStr = (syncType == syncReduce) ? "Reduce" : "Broadcast";
    std::string set_timer_str(syncTypeStr + "Set_" + get_run_identifier(loopName));
    galois::CondStatTimer<GALOIS_COMM_STATS> Tset(set_timer_str.c_str(), RNAME);

    auto& sharedNodes = (syncType == syncReduce) ? masterNodes : mirrorNodes;
    uint32_t num      = sharedNodes[from_id].size();

    Tset.start();

    if (num > 0) { // only enter if we expect message from that host
      // 1st deserialize gets data mode
      std::memcpy(&data_mode, bufPtr + recvCommBufferOffset, sizeof(data_mode));
      recvCommBufferOffset += sizeof(data_mode);

      if (data_mode != noData) {
          // deserialize the rest of the data in the buffer depending on the
          // data mode; arguments passed in here are mostly output vars
          deserializeMessage<syncType>(loopName, data_mode, num, bufPtr);

          galois::DynamicBitSet& bit_set_compute = BitsetFnTy::get();

          if (data_mode == bitsetData) {
            getOffsetsFromBitset<syncType>(loopName);
          }

          if (data_mode == onlyData) {
            setSubset<decltype(sharedNodes[from_id]), SyncFnTy, syncType,
                      async, true, true>(loopName, sharedNodes[from_id], num, bufPtr, bit_set_compute);
          } else { // bitsetData or offsetsData
            setSubset<decltype(sharedNodes[from_id]), SyncFnTy, syncType,
                      async, false, true>(loopName, sharedNodes[from_id], bit_set_count, bufPtr, bit_set_compute);
          }
          // TODO: reduce could update the bitset, so it needs to be copied
          // back to the device
      }
    }

    Tset.stop();
  }

  /**
   * Determines if there is anything to receive from a host and receives/applies
   * the messages.
   *
   * @tparam writeLocation Location data is written (src or dst)
   * @tparam readLocation Location data is read (src or dst)
   * @tparam syncType either reduce or broadcast
   * @tparam SyncFnTy synchronization structure with info needed to synchronize
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   *
   * @param loopName used to name timers for statistics
   */
  template <WriteLocation writeLocation, ReadLocation readLocation,
            SyncType syncType, typename SyncFnTy, typename BitsetFnTy,
            bool async>
  void syncNetRecv(std::string loopName) {
    std::string wait_timer_str("Wait_" + get_run_identifier(loopName));
    galois::CondStatTimer<GALOIS_COMM_STATS> Twait(wait_timer_str.c_str(), RNAME);

    if (async) {
      bool success = false;
      uint32_t host = ~0U;
      uint8_t* work = nullptr;
      do {
        success = net.receiveComm(host, work);

        if (success) {
          recvCommBufferOffset = 0;
          syncRecvApply<syncType, SyncFnTy, BitsetFnTy, async>(host, work, loopName);
        }
      } while (success);
    } else {
      bool success;
      uint32_t host;
      uint8_t* work;
      for (unsigned x = 0; x < numHosts; ++x) {
        if (x == id)
          continue;
        if (nothingToRecv(x, syncType, writeLocation, readLocation))
          continue;

        Twait.start();
        success = false;
        host = ~0U;
        work = nullptr;
        do {
#ifndef GALOIS_FULL_MIRRORING     
          check_remote_work<SyncFnTy>();
#endif
          success = net.receiveComm(host, work);
        } while (!success);
        Twait.stop();

        recvCommBufferOffset = 0;
        syncRecvApply<syncType, SyncFnTy, BitsetFnTy, async>(host, work, loopName);
      }
      incrementEvilPhase();
    }
  }

  /**
   * Receives messages from all other hosts and "applies" the message (reduce
   * or set) based on the sync structure provided.
   *
   * @tparam writeLocation Location data is written (src or dst)
   * @tparam readLocation Location data is read (src or dst)
   * @tparam syncType either reduce or broadcast
   * @tparam SyncFnTy synchronization structure with info needed to synchronize
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   *
   * @param loopName used to name timers for statistics
   */
  template <WriteLocation writeLocation, ReadLocation readLocation,
            SyncType syncType, typename SyncFnTy, typename BitsetFnTy,
            bool async>
  void syncRecv(std::string loopName) {
    std::string syncTypeStr = (syncType == syncReduce) ? "Reduce" : "Broadcast";
    galois::CondStatTimer<GALOIS_COMM_STATS> TRecvTime((syncTypeStr + "Recv_" + get_run_identifier(loopName)).c_str(), RNAME);

    TRecvTime.start();
    syncNetRecv<writeLocation, readLocation, syncType, SyncFnTy, BitsetFnTy, async>(loopName);
    TRecvTime.stop();
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Higher Level Sync Calls (broadcast/reduce, etc)
  ////////////////////////////////////////////////////////////////////////////////

  /**
   * Does a reduction of data from mirror nodes to master nodes.
   *
   * @tparam writeLocation Location data is written (src or dst)
   * @tparam readLocation Location data is read (src or dst)
   * @tparam ReduceFnTy reduce sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   *
   * @param loopName used to name timers for statistics
   */
  template <WriteLocation writeLocation, ReadLocation readLocation,
            typename ReduceFnTy, typename BitsetFnTy, bool async>
  inline void reduce(std::string loopName) {
    std::string timer_str("Reduce_" + get_run_identifier(loopName));
    galois::CondStatTimer<GALOIS_COMM_STATS> TsyncReduce(timer_str.c_str(), RNAME);

    TsyncReduce.start();

    syncSend<writeLocation, readLocation, syncReduce, ReduceFnTy, BitsetFnTy, async>(loopName);
    syncRecv<writeLocation, readLocation, syncReduce, ReduceFnTy, BitsetFnTy, async>(loopName);

    TsyncReduce.stop();

#ifndef GALOIS_FULL_MIRRORING     
    poll_for_remote_work<ReduceFnTy>();
#endif
  }

  /**
   * Does a broadcast of data from master to mirror nodes.
   *
   * @tparam writeLocation Location data is written (src or dst)
   * @tparam readLocation Location data is read (src or dst)
   * @tparam BroadcastFnTy broadcast sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   *
   * @param loopName used to name timers for statistics
   */
  template <WriteLocation writeLocation, ReadLocation readLocation,
            typename BroadcastFnTy, typename BitsetFnTy, bool async>
  inline void broadcast(std::string loopName) {
    std::string timer_str("Broadcast_" + get_run_identifier(loopName));
    galois::CondStatTimer<GALOIS_COMM_STATS> TsyncBroadcast(timer_str.c_str(),
                                                            RNAME);

    TsyncBroadcast.start();

    bool use_bitset = true;

    if (currentBVFlag != nullptr) {
      if (readLocation == readSource &&
          galois::runtime::src_invalid(*currentBVFlag)) {
        use_bitset     = false;
        *currentBVFlag = BITVECTOR_STATUS::NONE_INVALID;
        currentBVFlag  = nullptr;
      } else if (readLocation == readDestination &&
                 galois::runtime::dst_invalid(*currentBVFlag)) {
        use_bitset     = false;
        *currentBVFlag = BITVECTOR_STATUS::NONE_INVALID;
        currentBVFlag  = nullptr;
      } else if (readLocation == readAny &&
                 *currentBVFlag != BITVECTOR_STATUS::NONE_INVALID) {
        // the bitvector flag being non-null means this call came from
        // sync on demand; sync on demand will NEVER use readAny
        // if location is read Any + one of src or dst is invalid
        GALOIS_DIE("readAny + use of bitvector flag without none_invalid "
                   "should never happen");
      }
    }

      if (use_bitset) {
        syncSend<writeLocation, readLocation, syncBroadcast, BroadcastFnTy,
                 BitsetFnTy, async>(loopName);
      } else {
        syncSend<writeLocation, readLocation, syncBroadcast, BroadcastFnTy,
                 galois::InvalidBitsetFnTy, async>(loopName);
      }
      syncRecv<writeLocation, readLocation, syncBroadcast, BroadcastFnTy,
               BitsetFnTy, async>(loopName);

    TsyncBroadcast.stop();
  }

  /**
   * Do sync necessary for write source, read source.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   *
   * @param loopName used to name timers for statistics
   */
  template <typename SyncFnTy, typename BitsetFnTy, bool async>
  inline void sync_src_to_src(std::string loopName) {
    // do nothing for OEC
    // reduce and broadcast for IEC, CVC, UVC
    if (transposed || isVertexCut) {
      reduce<writeSource, readSource, SyncFnTy, BitsetFnTy, async>(loopName);
      broadcast<writeSource, readSource, SyncFnTy, BitsetFnTy, async>(loopName);
    }
  }

  /**
   * Do sync necessary for write source, read destination.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   *
   * @param loopName used to name timers for statistics
   */
  template <typename SyncFnTy, typename BitsetFnTy, bool async>
  inline void sync_src_to_dst(std::string loopName) {
    // only broadcast for OEC
    // only reduce for IEC
    // reduce and broadcast for CVC, UVC
    if (transposed) {
      reduce<writeSource, readDestination, SyncFnTy, BitsetFnTy, async>(
          loopName);
      if (isVertexCut) {
        broadcast<writeSource, readDestination, SyncFnTy, BitsetFnTy, async>(
            loopName);
      }
    } else {
      if (isVertexCut) {
        reduce<writeSource, readDestination, SyncFnTy, BitsetFnTy, async>(
            loopName);
      }
      broadcast<writeSource, readDestination, SyncFnTy, BitsetFnTy, async>(
          loopName);
    }
  }

  /**
   * Do sync necessary for write source, read any.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   *
   * @param loopName used to name timers for statistics
   */
  template <typename SyncFnTy, typename BitsetFnTy, bool async>
  inline void sync_src_to_any(std::string loopName) {
    // only broadcast for OEC
    // reduce and broadcast for IEC, CVC, UVC
    if (transposed || isVertexCut) {
      reduce<writeSource, readAny, SyncFnTy, BitsetFnTy, async>(loopName);
    }
    broadcast<writeSource, readAny, SyncFnTy, BitsetFnTy, async>(loopName);
  }

  /**
   * Do sync necessary for write dest, read source.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   *
   * @param loopName used to name timers for statistics
   */
  template <typename SyncFnTy, typename BitsetFnTy, bool async>
  inline void sync_dst_to_src(std::string loopName) {
    // only reduce for OEC
    // only broadcast for IEC
    // reduce and broadcast for CVC, UVC
    if (transposed) {
      if (isVertexCut) {
        reduce<writeDestination, readSource, SyncFnTy, BitsetFnTy, async>(
            loopName);
      }
      broadcast<writeDestination, readSource, SyncFnTy, BitsetFnTy, async>(
          loopName);
    } else {
      reduce<writeDestination, readSource, SyncFnTy, BitsetFnTy, async>(
          loopName);
      if (isVertexCut) {
        broadcast<writeDestination, readSource, SyncFnTy, BitsetFnTy, async>(
            loopName);
      }
    }
  }

  /**
   * Do sync necessary for write dest, read dest.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   *
   * @param loopName used to name timers for statistics
   */
  template <typename SyncFnTy, typename BitsetFnTy, bool async>
  inline void sync_dst_to_dst(std::string loopName) {
    // do nothing for IEC
    // reduce and broadcast for OEC, CVC, UVC
    if (!transposed || isVertexCut) {
      reduce<writeDestination, readDestination, SyncFnTy, BitsetFnTy, async>(
          loopName);
      broadcast<writeDestination, readDestination, SyncFnTy, BitsetFnTy, async>(
          loopName);
    }
  }

  /**
   * Do sync necessary for write dest, read any.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   *
   * @param loopName used to name timers for statistics
   */
  template <typename SyncFnTy, typename BitsetFnTy, bool async>
  inline void sync_dst_to_any(std::string loopName) {
    // only broadcast for IEC
    // reduce and broadcast for OEC, CVC, UVC
    if (!transposed || isVertexCut) {
      reduce<writeDestination, readAny, SyncFnTy, BitsetFnTy, async>(loopName);
    }
    broadcast<writeDestination, readAny, SyncFnTy, BitsetFnTy, async>(loopName);
  }

  /**
   * Do sync necessary for write any, read src.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   *
   * @param loopName used to name timers for statistics
   */
  template <typename SyncFnTy, typename BitsetFnTy, bool async>
  inline void sync_any_to_src(std::string loopName) {
    // only reduce for OEC
    // reduce and broadcast for IEC, CVC, UVC
    reduce<writeAny, readSource, SyncFnTy, BitsetFnTy, async>(loopName);
    if (transposed || isVertexCut) {
      broadcast<writeAny, readSource, SyncFnTy, BitsetFnTy, async>(loopName);
    }
  }

  /**
   * Do sync necessary for write any, read dst.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   *
   * @param loopName used to name timers for statistics
   */
  template <typename SyncFnTy, typename BitsetFnTy, bool async>
  inline void sync_any_to_dst(std::string loopName) {
    // only reduce for IEC
    // reduce and broadcast for OEC, CVC, UVC
    reduce<writeAny, readDestination, SyncFnTy, BitsetFnTy, async>(loopName);

    if (!transposed || isVertexCut) {
      broadcast<writeAny, readDestination, SyncFnTy, BitsetFnTy, async>(
          loopName);
    }
  }

  /**
   * Do sync necessary for write any, read any.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct that has info on how to access the bitset
   *
   * @param loopName used to name timers for statistics
   */
  template <typename SyncFnTy, typename BitsetFnTy, bool async>
  inline void sync_any_to_any(std::string loopName) {
    // reduce and broadcast for OEC, IEC, CVC, UVC
    reduce<writeAny, readAny, SyncFnTy, BitsetFnTy, async>(loopName);
    broadcast<writeAny, readAny, SyncFnTy, BitsetFnTy, async>(loopName);
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
   *
   * @param loopName used to name timers for statistics
   */
  template <WriteLocation writeLocation, ReadLocation readLocation,
            typename SyncFnTy, typename BitsetFnTy = galois::InvalidBitsetFnTy,
            bool async = false>
  inline void sync(std::string loopName) {
    if (partitionAgnostic) {
      sync_any_to_any<SyncFnTy, BitsetFnTy, async>(loopName);
    } else {
      if (writeLocation == writeSource) {
        if (readLocation == readSource) {
          sync_src_to_src<SyncFnTy, BitsetFnTy, async>(loopName);
        } else if (readLocation == readDestination) {
          sync_src_to_dst<SyncFnTy, BitsetFnTy, async>(loopName);
        } else { // readAny
          sync_src_to_any<SyncFnTy, BitsetFnTy, async>(loopName);
        }
      } else if (writeLocation == writeDestination) {
        if (readLocation == readSource) {
          sync_dst_to_src<SyncFnTy, BitsetFnTy, async>(loopName);
        } else if (readLocation == readDestination) {
          sync_dst_to_dst<SyncFnTy, BitsetFnTy, async>(loopName);
        } else { // readAny
          sync_dst_to_any<SyncFnTy, BitsetFnTy, async>(loopName);
        }
      } else { // writeAny
        if (readLocation == readSource) {
          sync_any_to_src<SyncFnTy, BitsetFnTy, async>(loopName);
        } else if (readLocation == readDestination) {
          sync_any_to_dst<SyncFnTy, BitsetFnTy, async>(loopName);
        } else { // readAny
          sync_any_to_any<SyncFnTy, BitsetFnTy, async>(loopName);
        }
      }
    }
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Sync on demand code (unmaintained, may not work)
  ////////////////////////////////////////////////////////////////////////////////
private:
  /**
   * Generic Sync on demand handler. Should NEVER get to this (hence
   * the galois die).
   */
  template <ReadLocation rl, typename SyncFnTy, typename BitsetFnTy>
  struct SyncOnDemandHandler {
    // note this call function signature is diff. from specialized versions:
    // will cause compile time error if this struct is used (which is what
    // we want)
    void call() { GALOIS_DIE("invalid read location for sync on demand"); }
  };

  /**
   * Sync on demand handler specialized for read source.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy tells program what data needs to be sync'd
   */
  template <typename SyncFnTy, typename BitsetFnTy>
  struct SyncOnDemandHandler<readSource, SyncFnTy, BitsetFnTy> {
    /**
     * Based on sync flags, handles syncs for cases when you need to read
     * at source
     *
     * @param substrate sync substrate
     * @param fieldFlags the flags structure specifying what needs to be
     * sync'd
     * @param loopName loopname used to name timers
     * @param bvFlag Copy of the bitvector status (valid/invalid at particular
     * locations)
     */
    static inline void call(GluonSubstrate* substrate,
                            galois::runtime::FieldFlags& fieldFlags,
                            std::string loopName, const BITVECTOR_STATUS&) {
      if (fieldFlags.src_to_src() && fieldFlags.dst_to_src()) {
        substrate->sync_any_to_src<SyncFnTy, BitsetFnTy>(loopName);
      } else if (fieldFlags.src_to_src()) {
        substrate->sync_src_to_src<SyncFnTy, BitsetFnTy>(loopName);
      } else if (fieldFlags.dst_to_src()) {
        substrate->sync_dst_to_src<SyncFnTy, BitsetFnTy>(loopName);
      }

      fieldFlags.clear_read_src();
    }
  };

  /**
   * Sync on demand handler specialized for read destination.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy tells program what data needs to be sync'd
   */
  template <typename SyncFnTy, typename BitsetFnTy>
  struct SyncOnDemandHandler<readDestination, SyncFnTy, BitsetFnTy> {
    /**
     * Based on sync flags, handles syncs for cases when you need to read
     * at destination
     *
     * @param substrate sync substrate
     * @param fieldFlags the flags structure specifying what needs to be
     * sync'd
     * @param loopName loopname used to name timers
     * @param bvFlag Copy of the bitvector status (valid/invalid at particular
     * locations)
     */
    static inline void call(GluonSubstrate* substrate,
                            galois::runtime::FieldFlags& fieldFlags,
                            std::string loopName, const BITVECTOR_STATUS&) {
      if (fieldFlags.src_to_dst() && fieldFlags.dst_to_dst()) {
        substrate->sync_any_to_dst<SyncFnTy, BitsetFnTy>(loopName);
      } else if (fieldFlags.src_to_dst()) {
        substrate->sync_src_to_dst<SyncFnTy, BitsetFnTy>(loopName);
      } else if (fieldFlags.dst_to_dst()) {
        substrate->sync_dst_to_dst<SyncFnTy, BitsetFnTy>(loopName);
      }

      fieldFlags.clear_read_dst();
    }
  };

  /**
   * Sync on demand handler specialized for read any.
   *
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy tells program what data needs to be sync'd
   */
  template <typename SyncFnTy, typename BitsetFnTy>
  struct SyncOnDemandHandler<readAny, SyncFnTy, BitsetFnTy> {
    /**
     * Based on sync flags, handles syncs for cases when you need to read
     * at both source and destination
     *
     * @param substrate sync substrate
     * @param fieldFlags the flags structure specifying what needs to be
     * sync'd
     * @param loopName loopname used to name timers
     * @param bvFlag Copy of the bitvector status (valid/invalid at particular
     * locations)
     */
    static inline void call(GluonSubstrate* substrate,
                            galois::runtime::FieldFlags& fieldFlags,
                            std::string loopName,
                            const BITVECTOR_STATUS& bvFlag) {
      bool src_write = fieldFlags.src_to_src() || fieldFlags.src_to_dst();
      bool dst_write = fieldFlags.dst_to_src() || fieldFlags.dst_to_dst();

      if (!(src_write && dst_write)) {
        // src or dst write flags aren't set (potentially both are not set),
        // but it's NOT the case that both are set, meaning "any" isn't
        // required in the "from"; can work at granularity of just src
        // write or dst wrte

        if (src_write) {
          if (fieldFlags.src_to_src() && fieldFlags.src_to_dst()) {
            if (bvFlag == BITVECTOR_STATUS::NONE_INVALID) {
              substrate->sync_src_to_any<SyncFnTy, BitsetFnTy>(loopName);
            } else if (galois::runtime::src_invalid(bvFlag)) {
              // src invalid bitset; sync individually so it can be called
              // without bitset
              substrate->sync_src_to_dst<SyncFnTy, BitsetFnTy>(loopName);
              substrate->sync_src_to_src<SyncFnTy, BitsetFnTy>(loopName);
            } else if (galois::runtime::dst_invalid(bvFlag)) {
              // dst invalid bitset; sync individually so it can be called
              // without bitset
              substrate->sync_src_to_src<SyncFnTy, BitsetFnTy>(loopName);
              substrate->sync_src_to_dst<SyncFnTy, BitsetFnTy>(loopName);
            } else {
              GALOIS_DIE("invalid bitvector flag setting in syncOnDemand");
            }
          } else if (fieldFlags.src_to_src()) {
            substrate->sync_src_to_src<SyncFnTy, BitsetFnTy>(loopName);
          } else { // src to dst is set
            substrate->sync_src_to_dst<SyncFnTy, BitsetFnTy>(loopName);
          }
        } else if (dst_write) {
          if (fieldFlags.dst_to_src() && fieldFlags.dst_to_dst()) {
            if (bvFlag == BITVECTOR_STATUS::NONE_INVALID) {
              substrate->sync_dst_to_any<SyncFnTy, BitsetFnTy>(loopName);
            } else if (galois::runtime::src_invalid(bvFlag)) {
              substrate->sync_dst_to_dst<SyncFnTy, BitsetFnTy>(loopName);
              substrate->sync_dst_to_src<SyncFnTy, BitsetFnTy>(loopName);
            } else if (galois::runtime::dst_invalid(bvFlag)) {
              substrate->sync_dst_to_src<SyncFnTy, BitsetFnTy>(loopName);
              substrate->sync_dst_to_dst<SyncFnTy, BitsetFnTy>(loopName);
            } else {
              GALOIS_DIE("invalid bitvector flag setting in syncOnDemand");
            }
          } else if (fieldFlags.dst_to_src()) {
            substrate->sync_dst_to_src<SyncFnTy, BitsetFnTy>(loopName);
          } else { // dst to dst is set
            substrate->sync_dst_to_dst<SyncFnTy, BitsetFnTy>(loopName);
          }
        }

        // note the "no flags are set" case will enter into this block
        // as well, and it is correctly handled by doing nothing since
        // both src/dst_write will be false
      } else {
        // it is the case that both src/dst write flags are set, so "any"
        // is required in the "from"; what remains to be determined is
        // the use of src, dst, or any for the destination of the sync
        bool src_read = fieldFlags.src_to_src() || fieldFlags.dst_to_src();
        bool dst_read = fieldFlags.src_to_dst() || fieldFlags.dst_to_dst();

        if (src_read && dst_read) {
          if (bvFlag == BITVECTOR_STATUS::NONE_INVALID) {
            substrate->sync_any_to_any<SyncFnTy, BitsetFnTy>(loopName);
          } else if (galois::runtime::src_invalid(bvFlag)) {
            substrate->sync_any_to_dst<SyncFnTy, BitsetFnTy>(loopName);
            substrate->sync_any_to_src<SyncFnTy, BitsetFnTy>(loopName);
          } else if (galois::runtime::dst_invalid(bvFlag)) {
            substrate->sync_any_to_src<SyncFnTy, BitsetFnTy>(loopName);
            substrate->sync_any_to_dst<SyncFnTy, BitsetFnTy>(loopName);
          } else {
            GALOIS_DIE("invalid bitvector flag setting in syncOnDemand");
          }
        } else if (src_read) {
          substrate->sync_any_to_src<SyncFnTy, BitsetFnTy>(loopName);
        } else { // dst_read
          substrate->sync_any_to_dst<SyncFnTy, BitsetFnTy>(loopName);
        }
      }

      fieldFlags.clear_read_src();
      fieldFlags.clear_read_dst();
    }
  };

  ////////////////////////////////////////////////////////////////////////////////
  // Public sync interface
  ////////////////////////////////////////////////////////////////////////////////

public:
  /**
   * Given a structure that contains flags signifying what needs to be
   * synchronized, syncOnDemand will synchronize what is necessary based
   * on the read location of the * field.
   *
   * @tparam readLocation Location in which field will need to be read
   * @tparam SyncFnTy sync structure for the field
   * @tparam BitsetFnTy struct which holds a bitset which can be used
   * to control synchronization at a more fine grain level
   * @param fieldFlags structure for field you are syncing
   * @param loopName Name of loop this sync is for for naming timers
   */
  template <ReadLocation readLocation, typename SyncFnTy,
            typename BitsetFnTy = galois::InvalidBitsetFnTy>
  inline void syncOnDemand(galois::runtime::FieldFlags& fieldFlags,
                           std::string loopName) {
    std::string timer_str("Sync_" + get_run_identifier(loopName));
    galois::StatTimer Tsync(timer_str.c_str(), RNAME);
    Tsync.start();

    currentBVFlag = &(fieldFlags.bitvectorStatus);

    // call a template-specialized function depending on the read location
    SyncOnDemandHandler<readLocation, SyncFnTy, BitsetFnTy>::call(
        this, fieldFlags, loopName, *currentBVFlag);

    currentBVFlag = nullptr;

    Tsync.stop();
  }

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
   * Get a run identifier using the set run and set round and
   * append to the passed in string in addition to the number identifier passed
   * in.
   *
   * @param loop_name String to append the run identifier
   * @param alterID another ID with which to add to the timer name.
   *
   * @returns String with run identifier appended to passed in loop name +
   * alterID
   */
  inline std::string get_run_identifier(std::string loop_name,
                                        unsigned alterID) const {
#if GALOIS_PER_ROUND_STATS
    return std::string(std::string(loop_name) + "_" + std::to_string(alterID) +
                       "_" + std::to_string(num_run) + "_" +
                       std::to_string(num_round));
#else
    return std::string(std::string(loop_name) + "_" + std::to_string(alterID) +
                       "_" + std::to_string(num_run));
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

      // GPU call
      bool batch_succeeded = FnTy::reset_batch(r.first, r.second - 1);

      // CPU always enters this block
      if (!batch_succeeded) {
        galois::do_all(
            galois::iterate(r.first, r.second),
            [&](uint32_t lid) { FnTy::reset(lid, userGraph.getData(lid)); },
            galois::no_stats());
      }
    }
  }

/* For Polling */
private:
    uint8_t stopUpdateBuffer = 0;
    bool stopDedicated = false;
    bool terminateFlag = false;
    std::vector<uint8_t*> sendWorkBuffer;

public:
    void reset_termination() {
        stopUpdateBuffer = 0;
        stopDedicated = false;
        terminateFlag = false;
        net.resetTermination();
    }

    void set_update_buf_to_identity(ValTy identity) {
        galois::do_all(
            galois::iterate((uint32_t)0, (uint32_t)phantomMasterUpdateBuffer.size()),
            [&](uint32_t lid) {
                if(phantomMasterUpdateBuffer[lid]) {
                    *(phantomMasterUpdateBuffer[lid]) = identity;
                }
            },
            galois::no_stats());
    }

    template<typename FnTy>
    void poll_for_remote_work_dedicated(const ValTy (*func)(ValTy&, const ValTy&)) {
        bool success;
        uint8_t* buf;
        size_t bufLen;
        
        while (stopUpdateBuffer == 0) {
            success = false;
            buf = nullptr;
            bufLen = 0;
            do {
                if (stopUpdateBuffer == 1) {
                    break;
                }

                success = net.receiveRemoteWork(buf, bufLen);
                if (!success) {
                    galois::substrate::asmPause();
                }
            } while (!success);
            
            if (success) { // received message
                // dedicated thread does not care about the number of aggregated message count
                bufLen -= sizeof(uint32_t);
                size_t offset = 0;

                uint32_t lid;
#ifndef GALOIS_EXCHANGE_PHANTOM_LID
                uint64_t gid;
#endif
                ValTy val;

                while (offset != bufLen) {
#ifdef GALOIS_EXCHANGE_PHANTOM_LID
                    std::memcpy(&lid, buf + offset, sizeof(uint32_t));
                    offset += sizeof(uint32_t);
#else
                    std::memcpy(&gid, buf + offset, sizeof(uint64_t));
                    offset += sizeof(uint64_t);
                    lid = userGraph.getLID(gid);
#endif
                    std::memcpy(&val, buf + offset, sizeof(val));
                    offset += sizeof(val);
                    func(*(phantomMasterUpdateBuffer[lid]), val);
                }

                net.deallocateRecvBuffer(buf);
            }
        }

        stopUpdateBuffer = 2;
        
        while (!stopDedicated) {
            success = false;
            buf = nullptr;
            bufLen = 0;
            do {
                if (stopDedicated) {
                    break;
                }

                success = net.receiveRemoteWork(buf, bufLen);
                if (!success) {
                    galois::substrate::asmPause();
                }
            } while (!success);
            
            if (success) { // received message
                // dedicated thread does not care about the number of aggregated message count
                bufLen -= sizeof(uint32_t);
                size_t offset = 0;

                uint32_t lid;
#ifndef GALOIS_EXCHANGE_PHANTOM_LID
                uint64_t gid;
#endif
                ValTy val;

                while (offset != bufLen) {
#ifdef GALOIS_EXCHANGE_PHANTOM_LID
                    std::memcpy(&lid, buf + offset, sizeof(uint32_t));
                    offset += sizeof(uint32_t);
#else
                    std::memcpy(&gid, buf + offset, sizeof(uint64_t));
                    offset += sizeof(uint64_t);
                    lid = userGraph.getLID(gid);
#endif
                    std::memcpy(&val, buf + offset, sizeof(val));
                    offset += sizeof(val);
                    FnTy::reduce_atomic(lid, userGraph.getData(lid), val);
                }

                net.deallocateRecvBuffer(buf);
            }
        }

    }
    
    template<typename FnTy>
    void sync_update_buf(ValTy identity) {
        while (stopUpdateBuffer != 2) {}
        galois::do_all(
            galois::iterate((uint32_t)0, (uint32_t)phantomMasterUpdateBuffer.size()),
            [&](uint32_t lid) {
                if(phantomMasterUpdateBuffer[lid]) {
                    if (*(phantomMasterUpdateBuffer[lid]) != identity) { // there is update
                        FnTy::reduce_atomic(lid, userGraph.getData(lid), *(phantomMasterUpdateBuffer[lid]));
                    }
                }
            },
            galois::no_stats());

        stopDedicated = true;
    }
    
    template<typename FnTy>
    void poll_for_remote_work() {
        if (!terminateFlag) {
            bool success;
            uint8_t* buf;
            size_t bufLen;
            while (true) {
                success = false;
                buf = nullptr;
                bufLen = 0;
                do {
                    success = net.receiveRemoteWork(terminateFlag, buf, bufLen);
                    if (terminateFlag) {
                        break;
                    }
                    if (!success) {
                        galois::substrate::asmPause();
                    }
                } while (!success);
                
                if (terminateFlag) {
                    break;
                }

                if (success) { // received message
                    uint32_t msgCount;
                    std::memcpy(&msgCount, buf + bufLen - sizeof(uint32_t), sizeof(uint32_t));

                    galois::on_each(
                        [&](unsigned tid, unsigned numT) {
                            unsigned quotient = msgCount / numT;
                            unsigned remainder = msgCount % numT;
                            unsigned start, size;
                            if (tid < remainder) {
                                start = tid * quotient + tid;
                                size = quotient + 1;
                            }
                            else {
                                start = tid * quotient + remainder;
                                size = quotient;
                            }
#ifdef GALOIS_EXCHANGE_PHANTOM_LID
                            size_t offset = start * (sizeof(uint32_t) + sizeof(ValTy));
#else
                            size_t offset = start * (sizeof(uint64_t) + sizeof(ValTy));
#endif
                            
                            uint32_t lid;
#ifndef GALOIS_EXCHANGE_PHANTOM_LID
                            uint64_t gid;
#endif
                            ValTy val;

                            for (unsigned i=0; i<size; i++) {
#ifdef GALOIS_EXCHANGE_PHANTOM_LID
                                std::memcpy(&lid, buf + offset, sizeof(uint32_t));
                                offset += sizeof(uint32_t);
#else
                                std::memcpy(&gid, buf + offset, sizeof(uint64_t));
                                offset += sizeof(uint64_t);
                                lid = userGraph.getLID(gid);
#endif
                                std::memcpy(&val, buf + offset, sizeof(val));
                                offset += sizeof(val);
                                FnTy::reduce_atomic(lid, userGraph.getData(lid), val);
                            }
                        }
                    );

                    net.deallocateRecvBuffer(buf);
                }
            }
        }
    }
    
    template<typename FnTy>
    void check_remote_work() {
        if (!terminateFlag) {
            bool success = false;;
            uint8_t* buf = nullptr;
            size_t bufLen = 0;
            do {
                success = net.receiveRemoteWork(terminateFlag, buf, bufLen);
                
                if (success) { // received message
                    uint32_t msgCount;
                    std::memcpy(&msgCount, buf + bufLen - sizeof(uint32_t), sizeof(uint32_t));

                    galois::on_each(
                        [&](unsigned tid, unsigned numT) {
                            unsigned quotient = msgCount / numT;
                            unsigned remainder = msgCount % numT;
                            unsigned start, size;
                            if (tid < remainder) {
                                start = tid * quotient + tid;
                                size = quotient + 1;
                            }
                            else {
                                start = tid * quotient + remainder;
                                size = quotient;
                            }
#ifdef GALOIS_EXCHANGE_PHANTOM_LID
                            size_t offset = start * (sizeof(uint32_t) + sizeof(ValTy));
#else
                            size_t offset = start * (sizeof(uint64_t) + sizeof(ValTy));
#endif
                            
                            uint32_t lid;
#ifndef GALOIS_EXCHANGE_PHANTOM_LID
                            uint64_t gid;
#endif
                            ValTy val;

                            for (unsigned i=0; i<size; i++) {
#ifdef GALOIS_EXCHANGE_PHANTOM_LID
                                std::memcpy(&lid, buf + offset, sizeof(uint32_t));
                                offset += sizeof(uint32_t);
#else
                                std::memcpy(&gid, buf + offset, sizeof(uint64_t));
                                offset += sizeof(uint64_t);
                                lid = userGraph.getLID(gid);
#endif
                                std::memcpy(&val, buf + offset, sizeof(val));
                                offset += sizeof(val);
                                FnTy::reduce_atomic(lid, userGraph.getData(lid), val);
                            }
                        }
                    );

                    net.deallocateRecvBuffer(buf);
                }
            } while (success);
        }
    }

    void net_flush() {
        net.flushRemoteWork();
        net.broadcastTermination();
        stopUpdateBuffer = 1;
    }

#ifdef GALOIS_EXCHANGE_PHANTOM_LID
    void send_data_to_remote(unsigned dst, uint32_t lid, ValTy val) {
#else
    void send_data_to_remote(unsigned dst, uint64_t gid, ValTy val) {
#endif
        unsigned tid = galois::substrate::ThreadPool::getTID();
        
        // serialize
        uint8_t* bufferPtr = sendWorkBuffer[tid];
        size_t offset = 0;
#ifdef GALOIS_EXCHANGE_PHANTOM_LID
        std::memcpy(bufferPtr, &lid, sizeof(lid));
        offset += sizeof(lid);
#else
        std::memcpy(bufferPtr, &gid, sizeof(gid));
        offset += sizeof(gid);
#endif
        std::memcpy(bufferPtr + offset, &val, sizeof(val));
        offset += sizeof(val);

        net.sendWork(tid, dst, bufferPtr, offset);
    }

};

template <typename GraphTy, typename ValTy>
constexpr const char* const galois::graphs::GluonSubstrate<GraphTy, ValTy>::RNAME;
} // end namespace graphs
} // end namespace galois

#endif // header guard
