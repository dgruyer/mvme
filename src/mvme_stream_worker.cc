/* mvme - Mesytec VME Data Acquisition
 *
 * Copyright (C) 2016-2018 mesytec GmbH & Co. KG <info@mesytec.com>
 *
 * Author: Florian Lüke <f.lueke@mesytec.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#include "mvme_stream_worker.h"

#include "analysis/a2_adapter.h"
#include "analysis/analysis.h"
#include "analysis/analysis_session.h"
#include "histo1d.h"
#include "mesytec_diagnostics.h"
#include "mvme_context.h"
#include "mvme_listfile.h"
#include "timed_block.h"
#include "vme_analysis_common.h"

#include <atomic>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

using vme_analysis_common::TimetickGenerator;

namespace
{

enum InternalState
{
    KeepRunning,
    StopIfQueueEmpty,
    StopImmediately,
    Pause,
    SingleStep,
};

static const QMap<InternalState, QString> InternalState_StringTable =
{
    { KeepRunning,                      QSL("InternalState::KeepRunning") },
    { StopIfQueueEmpty,                 QSL("InternalState::StopIfQueueEmpty") },
    { StopImmediately,                  QSL("InternalState::StopImmediately") },
    { Pause,                            QSL("InternalState::Pause") },
    { SingleStep,                       QSL("InternalState::SingleStep") },
};

static const u32 FilledBufferWaitTimeout_ms = 125;
static const u32 ProcessEventsMinInterval_ms = 500;
static const double PauseMaxSleep_ms = 125.0;

} // end anon namespace

const QMap<MVMEStreamWorkerState, QString> MVMEStreamWorkerState_StringTable =
{
    { MVMEStreamWorkerState::Idle,              QSL("Idle") },
    { MVMEStreamWorkerState::Paused,            QSL("Paused") },
    { MVMEStreamWorkerState::Running,           QSL("Running") },
    { MVMEStreamWorkerState::SingleStepping,    QSL("Stepping") },
};

struct MVMEStreamWorkerPrivate
{
    MVMEStreamProcessor streamProcessor;
    MVMEContext *context = nullptr;
    u32 m_listFileVersion = 1;
    bool m_startPaused = false;

    std::atomic<InternalState> internalState;
    MVMEStreamWorkerState state = MVMEStreamWorkerState::Idle;

    RunInfo runInfo;

    ThreadSafeDataBufferQueue *freeBuffers,
                              *fullBuffers;

    u64 nextBufferNumber = 0;
    inline DataBuffer *dequeueNextBuffer();
};

DataBuffer *MVMEStreamWorkerPrivate::dequeueNextBuffer()
{
    DataBuffer *buffer = nullptr;

    {
        QMutexLocker lock(&fullBuffers->mutex);

        if (fullBuffers->queue.isEmpty())
        {
            if (internalState == StopIfQueueEmpty)
            {
                internalState = StopImmediately;
                return buffer;
            }

            fullBuffers->wc.wait(&fullBuffers->mutex, FilledBufferWaitTimeout_ms);
        }

        if (!fullBuffers->queue.isEmpty())
        {
            buffer = fullBuffers->queue.dequeue();
        }
    }

    if (buffer)
    {
        buffer->id = this->nextBufferNumber++;
    }

    return buffer;
}

MVMEStreamWorker::MVMEStreamWorker(MVMEContext *context,
                                   ThreadSafeDataBufferQueue *freeBuffers,
                                   ThreadSafeDataBufferQueue *fullBuffers)
    : m_d(new MVMEStreamWorkerPrivate)
{
    m_d->internalState = KeepRunning;
    m_d->context = context;
    m_d->freeBuffers = freeBuffers;
    m_d->fullBuffers = fullBuffers;
}

MVMEStreamWorker::~MVMEStreamWorker()
{
    delete m_d;
}

MVMEStreamProcessor *MVMEStreamWorker::getStreamProcessor() const
{
    return &m_d->streamProcessor;
}

void MVMEStreamWorker::setState(MVMEStreamWorkerState newState)
{
    auto oldState = m_d->state;
    m_d->state = newState;

    qDebug() << __PRETTY_FUNCTION__
        << MVMEStreamWorkerState_StringTable[oldState] << "(" << static_cast<int>(oldState) << ") ->"
        << MVMEStreamWorkerState_StringTable[newState] << "(" << static_cast<int>(newState) << ")";

    emit stateChanged(newState);

    switch (newState)
    {
        case MVMEStreamWorkerState::Running:
            emit started();
            break;
        case MVMEStreamWorkerState::Idle:
            emit stopped();
            break;
        case MVMEStreamWorkerState::Paused:
        case MVMEStreamWorkerState::SingleStepping:
            break;
    }

    // for signals to cross thread boundaries
    QCoreApplication::processEvents();
}

void MVMEStreamWorker::logMessage(const QString &msg)
{
    m_d->context->logMessage(msg);
}

void MVMEStreamWorker::beginRun()
{
    m_d->runInfo = m_d->context->getRunInfo();

    m_d->streamProcessor.beginRun(
        m_d->runInfo,
        m_d->context->getAnalysis(),
        m_d->context->getVMEConfig(),
        m_d->m_listFileVersion,
        [this](const QString &msg) { m_d->context->logMessage(msg); });

    m_d->nextBufferNumber = 0;
}

namespace
{

using ProcessingState = MVMEStreamProcessor::ProcessingState;

void debug_dump(const ProcessingState &procState)
{
    Q_ASSERT(procState.buffer);

    qDebug() << ">>> begin ProcessingState";

    qDebug("  buffer=%p, buffer.id=%lu, buffer.data=%p, buffer.used=%lu bytes, %lu words",
           procState.buffer,
           procState.buffer->id,
           procState.buffer->data,
           procState.buffer->used,
           procState.buffer->used / sizeof(u32)
          );

    u32 lastSectionHeader = *procState.buffer->indexU32(procState.lastSectionHeaderOffset);

    qDebug("  lastSectionHeader=0x%08x, lastSectionHeaderOffset=%d",
           lastSectionHeader, procState.lastSectionHeaderOffset);

    for (s32 moduleIndex = 0;
         moduleIndex < MaxVMEModules;
         moduleIndex++)
    {
        if (procState.lastModuleDataSectionHeaderOffsets[moduleIndex] >= 0
            || procState.lastModuleDataBeginOffsets[moduleIndex] >= 0
            || procState.lastModuleDataEndOffsets[moduleIndex] >= 0)
        {
            qDebug("  moduleIndex=%d, dataSectionHeaderOffset=%d, moduleDataBeginOffset=%d, moduleDataEndOffset=%d => dataSize=%d",
                   moduleIndex,
                   procState.lastModuleDataSectionHeaderOffsets[moduleIndex],
                   procState.lastModuleDataBeginOffsets[moduleIndex],
                   procState.lastModuleDataEndOffsets[moduleIndex],
                   procState.lastModuleDataEndOffsets[moduleIndex] - procState.lastModuleDataBeginOffsets[moduleIndex]
                  );

            //qDebug("  moduleIndex=%d, dataSectionHeader=0x%08x, moduleDataBegin=0x%08x, moduleDataEnd=0x%08x"
        }
    }

    qDebug() << ">>> end ProcessingState";
}

static const QMap<ProcessingState::StepResult, QString> StepResult_StringTable =
{
    { ProcessingState::StepResult_Unset,            QSL("Unspecified") },
    { ProcessingState::StepResult_EventHasMore,     QSL("MultiEvent") },
    { ProcessingState::StepResult_EventComplete,    QSL("EventComplete") },
    { ProcessingState::StepResult_AtEnd,            QSL("BufferCompleted") },
    { ProcessingState::StepResult_Error,            QSL("ProcessingError") },
};

QTextStream &
log_processing_step(QTextStream &out, const ProcessingState &procState, const vats::MVMETemplates &vatsTemplates)
{
    Q_ASSERT(procState.buffer);

    using LF = listfile_v1;

    out << "buffer #" << procState.buffer->id
        << ", size=" << (procState.buffer->used / sizeof(u32)) << " words"
        << ", step result: " << StepResult_StringTable[procState.stepResult]
        << endl;

    try
    {
        if (procState.stepResult == ProcessingState::StepResult_EventHasMore
            || procState.stepResult == ProcessingState::StepResult_EventComplete)
        {
            u32 eventSectionHeader = *procState.buffer->indexU32(procState.lastSectionHeaderOffset);
            u32 eventIndex         = (eventSectionHeader & LF::EventTypeMask) >> LF::EventTypeShift;
            u32 eventSectionSize   = (eventSectionHeader & LF::SectionSizeMask) >> LF::SectionSizeShift;

            out << "  "
                << (QString("eventHeader=0x%1, @offset %2, idx=%3, sz=%4 words")
                    .arg(eventSectionHeader, 8, 16, QLatin1Char('0'))
                    .arg(procState.lastSectionHeaderOffset)
                    .arg(eventIndex)
                    .arg(eventSectionSize)
                   )
                << endl;

            bool endlFlag = false;

            for (s32 moduleIndex = 0;
                 moduleIndex < MaxVMEModules;
                 moduleIndex++)
            {
                if (procState.lastModuleDataSectionHeaderOffsets[moduleIndex] >= 0
                    && procState.lastModuleDataBeginOffsets[moduleIndex] >= 0
                    && procState.lastModuleDataEndOffsets[moduleIndex] >= 0)
                {
                    u32 moduleSectionHeader = *procState.buffer->indexU32(
                        procState.lastModuleDataSectionHeaderOffsets[moduleIndex]);

                    u32 *moduleDataPtr = procState.buffer->indexU32(
                        procState.lastModuleDataBeginOffsets[moduleIndex]);

                    const u32 *moduleDataEndPtr = procState.buffer->indexU32(
                        procState.lastModuleDataEndOffsets[moduleIndex]);


                    u32 moduleSectionSize = (moduleSectionHeader & LF::SubEventSizeMask) >> LF::SubEventSizeShift;
                    u32 moduleType = (moduleSectionHeader & LF::ModuleTypeMask) >> LF::ModuleTypeShift;
                    QString moduleTypeString = vats::get_module_meta_by_typeId(vatsTemplates, moduleType).typeName;

                    if (endlFlag) out << endl;

                    out << "    "
                        << (QString("moduleHeader=0x%1, @offset %2, idx=%3, sz=%4 words, type=%5")
                            .arg(moduleSectionHeader, 8, 16, QLatin1Char('0'))
                            .arg(procState.lastModuleDataSectionHeaderOffsets[moduleIndex])
                            .arg(moduleIndex)
                            .arg(moduleSectionSize)
                            .arg(moduleTypeString)
                           )
                        << endl;

                    size_t dataSize_words = moduleDataEndPtr - moduleDataPtr;
                    size_t dataSize_bytes = dataSize_words * sizeof(u32);

                    if (procState.stepResult == ProcessingState::StepResult_EventHasMore)
                    {
                        // the multievent case (except for the last part which I can't distinguish for now)

                        out << "    "
                            << (QString("multievent: begin@=%1, end@=%2, sz=%3")
                                .arg(procState.lastModuleDataBeginOffsets[moduleIndex])
                                .arg(procState.lastModuleDataEndOffsets[moduleIndex])
                                .arg(dataSize_words)
                               )
                            << endl;
                    }

                    BufferIterator moduleDataIter(reinterpret_cast<u8 *>(moduleDataPtr), dataSize_bytes);
                    logBuffer(moduleDataIter, [&out](const QString &str) { out << "      " << str << endl; });

                    endlFlag = true;
                }
            }
        }
    }
    catch (const end_of_buffer &)
    {
        out << "!!! Error formatting last processing step in buffer #" << procState.buffer->id
            << ": unexpectedly reached end of buffer. This is a bug!"
            << endl;
    }

    return out;
}

void single_step_one_event(ProcessingState &procState, MVMEStreamProcessor &streamProc)
{
    for (bool done = false; !done;)
    {
        streamProc.singleStepNextStep(procState);

        switch (procState.stepResult)
        {
            case ProcessingState::StepResult_EventHasMore:
            case ProcessingState::StepResult_EventComplete:
            case ProcessingState::StepResult_AtEnd:
            case ProcessingState::StepResult_Error:
                done = true;
                break;

            case ProcessingState::StepResult_Unset:
                break;
        }
    }

#ifndef NDEBUG
    if (procState.stepResult == ProcessingState::StepResult_EventHasMore
        || procState.stepResult == ProcessingState::StepResult_EventComplete)
    {
        debug_dump(procState);
    }
#endif
}

} // end anon namespace

/* The main worker loop. Call beginRun() before invoking start().
 * Currently also does a2_begin_run()/a2_end_run() to handle a2 threads if
 * enabled. */
void MVMEStreamWorker::start()
{
    qDebug() << __PRETTY_FUNCTION__ << "begin";
    Q_ASSERT(m_d->freeBuffers);
    Q_ASSERT(m_d->fullBuffers);
    Q_ASSERT(m_d->state == MVMEStreamWorkerState::Idle);
    Q_ASSERT(m_d->context->getAnalysis());

    using ProcessingState = MVMEStreamProcessor::ProcessingState;

    // Single stepping support (the templates are used for logging output)
    MVMEStreamProcessor::ProcessingState singleStepProcState;
    auto vatsTemplates = vats::read_templates();


    if (auto a2State = m_d->context->getAnalysis()->getA2AdapterState())
    {
        // Do not move this into Analysis::beginRun() as most of the time calls
        // to it are not directly followed by starting the StreamWorker,
        // meaning the threading setup is unnecessary.
        // This now also opens output file handles for ExportSink operators.
        a2::a2_begin_run(a2State->a2);
    }

    // Start stream consumers from within this thread
    m_d->streamProcessor.startConsumers();


    // Timers and timeticks
    auto &counters = m_d->streamProcessor.getCounters();
    counters.startTime = QDateTime::currentDateTime();
    counters.stopTime  = QDateTime();

    TimetickGenerator timetickGen;

    /* Fixed in MVMEContext::startDAQReplay:
     * There's a race condition here that leads to being stuck in the loop
     * below. If the replay is very short and the listfile reader is finished
     * before we reach this line here then stop(IfQueueEmpty) may already have
     * been called. Thus internalState will be StopIfQueueEmpty and we will
     * overwrite it below with either Pause or KeepRunning.  As the listfile
     * reader already sent it's finished signal which makes the context call
     * our stop() method we won't get any more calls to stop().  A way to fix
     * this would be to wait for the stream processor to enter it's loop and
     * only then start the listfile reader.
     */

    // Start out in running state unless pause mode was requested.
    m_d->internalState = m_d->m_startPaused ? Pause : KeepRunning;
    InternalState internalState = m_d->internalState;

    /* This emits started(). I've deliberately placed this after
     * m_d->internalState has been copied to avoid race conditions. */
    setState(MVMEStreamWorkerState::Running);

    while (internalState != StopImmediately)
    {
        if (m_d->state == MVMEStreamWorkerState::Running)
        {
            switch (internalState)
            {
                case KeepRunning:
                case StopIfQueueEmpty:
                    // keep running and process full buffers
                    if (auto buffer = m_d->dequeueNextBuffer())
                    {
                        m_d->streamProcessor.processDataBuffer(buffer);
                        enqueue(m_d->freeBuffers, buffer);
                    }
                    break;

                case Pause:
                    // transition to paused
                    setState(MVMEStreamWorkerState::Paused);
                    break;

                case StopImmediately:
                    // noop. loop will exit
                    break;

                case SingleStep:
                    // logic error: may only happen in paused state
                    InvalidCodePath;
                    break;
            }
        }
        else if (m_d->state == MVMEStreamWorkerState::Paused)
        {
            switch (internalState)
            {
                case Pause:
                    // stay paused
                    QThread::msleep(std::min(PauseMaxSleep_ms, timetickGen.getTimeToNextTick_ms()));
                    break;

                case SingleStep:
                    if (!singleStepProcState.buffer)
                    {
                        if (auto buffer = m_d->dequeueNextBuffer())
                        {
                            singleStepProcState = m_d->streamProcessor.singleStepInitState(buffer);
                        }
                    }

                    if (singleStepProcState.buffer)
                    {
                        single_step_one_event(singleStepProcState, m_d->streamProcessor);

                        QString logBuffer;
                        QTextStream logStream(&logBuffer);
                        log_processing_step(logStream, singleStepProcState, vatsTemplates);
                        m_d->context->logMessageRaw(logBuffer);

                        if (singleStepProcState.stepResult == ProcessingState::StepResult_AtEnd
                            || singleStepProcState.stepResult == ProcessingState::StepResult_Error)
                        {
                            enqueue(m_d->freeBuffers, singleStepProcState.buffer);
                            singleStepProcState = {};
                        }
                    }

                    m_d->internalState = Pause;

                    break;

                case KeepRunning:
                case StopIfQueueEmpty:
                case StopImmediately:
                    // resume
                    setState(MVMEStreamWorkerState::Running);

                    // if singlestepping stopped in the middle of a buffer
                    // process the rest of the buffer, then go back to running
                    // state
                    if (singleStepProcState.buffer)
                    {
                        while (true)
                        {
                            single_step_one_event(singleStepProcState, m_d->streamProcessor);

                            qDebug() << __PRETTY_FUNCTION__ << "resume after stepping case."
                                << "stepResult is: " << StepResult_StringTable[singleStepProcState.stepResult];

                            if (singleStepProcState.stepResult == ProcessingState::StepResult_AtEnd
                                || singleStepProcState.stepResult == ProcessingState::StepResult_Error)
                            {
                                enqueue(m_d->freeBuffers, singleStepProcState.buffer);
                                singleStepProcState = {};
                                break;
                            }
                        }
                    }

                    break;
            }
        }
        else
        {
            InvalidCodePath;
        }

        if (!m_d->runInfo.isReplay)
        {
            int elapsedSeconds = timetickGen.generateElapsedSeconds();

            while (elapsedSeconds >= 1)
            {
                m_d->streamProcessor.processExternalTimetick();
                elapsedSeconds--;
            }
        }

        // reload the possibly modified atomic
        internalState = m_d->internalState;
    }

    counters.stopTime = QDateTime::currentDateTime();

    if (auto a2State = m_d->context->getAnalysis()->getA2AdapterState())
    {
        a2::a2_end_run(a2State->a2);
    }

    m_d->streamProcessor.endRun();

    // analysis session auto save
    // NOTE: load is done in mvme.cpp
    auto sessionPath = m_d->context->getWorkspacePath(QSL("SessionDirectory"));
    if (!sessionPath.isEmpty())
    {
        auto filename = sessionPath + "/last_session.hdf5";
        auto result   = save_analysis_session(filename, m_d->context->getAnalysis());

        if (result.first)
        {
            logMessage(QString("Auto saved analysis session to %1").arg(filename));
        }
        // silent in the error case
    }

    setState(MVMEStreamWorkerState::Idle);

    qDebug() << __PRETTY_FUNCTION__ << "end";
}

void MVMEStreamWorker::stop(bool whenQueueEmpty)
{
    qDebug() << QDateTime::currentDateTime().toString("HH:mm:ss")
        << __PRETTY_FUNCTION__ << (whenQueueEmpty ? "when empty" : "immediately");

    m_d->internalState = whenQueueEmpty ? StopIfQueueEmpty : StopImmediately;
}

void MVMEStreamWorker::pause()
{
    qDebug() << __PRETTY_FUNCTION__;

    Q_ASSERT(m_d->internalState != InternalState::Pause);
    m_d->internalState = InternalState::Pause;
}

void MVMEStreamWorker::resume()
{
    qDebug() << __PRETTY_FUNCTION__;

    Q_ASSERT(m_d->internalState == InternalState::Pause);
    m_d->internalState = InternalState::KeepRunning;
}

void MVMEStreamWorker::singleStep()
{
    qDebug() << __PRETTY_FUNCTION__ << "current internalState ="
        << InternalState_StringTable[m_d->internalState];

    Q_ASSERT(m_d->internalState == InternalState::Pause);

    qDebug() << __PRETTY_FUNCTION__ << "setting internalState to SingleStep";
    m_d->internalState = SingleStep;
}

MVMEStreamWorkerState MVMEStreamWorker::getState() const
{
    return m_d->state;
}

const MVMEStreamProcessorCounters &MVMEStreamWorker::getCounters() const
{
    return m_d->streamProcessor.getCounters();
}

void MVMEStreamWorker::setListFileVersion(u32 version)
{
    qDebug() << __PRETTY_FUNCTION__ << version;

    m_d->m_listFileVersion = version;
}

void MVMEStreamWorker::setStartPaused(bool startPaused)
{
    qDebug() << __PRETTY_FUNCTION__ << startPaused;
    Q_ASSERT(getState() == MVMEStreamWorkerState::Idle);

    m_d->m_startPaused = startPaused;
}

bool MVMEStreamWorker::getStartPaused() const
{
    return m_d->m_startPaused;
}

void MVMEStreamWorker::setDiagnostics(std::shared_ptr<MesytecDiagnostics> diag)
{
    qDebug() << __PRETTY_FUNCTION__ << diag.get();
    m_d->streamProcessor.attachDiagnostics(diag);
}

bool MVMEStreamWorker::hasDiagnostics() const
{
    return m_d->streamProcessor.hasDiagnostics();
}

void MVMEStreamWorker::removeDiagnostics()
{
    m_d->streamProcessor.removeDiagnostics();
}
