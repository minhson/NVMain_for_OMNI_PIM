/*
 * Copyright (c) 2012-2014 Pennsylvania State University
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
 */

/*
 *  This file is part of NVMain- A cycle accurate timing, bit-accurate
 *  energy simulator for non-volatile memory. Originally developed by
 *  Matt Poremba at the Pennsylvania State University.
 *
 *  Website: http://www.cse.psu.edu/~poremba/nvmain/
 *  Email: mrp5060@psu.edu
 *
 *  ---------------------------------------------------------------------
 *
 *  If you use this software for publishable research, please include
 *  the original NVMain paper in the citation list and mention the use
 *  of NVMain.
 *
 */

#include "SimInterface/Gem5Interface/Gem5Interface.h"
#include "Simulators/gem5/nvmain_mem.hh"
#include "Utils/HookFactory.h"

#include "base/random.hh"
#include "base/statistics.hh"
#include "base/trace.hh"
#include "sim/sim_object.hh"
#include "debug/NVMain.hh"
#include "debug/NVMainMin.hh"
#include "config/the_isa.hh"
#include "debug/PIM.hh"

using namespace NVM;

// This members are singleton values used to hold the main instance of
// NVMain and it's wake/sleep (i.e., timing/atomic) status. These are
// needed since NVMain assumes a contiguous address range while gem5
// ISAs generally do not. The multiple instances allow for the gem5
// AddrRanges to be used normally while this class remapped to NVMains
// contiguous region.
NVMainMemory *NVMainMemory::masterInstance = NULL;

NVMainMemory::NVMainMemory(const Params &p)
    : NVMInterface(p), clockEvent(this), respondEvent(this),
      drainManager(NULL), lat(p.atomic_latency),
      lat_var(p.atomic_variance), nvmain_atomic(p.atomic_mode),
      NVMainWarmUp(p.NVMainWarmUp), port(name() + ".port", *this)
{
    std::cout << "And I've been called!" << std::endl;
    char *cfgparams;
    char *cfgvalues;
    char *cparam, *cvalue;

    char *saveptr1, *saveptr2;

    nextEventCycle = 0;

    m_nvmainPtr = NULL;
    m_nacked_requests = false;

    m_nvmainConfigPath = p.config;

    m_nvmainConfig = new Config( );

    m_nvmainConfig->Read( m_nvmainConfigPath );
    std::cout << "NVMainControl: Reading NVMain config file: " << m_nvmainConfigPath << "." << std::endl;

    clock = clockPeriod( );

    m_avgAtomicLatency = 100.0f;
    m_numAtomicAccesses = 0;

    retryRead = false;
    retryWrite = false;
    retryResp = false;
    m_requests_outstanding = 0;

    /*
     * Modified by Tao @ 01/22/2013
     * multiple parameters can be manually specified
     * please separate the parameters by comma ","
     * For example,
     *    configparams = tRCD,tCAS,tRP
     *    configvalues = 8,8,8
     */
    cfgparams = (char *)p.configparams.c_str();
    cfgvalues = (char *)p.configvalues.c_str();

    for( cparam = strtok_r( cfgparams, ",", &saveptr1 ), cvalue = strtok_r( cfgvalues, ",", &saveptr2 )
           ; (cparam && cvalue) ; cparam = strtok_r( NULL, ",", &saveptr1 ), cvalue = strtok_r( NULL, ",", &saveptr2) )
    {
        std::cout << "NVMain: Overriding parameter `" << cparam << "' with `" << cvalue << "'" << std::endl;
        m_nvmainConfig->SetValue( cparam, cvalue );
    }

   BusWidth = m_nvmainConfig->GetValue( "BusWidth" );
   tBURST = m_nvmainConfig->GetValue( "tBURST" );
   RATE = m_nvmainConfig->GetValue( "RATE" );

   lastWakeup = gem5::curTick();
}


NVMainMemory::~NVMainMemory()
{
    std::cout << "NVMain dtor called" << std::endl;
}


void
NVMainMemory::init()
{
    // if (!port.isConnected()) {
    //     fatal("NVMainMemory %s is unconnected!\n", name());
    // } else {
    //     port.sendRangeChange();
    // }

    if( masterInstance == NULL )
    {
        masterInstance = this;

        m_nvmainPtr = new NVM::NVMain( );
        m_statsPtr = new NVM::Stats( );
        m_nvmainSimInterface = new NVM::Gem5Interface( );
        m_nvmainEventQueue = new NVM::EventQueue( );
        m_nvmainGlobalEventQueue = new NVM::GlobalEventQueue( );
        m_tagGenerator = new NVM::TagGenerator( 1000 );

        m_nvmainConfig->SetSimInterface( m_nvmainSimInterface );

        statPrinter.nvmainPtr = m_nvmainPtr;
        statReseter.nvmainPtr = m_nvmainPtr;

        if( m_nvmainConfig->KeyExists( "StatsFile" ) )
        {
            statPrinter.statStream.open( m_nvmainConfig->GetString( "StatsFile" ).c_str(),
                                         std::ofstream::out | std::ofstream::app );
        }

        statPrinter.memory = this;
        statPrinter.forgdb = this;

        //registerExitCallback( &statPrinter );
        // ::Stats::registerDumpCallback( &statPrinter ); TODO: format statPrinter as std::function<void>&
        // ::Stats::registerResetCallback( &statReseter ); TODO format statReseter as std::function<void>&
        
        gem5::statistics::registerDumpCallback([this]() { statPrinter.process(); });
        gem5::statistics::registerResetCallback([this]() { statReseter.process(); });

        SetEventQueue( m_nvmainEventQueue );
        SetStats( m_statsPtr );
        SetTagGenerator( m_tagGenerator );

        m_nvmainGlobalEventQueue->SetFrequency( m_nvmainConfig->GetEnergy( "CPUFreq" ) * 1000000.0 );
        SetGlobalEventQueue( m_nvmainGlobalEventQueue );

        // TODO: Confirm global event queue frequency is the same as this SimObject's clock.

        /*  Add any specified hooks */
        std::vector<std::string>& hookList = m_nvmainConfig->GetHooks( );

        DPRINTF(NVMain, "Creating hook\n");
        DPRINTF(NVMain, "hookList size: %d\n", hookList.size( ));
        for( size_t i = 0; i < hookList.size( ); i++ )
        {
            DPRINTF(NVMain, "Creating hook %s\n", hookList[i]);
            std::cout << "Creating hook " << hookList[i] << std::endl;

            NVMObject *hook = HookFactory::CreateHook( hookList[i] );

            if( hook != NULL )
            {
                AddHook( hook );
                hook->SetParent( this );
                hook->Init( m_nvmainConfig );
            }
            else
            {
                DPRINTF(NVMain, "Warning: Could not create a hook named `%s'.\n", hookList[i]);
                std::cout << "Warning: Could not create a hook named `"
                    << hookList[i] << "'." << std::endl;
            }
        }

        /* Setup child and parent modules. */
        AddChild( m_nvmainPtr );
        m_nvmainPtr->SetParent( this );
        m_nvmainGlobalEventQueue->AddSystem( m_nvmainPtr, m_nvmainConfig );
        m_nvmainPtr->SetConfig( m_nvmainConfig );

        masterInstance->allInstances.push_back(this);
    }
    else
    {
        masterInstance->allInstances.push_back(this);
        masterInstance->otherInstance = this;
    }
}


void NVMainMemory::startup()
{
    DPRINTF(NVMain, "NVMainMemory: startup() called.\n");
    DPRINTF(NVMainMin, "NVMainMemory: startup() called.\n");

    /*
     *  Schedule the initial event. Needed for warmup and timing mode.
     *  If we are in atomic/fast-forward, wakeup will be disabled upon
     *  the first atomic request receieved in recvAtomic().
     */
    if (!masterInstance->clockEvent.scheduled())
        schedule(masterInstance->clockEvent, gem5::curTick() + clock);

    lastWakeup = gem5::curTick();
}


void NVMainMemory::wakeup()
{
    DPRINTF(NVMain, "NVMainMemory: wakeup() called.\n");
    DPRINTF(NVMainMin, "NVMainMemory: wakeup() called.\n");

    schedule(masterInstance->clockEvent, clockEdge());

    lastWakeup = gem5::curTick();
}


gem5::Port &
NVMainMemory::getPort(const std::string& if_name, gem5::PortID idx)
{
    if (if_name != "port") {
        return AbstractMemory::getPort(if_name, idx);
    } else {
        return port;
    }
}


void NVMainMemory::NVMainStatPrinter::process()
{
    assert(nvmainPtr != NULL);

    assert(gem5::curTick() >= memory->lastWakeup);
    gem5::Tick stepCycles = (gem5::curTick() - memory->lastWakeup) / memory->clock;

    memory->m_nvmainGlobalEventQueue->Cycle( stepCycles );

    nvmainPtr->CalculateStats();
    std::ostream& refStream = (statStream.is_open()) ? statStream : std::cout;
    nvmainPtr->GetStats()->PrintAll( refStream );
}


void NVMainMemory::NVMainStatReseter::process()
{
    assert(nvmainPtr != NULL);

    nvmainPtr->ResetStats();
    nvmainPtr->GetStats()->ResetAll( );
}


NVMainMemory::MemoryPort::MemoryPort(const std::string& _name, NVMainMemory& _memory)
    : ResponsePort(_name, &_memory), memory(_memory), forgdb(_memory)
{

}


gem5::AddrRangeList NVMainMemory::MemoryPort::getAddrRanges() const
{
    gem5::AddrRangeList ranges;
    ranges.push_back(memory.getAddrRange());
    return ranges;
}


void
NVMainMemory::SetRequestData(NVMainRequest *request, gem5::PacketPtr pkt)
{
    uint8_t *hostAddr;

    request->data.SetSize( pkt->getSize() );
    request->oldData.SetSize( pkt->getSize() );

    if (pkt->isRead() || pkt->isPIM())
    {
        gem5::RequestPtr dataReq = std::make_shared<gem5::Request>(pkt->getAddr(), pkt->getSize(), 0, gem5::Request::funcRequestorId);
        gem5::PacketPtr dataPkt = new gem5::Packet(dataReq, gem5::MemCmd::ReadReq);
        dataPkt->allocate();
        doFunctionalAccess(dataPkt);

        hostAddr = new uint8_t[ pkt->getSize() ];
        memcpy( hostAddr, dataPkt->getPtr<uint8_t>(), pkt->getSize() );

        for(int i = 0; i < pkt->getSize(); i++ )
        {
            request->oldData.SetByte(i, *(hostAddr + i));
            request->data.SetByte(i, *(hostAddr + i));
        }

        delete dataPkt;
        delete &dataReq;
        delete [] hostAddr;
    }
    else
    {
        gem5::RequestPtr dataReq = std::make_shared<gem5::Request>(pkt->getAddr(), pkt->getSize(), 0, gem5::Request::funcRequestorId);
        gem5::PacketPtr dataPkt = new gem5::Packet(dataReq, gem5::MemCmd::ReadReq);
        dataPkt->allocate();
        doFunctionalAccess(dataPkt);

        uint8_t *hostAddrT = new uint8_t[ pkt->getSize() ];
        memcpy( hostAddrT, dataPkt->getPtr<uint8_t>(), pkt->getSize() );

        hostAddr = new uint8_t[ pkt->getSize() ];
        memcpy( hostAddr, pkt->getPtr<uint8_t>(), pkt->getSize() );

        for(int i = 0; i < pkt->getSize(); i++ )
        {
            request->oldData.SetByte(i, *(hostAddrT + i));
            request->data.SetByte(i, *(hostAddr + i));
        }

        delete dataPkt;
        delete &dataReq;
        delete [] hostAddrT;
        delete [] hostAddr;
    }
}


gem5::Tick
NVMainMemory::MemoryPort::recvAtomic(gem5::PacketPtr pkt)
{
    if (pkt->cacheResponding())
        return 0;

    /*
     * calculate the latency. Now it is only random number
     */
    gem5::Tick latency = memory.lat;

    if (memory.lat_var != 0)
        latency += gem5::random_mt.random<gem5::Tick>(0, memory.lat_var);

    /*
     *  if NVMain also needs the packet to warm up the inline cache, create the request
     */
    if( memory.NVMainWarmUp )
    {
        NVMainRequest *request = new NVMainRequest( );

        memory.SetRequestData( request, pkt );

        if( !pkt->isRead() && !pkt->isWrite() )
        {
            // if it is neither read nor write, just return
            // well, speed may suffer a little bit...
            return latency;
        }

        /* initialize the request so that NVMain can correctly serve it */
        request->access = UNKNOWN_ACCESS;
        request->address.SetPhysicalAddress(pkt->req->getPaddr());
        request->status = MEM_REQUEST_INCOMPLETE;
        request->type = (pkt->isRead()) ? READ : WRITE;
        request->owner = (NVMObject *)&memory;
        if(pkt->req->hasPC()) request->programCounter = pkt->req->getPC();
        if(pkt->req->hasContextId()) request->threadId = pkt->req->contextId();

        /*
         * Issue the request to NVMain as an atomic request
         */
        memory.masterInstance->m_nvmainPtr->IssueAtomic(request);

        delete request;
    }

    /*
     * do the memory access to get the read data and change the response tag
     */
    memory.access(pkt);

    return latency;
}


void
NVMainMemory::MemoryPort::recvFunctional(gem5::PacketPtr pkt)
{
    pkt->pushLabel(memory.name());

    memory.doFunctionalAccess(pkt);

    for( std::deque<gem5::PacketPtr>::iterator i = memory.responseQueue.begin();
         i != memory.responseQueue.end(); ++i )
        pkt->trySatisfyFunctional((gem5::PacketPtr) (*i));

    pkt->popLabel();
}


bool
NVMainMemory::MemoryPort::recvTimingReq(gem5::PacketPtr pkt)
{
    /* added by Tao @ 01/24/2013, just copy the code from SimpleMemory */
    /// @todo temporary hack to deal with memory corruption issues until
    /// 4-phase transactions are complete
    for (int x = 0; x < memory.pendingDelete.size(); x++)
        delete memory.pendingDelete[x];
    memory.pendingDelete.clear();

    if (pkt->cacheResponding()) {
        memory.pendingDelete.push_back(pkt);
        return true;
    }

    if (!pkt->isRead() && !pkt->isWrite()) {
        DPRINTF(NVMain, "NVMainMemory: Received a packet that is neither read nor write.\n");
        DPRINTF(NVMainMin, "NVMainMemory: Received a packet that is neither read nor write.\n");

        bool needsResponse = pkt->needsResponse();

        memory.access(pkt);
        if (needsResponse) {
            assert(pkt->isResponse());

            pkt->headerDelay = pkt->payloadDelay = 0;

            memory.responseQueue.push_back(pkt);

            memory.ScheduleResponse( );
        } else {
            memory.pendingDelete.push_back(pkt);
        }

        return true;
    }


    if (memory.retryRead || memory.retryWrite)
    {
        DPRINTF(NVMain, "nvmain_mem.cc: Received request while waiting for retry!\n");
        DPRINTF(NVMainMin, "nvmain_mem.cc: Received request while waiting for retry!\n");
        return false;
    }

    // Bus latency is modeled in NVMain.
    pkt->headerDelay = pkt->payloadDelay = 0;

    NVMainRequest *request = new NVMainRequest( );

    bool canQueue, enqueued = false;
    
    memory.SetRequestData( request, pkt );
    /*
     *  NVMain expects linear addresses, so hack: If we are not the master
     *  instance, assume there are two channels because 3GB-4GB is skipped
     *  in X86 and subtract 1GB.
     *
     *  TODO: Have each channel communicate it's address range to determine
     *  this fix up value.
     */
    uint64_t addressFixUp = 0;
//FIXME#if THE_ISA == X86_ISA
    if( masterInstance != &memory )
    {
        addressFixUp = 0x40000000;
    }
//FIXME#elif THE_ISA == ARM_ISA
//FIXME    /* 
//FIXME     *  ARM regions are 2GB - 4GB followed by 34 GB - 64 GB. Work for up to
//FIXME     *  34 GB of memory. Further regions from 512 GB - 992 GB.
//FIXME     */
//FIXME    addressFixUp = (masterInstance == &memory) ? 0x80000000 : 0x800000000;
//FIXME#endif
    
    //PIM will be here
    if (pkt->isPIM()) 
    {
        DPRINTF(PIM, "Received PIM pkt in NVMain!\n");
        std::cout << "Received PIM pkt in NVMain!" << std::endl;
        //FIXMEPacket::PIMSenderState* senderState = dynamic_cast<Packet::PIMSenderState*>(pkt->senderState);
        gem5::Packet::PIMSenderState* senderState = dynamic_cast<gem5::Packet::PIMSenderState*>(pkt->findNextSenderState<gem5::Packet::PIMSenderState>());
        assert(senderState);
        
        request->access = UNKNOWN_ACCESS;
        request->addr_src1.SetPhysicalAddress(senderState->addr[0] - addressFixUp);
        request->addr_src2.SetPhysicalAddress(senderState->addr[1] - addressFixUp);
        request->addr_dst.SetPhysicalAddress(senderState->addr[2] - addressFixUp);
        request->pim_size = senderState->size;
        request->address.SetPhysicalAddress(senderState->addr[2] - addressFixUp);
        request->status = MEM_REQUEST_INCOMPLETE;
        request->type = READ;
        request->isPIM = true;
        int burstCount = (512 * 8) / 512;
        request->burstCount = burstCount;
        std::cout << "burstCount: " << request->burstCount << std::endl;

        if (pkt->cmd == gem5::MemCmd::PIMANDArray)
            request->isPIMANDArray = true;
        else if (pkt->cmd == gem5::MemCmd::PIMANDLatch)
            request->isPIMANDLatch = true;
        else if (pkt->cmd == gem5::MemCmd::PIMORLatch)
            request->isPIMORLatch = true;
        else
            request->isPIMXORLatch = true;
        request->owner = (NVMObject *)&memory;
        
        if(pkt->req->hasPC()) request->programCounter = pkt->req->getPC();
        if(pkt->req->hasContextId()) request->threadId = pkt->req->contextId();

        /* Call hooks here manually, since there is no one else to do it. */
        std::vector<NVMObject *>& preHooks  = memory.masterInstance->GetHooks( NVMHOOK_PREISSUE );
        std::vector<NVMObject *>& postHooks = memory.masterInstance->GetHooks( NVMHOOK_POSTISSUE );
        std::vector<NVMObject *>::iterator it;

        canQueue = memory.masterInstance->GetChild( )->IsIssuable( request );
        
        if (canQueue) {
            /* Call the pre-issue hooks */
            for( it = preHooks.begin(); it != preHooks.end(); it++ ) {
                (*it)->SetParent( memory.masterInstance );
                (*it)->IssueCommand( request );
            }
            
            enqueued = memory.masterInstance->GetChild( )->IssueCommand(request);
            assert( enqueued == true );
        
            NVMainMemoryRequest *memRequest = new NVMainMemoryRequest;
       
            memRequest->request = request;
            memRequest->packet = pkt;
            memRequest->issueTick = gem5::curTick();
            memRequest->atomic = false;

            DPRINTF(NVMain, "nvmain_mem.cc: Enqueued Mem request for 0x%x of type %s\n", request->address.GetPhysicalAddress( ), "PIM" );

            /* See if we need to reschedule the wakeup event sooner. */
            ncycle_t nextEvent = memory.masterInstance->m_nvmainGlobalEventQueue->GetNextEvent(NULL);
            DPRINTF(NVMain, "NVMainMemory: Next event after issue is %d\n", nextEvent);
            if( nextEvent < memory.nextEventCycle && masterInstance->clockEvent.scheduled() ) {
                ncycle_t currentCycle = memory.masterInstance->m_nvmainGlobalEventQueue->GetCurrentCycle();
                
                //assert(nextEvent >= currentCycle);
                ncycle_t stepCycles;
                if( nextEvent > currentCycle )
                    stepCycles = nextEvent - currentCycle;
                else
                    stepCycles = 1;

                gem5::Tick nextWake = gem5::curTick() + memory.clock * static_cast<gem5::Tick>(stepCycles);

                DPRINTF(NVMain, "NVMainMemory: Next event: %d CurrentCycle: %d\n", nextEvent, currentCycle);
                DPRINTF(NVMain, "NVMainMemory: Rescheduled wake at %d after %d cycles\n", nextWake, stepCycles);

                memory.nextEventCycle = nextEvent;
                memory.ScheduleClockEvent( nextWake );
            } else if( !masterInstance->clockEvent.scheduled() ) {
                ncycle_t currentCycle = memory.masterInstance->m_nvmainGlobalEventQueue->GetCurrentCycle();

                //assert(nextEvent >= currentCycle);
                ncycle_t stepCycles = nextEvent - currentCycle;
                if( stepCycles == 0 || nextEvent < currentCycle )
                    stepCycles = 1;

                gem5::Tick nextWake = gem5::curTick() + memory.clock * static_cast<gem5::Tick>(stepCycles);

                memory.nextEventCycle = nextEvent;
                memory.ScheduleClockEvent( nextWake );
            }
            
            memory.masterInstance->m_request_map.insert( std::pair<NVMainRequest *, NVMainMemoryRequest *>( request, memRequest ) );
            memory.m_requests_outstanding++;

            /*
             *  It seems gem5 will block until the packet gets a response, so create a copy of the request, so
             *  the memory controller has it, then delete the original copy to respond to the packet.
             */
            // for write request type only 
            // comment out if read request type is considerd for PIM instruction
            //FIXMENVMainMemoryRequest *requestCopy = new NVMainMemoryRequest( );

            //FIXMErequestCopy->request = new NVMainRequest( );
            //FIXME*(requestCopy->request) = *request;
            //FIXMErequestCopy->packet = pkt;
            //FIXMErequestCopy->issueTick = gem5::curTick();
            //FIXMErequestCopy->atomic = false;

            //FIXMEmemRequest->packet = NULL;

            //FIXMEmemory.masterInstance->m_request_map.insert( std::pair<NVMainRequest *, NVMainMemoryRequest *>( requestCopy->request, requestCopy ) );
            //FIXMEmemory.m_requests_outstanding++;

            //FIXMEmemory.RequestComplete( requestCopy->request );
           
            /* Call post-issue hooks. */
            if( request != NULL ) {
                for( it = postHooks.begin(); it != postHooks.end(); it++ ) {
                    (*it)->SetParent( &memory );
                    (*it)->IssueCommand( request );
                }
            }
        
        } else {
            DPRINTF(NVMain, "nvmain_mem.cc: Can not enqueue Mem request for 0x%x of type %s\n", request->address.GetPhysicalAddress( ), "PIM" );
            DPRINTF(NVMainMin, "nvmain_mem.cc: Can not enqueue Mem request for 0x%x of type %s\n", request->address.GetPhysicalAddress( ), "PIM" );

            if (pkt->isRead()) {
                memory.retryRead = true;
            }
            else {
                memory.retryWrite = true;
            }

            delete request;
            request = NULL;
        }

    }
    //PIM will be here
    
    else if (!pkt->isPIM()) 
    {
        // packet size of baseline cpu's packet is 64 byte
        //std::cout << "Size of packet: " << pkt->getSize() << std::endl;
        
        request->access = UNKNOWN_ACCESS;
        request->address.SetPhysicalAddress(pkt->req->getPaddr() - addressFixUp);
        request->status = MEM_REQUEST_INCOMPLETE;
        request->type = (pkt->isRead()) ? READ : WRITE;
        request->owner = (NVMObject *)&memory;

        if(pkt->req->hasPC()) request->programCounter = pkt->req->getPC();
        if(pkt->req->hasContextId()) request->threadId = pkt->req->contextId();

        /* Call hooks here manually, since there is no one else to do it. */
        std::vector<NVMObject *>& preHooks  = memory.masterInstance->GetHooks( NVMHOOK_PREISSUE );
        std::vector<NVMObject *>& postHooks = memory.masterInstance->GetHooks( NVMHOOK_POSTISSUE );
        std::vector<NVMObject *>::iterator it;

        canQueue = memory.masterInstance->GetChild( )->IsIssuable( request );

        if( canQueue )
        {
            /* Call pre-issue hooks */
            for( it = preHooks.begin(); it != preHooks.end(); it++ )
            {
                (*it)->SetParent( memory.masterInstance );
                (*it)->IssueCommand( request );
            }

            enqueued = memory.masterInstance->GetChild( )->IssueCommand(request);
            assert( enqueued == true );

            NVMainMemoryRequest *memRequest = new NVMainMemoryRequest;

            memRequest->request = request;
            memRequest->packet = pkt;
            memRequest->issueTick = gem5::curTick();
            memRequest->atomic = false;

            DPRINTF(NVMain, "nvmain_mem.cc: Enqueued Mem request for 0x%x of type %s\n", request->address.GetPhysicalAddress( ), ((pkt->isRead()) ? "READ" : "WRITE") );

            /* See if we need to reschedule the wakeup event sooner. */
            ncycle_t nextEvent = memory.masterInstance->m_nvmainGlobalEventQueue->GetNextEvent(NULL);
            DPRINTF(NVMain, "NVMainMemory: Next event after issue is %d\n", nextEvent);
            if( nextEvent < memory.nextEventCycle && masterInstance->clockEvent.scheduled() )
            {
                ncycle_t currentCycle = memory.masterInstance->m_nvmainGlobalEventQueue->GetCurrentCycle();

                //assert(nextEvent >= currentCycle);
                ncycle_t stepCycles;
                if( nextEvent > currentCycle )
                    stepCycles = nextEvent - currentCycle;
                else
                    stepCycles = 1;

                gem5::Tick nextWake = gem5::curTick() + memory.clock * static_cast<gem5::Tick>(stepCycles);

                DPRINTF(NVMain, "NVMainMemory: Next event: %d CurrentCycle: %d\n", nextEvent, currentCycle);
                DPRINTF(NVMain, "NVMainMemory: Rescheduled wake at %d after %d cycles\n", nextWake, stepCycles);

                memory.nextEventCycle = nextEvent;
                memory.ScheduleClockEvent( nextWake );
            }
            else if( !masterInstance->clockEvent.scheduled() )
            {
                ncycle_t currentCycle = memory.masterInstance->m_nvmainGlobalEventQueue->GetCurrentCycle();

                //assert(nextEvent >= currentCycle);
                ncycle_t stepCycles = nextEvent - currentCycle;
                if( stepCycles == 0 || nextEvent < currentCycle )
                    stepCycles = 1;

                gem5::Tick nextWake = gem5::curTick() + memory.clock * static_cast<gem5::Tick>(stepCycles);

                memory.nextEventCycle = nextEvent;
                memory.ScheduleClockEvent( nextWake );
            }

            memory.masterInstance->m_request_map.insert( std::pair<NVMainRequest *, NVMainMemoryRequest *>( request, memRequest ) );
            memory.m_requests_outstanding++;

            /*
             *  It seems gem5 will block until the packet gets a response, so create a copy of the request, so
             *  the memory controller has it, then delete the original copy to respond to the packet.
             */
            if( request->type == WRITE )
            {
                NVMainMemoryRequest *requestCopy = new NVMainMemoryRequest( );

                requestCopy->request = new NVMainRequest( );
                *(requestCopy->request) = *request;
                requestCopy->packet = pkt;
                requestCopy->issueTick = gem5::curTick();
                requestCopy->atomic = false;

                memRequest->packet = NULL;

                memory.masterInstance->m_request_map.insert( std::pair<NVMainRequest *, NVMainMemoryRequest *>( requestCopy->request, requestCopy ) );
                memory.m_requests_outstanding++;

                memory.RequestComplete( requestCopy->request );
            }

            /* Call post-issue hooks. */
            if( request != NULL )
            {
                for( it = postHooks.begin(); it != postHooks.end(); it++ )
                {
                    (*it)->SetParent( &memory );
                    (*it)->IssueCommand( request );
                }
            }
        }
        else
        {
            DPRINTF(NVMain, "nvmain_mem.cc: Can not enqueue Mem request for 0x%x of type %s\n", request->address.GetPhysicalAddress( ), ((pkt->isRead()) ? "READ" : "WRITE") );
            DPRINTF(NVMainMin, "nvmain_mem.cc: Can not enqueue Mem request for 0x%x of type %s\n", request->address.GetPhysicalAddress( ), ((pkt->isRead()) ? "READ" : "WRITE") );

            if (pkt->isRead())
            {
                memory.retryRead = true;
            }
            else
            {
                memory.retryWrite = true;
            }

            delete request;
            request = NULL;
        }
    }
    return enqueued;
}



gem5::Tick NVMainMemory::doAtomicAccess(gem5::PacketPtr pkt)
{
    access(pkt);
    return static_cast<gem5::Tick>(m_avgAtomicLatency);
}



void NVMainMemory::doFunctionalAccess(gem5::PacketPtr pkt)
{
    functionalAccess(pkt);
}

//FIXMEvoid NVMainMemory::dofunctionalRead(gem5::Addr addr, int size, uint8_t * res)
//FIXME{
//FIXME    functionalRead(addr, size, res);
//FIXME}
//FIXME
//FIXMEvoid NVMainMemory::dofunctionalWrite(gem5::Addr addr, int size, uint8_t * res)
//FIXME{
//FIXME    functionalWrite(addr, size, res);
//FIXME}

gem5::DrainState NVMainMemory::drain()
{
    if( !masterInstance->m_request_map.empty() )
    {
        return gem5::DrainState::Draining;
    }
    else
    {
        return gem5::DrainState::Drained;
    }
}


void NVMainMemory::MemoryPort::recvRespRetry( )
{
    memory.recvRetry( );
}


void NVMainMemory::MemoryPort::recvRetry( )
{
    memory.recvRetry( );
}


void NVMainMemory::recvRetry( )
{
    DPRINTF(NVMain, "NVMainMemory: recvRetry() called.\n");
    DPRINTF(NVMainMin, "NVMainMemory: recvRetry() called.\n");

    retryResp = false;
    SendResponses( );
}


bool NVMainMemory::RequestComplete(NVM::NVMainRequest *req)
{
    if ( req->isPIM )
    {
        std::cout << "NVMainMemory::RequestComplete received PIM request!" << std::endl;
    }
    bool isRead = (req->type == READ || req->type == READ_PRECHARGE);
    bool isWrite = (req->type == WRITE || req->type == WRITE_PRECHARGE);

    /* Ignore bus read/write requests generated by the banks. */
    if( req->type == BUS_WRITE || req->type == BUS_READ )
    {
        delete req;
        return true;
    }

    NVMainMemoryRequest *memRequest;
    std::map<NVMainRequest *, NVMainMemoryRequest *>::iterator iter;

    // Find the mem request pointer in the map.
    assert(masterInstance->m_request_map.count(req) != 0);
    iter = masterInstance->m_request_map.find(req);
    memRequest = iter->second;

    if(!memRequest->atomic)
    {
        bool respond = false;

        NVMainMemory *ownerInstance = dynamic_cast<NVMainMemory *>( req->owner );
        assert( ownerInstance != NULL );

        if( memRequest->packet )
        {
            respond = memRequest->packet->needsResponse();
            ownerInstance->access(memRequest->packet);
        }

        for( auto retryIter = masterInstance->allInstances.begin(); 
             retryIter != masterInstance->allInstances.end(); retryIter++ )
        {
            if( (*retryIter)->retryRead && (isRead || isWrite) )
            {
                (*retryIter)->retryRead = false;
                (*retryIter)->port.sendRetryReq();
            }
            if( (*retryIter)->retryWrite && (isRead || isWrite) )
            {
                (*retryIter)->retryWrite = false;
                (*retryIter)->port.sendRetryReq();
            }
        }

        DPRINTF(NVMain, "Completed Mem request for 0x%x of type %s\n", req->address.GetPhysicalAddress( ), (req->isPIM ? "PIM" : isRead ? "READ" : "WRITE"));

        if(respond)
        {
            ownerInstance->responseQueue.push_back(memRequest->packet);
            ownerInstance->ScheduleResponse( );

            delete req;
            delete memRequest;
        }
        else
        {
            if( memRequest->packet )
                ownerInstance->pendingDelete.push_back(memRequest->packet);

            CheckDrainState( );

            delete req;
            delete memRequest;
        }
    }
    else
    {
        delete req;
        delete memRequest;
    }


    masterInstance->m_request_map.erase(iter);
    //assert(m_requests_outstanding > 0);
    m_requests_outstanding--;

    return true;
}


void NVMainMemory::SendResponses( )
{
    if( responseQueue.empty() || retryResp == true )
        return;


    bool success = port.sendTimingResp( responseQueue.front() );

    if( success )
    {
        DPRINTF(NVMain, "NVMainMemory: Sending response.\n");

        responseQueue.pop_front( );

        if( !responseQueue.empty( ) )
            ScheduleResponse( );

        CheckDrainState( );
    }
    else
    {
        DPRINTF(NVMain, "NVMainMemory: Retrying response.\n");
        DPRINTF(NVMainMin, "NVMainMemory: Retrying response.\n");

        retryResp = true;
    }
}


void NVMainMemory::CheckDrainState( )
{
    if( drainManager != NULL && masterInstance->m_request_map.empty() )
    {
        DPRINTF(NVMain, "NVMainMemory: Drain completed.\n");
        DPRINTF(NVMainMin, "NVMainMemory: Drain completed.\n");

        drainManager->signalDrainDone( );
        drainManager = NULL;
    }
}


void NVMainMemory::ScheduleResponse( )
{
    if( !respondEvent.scheduled( ) )
        schedule(respondEvent, gem5::curTick() + clock);
}


void NVMainMemory::ScheduleClockEvent( gem5::Tick nextWake )
{
    if( !masterInstance->clockEvent.scheduled() )
        schedule(masterInstance->clockEvent, nextWake);
    else
        reschedule(masterInstance->clockEvent, nextWake);
}


void NVMainMemory::serialize(gem5::CheckpointOut &cp) const
{
    if (masterInstance != this)
        return;

    std::string nvmain_chkpt_dir = "";

    if( m_nvmainConfig->KeyExists( "CheckpointDirectory" ) )
        nvmain_chkpt_dir = m_nvmainConfig->GetString( "CheckpointDirectory" );

    if( nvmain_chkpt_dir != "" )
    {
        std::cout << "NVMainMemory: Writing to checkpoint directory " << nvmain_chkpt_dir << std::endl;

        m_nvmainPtr->CreateCheckpoint( nvmain_chkpt_dir );
    }
}


void NVMainMemory::unserialize(gem5::CheckpointIn &cp)
{
    if (masterInstance != this)
        return;

    std::string nvmain_chkpt_dir = "";

    if( m_nvmainConfig->KeyExists( "CheckpointDirectory" ) )
        nvmain_chkpt_dir = m_nvmainConfig->GetString( "CheckpointDirectory" );

    if( nvmain_chkpt_dir != "" )
    {
        std::cout << "NVMainMemory: Reading from checkpoint directory " << nvmain_chkpt_dir << std::endl;

        m_nvmainPtr->RestoreCheckpoint( nvmain_chkpt_dir );
    }
}


void NVMainMemory::tick( )
{
    // Cycle memory controller
    if (masterInstance == this)
    {
        /* Keep NVMain in sync with gem5. */
        assert(gem5::curTick() >= lastWakeup);
        ncycle_t stepCycles = (gem5::curTick() - lastWakeup) / clock;

        DPRINTF(NVMain, "NVMainMemory: Stepping %d cycles\n", stepCycles);
        m_nvmainGlobalEventQueue->Cycle( stepCycles );

        lastWakeup = gem5::curTick();

        ncycle_t nextEvent;

        nextEvent = m_nvmainGlobalEventQueue->GetNextEvent(NULL);
        if( nextEvent != std::numeric_limits<ncycle_t>::max() )
        {
            ncycle_t currentCycle = m_nvmainGlobalEventQueue->GetCurrentCycle();

            assert(nextEvent >= currentCycle);
            stepCycles = nextEvent - currentCycle;

            gem5::Tick nextWake = gem5::curTick() + clock * static_cast<gem5::Tick>(stepCycles);

            DPRINTF(NVMain, "NVMainMemory: Next event: %d CurrentCycle: %d\n", nextEvent, currentCycle);
            DPRINTF(NVMain, "NVMainMemory: Schedule wake for %d\n", nextWake);

            nextEventCycle = nextEvent;
            ScheduleClockEvent( nextWake );
        }
    }
}


//FIXMENVMainMemory *
//FIXMENVMainMemoryParams::create()
//FIXME{
//FIXME    return new NVMainMemory(this);
//FIXME}

