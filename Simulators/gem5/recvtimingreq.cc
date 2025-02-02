bool
NVMainMemory::MemoryPort::recvTimingReq(PacketPtr pkt)
{
    if (pkt->isPIM()) {
        DPRINTF(PIM, "Received PIM pkt in NVMain!\n");
        Packet::PIMSenderState* senderState = dynamic_cast<Packet::PIMSenderState*>(pkt->senderState);
        assert(senderState);
        
        request->access = UNKNOWN_ACCESS;
        request->addr_src1.SetPhysicalAddress(senderState->addr[0] - addressFixUp);
        request->addr_src2.SetPhysicalAddress(senderState->addr[1] - addressFixUp);
        request->addr_dst.SetPhysicalAddress(senderState->addr[2] - addressFixUp);
        request->pim_size = senderState->size;
        request->address.SetPhysicalAddress(senderState->addr[2] - addressFixUp);
        request->status = MEM_REQUEST_INCOMPLETE;
        request->type = PIM;
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
            memRequest->issueTick = curTick();
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

                Tick nextWake = curTick() + memory.clock * static_cast<Tick>(stepCycles);

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

                Tick nextWake = curTick() + memory.clock * static_cast<Tick>(stepCycles);

                memory.nextEventCycle = nextEvent;
                memory.ScheduleClockEvent( nextWake );
            }
            
            memory.masterInstance->m_request_map.insert( std::pair<NVMainRequest *, NVMainMemoryRequest *>( request, memRequest ) );
            memory.m_requests_outstanding++;

            /*
             *  It seems gem5 will block until the packet gets a response, so create a copy of the request, so
             *  the memory controller has it, then delete the original copy to respond to the packet.
             */
            NVMainMemoryRequest *requestCopy = new NVMainMemoryRequest( );

            requestCopy->request = new NVMainRequest( );
            *(requestCopy->request) = *request;
            requestCopy->packet = pkt;
            requestCopy->issueTick = curTick();
            requestCopy->atomic = false;

            memRequest->packet = NULL;

            memory.masterInstance->m_request_map.insert( std::pair<NVMainRequest *, NVMainMemoryRequest *>( requestCopy->request, requestCopy ) );
            memory.m_requests_outstanding++;

            memory.RequestComplete( requestCopy->request );
           
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



        // calculate PIM ops here
        //FIXMEint pim_size = senderState->size;
        //FIXMEconst int total = 3;
        //FIXMEstd::bitset<1024> src[total];
        //FIXMEif (pkt->cmd == MemCmd::PIMAND) {
        //FIXME    for (int i=0; i<senderState->addr.size(); i++) {
        //FIXME        DPRINTF(PIM, "Paddr of PIM input %d: [0x%lx]\n", i+1, senderState->addr[i]);
        //FIXME        // fix wrong functional data later
        //FIXME        //FIXMEmemory.dofunctionalRead(senderState->addr[i], pim_size, (uint8_t*) &src[i]);
        //FIXME        //FIXMEstd::cout << "src" << i+1 << ": " << std::dec << src[i] << std::endl;
        //FIXME    }
        //FIXME}
    }

}
