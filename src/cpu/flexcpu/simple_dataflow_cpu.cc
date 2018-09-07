/*
 * Copyright (c) 2018 The Regents of The University of California
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
 * Authors: Bradley Wang
 */

#include "cpu/flexcpu/simple_dataflow_cpu.hh"

#include "arch/locked_mem.hh"
#include "arch/mmapped_ipr.hh"
#include "base/compiler.hh"
#include "debug/SDCPUCoreEvent.hh"

using namespace std;
using namespace TheISA;


SimpleDataflowCPU::SimpleDataflowCPU(SimpleDataflowCPUParams* params):
    BaseCPU(params),
    cacheBlockMask(~(cacheLineSize() - 1)),
    clockedDtbTranslation(params->clocked_dtb_translation),
    clockedExecution(params->clocked_execution),
    clockedInstFetch(params->clocked_inst_fetch),
    clockedItbTranslation(params->clocked_itb_translation),
    clockedMemoryRequest(params->clocked_memory_request),
    executionLatency(params->execution_latency),
    _dataPort(name() + "._dataPort", this),
    _instPort(name() + "._instPort", this)
{
    fatal_if(FullSystem, "FullSystem not implemented for SimpleDataflowCPU");

    for (ThreadID i = 0; i < numThreads; i++) {
        // Move constructor + vector should allow us to avoid memory leaks like
        // the SimpleCPU implementations have.
        threads.push_back(m5::make_unique<SDCPUThread>(this, i, params->system,
            params->workload[i], params->itb, params->dtb, params->isa[i],
            params->fetch_buffer_size,
            params->strict_serialization));

        threadContexts.push_back(threads[i]->getThreadContext());
    }
    // TODO add any other constructor details
}

SimpleDataflowCPU::~SimpleDataflowCPU()
{
    //
}

void
SimpleDataflowCPU::activateContext(ThreadID tid)
{
    DPRINTF(SDCPUCoreEvent, "activateContext(%d)\n", tid);

    BaseCPU::activateContext(tid);
}

void
SimpleDataflowCPU::attemptAllDataXlations()
{
    DPRINTF(SDCPUCoreEvent, "attemptAllDataXlations()\n");
    assert(!dataTranslationReqs.empty());

    while (!dataTranslationReqs.empty()) {
        auto xlation_req = dataTranslationReqs.front();
        RequestPtr req = xlation_req.request;
        ThreadContext* tc = xlation_req.tc;
        bool write = xlation_req.write;
        TranslationCallback callback = xlation_req.callback;

        DPRINTF(SDCPUCoreEvent, "handleDataAddrTranslation()\n");

        // Retrieve dtb from the ThreadContext
        BaseTLB* dtb = tc->getDTBPtr();

        // The event callback for when the translation completed comes through
        // a BaseTLB::Translation object, so we pass that callback straight to
        // the requestor's callback function by subclassing it.
        BaseTLB::Translation* handler = new CallbackTransHandler(callback,
                                                                 true);

        dtb->translateTiming(req, tc, handler,
                             write ? BaseTLB::Write : BaseTLB::Read);

        dataTranslationReqs.pop_front();
    }
}

void
SimpleDataflowCPU::attemptAllExecutions()
{
    DPRINTF(SDCPUCoreEvent, "attemptAllExecutions()\n");
    assert(!executionReqs.empty());

    while (!executionReqs.empty()) {
        auto req = executionReqs.front();

        Event* event = new EventFunctionWrapper([this, req]() {
            completeExecution(req);
        }, name() + ".executeEvent", true);

        schedule(event, clockedExecution ? clockEdge(executionLatency) :
                 curTick() + cyclesToTicks(executionLatency));

        executionReqs.pop_front();
    }

}

void
SimpleDataflowCPU::attemptAllFetchReqs()
{
    DPRINTF(SDCPUCoreEvent, "attemptAllFetchReqs()\n");
    assert(!fetchReqs.empty());

    if (_instPort.receivedPushback)
        return; // Don't send any requests while we're waiting for recvReqRetry

    // Only limitation right now is the memory system
    while (!fetchReqs.empty()) {
        const FetchReq& request = fetchReqs.front();

        if (!_instPort.sendTimingReq(request.packet)) {
            DPRINTF(SDCPUCoreEvent, "_instPort reported busy\n");

            _instPort.receivedPushback = true;
            return; // Stop trying to send requests until we're called again
                    // in recvReqRetry().
        }

        DPRINTF(SDCPUCoreEvent, "Sent a fetch request(pa: %#x)\n",
                                request.packet->req->getPaddr());

        // We mark the control flow so that we know what to call when memory
        // responds
        outstandingFetches.emplace(request.packet, request.callback);
        fetchReqs.pop_front();
    }
}

void
SimpleDataflowCPU::attemptAllInstXlations()
{
    DPRINTF(SDCPUCoreEvent, "attemptAllInstXlations()\n");
    assert(!instTranslationReqs.empty());

    while (!instTranslationReqs.empty()) {
        auto xlation_req = instTranslationReqs.front();
        RequestPtr req = xlation_req.request;
        ThreadContext* tc = xlation_req.tc;
        TranslationCallback callback = xlation_req.callback;

        DPRINTF(SDCPUCoreEvent, "handleInstAddrTranslation()\n");

        // Retrieve dtb from the ThreadContext
        BaseTLB* itb = tc->getITBPtr();

        // The event callback for when the translation completed comes through
        // a BaseTLB::Translation object, so we pass that callback straight to
        // the requestor's callback function by subclassing it.
        BaseTLB::Translation* handler = new CallbackTransHandler(callback,
                                                                 true);

        itb->translateTiming(req, tc, handler, BaseTLB::Execute);

        instTranslationReqs.pop_front();
    }
}

void
SimpleDataflowCPU::attemptAllMemReqs()
{
    DPRINTF(SDCPUCoreEvent, "attemptAllMemReqs()\n");
    assert(!memReqs.empty());

    if (_dataPort.receivedPushback)
        return;

    while (!memReqs.empty()) {
        const MemAccessReq& request = memReqs.front();
        const PacketPtr& packet = request.packet;
        const RequestPtr& req = packet->req;

        if (req->isLLSC() && packet->isWrite()) {
            if (!TheISA::handleLockedWrite(request.tc, req, cacheBlockMask)) {
                panic("Haven't quite decided how to do this yet");
                // TODO kill the request
            }
        }

        // Alpha specific?
        if (req->isMmappedIpr()) {
            // TODO think about limiting access to mmapped IPR
            panic("SDCPU not programmed to understand mmapped IPR!");
            // Cycles delay = TheISA::handleIprRead(tc, pkt);
            // TODO create an event delay cycles later that calls completeAcc()
            //      on the StaticInst.
            return;
        }

        if (!_dataPort.sendTimingReq(request.packet)) {
            DPRINTF(SDCPUCoreEvent, "_dataPort reported busy\n");

            _dataPort.receivedPushback = true;
            return; // Stop trying to send requests until we're called again
                    // in recvReqRetry().
        }

        if (req->isLLSC() && packet->isRead()) {
            TheISA::handleLockedRead(request.tc, req);
        }

        DPRINTF(SDCPUCoreEvent, "Sent a memory request(pa: %#x)\n",
                                req->getPaddr());

        outstandingMemReqs.emplace(packet, request);
        memReqs.pop_front();
    }
}

void
SimpleDataflowCPU::completeExecution(const ExecutionReq& req)
{
    const shared_ptr<ExecContext> ctxt = req.execContext.lock();

    Fault fault = ctxt ?
                  (req.staticInst->isMemRef() ?
                   req.staticInst->initiateAcc(ctxt.get(), req.traceData) :
                   req.staticInst->execute(ctxt.get(), req.traceData)) :
                  NoFault;

    req.callback(fault);
}

void
SimpleDataflowCPU::completeMemAccess(const MemAccessReq& req)
{
    PacketPtr pkt;

    if (req.split) {
        if (req.packet == req.split->low) {
            req.split->lowReceived = true;
        } else if (req.packet == req.split->high) {
            req.split->highReceived = true;
        } else {
            panic("Malformed split MemAccessReq received!");
        }

        if (!(req.split->lowReceived && req.split->highReceived))
            return; // We need to wait for the other to complete as well

        pkt = req.split->main;
        assert(pkt);
        pkt->makeResponse();
    } else {
        pkt = req.packet;
    }

    const shared_ptr<ExecContext> ctxt = req.execContext.lock();

    Fault fault = ctxt ?
        req.staticInst->completeAcc(pkt, ctxt.get(), req.traceData) : NoFault;

    if (req.split) {
        // The packets we actually sent to memory are deleted by recvTimingResp
        delete req.split->main;
        delete req.split;
    }

    req.callback(fault);
}

MasterPort&
SimpleDataflowCPU::getDataPort()
{
    return _dataPort;
}

MasterPort&
SimpleDataflowCPU::getInstPort()
{
    return _instPort;
}

void
SimpleDataflowCPU::init()
{
    BaseCPU::init();

    if (!params()->switched_out &&
        system->getMemoryMode() != Enums::timing)
    {
        fatal("The Dataflow CPU requires the memory system to be in "
              "'timing' mode.\n");
    }

    for (auto& thread : threads) {
        ThreadContext* tc = thread->getThreadContext();
        tc->initMemProxies(tc);

        // TODO if fullsystem, we need to check if we need to call initCPU on
        // the ISA
    }
    // TODO add post-construction initialization code
}

void
SimpleDataflowCPU::requestDataAddrTranslation(const RequestPtr& req,
    ThreadContext* tc, bool write,
    TranslationCallback callback_func)
{
    DPRINTF(SDCPUCoreEvent, "requestDataAddrTranslation()\n");
    // delay if we're out of slots for simultaneous dtb translations

    dataTranslationReqs.push_back({req, tc, write, callback_func});

    if (!attemptAllDataXlationsEvent.scheduled()) {
        // This will call attemptAllDataXlations()
        schedule(&attemptAllDataXlationsEvent,
                 clockedDtbTranslation ? clockEdge() : curTick());
    }
}

void
SimpleDataflowCPU::requestExecution(StaticInstPtr inst,
                                    weak_ptr<ExecContext> context,
                                    Trace::InstRecord* trace_data,
                                    ExecCallback callback_func)
{
    DPRINTF(SDCPUCoreEvent, "requestExecution()\n");

    executionReqs.push_back({inst, context, trace_data, callback_func});

    if (!attemptAllExecutionsEvent.scheduled()) {
        // This will call attemptAllExecutions()
        schedule(&attemptAllExecutionsEvent,
                 clockedExecution ? clockEdge() : curTick());
    }
}

void
SimpleDataflowCPU::requestInstAddrTranslation(const RequestPtr& req,
    ThreadContext* tc,
    TranslationCallback callback_func)
{
    DPRINTF(SDCPUCoreEvent, "requestInstAddrTranslation()\n");
    // delay if we're out of slots for simultaneous itb translations

    instTranslationReqs.push_back({req, tc, false, callback_func});

    if (!attemptAllInstXlationsEvent.scheduled()) {
        // This will call attemptAllInstXlation()
        schedule(&attemptAllInstXlationsEvent,
                 clockedItbTranslation ? clockEdge() : curTick());
    }
}

bool
SimpleDataflowCPU::requestInstructionData(const RequestPtr& req,
    FetchCallback callback_func)
{
    DPRINTF(SDCPUCoreEvent, "requestInstructionData()\n");

    // Allocate some memory for objects we're about to send through the Port
    // system
    PacketPtr pkt = new Packet(req, MemCmd::ReadReq);
    pkt->allocate();

    // Note: pkt is deleted in InstPort::recvTimingResp();

    // Schedule our stuff for sending to the Port
    fetchReqs.push_back({pkt, callback_func});

    if (!attemptAllFetchReqsEvent.scheduled()) {
        // This will call attemptAllFetchReqs()
        schedule(&attemptAllFetchReqsEvent,
                 clockedInstFetch ? clockEdge() : curTick());

    }

    return true;
}

bool
SimpleDataflowCPU::requestMemRead(const RequestPtr& req, ThreadContext* tc,
                                  StaticInstPtr inst,
                                  weak_ptr<ExecContext> context,
                                  Trace::InstRecord* trace_data,
                                  MemCallback callback_func)
{
    DPRINTF(SDCPUCoreEvent, "requestMemRead()\n");

    // Request can be handled immediately
    if (req->getFlags().isSet(Request::NO_ACCESS)) {
        return false; // TODO handle now.
    }

    PacketPtr pkt = Packet::createRead(req);
    pkt->allocate();

    // Note: pkt is deleted in DataPort::recvTimingResp();

    // Alpha specific?
    if (req->isMmappedIpr()) {
        // TODO think about limiting access to mmapped IPR
        panic("SDCPU not programmed to understand mmapped IPR!");
        // Cycles delay = TheISA::handleIprRead(tc, pkt);
        // TODO create an event delay cycles later that calls completeAcc() on
        //      the StaticInst.
        return false;
    }

    memReqs.push_back(
        {pkt, inst, context, trace_data, tc, callback_func, nullptr}
    );

     if (!attemptAllMemReqsEvent.scheduled()) {
        // This will call attemptAllMemReqs()
        schedule(&attemptAllMemReqsEvent,
                 clockedMemoryRequest ? clockEdge() : curTick());
    }

    return true;
}

bool
SimpleDataflowCPU::requestMemWrite(const RequestPtr& req, ThreadContext* tc,
                                   StaticInstPtr inst,
                                   weak_ptr<ExecContext> context,
                                   Trace::InstRecord* trace_data,
                                   uint8_t* data, MemCallback callback_func)
{
    DPRINTF(SDCPUCoreEvent, "requestMemWrite()\n");

    // Request can be handled immediately
    if (req->getFlags().isSet(Request::NO_ACCESS)) {
        return false; // TODO handle now.
    }

    PacketPtr pkt = Packet::createWrite(req);
    // Note: pkt is deleted in DataPort::recvTimingResp();

    if (data) {
        const unsigned req_size = req->getSize();

        pkt->allocate();
        memcpy(pkt->getPtr<uint8_t>(), data, req_size);
    } else { // Assume that no pointer given means we're filling with zero
        assert(req->getFlags() & Request::STORE_NO_DATA);
        // Maybe this line is important? Not all CPUs make this check, so I'm
        // leaving this commented out to see if it causes problems...
        // memset(mailbox, 0, req_size);
        panic("Should be unreachable...");
    }

    // Alpha things?
    if (req->isMmappedIpr()) {
        // TODO think about limiting access to mmapped IPR
        panic("SDCPU not programmed to understand mmapped IPR!");
        // Cycles delay = TheISA::handleIprWrite(tc, pkt);
        // TODO create an event delay cycles later that calls completeAcc() on
        //      the StaticInst.
        return false;
    }

    memReqs.push_back(
        {pkt, inst, context, trace_data, tc, callback_func, nullptr}
    );

    if (!attemptAllMemReqsEvent.scheduled()) {
        // This will call attemptAllMemReqs()
        schedule(&attemptAllMemReqsEvent,
                 clockedMemoryRequest ? clockEdge() : curTick());
    }

    return true;
}

bool
SimpleDataflowCPU::requestSplitMemRead(const RequestPtr& main,
                                       const RequestPtr& low,
                                       const RequestPtr& high,
                                       ThreadContext* tc, StaticInstPtr inst,
                                       weak_ptr<ExecContext> context,
                                       Trace::InstRecord* trace_data,
                                       MemCallback callback_func)
{
    // Request can be handled immediately
    if (main->getFlags().isSet(Request::NO_ACCESS)) {
        return false; // TODO figure out if this is even possible on split
                      //      requests?
    }

    SplitAccCtrlBlk* split_acc = new SplitAccCtrlBlk;

    split_acc->low = Packet::createRead(low);
    split_acc->high = Packet::createRead(high);

    split_acc->main = Packet::createRead(main);
    split_acc->main->allocate();
    split_acc->low->dataStatic(split_acc->main->getPtr<uint8_t>());
    split_acc->high->dataStatic(split_acc->main->getPtr<uint8_t>()
                                + low->getSize());

    memReqs.push_back({split_acc->low, inst, context, trace_data, tc,
                       callback_func, split_acc});
    memReqs.push_back({split_acc->high, inst, context, trace_data, tc,
                       callback_func, split_acc});

    if (!attemptAllMemReqsEvent.scheduled()) {
        // This will call attemptAllMemReqs()
        schedule(&attemptAllMemReqsEvent,
                 clockedMemoryRequest ? clockEdge() : curTick());
    }

    return true;
}

bool
SimpleDataflowCPU::requestSplitMemWrite(const RequestPtr& main,
                                        const RequestPtr& low,
                                        const RequestPtr& high,
                                        ThreadContext* tc, StaticInstPtr inst,
                                        weak_ptr<ExecContext> context,
                                        Trace::InstRecord* trace_data,
                                        uint8_t* data,
                                        MemCallback callback_func)
{
    // Request can be handled immediately
    if (main->getFlags().isSet(Request::NO_ACCESS)) {
        return false; // TODO figure out if this is even possible on split
                      //      requests?
    }

    SplitAccCtrlBlk* split_acc = new SplitAccCtrlBlk;

    // This pointer may be used in completeAcc. It probably doesn't need data.
    // But, we give it data, just to be safe.
    split_acc->main = Packet::createWrite(main);
    split_acc->main->allocate();

    split_acc->low = Packet::createWrite(low);
    split_acc->high = Packet::createWrite(high);

    if (data) {
        split_acc->main->setData(data);
        split_acc->low->dataStatic(data);
        split_acc->high->dataStatic(data + low->getSize());
    } else { // Assume that no pointer given means we're filling with zero
        assert(main->getFlags() & Request::STORE_NO_DATA);
        // Maybe this line is important? Not all CPUs make this check, so I'm
        // leaving this commented out to see if it causes problems...
        // memset(mailbox, 0, req_size);
        panic("Should be unreachable...");
    }

    memReqs.push_back({split_acc->low, inst, context, trace_data, tc,
                       callback_func, split_acc});
    memReqs.push_back({split_acc->high, inst, context, trace_data, tc,
                       callback_func, split_acc});

    if (!attemptAllMemReqsEvent.scheduled()) {
        // This will call attemptAllMemReqs()
        schedule(&attemptAllMemReqsEvent,
                 clockedMemoryRequest ? clockEdge() : curTick());
    }


    return true;
}

void
SimpleDataflowCPU::startup()
{
    BaseCPU::startup();

    for (auto& thread : threads) {
        thread->startup();
    }
    // TODO add code which needs to be run during the first simulation event
}

void
SimpleDataflowCPU::suspendContext(ThreadID tid)
{
    // Need to de-schedule any instructions in the pipeline?
    BaseCPU::suspendContext(tid);
}

void
SimpleDataflowCPU::haltContext(ThreadID tid)
{
    // Need to de-schedule any instructions in the pipeline?
    BaseCPU::haltContext(tid);
}

void
SimpleDataflowCPU::switchOut()
{
    // TODO flush speculative state.
    // TODO clear any outstanding memory requests.

    BaseCPU::switchOut();
}

void
SimpleDataflowCPU::takeOverFrom(BaseCPU* cpu)
{
    // TODO may want to ensure clean speculative state prior to attempting?

    BaseCPU::takeOverFrom(cpu); // BaseCPU::takeOverFrom() already handles the
                                // calls to ThreadContext::takeOverFrom() via
                                // the internal ThreadContext* table.

    // TODO load any other state.
}

Counter
SimpleDataflowCPU::totalInsts() const
{
    Counter insts = 0;
    for (auto &thread : threads) {
        insts += thread->getNumInsts();
    }
    return insts;
}

Counter
SimpleDataflowCPU::totalOps() const
{
    Counter ops = 0;
    for (auto &thread : threads) {
        ops += thread->getNumOps();
    }
    return ops;
}

void
SimpleDataflowCPU::wakeup(ThreadID tid)
{
    assert(tid < numThreads);

    // Get the ThreadContext interface pointer from BaseCPU::getContext(), for
    // flexibility in how the ThreadContext is retrieved (in case we change
    // from using SimpleThread).
    ThreadContext* tc = getContext(tid);
    if (tc->status() == ThreadContext::Suspended) {
        tc->activate();
    }
}

bool
SimpleDataflowCPU::DataPort::recvTimingResp(PacketPtr pkt)
{
    panic_if(!cpu->outstandingMemReqs.count(pkt),
        "Received a data port response we don't remember sending!");

    const MemAccessReq& acc = cpu->outstandingMemReqs[pkt];

    cpu->completeMemAccess(acc);

    cpu->outstandingMemReqs.erase(pkt);
    delete pkt;

    return true;
}

void
SimpleDataflowCPU::DataPort::recvReqRetry()
{
    receivedPushback = false;

    if (!cpu->attemptAllMemReqsEvent.scheduled()) {
        // This will call attemptAllMemReqs()
        cpu->schedule(&cpu->attemptAllMemReqsEvent,
                    cpu->clockedMemoryRequest ? cpu->clockEdge() : curTick());
    }
}

bool
SimpleDataflowCPU::InstPort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(SDCPUCoreEvent, "Received _instPort response (pa: %#x)\n",
                            pkt->req->getPaddr());

    panic_if(!cpu->outstandingFetches.count(pkt),
        "Received an instruction port response we don't remember sending!");

    const FetchCallback& callback = cpu->outstandingFetches[pkt];

    // Right now, this function is assuming that the request function will set
    // up the callback in a way that cleans up after itself. Since this
    // function never calls new, we don't explicitly manage pointer lifespan
    // here.
    callback(pkt->getPtr<uint8_t>());

    cpu->outstandingFetches.erase(pkt);
    delete pkt;

    return true;
}

void
SimpleDataflowCPU::InstPort::recvReqRetry()
{
    receivedPushback = false;

    if (!cpu->attemptAllFetchReqsEvent.scheduled()) {
        // This will call attemptAllFetchReqs()
        cpu->schedule(&cpu->attemptAllFetchReqsEvent,
            cpu->clockedInstFetch ? cpu->clockEdge() : curTick());
    }
}

SimpleDataflowCPU*
SimpleDataflowCPUParams::create()
{
    return new SimpleDataflowCPU(this);
}
