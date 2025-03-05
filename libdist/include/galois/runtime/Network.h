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
 * @file Network.h
 *
 * Contains the network interface class which is the base class for all
 * network layer implementations.
 */

#ifndef GALOIS_RUNTIME_NETWORK_H
#define GALOIS_RUNTIME_NETWORK_H

#include "galois/runtime/Serialize.h"
#include "galois/runtime/MemUsage.h"
#include "galois/substrate/Barrier.h"

#include <mpi.h>

#include <cstdint>
#include <optional>
#include <tuple>

namespace galois::runtime {

//! typedef for buffer that stores data to be sent out
using SendBuffer = SerializeBuffer;
//! typedef for buffer that received data is saved into
using RecvBuffer = DeSerializeBuffer;

/**
 * A class that defines functions that a network interface in Galois should
 * have. How the sends/recvs/stat-collecting happens as well
 * as the network layer itself is up to the implemention of the class.
 */
class NetworkInterface {
protected:
  //! Initialize the MPI system. Should only be called once per process.
  void initializeMPI();

  //! Finalize the MPI system. Should only be called once per process.
  void finalizeMPI();

  //! Memory usage tracker
  MemUsageTracker memUsageTracker;

#ifdef GALOIS_USE_BARE_MPI
public:
  //! Wrapper that calls into increment mem usage on the memory usage tracker
  inline void incrementMemUsage(uint64_t size) {
    memUsageTracker.incrementMemUsage(size);
  }
  //! Wrapper that calls into decrement mem usage on the memory usage tracker
  inline void decrementMemUsage(uint64_t size) {
    memUsageTracker.decrementMemUsage(size);
  }
#endif

public:
  //! This machine's host ID
  static uint32_t ID;
  //! The total number of machines in the current program
  static uint32_t Num;

  /**
   * Constructor for interface.
   */
  NetworkInterface();

  /**
   * Destructor destroys MPI (if it exists).
   */
  virtual ~NetworkInterface();
  
  virtual void allocateRecvCommBuffer(size_t alloc_size) = 0;
  
  virtual void deallocateRecvBuffer(uint8_t* buf) = 0;

  //! Send a message to a given (dest) host.  A message is simply a
  //! landing pad (recv, funciton pointer) and some data (buf)
  //! on the receiver, recv(buf) will be called durring handleReceives()
  //! buf is invalidated by this operation
  void sendMsg(uint32_t dest, void (*recv)(uint32_t, RecvBuffer&),
               SendBuffer& buf);

  //! Send a message letting the network handle the serialization and
  //! deserialization slightly slower
  template <typename... Args>
  void sendSimple(uint32_t dest, void (*recv)(uint32_t, Args...),
                  Args... param);

  //! Send a message to a given (dest) host.  A message is simply a
  //! tag (tag) and some data (buf)
  //! on the receiver, buf will be returned on a receiveTagged(tag)
  //! buf is invalidated by this operation
  virtual void sendTagged(uint32_t dest, uint32_t tag, SendBuffer& buf,
                          int type = 0) = 0;
  
  virtual void sendWork(unsigned tid, uint32_t dest, uint8_t* bufPtr, size_t len) = 0;
  
  virtual void sendComm(uint32_t dest, uint8_t* bufPtr, size_t len) = 0;

  //! Send a message to all hosts.  A message is simply a
  //! landing pad (recv) and some data (buf)
  //! buf is invalidated by this operation
  void broadcast(void (*recv)(uint32_t, RecvBuffer&), SendBuffer& buf,
                 bool self = false);

  //! Broadcast a message allowing the network to handle serialization and
  //! deserialization
  template <typename... Args>
  void broadcastSimple(void (*recv)(uint32_t, Args...), Args... param);

  //! Receive and dispatch messages
  void handleReceives();

  //! Wrapper to reset the mem usage tracker's stats
  inline void resetMemUsage() { memUsageTracker.resetMemUsage(); }

  //! Reports the memory usage tracker's statistics to the stat manager
  void reportMemUsage() const;

  //! Receive a tagged message
  virtual std::optional<std::pair<uint32_t, RecvBuffer>>
  receiveTagged(uint32_t tag, int type = 0) = 0;
  
  virtual std::optional<std::pair<uint32_t, RecvBuffer>>
  receiveTagged(bool& terminateFlag, uint32_t tag, int type = 0) = 0;
  
  virtual std::optional<std::pair<uint32_t, RecvBuffer>>
  receiveTaggedFromHost(uint32_t host, bool& terminateFlag, uint32_t tag, int type = 0) = 0;
  
  virtual bool receiveRemoteWork(uint8_t*& work, size_t& workLen) = 0;
  
  virtual bool receiveRemoteWork(bool& terminateFlag, uint8_t*& work, size_t& workLen) = 0;
  
  virtual bool receiveComm(uint32_t& host, uint8_t*& work) = 0;
  
  virtual void resetWorkTermination() = 0;

  virtual void resetDataTermination() = 0;
  
  //! move send buffers out to network
  virtual void flush() = 0;
  
  virtual void flushData() = 0;
  
  virtual void flushRemoteWork() = 0;
  
  virtual void flushComm() = 0;
  
  virtual void broadcastWorkTermination() = 0;
  
  virtual void signalDataTermination(uint32_t dest) = 0;

  //! @returns true if any send is in progress or is pending to be enqueued
  virtual bool anyPendingSends() = 0;

  //! @returns true if any receive is in progress or is pending to be dequeued
  virtual bool anyPendingReceives() = 0;
};

//! Variable that keeps track of which network send/recv phase a program is
//! currently on. Can be seen as a count of send/recv rounds that have occured.
extern uint32_t evilPhase;

//! Reserved tag for remote work
extern uint32_t remoteWorkTag;
//! Reserved tag for remote work termination message
extern uint32_t workTerminationTag;
//! Reserved tag for communication
extern uint32_t communicationTag;
//! Reserved tag for remote data termination message
extern uint32_t dataTerminationTag;

//! Get the network interface
//! @returns network interface
NetworkInterface& getSystemNetworkInterface();

namespace internal {
//! Deletes the system network interface (if it exists).
void destroySystemNetworkInterface();
} // namespace internal

//! Gets this host's ID
//! @returns ID of this host
uint32_t getHostID();

//! Gets the number of hosts
//! @returns number of hosts
uint32_t getHostNum();

//! Returns a BufferedNetwork interface
NetworkInterface& makeNetworkBuffered();

//! Returns a LCINetwork interface
NetworkInterface& makeNetworkLCI();

//! Returns a host barrier, which is a regular MPI-Like Barrier for all hosts.
//! @warning Should not be called within a parallel region; assumes only one
//! thread is calling it
substrate::Barrier& getHostBarrier();
//! Returns a fence that ensures all pending messages are delivered, acting
//! like a memory-barrier
substrate::Barrier& getHostFence();

////////////////////////////////////////////////////////////////////////////////
// Implementations
////////////////////////////////////////////////////////////////////////////////
namespace { // anon
template <typename... Args>
static void genericLandingPad(uint32_t src, RecvBuffer& buf) {
  void (*fp)(uint32_t, Args...);
  std::tuple<Args...> args;
  gDeserialize(buf, fp, args);
  std::apply([fp, src](Args... params) { fp(src, params...); }, args);
}

} // namespace

template <typename... Args>
void NetworkInterface::sendSimple(uint32_t dest,
                                  void (*recv)(uint32_t, Args...),
                                  Args... param) {
  SendBuffer buf;
  gSerialize(buf, (uintptr_t)recv, param...,
             (uintptr_t)genericLandingPad<Args...>);
  sendTagged(dest, 0, buf);
}

template <typename... Args>
void NetworkInterface::broadcastSimple(void (*recv)(uint32_t, Args...),
                                       Args... param) {
  SendBuffer buf;
  gSerialize(buf, (uintptr_t)recv, param...);
  broadcast(genericLandingPad<Args...>, buf, false);
}

} // namespace galois::runtime
#endif
