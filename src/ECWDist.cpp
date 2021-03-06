#include "util.h"
#include "WorkDistribution.hpp"
#include "ECImpl.hpp"

#include <fstream>
#include <string>
#include <mpi.h>
#include <limits>

// Make ReadStore swappable
static void swap(ReadStore& left, ReadStore& right){
    left.swap(right);
}

// Error correction for a batch of reads
struct BatchEC{
    void operator()(const ReadStore& rbatch, ECImpl& ecr, int, int) {
        int rID = rbatch.readId;
        for(unsigned i = 0; i < rbatch.size(); i++){
            int position = rbatch.readsOffset[i];
            int qposition = rbatch.qualsOffset[i];
            char* addr = const_cast<char*> (&(rbatch.readsString[position]));
            char* qAddr = const_cast<char*> (&(rbatch.qualsString[qposition]));
            ecr.readEC(rID, addr, qAddr);
            rID++;
        }
    // TODO: think about writing output when ecr has a lot of error records
    }
};

// Error correction for a single read within the batch
struct UnitEC{
    void operator()(const ReadStore& rbatch, ECImpl& ecr, unsigned ipos) {
        if(ipos >= rbatch.size())
            return;
        int position = rbatch.readsOffset[ipos];
        int qposition = rbatch.qualsOffset[ipos];
        char* addr = const_cast<char*> (&(rbatch.readsString[position]));
        char* qAddr = const_cast<char*> (&(rbatch.qualsString[qposition]));
        ecr.readEC(rbatch.readId + ipos, addr, qAddr);
        // TODO: think about writing output when ecr has a lot of error records
    }
};

// Load a batch of reads starting from offset
struct ReadBatchLoader{
    bool operator()(const ECImpl& ecr, unsigned long woffStart,
                     unsigned long woffEnd, ReadStore& tmp){
        // std::cout << "C" ;
        std::ifstream fin(ecr.getPara().iFaName);
        fin.seekg(woffStart);
        readBatch(&fin, std::numeric_limits<int>::max(), woffEnd, tmp);
        return (tmp.size() > 0);
    }
};


struct ReadBatchLoader2{
    bool operator()(const ECImpl& ecr, unsigned long woffStart,
                     unsigned long woffEnd, ReadStore& tmp){
        ecr.getECData().loadReads(woffStart, woffEnd, tmp);
        return (tmp.size() > 0);
    }
};

long getTotalWork(const std::string& fileName){
    std::ifstream fin(fileName.c_str());
    fin.seekg(0,std::ios::end);
    return fin.tellg();
}

struct ChunkSizePolicy{
    const Para& params;
    long nWorkers;
    long tChunk;
    ChunkSizePolicy(unsigned long totalWork, ECImpl& cref):
        params(cref.getECData().getParams()){
        nWorkers = params.m_size * (params.numThreads + 1);
        tChunk = totalWork / (nWorkers * params.workFactor);
        if(params.m_rank == 0)
            std::cout << "chunk\t" << tChunk << std::endl;
    }
    unsigned long operator()(unsigned long){
        return tChunk;
    }
};

struct ChunkSizePolicy2{
    const Para& params;
    unsigned long upThreshold;
    unsigned long downWorkChunk;
    unsigned long upWorkChunk;
    ChunkSizePolicy2(unsigned long totalWork, ECImpl& cref) :
        params(cref.getECData().getParams()){
        upThreshold = (totalWork/2);
        downWorkChunk = 2500;
        upWorkChunk = 500;
        if(params.m_rank == 0){
            std::cout << "threshold\t" << upThreshold << std::endl;
            std::cout << "up chunk\t" << upWorkChunk << std::endl;
            std::cout << "down chunk\t" << downWorkChunk << std::endl;
        }
    }
    unsigned long operator()(unsigned long cWork){
        return (cWork < upThreshold) ? downWorkChunk : upWorkChunk;
    }
};


void write_errors(ECData& ecdata, std::vector<ECImpl>& threadEC){
    if(ecdata.getParams().writeOutput != 0){
        std::ofstream ofs(ecdata.getParams().outputFilename.c_str());
        if(ofs.good()){
            for(auto eit = threadEC.begin(); eit != threadEC.end(); eit++)
                eit->writeErrors(ofs);
        }
    }
}

void get_timings(const std::vector<timespec>& stateTimings,
                 std::vector<double>& stTimings){
    stTimings.push_back(elapsed_local(stateTimings[PENDING_WORK],
                                      stateTimings[ASSIGN_WORK]));
    stTimings.push_back(elapsed_local(stateTimings[FINISHED_WORK],
                                      stateTimings[PENDING_WORK]));
    stTimings.push_back(elapsed_local(stateTimings[NR_WORK_STATES],
                                      stateTimings[FINISHED_WORK]));
}

void ec_wdist0(ECData& ecdata, std::vector<double>& stTimings){
    Para& params = ecdata.getParams();
    long nThreads = (long) params.numThreads;
    std::vector<ECImpl> threadEC(nThreads + 1,
                                 ECImpl(ecdata, params));
    long tWork = getTotalWork(params.iFaName);

    WorkDistribution<ReadStore, unsigned long, ECImpl,
                     ReadBatchLoader, BatchEC, UnitEC,
                     ChunkSizePolicy> wdist(tWork, threadEC,
                                             params.numThreads);
    wdist.main();
    write_errors(ecdata, threadEC);
    get_timings(wdist.getStateTimings(), stTimings);
}

void ec_wdist2(ECData& ecdata, std::vector<double>& stTimings){
    Para& params = ecdata.getParams();
    long nThreads = (long) params.numThreads;
    std::vector<ECImpl> threadEC(nThreads + 1,
                                 ECImpl(ecdata, params));
    long tWork = ecdata.getFullReadSize();

    if(params.m_rank == 0){
        std::cout << "work\t" << tWork << std::endl;
    }

    WorkDistribution<ReadStore, unsigned long, ECImpl,
                     ReadBatchLoader2, BatchEC, UnitEC,
                     ChunkSizePolicy2> wdist(tWork, threadEC,
                                             params.numThreads);
    wdist.main();
    write_errors(ecdata, threadEC);
    get_timings(wdist.getStateTimings(), stTimings);
}
