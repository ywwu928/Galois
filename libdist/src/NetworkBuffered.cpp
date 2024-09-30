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
#include <unordered_map>
#include <tuple>

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

  static const int COMM_MIN = 1400; //! bytes (sligtly smaller than an ethernet packet)

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
    std::atomic<uint32_t> dataPresent = ~0;

  public:
    std::optional<RecvBuffer> popMsg(uint32_t tag, std::atomic<size_t>& inflightRecvs, uint32_t& src) {
      if (data.empty() || data.front().tag != tag)
        return std::optional<RecvBuffer>();

      src = data.front().host;
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

  std::vector<recvBuffer> recvData;

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
    std::atomic<bool> flush = false;
    std::atomic<size_t> numBytes = 0;
    SimpleLock slock;

  public:
    size_t size() { return messages.size(); }

    void setFlush() {
        flush = true;
    }
    
    bool checkFlush() {
        return flush;
    }
    
    void assemble(std::vector<std::pair<uint32_t, vTy>>& payloads, std::atomic<size_t>& GALOIS_UNUSED(inflightSends)) {
        std::unordered_map<uint32_t, std::tuple<uint32_t, int, vTy>> tagMap;

        while (!messages.empty()) {
            slock.lock();
            auto& m = messages.front();
            slock.unlock();
            
            union {
                uint32_t a;
                uint8_t b[sizeof(uint32_t)];
            } foo;

            if (tagMap.find(m.tag) != tagMap.end()) {
                uint32_t& len = std::get<0>(tagMap[m.tag]);
                int& num = std::get<1>(tagMap[m.tag]);
                vTy& vec = std::get<2>(tagMap[m.tag]);
                // do not let it go over the integer limit because MPI_Isend cannot deal with it
                if ((len + num + m.data.size() + sizeof(uint32_t)) > static_cast<size_t>(std::numeric_limits<int>::max())) {
                    // first put onto payloads
                    payloads.push_back(std::make_pair(m.tag, std::move(vec)));
                    ++inflightSends;
                    // then reset values
                    len = m.data.size();
                    num = sizeof(uint32_t);
                    vTy vec_temp;
                    
                    foo.a = m.data.size();
                    vec_temp.insert(vec_temp.end(), &foo.b[0], &foo.b[sizeof(uint32_t)]);
                    vec_temp.insert(vec_temp.end(), m.data.begin(), m.data.end());

                    vec = std::move(vec_temp);
                }
                else {
                    len += m.data.size();
                    num += sizeof(uint32_t);

                    foo.a = m.data.size();
                    vec.insert(vec.end(), &foo.b[0], &foo.b[sizeof(uint32_t)]);
                    vec.insert(vec.end(), m.data.begin(), m.data.end());
                }
            } else {
                uint32_t len = m.data.size();
                int num = sizeof(uint32_t);
                vTy vec;
                
                foo.a = m.data.size();
                vec.insert(vec.end(), &foo.b[0], &foo.b[sizeof(uint32_t)]);
                vec.insert(vec.end(), m.data.begin(), m.data.end());

                tagMap[m.tag] = std::make_tuple(len, num, std::move(vec));
            }
            
            slock.lock();
            messages.pop_front();
            slock.unlock();
            --inflightSends;
            numBytes -= m.data.size();
        }

        flush = false;

        // push all payloads in the map into the vector
        for (auto it=tagMap.begin(); it!=tagMap.end(); ++it) {
            payloads.push_back(std::make_pair(it->first, std::move(std::get<2>(it->second))));
            ++inflightSends;
        }

        // sort the payloads with respect to tag
        // lower tag value should go first (to make sure remote request get sent out before the termination message)
        std::sort(payloads.begin(), payloads.end(),
                [](const auto& a, const auto& b) {
                    return a.first < b.first;
                }
        );
    }

    void add(uint32_t tag, vTy& b) {
      std::lock_guard<SimpleLock> lg(slock);
      unsigned oldNumBytes = numBytes;
      numBytes += b.size();
      galois::runtime::trace("BufferedAdd", oldNumBytes, numBytes, tag, galois::runtime::printVec(b));
      messages.emplace_back(tag, b);

      if (numBytes >= COMM_MIN) {
          flush = true;
      }
    }
  }; // end send buffer class

  std::vector<sendBuffer> sendData;

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
    unsigned int recvThreadNum = 0;
    while (ready != 3) {
      for (unsigned i = 0; i < Num; ++i) {
          netio->progress();
          
          // handle send queue
          auto& sd = sendData[i];
          if (sd.checkFlush()) {
              std::vector<std::pair<uint32_t, vTy>> payloads;
              sd.assemble(payloads, inflightSends);
              
              for (size_t k=0; k<payloads.size(); k++) {
                  NetworkIO::message msg;
                  msg.host = i;
                  msg.tag = payloads[k].first;
                  msg.data = std::move(payloads[k].second);

                  if (msg.tag != ~0U) {
                      galois::runtime::trace("BufferedSending", msg.host, msg.tag, galois::runtime::printVec(msg.data));
                      ++statSendEnqueued;
                      netio->enqueue(std::move(msg));
                  }
              }
          }
          
          // handle receive
          NetworkIO::message rdata = netio->dequeue();
          if (rdata.data.size()) {
              ++statRecvDequeued;
              assert(rdata.data.size() != (unsigned int)std::count(rdata.data.begin(), rdata.data.end(), 0));
              galois::runtime::trace("BufferedRecieving", rdata.host, rdata.tag, galois::runtime::printVec(rdata.data));

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
                  recvData[recvThreadNum].add(std::move(sub_msg));

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
    
    recvData = decltype(recvData)(numT);
    sendData = decltype(sendData)(Num);
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
    
    auto& sd = sendData[dest];
    sd.add(tag, buf.getVec());
  }

  virtual std::optional<std::pair<uint32_t, RecvBuffer>>
  receiveTagged(uint32_t tag, int phase) {
      unsigned thread_id = galois::substrate::ThreadPool::getTID();
      tag += phase;
      
      for (unsigned t=0; t<numT; t++) {
          unsigned tid = (thread_id + t) % numT;
          auto& rq = recvData[tid];
          if (rq.hasData(tag)) {
              if (rq.try_lock()) {
                  uint32_t src;
                  auto buf = rq.popMsg(tag, inflightRecvs, src);
                  rq.unlock();
                  if (buf) {
                      ++statRecvNum;
                      statRecvBytes += buf->size();
                      memUsageTracker.decrementMemUsage(buf->size());
                      galois::runtime::trace("recvTagged", src, tag, galois::runtime::printVec(buf->getVec()));
                      anyReceivedMessages = true;
                      return std::optional<std::pair<uint32_t, RecvBuffer>>(std::make_pair(src, std::move(*buf)));
                  }
              }
          }

          galois::runtime::trace("recvTagged BLOCKED this by that", tag, rq.getPresentTag());
      }

      return std::optional<std::pair<uint32_t, RecvBuffer>>();
  }
  
  virtual std::optional<std::tuple<bool, uint32_t, RecvBuffer>>
  receiveTaggedCheckTermination(uint32_t tag, uint32_t termination_tag) {
      unsigned thread_id = galois::substrate::ThreadPool::getTID();
      
      for (unsigned t=0; t<numT; t++) {
          unsigned tid = (thread_id + t) % numT;
          auto& rq = recvData[tid];
          if (rq.hasData(tag)) {
              if (rq.try_lock()) {
                  uint32_t src;
                  auto buf = rq.popMsg(tag, inflightRecvs, src);
                  rq.unlock();
                  if (buf) {
                      ++statRecvNum;
                      statRecvBytes += buf->size();
                      memUsageTracker.decrementMemUsage(buf->size());
                      galois::runtime::trace("recvTaggedCheckTermination", src, tag, galois::runtime::printVec(buf->getVec()));
                      anyReceivedMessages = true;
                      return std::optional<std::tuple<bool, uint32_t, RecvBuffer>>(std::make_tuple(false, src, std::move(*buf)));
                  }
              }
          }
          else if (rq.hasData(termination_tag)) {
              if (rq.try_lock()) {
                  uint32_t src;
                  auto buf = rq.popMsg(termination_tag, inflightRecvs, src);
                  rq.unlock();
                  if (buf) {
                      ++statRecvNum;
                      statRecvBytes += buf->size();
                      memUsageTracker.decrementMemUsage(buf->size());
                      galois::runtime::trace("recvTaggedCheckTermination", src, termination_tag, galois::runtime::printVec(buf->getVec()));
                      anyReceivedMessages = true;
                      return std::optional<std::tuple<bool, uint32_t, RecvBuffer>>(std::make_tuple(true, src, std::move(*buf)));
                  }
              }
          }

          galois::runtime::trace("recvTagged BLOCKED this by that", tag, rq.getPresentTag());
      }

      return std::optional<std::tuple<bool, uint32_t, RecvBuffer>>();
  }
  
  virtual std::optional<std::pair<uint32_t, RecvBuffer>>
  receiveTaggedCheckEmpty(uint32_t tag, bool& empty) {
      unsigned thread_id = galois::substrate::ThreadPool::getTID();
      empty = true;
      
      for (unsigned t=0; t<numT; t++) {
          unsigned tid = (thread_id + t) % numT;
          auto& rq = recvData[tid];
          if (rq.hasData(tag)) {
              empty = false;
              if (rq.try_lock()) {
                  uint32_t src;
                  auto buf = rq.popMsg(tag, inflightRecvs, src);
                  rq.unlock();
                  if (buf) {
                      ++statRecvNum;
                      statRecvBytes += buf->size();
                      memUsageTracker.decrementMemUsage(buf->size());
                      galois::runtime::trace("recvTagged", src, tag, galois::runtime::printVec(buf->getVec()));
                      anyReceivedMessages = true;
                      return std::optional<std::pair<uint32_t, RecvBuffer>>(std::make_pair(src, std::move(*buf)));
                  }
              }
          }

          galois::runtime::trace("recvTagged BLOCKED this by that", tag, rq.getPresentTag());
      }

      return std::optional<std::pair<uint32_t, RecvBuffer>>();
  }
  
  virtual void flush() {
    for (auto& sd : sendData) {
        sd.setFlush();
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
    std::vector<unsigned long> retval(2);
    retval[0] = statSendEnqueued;
    retval[1] = statRecvDequeued;
    return retval;
  }

  virtual std::vector<std::pair<std::string, unsigned long>>
  reportExtraNamed() const {
    std::vector<std::pair<std::string, unsigned long>> retval(2);
    retval[0].first = "SendEnqueued";
    retval[1].first = "RecvDequeued";
    retval[0].second = statSendEnqueued;
    retval[1].second = statRecvDequeued;
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
