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
 * @file Network.cpp
 *
 * Contains implementations for basic NetworkInterface functions and
 * initializations of some NetworkInterface variables.
 */

#include "galois/runtime/Tracer.h"
#include "galois/runtime/Network.h"
#include "galois/runtime/NetworkIO.h"

#include <iostream>
#include <mutex>
#include <chrono>
#include <xmmintrin.h>

namespace cll = llvm::cl;
constexpr uint32_t workSize = 8; // lid (uint32_t) + val (uint32_t or float)
cll::opt<uint32_t> workCountExp("workCountExp",
                                cll::desc("The number of remote work in an aggregated message (exponent with base 2)"),
                                cll::init(12));
cll::opt<uint32_t> sendBufCountExp("sendBufCountExp",
                                   cll::desc("The number of send buffers in the pool"),
                                   cll::init(14));
cll::opt<uint32_t> recvBufCountExp("recvBufCountExp",
                                   cll::desc("The number of receive buffers in the pool"),
                                   cll::init(16));
//uint32_t sendBufCountExp = 26 - workCountExp;
//uint32_t recvBufCountExp = 28 - workCountExp;

namespace galois::runtime {

uint32_t evilPhase = 4; // 0, 1, 2 and 3 is reserved
uint32_t remoteWorkTag = 0; // 0 is reserved for remote work
uint32_t workTerminationTag = 1; // 1 is reserved for remote work termination message
uint32_t communicationTag = 2; // 2 is reserved for communication phase
uint32_t dataTerminationTag = 3; // 3 is reserved for remote data termination message

uint32_t NetworkInterface::ID  = 0;
uint32_t NetworkInterface::Num = 1;

uint32_t getHostID() { return NetworkInterface::ID; }
uint32_t getHostNum() { return NetworkInterface::Num; }

NetworkIO::~NetworkIO() {}

//! Receive broadcasted messages over the network
static void bcastLandingPad(uint32_t src, RecvBuffer& buf);

static void bcastLandingPad(uint32_t src, RecvBuffer& buf) {
  uintptr_t fp;
  gDeserialize(buf, fp);
  auto recv = (void (*)(uint32_t, RecvBuffer&))fp;
  trace("NetworkInterface::bcastLandingPad", (void*)recv);
  recv(src, buf);
}

void NetworkInterface::initializeMPI() {
    int supportProvided;
    int initSuccess =
        MPI_Init_thread(NULL, NULL, MPI_THREAD_MULTIPLE, &supportProvided);
    if (initSuccess != MPI_SUCCESS) {
        MPI_Abort(MPI_COMM_WORLD, initSuccess);
    }

    if (supportProvided != MPI_THREAD_MULTIPLE) {
        GALOIS_DIE("MPI_THREAD_MULTIPLE not supported.");
    }

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_split(MPI_COMM_WORLD, 0, rank, &comm_barrier);
    MPI_Comm_split(MPI_COMM_WORLD, 1, rank, &comm_comm);
}

void NetworkInterface::finalizeMPI() {
    int finalizeSuccess = MPI_Finalize();

    if (finalizeSuccess != MPI_SUCCESS) {
        MPI_Abort(MPI_COMM_WORLD, finalizeSuccess);
    }

    galois::gDebug("[", NetworkInterface::ID, "] MPI finalized");
}

RecvBuffer NetworkInterface::recvBufferData::pop() {
    frontTag = ~0U;

    return RecvBuffer(std::move(frontMsg.data));
}

// Worker thread interface
void NetworkInterface::recvBufferData::add(uint32_t tag, vTy&& vec) {
    messages.enqueue(recvMessage(tag, std::move(vec)));
}
      
bool NetworkInterface::recvBufferData::hasMsg(uint32_t tag) {
    if (frontTag == ~0U) {
        if (messages.size_approx() != 0) {
            bool success = messages.try_dequeue(frontMsg);
            if (success) {
                frontTag = frontMsg.tag;
            }
        }
    }
  
    return frontTag == tag;
}

bool NetworkInterface::recvBufferCommunication::tryPopMsg(uint32_t& host, uint8_t*& work) {
    std::pair<uint32_t, uint8_t*> message;
    bool success = messages.try_dequeue(message);
    if (success) {
        host = message.first;
        work = message.second;
    }

    return success;
}

// Worker thread interface
void NetworkInterface::recvBufferCommunication::add(uint32_t host, uint8_t* work) {
    messages.enqueue(std::make_pair(host, work));
}

bool NetworkInterface::recvBufferRemoteWork::tryPopFullMsg(uint8_t*& work) {
    bool success = fullMessages.try_dequeue_from_producer(ptokFull, work);
    __builtin_prefetch(work, 0, 3);
    return success;
}

bool NetworkInterface::recvBufferRemoteWork::tryPopPartialMsg(uint8_t*& work, size_t& workLen) {
    std::pair<uint8_t*, size_t> message;
    bool success = partialMessages.try_dequeue_from_producer(ptokPartial, message);
    if (success) {
        work = message.first;
        workLen = message.second;
    }
    __builtin_prefetch(work, 0, 3);

    return success;
}

// Worker thread interface
void NetworkInterface::recvBufferRemoteWork::addFull(uint8_t* work) {
    fullMessages.enqueue(ptokFull, work);
}

void NetworkInterface::recvBufferRemoteWork::addPartial(uint8_t* work, size_t workLen) {
    partialMessages.enqueue(ptokPartial, std::make_pair(work, workLen));
}

bool NetworkInterface::sendBufferData::pop(uint32_t& tag, uint8_t*& data, size_t& dataLen) {
    std::tuple<uint32_t, uint8_t*, size_t> message;
    bool success = messages.try_dequeue(message);
    if (success) {
        flush.fetch_sub(1);
        tag = std::get<0>(message);
        data = std::get<1>(message);
        dataLen = std::get<2>(message);
    }

    return success;
}

void NetworkInterface::sendBufferData::push(uint32_t tag, uint8_t* data, size_t dataLen) {
    messages.enqueue(std::make_tuple(tag, data, dataLen));
    flush.fetch_add(1);
}

void NetworkInterface::sendBufferRemoteWork::setNet(NetworkInterface* _net) {
    net = _net;
  
    if (buf == nullptr) {
        // allocate new buffer
        buf = net->sendAllocators[tid].allocate();
        __builtin_prefetch(buf, 1, 3);
    }
  
}

void NetworkInterface::sendBufferRemoteWork::setFlush() {
    if (msgCount != 0) {
        // put number of message count at the very last
        size_t bufLen = msgCount << 3; // 2 * sizeof(uint32_t) * msgCount
        *((uint32_t*)(buf + bufLen)) = msgCount;
        bufLen += sizeof(uint32_t);
        partialMessage = std::make_pair(buf, bufLen);
        flush.fetch_add(1);
        partialFlag = true;
    
        // allocate new buffer
        buf = net->sendAllocators[tid].allocate();
        __builtin_prefetch(buf, 1, 3);
        msgCount = 0;
    }
}

void NetworkInterface::sendBufferRemoteWork::popPartial(uint8_t*& work, size_t& workLen) {
    work = partialMessage.first;
    workLen = partialMessage.second;
    partialFlag = false;
    flush.fetch_sub(1);
}

bool NetworkInterface::sendBufferRemoteWork::pop(uint8_t*& work) {
    bool success = messages.try_dequeue(work);
    if (success) {
        flush.fetch_sub(1);
    }

    return success;
}

template <typename ValTy>
void NetworkInterface::sendBufferRemoteWork::add(uint32_t lid, ValTy val) {
    // aggregate message
    //auto start = std::chrono::high_resolution_clock::now();
    *((uint32_t*)buf + (msgCount << 1)) = lid;
    *((ValTy*)buf + (msgCount << 1) + 1) = val;
    //auto end = std::chrono::high_resolution_clock::now();
    //auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    //if (msgCount == 0)
    //    galois::gPrint("Host ", ID, " : writeBuffer takes ", duration.count(), " ns (msgCount = ", msgCount, ")\n");
    msgCount += 1;

    if (msgCount == net->workCount) {
        messages.enqueue(buf);
        flush.fetch_add(1);

        // allocate new buffer
        buf = net->sendAllocators[tid].allocate();
        __builtin_prefetch(buf, 1, 3);
        msgCount = 0;
    }
}

// explicit instantiation
template void NetworkInterface::sendBufferRemoteWork::add<uint32_t>(uint32_t lid, uint32_t val);
template void NetworkInterface::sendBufferRemoteWork::add<float>(uint32_t lid, float val);
    
void NetworkInterface::sendTrackComplete() {
    for (unsigned t=0; t<numT; t++) {
        if (!sendInflight[t].empty()) {
            int flag = 0;
            MPI_Status status;
            auto& f = sendInflight[t].front();
            int rv  = MPI_Test(&f.req, &flag, &status);
            handleError(rv);
            if (flag) {
                // return buffer back to pool
                sendAllocators[t].deallocate(f.buf);

                sendInflight[t].pop_front();
            }
        }
    }
}

void NetworkInterface::send(uint32_t dest, uint32_t tag, uint8_t* buf, size_t bufLen) {
    __builtin_prefetch(buf, 0, 3);
    MPI_Request req;
    int rv = MPI_Isend(buf, bufLen, MPI_BYTE, dest, tag, comm_comm, &req);
    handleError(rv);
}

void NetworkInterface::sendFullTrack(unsigned tid, uint32_t dest, uint8_t* buf) {
    __builtin_prefetch(buf, 0, 3);
    sendInflight[tid].emplace_back(buf);
    auto& f = sendInflight[tid].back();
    int rv = MPI_Isend(buf, aggMsgSize, MPI_BYTE, dest, remoteWorkTag, comm_comm, &f.req);
    handleError(rv);
}

void NetworkInterface::sendPartialTrack(unsigned tid, uint32_t dest, uint8_t* buf, size_t bufLen) {
    __builtin_prefetch(buf, 0, 3);
    sendInflight[tid].emplace_back(buf);
    auto& f = sendInflight[tid].back();
    int rv = MPI_Isend(buf, bufLen, MPI_BYTE, dest, remoteWorkTag, comm_comm, &f.req);
    handleError(rv);
}

// FIXME: Does synchronous recieves overly halt forward progress?
void NetworkInterface::recvProbe() {
    int flag = 0;
    MPI_Status status;
    // check for new messages
    int rv = MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, comm_comm, &flag, &status);
    handleError(rv);
    if (flag) {
        int nbytes;
        rv = MPI_Get_count(&status, MPI_BYTE, &nbytes);
        handleError(rv);

        if (status.MPI_TAG == (int)remoteWorkTag) {
            // allocate new buffer
            uint8_t* buf;
            buf = recvAllocator.allocate();
            __builtin_prefetch(buf, 1, 3);

            recvInflight.emplace_back(status.MPI_SOURCE, status.MPI_TAG, buf, nbytes);
            auto& m = recvInflight.back();
            rv = MPI_Irecv(buf, nbytes, MPI_BYTE, status.MPI_SOURCE, status.MPI_TAG, comm_comm, &m.req);
            handleError(rv);
        }
        else if (status.MPI_TAG == (int)communicationTag) {
            __builtin_prefetch(recvCommBuffer[status.MPI_SOURCE], 1, 3);
            recvInflight.emplace_back(status.MPI_SOURCE, status.MPI_TAG, recvCommBuffer[status.MPI_SOURCE], nbytes);
            auto& m = recvInflight.back();
            rv = MPI_Irecv(recvCommBuffer[status.MPI_SOURCE], nbytes, MPI_BYTE, status.MPI_SOURCE, status.MPI_TAG, comm_comm, &m.req);
            handleError(rv);
        }
        else {
            recvInflight.emplace_back(status.MPI_SOURCE, status.MPI_TAG, nbytes);
            auto& m = recvInflight.back();
            rv = MPI_Irecv(m.data.data(), nbytes, MPI_BYTE, status.MPI_SOURCE, status.MPI_TAG, comm_comm, &m.req);
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
            if (m.tag == workTerminationTag) {
                hostWorkTermination[m.host] = true;
            }
            else if (m.tag == remoteWorkTag) {
                if (m.bufLen == aggMsgSize) {
                    recvRemoteWork.addFull(m.buf);
                }
                else {
                    recvRemoteWork.addPartial(m.buf, m.bufLen);
                }
            }
            else if (m.tag == communicationTag) {
                recvCommunication.add(m.host, m.buf);
            }
            else if (m.tag == dataTerminationTag) {
                hostDataTermination[m.host] = true;
            }
            else {
                recvData[m.host].add(m.tag, std::move(m.data));
            }
            
            recvInflight.pop_front();
        }
    }
}

void NetworkInterface::workerThread() {

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
    std::vector<unsigned> hostOrder(Num - 1);
    for (unsigned i = 0; i < Num - 1; i++) {
        if (i + ID + 1 >= Num) {
            hostOrder[i] = i + ID + 1 - Num;
        } else {
            hostOrder[i] = i + ID + 1;
        }
    }

    while (ready < 2) { /*fprintf(stderr, "[WaitOnReady-2]");*/
    };
    
    recvAllocator.touch();
    
    while (ready != 3) {
        for (unsigned i = 0; i < Num - 1; ++i) {
            unsigned h = hostOrder[i];
            
            // handle send queue
            sendTrackComplete();
            
            // 1. remote work
            bool hostWorkEmpty = true;
            for (unsigned t=0; t<numT; t++) {
                // push progress forward on the network IO
                recvProbe();
  
                auto& srw = sendRemoteWork[h][t];
                if (srw.checkFlush()) {
                    hostWorkEmpty = false;
                  
                    uint8_t* work;
                    bool success = srw.pop(work);
                  
                    if (success) {
                        sendFullTrack(t, h, work);
                    }
                    else {
                        if (srw.checkPartial()) {
                            size_t workLen;
                            srw.popPartial(work, workLen);
                            sendPartialTrack(t, h, work, workLen);
                        }
                    }
                }
            }

            if(hostWorkEmpty) { // wait until all works are sent
                // 2. work termination
                if (sendWorkTermination[h]) {
                    send(h, workTerminationTag, nullptr, 0);
                    sendWorkTermination[h] = false;
                }
            }
          
            // 3. data
            recvProbe();
            auto& sd = sendData[h];
            if (sd.checkFlush()) {
                uint32_t tag;
                uint8_t* data;
                size_t dataLen;
                bool success = sd.pop(tag, data, dataLen);
              
                if (success) {
                    send(h, tag, data, dataLen);
                }
            }
        }
    }
  
    finalizeMPI();
}

NetworkInterface::NetworkInterface()
    : workCount(1 << workCountExp),
      aggMsgSize(workSize * workCount),
      sendBufCount(1 << sendBufCountExp),
      recvBufCount(1 << recvBufCountExp) {
    ready               = 0;
    worker = std::thread(&NetworkInterface::workerThread, this);
    numT = galois::getActiveThreads();
    sendAllocators = decltype(sendAllocators)(numT);
    for (unsigned t=0; t<numT; t++) {
        sendAllocators[t].setup(aggMsgSize, sendBufCount);
    }
    recvAllocator.setup(aggMsgSize, recvBufCount);
    while (ready != 1) {};

    recvData = decltype(recvData)(Num);
    sendData = decltype(sendData)(Num);
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
    sendWorkTerminationValid = decltype(sendWorkTerminationValid)(Num);
    hostWorkTermination = decltype(hostWorkTermination)(Num);
    hostWorkTerminationValid = decltype(hostWorkTerminationValid)(Num);
    for (unsigned i=0; i<Num; i++) {
        sendWorkTermination[i] = false;
        if (i == ID) {
            sendWorkTerminationValid[i] = false;
            hostWorkTermination[i] = true;
            hostWorkTerminationValid[i] = false;
        }
        else {
            sendWorkTerminationValid[i] = true;
            hostWorkTermination[i] = false;
            hostWorkTerminationValid[i] = true;
        }
    }
    hostDataTermination = decltype(hostDataTermination)(Num);
    for (unsigned i=0; i<Num; i++) {
        if (i == ID) {
            hostDataTermination[i] = true;
        }
        else {
            hostDataTermination[i] = false;
        }
    }
    sendInflight = decltype(sendInflight)(numT);
    ready    = 2;
}

NetworkInterface::~NetworkInterface() {
    ready = 3;
    worker.join();

    for (unsigned i=0; i<Num; i++) {
        if (recvCommBuffer[i] != nullptr){
            free(recvCommBuffer[i]);
        }
    }
}

void NetworkInterface::sendMsg(uint32_t dest,
                               void (*recv)(uint32_t, RecvBuffer&),
                               SendBuffer& buf) {
    gSerialize(buf, recv);
    sendTagged(dest, 0, buf);
}

void NetworkInterface::sendTagged(uint32_t dest, uint32_t tag, SendBuffer& buf, int phase) {
    tag += phase;

    sendData[dest].push(tag, buf.getVec().extractData(), buf.getVec().size());
}

template <typename ValTy>
void NetworkInterface::sendWork(unsigned tid, uint32_t dest, uint32_t lid, ValTy val) {
    sendRemoteWork[dest][tid].add<ValTy>(lid, val);
}

// explicit instantiation
template void NetworkInterface::sendWork<uint32_t>(unsigned tid, uint32_t dest, uint32_t lid, uint32_t val);
template void NetworkInterface::sendWork<float>(unsigned tid, uint32_t dest, uint32_t lid, float val);

void NetworkInterface::sendComm(uint32_t dest, uint8_t* bufPtr, size_t len) {
    sendData[dest].push(communicationTag, bufPtr, len);
}

void NetworkInterface::broadcast(void (*recv)(uint32_t, RecvBuffer&),
                                 SendBuffer& buf, bool self) {
    trace("NetworkInterface::broadcast", (void*)recv);
    auto fp = (uintptr_t)recv;
    for (unsigned x = 0; x < Num; ++x) {
        if (x != ID) {
            SendBuffer b;
            gSerialize(b, fp, buf, (uintptr_t)&bcastLandingPad);
            sendTagged(x, 0, b);
        } else if (self) {
            RecvBuffer rb(buf.begin(), buf.end());
            recv(ID, rb);
        }
    }
}

void NetworkInterface::allocateRecvCommBuffer(size_t alloc_size) {
    for (unsigned i=0; i<Num; i++) {
        void* ptr = malloc(alloc_size);
        if (ptr == nullptr) {
            galois::gError("Failed to allocate memory for the communication receive work buffer\n");
        }
        recvCommBuffer.push_back(static_cast<uint8_t*>(ptr));
    }
}

void NetworkInterface::deallocateRecvBuffer(uint8_t* buf) {
    recvAllocator.deallocate(buf);
}

void NetworkInterface::handleReceives() {
    auto opt = receiveTagged(0);
    while (opt) {
        uint32_t src    = std::get<0>(*opt);
        RecvBuffer& buf = std::get<1>(*opt);
        uintptr_t fp    = 0;
        gDeserializeRaw(buf.r_linearData() + buf.r_size() - sizeof(uintptr_t), fp);
        buf.pop_back(sizeof(uintptr_t));
        assert(fp);
        auto f = (void (*)(uint32_t, RecvBuffer&))fp;
        f(src, buf);
        opt = receiveTagged(0);
    }
}

std::optional<std::pair<uint32_t, RecvBuffer>>
NetworkInterface::receiveTagged(uint32_t tag, int phase) {
    tag += phase;

    for (unsigned h=0; h<Num; h++) {
        if (h == ID) {
            continue;
        }

        auto& rq = recvData[h];
        if (rq.hasMsg(tag)) {
            auto buf = rq.pop();
            return std::optional<std::pair<uint32_t, RecvBuffer>>(std::make_pair(h, std::move(buf)));
        }
    }

    return std::optional<std::pair<uint32_t, RecvBuffer>>();
}
  
std::optional<std::pair<uint32_t, RecvBuffer>>
NetworkInterface::receiveTagged(bool& terminateFlag, uint32_t tag, int phase) {
    tag += phase;

    for (unsigned h=0; h<Num; h++) {
        if (h == ID) {
            continue;
        }

        auto& rq = recvData[h];
        if (rq.hasMsg(tag)) {
            auto buf = rq.pop();
            return std::optional<std::pair<uint32_t, RecvBuffer>>(std::make_pair(h, std::move(buf)));
        }
    }
  
    if (checkDataTermination()) {
        terminateFlag = true;
    }

    return std::optional<std::pair<uint32_t, RecvBuffer>>();
}

bool NetworkInterface::receiveRemoteWork(std::atomic<bool>& terminateFlag, bool& fullFlag, uint8_t*& work, size_t& workLen) {
    bool success;
    while(true) {
        success = recvRemoteWork.tryPopFullMsg(work);
        if (success) {
            fullFlag = true;
            return true;
        }
        
        success = recvRemoteWork.tryPopPartialMsg(work, workLen);
        if (success) {
            fullFlag = false;
            return true;;
        }

        if (checkWorkTermination()) {
            terminateFlag = true;
            return false;
        }
    }
}

void NetworkInterface::receiveComm(uint32_t& host, uint8_t*& work) {
    bool success;
    do {
        success = recvCommunication.tryPopMsg(host, work);
    } while(!success);
}

void NetworkInterface::flushRemoteWork() {
    galois::on_each(
        [&](unsigned tid, unsigned) {
            for (uint32_t h=0; h<Num; h++) {
                if (h == ID) {
                    continue;
                }

                sendRemoteWork[h][tid].setFlush();
            }
        }
    );
}
  
void NetworkInterface::excludeSendWorkTermination(uint32_t host) {
    sendWorkTerminationValid[host] = false;
}
  
void NetworkInterface::excludeHostWorkTermination(uint32_t host) {
    hostWorkTerminationValid[host] = false;
    hostWorkTermination[host] = true;
}
  
void NetworkInterface::resetWorkTermination() {
    for (unsigned i=0; i<Num; i++) {
        if (hostWorkTerminationValid[i]) {
            hostWorkTermination[i] = false;
        }
    }
}

bool NetworkInterface::checkWorkTermination() {
    for (unsigned i=0; i<Num; i++) {
        if (!hostWorkTermination[i]) {
            return false;
        }
    }
    return true;
}

void NetworkInterface::resetDataTermination() {
    for (unsigned i=0; i<Num; i++) {
        if (i == ID) {
            continue;
        }
        hostDataTermination[i] = false;
    }
}

bool NetworkInterface::checkDataTermination() {
    for (unsigned i=0; i<Num; i++) {
        if (hostDataTermination[i] == false) {
            return false;
        }
    }
    return true;
}

void NetworkInterface::signalDataTermination(uint32_t dest) {
    sendData[dest].push(dataTerminationTag, nullptr, 0);
}

void NetworkInterface::broadcastWorkTermination() {
    for (unsigned i=0; i<Num; i++) {
        if (sendWorkTerminationValid[i]) {
            sendWorkTermination[i] = true;
        }
    }
}

void NetworkInterface::reportMemUsage() const {
    std::string str("CommunicationMemUsage");
    galois::runtime::reportStat_Tmin("dGraph", str + "Min",
                                     memUsageTracker.getMaxMemUsage());
    galois::runtime::reportStat_Tmax("dGraph", str + "Max",
                                     memUsageTracker.getMaxMemUsage());
}

void NetworkInterface::touchBufferPool() {
    galois::on_each([&](unsigned tid, unsigned) {
        sendAllocators[tid].touch();

        for (unsigned i=0; i<Num; i++) {
            sendRemoteWork[i][tid].touchBuf();
        }
    });
}

void NetworkInterface::prefetchBuffers() {
    galois::on_each([&](unsigned tid, unsigned) {
        for (unsigned i=0; i<Num; i++) {
            sendRemoteWork[i][tid].prefetchBuf();
        }
    });
}

NetworkInterface& getSystemNetworkInterface() {
    static std::atomic<NetworkInterface*> net;
    static substrate::SimpleLock m_mutex;

    // create the interface if it doesn't yet exist in the static variable
    auto* tmp = net.load();
    if (tmp == nullptr) {
        std::lock_guard<substrate::SimpleLock> lock(m_mutex);
        tmp = net.load();
        if (tmp == nullptr) {
            tmp = new NetworkInterface();
            net.store(tmp);
        }
    }

    return *tmp;
}

void internal::destroySystemNetworkInterface() {
    // get net interface, then delete it
    NetworkInterface& netInterface = getSystemNetworkInterface();
    delete &netInterface;
}

} // namespace
