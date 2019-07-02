
/*# Copyright (c) 2019 The Regents of the University of California
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# Authors: Nima Ganjehloo
*/

#ifndef __VERILATOR_DRIVEN_OBJECT_HH__
#define __VERILATOR_DRIVEN_OBJECT_HH__

//verilator design includes
#include "verilator_driver.hh"

//gem5 general includes
#include "params/DrivenObject.hh"
#include "sim/clocked_object.hh"

class DrivenObject : public ClockedObject
{
  private:
    //clocks the verilator device.
    void updateCycle();

    //event queue var to schedule mem requests and cycle updates
    EventFunctionWrapper event;

    //how many cycles to reset the top level of the design
    unsigned int resetCycles;

    //driver for top level design
    VerilatorDriver driver;

  public:
    DrivenObject(DrivenObjectParams *p);

    //resets cpu then schedules memory request to fetch instruction
    void startup() override;
};
#endif