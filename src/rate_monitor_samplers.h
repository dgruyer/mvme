#ifndef __RATE_MONITOR_SAMPLERS_H__
#define __RATE_MONITOR_SAMPLERS_H__

#include "rate_monitor_base.h"
#include "mvme_stream_processor.h"
#include "sis3153_readout_worker.h"

struct StreamProcessorSampler
{
    RateSampler bytesProcessed;
    RateSampler buffersProcessed;
    RateSampler buffersWithErrors;
    RateSampler eventSections;
    RateSampler invalidEventIndices;

    using ModuleEntries = std::array<RateSampler, MaxVMEModules>;
    std::array<RateSampler, MaxVMEEvents> eventEntries;
    std::array<ModuleEntries, MaxVMEEvents> moduleEntries;

    void sample(const MVMEStreamProcessorCounters &counters)
    {
        bytesProcessed.sample(counters.bytesProcessed);
        buffersProcessed.sample(counters.buffersProcessed);
        buffersWithErrors.sample(counters.buffersWithErrors);
        eventSections.sample(counters.eventSections);
        invalidEventIndices.sample(counters.invalidEventIndices);

        for (size_t ei = 0; ei < MaxVMEEvents; ei++)
        {
            eventEntries[ei].sample(counters.eventCounters[ei]);

            for (size_t mi = 0; mi < MaxVMEModules; mi++)
            {
                moduleEntries[ei][mi].sample(counters.moduleCounters[ei][mi]);
            }
        }
    }
};

struct DAQStatsSampler
{
    RateSampler totalBytesRead;
    RateSampler totalBuffersRead;
    RateSampler buffersWithErrors;
    RateSampler droppedBuffers;
    RateSampler totalNetBytesRead;
    RateSampler listFileBytesWritten;

    void sample(const DAQStats &counters)
    {
        totalBytesRead.sample(counters.totalBytesRead);
        totalBuffersRead.sample(counters.totalBuffersRead);
        buffersWithErrors.sample(counters.buffersWithErrors);
        droppedBuffers.sample(counters.droppedBuffers);
        totalNetBytesRead.sample(counters.totalNetBytesRead);
        listFileBytesWritten.sample(counters.listFileBytesWritten);
    }
};

struct SIS3153Sampler
{
    using StackListCountEntries = std::array<RateSampler, SIS3153Constants::NumberOfStackLists>;

    StackListCountEntries stackListCounts;
    StackListCountEntries stackListBerrCounts_Block;
    StackListCountEntries stackListBerrCounts_Read;
    StackListCountEntries stackListBerrCounts_Write;
    RateSampler lostEvents;
    RateSampler multiEventPackets;
    StackListCountEntries embeddedEvents;
    StackListCountEntries partialFragments;
    StackListCountEntries reassembledPartials;

    void sample(const SIS3153ReadoutWorker::Counters &counters)
    {
        lostEvents.sample(counters.lostEvents);
        multiEventPackets.sample(counters.multiEventPackets);

        for (size_t i = 0; i < std::tuple_size<StackListCountEntries>::value; i++)
        {
            stackListCounts[i].sample(counters.stackListCounts[i]);
            stackListBerrCounts_Block[i].sample(counters.stackListBerrCounts_Block[i]);
            stackListBerrCounts_Read[i].sample(counters.stackListBerrCounts_Read[i]);
            stackListBerrCounts_Write[i].sample(counters.stackListBerrCounts_Write[i]);
            embeddedEvents[i].sample(counters.embeddedEvents[i]);
            partialFragments[i].sample(counters.partialFragments[i]);
            reassembledPartials[i].sample(counters.reassembledPartials[i]);
        }
    }
};

#endif /* __RATE_MONITOR_SAMPLERS_H__ */
