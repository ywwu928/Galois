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

#include <iostream>
#include <mutex>
#include <chrono>
#include <xmmintrin.h>
#include <cstring>

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

uint32_t NetworkInterface::ID  = 0;
uint32_t NetworkInterface::Num = 1;

uint32_t getHostID() { return NetworkInterface::ID; }
uint32_t getHostNum() { return NetworkInterface::Num; }

void NetworkInterface::initializeMPI() {
    int supportProvided;
    int initSuccess =
        MPI_Init_thread(nullptr, nullptr, MPI_THREAD_MULTIPLE, &supportProvided);
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

bool NetworkInterface::recvBufferCommunication::tryPopMsg(uint32_t& host) {
    bool success = hosts.try_dequeue(host);
    return success;
}

void NetworkInterface::recvBufferCommunication::add(uint32_t host) {
    hosts.enqueue(host);
}

bool NetworkInterface::recvBufferRemoteWork::tryPopFullMsg(uint8_t*& work) {
    bool success = fullMessages.try_dequeue_from_producer(ptokFull, work);
    __builtin_prefetch(work, 0, 3);
    return success;
}

bool NetworkInterface::recvBufferRemoteWork::tryPopPartialMsg(uint8_t*& work, size_t& workLen) {
    std::pair<uint8_t*, size_t> message;
    bool success = partialMessages.try_dequeue_from_producer(ptokPartial, message);
    work = message.first;
    workLen = message.second;
    __builtin_prefetch(work, 0, 3);

    return success;
}

void NetworkInterface::recvBufferRemoteWork::addFull(uint8_t* work) {
    fullMessages.enqueue(ptokFull, work);
}

void NetworkInterface::recvBufferRemoteWork::addPartial(uint8_t* work, size_t workLen) {
    partialMessages.enqueue(ptokPartial, std::make_pair(work, workLen));
}

bool NetworkInterface::sendBufferData::pop(uint32_t& tag, uint8_t*& data, size_t& dataLen) {
    std::tuple<uint32_t, uint8_t*, size_t> message;
    bool success = messages.try_dequeue(message);
    tag = std::get<0>(message);
    data = std::get<1>(message);
    dataLen = std::get<2>(message);

    return success;
}

void NetworkInterface::sendBufferData::push(uint32_t tag, uint8_t* data, size_t dataLen) {
    messages.enqueue(std::make_tuple(tag, data, dataLen));
}

void NetworkInterface::sendBufferRemoteWork::setNet(NetworkInterface* _net) {
    net = _net;
  
    // allocate new buffer
    buf = net->sendAllocators[tid].allocate();
    __builtin_prefetch(buf, 1, 3);
}

void NetworkInterface::sendBufferRemoteWork::enqueue(uint8_t* msg) {
    messages.enqueue(msg);
}

bool NetworkInterface::sendBufferRemoteWork::pop(uint8_t*& work) {
    bool success = messages.try_dequeue(work);
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

        // allocate new buffer
        buf = net->sendAllocators[tid].allocate();
        __builtin_prefetch(buf, 1, 3);
        msgCount = 0;
    }
}

// explicit instantiation
template void NetworkInterface::sendBufferRemoteWork::add<uint32_t>(uint32_t lid, uint32_t val);
template void NetworkInterface::sendBufferRemoteWork::add<float>(uint32_t lid, float val);
    
void NetworkInterface::sendDataComplete() {
    if (!sendInflightData.empty()) {
        int flag = 0;
        MPI_Status status;
        auto& f = sendInflightData.front();
        MPI_Test(&f.req, &flag, &status);
        if (flag) {
            free(f.buf);
            sendInflightData.pop_front();
        }
        else {
            sendInflightData.push_back(f);
            sendInflightData.pop_front();
        }
    }
}
    
void NetworkInterface::sendWorkComplete() {
    for (unsigned t=0; t<numT; t++) {
        if (!sendInflightWork[t].empty()) {
            int flag = 0;
            MPI_Status status;
            auto& f = sendInflightWork[t].front();
            MPI_Test(&f.req, &flag, &status);
            if (flag) {
                // return buffer back to pool
                sendAllocators[t].deallocate(f.buf);
                sendInflightWork[t].pop_front();
            }
            else {
                sendInflightWork[t].push_back(f);
                sendInflightWork[t].pop_front();
            }
        }
    }
}
    
void NetworkInterface::sendWorkCompleteUntilEmpty() {
    bool empty;
    do {
        empty = true;
        for (unsigned t=0; t<numT; t++) {
            while (!sendInflightWork[t].empty()) {
                empty = false;

                int flag = 0;
                MPI_Status status;
                auto& f = sendInflightWork[t].front();
                MPI_Test(&f.req, &flag, &status);
                if (flag) {
                    // return buffer back to pool
                    sendAllocators[t].deallocate(f.buf);
                    sendInflightWork[t].pop_front();
                }
                else {
                    sendInflightWork[t].push_back(f);
                    sendInflightWork[t].pop_front();
                    break;
                }
            }
        }
    } while (!empty);
}

void NetworkInterface::sendTaggedData(uint32_t dest, uint32_t tag, uint8_t* buf, size_t bufLen) {
    __builtin_prefetch(buf, 0, 3);
    sendInflightData.emplace_back(buf);
    auto& f = sendInflightData.back();
    MPI_Isend(buf, bufLen, MPI_BYTE, dest, tag, comm_comm, &f.req);
}

void NetworkInterface::sendFullWork(unsigned tid, uint32_t dest, uint8_t* buf) {
    __builtin_prefetch(buf, 0, 3);
    sendInflightWork[tid].emplace_back(buf);
    auto& f = sendInflightWork[tid].back();
    MPI_Isend(buf, aggMsgSize, MPI_BYTE, dest, remoteWorkTag, comm_comm, &f.req);
}

void NetworkInterface::sendPartialWork(unsigned tid, uint32_t dest, uint8_t* buf, size_t bufLen) {
    __builtin_prefetch(buf, 0, 3);
    sendInflightWork[tid].emplace_back(buf);
    auto& f = sendInflightWork[tid].back();
    MPI_Isend(buf, bufLen, MPI_BYTE, dest, remoteWorkTag, comm_comm, &f.req);
}

void NetworkInterface::sendTermination(uint32_t dest, uint32_t tag) {
    MPI_Isend(nullptr, 0, MPI_BYTE, dest, tag, comm_comm, &inflightTermination[dest]);
}

void NetworkInterface::recvProbeData() {
    int flag = 0;
    MPI_Status status;
    // check for new messages
    MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, comm_comm, &flag, &status);
    if (flag) {
        int nbytes;
        MPI_Get_count(&status, MPI_BYTE, &nbytes);
        
        recvInflightData.emplace_back(status.MPI_SOURCE, status.MPI_TAG, nbytes);
        auto& m = recvInflightData.back();
        MPI_Irecv(m.data.data(), nbytes, MPI_BYTE, status.MPI_SOURCE, status.MPI_TAG, comm_comm, &m.req);
    }

    // complete messages
    if (!recvInflightData.empty()) {
        auto& m  = recvInflightData.front();
        int flag = 0;
        MPI_Test(&m.req, &flag, MPI_STATUS_IGNORE);
        if (flag) {
            recvData[m.host].add(m.tag, std::move(m.data));
            recvInflightData.pop_front();
        }
    }
}

void NetworkInterface::recvProbeWork() {
    int flag = 0;
    MPI_Status status;
    // check for new messages
    MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, comm_comm, &flag, &status);
    if (flag) {
        int nbytes;
        MPI_Get_count(&status, MPI_BYTE, &nbytes);

        if (status.MPI_TAG ==  (int)remoteWorkTag) {
            // allocate new buffer
            uint8_t* buf;
            buf = recvAllocator.allocate();
            __builtin_prefetch(buf, 1, 3);

            recvInflightWork.emplace_back(buf, nbytes);
            auto& m = recvInflightWork.back();
            MPI_Irecv(buf, nbytes, MPI_BYTE, status.MPI_SOURCE, status.MPI_TAG, comm_comm, &m.req);
        }
        else { // workTerminationTag
            MPI_Irecv(nullptr, 0, MPI_BYTE, status.MPI_SOURCE, status.MPI_TAG, comm_comm, &inflightTermination[Num + status.MPI_SOURCE]);
            terminationCountTemp += 1;
        }
    }

    // complete messages
    if (!recvInflightWork.empty()) {
        auto& m  = recvInflightWork.front();
        flag = 0;
        MPI_Test(&m.req, &flag, MPI_STATUS_IGNORE);
        if (flag) {
            if (m.bufLen == aggMsgSize) {
                recvRemoteWork.addFull(m.buf);
            }
            else {
                recvRemoteWork.addPartial(m.buf, m.bufLen);
            }
            
            recvInflightWork.pop_front();
            return;
        }
        else {
            recvInflightWork.push_back(m);
            recvInflightWork.pop_front();
        }
    }
    else {
        if (terminationCountTemp != 0) {
            hostWorkTerminationCount.fetch_add(terminationCountTemp, std::memory_order_release);
            terminationCountTemp = 0;
        }
    }
}

void NetworkInterface::recvProbeComm() {
    int flag = 0;
    MPI_Status status;
    // check for new messages
    MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, comm_comm, &flag, &status);
    if (flag) {
        int nbytes;
        MPI_Get_count(&status, MPI_BYTE, &nbytes);

        if (status.MPI_TAG == (int)communicationTag) {
            __builtin_prefetch(recvCommBuffer[status.MPI_SOURCE], 1, 3);

            MPI_Request* req = (MPI_Request*)malloc(sizeof(MPI_Request));
            recvInflightComm.push_back(req);
            MPI_Irecv(recvCommBuffer[status.MPI_SOURCE], nbytes, MPI_BYTE, status.MPI_SOURCE, status.MPI_TAG, comm_comm, req);
        }
    }

    // complete messages
    if (!recvInflightComm.empty()) {
        MPI_Request* req  = recvInflightComm.front();
        flag = 0;
        MPI_Test(req, &flag, &status);
        if (flag) {
            recvCommunication.add(status.MPI_SOURCE);
            free(req);
            recvInflightComm.pop_front();
        }
        else {
            recvInflightComm.push_back(req);
            recvInflightComm.pop_front();
        }
    }
}

void NetworkInterface::recvProbeDataTermination() {
    int flag = 0;
    MPI_Status status;
    // check for new messages
    MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, comm_comm, &flag, &status);
    if (flag) {
        int nbytes;
        MPI_Get_count(&status, MPI_BYTE, &nbytes);
        
        if (status.MPI_TAG ==  (int)dataTerminationTag) {
            MPI_Irecv(nullptr, 0, MPI_BYTE, status.MPI_SOURCE, status.MPI_TAG, comm_comm, &inflightTermination[Num + status.MPI_SOURCE]);
            terminationCountTemp += 1;
        }
        else {
            recvInflightData.emplace_back(status.MPI_SOURCE, status.MPI_TAG, nbytes);
            auto& m = recvInflightData.back();
            MPI_Irecv(m.data.data(), nbytes, MPI_BYTE, status.MPI_SOURCE, status.MPI_TAG, comm_comm, &m.req);
        }
    }

    // complete messages
    if (!recvInflightData.empty()) {
        auto& m  = recvInflightData.front();
        int flag = 0;
        MPI_Test(&m.req, &flag, MPI_STATUS_IGNORE);
        if (flag) {
            recvData[m.host].add(m.tag, std::move(m.data));
            recvInflightData.pop_front();
        }
    }
    else {
        if (terminationCountTemp != 0) {
            hostDataTerminationCount.fetch_add(terminationCountTemp, std::memory_order_release);
            terminationCountTemp = 0;
        }
    }
}

void NetworkInterface::terminationComplete() {
    std::vector<MPI_Status> terminationStatus(2 * Num);
    MPI_Waitall(2 * Num, inflightTermination.data(), terminationStatus.data());
}

void NetworkInterface::commThread() {
    ID = getID();
    Num = getNum();

    ready.store(1, std::memory_order_release);
    std::vector<unsigned> hostOrder(Num - 1);
    for (unsigned i = 0; i < Num - 1; i++) {
        if (i + ID + 1 >= Num) {
            hostOrder[i] = i + ID + 1 - Num;
        } else {
            hostOrder[i] = i + ID + 1;
        }
    }

    while (ready.load(std::memory_order_acquire) < 2) { /*fprintf(stderr, "[WaitOnReady-2]");*/
    };
    
    // for graph partitioning
    while (ready.load(std::memory_order_acquire) == 2) {
        for (unsigned i = 0; i < Num - 1; ++i) {
            unsigned h = hostOrder[i];
                
            // handle send queue
            sendDataComplete();
            
            recvProbeData();
          
            // only data
            uint32_t tag;
            uint8_t* data;
            size_t dataLen;
            bool success = sendData[h].pop(tag, data, dataLen);
          
            if (success) {
                sendTaggedData(h, tag, data, dataLen);
            }
        }
    }
    
    // for program execution
    for (unsigned i=0; i<Num; i++) {
        if (i == ID) {
            continue;
        }

        *(recvCommBuffer[i]) = (uint8_t)0;
    }
    
    recvAllocator.touch();
    
    while (ready.load(std::memory_order_acquire) == 3) {
        while (ready.load(std::memory_order_acquire) == 3 && !flush.load(std::memory_order_acquire)) {
            for (unsigned i = 0; i < Num - 1; ++i) {
                unsigned h = hostOrder[i];
                
                // handle send queue
                sendWorkComplete();
                
                // remote work
                for (unsigned t=0; t<numT; t++) {
                    // push progress forward on the network IO
                    recvProbeWork();
      
                    auto& srw = sendRemoteWork[h][t];

                    uint8_t* work;
                    bool success = srw.pop(work);
                  
                    if (success) {
                        sendFullWork(t, h, work);
                    }
                }
            }
        }

        // flush all full work messages
        for (unsigned i = 0; i < Num - 1; ++i) {
            unsigned h = hostOrder[i];
            
            for (unsigned t=0; t<numT; t++) {
                // push progress forward on the network IO
                recvProbeWork();
  
                auto& srw = sendRemoteWork[h][t];

                uint8_t* work;
                bool success = srw.pop(work);
              
                while (success) {
                    sendFullWork(t, h, work);
                    success = srw.pop(work);
                }
            }
        }

        // flush all partial work messages
        for (unsigned i = 0; i < Num - 1; ++i) {
            unsigned h = hostOrder[i];

            if (partialBufLen[h] > 0) {
                sendPartialWork(0, h, partialBuf[h], partialBufLen[h]);
            }
            else {
                sendAllocators[0].deallocate(partialBuf[h]);
            }

            // send work termination
            if (sendWorkTermination[h].load(std::memory_order_acquire)) {
                sendTermination(h, workTerminationTag);
                sendWorkTermination[h].store(false, std::memory_order_relaxed);
            }

            partialBuf[h] = nullptr;
            partialBufLen[h] = 0;
        }

        // reset flush
        flush.store(false, std::memory_order_relaxed);
        
        while (ready.load(std::memory_order_acquire) == 3 && !recvAll.load(std::memory_order_acquire)) {
            // push progress forward on the network IO
            recvProbeWork();
        }

        // reset recvAll
        recvAll.store(false, std::memory_order_relaxed);
            
        // handle send queue
        sendWorkCompleteUntilEmpty();

        terminationComplete();
    }
    
    while (ready.load(std::memory_order_acquire) == 4) {
        for (unsigned i = 0; i < Num - 1; ++i) {
            unsigned h = hostOrder[i];
            
            // handle send queue
            sendDataComplete();
          
            // data
            recvProbeComm();
            uint32_t tag;
            uint8_t* data;
            size_t dataLen;
            bool success = sendData[h].pop(tag, data, dataLen);
          
            if (success) {
                sendTaggedData(h, tag, data, dataLen);
            }
        }
    }
    
    // for collecting stats
    while (ready.load(std::memory_order_acquire) == 5) {
        for (unsigned i = 0; i < Num - 1; ++i) {
            unsigned h = hostOrder[i];
                
            // handle send queue
            sendDataComplete();
            
            recvProbeDataTermination();
          
            // only data
            uint32_t tag;
            uint8_t* data;
            size_t dataLen;
            bool success = sendData[h].pop(tag, data, dataLen);
          
            if (success) {
                sendTaggedData(h, tag, data, dataLen);
            }
            else {
                if (sendDataTermination[h].load(std::memory_order_acquire) == true) {
                    sendTermination(h, dataTerminationTag);
                    sendDataTermination[h].store(false, std::memory_order_relaxed);
                }
            }
        }
    }
}

NetworkInterface::NetworkInterface()
    : workCount(1 << workCountExp),
      aggMsgSize(workSize * workCount),
      sendBufCount(1 << sendBufCountExp),
      recvBufCount(1 << recvBufCountExp) {
    ready.store(0, std::memory_order_release);
    initializeMPI();
    comm = std::thread(&NetworkInterface::commThread, this);
    numT = galois::getActiveThreads();
    sendAllocators = decltype(sendAllocators)(numT);
    for (unsigned t=0; t<numT; t++) {
        sendAllocators[t].setup(aggMsgSize, sendBufCount);
    }
    recvAllocator.setup(aggMsgSize, recvBufCount);
    while (ready.load(std::memory_order_acquire) != 1) {};

    flush.store(false, std::memory_order_relaxed);
    partialBuf = decltype(partialBuf)(Num);
    partialBufLen = decltype(partialBufLen)(Num);
    for (unsigned i=0; i<Num; i++) {
        partialBuf[i] = nullptr;
        partialBufLen[i] = 0;
    }

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

    recvAll.store(false, std::memory_order_relaxed);
    sendWorkTermination = decltype(sendWorkTermination)(Num);
    sendWorkTerminationValid = decltype(sendWorkTerminationValid)(Num);
    hostWorkTerminationBase = 0;
    hostWorkTerminationCount.store(0, std::memory_order_relaxed);
    for (unsigned i=0; i<Num; i++) {
        sendWorkTermination[i].store(false, std::memory_order_relaxed);
        if (i == ID) {
            sendWorkTerminationValid[i] = false;
        }
        else {
            sendWorkTerminationValid[i] = true;
        }
    }
    sendDataTermination = decltype(sendDataTermination)(Num);
    hostDataTerminationCount.store(1, std::memory_order_relaxed);
    for (unsigned i=0; i<Num; i++) {
        sendDataTermination[i].store(false, std::memory_order_relaxed);
    }
    terminationCountTemp = 0;
    
    sendInflightWork = decltype(sendInflightWork)(numT);
    inflightTermination = decltype(inflightTermination)(2 * Num);
    for (unsigned i=0; i<2*Num; i++) {
        inflightTermination[i] = MPI_REQUEST_NULL;
    }

    ready.store(2, std::memory_order_release);
}

NetworkInterface::~NetworkInterface() {
    ready.store(6, std::memory_order_release);
    comm.join();
  
    finalizeMPI();

    for (unsigned i=0; i<Num; i++) {
        if (i == ID) {
            continue;
        }
        
        free(recvCommBuffer[i]);
    }
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

void NetworkInterface::allocateRecvCommBuffer(size_t alloc_size) {
    recvCommBuffer.resize(Num, nullptr);
    for (unsigned i=0; i<Num; i++) {
        if (i == ID) {
            continue;
        }

        void* ptr = malloc(alloc_size);
        if (ptr == nullptr) {
            galois::gError("Failed to allocate memory for the communication receive work buffer\n");
        }
        recvCommBuffer[i] = (uint8_t*)ptr;
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
  
    if (hostDataTerminationCount.load(std::memory_order_acquire) == Num) {
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
            return true;
        }

        if (hostWorkTerminationCount.load(std::memory_order_acquire) == Num) {
            terminateFlag.store(true, std::memory_order_release);
            return false;
        }

        std::this_thread::yield();
    }
}

void NetworkInterface::receiveComm(uint32_t& host, uint8_t*& work) {
    bool success;
    do {
        success = recvCommunication.tryPopMsg(host);
    } while(!success);

    work = recvCommBuffer[host];
}

void NetworkInterface::flushRemoteWork() {
    // aggregate partial messages across threads
    for (uint32_t h = 0; h < Num; ++h) {
        if (h == ID) {
            continue;
        }
        
        uint8_t* aggBuf = sendAllocators[0].allocate();
        __builtin_prefetch(aggBuf, 1, 3);
        uint32_t aggMsgCount = 0;
        uint32_t remainWorkCount = workCount;
        for (unsigned t=0; t<numT; t++) {
            auto& srw = sendRemoteWork[h][t];
            uint32_t msgCount = srw.getMsgCount();

            if (msgCount != 0) {
                size_t aggBufLen = aggMsgCount << 3; // 2 * sizeof(uint32_t) * aggMsgCount
                uint8_t* buf = srw.getBuf();
                
                if (msgCount < remainWorkCount) {
                    size_t bufLen = msgCount << 3;

                    std::memcpy((aggBuf + aggBufLen), buf, bufLen);

                    aggMsgCount += msgCount;
                    remainWorkCount -= msgCount;
                    srw.resetMsgCount();
                }
                else if (msgCount == remainWorkCount) {
                    size_t bufLen = msgCount << 3;

                    std::memcpy((aggBuf + aggBufLen), buf, bufLen);

                    sendRemoteWork[h][0].enqueue(aggBuf);
                    
                    aggBuf = sendAllocators[0].allocate();
                    __builtin_prefetch(aggBuf, 1, 3);
                    
                    aggMsgCount = 0;
                    remainWorkCount = workCount;
                    srw.resetMsgCount();
                }
                else { // msgCount > remainWorkCount
                    size_t remainLen = remainWorkCount << 3;

                    std::memcpy((aggBuf + aggBufLen), buf, remainLen);

                    sendRemoteWork[h][0].enqueue(aggBuf);

                    aggBuf = sendAllocators[0].allocate();
                    __builtin_prefetch(aggBuf, 1, 3);

                    aggMsgCount = msgCount - remainWorkCount;
                    remainWorkCount = workCount - aggMsgCount;

                    aggBufLen = aggMsgCount << 3;

                    std::memcpy(aggBuf, (buf + remainLen), aggBufLen);

                    srw.resetMsgCount();
                }
            }
        }

        partialBuf[h] = aggBuf;
        if (aggMsgCount != 0) {
            size_t aggBufLen = aggMsgCount << 3;
            *((uint32_t*)(aggBuf + aggBufLen)) = aggMsgCount;
            aggBufLen += sizeof(uint32_t);
            partialBufLen[h] = aggBufLen;
        }
        else {
            partialBufLen[h] = 0;
        }
        
        if (sendWorkTerminationValid[h]) {
            sendWorkTermination[h].store(true, std::memory_order_release);
        }
    }
    
    flush.store(true, std::memory_order_release);
}
  
void NetworkInterface::excludeSendWorkTermination(uint32_t host) {
    sendWorkTerminationValid[host] = false;
}
  
void NetworkInterface::excludeHostWorkTermination() {
    hostWorkTerminationBase += 1;
    hostWorkTerminationCount.fetch_add(1, std::memory_order_relaxed);
}
  
void NetworkInterface::resetWorkTermination() {
    recvAll.store(true, std::memory_order_release);
    hostWorkTerminationCount.store(hostWorkTerminationBase, std::memory_order_relaxed);
}

void NetworkInterface::resetDataTermination() {
    hostDataTerminationCount.store(1, std::memory_order_relaxed);
}

void NetworkInterface::signalDataTermination(uint32_t dest) {
    sendDataTermination[dest].store(true, std::memory_order_release);
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
