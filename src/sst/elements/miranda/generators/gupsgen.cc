// Copyright 2009-2017 Sandia Corporation. Under the terms
// of Contract DE-NA0003525 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2017, Sandia Corporation
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
#include <sst/core/params.h>
#include <sst/core/rng/marsaglia.h>
#include <sst/elements/miranda/generators/gupsgen.h>

using namespace SST::Miranda;

GUPSGenerator::GUPSGenerator( Component* owner, Params& params ) :
	RequestGenerator(owner, params) {

	const uint32_t verbose = params.find<uint32_t>("verbose", 0);

	out = new Output("GUPSGenerator[@p:@l]: ", verbose, 0, Output::STDOUT);

	iterations = params.find<uint64_t>("iterations", 1);
	issueCount = (params.find<uint64_t>("count", 1000)) * iterations;
	reqLength  = params.find<uint64_t>("length", 8);
	maxAddr    = params.find<uint64_t>("max_address", 524288);
	seed_a     = params.find<uint64_t>("seed_a", 11);
	seed_b     = params.find<uint64_t>("seed_b", 31);
        write_cmd  = params.find<uint32_t>("write_cmd", 0xFFFF );
        read_cmd   = params.find<uint32_t>("read_cmd", 0xFFFF );
	rng = new MarsagliaRNG(seed_a, seed_b);

	out->verbose(CALL_INFO, 1, 0, "Will issue %" PRIu64 " operations\n", issueCount);
	out->verbose(CALL_INFO, 1, 0, "Request lengths: %" PRIu64 " bytes\n", reqLength);
	out->verbose(CALL_INFO, 1, 0, "Maximum address: %" PRIu64 "\n", maxAddr);

        if( write_cmd != 0xFFFF ){
          out->verbose(CALL_INFO, 1, 0, "Custom WR opcode %" PRIu32 "\n", write_cmd );
        }
        if( read_cmd != 0xFFFF ){
          out->verbose(CALL_INFO, 1, 0, "Custom RD opcode %" PRIu32 "\n", read_cmd );
        }

	issueOpFences = params.find<std::string>("issue_op_fences", "yes") == "yes";
}

GUPSGenerator::~GUPSGenerator() {
	delete out;
	delete rng;
}

void GUPSGenerator::generate(MirandaRequestQueue<GeneratorRequest*>* q) {

	const uint64_t rand_addr = rng->generateNextUInt64();
	// Ensure we have a reqLength aligned request
	const uint64_t addr_under_limit = (rand_addr % maxAddr);
	const uint64_t addr = (addr_under_limit < reqLength) ? addr_under_limit :
		(rand_addr % maxAddr) - (rand_addr % reqLength);

	out->verbose(CALL_INFO, 4, 0, "Generating next request number: %" PRIu64 " at address %" PRIu64 "\n", issueCount, addr);

        MemoryOpRequest* readAddr;
        MemoryOpRequest* writeAddr;

        if( read_cmd == 0xFFFF ){
          // issue standard read
          readAddr = new MemoryOpRequest(addr, reqLength, READ);
        }else{
          // issue custom read
          new MemoryOpRequest(addr, reqLength, read_cmd);
        }

        if( write_cmd == 0xFFFF ){
	  writeAddr = new MemoryOpRequest(addr, reqLength, WRITE);
        }else{
	  writeAddr = new MemoryOpRequest(addr, reqLength, write_cmd);
        }

	writeAddr->addDependency(readAddr->getRequestID());

	q->push_back(readAddr);
	q->push_back(writeAddr);

	issueCount--;
}

bool GUPSGenerator::isFinished() {
	return (issueCount == 0);
}

void GUPSGenerator::completed() {

}
