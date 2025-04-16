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
#include "galois/substrate/Barrier.h"
#include "galois/runtime/Mem.h"
#include "galois/runtime/readerwriterqueue.h"
#include "galois/runtime/concurrentqueue.h"
#include "llvm/Support/CommandLine.h"

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

  uint32_t workCount;
  size_t aggMsgSize;
  size_t sendBufCount;
  size_t recvBufCount;
  
protected:
  //! Initialize the MPI system. Should only be called once per process.
  void initializeMPI();

  //! Finalize the MPI system. Should only be called once per process.
  void finalizeMPI();

private:
  std::vector<FixedSizeBufferAllocator> sendAllocators;
  FixedSizeBufferAllocator recvAllocator;

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

      uint32_t frontTag;
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
      // single producer multiple consumer
      moodycamel::ConcurrentQueue<uint8_t*> fullMessages;
      moodycamel::ConcurrentQueue<std::pair<uint8_t*, size_t>> partialMessages;

      moodycamel::ProducerToken ptokFull, ptokPartial;

  public:
      recvBufferRemoteWork() : ptokFull(fullMessages), ptokPartial(partialMessages) {}

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

  public:
      sendBufferData() {}
    
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

  public:
      sendBufferRemoteWork() : net(nullptr), tid(0), buf(nullptr), msgCount(0) {}

      void setNet(NetworkInterface* _net);
      
      inline void setTID(unsigned _tid) {
          tid = _tid;
      }
      
      void setFlush();

      inline bool checkPartial() {
          return partialFlag;
      }

      void popPartial(uint8_t*& work, size_t& workLen);
    
      bool pop(uint8_t*& work);

      template <typename ValTy>
      void add(uint32_t lid, ValTy val);

      inline void touchBuf() {
          *buf = (uint8_t)0;
      }

      inline void prefetchBuf() {
          __builtin_prefetch(buf, 1, 3);
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
  std::vector<bool> sendWorkTerminationValid;
  uint32_t hostWorkTerminationBase;
  std::atomic<uint32_t> hostWorkTerminationCount;
  
  std::atomic<uint32_t> hostDataTerminationCount;

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
  //! tag (tag) and some data (buf)
  //! on the receiver, buf will be returned on a receiveTagged(tag)
  //! buf is invalidated by this operation
  void sendTagged(uint32_t dest, uint32_t tag, SendBuffer& buf,
                          int type = 0);
  
  template <typename ValTy>
  void sendWork(unsigned tid, uint32_t dest, uint32_t lid, ValTy val);
  
  void sendComm(uint32_t dest, uint8_t* bufPtr, size_t len);

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

  bool receiveRemoteWork(std::atomic<bool>& terminateFlag, bool& fullFlag, uint8_t*& work, size_t& workLen);
  
  void receiveComm(uint32_t& host, uint8_t*& work);
  
  //! move send buffers out to network
  void flushRemoteWork();

  void excludeSendWorkTermination(uint32_t host);

  void excludeHostWorkTermination();
  
  void resetWorkTermination();
  
  void resetDataTermination();
  
  void signalDataTermination(uint32_t dest);

  void broadcastWorkTermination();

  void touchBufferPool();

  void prefetchBuffers();
};

//! Variable that keeps track of which network send/recv phase a program is
//! currently on. Can be seen as a count of send/recv rounds that have occured.
extern uint32_t evilPhase;

inline constexpr uint32_t remoteWorkTag = 0; // 0 is reserved for remote work
inline constexpr uint32_t workTerminationTag = 1; // 1 is reserved for remote work termination message
inline constexpr uint32_t communicationTag = 2; // 2 is reserved for communication phase
inline constexpr uint32_t dataTerminationTag = 3; // 3 is reserved for remote data termination message

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

} // namespace galois::runtime
#endif
