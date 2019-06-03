#include "mvlc_stream_workers.h"

#include <QThread>

#include "mvme_context.h"
#include "vme_analysis_common.h"
#include "mvlc/mvlc_impl_eth.h"


using namespace vme_analysis_common;
using namespace mesytec::mvlc;

VMEConfReadoutScripts collect_readout_scripts(const VMEConfig &vmeConfig)
{
    VMEConfReadoutScripts readoutScripts;

    for (const auto &eventConfig: vmeConfig.getEventConfigs())
    {
        std::vector<vme_script::VMEScript> moduleReadoutScripts;

        for (const auto &moduleConfig: eventConfig->getModuleConfigs())
        {
            auto rdoScript = moduleConfig->getReadoutScript()->getScript();
            moduleReadoutScripts.emplace_back(rdoScript);
        }

        readoutScripts.emplace_back(moduleReadoutScripts);
    }

    return readoutScripts;
}

//
// MVLC_StreamWorkerBase
//
MVLC_StreamWorkerBase::MVLC_StreamWorkerBase(
    MVMEContext *context,
    ThreadSafeDataBufferQueue *freeBuffers,
    ThreadSafeDataBufferQueue *fullBuffers,
    QObject *parent)
: StreamWorkerBase(parent)
, m_context(context)
, m_freeBuffers(freeBuffers)
, m_fullBuffers(fullBuffers)
, m_state(MVMEStreamWorkerState::Idle)
, m_desiredState(MVMEStreamWorkerState::Idle)
, m_stopFlag(StopWhenQueueEmpty)
{
}

MVLC_StreamWorkerBase::~MVLC_StreamWorkerBase()
{
}

void MVLC_StreamWorkerBase::setState(MVMEStreamWorkerState newState)
{
    // This implementation copies the behavior of MVMEStreamWorker::setState.
    // Signal emission is done in the exact same order.
    // The implementation was and is buggy: the transition into Running always
    // caused started() to be emitted even when coming from Paused state.

    m_state = newState;
    m_desiredState = newState;
    emit stateChanged(m_state);

    switch (newState)
    {
        case MVMEStreamWorkerState::Idle:
            emit stopped();
            break;

        case MVMEStreamWorkerState::Running:
            emit started();
            break;

        case MVMEStreamWorkerState::Paused:
        case MVMEStreamWorkerState::SingleStepping:
            break;
    }
}

void MVLC_StreamWorkerBase::start()
{
    using WorkerState = MVMEStreamWorkerState;

    if (m_state != WorkerState::Idle)
    {
        logError("worker state != Idle, ignoring request to start");
        return;
    }

    const auto runInfo = m_context->getRunInfo();
    const auto vmeConfig = m_context->getVMEConfig();
    auto analysis = m_context->getAnalysis();

    {
        CountersLock guard(m_countersMutex);
        m_counters = {};
        m_counters.startTime = QDateTime::currentDateTime();
    }
    beginRun_(runInfo, vmeConfig, analysis);

    setState(m_startPaused ? WorkerState::Paused : WorkerState::Running);

    TimetickGenerator timetickGen;

    while (true)
    {
        // running
        if (likely(m_state == WorkerState::Running && m_desiredState == WorkerState::Running))
        {
            static const unsigned FullBufferWaitTime_ms = 100;

            if (auto buffer = dequeue(m_fullBuffers, FullBufferWaitTime_ms))
                processBuffer(buffer, runInfo, vmeConfig, analysis);
        }
        // pause
        else if (m_state == WorkerState::Running && m_desiredState == WorkerState::Paused)
        {
            setState(WorkerState::Paused);
        }
        // resume
        else if (m_state == WorkerState::Paused && m_desiredState == WorkerState::Running)
        {
            setState(WorkerState::Running);
        }
        // stopping
        else if (m_desiredState == WorkerState::Idle)
        {
            if (m_stopFlag == StopImmediately)
                break;

            // The StopWhenQueueEmpty case
            if (auto buffer = dequeue(m_fullBuffers))
                processBuffer(buffer, runInfo, vmeConfig, analysis);
            else
                break;
        }
        // paused
        else if (m_state == WorkerState::Paused)
        {
            static const unsigned PauseSleepDuration_ms = 100;

            QThread::msleep(std::min(
                    PauseSleepDuration_ms,
                    static_cast<unsigned>(timetickGen.getTimeToNextTick_ms())));
        }
        else
            InvalidCodePath;

        if (!runInfo.isReplay)
        {
            int elapsedSeconds = timetickGen.generateElapsedSeconds();

            while (elapsedSeconds >= 1)
            {
                analysis->processTimetick();
                elapsedSeconds--;
            }
        }
    }

    {
        CountersLock guard(m_countersMutex);
        m_counters.stopTime = QDateTime::currentDateTime();
    }

    setState(WorkerState::Idle);
}

void MVLC_StreamWorkerBase::processBuffer(
    DataBuffer *buffer,
    const RunInfo &runInfo,
    const VMEConfig *vmeConfig,
    analysis::Analysis *analysis)
{
    bool ok = processBuffer_(buffer, runInfo, vmeConfig, analysis);
    enqueue(m_freeBuffers, buffer);

    {
        CountersLock guard(m_countersMutex);
        m_counters.bytesProcessed += buffer->used;
        m_counters.buffersProcessed++;
        if (!ok) m_counters.buffersWithErrors++;
    }
}

void MVLC_StreamWorkerBase::stop(bool whenQueueEmpty)
{
    m_stopFlag = (whenQueueEmpty ? StopWhenQueueEmpty : StopImmediately);
    m_desiredState = MVMEStreamWorkerState::Idle;
}

void MVLC_StreamWorkerBase::pause()
{
    m_desiredState = MVMEStreamWorkerState::Paused;
}

void MVLC_StreamWorkerBase::resume()
{
    m_desiredState = MVMEStreamWorkerState::Running;
}

void MVLC_StreamWorkerBase::singleStep()
{
    logError("SingleSteppping not implemented for the MVLC data processor");
    //m_desiredState = MVMEStreamWorkerState::SingleStepping;
}

void MVLC_StreamWorkerBase::startupConsumers()
{
    for (auto c: m_bufferConsumers)
    {
        c->startup();
    }

    for (auto c: m_moduleConsumers)
    {
        c->startup();
    }
}

void MVLC_StreamWorkerBase::shutdownConsumers()
{
    for (auto c: m_bufferConsumers)
    {
        c->shutdown();
    }

    for (auto c: m_moduleConsumers)
    {
        c->shutdown();
    }
}

//
// MVLC_ETH_StreamWorker
//
MVLC_ETH_StreamWorker::MVLC_ETH_StreamWorker(
    MVMEContext *context,
    ThreadSafeDataBufferQueue *freeBuffers,
    ThreadSafeDataBufferQueue *fullBuffers,
    QObject *parent)
    : MVLC_StreamWorkerBase(context, freeBuffers, fullBuffers, parent)
{}

MVLC_ETH_StreamWorker::~MVLC_ETH_StreamWorker()
{
}

void MVLC_ETH_StreamWorker::beginRun_(
    const RunInfo &runInfo,
    const VMEConfig *vmeConfig,
    analysis::Analysis *analysis)
{
    m_parser = std::unique_ptr<ReadoutParser_ETH>(
        make_readout_parser_eth(
            collect_readout_scripts(*vmeConfig)));

    m_parserCallbacks.beginEvent = [analysis](int ei)
    {
        analysis->beginEvent(ei);
    };

    m_parserCallbacks.moduleData = [analysis](int ei, int mi, u32 *data, u32 size)
    {
        analysis->processModuleData(ei, mi, data, size);
    };

    m_parserCallbacks.endEvent = [analysis](int ei)
    {
        analysis->endEvent(ei);
    };
}

// Input is a sequence of MVLC_ETH formatted buffers as generated by
// MVLCReadoutWorker::readout_eth().
bool MVLC_ETH_StreamWorker::processBuffer_(
    DataBuffer *buffer,
    const RunInfo &runInfo,
    const VMEConfig *vmeConfig,
    analysis::Analysis *analysis)
{
    if (buffer->tag != static_cast<int>(DataBufferFormatTags::MVLC_ETH))
        return false;

    try
    {
        parse_readout_buffer(
            *m_parser, m_parserCallbacks,
            buffer->id, buffer->data, buffer->used);

        return true;
    } catch (const end_of_buffer &)
    {
    }
    catch (const std::runtime_error &)
    {
    }

    return false;
}

//
// MVLC_USB_StreamWorker
//
MVLC_USB_StreamWorker::~MVLC_USB_StreamWorker()
{
}

void MVLC_USB_StreamWorker::beginRun_(
    const RunInfo &runInfo,
    const VMEConfig *vmeConfig,
    analysis::Analysis *analysis)
{
}

// Input is a sequence of MVLC_USB formatted buffers as generated by
// MVLCReadoutWorker::readout_usb()
bool MVLC_USB_StreamWorker::processBuffer_(
    DataBuffer *buffer,
    const RunInfo &runInfo,
    const VMEConfig *vmeConfig,
    analysis::Analysis *analysis)
{
    if (buffer->tag != static_cast<int>(DataBufferFormatTags::MVLC_USB))
        return false;

    try
    {
        return true;
    } catch (const end_of_buffer &)
    {
        return false;
    }
    catch (const std::runtime_error &)
    {
        return false;
    }

    return false;
}
