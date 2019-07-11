// Copyright 2013-2018 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright(c) 2013-2018, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include <sst_config.h>
#include "directoryController.h"


#include <sst/core/params.h>
#include <sst/core/simulation.h>

#include "memNIC.h"

/* Debug macros */
#ifdef __SST_DEBUG_OUTPUT__ /* From sst-core, enable with --enable-debug */
#define is_debug_addr(addr) (DEBUG_ADDR.empty() || DEBUG_ADDR.find(addr) != DEBUG_ADDR.end())
#define is_debug_event(ev) (DEBUG_ADDR.empty() || ev->doDebug(DEBUG_ADDR))
#else
#define is_debug_addr(addr) false
#define is_debug_event(ev) false
#endif

using namespace SST;
using namespace SST::MemHierarchy;
using namespace std;


const MemEvent::id_type DirectoryController::DirEntry::NO_LAST_REQUEST = std::make_pair((uint64_t)-1, -1);

DirectoryController::DirectoryController(ComponentId_t id, Params &params) :
    Component(id) {
    int debugLevel = params.find<int>("debug_level", 0);
    cacheLineSize = params.find<uint32_t>("cache_line_size", 64);

    dbg.init("", debugLevel, 0, (Output::output_location_t)params.find<int>("debug", 0));
    if (debugLevel < 0 || debugLevel > 10)     dbg.fatal(CALL_INFO, -1, "Debugging level must be between 0 and 10. \n");


    // Detect deprecated parameters and warn/fatal
    // Currently deprecated - network_num_vc
    bool found;
    out.init("", params.find<int>("verbose", 1), 0, Output::STDOUT);
    params.find<int>("network_num_vc", 0, found);
    if (found) {
        out.output("%s, ** Found deprecated parameter: network_num_vc ** MemHierarchy does not use multiple virtual channels. Remove this parameter from your input deck to eliminate this message.\n", getName().c_str());
    }

    // Debug address
    std::vector<Addr> addrArr;
    params.find_array<Addr>("debug_addr", addrArr);
    for (std::vector<Addr>::iterator it = addrArr.begin(); it != addrArr.end(); it++) {
        DEBUG_ADDR.insert(*it);
    }

    targetCount = 0;

    registerTimeBase("1 ns", true);

    entryCacheMaxSize = params.find<uint64_t>("entry_cache_size", 32768);
    entryCacheSize = 0;
    std::string net_bw = params.find<std::string>("network_bw", "80GiB/s");

    MemRegion region;
    bool gotRegion = false;
    region.start = params.find<uint64_t>("addr_range_start", 0, found);
    if (!found) region.start = params.find<uint64_t>("memNIC.addr_range_start", 0, found);
    if (!found) region.start = params.find<uint64_t>("memlink.addr_range_start", 0, found);
    gotRegion |= found; 

    region.end = params.find<uint64_t>("addr_range_end", (uint64_t) - 1, found);
    if (!found) region.end = params.find<uint64_t>("memNIC.addr_range_end", (uint64_t) - 1, found);
    if (!found) region.end = params.find<uint64_t>("memlink.addr_range_end", (uint64_t) - 1, found);
    gotRegion |= found;

    string ilSize   = params.find<std::string>("interleave_size", "0B", found);
    if (!found) ilSize = params.find<std::string>("memNIC.interleave_size", "0B", found);
    if (!found) ilSize = params.find<std::string>("memlink.interleave_size", "0B", found);
    gotRegion |= found;

    string ilStep   = params.find<std::string>("interleave_step", "0B", found);
    if (!found) ilStep = params.find<std::string>("memNIC.interleave_step", "0B", found);
    if (!found) ilStep = params.find<std::string>("memlink.interleave_step", "0B", found);
    gotRegion |= found;


    memOffset       = params.find<uint64_t>("mem_addr_start", 0);
    string protstr  = params.find<std::string>("coherence_protocol", "MESI");
    dbg.debug(_L5_, "Directory controller using protocol: %s\n", protstr.c_str());

    UnitAlgebra packetSize = UnitAlgebra(params.find<std::string>("min_packet_size", "8B"));
    if (!packetSize.hasUnits("B")) dbg.fatal(CALL_INFO, -1, "%s, Invalid param: min_packet_size - must have units of bytes (B). SI units are ok. You specified '%s'\n", getName().c_str(), packetSize.toString().c_str());

    int mshrSize    = params.find<int>("mshr_num_entries",-1);
    if (mshrSize == -1) mshrSize = HUGE_MSHR;
    if (mshrSize < 1) dbg.fatal(CALL_INFO, -1, "Invalid param(%s): mshr_num_entries - must be at least 1 or else -1 to indicate a very large MSHR\n", getName().c_str());
    mshr                = new MSHR(&dbg, mshrSize, this->getName(), DEBUG_ADDR);

    if(0 == region.end) region.end = (uint64_t)-1;
    numTargets = 0;

    /* Check parameter validity */
    if (protstr == "mesi" || protstr == "MESI") protocol = CoherenceProtocol::MESI;
    else if (protstr == "msi" || protstr == "MSI") protocol = CoherenceProtocol::MSI;
    else dbg.fatal(CALL_INFO, -1, "Invalid param(%s): coherence_protocol - must be 'MESI' or 'MSI'. You specified: %s\n", getName().c_str(), protstr.c_str());

    /* Check interleaveSize & Step
     * Both must be specified in B (SI units ok)
     * Both must be divisible by the cache line size */
    fixByteUnits(ilSize);
    fixByteUnits(ilStep);
    region.interleaveSize = UnitAlgebra(ilSize).getRoundedValue();
    region.interleaveStep = UnitAlgebra(ilStep).getRoundedValue();
    if (!UnitAlgebra(ilSize).hasUnits("B") || region.interleaveSize % cacheLineSize != 0) {
        dbg.fatal(CALL_INFO, -1, "Invalid param(%s): interleave_size - must be specified in bytes with units (SI units OK) and must also be a multiple of cache_line_size. This definition has CHANGED. Example: If you used to set this to '1', change it to '1KiB'. You specified %s\n",
                getName().c_str(), ilSize.c_str());
    }
    if (!UnitAlgebra(ilStep).hasUnits("B") || region.interleaveStep % cacheLineSize != 0) {
        dbg.fatal(CALL_INFO, -1, "Invalid param(%s): interleave_step - must be specified in bytes with units (SI units OK) and must also be a multiple of cache_line_size. This definition has CHANGED. Example: If you used to set this to '4', change it to '4KiB'. You specified %s\n",
                getName().c_str(), ilStep.c_str());
    }

    /* Get latencies */
    accessLatency   = params.find<uint64_t>("access_latency_cycles", 0);
    mshrLatency     = params.find<uint64_t>("mshr_latency_cycles", 0);


    network = loadUserSubComponent<MemLinkBase>("cpulink");
    memLink = loadUserSubComponent<MemLinkBase>("memlink");
    if (network || memLink) {
        if (!network) {
            network = memLink;
            memLink = nullptr;
        }
        if (gotRegion) {
            network->setRegion(region);
        } else {
            if (network->getRegion() != region) {
                out.output(CALL_INFO, "%s, Warning: getting region parameters (addr_range_start/end, interleave_step/size) from link subcomponent."
                        " In the future this will not be supported and region parameters should be declared in the directory's parameters instead.\n", getName().c_str());
            }
            region = network->getRegion();
        }
            
        if (memLink)
            memLink->setRegion(region);

        network->setRecvHandler(new Event::Handler<DirectoryController>(this, &DirectoryController::handlePacket));
        network->setName(getName());
        memoryName = "";
        if (!memLink) { // Need a memory name
            dbg.verbose(_INFO_, "%s, Detected user defined subcomponent for either the cpu or mem link but not both. Assuming this component has just one link.\n", getName().c_str());
            memoryName  = params.find<std::string>("net_memory_name", "");
            if (memoryName == "")
                dbg.fatal(CALL_INFO,-1,"Param not specified(%s): net_memory_name - name of the memory owned by this directory controller. If you intended to have a direct link to memory, please declare both a cpulink and memlink in your input configuration and ignore this parameter.\n", getName().c_str());
        } else {
            memLink->setRecvHandler(new Event::Handler<DirectoryController>(this, &DirectoryController::handlePacket));
            memLink->setName(getName());
        }
    } else {
        /* Set up links/network to cache & memory */
        /* First, fixup nic parameters and warn that we're doing it */
        if (fixupParam(params, "network_bw", "memNIC.network_bw"))
            out.output(CALL_INFO, "Note (%s): Changed 'network_bw' to 'memNIC.network_bw' in params. Change your input file to remove this notice.\n", getName().c_str());
        if (fixupParam(params, "network_input_buffer_size", "memNIC.network_input_buffer_size"))
            out.output(CALL_INFO, "Note (%s): Changed 'network_input_buffer_size' to 'memNIC.network_input_buffer_size' in params. Change your input file to remove this notice.\n", getName().c_str());
        if (fixupParam(params, "network_output_buffer_size", "memNIC.network_output_buffer_size"))
            out.output(CALL_INFO, "Note (%s): Changed 'network_output_buffer_size' to 'memNIC.network_output_buffer_size' in params. Change your input file to remove this notice.\n", getName().c_str());
        if (fixupParam(params, "addr_range_start", "memNIC.addr_range_start"))
            out.output(CALL_INFO, "Note (%s): Changed 'addr_range_start' to 'memNIC.addr_range_start' in params. Change your input file to remove this notice.\n", getName().c_str());
        if (fixupParam(params, "addr_range_end", "memNIC.addr_range_end"))
            out.output(CALL_INFO, "Note (%s): Changed 'addr_range_end' to 'memNIC.addr_range_end' in params. Change your input file to remove this notice.\n", getName().c_str());
        if (fixupParam(params, "interleave_size", "memNIC.interleave_size"))
            out.output(CALL_INFO, "Note (%s): Changed 'interleave_size' to 'memNIC.interleave_size' in params. Change your input file to remove this notice.\n", getName().c_str());
        if (fixupParam(params, "interleave_step", "memNIC.interleave_step"))
            out.output(CALL_INFO, "Note (%s): Changed 'interleave_step' to 'memNIC.interleave_step' in params. Change your input file to remove this notice.\n", getName().c_str());
        if (fixupParam(params, "min_packet_size", "memNIC.min_packet_size"))
            out.output(CALL_INFO, "Note (%s): Changed 'min_packet_size' to 'memNIC.min_packet_size' in params. Change your input file to remove this notice.\n", getName().c_str());

        Params nicParams = params.find_prefix_params("memNIC.");

        nicParams.insert("group", "3", false);
        int cl = nicParams.find<int>("group");
        nicParams.insert("sources", std::to_string(cl - 1), false);
        nicParams.insert("destinations", std::to_string(cl + 1), false);

        // Determine which ports are connected
        unsigned int portCount = 1;
        if (isPortConnected("network_ack")) portCount++;
        if (isPortConnected("network_fwd")) portCount++;
        if (isPortConnected("network_data")) portCount++;
        if (portCount == 4) {
            nicParams.insert("req.port", "network");
            nicParams.insert("ack.port", "network_ack");
            nicParams.insert("fwd.port", "network_fwd");
            nicParams.insert("data.port", "network_data");
            network = loadAnonymousSubComponent<MemLinkBase>("memHierarchy.MemNICFour", "cpulink", 0, ComponentInfo::SHARE_PORTS | ComponentInfo::INSERT_STATS, nicParams);
        } else {
            nicParams.insert("port", "network");
            network = loadAnonymousSubComponent<MemLinkBase>("memHierarchy.MemNIC", "cpulink", 0, ComponentInfo::SHARE_PORTS | ComponentInfo::INSERT_STATS, nicParams);
        }

        network->setRecvHandler(new Event::Handler<DirectoryController>(this, &DirectoryController::handlePacket));

        if (isPortConnected("memory")) {
            Params memParams = params.find_prefix_params("memlink.");
            memParams.insert("port", "memory");
            memParams.insert("latency", "1ns");
            memParams.insert("addr_range_start", std::to_string(region.start), false);
            memParams.insert("addr_range_end", std::to_string(region.end), false);
            memParams.insert("interleave_size", ilSize, false);
            memParams.insert("interleave_step", ilStep, false);
            memLink = loadAnonymousSubComponent<MemLinkBase>("memHierarchy.MemLink", "memlink", 0, ComponentInfo::SHARE_PORTS | ComponentInfo::INSERT_STATS, memParams);
            memLink->setRecvHandler(new Event::Handler<DirectoryController>(this, &DirectoryController::handlePacket));
            if (!memLink) {
                dbg.fatal(CALL_INFO, -1, "%s, Error creating link to memory from directory controller\n", getName().c_str());
            }
            memoryName = "";
        } else {
            memoryName  = params.find<std::string>("net_memory_name", "");
            if (memoryName == "")
                dbg.fatal(CALL_INFO,-1,"Param not specified(%s): net_memory_name - name of the memory owned by this directory controller. If you did not intend to connect to memory over the network, please connect memory to the 'memory' port and ignore this parameter.\n", getName().c_str());
            memLink = nullptr;

        }
        network->setName(getName());
        if (memLink) memLink->setName(getName());
    }
    clockHandler = new Clock::Handler<DirectoryController>(this, &DirectoryController::clock);
    defaultTimeBase = registerClock(params.find<std::string>("clock", "1GHz"), clockHandler);
    clockOn = true;

    // Requests per cycle
    maxRequestsPerCycle = params.find<int>("max_requests_per_cycle", 0);

    // Timestamp - aka cycle count
    timestamp = 0;

    // Coherence protocol configuration
    waitWBAck = false; // Don't expect WB Acks

    // Register statistics
    stat_replacementRequestLatency  = registerStatistic<uint64_t>("replacement_request_latency");
    stat_getRequestLatency          = registerStatistic<uint64_t>("get_request_latency");
    stat_cacheHits                  = registerStatistic<uint64_t>("directory_cache_hits");
    stat_mshrHits                   = registerStatistic<uint64_t>("mshr_hits");
    stat_GetXReqReceived            = registerStatistic<uint64_t>("requests_received_GetX");
    stat_GetSXReqReceived          = registerStatistic<uint64_t>("requests_received_GetSX");
    stat_GetSReqReceived            = registerStatistic<uint64_t>("requests_received_GetS");
    stat_PutMReqReceived            = registerStatistic<uint64_t>("requests_received_PutM");
    stat_PutEReqReceived            = registerStatistic<uint64_t>("requests_received_PutE");
    stat_PutSReqReceived            = registerStatistic<uint64_t>("requests_received_PutS");
    stat_NACKRespReceived           = registerStatistic<uint64_t>("responses_received_NACK");
    stat_FetchRespReceived          = registerStatistic<uint64_t>("responses_received_FetchResp");
    stat_FetchXRespReceived         = registerStatistic<uint64_t>("responses_received_FetchXResp");
    stat_PutMRespReceived           = registerStatistic<uint64_t>("responses_received_PutM");
    stat_PutERespReceived           = registerStatistic<uint64_t>("responses_received_PutE");
    stat_PutSRespReceived           = registerStatistic<uint64_t>("responses_received_PutS");
    stat_dataReads                  = registerStatistic<uint64_t>("memory_requests_data_read");
    stat_dataWrites                 = registerStatistic<uint64_t>("memory_requests_data_write");
    stat_dirEntryReads              = registerStatistic<uint64_t>("memory_requests_directory_entry_read");
    stat_dirEntryWrites             = registerStatistic<uint64_t>("memory_requests_directory_entry_write");
    stat_InvSent                    = registerStatistic<uint64_t>("requests_sent_Inv");
    stat_FetchInvSent               = registerStatistic<uint64_t>("requests_sent_FetchInv");
    stat_FetchInvXSent              = registerStatistic<uint64_t>("requests_sent_FetchInvX");
    stat_NACKRespSent               = registerStatistic<uint64_t>("responses_sent_NACK");
    stat_GetSRespSent               = registerStatistic<uint64_t>("responses_sent_GetSResp");
    stat_GetXRespSent               = registerStatistic<uint64_t>("responses_sent_GetXResp");
    stat_MSHROccupancy              = registerStatistic<uint64_t>("MSHR_occupancy");
    stat_NoncacheReceived           = registerStatistic<uint64_t>("requests_received_noncacheable");
    stat_CustomReceived             = registerStatistic<uint64_t>("requests_received_custom");

}



DirectoryController::~DirectoryController(){
    for(std::unordered_map<Addr, DirEntry*>::iterator i = directory.begin(); i != directory.end() ; ++i){
        delete i->second;
    }
    directory.clear();

    while(workQueue.size()){
        MemEvent *front = workQueue.front().first;
        delete front;
        workQueue.pop_front();
    }
}



void DirectoryController::handlePacket(SST::Event *event){
    MemEventBase *evb = static_cast<MemEventBase*>(event);
    evb->setDeliveryTime(getCurrentSimTimeNano());

    if (!clockOn) {
        turnClockOn();
    }

    /* Forward events that we don't handle */
    if (MemEventTypeArr[(int)evb->getCmd()] != MemEventType::Cache || evb->queryFlag(MemEvent::F_NONCACHEABLE)) {

        if (is_debug_event(evb)) {
            dbg.debug(_L3_, "\n%" PRIu64 " (%s) Received: %s\n",
                    getCurrentSimTimeNano(), getName().c_str(), evb->getVerboseString().c_str());
        }

        if (BasicCommandClassArr[(int)evb->getCmd()] == BasicCommandClass::Request)
            handleNoncacheableRequest(evb);
        else
            handleNoncacheableResponse(evb);
        return;

    }

    MemEvent *ev = static_cast<MemEvent*>(event);
    if (ev->getCmd() == Command::GetSResp || ev->getCmd() == Command::GetXResp || ev->getCmd() == Command::FlushLineResp
            || ev->getCmd() == Command::ForceInv || ev->getCmd() == Command::FetchInv || ev->getCmd() == Command::AckPut) {
        handleMemoryResponse(event);
    } else {
        workQueue.push_back(std::make_pair(ev,false));
    }
}

/**
 * Profile requests sent to directory controller
 */
inline void DirectoryController::profileRequestRecv(MemEvent * event, DirEntry * entry) {
    Command cmd = event->getCmd();
    switch (cmd) {
        case Command::GetX:
            stat_GetXReqReceived->addData(1);
            break;
        case Command::GetSX:
            stat_GetSXReqReceived->addData(1);
            break;
        case Command::GetS:
            stat_GetSReqReceived->addData(1);
            break;
    case Command::PutM:
        stat_PutMReqReceived->addData(1);
        break;
    case Command::PutE:
        stat_PutEReqReceived->addData(1);
        break;
    case Command::PutS:
        stat_PutSReqReceived->addData(1);
        break;
    default:
        break;

    }
    if (!entry || entry->isCached()) {
        stat_cacheHits->addData(1);
    }
}
/**
 * Profile requests sent from directory controller to memory or other caches
 */
inline void DirectoryController::profileRequestSent(MemEvent * event) {
    Command cmd = event->getCmd();
    switch(cmd) {
        case Command::PutM:
        if (event->getAddr() == 0) {
            stat_dirEntryWrites->addData(1);
        } else {
            stat_dataWrites->addData(1);
        }
        break;
        case Command::GetX:
        if (event->queryFlag(MemEvent::F_NONCACHEABLE)) {
            stat_dataWrites->addData(1);
            break;
        }
        case Command::GetSX:
        case Command::GetS:
        if (event->getAddr() == 0) {
            stat_dirEntryReads->addData(1);
        } else {
            stat_dataReads->addData(1);
        }
        break;
        case Command::FetchInv:
        stat_FetchInvSent->addData(1);
        break;
        case Command::FetchInvX:
        stat_FetchInvXSent->addData(1);
        break;
        case Command::Inv:
        stat_InvSent->addData(1);
        break;
    default:
        break;

    }
}

/**
 * Profile responses sent from directory controller to caches
 */
inline void DirectoryController::profileResponseSent(MemEvent * event) {
    Command cmd = event->getCmd();
    switch(cmd) {
        case Command::GetSResp:
        stat_GetSRespSent->addData(1);
        break;
        case Command::GetXResp:
        stat_GetXRespSent->addData(1);
        break;
    case Command::NACK:
        stat_NACKRespSent->addData(1);
        break;
    default:
        break;
    }
}

/**
 * Profile responses received by directory controller from caches
 */
inline void DirectoryController::profileResponseRecv(MemEvent * event) {
    Command cmd = event->getCmd();
    switch(cmd) {
        case Command::FetchResp:
        stat_FetchRespReceived->addData(1);
        break;
        case Command::FetchXResp:
        stat_FetchXRespReceived->addData(1);
        break;
    case Command::PutM:
        stat_PutMRespReceived->addData(1);
        break;
    case Command::PutE:
        stat_PutERespReceived->addData(1);
        break;
    case Command::PutS:
        stat_PutSRespReceived->addData(1);
        break;
    case Command::NACK:
        stat_NACKRespReceived->addData(1);
        break;
    default:
        break;
    }
}

/**
 *  Called each cycle. Handle any waiting events in the queue.
 */
bool DirectoryController::clock(SST::Cycle_t cycle){
    timestamp++;
    stat_MSHROccupancy->addData(mshr->getSize());

    sendOutgoingEvents();

    bool netIdle = network->clock();
    bool empty = workQueue.empty() && netMsgQueue.empty() && memMsgQueue.empty();

    int requestsThisCycle = 0;
    while(!workQueue.empty()) {
        requestsThisCycle++;
        MemEvent *event = workQueue.front().first;
        bool replay = workQueue.front().second;
        workQueue.erase(workQueue.begin());
	processPacket(event, replay);
        if (requestsThisCycle == maxRequestsPerCycle) {
            break;
        }
    }

    if (empty && netIdle && clockOn) {
        clockOn = false;
        lastActiveClockCycle = timestamp;
        return true;
    }
    return false;
}

void DirectoryController::processPacket(MemEvent * ev, bool replay) {
    if (is_debug_event(ev)) {
        dbg.debug(_L3_, "\n%" PRIu64 " (%s) Processing: %s\n", getCurrentSimTimeNano(), getName().c_str(), ev->getVerboseString().c_str());
        cout << flush;
    }

    if(! isRequestAddressValid(ev->getAddr()) ) {
	dbg.fatal(CALL_INFO, -1, "%s, Error: Request address is not valid. Event: %s. Time = %" PRIu64 "ns\n",
                getName().c_str(), ev->getVerboseString().c_str(), getCurrentSimTimeNano());
    }

    Command cmd = ev->getCmd();
    switch (cmd) {
        case Command::GetS:
            handleGetS(ev, replay);
            break;
        case Command::GetSX:    // handled like GetX, this is a GetS-lock request
        case Command::GetX:
            handleGetX(ev, replay);
            break;
        case Command::NACK:
            handleNACK(ev);
            break;
        case Command::PutS:
            handlePutS(ev);
            break;
        case Command::PutE:
            handlePutE(ev);
            break;
        case Command::PutM:
            handlePutM(ev);
            break;
        case Command::FetchResp:
            handleFetchResp(ev, false);
            break;
        case Command::FetchXResp:
            handleFetchXResp(ev, false);
            break;
        case Command::AckInv:
            handleAckInv(ev);
            break;
        case Command::FlushLineInv:
            handleFlushLineInv(ev);
            break;
        case Command::FlushLine:
            handleFlushLine(ev);
            break;
        case Command::FetchInv:
        case Command::ForceInv:
            handleBackInv(ev);
            break;
        default:
            dbg.fatal(CALL_INFO, -1 , "%s, Error: Received unrecognized request: %s. Time = %" PRIu64 "ns\n",
                    getName().c_str(), ev->getVerboseString().c_str(), getCurrentSimTimeNano());
    }
}


void DirectoryController::handleNoncacheableRequest(MemEventBase * ev) {
    if (!(ev->queryFlag(MemEventBase::F_NORESPONSE))) {
        noncacheMemReqs[ev->getID()] = ev->getSrc();
    }
    stat_NoncacheReceived->addData(1);

    if (ev->getCmd() == Command::CustomReq)
        stat_CustomReceived->addData(1);

    ev->setSrc(getName());
    ev->setDst(memoryName);

    sendEventToMem(ev);
    return;
}


void DirectoryController::handleNoncacheableResponse(MemEventBase * ev) {
    if (noncacheMemReqs.find(ev->getID()) == noncacheMemReqs.end()) {
        dbg.fatal(CALL_INFO, -1, "%s, Error: Received a noncacheable response that does not match a pending request. Event: %s\n. Time: %" PRIu64 "ns\n",
                getName().c_str(), ev->getVerboseString().c_str(), getCurrentSimTimeNano());
    }
    ev->setDst(noncacheMemReqs[ev->getID()]);
    ev->setSrc(getName());

    noncacheMemReqs.erase(ev->getID());

    sendEventToCaches(ev, timestamp);
    return;
}

void DirectoryController::sendOutgoingEvents() {
    bool debugLine = false;
    while (!netMsgQueue.empty() && netMsgQueue.begin()->first <= timestamp) {
        MemEventBase * ev = netMsgQueue.begin()->second;

        if (is_debug_event(ev)) {
            if (!debugLine) dbg.debug(_L3_, "\n");
            debugLine = true;
            dbg.debug(_L3_, "%" PRIu64 " (%s) Sending event to network: %s\n",
                    getCurrentSimTimeNano(), getName().c_str(), ev->getBriefString().c_str());
        }

        network->send(ev);
        netMsgQueue.erase(netMsgQueue.begin());
    }

    while (!memMsgQueue.empty() && memMsgQueue.begin()->first <= timestamp) {
        MemEventBase * ev = memMsgQueue.begin()->second;

        if (is_debug_event(ev)) {
            if (!debugLine) dbg.debug(_L3_, "\n");
            debugLine = true;
            dbg.debug(_L3_, "%" PRIu64 " (%s) Sending event to memory: %s\n",
                    getCurrentSimTimeNano(), getName().c_str(), ev->getBriefString().c_str());
        }

        sendEventToMem(ev);
        memMsgQueue.erase(memMsgQueue.begin());
    }
}


/* Event handlers */

/** GetS */
void DirectoryController::handleGetS(MemEvent * event, bool inMSHR) {
    Addr addr = event->getBaseAddr();
    DirEntry * entry = getDirEntry(addr);
    State state = entry->getState();
    bool cached = entry->isCached();
    MemEventStatus status = MemEventStatus::OK;

    /* Put request in MSHR (all GetS requests need to be buffered while they wait for data) and stall if there are waiting requests ahead of us */
    if (!(mshr->elementIsHit(addr, event))) {
        bool conflict = mshr->isHit(addr);
        if (!mshr->insert(addr, event)) {
            mshrNACKRequest(event);
            return;
        }
        if (conflict) {
            return; // stall event until conflicting request finishes
        } else {
            profileRequestRecv(event, entry);
        }
    } else if (!inMSHR) {
        profileRequestRecv(event, entry);
    }

    if (!cached) {

        if (is_debug_addr(entry->getBaseAddr())) dbg.debug(_L6_, "Entry %" PRIx64 " not in cache.  Requesting from memory.\n", entry->getBaseAddr());

        retrieveDirEntry(entry, event, true);
        return;
    }

    switch (state) {
        case I:
            entry->setState(IS);
            issueMemoryRequest(event, entry);
            break;
        case S:
            entry->setState(S_D);
            issueMemoryRequest(event, entry);
            break;
        case M:
            entry->setState(M_InvX);
            issueFetch(event, entry, Command::FetchInvX);
            break;
        default:
            dbg.fatal(CALL_INFO, -1, "Directory %s received GetS but state is %s. Event: %s\n", getName().c_str(), StateString[state], event->getVerboseString().c_str());
    }
}


/** GetX */
void DirectoryController::handleGetX(MemEvent * event, bool replay) {
    Addr addr = event->getBaseAddr();
    DirEntry * entry = getDirEntry(addr);
    State state = entry->getState();

    /* Put request in MSHR (all GetS requests need to be buffered while they wait for data) and stall if there are waiting requests ahead of us */
    if (!(mshr->elementIsHit(addr, event))) {
        bool conflict = mshr->isHit(addr);
        if (!mshr->insert(addr, event)) {
            mshrNACKRequest(event);
            return;
        }
        if (conflict) {
            return; // stall event until conflicting request finishes
        } else {
            profileRequestRecv(event, entry);
        }
    } else if (!replay)
        profileRequestRecv(event, entry);

    if (!entry->isCached()) {
        if (is_debug_addr(entry->getBaseAddr())) dbg.debug(_L6_, "Entry %" PRIx64 " not in cache.  Requesting from memory.\n", entry->getBaseAddr());

        retrieveDirEntry(entry, event, true);
        return;
    }

    MemEvent * respEv;
    switch (state) {
        case I:
            entry->setState(IM);
            issueMemoryRequest(event, entry);
            break;
        case S:
            if (entry->getSharerCount() == 1 && entry->isSharer(node_id(event->getSrc()))) {   // Special case: upgrade
                mshr->removeFront(event->getBaseAddr());

                if (is_debug_event(event)) dbg.debug(_L10_, "\t%s\tMSHR remove event <%s, %" PRIx64 ">\n", getName().c_str(), CommandString[(int)event->getCmd()], addr);

                entry->setState(M);
                entry->removeSharer(node_name_to_id(event->getSrc()));
                entry->setOwner(node_name_to_id(event->getSrc()));
                respEv = event->makeResponse();
                respEv->setSize(cacheLineSize);
                profileResponseSent(respEv);
                sendEventToCaches(respEv, timestamp + accessLatency);

                if (is_debug_event(event)) {
                    dbg.debug(_L4_, "Sending response for 0x%" PRIx64 " to %s, send time: %" PRIu64 "\n", entry->getBaseAddr(), respEv->getDst().c_str(), timestamp + accessLatency);
                }

                postRequestProcessing(event, entry, true);
                replayWaitingEvents(entry->getBaseAddr());
                updateCache(entry);
            } else {
                entry->setState(S_Inv);
                issueInvalidates(event, entry, Command::Inv);
            }
            break;
        case M:
            entry->setState(M_Inv);
            issueFetch(event, entry, Command::FetchInv);
            break;
        default:
            dbg.fatal(CALL_INFO, -1, "Directory %s received %s but state is %s. Event: %s\n", getName().c_str(), CommandString[(int)event->getCmd()], StateString[state], event->getVerboseString().c_str());
    }
}


/*
 * Handle PutS request - either a request or a response to an Inv
 */
void DirectoryController::handlePutS(MemEvent * event) {
    Addr addr = event->getBaseAddr();
    DirEntry * entry = getDirEntry(addr);
    State state = entry->getState();

    entry->removeSharer(node_name_to_id(event->getSrc()));
    if (mshr->elementIsHit(addr, event)) 
        mshr->removeElement(addr, event);

    switch (state) {
        case S:
            profileRequestRecv(event, entry);
            if (entry->getSharerCount() == 0) {
                entry->setState(I);
            }
            sendAckPut(event);
            postRequestProcessing(event, entry, true);   // profile & delete ev
            replayWaitingEvents(addr);
            updateCache(entry);             // update dir cache
            break;
        case S_D:
            profileRequestRecv(event, entry);
            sendAckPut(event);
            postRequestProcessing(event, entry, false);
            break;
        case S_Inv:
        case SD_Inv:
            profileResponseRecv(event);
            entry->decrementWaitingAcks();
            delete event;
            if (entry->getWaitingAcks() == 0) {
                if (state == S_Inv) (entry->getSharerCount() > 0) ? entry->setState(S) : entry->setState(I);
                else (entry->getSharerCount() > 0) ? entry->setState(S_D) : entry->setState(IS);
                replayWaitingEvents(addr);
            }
            break;
        default:
            dbg.fatal(CALL_INFO, -1, "%s, Error: Directory received PutS but state is %s. Event = %s. Time = %" PRIu64 "ns\n",
                    getName().c_str(), StateString[state], event->getVerboseString().c_str(), getCurrentSimTimeNano());
    }
}


/* Handle PutE */
void DirectoryController::handlePutE(MemEvent * event) {
    Addr addr = event->getBaseAddr();
    DirEntry * entry = getDirEntry(addr);

    /* Error checking */
    if (!((uint32_t)entry->getOwner() == node_name_to_id(event->getSrc()))) {
	dbg.fatal(CALL_INFO, -1, "%s, Error: received PutE from a node who does not own the block. Event = %s. Time = %" PRIu64 "ns\n",
                getName().c_str(), event->getVerboseString().c_str(), getCurrentSimTimeNano());
    }

    if (!(entry->isCached())) {
        if (!(mshr->elementIsHit(addr, event))) {
            stat_mshrHits->addData(1);
            bool inserted = mshr->insert(addr, event);

            if (is_debug_event(event)) {
                dbg.debug(_L8_, "Inserting request in mshr. %s. MSHR size: %d\n", event->getBriefString().c_str(), mshr->getSize());
            }

            if (!inserted) {
                if (is_debug_event(event)) dbg.debug(_L8_, "MSHR is full. NACKing request\n");

                mshrNACKRequest(event);
                return;
            }
        }

        if (is_debug_event(event)) dbg.debug(_L6_, "Entry %" PRIx64 " not in cache.  Requesting from memory.\n", entry->getBaseAddr());

        retrieveDirEntry(entry, event, true);
        return;
    }

    /* Update owner state */
    profileRequestRecv(event, entry);
    entry->clearOwner();

    State state = entry->getState();
    switch  (state) {
        case M:
            entry->setState(I);
            sendAckPut(event);
            postRequestProcessing(event, entry, true);  // profile & delete ev
            replayWaitingEvents(entry->getBaseAddr());
            updateCache(entry);         // update cache;
            break;
        case M_Inv:     /* If PutE comes with data then we can handle this immediately but otherwise */
            entry->setState(IM);
            issueMemoryRequest(static_cast<MemEvent*>(mshr->lookupFront(addr)), entry);
            postRequestProcessing(event, entry, false);  // profile & delete ev
            break;
        case M_InvX:
            entry->setState(IS);
            issueMemoryRequest(static_cast<MemEvent*>(mshr->lookupFront(addr)), entry);
            postRequestProcessing(event, entry, false);  // profile & delete ev
            break;
        default:
            dbg.fatal(CALL_INFO, -1, "%s, Error: Directory received PutE but state is %s. Event: %s. Time = %" PRIu64 "ns\n",
                    getName().c_str(), StateString[state], event->getVerboseString().c_str(), getCurrentSimTimeNano());
    }
}


/* Handle PutM */
void DirectoryController::handlePutM(MemEvent * event) {
    Addr addr = event->getBaseAddr();
    DirEntry * entry = getDirEntry(addr);
    State state = entry->getState();

    /* Error checking */
    if (!((uint32_t)entry->getOwner() == node_name_to_id(event->getSrc()))) {
	dbg.fatal(CALL_INFO, -1, "%s, Error: received PutM from a node who does not own the block. Event: %s. Time = %" PRIu64 "ns\n",
                getName().c_str(), event->getVerboseString().c_str(), getCurrentSimTimeNano());
    }

    if (!(entry->isCached())) {
        if (!(mshr->elementIsHit(addr, event))) {
            stat_mshrHits->addData(1);
            bool inserted = mshr->insert(addr, event);

            if (is_debug_event(event)) {
                dbg.debug(_L8_, "Inserting request in mshr. Cmd = %s, BaseAddr = 0x%" PRIx64 ", Addr = 0x%" PRIx64 ", MSHR size: %d\n", CommandString[(int)event->getCmd()], addr, event->getAddr(), mshr->getSize());
            }

            if (!inserted) {
                if (is_debug_event(event)) dbg.debug(_L8_, "MSHR is full. NACKing request\n");

                mshrNACKRequest(event);
                return;
            }
        }

        if (is_debug_event(event)) dbg.debug(_L6_, "Entry %" PRIx64 " not in cache.  Requesting from memory.\n", entry->getBaseAddr());

        retrieveDirEntry(entry, event, true);
        return;
    }

    switch  (state) {
        case M:
            profileRequestRecv(event, entry);
            writebackData(event, Command::PutM);
            entry->clearOwner();
            entry->setState(I);
            sendAckPut(event);
            postRequestProcessing(event, entry, true);  // profile & delete event
            replayWaitingEvents(entry->getBaseAddr());
            updateCache(entry);
            break;
        case M_Inv:
        case M_InvX:
            handleFetchResp(event, false);
            break;
        default:
            dbg.fatal(CALL_INFO, -1, "%s, Error: Directory received PutM but state is %s. Event: %s. Time = %" PRIu64 "ns, %" PRIu64 " cycles.\n",
                    getName().c_str(), StateString[state], event->getVerboseString().c_str(), getCurrentSimTimeNano(), timestamp);
    }
}


/*
 *  Handle FlushLine
 *  Only flush dirty data
 */
void DirectoryController::handleFlushLine(MemEvent * event) {
    Addr addr = event->getBaseAddr();
    DirEntry * entry = getDirEntry(addr);
    if (!entry->isCached()) {

        if (is_debug_addr(entry->getBaseAddr())) dbg.debug(_L6_, "Entry %" PRIx64 " not in cache.  Requesting from memory.\n", entry->getBaseAddr());

        retrieveDirEntry(entry, event, true);
        return;
    }

    bool shouldNACK = false;
    bool inMSHR = mshr->elementIsHit(addr, event);
    bool mshrConflict = !inMSHR && mshr->isHit(addr);

    int srcID = node_id(event->getSrc());
    State state = entry->getState();

    switch(state) {
        case I:
            if (!inMSHR) {
                if (!mshr->insert(addr, event)) {
                    mshrNACKRequest(event);
                    break;
                } else profileRequestRecv(event, entry);
                if (mshrConflict) break;
            }
            forwardFlushRequest(event);
            break;
        case S:
            if (!inMSHR && !mshr->insert(addr, event)) {
                mshrNACKRequest(event);
                break;
            }
            forwardFlushRequest(event);
            break;
        case M:
            if (!inMSHR && !mshr->insert(addr, event)) {
                mshrNACKRequest(event);
                break;
            }
            if (entry->getOwner() == srcID) {
                entry->clearOwner();
                entry->addSharer(srcID);
                entry->setState(S);
                if (event->getDirty()) {
                    writebackData(event, Command::FlushLine);
                } else {
                    forwardFlushRequest(event);
                }
            } else {
                entry->setState(M_InvX);
                issueFetch(event, entry, Command::FetchInvX);
            }
            break;
        case IS:        // Sharer in progress
        case IM:        // Owner in progress
        case S_D:       // Sharer in progress
        case SD_Inv:    // Sharer and inv in progress
        case S_Inv:     // Owner in progress, waiting on AckInv
            if (!inMSHR && !mshr->insert(addr, event)) mshrNACKRequest(event);
            break;
        case M_Inv:     // Owner in progress, waiting on FetchInvResp
            // Assume that the sender will treat our outstanding FetchInv as an Inv and clean up accordingly
            if (entry->getOwner() == srcID) {
                entry->clearOwner();
                entry->addSharer(srcID);
                entry->setState(S_Inv);
                entry->incrementWaitingAcks();
                if (event->getDirty()) {
                    writebackData(event, Command::PutM);
                    event->setPayload(0, NULL);
                    event->setDirty(false);
                }
            }
            if (!inMSHR && !mshr->insert(addr, event)) mshrNACKRequest(event);
            break;
        case M_InvX:    // Sharer in progress, waiting on FetchInvXResp
            // Clear owner, add owner as sharer
            if (entry->getOwner() == srcID) {
                if (event->getDirty()) {
                    handleFetchXResp(event, true);
                    event->setPayload(0, NULL);
                    event->setDirty(false);
                } else {
                    entry->clearOwner();
                    entry->addSharer(srcID);
                    entry->setState(S);
                }
                if (!inMSHR && !mshr->insert(addr, event)) mshrNACKRequest(event);
                replayWaitingEvents(addr);
                updateCache(entry);
            } else if (!inMSHR && !mshr->insert(addr, event)) mshrNACKRequest(event);
            break;
        default:
            dbg.fatal(CALL_INFO, -1, "%s, Error: Directory received FlushLine but state is %s. Event: %s. Time = %" PRIu64 "ns\n",
                    getName().c_str(), StateString[state], event->getVerboseString().c_str(), getCurrentSimTimeNano());
    }
}


/*
 *  Handle FlushLineInv
 *  Wait for mem ack if dirty, else local OK.
 */
void DirectoryController::handleFlushLineInv(MemEvent * event) {
    Addr addr = event->getBaseAddr();
    DirEntry * entry = getDirEntry(addr);

    if (!entry->isCached()) {
        if (is_debug_addr(addr)) dbg.debug(_L6_, "Entry %" PRIx64 " not in cache.  Requesting from memory.\n", addr);

        retrieveDirEntry(entry, event, true);
        return;
    }

    bool shouldNACK = false;
    bool inMSHR = mshr->elementIsHit(addr, event);
    bool mshrConflict = !inMSHR && mshr->isHit(addr);

    int srcID = node_id(event->getSrc());
    State state = entry->getState();

    switch (state) {
        case I:
            if (!inMSHR) {
                if (!mshr->insert(addr, event)) {
                    mshrNACKRequest(event);
                    break;
                } else profileRequestRecv(event, entry);
                if (mshrConflict) 
                    break;
            }
            forwardFlushRequest(event);
            break;
        case S:
            if (!inMSHR && !mshr->insert(addr, event)) {
                mshrNACKRequest(event);
                break;
            }
            if (entry->isSharer(srcID)) entry->removeSharer(srcID);
            if (entry->getSharerCount() == 0) {
                entry->setState(I);
                forwardFlushRequest(event);
            } else {
                entry->setState(S_Inv);
                issueInvalidates(event, entry, Command::Inv);
            }
            break;
        case M:
            if (!inMSHR && !mshr->insert(addr, event)) {
                mshrNACKRequest(event);
                break;
            }
            if (entry->getOwner() == srcID) {
                entry->clearOwner();
                entry->setState(I);
                if (event->getDirty()) {
                    writebackData(event, Command::FlushLine);
                } else {
                    forwardFlushRequest(event);
                }
            } else {
                entry->setState(M_Inv);
                issueFetch(event, entry, Command::FetchInv);
            }
            break;
        case IS:        // Sharer in progress
        case IM:        // Owner in progress
        case S_D:       // Sharer in progress
            if (!inMSHR && !mshr->insert(addr, event)) mshrNACKRequest(event);
            break;
        case S_Inv:     // Owner in progress, waiting on AckInv
        case SD_Inv:
            if (!inMSHR && !mshr->insert(addr, event)) mshrNACKRequest(event);
            if (entry->isSharer(srcID)) {
                profileResponseRecv(event);
                entry->removeSharer(srcID);
                entry->decrementWaitingAcks();
                if (entry->getWaitingAcks() == 0) {
                    if (state == S_Inv) entry->getSharerCount() > 0 ? entry->setState(S) : entry->setState(I);
                    else entry->getSharerCount() > 0 ? entry->setState(S_D) : entry->setState(IS);
                    replayWaitingEvents(addr);
                    updateCache(entry);
                }
            }
            break;
        case M_Inv:     // Owner in progress, waiting on FetchInvResp
        case M_InvX:    // Sharer in progress, waiting on FetchInvXResp
            if (entry->getOwner() == srcID) {
                if (event->getDirty()) {
                    handleFetchResp(event, true); // don't delete the event! we need it!
                    event->setDirty(false);        // Don't writeback data when event is replayed
                    event->setPayload(0, NULL);
                } else {
                    entry->clearOwner();
                    // Don't have data, set state to I & retry
                    entry->setState(I);
                }
                if (!inMSHR && !mshr->insert(addr, event)) mshrNACKRequest(event);
                replayWaitingEvents(entry->getBaseAddr());
                updateCache(entry);
            } else if (!inMSHR && !mshr->insert(addr, event)) mshrNACKRequest(event);
            break;
        default:
            dbg.fatal(CALL_INFO, -1, "%s, Error: Directory received FlushLineInv but state is %s. Event: %s. Time = %" PRIu64 "ns\n",
                    getName().c_str(), StateString[state], event->getVerboseString().c_str(), getCurrentSimTimeNano());
    }
}



/* Handle the incoming event as a fetch Response (FetchResp, FetchXResp, PutM) */
void DirectoryController::handleFetchResp(MemEvent * event, bool keepEvent) {
    Addr addr = event->getBaseAddr();
    DirEntry * entry = getDirEntry(addr);
    MemEvent * reqEv = mshr->removeFront(addr);

    if (is_debug_event(event)) dbg.debug(_L4_, "Finishing Fetch for reqEv = %s.\n", reqEv->getBriefString().c_str());

    /* Error checking */
    if (!((uint32_t)entry->getOwner() == node_name_to_id(event->getSrc()))) {
	dbg.fatal(CALL_INFO, -1, "%s, Error: received FetchResp from a node who does not own the block. Event: %s. Time = %" PRIu64 "ns, %" PRIu64 " cycles\n",
                getName().c_str(), event->getVerboseString().c_str(), getCurrentSimTimeNano(), timestamp);
    }

    /* Profile response */
    profileResponseRecv(event);

    /* Clear previous owner state and writeback block. */
    entry->clearOwner();

    MemEvent * respEv = nullptr;
    State state = entry->getState();

    /* Handle request */
    switch (state) {
        case M_Inv:
            if (reqEv->getCmd() != Command::FetchInv && reqEv->getCmd() != Command::ForceInv) { // GetX request, not back invalidation
                writebackData(event, Command::PutM);
                entry->setOwner(node_id(reqEv->getSrc()));
                entry->setState(M);
            } else entry->setState(I);
            respEv = reqEv->makeResponse();
            break;
        case M_InvX:    // GetS request
            if (reqEv->getCmd() == Command::FetchInv || reqEv->getCmd() == Command::ForceInv) {
                // TODO should probably just handle the Get* that caused a FetchInvX first and then do the FetchInv/ForceInv
                // Actually should only do FetchInv/ForceInv first if it required a request to memory (IS or IM) -> everything else should
                // have the FetchInv/ForceInv go second
            }
            writebackData(event, Command::PutM);
            if (protocol == CoherenceProtocol::MESI && entry->getSharerCount() == 0) {
                entry->setOwner(node_id(reqEv->getSrc()));
                respEv = reqEv->makeResponse(Command::GetXResp);
                entry->setState(M);
            } else {
                entry->addSharer(node_id(reqEv->getSrc()));
                respEv = reqEv->makeResponse();
                entry->setState(S);
            }
            break;
        default:
            dbg.fatal(CALL_INFO, -1, "%s, Error: Directory received %s but state is %s. Event: %s. Time = %" PRIu64 "ns, %" PRIu64 " cycles\n",
                    getName().c_str(), CommandString[(int)event->getCmd()], StateString[state], event->getVerboseString().c_str(), getCurrentSimTimeNano(), timestamp);
    }
    respEv->setPayload(event->getPayload());
    profileResponseSent(respEv);
    if (reqEv->getCmd() == Command::FetchInv || reqEv->getCmd() == Command::ForceInv)
        memMsgQueue.insert(std::make_pair(timestamp + mshrLatency, respEv));
    else
        sendEventToCaches(respEv, timestamp + mshrLatency);

    if (!keepEvent) delete event;

    postRequestProcessing(reqEv, entry, true);
    replayWaitingEvents(entry->getBaseAddr());
    updateCache(entry);
}


/* FetchXResp */
void DirectoryController::handleFetchXResp(MemEvent * event, bool keepEvent) {
    if (is_debug_event(event)) dbg.debug(_L4_, "Finishing Fetch.\n");

    Addr addr = event->getBaseAddr();
    DirEntry * entry = getDirEntry(addr);
    MemEvent * reqEv = mshr->removeFront(addr);

    /* Error checking */
    if (!((uint32_t)entry->getOwner() == node_name_to_id(event->getSrc()))) {
	dbg.fatal(CALL_INFO, -1, "%s, Error: received FetchResp from a node who does not own the block. Event = %s. Time = %" PRIu64 "ns, %" PRIu64 " cycles\n",
                getName().c_str(), event->getVerboseString().c_str(), getCurrentSimTimeNano(), timestamp );
    }

    /* Profile response */
    profileResponseRecv(event);

    /* Clear previous owner state and writeback block. */
    entry->clearOwner();
    entry->addSharer(node_name_to_id(event->getSrc()));
    entry->setState(S);
    if (event->getDirty()) writebackData(event, Command::PutM);

    MemEvent * respEv = reqEv->makeResponse();
    entry->addSharer(node_id(reqEv->getSrc()));

    respEv->setPayload(event->getPayload());
    profileResponseSent(respEv);
    sendEventToCaches(respEv, timestamp + mshrLatency);

    if (!keepEvent) delete event;

    postRequestProcessing(reqEv, entry, true);
    replayWaitingEvents(entry->getBaseAddr());
    updateCache(entry);

}


/* AckInv */
void DirectoryController::handleAckInv(MemEvent * event) {
    Addr addr = event->getBaseAddr();
    DirEntry * entry = getDirEntry(addr);
    State state = entry->getState();

    if (entry->isSharer(node_id(event->getSrc())))
        entry->removeSharer(node_name_to_id(event->getSrc()));
    if ((uint32_t)entry->getOwner() == node_name_to_id(event->getSrc()))
        entry->clearOwner();

    if (mshr->elementIsHit(addr, event)) mshr->removeElement(addr, event);

    switch (state) {
        case S_Inv:
        case SD_Inv:
            profileResponseRecv(event);
            entry->decrementWaitingAcks();
            delete event;
            if (entry->getWaitingAcks() == 0) {
                if (state == S_Inv) entry->getSharerCount() > 0 ? entry->setState(S) : entry->setState(I);
                else entry->getSharerCount() > 0 ? entry->setState(S_D) : entry->setState(IS);
                replayWaitingEvents(addr);
            }
            break;
        case M_Inv:
            profileResponseRecv(event);
            delete event;
            entry->setState(I);
            replayWaitingEvents(addr);
            break;
        default:
            dbg.fatal(CALL_INFO, -1, "%s, Error: Directory received AckInv but state is %s. Event = %s. Time = %" PRIu64 "ns\n",
                    getName().c_str(), StateString[state], event->getVerboseString().c_str(), getCurrentSimTimeNano());
    }
}


/*
 *  Handle a backwards invalidation (shootdown)
 */
void DirectoryController::handleBackInv(MemEvent* event) {
    Addr addr = event->getBaseAddr();
    DirEntry * entry = getDirEntry(addr);

    /* Put invalidation in mshr if needed -> resolve conflicts
     * so that invalidation takes precedence over
     * memory requests */
    bool noreplay = false;
    if (!(mshr->elementIsHit(addr, event))) {
        /*
         *  GetS/X/etc: Jump ahead and resolve now
         *  Flush: to-do
         *  PutE,M: Wait, only happens when the dir entry i snot cached
         *  Could also be WB Ack
         */
        bool conflict = mshr->isHit(addr);
        bool inserted = false;
        if (conflict) {
            if (mshr->exists(addr)) { // front entry is an event
                /* If state is IS, IM, or S_D, we are waiting on memory so resolve this Inv first to avoid deadlock */
                if (entry->getState() == IS || entry->getState() == IM || entry->getState() == S_D) {
                    inserted = mshr->insertInv(addr, event, false); // insert front
                    conflict = false; // don't stall
                    noreplay = true; // don't replay second event if this one completes
                } else {
                    inserted = mshr->insertInv(addr, event, true); // insert second
                }
            } else {
                inserted = mshr->insertInv(addr, event, true); // insert second
            }
        } else {
            inserted = mshr->insert(addr, event);
        }

        if (!inserted) {
            mshrNACKRequest(event, true);
            return;
        }
        if (conflict) return; // stall until conflicting request finishes
        else profileRequestRecv(event, entry);
    }

    if (!entry->isCached()) {
        if (is_debug_addr(entry->getBaseAddr())) dbg.debug(_L6_, "Entry %" PRIx64 " not in cache.  Requesting from memory.\n", entry->getBaseAddr());

        retrieveDirEntry(entry, event, true);
        return;
    }

    MemEvent * respEv;
    State state = entry->getState();
    switch (state) {
        case I:
        case IS:
        case IM:
            respEv = event->makeResponse(Command::AckInv);
            respEv->setDst(memoryName);
            memMsgQueue.insert(std::make_pair(timestamp + accessLatency, respEv));
            if (mshr->elementIsHit(addr, event)) mshr->removeElement(addr, event);
            if (!noreplay) replayWaitingEvents(entry->getBaseAddr());
            break;
        case S:
            entry->setState(S_Inv);
            issueInvalidates(event, entry, Command::Inv); // Fwd inv for forceInv and fetchInv
            break;
        case M:
            entry->setState(M_Inv);
            issueFetch(event, entry, event->getCmd());
            break;
        case S_D:
            // Need to order inv ahead of read and end up in IS; won't get data response
            // until we end up in IS so no need to figure out races
            entry->setState(SD_Inv);
            issueInvalidates(event, entry, Command::Inv);
            break;
        case I_d:
        case S_d:
        case M_d:
            break; // Stall for entry response
        default:
            dbg.fatal(CALL_INFO, -1, "%s, Error: No handler for event in state %s. Event = %s. Time = %" PRIu64 "ns\n",
                    getName().c_str(), StateString[state], event->getVerboseString().c_str(), getCurrentSimTimeNano());
    }
}


/* handle NACK */
void DirectoryController::handleNACK(MemEvent * event) {
    MemEvent* origEvent = event->getNACKedEvent();
    profileResponseRecv(event);

    DirEntry *entry = getDirEntry(origEvent->getBaseAddr());
    if (is_debug_event(event)) {
        dbg.debug(_L5_, "Orig resp ID = (%" PRIu64 ",%d), Nack resp ID = (%" PRIu64 ",%d), last req ID = (%" PRIu64 ",%d)\n",
	        origEvent->getResponseToID().first, origEvent->getResponseToID().second, event->getResponseToID().first,
        	event->getResponseToID().second, entry->lastRequest.first, entry->lastRequest.second);
    }

    /* Retry request if it has not already been handled */
    if ((event->getResponseToID() == entry->lastRequest) || origEvent->getCmd() == Command::Inv) {
	/* Re-send request */
	sendEventToCaches(origEvent, timestamp + mshrLatency);

        if (is_debug_event(event)) dbg.debug(_L5_,"Orig Cmd NACKed, retry = %s \n", CommandString[(int)origEvent->getCmd()]);
    } else {
	if (is_debug_event(event)) dbg.debug(_L5_,"Orig Cmd NACKed, no retry = %s \n", CommandString[(int)origEvent->getCmd()]);
    }

    delete event;
    return;
}


/* Memory response handler - calls handler for each event from memory */
void DirectoryController::handleMemoryResponse(SST::Event *event){
    MemEvent *ev = static_cast<MemEvent*>(event);

    if (is_debug_event(ev)) {
        dbg.debug(_L3_, "\n%" PRIu64 " (%s) Received: %s\n",
                getCurrentSimTimeNano(), getName().c_str(), ev->getVerboseString().c_str());
    }

    if (!clockOn) {
        turnClockOn();
    }

    if (ev->queryFlag(MemEvent::F_NONCACHEABLE)) {
        if (noncacheMemReqs.find(ev->getResponseToID()) != noncacheMemReqs.end()) {
            ev->setDst(noncacheMemReqs[ev->getResponseToID()]);
            ev->setSrc(getName());

            noncacheMemReqs.erase(ev->getResponseToID());
            profileResponseSent(ev);

            network->send(ev);

            return;
        }
        dbg.fatal(CALL_INFO, -1, "%s, Error: Received unexpected noncacheable response from memory. %s. Time = %" PRIu64 "ns\n",
                getName().c_str(), ev->getVerboseString().c_str(), getCurrentSimTimeNano());
    }

    /* Cacheable response - figure out what it is for */
    if (ev->getCmd() == Command::ForceInv || ev->getCmd() == Command::FetchInv) {
        handleBackInv(ev);
        return; // don't delete
    } else if (ev->getCmd() == Command::AckPut) {
        mshr->removeWriteback(memReqs[ev->getResponseToID()]);
        memReqs.erase(ev->getResponseToID());
        replayWaitingEvents(ev->getBaseAddr());
    } else if (ev->getBaseAddr() == 0 && dirEntryMiss.find(ev->getResponseToID()) != dirEntryMiss.end()) {    // directory entry miss
        ev->setBaseAddr(dirEntryMiss[ev->getResponseToID()]);
        dirEntryMiss.erase(ev->getResponseToID());
        handleDirEntryMemoryResponse(ev);
    } else if (memReqs.find(ev->getResponseToID()) != memReqs.end()) {
        ev->setBaseAddr(memReqs[ev->getResponseToID()]);
        memReqs.erase(ev->getResponseToID());
        if (ev->getCmd() == Command::FlushLineResp) handleFlushLineResponse(ev);
        else handleDataResponse(ev);

    } else {
        dbg.fatal(CALL_INFO, -1, "%s, Error: Received unexpected response from memory - matching request not found. %s. Time = %" PRIu64 "ns\n",
                getName().c_str(), ev->getVerboseString().c_str(), getCurrentSimTimeNano());
    }

    delete ev;
}


/* Handle GetSResp or GetXResp from memory */
void DirectoryController::handleDataResponse(MemEvent * event) {
    Addr addr = event->getBaseAddr();
    DirEntry * entry = getDirEntry(addr);

    State state = entry->getState();

    MemEvent * reqEv = mshr->removeFront(addr);
    if (is_debug_event(event)) dbg.debug(_L10_, "\t%s\tMSHR remove event <%s, %" PRIx64 ">\n", getName().c_str(), CommandString[(int)reqEv->getCmd()], reqEv->getBaseAddr());

    MemEvent * respEv = nullptr;
    switch (state) {
        case IS:
        case S_D:
            if (protocol == CoherenceProtocol::MESI && entry->getSharerCount() == 0) {
                respEv = reqEv->makeResponse(Command::GetXResp);
                entry->setState(M);
                entry->setOwner(node_id(reqEv->getSrc()));
            } else {
                respEv = reqEv->makeResponse();
                entry->setState(S);
                entry->addSharer(node_id(reqEv->getSrc()));
            }
            break;
        case IM:
        case SM:
            respEv = reqEv->makeResponse();
            entry->setState(M);
            entry->setOwner(node_id(reqEv->getSrc()));
            entry->clearSharers();  // Case SM: new owner was a sharer
            break;
        default:
            dbg.fatal(CALL_INFO,1,"Directory %s received Get Response for addr 0x%" PRIx64 " but state is %s. Event: %s\n",
                    getName().c_str(), addr, StateString[state], event->getVerboseString().c_str());
    }

    respEv->setSize(cacheLineSize);
    respEv->setPayload(event->getPayload());
    respEv->setMemFlags(event->getMemFlags());
    profileResponseSent(respEv);
    sendEventToCaches(respEv, timestamp + mshrLatency);

    if (is_debug_event(event)) {
        dbg.debug(_L4_, "\tSending requested data for 0x%" PRIx64 " to %s\n", entry->getBaseAddr(), respEv->getDst().c_str());
    }

    postRequestProcessing(reqEv, entry, true);
    replayWaitingEvents(entry->getBaseAddr());
    updateCache(entry);
}


/* Handle FlushLineResp from memory */
void DirectoryController::handleFlushLineResponse(MemEvent * event) {
    Addr addr = event->getBaseAddr();
    MemEvent * reqEv = mshr->removeFront(addr);

    if (is_debug_event(event)) dbg.debug(_L10_, "\t%s\tMSHR remove event <%s, %" PRIx64 ">\n", getName().c_str(), CommandString[(int)reqEv->getCmd()], reqEv->getBaseAddr());

    reqEv->setMemFlags(event->getMemFlags()); // Copy anything back up that needs to be

    MemEvent * me = reqEv->makeResponse();
    me->setDst(reqEv->getSrc());
    me->setRqstr(reqEv->getRqstr());
    me->setSuccess(event->queryFlag(MemEvent::F_SUCCESS));
    me->setMemFlags(reqEv->getMemFlags());

    profileResponseSent(me);
    uint64_t deliveryTime = timestamp + accessLatency;
    netMsgQueue.insert(std::make_pair(deliveryTime, me));

    replayWaitingEvents(reqEv->getBaseAddr());

    delete reqEv;
}


/* Handle response for directory entry */
void DirectoryController::handleDirEntryMemoryResponse(MemEvent * event) {
    Addr dirAddr = event->getBaseAddr();

    DirEntry * entry = getDirEntry(dirAddr);
    entry->setCached(true);

    State st = entry->getState();
    switch (st) {
        case I_d:
            entry->setState(I);
            break;
        case S_d:
            entry->setState(S);
            break;
        case M_d:
            entry->setState(M);
            break;
        default:
            dbg.fatal(CALL_INFO, -1, "Directory Controller %s: DirEntry response received for addr 0x%" PRIx64 " but state is %s. Event: %s\n",
                    getName().c_str(), entry->getBaseAddr(), StateString[st], event->getVerboseString().c_str());
    }
    MemEvent * reqEv = static_cast<MemEvent*>(mshr->lookupFront(dirAddr));
    processPacket(reqEv, true);
}









void DirectoryController::issueInvalidates(MemEvent * ev, DirEntry * entry, Command cmd) {
    uint32_t rqst_id = node_id(ev->getSrc());
    for (uint32_t i = 0; i < numTargets; i++) {
        if (i == rqst_id) continue;
        if (entry->isSharer(i)) {
            sendInvalidate(i, ev, entry, cmd);
            entry->incrementWaitingAcks();
        }
    }
    entry->lastRequest = DirEntry::NO_LAST_REQUEST;

    if (is_debug_addr(entry->getBaseAddr())) dbg.debug(_L4_, "Sending Invalidates to fulfill request for exclusive, BsAddr = %" PRIx64 ".\n", entry->getBaseAddr());
}

/* Send Fetch to owner */
void DirectoryController::issueFetch(MemEvent * ev, DirEntry * entry, Command cmd) {
    MemEvent * fetch = new MemEvent(getName(), ev->getAddr(), ev->getBaseAddr(), cmd, cacheLineSize);
    fetch->setDst(nodeid_to_name[entry->getOwner()]);
    entry->lastRequest = fetch->getID();
    profileRequestSent(fetch);
    sendEventToCaches(fetch, timestamp + accessLatency);
}

/* Send Get* request to memory */
void DirectoryController::issueMemoryRequest(MemEvent * ev, DirEntry * entry) {
    MemEvent *reqEv         = new MemEvent(*ev);
    reqEv->setSrc(getName());
    reqEv->setDst(memoryName);
    memReqs[reqEv->getID()] = ev->getBaseAddr();
    profileRequestSent(reqEv);

    uint64_t deliveryTime = timestamp + accessLatency;

    memMsgQueue.insert(std::make_pair(deliveryTime, reqEv));

    if (is_debug_addr(entry->getBaseAddr())) {
        dbg.debug(_L5_, "\tRequesting data from memory.  Cmd = %s, BaseAddr = x%" PRIx64 ", Size = %u, noncacheable = %s\n",
                CommandString[(int)reqEv->getCmd()], reqEv->getBaseAddr(), reqEv->getSize(), reqEv->queryFlag(MemEvent::F_NONCACHEABLE) ? "true" : "false");
    }
}


/* Send request for directory entry to memory */
void DirectoryController::retrieveDirEntry(DirEntry * entry, MemEvent* event, bool inMSHR) {
    State st = entry->getState();
    switch (st) {
        case I:
            entry->setState(I_d);
            break;
        case S:
            entry->setState(S_d);
            break;
        case M:
            entry->setState(M_d);
            break;
        case I_d:
        case S_d:
        case M_d:
            return; // Already requested, just stall
        default:
            dbg.fatal(CALL_INFO,-1,"Direcctory Controller %s: cache miss for addr 0x%" PRIx64 " but state is %s\n",getName().c_str(),entry->getBaseAddr(), StateString[st]);
    }

    Addr entryAddr       = 0; /* Dummy addr reused for dir cache misses */
    MemEvent *me         = new MemEvent(getName(), entryAddr, entryAddr, Command::GetS, cacheLineSize);
    me->setAddrGlobal(false);
    me->setSize(entrySize);
    me->setDst(memoryName);
    dirEntryMiss[me->getID()] = entry->getBaseAddr();
    profileRequestSent(me);

    uint64_t deliveryTime = timestamp + accessLatency;

    memMsgQueue.insert(std::make_pair(deliveryTime, me));

    if (is_debug_addr(entry->getBaseAddr())) dbg.debug(_L10_, "Requesting Entry from memory for 0x%" PRIx64 "(%" PRIu64 ", %d)\n", entry->getBaseAddr(), me->getID().first, me->getID().second);
}



void DirectoryController::mshrNACKRequest(MemEvent* ev, bool mem) {
    MemEvent * nackEv = ev->makeNACKResponse(ev);
    profileResponseSent(nackEv);
    if (mem)
        memMsgQueue.insert(std::make_pair(timestamp + 1, nackEv));
    else
        sendEventToCaches(nackEv, timestamp + 1);
}

void DirectoryController::printStatus(Output &statusOut) {
    statusOut.output("MemHierarchy::DirectoryController %s\n", getName().c_str());
    statusOut.output("  Cached entries: %" PRIu64 "\n", entryCacheSize);
    statusOut.output("  Requests waiting to be handled:  %zu\n", workQueue.size());
    for(std::list<std::pair<MemEvent*,bool> >::iterator i = workQueue.begin() ; i != workQueue.end() ; ++i){
        statusOut.output("    %s, %s\n", i->first->getVerboseString().c_str(), i->second ? "replay" : "new");
    }

    if (mshr) {
        statusOut.output("  MSHR Status:\n");
        mshr->printStatus(statusOut);
    }

    if (network) {
        statusOut.output("  NIC Status: ");
        network->printStatus(statusOut);
    }

    if (memLink) {
        statusOut.output("  Memory Link Status: ");
        memLink->printStatus(statusOut);
    }

    statusOut.output("  Directory entries:\n");
    for (std::unordered_map<Addr, DirEntry*>::iterator it = directory.begin(); it != directory.end(); it++) {
        statusOut.output("    0x%" PRIx64 " %s\n", it->first, it->second->getString().c_str());
    }
    statusOut.output("End MemHierarchy::DirectoryController\n\n");
}

void DirectoryController::emergencyShutdown() {
    if (out.getVerboseLevel() > 1) {
        if (out.getOutputLocation() == Output::STDOUT)
            out.setOutputLocation(Output::STDERR);
        printStatus(out);
        out.output("   Checking for unreceived events on network link:\n");
        network->emergencyShutdownDebug(out);
    }
}


DirectoryController::DirEntry* DirectoryController::getDirEntry(Addr baseAddr){
    std::unordered_map<Addr,DirEntry*>::iterator i = directory.find(baseAddr);
    DirEntry *entry;
    if (directory.end() == i) {
        entry = new DirEntry(baseAddr, numTargets, &dbg);
        entry->cacheIter = entryCache.end();
        directory[baseAddr] = entry;
        entry->setCached(true);   // TODO fix this so new entries go to memory if we're caching, little bit o cheatin here
    } else {
        entry = i->second;
    }
    return entry;
}


void DirectoryController::sendInvalidate(int target, MemEvent * reqEv, DirEntry* entry, Command cmd){
    MemEvent *me = new MemEvent(getName(), entry->getBaseAddr(), entry->getBaseAddr(), cmd, cacheLineSize);
    me->setDst(nodeid_to_name[target]);
    me->setRqstr(reqEv->getRqstr());

    if (is_debug_event(reqEv)) dbg.debug(_L4_, "Sending Invalidate.  Dst: %s\n", nodeid_to_name[target].c_str());
    profileRequestSent(me);

    uint64_t deliveryTime = timestamp + accessLatency;
    netMsgQueue.insert(std::make_pair(deliveryTime, me));
}

void DirectoryController::sendAckPut(MemEvent * event) {
    MemEvent * me = event->makeResponse(Command::AckPut);
    me->setDst(event->getSrc());
    me->setRqstr(event->getRqstr());
    me->setPayload(0, nullptr);
    me->setSize(cacheLineSize);

    profileResponseSent(me);

    uint64_t deliveryTime = timestamp + accessLatency;
    netMsgQueue.insert(std::make_pair(deliveryTime, me));

}


void DirectoryController::forwardFlushRequest(MemEvent * event) {
    MemEvent *reqEv     = new MemEvent(getName(), event->getAddr(), event->getBaseAddr(), Command::FlushLine, cacheLineSize);
    reqEv->setRqstr(event->getRqstr());
    reqEv->setVirtualAddress(event->getVirtualAddress());
    reqEv->setInstructionPointer(event->getInstructionPointer());
    reqEv->setMemFlags(event->getMemFlags());
    memReqs[reqEv->getID()] = event->getBaseAddr();
    profileRequestSent(reqEv);


    uint64_t deliveryTime = timestamp + accessLatency;
    reqEv->setDst(memoryName);

    memMsgQueue.insert(std::make_pair(deliveryTime, reqEv));

    if (is_debug_event(event)) {
        dbg.debug(_L5_, "Forwarding FlushLine to memory. BaseAddr = x%" PRIx64 ", Size = %u, noncacheable = %s\n",
                reqEv->getBaseAddr(), reqEv->getSize(), reqEv->queryFlag(MemEvent::F_NONCACHEABLE) ? "true" : "false");
    }
}

uint32_t DirectoryController::node_id(const std::string &name){
	uint32_t id;
	std::map<std::string, uint32_t>::iterator i = node_lookup.find(name);
	if(node_lookup.end() == i){
		node_lookup[name] = id = targetCount++;
        nodeid_to_name.resize(targetCount);
        nodeid_to_name[id] = name;
	}
    else id = i->second;

	return id;
}



uint32_t DirectoryController::node_name_to_id(const std::string &name){
    std::map<std::string, uint32_t>::iterator i = node_lookup.find(name);

    if(node_lookup.end() == i) {
	dbg.fatal(CALL_INFO, -1, "%s, Error: Attempt to lookup node ID but name not found: %s. Time = %" PRIu64 "ns\n",
                getName().c_str(), name.c_str(), getCurrentSimTimeNano());
    }

    uint32_t id = i->second;
    return id;
}

void DirectoryController::updateCache(DirEntry *entry){
    if(0 == entryCacheMaxSize){
        sendEntryToMemory(entry);
    } else {
        /* Find if we're in the cache */
        if(entry->cacheIter != entryCache.end()){
            entryCache.erase(entry->cacheIter);
            --entryCacheSize;
            entry->cacheIter = entryCache.end();
        }

        /* Find out if we're no longer cached, and just remove */
        if (entry->getState() == I){
            if (is_debug_addr(entry->getBaseAddr())) dbg.debug(_L10_, "Entry for 0x%" PRIx64 " has no references - purging\n", entry->getBaseAddr());

            directory.erase(entry->getBaseAddr());
            delete entry;
            return;
        } else {
            entryCache.push_front(entry);
            entry->cacheIter = entryCache.begin();
            ++entryCacheSize;

            while(entryCacheSize > entryCacheMaxSize){
                DirEntry *oldEntry = entryCache.back();
                // If the oldest entry is still in progress, everything is in progress
                if(mshr->isHit(oldEntry->getBaseAddr())) break;

                if (is_debug_addr(entry->getBaseAddr())) dbg.debug(_L10_, "entryCache too large.  Evicting entry for 0x%" PRIx64 "\n", oldEntry->getBaseAddr());

                entryCache.pop_back();
                --entryCacheSize;
                oldEntry->cacheIter = entryCache.end();
                oldEntry->setCached(false);
                sendEntryToMemory(oldEntry);
            }
        }
    }
}



void DirectoryController::sendEntryToMemory(DirEntry *entry){
    Addr entryAddr = 0; // Always use local address 0 for directory entries
    MemEvent *me   = new MemEvent(getName(), entryAddr, entryAddr, Command::PutE, cacheLineSize); // MemController discards PutE's without writeback so this is safe
    me->setSize(entrySize);
    profileRequestSent(me);

    uint64_t deliveryTime = timestamp + accessLatency;
    me->setDst(memoryName);

    memMsgQueue.insert(std::make_pair(deliveryTime, me));
}



MemEvent::id_type DirectoryController::writebackData(MemEvent *data_event, Command wbCmd) {
    MemEvent *ev       = new MemEvent(getName(), data_event->getBaseAddr(), data_event->getBaseAddr(), wbCmd, cacheLineSize);

    if(data_event->getPayload().size() != cacheLineSize) {
	dbg.fatal(CALL_INFO, -1, "%s, Error: Writing back data request but payload does not match cache line size of %uB. Event: %s. Time = %" PRIu64 "ns\n",
                getName().c_str(), cacheLineSize, ev->getVerboseString().c_str(), getCurrentSimTimeNano());
    }

    ev->setSize(data_event->getPayload().size());
    ev->setPayload(data_event->getPayload());
    ev->setDst(memoryName);
    profileRequestSent(ev);

    /* We will get a response if this is a flush request or if we are talking to an endpoint that sends WB Acks */
    if (!ev->isWriteback() || waitWBAck) memReqs[ev->getID()] = data_event->getBaseAddr();
    if (ev->isWriteback() && waitWBAck) {
        mshr->insertWriteback(data_event->getBaseAddr());
    }

    uint64_t deliveryTime = timestamp + accessLatency;
    memMsgQueue.insert(std::make_pair(deliveryTime, ev));

    if (is_debug_event(ev)) {
        dbg.debug(_L5_, "\tWriting back data. %s.\n", ev->getBriefString().c_str());
    }

    return ev->getID();
}

void DirectoryController::postRequestProcessing(MemEvent * ev, DirEntry * entry, bool stable) {
    Command cmd = ev->getCmd();
    if (cmd == Command::GetS || cmd == Command::GetX || cmd == Command::GetSX) {
        stat_getRequestLatency->addData(getCurrentSimTimeNano() - ev->getDeliveryTime());
    } else {
        stat_replacementRequestLatency->addData(getCurrentSimTimeNano() - ev->getDeliveryTime());
    }

    delete ev;
    if (stable)
        entry->setToSteadyState();
}

void DirectoryController::replayWaitingEvents(Addr addr) {
    if (mshr->isHit(addr)) {
        list<MSHREntry> * replayEntries = mshr->getAll(addr);
        if (replayEntries->begin()->elem.isEvent()) {
            MemEvent *ev = (replayEntries->begin()->elem).getEvent();

            if (is_debug_addr(addr)) dbg.debug(_L5_, "\tReactivating event. %s\n", ev->getBriefString().c_str());

            workQueue.insert(workQueue.begin(), std::make_pair(ev,true));
        }
    }
}

void DirectoryController::sendEventToCaches(MemEventBase *ev, uint64_t deliveryTime){
    netMsgQueue.insert(std::make_pair(deliveryTime, ev));
}


void DirectoryController::sendEventToMem(MemEventBase *ev) {
    if (memLink) memLink->send(ev);
    else network->send(ev);
}


bool DirectoryController::isRequestAddressValid(Addr addr){
    if (!memLink) return network->isRequestAddressValid(addr);
    else return memLink->isRequestAddressValid(addr);

    if(0 == region.interleaveSize) {
        return (addr >= region.start && addr < region.end);
    } else {
        if (addr < region.start) return false;
        if (addr >= region.end) return false;

        addr        = addr - region.start;
        Addr offset = addr % region.interleaveStep;

        if (offset >= region.interleaveSize) return false;
        return true;
    }

}

void DirectoryController::turnClockOn() {
    clockOn = true;
    timestamp = reregisterClock(defaultTimeBase, clockHandler);
    timestamp--; // reregisterClock returns next cycle clock will be enabled, set timestamp to current cycle
    uint64_t inactiveCycles = timestamp - lastActiveClockCycle;
    for (uint64_t i = 0; i < inactiveCycles; i++) {
        stat_MSHROccupancy->addData(mshr->getSize());
    }
}


/*static char dirEntStatus[1024] = {0};
const char* DirectoryController::printDirectoryEntryStatus(Addr baseAddr){
    DirEntry *entry = getDirEntry(baseAddr);
    if(!entry){
        sprintf(dirEntStatus, "[Not Created]");
    } else {
        uint32_t refs = entry->countRefs();

        if(0 == refs) sprintf(dirEntStatus, "[Noncacheable]");
        else if(entry->isDirty()){
            uint32_t owner = entry->findOwner();
            sprintf(dirEntStatus, "[owned by %s]", nodeid_to_name[owner].c_str());
        }
        else sprintf(dirEntStatus, "[Shared by %u]", refs);


    }
    return dirEntStatus;
}*/



void DirectoryController::init(unsigned int phase) {

    network->init(phase);
    if (memLink) memLink->init(phase);


    // Must happen after network init or merlin croaks
    // InitData: Name, NULLCMD, Endpoint type, inclusive of all upper levels, will send writeback acks, line size
    if (!phase) {
        // Push our region to memory (backward compatibility)
        if (!memLink) {
            MemEventInitRegion * reg = new MemEventInitRegion(getName(), network->getRegion(), true);
            reg->setDst(memoryName);
            network->sendInitData(reg);
        } else {
            MemEventInitRegion * reg = new MemEventInitRegion(getName(), memLink->getRegion(), true);
            memLink->sendInitData(reg);
        }
        // Tell memory we're here
        if (memLink) {
            memLink->sendInitData(new MemEventInitCoherence(getName(), Endpoint::Directory, true, true, cacheLineSize, true));
        }
        // Announce to network we're here
        network->sendInitData(new MemEventInitCoherence(getName(), Endpoint::Directory, true, true, cacheLineSize, true));
    }

    /* Pass data on to memory */
    while(MemEventInit *ev = network->recvInitData()){
        if (ev->getCmd() == Command::NULLCMD) {
            dbg.debug(_L10_, "%s received init event: %s\n",
                getName().c_str(), ev->getVerboseString().c_str());
            if (ev->getInitCmd() == MemEventInit::InitCommand::Coherence) {
                MemEventInitCoherence * mEv = static_cast<MemEventInitCoherence*>(ev);
                if (mEv->getType() == Endpoint::Scratchpad)
                    waitWBAck = true;
            }
            delete ev;
        } else {

            /* Check that memory name is valid - really only need to do this once, but since it's init, whatever */
            if (!memLink && !network->isDest(memoryName)) {
                dbg.fatal(CALL_INFO,-1,"%s, Invalid param: net_memory_name - must name a valid memory component in the system. You specified: %s\n",getName().c_str(), memoryName.c_str());
            }

            dbg.debug(_L10_, "Found Init Info for address 0x%" PRIx64 "\n", ev->getAddr());
            if (isRequestAddressValid(ev->getAddr())){
                dbg.debug(_L10_, "Sending Init Data for address 0x%" PRIx64 " to memory\n", ev->getAddr());
                ev->setDst(memoryName);
                if (memLink) {
                    memLink->sendInitData(ev);
                } else {
                    network->sendInitData(ev);
                }
            } else
                delete ev;

        }

    }

    SST::Event * ev;
    if (memLink) {
        while ((ev = memLink->recvInitData())) {
            MemEventInit * initEv = dynamic_cast<MemEventInit*>(ev);
            if (initEv && initEv->getCmd() == Command::NULLCMD) {
                dbg.debug(_L10_, "%s received init event: %s\n",
                        getName().c_str(), initEv->getVerboseString().c_str());

            }
            delete ev;
        }
    }
}



void DirectoryController::finish(void){
    network->finish();
}



void DirectoryController::setup(void){
    network->setup();

    numTargets = network->getSources()->size();

    if(0 == numTargets) dbg.fatal(CALL_INFO,-1,"%s, Error: Did not find any caches during init\n",getName().c_str());

    entrySize = (numTargets+1)/8 +1;
}

