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
#include "galois/runtime/Mem.h"
#include "galois/runtime/readerwriterqueue.h"

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
public:
  MPI_Comm comm_barrier, comm_comm;

  static constexpr uint32_t WORK_SIZE = 8; // lid (uint32_t) + val (uint32_t or float)
  static constexpr uint32_t WORK_COUNT = 1 << 12;
  static constexpr size_t AGG_MSG_SIZE = WORK_SIZE * WORK_COUNT;
  static constexpr size_t SEND_BUF_COUNT = 1 << 14;
  static constexpr size_t RECV_BUF_COUNT = 1 << 16;
  
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

private:
  std::vector<FixedSizeBufferAllocator<AGG_MSG_SIZE, SEND_BUF_COUNT>> sendAllocators;
  FixedSizeBufferAllocator<AGG_MSG_SIZE, RECV_BUF_COUNT> recvAllocator;

  std::vector<uint8_t*> recvCommBuffer;

  unsigned numT;

  using vTy = galois::PODResizeableArray<uint8_t>;
  
  /**
   * Wrapper for dealing with MPI error codes. Program dies if the error code
   * isn't MPI_SUCCESS.
   *
   * @param rc Error code to check for success
   */
  static void handleError(int rc) {
      if (rc != MPI_SUCCESS) {
          MPI_Abort(MPI_COMM_WORLD, rc);
      }
  }
  
  /**
   * Get the host id of the caller.
   *
   * @returns host id of the caller with regard to the MPI setup
   */
  int getID() {
      int rank;
      handleError(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
      return rank;
  }

  /**
   * Get the total number of hosts in the system.
   *
   * @returns number of hosts with regard to the MPI setup
   */
  int getNum() {
      int hostSize;
      handleError(MPI_Comm_size(MPI_COMM_WORLD, &hostSize));
      return hostSize;
  }

  struct recvMessage {
      uint32_t tag;  //!< tag on message indicating distinct communication phases
      vTy data;      //!< data portion of message

      //! Default constructor initializes host and tag to large numbers.
      recvMessage() : tag(~0) {}
      //! @param h Host to send message to
      //! @param t Tag to associate with message
      //! @param d Data to save in message
      recvMessage(uint32_t _tag, vTy&& _data) : tag(_tag), data(std::move(_data)) {}
  };

  /**
   * Receive buffers for the buffered network interface
   */
  class recvBufferData {
      // single producer single consumer
      moodycamel::ReaderWriterQueue<recvMessage> messages;

      std::atomic<uint32_t> frontTag;
      recvMessage frontMsg;

  public:
      recvBufferData() : frontTag(~0U) {}

      RecvBuffer pop();

      void add(uint32_t tag, vTy&& vec);
      
      bool hasMsg(uint32_t tag);
  }; // end recv buffer class

  std::vector<recvBufferData> recvData;
  
  /**
   * Receive buffers for the buffered network interface
   */
  class recvBufferCommunication {
      // single producer single consumer
      moodycamel::ReaderWriterQueue<std::pair<uint32_t, uint8_t*>> messages;

  public:
      recvBufferCommunication() {}

      bool tryPopMsg(uint32_t& host, uint8_t*& work);
      
      void add(uint32_t host, uint8_t* work);
  }; // end recv buffer class

  recvBufferCommunication recvCommunication;

  /**
   * Receive buffers for the buffered network interface
   */
  class recvBufferRemoteWork {
      // single producer single consumer
      moodycamel::ReaderWriterQueue<uint8_t*> fullMessages;
      moodycamel::ReaderWriterQueue<std::pair<uint8_t*, size_t>> partialMessages;

  public:
      recvBufferRemoteWork() {}

      bool tryPopFullMsg(uint8_t*& work);
      bool tryPopPartialMsg(uint8_t*& work, size_t& workLen);

      void addFull(uint8_t* work);
      void addPartial(uint8_t* work, size_t workLen);
  }; // end recv buffer class

  recvBufferRemoteWork recvRemoteWork;

  /**
   * Single producer single consumer with multiple tags
   */
  class sendBufferData {
      moodycamel::ReaderWriterQueue<std::tuple<uint32_t, uint8_t*, size_t>> messages;

      std::atomic<size_t> flush;

  public:
      sendBufferData() : flush(0) {}
    
      bool checkFlush() {
          return flush > 0;
      }
    
      bool pop(uint32_t& tag, uint8_t*& data, size_t& dataLen);

      void push(uint32_t tag, uint8_t* data, size_t dataLen);
  };

  std::vector<sendBufferData> sendData;

  /**   
   * single producer single consumer with single tag
   */
  class sendBufferRemoteWork {
      NetworkInterface* net;
      unsigned tid;

      moodycamel::ReaderWriterQueue<uint8_t*> messages;

      uint8_t* buf;
      uint32_t msgCount;

      std::pair<uint8_t*, size_t> partialMessage;
      std::atomic<bool> partialFlag;
      
      std::atomic<size_t> flush;

  public:
      sendBufferRemoteWork() : net(nullptr), tid(0), buf(nullptr), msgCount(0), flush(0) {}

      void setNet(NetworkInterface* _net);
      
      void setTID(unsigned _tid) {
          tid = _tid;
      }
      
      void setFlush();
    
      bool checkFlush() {
          return flush > 0;
      }

      bool checkPartial() {
          return partialFlag;
      }

      void popPartial(uint8_t*& work, size_t& workLen);
    
      bool pop(uint8_t*& work);

      template <typename ValTy>
      void add(uint32_t lid, ValTy val);

      inline void touchBuf() {
          volatile uint8_t temp = *buf;
      }
  };
  
  std::vector<std::vector<sendBufferRemoteWork>> sendRemoteWork;

  /**
   * Message type to recv in this network IO layer.
   */
  struct trackMessageSend {
      uint8_t* buf;
      MPI_Request req;
        
      trackMessageSend(uint8_t* _buf) : buf(_buf) {}
  };
  
  std::vector<std::deque<trackMessageSend>> sendInflight;
    
  void sendTrackComplete();

  void send(uint32_t dest, uint32_t tag, uint8_t* buf, size_t bufLen);
  
  void sendFullTrack(unsigned tid, uint32_t dest, uint8_t* buf);

  void sendPartialTrack(unsigned tid, uint32_t dest, uint8_t* buf, size_t bufLen);

  /**
   * Message type to recv in this network IO layer.
   */
  struct mpiMessageRecv {
      uint32_t host;
      uint32_t tag;
      vTy data;
      uint8_t* buf;
      size_t bufLen;
      MPI_Request req;
        
      mpiMessageRecv(uint32_t host, uint32_t tag, size_t len) : host(host), tag(tag), data(len) {}
      mpiMessageRecv(uint32_t host, uint32_t tag, uint8_t* b, size_t len) : host(host), tag(tag), buf(b), bufLen(len) {}
  };
  
  std::deque<mpiMessageRecv> recvInflight;
  
  void recvProbe();
  
  void workerThread();
  
  std::thread worker;
  std::atomic<int> ready;
  
  std::vector<std::atomic<bool>> sendWorkTermination;
  std::vector<std::atomic<uint32_t>> hostWorkTermination;
  std::vector<bool> sendWorkTerminationValid;
  
  std::vector<std::atomic<uint32_t>> hostDataTermination;
  std::vector<bool> hostWorkTerminationValid;

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
  ~NetworkInterface();

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
  void sendTagged(uint32_t dest, uint32_t tag, SendBuffer& buf,
                          int type = 0);
  
  template <typename ValTy>
  void sendWork(unsigned tid, uint32_t dest, uint32_t lid, ValTy val);
  
  void sendComm(uint32_t dest, uint8_t* bufPtr, size_t len);

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

  void allocateRecvCommBuffer(size_t alloc_size);

  void deallocateRecvBuffer(uint8_t* buf);

  //! Receive a tagged message
  std::optional<std::pair<uint32_t, RecvBuffer>>
  receiveTagged(uint32_t tag, int type = 0);
  
  std::optional<std::pair<uint32_t, RecvBuffer>>
  receiveTagged(bool& terminateFlag, uint32_t tag, int type = 0);
  
  void receiveRemoteWorkUntilSignal(std::atomic<bool>& stopFlag, bool& fullFlag, uint8_t*& work, size_t& workLen);

  void receiveRemoteWork(bool& terminateFlag, bool& fullFlag, uint8_t*& work, size_t& workLen);
  
  void receiveComm(uint32_t& host, uint8_t*& work);
  
  //! move send buffers out to network
  void flushRemoteWork();

  void excludeSendWorkTermination(uint32_t host);

  void excludeHostWorkTermination(uint32_t host);
  
  void resetWorkTermination();

  bool checkWorkTermination();
  
  void resetDataTermination();

  bool checkDataTermination();
  
  void signalDataTermination(uint32_t dest);

  void broadcastWorkTermination();

  //! Wrapper to reset the mem usage tracker's stats
  inline void resetMemUsage() { memUsageTracker.resetMemUsage(); }

  //! Reports the memory usage tracker's statistics to the stat manager
  void reportMemUsage() const;

  // touch all the buffers in the buffer pool
  void touchBufferPool();
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
