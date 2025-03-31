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

bool NetworkInterface::recvBufferRemoteWork::tryPopMsg(uint8_t*& work, size_t& workLen) {
    std::pair<uint8_t*, size_t> message;
    bool success = messages.try_dequeue(message);
    if (success) {
        work = message.first;
        workLen = message.second;
    }

    return success;
}

// Worker thread interface
void NetworkInterface::recvBufferRemoteWork::add(uint8_t* work, size_t workLen) {
    messages.enqueue(std::make_pair(work, workLen));
}

bool NetworkInterface::sendBufferData::pop(uint32_t& tag, uint8_t*& data, size_t& dataLen) {
    std::tuple<uint32_t, uint8_t*, size_t> message;
    bool success = messages.try_dequeue(message);
    if (success) {
        flush -= 1;
        tag = std::get<0>(message);
        data = std::get<1>(message);
        dataLen = std::get<2>(message);
    }

    return success;
}

void NetworkInterface::sendBufferData::push(uint32_t tag, uint8_t* data, size_t dataLen) {
    messages.enqueue(std::make_tuple(tag, data, dataLen));
    flush += 1;
}

void NetworkInterface::sendBufferRemoteWork::setNet(NetworkInterface* _net) {
    net = _net;
  
    if (buf == nullptr) {
        // allocate new buffer
        do {
            buf = net->sendAllocators[tid].allocate();
        } while (buf == nullptr);
    }
  
}

void NetworkInterface::sendBufferRemoteWork::setFlush() {
    if (msgCount != 0) {
        // put number of message count at the very last
        *((uint32_t*)(buf + bufLen)) = msgCount;
        bufLen += sizeof(uint32_t);
        messages.enqueue(std::make_pair(buf, bufLen));
    
        // allocate new buffer
        do {
            buf = net->sendAllocators[tid].allocate();
        } while (buf == nullptr);
        bufLen = 0;
        msgCount = 0;
        
        flush += 1;
    }
}

bool NetworkInterface::sendBufferRemoteWork::pop(uint8_t*& work, size_t& workLen) {
    std::pair<uint8_t*, size_t> message;
    bool success = messages.try_dequeue(message);
    if (success) {
        flush -= 1;
        work = message.first;
        workLen = message.second;
    }

    return success;
}

template <typename ValTy>
void NetworkInterface::sendBufferRemoteWork::add(uint32_t lid, ValTy val) {
    size_t workLen = sizeof(uint32_t) + sizeof(ValTy);
    if (bufLen + workLen + sizeof(uint32_t) > AGG_MSG_SIZE) {
        // put number of message count at the very last
        *((uint32_t*)(buf + bufLen)) = msgCount;
        bufLen += sizeof(uint32_t);
        messages.enqueue(std::make_pair(buf, bufLen));

        // allocate new buffer
        do {
            buf = net->sendAllocators[tid].allocate();
        } while (buf == nullptr);
        *((uint32_t*)buf) = lid;
        bufLen = sizeof(uint32_t);
        *((ValTy*)(buf + bufLen)) = val;
        bufLen += sizeof(ValTy);
        msgCount = 1;

        flush += 1;
    }
    else {
        // aggregate message
        *((uint32_t*)(buf + bufLen)) = lid;
        bufLen += sizeof(uint32_t);
        *((ValTy*)(buf + bufLen)) = val;
        bufLen += sizeof(ValTy);
        msgCount += 1;
    }
}

// explicit instantiation
template void NetworkInterface::sendBufferRemoteWork::add<uint32_t>(uint32_t lid, uint32_t val);
template void NetworkInterface::sendBufferRemoteWork::add<float>(uint32_t lid, float val);
template void NetworkInterface::sendBufferRemoteWork::add<uint64_t>(uint32_t lid, uint64_t val);
template void NetworkInterface::sendBufferRemoteWork::add<double>(uint32_t lid, double val);
    
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
    MPI_Request req;
    int rv = MPI_Isend(buf, bufLen, MPI_BYTE, dest, tag, comm_comm, &req);
    handleError(rv);
}

void NetworkInterface::sendTrack(unsigned tid, uint32_t dest, uint8_t* buf, size_t bufLen) {
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
            do {
                buf = recvAllocator.allocate();
            } while (buf == nullptr);

            recvInflight.emplace_back(status.MPI_SOURCE, status.MPI_TAG, buf, nbytes);
            auto& m = recvInflight.back();
            rv = MPI_Irecv(buf, nbytes, MPI_BYTE, status.MPI_SOURCE, status.MPI_TAG, comm_comm, &m.req);
            handleError(rv);
        }
        else if (status.MPI_TAG == (int)communicationTag) {
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
                hostWorkTermination[m.host] += 1;
            }
            else if (m.tag == remoteWorkTag) {
                recvRemoteWork.add(m.buf, m.bufLen);
            }
            else if (m.tag == communicationTag) {
                recvCommunication.add(m.host, m.buf);
            }
            else if (m.tag == dataTerminationTag) {
                hostDataTermination[m.host] += 1;
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
                    size_t workLen;
                    bool success = srw.pop(work, workLen);
                  
                    if (success) {
                        sendTrack(t, h, work, workLen);
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

NetworkInterface::NetworkInterface() {
    ready               = 0;
    worker = std::thread(&NetworkInterface::workerThread, this);
    numT = galois::getActiveThreads();
    sendAllocators = decltype(sendAllocators)(numT);
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
    hostDataTermination = decltype(hostDataTermination)(Num);
    for (unsigned i=0; i<Num; i++) {
        if (i == ID) {
            hostDataTermination[i] = 1;
        }
        else {
            hostDataTermination[i] = 0;
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
template void NetworkInterface::sendWork<uint64_t>(unsigned tid, uint32_t dest, uint32_t lid, uint64_t val);
template void NetworkInterface::sendWork<double>(unsigned tid, uint32_t dest, uint32_t lid, double val);

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

bool NetworkInterface::receiveRemoteWork(uint8_t*& work, size_t& workLen) {
    bool success = recvRemoteWork.tryPopMsg(work, workLen);
    return success;
}

bool NetworkInterface::receiveRemoteWork(bool& terminateFlag, uint8_t*& work, size_t& workLen) {
    bool success = recvRemoteWork.tryPopMsg(work, workLen);
    if (!success) {
        if (checkWorkTermination()) {
            terminateFlag = true;
        }
    }

    return success;
}

bool NetworkInterface::receiveComm(uint32_t& host, uint8_t*& work) {
    bool success = recvCommunication.tryPopMsg(host, work);
    return success;
}

void NetworkInterface::flushRemoteWork() {
    for (auto& hostSendRemoteWork : sendRemoteWork) {
        for (auto& threadSendRemoteWork : hostSendRemoteWork) {
            threadSendRemoteWork.setFlush();
        }
    }
}
  
void NetworkInterface::resetWorkTermination() {
    for (unsigned i=0; i<Num; i++) {
        if (i == ID) {
            continue;
        }
        hostWorkTermination[i] -= 1;
    }
}

bool NetworkInterface::checkWorkTermination() {
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

void NetworkInterface::resetDataTermination() {
    for (unsigned i=0; i<Num; i++) {
        if (i == ID) {
            continue;
        }
        hostDataTermination[i] -= 1;
    }
}

bool NetworkInterface::checkDataTermination() {
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

void NetworkInterface::signalDataTermination(uint32_t dest) {
    sendData[dest].push(dataTerminationTag, nullptr, 0);
}

void NetworkInterface::broadcastWorkTermination() {
    for (unsigned i=0; i<Num; i++) {
        if (i == ID) {
            continue;
        }
        else {
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
