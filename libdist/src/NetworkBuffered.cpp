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

#include "galois/runtime/NetworkBuffered.h"

namespace galois::runtime {

std::optional<RecvBuffer> NetworkInterfaceBuffered::recvBufferData::tryPopMsg(uint32_t tag) {
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
void NetworkInterfaceBuffered::recvBufferData::add(uint32_t tag, vTy&& vec) {
    messages.enqueue(ptok, recvMessage(tag, std::move(vec)));
}
      
bool NetworkInterfaceBuffered::recvBufferData::hasMsg(uint32_t tag) {
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

bool NetworkInterfaceBuffered::recvBufferCommunication::tryPopMsg(uint32_t& host, uint8_t*& work) {
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
void NetworkInterfaceBuffered::recvBufferCommunication::add(uint32_t host, uint8_t* work) {
    messages.enqueue(ptok, std::make_pair(host, work));
}

bool NetworkInterfaceBuffered::recvBufferRemoteWork::tryPopMsg(uint8_t*& work, size_t& workLen) {
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
void NetworkInterfaceBuffered::recvBufferRemoteWork::add(uint8_t* work, size_t workLen) {
    messages.enqueue(ptok, std::make_pair(work, workLen));
}

NetworkInterfaceBuffered::sendMessage NetworkInterfaceBuffered::sendBufferData::pop() {
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

void NetworkInterfaceBuffered::sendBufferData::push(uint32_t tag, vTy&& b) {
    messages.enqueue(ptok, sendMessage(tag, std::move(b)));
    ++inflightSends;
    flush += 1;
}
    
bool NetworkInterfaceBuffered::sendBufferCommunication::pop(uint8_t*& work, size_t& workLen) {
    std::pair<uint8_t*, size_t> message;
    bool success = messages.try_dequeue_from_producer(ptok, message);
    if (success) {
        flush -= 1;
        work = message.first;
        workLen = message.second;
    }

    return success;
}

void NetworkInterfaceBuffered::sendBufferCommunication::push(uint8_t* work, size_t workLen) {
    messages.enqueue(ptok, std::make_pair(work, workLen));
    ++inflightSends;
    flush += 1;
}

void NetworkInterfaceBuffered::sendBufferRemoteWork::setNet(NetworkInterfaceBuffered* _net) {
    net = _net;
  
    if (buf == nullptr) {
        // allocate new buffer
        do {
            buf = net->sendAllocators[tid].allocate();
          
            if (buf == nullptr) {
                galois::substrate::asmPause();
            }
        } while (buf == nullptr);
    }
  
}

void NetworkInterfaceBuffered::sendBufferRemoteWork::setFlush() {
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

bool NetworkInterfaceBuffered::sendBufferRemoteWork::pop(uint8_t*& work, size_t& workLen) {
    std::pair<uint8_t*, size_t> message;
    bool success = messages.try_dequeue_from_producer(ptok, message);
    if (success) {
        flush -= 1;
        work = message.first;
        workLen = message.second;
    }

    return success;
}

void NetworkInterfaceBuffered::sendBufferRemoteWork::add(uint32_t* lid, void* val, size_t valLen) {
    size_t workLen = sizeof(uint32_t) + valLen;
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
        std::memcpy(buf, lid, sizeof(uint32_t));
        bufLen = sizeof(uint32_t);
        std::memcpy(buf + bufLen, val, valLen);
        bufLen += valLen;
        msgCount = 1;

        ++inflightSends;
        flush += 1;
    }
    else {
        // aggregate message
        std::memcpy(buf + bufLen, lid, sizeof(uint32_t));
        bufLen += sizeof(uint32_t);
        std::memcpy(buf + bufLen, val, valLen);
        bufLen += valLen;
        msgCount += 1;
    }
}
    
void NetworkInterfaceBuffered::sendComplete() {
    for (unsigned t=0; t<numT; t++) {
        if (!sendInflight[t].empty()) {
            int flag = 0;
            MPI_Status status;
            auto& f = sendInflight[t].front();
            int rv  = MPI_Test(&f.req, &flag, &status);
            handleError(rv);
            if (flag) {
                if (f.tag == galois::runtime::workTerminationTag) {
                    --inflightWorkTermination;
                }
                else if (f.tag == galois::runtime::remoteWorkTag) {
                    --sendRemoteWork[f.host][t].inflightSends;
                    // return buffer back to pool
                    sendAllocators[t].deallocate(f.buf);
                }
                else if (f.tag == galois::runtime::communicationTag) {
                    --sendCommunication[f.host].inflightSends;
                }
                else if (f.tag == galois::runtime::dataTerminationTag) {
                    --inflightDataTermination;
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

void NetworkInterfaceBuffered::send(unsigned tid, uint32_t dest, sendMessage m) {
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

// FIXME: Does synchronous recieves overly halt forward progress?
void NetworkInterfaceBuffered::recvProbe() {
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
            if (m.tag == galois::runtime::workTerminationTag) {
                hostWorkTermination[m.host] += 1;
            }
            else if (m.tag == galois::runtime::remoteWorkTag) {
                ++recvRemoteWork.inflightRecvs;
                recvRemoteWork.add(m.buf, m.bufLen);
            }
            else if (m.tag == galois::runtime::communicationTag) {
                ++recvCommunication.inflightRecvs;
                recvCommunication.add(m.host, m.buf);
            }
            else if (m.tag == galois::runtime::dataTerminationTag) {
                hostDataTermination[m.host] += 1;
            }
            else {
                ++recvData[m.host].inflightRecvs;
                recvData[m.host].add(m.tag, std::move(m.data));
            }
            
            recvInflight.pop_front();
        }
    }
}

void NetworkInterfaceBuffered::workerThread() {

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
                bool hostWorkEmpty = true;
                for (unsigned t=0; t<numT; t++) {
                    // push progress forward on the network IO
                    sendComplete();
                    recvProbe();
      
                    auto& srw = sendRemoteWork[i][t];
                    if (srw.checkFlush()) {
                        hostWorkEmpty = false;
                      
                        uint8_t* work = nullptr;
                        size_t workLen = 0;
                        bool success = srw.pop(work, workLen);
                      
                        if (success) {
                            send(t, i, sendMessage(galois::runtime::remoteWorkTag, work, workLen));
                        }
                    }
                }

                if(hostWorkEmpty) { // wait until all works are sent
                    // 2. work termination
                    if (sendWorkTermination[i]) {
                        ++inflightWorkTermination;
                        // put it on the last thread to make sure it is sent last after all the work are sent
                        send(numT - 1, i, sendMessage(galois::runtime::workTerminationTag));
                        sendWorkTermination[i] = false;
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
                bool hostDataEmpty = true;
                auto& sd = sendData[i];
                if (sd.checkFlush()) {
                    hostDataEmpty = false;

                    sendMessage msg = sd.pop();
                  
                    if (msg.tag != ~0U) {
                        // put it on the first thread
                        send(0, i, std::move(msg));
                    }
                }

                if(hostDataEmpty) { // wait until all data are sent
                    // 5. data termination
                    if (sendDataTermination[i]) {
                        ++inflightDataTermination;
                        // put it on the last thread to make sure it is sent last after all the work are sent
                        send(numT - 1, i, sendMessage(galois::runtime::dataTerminationTag));
                        sendDataTermination[i] = false;
                    }
                }
            }
        }
    }
  
    finalizeMPI();
}
  
void NetworkInterfaceBuffered::resetWorkTermination() {
    for (unsigned i=0; i<Num; i++) {
        if (i == ID) {
            continue;
        }
        hostWorkTermination[i] -= 1;
    }
}

bool NetworkInterfaceBuffered::checkWorkTermination() {
    for (unsigned i=0; i<Num; i++) {
        if (i == ID) {
            continue;
        }
        if (hostWorkTermination[i] == 0) {
            return false;
        }
    }
    return true;
}

void NetworkInterfaceBuffered::resetDataTermination() {
    for (unsigned i=0; i<Num; i++) {
        if (i == ID) {
            continue;
        }
        hostDataTermination[i] -= 1;
    }
}

bool NetworkInterfaceBuffered::checkDataTermination() {
    for (unsigned i=0; i<Num; i++) {
        if (i == ID) {
            continue;
        }
        if (hostDataTermination[i] == 0) {
            return false;
        }
    }
    return true;
}

NetworkInterfaceBuffered::NetworkInterfaceBuffered() {
    ready               = 0;
    inflightWorkTermination = 0;
    inflightDataTermination = 0;
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
    sendWorkTermination = decltype(sendWorkTermination)(Num);
    hostWorkTermination = decltype(hostWorkTermination)(Num);
    for (unsigned i=0; i<Num; i++) {
        sendWorkTermination[i] = false;
        if (i == ID) {
            hostWorkTermination[i] = 1;
        }
        else {
            hostWorkTermination[i] = 0;
        }
    }
    sendDataTermination = decltype(sendDataTermination)(Num);
    hostDataTermination = decltype(hostDataTermination)(Num);
    for (unsigned i=0; i<Num; i++) {
        sendDataTermination[i] = false;
        if (i == ID) {
            hostDataTermination[i] = 1;
        }
        else {
            hostDataTermination[i] = 0;
        }
    }
    ready    = 2;
}

NetworkInterfaceBuffered::~NetworkInterfaceBuffered() {
    ready = 3;
    worker.join();

    for (unsigned i=0; i<Num; i++) {
        if (recvCommBuffer[i] != nullptr){
            free(recvCommBuffer[i]);
        }
    }
}

void NetworkInterfaceBuffered::allocateRecvCommBuffer(size_t alloc_size) {
    for (unsigned i=0; i<Num; i++) {
        void* ptr = malloc(alloc_size);
        if (ptr == nullptr) {
            galois::gError("Failed to allocate memory for the communication receive work buffer\n");
        }
        recvCommBuffer.push_back(static_cast<uint8_t*>(ptr));
    }
}

void NetworkInterfaceBuffered::deallocateRecvBuffer(uint8_t* buf) {
    recvAllocator.deallocate(buf);
}

void NetworkInterfaceBuffered::sendTagged(uint32_t dest, uint32_t tag, SendBuffer& buf, int phase) {
    tag += phase;

    sendData[dest].push(tag, std::move(buf.getVec()));
}

void NetworkInterfaceBuffered::sendWork(unsigned tid, uint32_t dest, uint32_t* lid, void* val, size_t valLen) {
    sendRemoteWork[dest][tid].add(lid, val, valLen);
}

void NetworkInterfaceBuffered::sendComm(uint32_t dest, uint8_t* bufPtr, size_t len) {
    sendCommunication[dest].push(bufPtr, len);
}

std::optional<std::pair<uint32_t, RecvBuffer>>
NetworkInterfaceBuffered::receiveTagged(uint32_t tag, int phase) {
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
  
std::optional<std::pair<uint32_t, RecvBuffer>>
NetworkInterfaceBuffered::receiveTagged(bool& terminateFlag, uint32_t tag, int phase) {
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
  
    if (checkDataTermination()) {
        terminateFlag = true;
    }

    return std::optional<std::pair<uint32_t, RecvBuffer>>();
}

std::optional<std::pair<uint32_t, RecvBuffer>>
NetworkInterfaceBuffered::receiveTaggedFromHost(uint32_t host, bool& terminateFlag, uint32_t tag, int phase) {
    tag += phase;

    auto& rq = recvData[host];
    if (rq.hasMsg(tag)) {
        auto buf = rq.tryPopMsg(tag);
        if (buf.has_value()) {
            anyReceivedMessages = true;
            return std::optional<std::pair<uint32_t, RecvBuffer>>(std::make_pair(host, std::move(buf.value())));
        }
    }
  
    if (hostDataTermination[host] > 0) {
        terminateFlag = true;
    }

    return std::optional<std::pair<uint32_t, RecvBuffer>>();
}

bool NetworkInterfaceBuffered::receiveRemoteWork(uint8_t*& work, size_t& workLen) {
    bool success = recvRemoteWork.tryPopMsg(work, workLen);
    return success;
}

bool NetworkInterfaceBuffered::receiveRemoteWork(bool& terminateFlag, uint8_t*& work, size_t& workLen) {
    terminateFlag = false;

    bool success = recvRemoteWork.tryPopMsg(work, workLen);
    if (success) {
        anyReceivedMessages = true;
    }
    else {
        if (checkWorkTermination()) {
            terminateFlag = true;
        }
    }

    return success;
}

bool NetworkInterfaceBuffered::receiveComm(uint32_t& host, uint8_t*& work) {
    bool success = recvCommunication.tryPopMsg(host, work);
    if (success) {
        anyReceivedMessages = true;
    }

    return success;
}

void NetworkInterfaceBuffered::flush() {
    flushData();
}

void NetworkInterfaceBuffered::flushData() {
    for (auto& sd : sendData) {
        sd.setFlush();
    }
}

void NetworkInterfaceBuffered::flushRemoteWork() {
    for (auto& hostSendRemoteWork : sendRemoteWork) {
        for (auto& threadSendRemoteWork : hostSendRemoteWork) {
            threadSendRemoteWork.setFlush();
        }
    }
}

void NetworkInterfaceBuffered::flushComm() {
    for (auto& sc : sendCommunication) {
        sc.setFlush();
    }
}

void NetworkInterfaceBuffered::broadcastWorkTermination() {
    for (unsigned i=0; i<Num; i++) {
        if (i == ID) {
            continue;
        }
        else {
            sendWorkTermination[i] = true;
        }
    }
}

void NetworkInterfaceBuffered::signalDataTermination(uint32_t dest) {
    sendDataTermination[dest] = true;
}

bool NetworkInterfaceBuffered::anyPendingSends() {
    if (inflightWorkTermination > 0) {
        return true;
    }
    if (inflightDataTermination > 0) {
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

bool NetworkInterfaceBuffered::anyPendingReceives() {
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

/**
 * Create a buffered network interface, or return one if already
 * created.
 */
NetworkInterfaceBuffered& makeNetworkBuffered() {
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

NetworkInterfaceBuffered& getSystemNetworkInterfaceBuffered() {
  return makeNetworkBuffered();
}

} // namespace
