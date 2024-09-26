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
#include "galois/runtime/NetworkIO.h"
#include "galois/runtime/Tracer.h"
#include "galois/Threads.h"

#include <thread>
#include <mutex>
#include <iostream>
#include <limits>

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

  static const int COMM_MIN =
      1400; //! bytes (sligtly smaller than an ethernet packet)
  static const int COMM_DELAY = 100; //! microseconds delay

  unsigned long statSendNum;
  unsigned long statSendBytes;
  unsigned long statSendEnqueued;
  unsigned long statRecvNum;
  unsigned long statRecvBytes;
  unsigned long statRecvDequeued;
  bool anyReceivedMessages;

  unsigned int numT;

  // using vTy = std::vector<uint8_t>;
  using vTy = galois::PODResizeableArray<uint8_t>;

  /**
   * Receive buffers for the buffered network interface
   */
  class recvBuffer {
    std::deque<NetworkIO::message> data;
    SimpleLock rlock;
    // tag of head of queue
    std::atomic<uint32_t> dataPresent;

  public:
    std::optional<RecvBuffer> popMsg(uint32_t tag, std::atomic<size_t>& inflightRecvs) {
      if (data.empty() || data.front().tag != tag)
        return std::optional<RecvBuffer>();

      vTy vec(std::move(data.front().data));

      data.pop_front();
      --inflightRecvs;
      if (!data.empty()) {
        dataPresent = data.front().tag;
      } else {
        dataPresent = ~0;
      }

      return std::optional<RecvBuffer>(RecvBuffer(std::move(vec), sizeof(uint32_t)));
    }

    // Worker thread interface
    void add(NetworkIO::message m) {
      std::lock_guard<SimpleLock> lg(rlock);
      if (data.empty()) {
        galois::runtime::trace("ADD LATEST ", m.tag);
        dataPresent = m.tag;
      }

      // std::cerr << m.data.size() << " " <<
      //              std::count(m.data.begin(), m.data.end(), 0) << "\n";
      // for (auto x : m.data) {
      //   std::cerr << (int) x << " ";
      // }
      // std::cerr << "\n";
      // std::cerr << "A " << m.host << " " << m.tag << " " << m.data.size() <<
      // "\n";

      data.push_back(std::move(m));

      assert(data.back().data.size() !=
             (unsigned int)std::count(data.back().data.begin(),
                                      data.back().data.end(), 0));
    }

    bool hasData(uint32_t tag) { return dataPresent == tag; }

    size_t size() { return data.size(); }

    uint32_t getPresentTag() { return dataPresent; }

    bool try_lock() { return rlock.try_lock();}
    
    void unlock() { return rlock.unlock();}
  }; // end recv buffer class

  std::vector<std::vector<recvBuffer>> recvData;

  /**
   * Send buffers for the buffered network interface
   */
  class sendBuffer {
    struct msg {
      uint32_t tag;
      vTy data;
      msg(uint32_t t, vTy& _data) : tag(t), data(std::move(_data)) {}
    };

    std::deque<msg> messages;
    std::atomic<size_t> numBytes;
    std::atomic<unsigned> urgent;
    //! @todo FIXME track time since some epoch in an atomic.
    std::chrono::high_resolution_clock::time_point time;
    SimpleLock slock, timelock;

  public:
    unsigned long statSendTimeout;
    unsigned long statSendOverflow;
    unsigned long statSendUrgent;

    size_t size() { return messages.size(); }
    
    void subtractNumBytes(size_t num) {
        numBytes -= num;
    }

    void markUrgent() {
      if (numBytes) {
        std::lock_guard<SimpleLock> lg(slock);
        urgent = messages.size();
      }
    }

    bool isUrgent() {
        return (urgent > 0);
    }

    void decrementUrgent() {
        --urgent;
    }

    bool ready() {
      if (numBytes == 0)
        return false;
      if (urgent) {
        ++statSendUrgent;
        return true;
      }
      if (numBytes > COMM_MIN) {
        ++statSendOverflow;
        return true;
      }
      auto n = std::chrono::high_resolution_clock::now();
      decltype(n) mytime;
      {
        std::lock_guard<SimpleLock> lg(timelock);
        mytime = time;
      }
      auto elapsed =
          std::chrono::duration_cast<std::chrono::microseconds>(n - mytime);
      if (elapsed.count() > COMM_DELAY) {
        ++statSendTimeout;
        return true;
      }
      return false;
    }

    void lock() {
        slock.lock();
    }

    void unlock() {
        slock.unlock();
    }

    std::deque<msg>& getMessages() {
        return messages;
    }

    void add(uint32_t tag, vTy& b) {
      std::lock_guard<SimpleLock> lg(slock);
      if (messages.empty()) {
        std::lock_guard<SimpleLock> lg(timelock);
        time = std::chrono::high_resolution_clock::now();
      }
      unsigned oldNumBytes = numBytes;
      numBytes += b.size();
      galois::runtime::trace("BufferedAdd", oldNumBytes, numBytes, tag,
                             galois::runtime::printVec(b));
      messages.emplace_back(tag, b);
    }
  }; // end send buffer class

  std::vector<std::vector<sendBuffer>> sendData;
    
  std::pair<uint32_t, vTy>
  assemble(unsigned dst, unsigned start_tid, std::atomic<size_t>& GALOIS_UNUSED(inflightSends)) {
      uint32_t tag = ~0;
      bool tag_set = false;
      
      vTy vec;
      uint32_t len = 0;
      int num      = 0;
      bool skip = false;
      
      for (unsigned t=0; t<numT; t++) {
          if (skip) {
              break;
          }

          unsigned tid = (start_tid + t) % numT;
          
          auto& sendBuf = sendData[tid][dst];
          if (sendBuf.ready()) {
              sendBuf.lock();
              
              // set tag
              auto& messages = sendBuf.getMessages();
              if (messages.empty()) {
                  continue;
              }
              else {
                  if (!tag_set) {
                      tag = messages.front().tag;
                      tag_set = true;
                  }
              }

              // compute message size
              for (auto& m : messages) {
                  if (m.tag != tag) {
                      break;
                  } else {
                      // do not let it go over the integer limit because MPI_Isend cannot
                      // deal with it
                      if ((m.data.size() + sizeof(uint32_t) + len + num) > static_cast<size_t>(std::numeric_limits<int>::max())) {
                          skip = true;
                          break;
                      }

                      len += m.data.size();
                      num += sizeof(uint32_t);
                  }
              }
              sendBuf.unlock();
              
              // construct message
              vec.reserve(len + num);
              
              // go out of our way to avoid locking out senders when making messages
              sendBuf.lock();
              while (vec.size() < len) {
                auto& m = messages.front();
                sendBuf.unlock();

                union {
                  uint32_t a;
                  uint8_t b[sizeof(uint32_t)];
                } foo;
                foo.a = m.data.size();
                vec.insert(vec.end(), &foo.b[0], &foo.b[sizeof(uint32_t)]);
                vec.insert(vec.end(), m.data.begin(), m.data.end());
                if (sendBuf.isUrgent())
                  sendBuf.decrementUrgent();
                
                sendBuf.lock();
                sendBuf.subtractNumBytes(m.data.size());
                messages.pop_front();
                --inflightSends;
              };
              sendBuf.unlock();
          }
      }
      
      return std::make_pair(tag, std::move(vec));
  }

  uint32_t getSubMessageLen(vTy& data_array, size_t offset) {
      if ((data_array.size() - offset) > sizeof(uint32_t)) {
          union {
              uint8_t a[sizeof(uint32_t)];
              uint32_t b;
          } c;

          for (size_t i=0; i<sizeof(uint32_t); i++) {
              c.a[i] = data_array[offset + i];
          }

          return c.b;
      } else {
          return ~0;
      }
  }

  std::optional<vTy> getSubMessage(vTy& data_array, size_t& offset, uint32_t len) {
      if ((data_array.size() - offset) > len) {
          vTy vec;

          vec.insert(vec.end(), data_array.begin() + offset, data_array.begin() + offset + sizeof(uint32_t));
          offset += sizeof(uint32_t);
          vec.insert(vec.end(), data_array.begin() + offset, data_array.begin() + offset + len);
          offset += len;

          return std::optional<vTy>(std::move(vec));
      } else {
          return std::optional<vTy>();
      }
  }

  void workerThread() {
    initializeMPI();
    int rank;
    int hostSize;

    int rankSuccess = MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if (rankSuccess != MPI_SUCCESS) {
      MPI_Abort(MPI_COMM_WORLD, rankSuccess);
    }

    int sizeSuccess = MPI_Comm_size(MPI_COMM_WORLD, &hostSize);
    if (sizeSuccess != MPI_SUCCESS) {
      MPI_Abort(MPI_COMM_WORLD, sizeSuccess);
    }

    galois::gDebug("[", NetworkInterface::ID, "] MPI initialized");
    std::tie(netio, ID, Num) =
        makeNetworkIOMPI(memUsageTracker, inflightSends, inflightRecvs);

    assert(ID == (unsigned)rank);
    assert(Num == (unsigned)hostSize);

    ready = 1;
    while (ready < 2) { /*fprintf(stderr, "[WaitOnReady-2]");*/
    };
    unsigned int sendThreadNum = 0;
    unsigned int recvThreadNum = 0;
    while (ready != 3) {
      for (unsigned i = 0; i < Num; ++i) {
        netio->progress();

        // handle send queue i
        NetworkIO::message msg;
        msg.host = i;
        std::tie(msg.tag, msg.data) = assemble(i, sendThreadNum, inflightSends);

        if (msg.tag != ~0U && msg.data.size() != 0) {
          ++inflightSends;
          galois::runtime::trace("BufferedSending", msg.host, msg.tag, galois::runtime::printVec(msg.data));
          ++statSendEnqueued;
          netio->enqueue(std::move(msg));
        }

        sendThreadNum = (sendThreadNum + 1) % numT;
        
        // handle receive
        NetworkIO::message rdata = netio->dequeue();
        if (rdata.data.size()) {
          ++statRecvDequeued;
          assert(rdata.data.size() !=
                 (unsigned int)std::count(rdata.data.begin(), rdata.data.end(),
                                          0));
          galois::runtime::trace("BufferedRecieving", rdata.host, rdata.tag,
                                 galois::runtime::printVec(rdata.data));

          // Disassemble the aggregated message
          --inflightRecvs;
          size_t offset = 0;
          while (offset < rdata.data.size()) {
              // Read the length of the next message
              uint32_t len = getSubMessageLen(rdata.data, offset);
              if (len == ~0U || len == 0) {
                  galois::gError("Cannot read the length of the received message!\n");
                  break; // Not enough data to read the message size
              }

              NetworkIO::message sub_msg;
              sub_msg.host = rdata.host;
              sub_msg.tag = rdata.tag;
              
              // Read the message data
              auto vec = getSubMessage(rdata.data, offset, len);
              if (vec.has_value()) {
                  sub_msg.data = std::move(vec.value());
              }
              else {
                  galois::gError("Cannot read the entire received message (length mismatch)!\n");
                  break; // Not enough data for the complete message
              }

              // Add the disassembled message to the receive buffer
              ++inflightRecvs;
              recvData[recvThreadNum][rdata.host].add(std::move(sub_msg));

              recvThreadNum = (recvThreadNum + 1) % numT;
          }
        }
      }
    }
    finalizeMPI();
  }
  
  std::thread worker;
  std::atomic<int> ready;

public:
  NetworkInterfaceBuffered() {
    inflightSends       = 0;
    inflightRecvs       = 0;
    ready               = 0;
    anyReceivedMessages = false;
    worker = std::thread(&NetworkInterfaceBuffered::workerThread, this);
    galois::substrate::getThreadPool().addBackgroundThreadNum(1);
    numT = galois::getActiveThreads();
    while (ready != 1) {
    };
    
    recvData.resize(numT);
    for (auto& threadRecvData : recvData) {
        std::vector<recvBuffer> temp(Num);
        threadRecvData = std::move(temp);
    }
    sendData.resize(numT);
    for (auto& threadSendData : sendData) {
        std::vector<sendBuffer> temp(Num);
        threadSendData = std::move(temp);
    }
    ready    = 2;
  }

  virtual ~NetworkInterfaceBuffered() {
    ready = 3;
    worker.join();
    galois::substrate::getThreadPool().subtractBackgroundThreadNum(1);
  }

  std::unique_ptr<galois::runtime::NetworkIO> netio;

  virtual void sendTagged(uint32_t dest, uint32_t tag, SendBuffer& buf,
                          int phase) {
    ++inflightSends;
    tag += phase;
    statSendNum += 1;
    statSendBytes += buf.size();
    galois::runtime::trace("sendTagged", dest, tag,
                           galois::runtime::printVec(buf.getVec()));
    
    unsigned thread_id = galois::substrate::ThreadPool::getTID();
    auto& sd = sendData[thread_id][dest];
    sd.add(tag, buf.getVec());
  }

  virtual std::optional<std::pair<uint32_t, RecvBuffer>>
  receiveTagged(uint32_t tag, int phase) {
      unsigned thread_id = galois::substrate::ThreadPool::getTID();
      tag += phase;
      
      for (unsigned t=0; t<numT; t++) {
          unsigned tid = (thread_id + t) % numT;
          for (unsigned h = 0; h < Num; ++h) {
              auto& rq = recvData[tid][h];
              if (rq.hasData(tag)) {
                  if (rq.try_lock()) {
                      auto buf = rq.popMsg(tag, inflightRecvs);
                      rq.unlock();
                      if (buf) {
                          ++statRecvNum;
                          statRecvBytes += buf->size();
                          memUsageTracker.decrementMemUsage(buf->size());
                          galois::runtime::trace("recvTagged", h, tag, galois::runtime::printVec(buf->getVec()));
                          anyReceivedMessages = true;
                          return std::optional<std::pair<uint32_t, RecvBuffer>>(std::make_pair(h, std::move(*buf)));
                      }
                  }
              }

              galois::runtime::trace("recvTagged BLOCKED this by that", tag, rq.getPresentTag());
          }
      }

      return std::optional<std::pair<uint32_t, RecvBuffer>>();
  }
  
  virtual void flush() {
    for (auto& threadSendData : sendData) {
        for (auto& sd : threadSendData) {
            sd.markUrgent();
        }
    }
  }

  virtual bool anyPendingSends() { return (inflightSends > 0); }

  virtual bool anyPendingReceives() {
    if (anyReceivedMessages) { // might not be acted on by the computation yet
      anyReceivedMessages = false;
      // galois::gDebug("[", ID, "] receive out of buffer \n");
      return true;
    }
    // if (inflightRecvs > 0) {
    // galois::gDebug("[", ID, "] inflight receive: ", inflightRecvs, " \n");
    // }
    return (inflightRecvs > 0);
  }

  virtual unsigned long reportSendBytes() const { return statSendBytes; }
  virtual unsigned long reportSendMsgs() const { return statSendNum; }
  virtual unsigned long reportRecvBytes() const { return statRecvBytes; }
  virtual unsigned long reportRecvMsgs() const { return statRecvNum; }

  virtual std::vector<unsigned long> reportExtra() const {
    std::vector<unsigned long> retval(5);
    for (auto& threadSendData : sendData) {
        for (auto& sd : threadSendData) {
            retval[0] += sd.statSendTimeout;
            retval[1] += sd.statSendOverflow;
            retval[2] += sd.statSendUrgent;
        }
    }
    retval[3] = statSendEnqueued;
    retval[4] = statRecvDequeued;
    return retval;
  }

  virtual std::vector<std::pair<std::string, unsigned long>>
  reportExtraNamed() const {
    std::vector<std::pair<std::string, unsigned long>> retval(5);
    retval[0].first = "SendTimeout";
    retval[1].first = "SendOverflow";
    retval[2].first = "SendUrgent";
    retval[3].first = "SendEnqueued";
    retval[4].first = "RecvDequeued";
    for (auto& threadSendData : sendData) {
        for (auto& sd : threadSendData) {
            retval[0].second += sd.statSendTimeout;
            retval[1].second += sd.statSendOverflow;
            retval[2].second += sd.statSendUrgent;
        }
    }
    retval[3].second = statSendEnqueued;
    retval[4].second = statRecvDequeued;
    return retval;
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
