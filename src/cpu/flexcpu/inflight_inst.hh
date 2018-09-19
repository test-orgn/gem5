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

#ifndef __CPU_FLEXCPU_INFLIGHT_INST_HH__
#define __CPU_FLEXCPU_INFLIGHT_INST_HH__

#include <memory>
#include <vector>

#include "arch/isa.hh"
#include "base/addr_range.hh"
#include "cpu/exec_context.hh"
#include "cpu/flexcpu/generic_reg.hh"
#include "cpu/reg_class.hh"
#include "sim/insttracer.hh"

class InflightInst : public ExecContext,
                     public std::enable_shared_from_this<InflightInst>
{
  public:
    using IntReg = TheISA::IntReg;
    using PCState = TheISA::PCState;
    using FloatReg = TheISA::FloatReg;
    using FloatRegBits = TheISA::FloatRegBits;
    using MiscReg = TheISA::MiscReg;

    using CCReg = TheISA::CCReg;
    using VecRegContainer = TheISA::VecRegContainer;
    using VecElem = TheISA::VecElem;

    // Mutually exclusive states through which InflightInsts will transition.
    enum Status
    {
        Invalid = 0,
        Waiting, // Has not been executed yet. Likely has dependencies still.
        // Is a dedicated Ready (dependencies satisfied) but not executed state
        // valuable?
        Executing, // Request for execution sent, but waiting for results.
        EffAddred, // Effective address calculated, but not yet sent to memory.
        Memorying, // Request for memory sent, but waiting for results.
        Complete, // Results have been received, but not yet committed.
        Committed // Values have been committed, but this object might be alive
                  // for a little longer due to the shared_ptr being in the
                  // call-stack.
    };

    struct DataSource
    {
        std::weak_ptr<InflightInst> producer;
        int8_t resultIdx;
    };

    class MemIface
    {
      public:
        virtual Fault readMem(std::shared_ptr<InflightInst> inst, Addr addr,
                              uint8_t *data, unsigned int size,
                              Request::Flags flags)
        { panic("MemIface::readMem() not implemented!"); }

        virtual Fault initiateMemRead(std::shared_ptr<InflightInst> inst,
                                      Addr addr, unsigned int size,
                                      Request::Flags flags)
        { panic("MemIface::initiateMemRead() not implemented!"); }

        virtual Fault writeMem(std::shared_ptr<InflightInst> inst,
                               uint8_t *data, unsigned int size, Addr addr,
                               Request::Flags flags, uint64_t *res)
        { panic("MemIface::writeMem() not implemented!"); }

        static const MemIface unimplemented;
    };

    // Maybe another interface for more nuanced

  protected:
    ThreadContext* backingContext;
    TheISA::ISA* backingISA;
    MemIface* backingMemoryInterface;

    // Where am I in completing this instruction?
    Status _status;
    bool _squashed = false;

    // What is this instruction I'm executing?
    InstSeqNum _seqNum;
    StaticInstPtr instRef;

    /**
     * A storage for the exact PCState at the time of executing this
     * instruction. Any changes to the PC through the instruction can also be
     * captured in this variable, allowing this variable also to serve as the
     * result storage for changes to the PCState. If this variable is changed
     * in this fashion however, we may need to flush speculative state.
     */
    PCState _pcState;

    Trace::InstRecord* _traceData = nullptr;

    // Callbacks made during a state transition.
    std::vector<std::function<void()>> commitCallbacks;
    std::vector<std::function<void()>> completionCallbacks;

    std::vector<std::function<void()>> effAddrCalculatedCallbacks;

    // How many dependencies have to be resolved before I can execute?
    size_t remainingDependencies = 0;

    // How many dependencies have to be resolved before I can go to memory?
    size_t remainingMemDependencies = 0;

    std::vector<std::function<void()>> readyCallbacks;
    std::vector<std::function<void()>> memReadyCallbacks;

    std::vector<std::function<void()>> squashCallbacks;

    std::vector<DataSource> sources;
    std::vector<GenericReg> results;
    std::vector<bool> resultValid;

    /**
     * For use in storing results of writes to miscRegs that don't go through
     * the operands of the StaticInst.
     */
    std::vector<MiscReg> miscResultVals;
    std::vector<int> miscResultIdxs;

    Fault _fault = NoFault;

    // For use in tracking dependencies through memory
    bool accessedPAddrsValid = false;
    AddrRange accessedPAddrs;

    bool _isSplitMemReq = false;
    bool accessedSplitPAddrsValid = false;
    AddrRange accessedSplitPAddrs; // Second variable to store range for second
                                   // request as part of split accesses

    // TODO deal with macroops?
    // TODO deal with memory operations, a la LSQ?

  public:
    InflightInst(ThreadContext* backing_context, TheISA::ISA* backing_isa,
                 MemIface* backing_mem_iface, InstSeqNum seq_num,
                 const TheISA::PCState& pc_,
                 StaticInstPtr inst_ref = StaticInst::nullStaticInstPtr);

    // Unimplemented copy due to presence of raw pointers.
    InflightInst(const InflightInst& other) = delete;

    virtual ~InflightInst();

    /**
     * Tells this InflightInst to call a particular function when it has been
     * made aware of a commit.
     *
     * NOTE: The order in which callbacks are added will determine the order in
     *       which the callbacks are called upon notify. This is a hard promise
     *       made by InflightInst, in order to provide some control on ordering
     *       for any functionality where ordering is important.
     *
     * @param callback The function to call.
     */
    void addCommitCallback(std::function<void()> callback);
    void addCommitDependency(std::shared_ptr<InflightInst> parent);
    void addCompletionCallback(std::function<void()> callback);

    /**
     * If parent has not yet completed and is not squashed, then this will
     * increment an internal counter, which will be decremented when parent
     * completes. When all dependencies have been satisfied (counter reaches
     * 0), notifyReady() will be called.
     *
     * NOTE: This function uses the callback system to implement dependencies,
     *       so if callback order is important, pay attention to the order in
     *       which you add callbacks AND dependencies.
     *
     * @param parent The other instruction whose completion this instruction is
     *  dependent on.
     */
    void addDependency(std::shared_ptr<InflightInst> parent);

    void addEffAddrCalculatedCallback(std::function<void()> callback);
    void addMemReadyCallback(std::function<void()> callback);
    void addMemCommitDependency(std::shared_ptr<InflightInst> parent);
    void addMemDependency(std::shared_ptr<InflightInst> parent);
    void addMemEffAddrDependency(std::shared_ptr<InflightInst> parent);
    void addReadyCallback(std::function<void()> callback);
    void addSquashCallback(std::function<void()> callback);
    // May be useful to add ability to remove a callback. Will be difficult if
    // we use raw function objects, could be resolved by holding pointers?

    /**
     * Takes all state-changing effects that this InflightInst has, and applies
     * them to the backing ThreadContext of this InflightInst.
     */
    void commitToTC();

    bool effAddrOverlap(const InflightInst& other) const;

    inline void effPAddrRange(AddrRange range)
    {
        assert(range.valid());
        accessedPAddrsValid = true;
        accessedPAddrs = range;
    }
    inline void effPAddrRangeHigh(AddrRange range)
    {
        assert(_isSplitMemReq && range.valid());
        accessedSplitPAddrsValid = true;
        accessedSplitPAddrs = range;
    }

    inline const Fault& fault() const
    { return _fault; }
    inline const Fault& fault(const Fault& f)
    { return _fault = f; }

    /**
     * Accessor function to retrieve a result produced by this instruction.
     * Non-const variant returns a reference, so that external classes can make
     * modifications if they need to.
     */
    inline const GenericReg& getResult(int8_t result_idx) const
    {
        panic_if(!resultValid[result_idx], "Tried to access invalid result!");
        return results[result_idx];
    }
    inline GenericReg& getResult(int8_t result_idx)
    {
        panic_if(!resultValid[result_idx], "Tried to access invalid result!");
        return results[result_idx];
    }

    // Note: due to the sequential nature of the status items, a later status
    //       naturally implies earlier statuses have been met. (e.g. A complete
    //       memory instruction must have already had its effective address
    //       calculated)
    inline bool isCommitted() const
    { return status() >= Committed; }

    inline bool isComplete() const
    { return status() >= Complete; }

    inline bool isEffAddred() const
    { return status() >= EffAddred; }

    inline bool isFaulted() const
    { return fault() != NoFault; }

    inline bool isMemReady()
    { return remainingMemDependencies == 0; }

    inline bool isReady() const
    { return remainingDependencies == 0; }

    inline bool isSplitMemReq() const
    { return _isSplitMemReq; }
    inline bool isSplitMemReq(bool split)
    { return _isSplitMemReq = split; }

    inline bool isSquashed() const
    { return _squashed; }

    /**
     * Notify all registered commit callback listeners that this in-flight
     * instruction has been committed.
     *
     * NOTE: The responsibility for calling this function falls with whoever
     *       is managing commit of this instruction.
     */
    void notifyCommitted();

    /**
     * Notify all registered completion callback listeners that this in-flight
     * instruction has completed execution (is ready to commit).
     *
     * NOTE: The responsibility for calling this function falls with whoever
     *       is managing execution of this instruction.
     */
    void notifyComplete();

    /**
     * Notify all registered effective address callback listeners that this
     * in-flight instruction has had its effective address(es) calculated.
     *
     * NOTE: The responsibility for calling this function falls with whoever
     *       is calculating the effective address of this MEMORY instruction.
     */
    void notifyEffAddrCalculated();

    /**
     * Notify all registered memory ready callback listeners that this
     * in-flight instruction is ready to send to memory.
     *
     * NOTE: The responsibility for calling this function falls with this
     *       class' internal dependency system.
     */
    void notifyMemReady();

    /**
     * Notify all registered ready callback listeners that this in-flight
     * instruction is ready to execute (has no more outstanding dependencies).
     *
     * NOTE: The responsibility for calling this function falls with this
     *       class' internal dependency system.
     */
    void notifyReady();

    /**
     * Notify all registered squash callback listeners that this in-flight
     * instruction has been squashed.
     *
     * NOTE: The responsibility for calling this function falls with whoever
     *       is managing squashing of this instruction.
     */
    void notifySquashed();

    /**
     * Set this InflightInst to grab data for its execution from a particular
     * producer. Used to attach data dependencies in a way that can feed data
     * from a ROB or equivalent structure. Note that this function will not
     * actually add the dependency, and that guaranteeing that the producer has
     * completed execution when read should be done through a call to
     * addDependency().
     *
     * @param src_idx Which source of this instruction to set
     * @param producer What instruction will produce the value I need
     * @param res_idx Which of the items the producing instruction produces
     *  that I want
     */
    void setDataSource(int8_t src_idx, DataSource source);

    /**
     * Ask the InflightInst what its current sequence number is
     */
    inline InstSeqNum seqNum() const
    { return _seqNum; }

    /**
     * Set the sequence number of the InflightInst
     */
    inline InstSeqNum seqNum(InstSeqNum seqNum)
    { return _seqNum = seqNum; }

    inline const StaticInstPtr& staticInst() const
    { return instRef; }
    const StaticInstPtr& staticInst(const StaticInstPtr& inst_ref);

    /**
     * Ask the InflightInst what its current status is
     */
    inline Status status() const
    { return _status; }

    /**
     * Set the status of the InflightInst
     */
    inline Status status(Status status)
    {
        assert(status > _status); // Only allow forward state transitions
        return _status = status;
    }

    inline Trace::InstRecord* traceData() const
    { return _traceData; }
    inline Trace::InstRecord* traceData(Trace::InstRecord* const trace_data)
    { return _traceData = trace_data; }


    // BEGIN ExecContext interface functions

    IntReg readIntRegOperand(const StaticInst* si, int op_idx) override;
    void setIntRegOperand(const StaticInst* si, int dst_idx,
                          IntReg val) override;

    FloatReg readFloatRegOperand(const StaticInst* si, int op_idx) override;
    FloatRegBits readFloatRegOperandBits(const StaticInst* si,
                                            int op_idx) override;

    void setFloatRegOperand(const StaticInst* si, int dst_idx,
                            FloatReg val) override;
    void setFloatRegOperandBits(const StaticInst* si, int dst_idx,
                                FloatRegBits val) override;


    const VecRegContainer&
    readVecRegOperand(const StaticInst* si, int op_idx) const override;

    VecRegContainer&
    getWritableVecRegOperand(const StaticInst* si, int op_idx) override;

    void setVecRegOperand(const StaticInst* si, int dst_idx,
                          const VecRegContainer& val) override;


    ConstVecLane8
    readVec8BitLaneOperand(const StaticInst* si, int op_idx) const override;

    ConstVecLane16
    readVec16BitLaneOperand(const StaticInst* si, int op_idx) const override;

    ConstVecLane32
    readVec32BitLaneOperand(const StaticInst* si, int op_idx) const override;

    ConstVecLane64
    readVec64BitLaneOperand(const StaticInst* si, int op_idx) const override;

    void setVecLaneOperand(const StaticInst* si, int dst_idx,
                           const LaneData<LaneSize::Byte>& val) override;
    void setVecLaneOperand(const StaticInst* si, int dst_idx,
                           const LaneData<LaneSize::TwoByte>& val) override;
    void setVecLaneOperand(const StaticInst* si, int dst_idx,
                           const LaneData<LaneSize::FourByte>& val) override;
    void setVecLaneOperand(const StaticInst* si, int dst_idx,
                           const LaneData<LaneSize::EightByte>& val) override;

    VecElem readVecElemOperand(const StaticInst* si,
                               int op_idx) const override;
    void setVecElemOperand(const StaticInst* si, int dst_idx,
                           const VecElem val) override;

    CCReg readCCRegOperand(const StaticInst* si, int op_idx) override;
    void setCCRegOperand(const StaticInst* si, int dst_idx, CCReg val)
                         override;

    MiscReg readMiscRegOperand(const StaticInst* si, int op_idx) override;
    void setMiscRegOperand(const StaticInst* si, int dst_idx,
                           const MiscReg& val) override;

    MiscReg readMiscReg(int misc_reg) override;
    void setMiscReg(int misc_reg, const MiscReg& val) override;

    PCState pcState() const override;
    void pcState(const PCState& val) override;


    Fault readMem(Addr addr, uint8_t *data, unsigned int size,
                  Request::Flags flags) override;

    Fault initiateMemRead(Addr addr, unsigned int size, Request::Flags flags)
                          override;

    Fault writeMem(uint8_t* data, unsigned int size, Addr addr,
                   Request::Flags flags, uint64_t* res) override;


    void setStCondFailures(unsigned int sc_failures) override;

    unsigned int readStCondFailures() const override;

    void syscall(int64_t callnum, Fault* fault) override;

    ThreadContext* tcBase() override;

    /**
     * Alpha-Specific
     */
    Fault hwrei() override;
    bool simPalCheck(int palFunc) override;

    /**
     * ARM-specific
     */
    bool readPredicate() override;
    void setPredicate(bool val) override;

    /**
     * X86-specific
     */
    void demapPage(Addr vaddr, uint64_t asn) override;
    void armMonitor(Addr address) override;
    bool mwait(PacketPtr pkt) override;
    void mwaitAtomic(ThreadContext* tc) override;
    AddressMonitor* getAddrMonitor() override;

    /**
     * MIPS-Specific
     */
#if THE_ISA == MIPS_ISA
    MiscReg readRegOtherThread(const RegId& reg,
                               ThreadID tid = InvalidThreadID) override;
    void setRegOtherThread(const RegId& reg, MiscReg val,
                           ThreadID tid = InvalidThreadID) override;
#endif

    // END ExecContext interface functions
}; // END class InflightInst

#endif // __CPU_FLEXCPU_INFLIGHT_INST_HH__