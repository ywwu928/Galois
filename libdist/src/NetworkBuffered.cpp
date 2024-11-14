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
#include "galois/runtime/Tracer.h"
#include "galois/runtime/Mem.h"
#include "galois/runtime/Mem.h"
#include "galois/Threads.h"
#include "galois/concurrentqueue.h"

#include <thread>
#include <mutex>
#include <iostream>
#include <limits>
#include <functional>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

using namespace galois::runtime;
using namespace galois::substrate;

namespace {

/**
 * @class NetworkInterfaceBuffered
 *
 * Buffered network interface: messages are buffered before they are sent out.
 * A single worker thread is initialized to send/receive messages from/to
 * buffers.
 */
class NetworkInterfaceBuffered : public NetworkInterface {
  using NetworkInterface::ID;
  using NetworkInterface::Num;

#ifdef GALOIS_NO_MIRRORING
  static const size_t AGG_MSG_SIZE = 2 << 17;
  static const size_t SEND_BUF_COUNT = 2 << 12;
  static const size_t RECV_BUF_COUNT = 2 << 14;
#else
  static const size_t AGG_MSG_SIZE = 2 << 14;
  static const size_t SEND_BUF_COUNT = 2 << 10;
  static const size_t RECV_BUF_COUNT = 2 << 15;
#endif

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

      std::optional<RecvBuffer> tryPopMsg(uint32_t tag) {
          if (frontTag == ~0U) { // no messages available
              return std::optional<RecvBuffer>();
          }
          else {
              if (frontTag != tag) {
                  return std::optional<RecvBuffer>();
              }
              else {
                  frontTag = ~0U;
                  
                  --inflightRecvs;
                  return std::optional<RecvBuffer>(RecvBuffer(std::move(frontMsg.data)));
              }
          }
      }

      // Worker thread interface
      void add(uint32_t tag, vTy&& vec) {
          messages.enqueue(ptok, recvMessage(tag, std::move(vec)));
      }
      
      bool hasMsg(uint32_t tag) {
          if (frontTag == ~0U) {
              if (messages.size_approx() != 0) {
                  bool success = messages.try_dequeue_from_producer(ptok, frontMsg);
                  if (success) {
                      frontTag = frontMsg.tag;
                  }
              }
          }
          
          return frontTag == tag;
      }
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

      bool tryPopMsg(uint32_t& host, uint8_t*& work) {
          std::pair<uint32_t, uint8_t*> message;
          bool success = messages.try_dequeue_from_producer(ptok, message);
          if (success) {
              --inflightRecvs;
              host = message.first;
              work = message.second;
          }

          return success;
      }

      // Worker thread interface
      void add(uint32_t host, uint8_t* work) {
          messages.enqueue(ptok, std::make_pair(host, work));
      }
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

      bool tryPopMsg(uint8_t*& work, size_t& workLen) {
          std::pair<uint8_t*, size_t> message;
          bool success = messages.try_dequeue_from_producer(ptok, message);
          if (success) {
              --inflightRecvs;
              work = message.first;
              workLen = message.second;
          }

          return success;
      }

      // Worker thread interface
      void add(uint8_t* work, size_t workLen) {
          messages.enqueue(ptok, std::make_pair(work, workLen));
      }
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
      
      void setFlush() {
          flush += 1;
      }
    
      bool checkFlush() {
          return flush > 0;
      }
    
      sendMessage pop() {
          sendMessage m;
          bool success = messages.try_dequeue_from_producer(ptok, m);
          if (success) {
              flush -= 1;
              return m;
          }
          else {
              return sendMessage(~0U);
          }
      }

      void push(uint32_t tag, vTy&& b) {
          messages.enqueue(ptok, sendMessage(tag, std::move(b)));
          ++inflightSends;
          flush += 1;
      }
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
      
      void setFlush() {
          flush += 1;
      }
    
      bool checkFlush() {
          return flush > 0;
      }
    
      bool pop(uint8_t*& work, size_t& workLen) {
          std::pair<uint8_t*, size_t> message;
          bool success = messages.try_dequeue_from_producer(ptok, message);
          if (success) {
              flush -= 1;
              work = message.first;
              workLen = message.second;
          }

          return success;
      }

      void push(uint8_t* work, size_t workLen) {
          messages.enqueue(ptok, std::make_pair(work, workLen));
          ++inflightSends;
          flush += 1;
      }
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

      void setNet(NetworkInterfaceBuffered* _net) {
          net = _net;
      }
      
      void setTID(unsigned _tid) {
          tid = _tid;
      }
      
      void setFlush() {
          if (msgCount != 0) {
            // put number of message count at the very last
            std::memcpy(buf + bufLen, &msgCount, sizeof(uint32_t));
            bufLen += sizeof(uint32_t);
            messages.enqueue(ptok, std::make_pair(buf, bufLen));
            
            // allocate new buffer
            do {
                buf = net->sendAllocators[tid].allocate();
                
                if (buf == nullptr) {
                    galois::substrate::asmPause();
                }
            } while (buf == nullptr);
            bufLen = 0;
            msgCount = 0;
            
            ++inflightSends;
            flush += 1;
          }
      }
    
      bool checkFlush() {
          return flush > 0;
      }
    
      bool pop(uint8_t*& work, size_t& workLen) {
          std::pair<uint8_t*, size_t> message;
          bool success = messages.try_dequeue_from_producer(ptok, message);
          if (success) {
              flush -= 1;
              work = message.first;
              workLen = message.second;
          }

          return success;
      }

      void add(uint8_t* work, size_t workLen) {
          if (buf == nullptr) {
              // allocate new buffer
              do {
                  buf = net->sendAllocators[tid].allocate();
                  
                  if (buf == nullptr) {
                      galois::substrate::asmPause();
                  }
              } while (buf == nullptr);
          }
          
          if (bufLen + workLen + sizeof(uint32_t) > AGG_MSG_SIZE) {
              // put number of message count at the very last
              std::memcpy(buf + bufLen, &msgCount, sizeof(uint32_t));
              bufLen += sizeof(uint32_t);
              messages.enqueue(ptok, std::make_pair(buf, bufLen));

              // allocate new buffer
              do {
                  buf = net->sendAllocators[tid].allocate();
                  
                  if (buf == nullptr) {
                      galois::substrate::asmPause();
                  }
              } while (buf == nullptr);
              std::memcpy(buf, work, workLen);
              bufLen = workLen;
              msgCount = 1;

              ++inflightSends;
              flush += 1;
          }
          else {
              // aggregate message
              std::memcpy(buf + bufLen, work, workLen);
              bufLen += workLen;
              msgCount += 1;
          }
      }
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
    
  void sendComplete() {
      for (unsigned t=0; t<numT; t++) {
          if (!sendInflight[t].empty()) {
              int flag = 0;
              MPI_Status status;
              auto& f = sendInflight[t].front();
              int rv  = MPI_Test(&f.req, &flag, &status);
              handleError(rv);
              if (flag) {
                  if (f.tag == galois::runtime::terminationTag) {
                      --inflightTermination;
                  }
                  else if (f.tag == galois::runtime::remoteWorkTag) {
                    --sendRemoteWork[f.host][t].inflightSends;
                    // return buffer back to pool
                    sendAllocators[t].deallocate(f.buf);
                  }
                  else if (f.tag == galois::runtime::communicationTag) {
                    --sendCommunication[f.host].inflightSends;
                  }
                  else {
                    --sendData[f.host].inflightSends;
                  }

                  sendInflight[t].pop_front();
                  break;
              }
          }
      }
  }

  void send(unsigned tid, uint32_t dest, sendMessage m) {
      if (m.tag == galois::runtime::remoteWorkTag || m.tag == galois::runtime::communicationTag) {
          sendInflight[tid].emplace_back(dest, m.tag, m.buf, m.bufLen);
          auto& f = sendInflight[tid].back();
          int rv = MPI_Isend(f.buf, f.bufLen, MPI_BYTE, f.host, f.tag, MPI_COMM_WORLD, &f.req);
          handleError(rv);
      }
      else if (m.tag != ~0U) {
          sendInflight[tid].emplace_back(dest, m.tag, std::move(m.data));
          auto& f = sendInflight[tid].back();
          int rv = MPI_Isend(f.data.data(), f.data.size(), MPI_BYTE, f.host, f.tag, MPI_COMM_WORLD, &f.req);
          handleError(rv);
      }
  }
  
  std::deque<mpiMessage> recvInflight;
  
    // FIXME: Does synchronous recieves overly halt forward progress?
  void recvProbe() {
      int flag = 0;
      MPI_Status status;
      // check for new messages
      int rv = MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);
      handleError(rv);
      if (flag) {
          int nbytes;
          rv = MPI_Get_count(&status, MPI_BYTE, &nbytes);
          handleError(rv);

          if (status.MPI_TAG == (int)galois::runtime::remoteWorkTag) {
              // allocate new buffer
              uint8_t* buf;
              do {
                  buf = recvAllocator.allocate();
                  
                  if (buf == nullptr) {
                      galois::substrate::asmPause();
                  }
              } while (buf == nullptr);

              recvInflight.emplace_back(status.MPI_SOURCE, status.MPI_TAG, buf, nbytes);
              auto& m = recvInflight.back();
              rv = MPI_Irecv(buf, nbytes, MPI_BYTE, status.MPI_SOURCE, status.MPI_TAG, MPI_COMM_WORLD, &m.req);
              handleError(rv);
          }
          else if (status.MPI_TAG == (int)galois::runtime::communicationTag) {
              recvInflight.emplace_back(status.MPI_SOURCE, status.MPI_TAG, recvCommBuffer[status.MPI_SOURCE], nbytes);
              auto& m = recvInflight.back();
              rv = MPI_Irecv(recvCommBuffer[status.MPI_SOURCE], nbytes, MPI_BYTE, status.MPI_SOURCE, status.MPI_TAG, MPI_COMM_WORLD, &m.req);
              handleError(rv);
          }
          else {
              recvInflight.emplace_back(status.MPI_SOURCE, status.MPI_TAG, nbytes);
              auto& m = recvInflight.back();
              rv = MPI_Irecv(m.data.data(), nbytes, MPI_BYTE, status.MPI_SOURCE, status.MPI_TAG, MPI_COMM_WORLD, &m.req);
              handleError(rv);
          }
      }
  
      // complete messages
      if (!recvInflight.empty()) {
          auto& m  = recvInflight.front();
          int flag = 0;
          rv       = MPI_Test(&m.req, &flag, MPI_STATUS_IGNORE);
          handleError(rv);
          if (flag) {
              if (m.tag == galois::runtime::terminationTag) {
                  hostTermination[m.host] += 1;
              }
              else if (m.tag == galois::runtime::remoteWorkTag) {
                  ++recvRemoteWork.inflightRecvs;
                  recvRemoteWork.add(m.buf, m.bufLen);
              }
              else if (m.tag == galois::runtime::communicationTag) {
                  ++recvCommunication.inflightRecvs;
                  recvCommunication.add(m.host, m.buf);
              }
              else {
                  ++recvData[m.host].inflightRecvs;
                  recvData[m.host].add(m.tag, std::move(m.data));
              }
                
              recvInflight.pop_front();
          }
      }
  }
  
  void workerThread() {

      // Set thread affinity
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);           // Clear the CPU set
      CPU_SET(commCoreID, &cpuset);   // Set the specified core

      // Get the native handle of the std::thread
      pthread_t thread = pthread_self();
    
      // Set the CPU affinity of the thread
      if (pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset) != 0) {
          std::cerr << "Error setting thread affinity" << std::endl;
          return;
      }

      initializeMPI();
      galois::gDebug("[", NetworkInterface::ID, "] MPI initialized");
      ID = getID();
      Num = getNum();

      ready = 1;
      while (ready < 2) { /*fprintf(stderr, "[WaitOnReady-2]");*/
      };
      while (ready != 3) {
          for (unsigned i = 0; i < Num; ++i) {
              if (i != ID) {
                  // handle send queue
                  // 1. remote work
                  bool hostEmpty = true;
                  for (unsigned t=0; t<numT; t++) {
                      // push progress forward on the network IO
                      sendComplete();
                      recvProbe();
          
                      auto& srw = sendRemoteWork[i][t];
                      if (srw.checkFlush()) {
                          hostEmpty = false;
                          
                          uint8_t* work = nullptr;
                          size_t workLen = 0;
                          bool success = srw.pop(work, workLen);
                          
                          if (success) {
                              send(t, i, sendMessage(galois::runtime::remoteWorkTag, work, workLen));
                          }
                      }
                  }

                  if(hostEmpty) { // wait until all works are sent
                      // 2. termination
                      if (sendTermination[i]) {
                          ++inflightTermination;
                          // put it on the last thread to make sure it is sent last after all the work are sent
                          send(numT - 1, i, sendMessage(galois::runtime::terminationTag));
                          sendTermination[i] = false;
                      }
                  }
                  
                  // 3. communication
                  auto& sc = sendCommunication[i];
                  if (sc.checkFlush()) {
                      uint8_t* work = nullptr;
                      size_t workLen = 0;
                      bool success = sc.pop(work, workLen);
                      
                      if (success) {
                          // put it on the first thread
                          send(0, i, sendMessage(galois::runtime::communicationTag, work, workLen));
                      }
                  }
                  
                  // 4. data
                  auto& sd = sendData[i];
                  if (sd.checkFlush()) {
                      sendMessage msg = sd.pop();
                      
                      if (msg.tag != ~0U) {
                          // put it on the first thread
                          send(0, i, std::move(msg));
                      }
                  }
              }
          }
      }
      
      finalizeMPI();
  }
  
  std::thread worker;
  std::atomic<int> ready;
  
  std::atomic<size_t> inflightTermination;
  std::vector<std::atomic<bool>> sendTermination;
  std::vector<std::atomic<uint32_t>> hostTermination;
  virtual void resetTermination() {
      for (unsigned i=0; i<Num; i++) {
          if (i == ID) {
              continue;
          }
          hostTermination[i] -= 1;
      }
  }

  bool checkTermination() {
      for (unsigned i=0; i<Num; i++) {
          if (i == ID) {
              continue;
          }
          if (hostTermination[i] == 0) {
              return false;
          }
      }
      return true;
  }

public:
  NetworkInterfaceBuffered() {
    ready               = 0;
    inflightTermination = 0;
    anyReceivedMessages = false;
    worker = std::thread(&NetworkInterfaceBuffered::workerThread, this);
    numT = galois::getActiveThreads();
    sendAllocators = decltype(sendAllocators)(numT);
    sendInflight = decltype(sendInflight)(numT);
    while (ready != 1) {};
    
    recvData = decltype(recvData)(Num);
    sendData = decltype(sendData)(Num);
    sendCommunication = decltype(sendCommunication)(Num);
    sendRemoteWork.resize(Num);
    for (auto& hostSendRemoteWork : sendRemoteWork) {
        std::vector<sendBufferRemoteWork> temp(numT);
        hostSendRemoteWork = std::move(temp);
    }
    for (unsigned i=0; i<Num; i++) {
        for (unsigned t=0; t<numT; t++) {
            sendRemoteWork[i][t].setNet(this);
            sendRemoteWork[i][t].setTID(t);
        }
    }
    sendTermination = decltype(sendTermination)(Num);
    hostTermination = decltype(hostTermination)(Num);
    for (unsigned i=0; i<Num; i++) {
        sendTermination[i] = false;
        if (i == ID) {
            hostTermination[i] = 1;
        }
        else {
            hostTermination[i] = 0;
        }
    }
    ready    = 2;
  }

  virtual ~NetworkInterfaceBuffered() {
    ready = 3;
    worker.join();
    
    for (unsigned i=0; i<Num; i++) {
        if (recvCommBuffer[i] != nullptr){
            free(recvCommBuffer[i]);
        }
    }
  }

  virtual void allocateRecvCommBuffer(size_t alloc_size) {
      for (unsigned i=0; i<Num; i++) {
          void* ptr = malloc(alloc_size);
          if (ptr == nullptr) {
              galois::gError("Failed to allocate memory for the communication receive work buffer\n");
          }
          recvCommBuffer.push_back(static_cast<uint8_t*>(ptr));
      }
  }

  virtual void deallocateRecvBuffer(uint8_t* buf) {
      recvAllocator.deallocate(buf);
  }

  virtual void sendTagged(uint32_t dest, uint32_t tag, SendBuffer& buf, int phase) {
    tag += phase;

    sendData[dest].push(tag, std::move(buf.getVec()));
  }
  
  virtual void sendWork(unsigned tid, uint32_t dest, uint8_t* bufPtr, size_t len) {
    sendRemoteWork[dest][tid].add(bufPtr, len);
  }
  
  virtual void sendComm(uint32_t dest, uint8_t* bufPtr, size_t len) {
    sendCommunication[dest].push(bufPtr, len);
  }

  virtual std::optional<std::pair<uint32_t, RecvBuffer>>
  receiveTagged(uint32_t tag, int phase) {
      tag += phase;

      for (unsigned h=0; h<Num; h++) {
          if (h == ID) {
              continue;
          }

          auto& rq = recvData[h];
          if (rq.hasMsg(tag)) {
              auto buf = rq.tryPopMsg(tag);
              if (buf.has_value()) {
                  anyReceivedMessages = true;
                  return std::optional<std::pair<uint32_t, RecvBuffer>>(std::make_pair(h, std::move(buf.value())));
              }
          }
      }

      return std::optional<std::pair<uint32_t, RecvBuffer>>();
  }
  
  virtual bool receiveRemoteWork(uint8_t*& work, size_t& workLen) {
      bool success = recvRemoteWork.tryPopMsg(work, workLen);
      return success;
  }

  virtual bool receiveRemoteWork(bool& terminateFlag, uint8_t*& work, size_t& workLen) {
      terminateFlag = false;

      bool success = recvRemoteWork.tryPopMsg(work, workLen);
      if (success) {
          anyReceivedMessages = true;
      }
      else {
          if (checkTermination()) {
              terminateFlag = true;
          }
      }

      return success;
  }
  
  virtual bool receiveComm(uint32_t& host, uint8_t*& work) {
      bool success = recvCommunication.tryPopMsg(host, work);
      if (success) {
          anyReceivedMessages = true;
      }

      return success;
  }
  
  virtual void flush() {
      flushData();
  }
  
  virtual void flushData() {
    for (auto& sd : sendData) {
        sd.setFlush();
    }
  }
  
  virtual void flushRemoteWork() {
    for (auto& hostSendRemoteWork : sendRemoteWork) {
        for (auto& threadSendRemoteWork : hostSendRemoteWork) {
            threadSendRemoteWork.setFlush();
        }
    }
  }
  
  virtual void flushComm() {
    for (auto& sc : sendCommunication) {
        sc.setFlush();
    }
  }

  virtual void broadcastTermination() {
      for (unsigned i=0; i<Num; i++) {
          if (i == ID) {
              continue;
          }
          else {
              sendTermination[i] = true;
          }
      }
  }

  virtual bool anyPendingSends() {
      if (inflightTermination > 0) {
          return true;
      }
      for (unsigned i=0; i<Num; i++) {
          if (sendData[i].inflightSends > 0) {
              return true;
          }
          for (unsigned t=0; t<numT; t++) {
              if (sendRemoteWork[i][t].inflightSends > 0) {
                  return true;
              }
          }
          if (sendCommunication[i].inflightSends > 0) {
              return true;
          }
      }

      return false;
  }

  virtual bool anyPendingReceives() {
    if (anyReceivedMessages) { // might not be acted on by the computation yet
      anyReceivedMessages = false;
      // galois::gDebug("[", ID, "] receive out of buffer \n");
      return true;
    }

    if (recvRemoteWork.inflightRecvs > 0) {
        return true;
    }
    for (unsigned i=0; i<Num; i++) {
        if (recvData[i].inflightRecvs > 0) {
            return true;
        }
    }
    if (recvCommunication.inflightRecvs > 0) {
        return true;
    }

    return false;
  }
};

} // namespace

/**
 * Create a buffered network interface, or return one if already
 * created.
 */
NetworkInterface& galois::runtime::makeNetworkBuffered() {
  static std::atomic<NetworkInterfaceBuffered*> net;
  static substrate::SimpleLock m_mutex;

  // create the interface if it doesn't yet exist in the static variable
  auto* tmp = net.load();
  if (tmp == nullptr) {
    std::lock_guard<substrate::SimpleLock> lock(m_mutex);
    tmp = net.load();
    if (tmp == nullptr) {
      tmp = new NetworkInterfaceBuffered();
      net.store(tmp);
    }
  }

  return *tmp;
}
