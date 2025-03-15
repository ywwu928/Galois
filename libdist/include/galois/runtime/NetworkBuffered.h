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
 * @file NetworkBuffered.cpp
 *
 * Contains NetworkInterfaceBuffered, an implementation of a network interface
 * that buffers messages before sending them out.
 *
 * @todo document this file more
 */

#include "galois/runtime/Network.h"
#include "galois/runtime/Mem.h"
#include "galois/concurrentqueue.h"

namespace galois::runtime {

/**
 * @class NetworkInterfaceBuffered
 *
 * Buffered network interface: messages are buffered before they are sent out.
 * A single worker thread is initialized to send/receive messages from/to
 * buffers.
 */
class NetworkInterfaceBuffered : public NetworkInterface {
  static const size_t AGG_MSG_SIZE = 2 << 14;
  static const size_t SEND_BUF_COUNT = 2 << 13;
  static const size_t RECV_BUF_COUNT = 2 << 15;

  std::vector<FixedSizeBufferAllocator<AGG_MSG_SIZE, SEND_BUF_COUNT>> sendAllocators;
  FixedSizeBufferAllocator<AGG_MSG_SIZE, RECV_BUF_COUNT> recvAllocator;

  std::vector<uint8_t*> recvCommBuffer;

  bool anyReceivedMessages;

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
      recvMessage(uint32_t t, vTy&& d) : tag(t), data(std::move(d)) {}
  };

  /**
   * Receive buffers for the buffered network interface
   */
  class recvBufferData {
      // single producer single consumer
      moodycamel::ConcurrentQueue<recvMessage> messages;
      moodycamel::ProducerToken ptok;

      std::atomic<uint32_t> frontTag;
      recvMessage frontMsg;

  public:
      std::atomic<size_t> inflightRecvs = 0;

      recvBufferData() : ptok(messages), frontTag(~0U) {}

      std::optional<RecvBuffer> tryPopMsg(uint32_t tag);

      void add(uint32_t tag, vTy&& vec);
      
      bool hasMsg(uint32_t tag);
  }; // end recv buffer class

  std::vector<recvBufferData> recvData;
  
  /**
   * Receive buffers for the buffered network interface
   */
  class recvBufferCommunication {
      // single producer single consumer
      moodycamel::ConcurrentQueue<std::pair<uint32_t, uint8_t*>> messages;
      moodycamel::ProducerToken ptok;

  public:
      std::atomic<size_t> inflightRecvs = 0;

      recvBufferCommunication() : ptok(messages) {}

      bool tryPopMsg(uint32_t& host, uint8_t*& work);
      
      void add(uint32_t host, uint8_t* work);
  }; // end recv buffer class

  recvBufferCommunication recvCommunication;

  /**
   * Receive buffers for the buffered network interface
   */
  class recvBufferRemoteWork {
      // single producer single consumer
      moodycamel::ConcurrentQueue<std::pair<uint8_t*, size_t>> messages;
      moodycamel::ProducerToken ptok;

  public:
      std::atomic<size_t> inflightRecvs = 0;

      recvBufferRemoteWork() : ptok(messages) {}

      bool tryPopMsg(uint8_t*& work, size_t& workLen);

      void add(uint8_t* work, size_t workLen);
  }; // end recv buffer class

  recvBufferRemoteWork recvRemoteWork;

  struct sendMessage {
      uint32_t tag;  //!< tag on message indicating distinct communication phases
      vTy data;      //!< data portion of message
      uint8_t* buf;
      size_t bufLen;

      //! Default constructor initializes host and tag to large numbers.
      sendMessage() : tag(~0), buf(nullptr), bufLen(0) {}
      //! @param t Tag to associate with message
      //! @param d Data to save in message
      sendMessage(uint32_t t) : tag(t), buf(nullptr), bufLen(0) {}
      sendMessage(uint32_t t, vTy&& d) : tag(t), data(std::move(d)), buf(nullptr), bufLen(0) {}
      sendMessage(uint32_t t, uint8_t* b, size_t len) : tag(t), buf(b), bufLen(len) {}
  };

  /**
   * Single producer single consumer with multiple tags
   */
  class sendBufferData {
      moodycamel::ConcurrentQueue<sendMessage> messages;
      moodycamel::ProducerToken ptok;

      std::atomic<size_t> flush;

  public:
      std::atomic<size_t> inflightSends = 0;
      
      sendBufferData() : ptok(messages), flush(0) {}
      
      void setFlush() {}
    
      bool checkFlush() {
          return flush > 0;
      }
    
      sendMessage pop();

      void push(uint32_t tag, vTy&& b);
  };

  std::vector<sendBufferData> sendData;
  
  /**
   * Single producer single consumer with single tag
   */
  class sendBufferCommunication {
      moodycamel::ConcurrentQueue<std::pair<uint8_t*, size_t>> messages;
      moodycamel::ProducerToken ptok;

      std::atomic<size_t> flush;

  public:
      std::atomic<size_t> inflightSends = 0;
      
      sendBufferCommunication() : ptok(messages), flush(0) {}
      
      void setFlush() {}
    
      bool checkFlush() {
          return flush > 0;
      }
    
      bool pop(uint8_t*& work, size_t& workLen);

      void push(uint8_t* work, size_t workLen);
  };

  std::vector<sendBufferCommunication> sendCommunication;

  /**   
   * single producer single consumer with single tag
   */
  class sendBufferRemoteWork {
      NetworkInterfaceBuffered* net;
      unsigned tid;

      moodycamel::ConcurrentQueue<std::pair<uint8_t*, size_t>> messages;
      moodycamel::ProducerToken ptok;

      uint8_t* buf;
      size_t bufLen;
      uint32_t msgCount;
      
      std::atomic<size_t> flush;

  public:
      std::atomic<size_t> inflightSends = 0;

      sendBufferRemoteWork() : net(nullptr), tid(0), ptok(messages), buf(nullptr), bufLen(0), msgCount(0), flush(0) {}

      void setNet(NetworkInterfaceBuffered* _net);
      
      void setTID(unsigned _tid) {
          tid = _tid;
      }
      
      void setFlush();
    
      bool checkFlush() {
          return flush > 0;
      }
    
      bool pop(uint8_t*& work, size_t& workLen);

      void add(uint32_t* lid, void* val, size_t valLen);
  };
  
  std::vector<std::vector<sendBufferRemoteWork>> sendRemoteWork;

  /**
   * Message type to send/recv in this network IO layer.
   */
  struct mpiMessage {
      uint32_t host;
      uint32_t tag;
      vTy data;
      uint8_t* buf;
      size_t bufLen;
      MPI_Request req;
        
      mpiMessage(uint32_t host, uint32_t tag, vTy&& data) : host(host), tag(tag), data(std::move(data)), buf(nullptr), bufLen(0) {}
      mpiMessage(uint32_t host, uint32_t tag, size_t len) : host(host), tag(tag), data(len), buf(nullptr), bufLen(0) {}
      mpiMessage(uint32_t host, uint32_t tag, uint8_t* b, size_t len) : host(host), tag(tag), buf(b), bufLen(len) {}
  };

  
  std::vector<std::deque<mpiMessage>> sendInflight;
    
  void sendComplete();

  void send(unsigned tid, uint32_t dest, sendMessage m);
  
  std::deque<mpiMessage> recvInflight;
  
  void recvProbe();
  
  void workerThread();
  
  std::thread worker;
  std::atomic<int> ready;
  
  std::atomic<size_t> inflightWorkTermination;
  std::vector<std::atomic<bool>> sendWorkTermination;
  std::vector<std::atomic<uint32_t>> hostWorkTermination;
  
  std::atomic<size_t> inflightDataTermination;
  std::vector<std::atomic<bool>> sendDataTermination;
  std::vector<std::atomic<uint32_t>> hostDataTermination;
  
public:
  void resetWorkTermination();

  bool checkWorkTermination();
  
  void resetDataTermination() override;

  bool checkDataTermination();

  NetworkInterfaceBuffered();
  
  ~NetworkInterfaceBuffered() override;

  void allocateRecvCommBuffer(size_t alloc_size);

  void deallocateRecvBuffer(uint8_t* buf);

  void sendTagged(uint32_t dest, uint32_t tag, SendBuffer& buf, int phase = 0) override;
  
  void sendWork(unsigned tid, uint32_t dest, uint32_t* lid, void* val, size_t valLen);
  
  void sendComm(uint32_t dest, uint8_t* bufPtr, size_t len);

  std::optional<std::pair<uint32_t, RecvBuffer>>
  receiveTagged(uint32_t tag, int phase = 0) override;
  
  std::optional<std::pair<uint32_t, RecvBuffer>>
  receiveTagged(bool& terminateFlag, uint32_t tag, int phase = 0) override;
  
  std::optional<std::pair<uint32_t, RecvBuffer>>
  receiveTaggedFromHost(uint32_t host, bool& terminateFlag, uint32_t tag, int phase = 0) override;
  
  bool receiveRemoteWork(uint8_t*& work, size_t& workLen);

  bool receiveRemoteWork(bool& terminateFlag, uint8_t*& work, size_t& workLen);
  
  bool receiveComm(uint32_t& host, uint8_t*& work);
  
  void flush() override;
  
  void flushData() override;
  
  void flushRemoteWork();
  
  void flushComm();

  void broadcastWorkTermination();
  
  void signalDataTermination(uint32_t dest) override;

  bool anyPendingSends() override;

  bool anyPendingReceives() override;
};

/**
 * Create a buffered network interface, or return one if already
 * created.
 */
NetworkInterfaceBuffered& makeNetworkBuffered();

NetworkInterfaceBuffered& getSystemNetworkInterfaceBuffered();

} // namespace
