/*
 * Copyright (c) 2017 Jason Lowe-Power
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Jason Lowe-Power
 */

#include "learning_gem5/part2/simple_memobj.hh"

#include "debug/SimpleMemobj.hh"

SimpleMemobj::SimpleMemobj(SimpleMemobjParams *params) :
    MemObject(params),
    dataPort(params->name + ".data_port", this),
    memPort(params->name + ".mem_side", this),
    bypassSlb(params->bypass_slb),
    dataPortNeedsRetry(false),
    blocked(false)
{
}

BaseMasterPort&
SimpleMemobj::getMasterPort(const std::string& if_name, PortID idx)
{
    panic_if(idx != InvalidPortID, "This object doesn't support vector ports");

    // This is the name from the Python SimObject declaration (SimpleMemobj.py)
    if (if_name == "mem_side") {
        return memPort;
    } else {
        // pass it along to our super class
        return MemObject::getMasterPort(if_name, idx);
    }
}

BaseSlavePort&
SimpleMemobj::getSlavePort(const std::string& if_name, PortID idx)
{
    panic_if(idx != InvalidPortID, "This object doesn't support vector ports");

    // This is the name from the Python SimObject declaration in SimpleCache.py
    if (if_name == "data_port") {
        return dataPort;
    } else {
        // pass it along to our super class
        return MemObject::getSlavePort(if_name, idx);
    }
}


AddrRangeList
SimpleMemobj::CPUSidePort::getAddrRanges() const
{
    return owner->getAddrRanges();
}

void
SimpleMemobj::CPUSidePort::recvFunctional(PacketPtr pkt)
{
    // Just forward to the memobj.
    return owner->handleFunctional(pkt);
}

bool
SimpleMemobj::CPUSidePort::recvTimingReq(PacketPtr pkt)
{
    // Just forward to the memobj.
    return owner->handleRequest(pkt);
}

void
SimpleMemobj::CPUSidePort::recvRespRetry()
{
    owner->memPort.sendRetryResp();
}

bool
SimpleMemobj::MemSidePort::recvTimingResp(PacketPtr pkt)
{
    // Just forward to the memobj.
    return owner->handleResponse(pkt);
}

void
SimpleMemobj::sendFromRetryQueue()
{
    assert(!retryQueue.empty());
    if (blocked) return;
    DPRINTF(SimpleMemobj, "Retrying %s\n",
            retryQueue.front()->print());
    bool res = memPort.sendTimingReq(retryQueue.front());
    if (res) {
        retryQueue.pop_front();
        if (!retryQueue.empty()) {
            schedule(new EventFunctionWrapper(
                        [this] { sendFromRetryQueue(); }, name()+".sendretry",
                        true),
                     nextCycle());
        }
    } else {
        blocked = true;
    }
}

void
SimpleMemobj::MemSidePort::recvReqRetry()
{
    owner->blocked = false;
    if (owner->dataPortNeedsRetry) {
        owner->dataPort.sendRetryReq();
        owner->dataPortNeedsRetry = false;
    }
    if (!owner->retryQueue.empty()) {
        owner->sendFromRetryQueue();
    }
}

void
SimpleMemobj::MemSidePort::recvRangeChange()
{
    owner->sendRangeChange();
}

bool
SimpleMemobj::handleRequest(PacketPtr pkt)
{
    DPRINTF(SimpleMemobj, "Got request for addr %#x\n", pkt->getAddr());

    // Assuming all load misses cause a read shared
    if (bypassSlb || pkt->cmd != MemCmd::ReadSharedReq ||
            pkt->req->isPrefetch()) {
        assert(!blocked);
        // Simply forward to the memory port
        bool res = memPort.sendTimingReq(pkt);
        if (!res) {
            dataPortNeedsRetry = true;
            blocked = true;
        }
        return res;
    } else {
        DPRINTF(SimpleMemobj, "Delaying as addr %x from PC %x (%s)\n",
                pkt->req->getPaddr(), pkt->req->getPC(), pkt->print());
        auto it = slb.find(pkt->req->getPaddr());
        assert(it == slb.end());
        slb[pkt->req->getPaddr()] = std::make_pair(pkt, curTick());
        return true;
    }
}

bool
SimpleMemobj::handleResponse(PacketPtr pkt)
{
    DPRINTF(SimpleMemobj, "Got response for addr %#x\n", pkt->getAddr());

    return dataPort.sendTimingResp(pkt);
}

void
SimpleMemobj::handleFunctional(PacketPtr pkt)
{
    // Just pass this on to the memory side to handle for now.
    memPort.sendFunctional(pkt);
}

AddrRangeList
SimpleMemobj::getAddrRanges() const
{
    DPRINTF(SimpleMemobj, "Sending new ranges\n");
    // Just use the same ranges as whatever is on the memory side.
    return memPort.getAddrRanges();
}

void
SimpleMemobj::sendRangeChange()
{
    dataPort.sendRangeChange();
}

void
SimpleMemobj::squash(Addr addr)
{
    if (bypassSlb) return;
    DPRINTF(SimpleMemobj, "Got squash for addr %x\n", addr);
    auto it = slb.find(addr);
    if (it != slb.end()) {
        DPRINTF(SimpleMemobj, "Forwarding %s\n", it->second.first->print());
        if (!retryQueue.empty()) {
            retryQueue.push_back(it->second.first);
        } else {
            bool res = memPort.sendTimingReq(it->second.first);
            if (!res) {
                DPRINTF(SimpleMemobj, "Putting %s on the retry queue\n",
                        it->second.first->print());
                retryQueue.push_back(it->second.first);
                blocked = true;
            }
        }
        slb.erase(it);
        squashes++;
        queuedTime.sample(curTick() - it->second.second);
    }
}

void
SimpleMemobj::commit(Addr addr)
{
    if (bypassSlb) return;
    DPRINTF(SimpleMemobj, "Got commit for addr %x\n", addr);
    auto it = slb.find(addr);
    if (it != slb.end()) {
        DPRINTF(SimpleMemobj, "Forwarding %s\n", it->second.first->print());
        if (!retryQueue.empty()) {
            retryQueue.push_back(it->second.first);
        } else {
            bool res = memPort.sendTimingReq(it->second.first);
            if (!res) {
                DPRINTF(SimpleMemobj, "Putting %s on the retry queue\n",
                        it->second.first->print());
                retryQueue.push_back(it->second.first);
            }
        }
        slb.erase(it);
        queuedTime.sample(curTick() - it->second.second);
    }
}

void
SimpleMemobj::regStats()
{
    MemObject::regStats();

    squashes.name(name() + ".squashes")
        .desc("Number of squashes")
        ;

    queuedTime.name(name() + ".queuedTime")
        .desc("Ticks for misses to the cache")
        .init(16) // number of buckets
        ;
}


SimpleMemobj*
SimpleMemobjParams::create()
{
    return new SimpleMemobj(this);
}
