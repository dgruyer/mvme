/* mvme - Mesytec VME Data Acquisition
 *
 * Copyright (C) 2016, 2017  Florian Lüke <f.lueke@mesytec.com>
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
#include "mvme_event_processor.h"

#include "analysis/a2_adapter.h"
#include "analysis/analysis.h"
#include "analysis/analysis_impl_switch.h"
#include "histo1d.h"
#include "mesytec_diagnostics.h"
#include "mvme_context.h"
#include "mvme_listfile.h"
#include "timed_block.h"

#include <atomic>
#include <QCoreApplication>
#include <QElapsedTimer>

enum RunAction
{
    KeepRunning,
    StopIfQueueEmpty,
    StopImmediately
};

static const u32 FilledBufferWaitTimeout_ms = 250;
static const u32 ProcessEventsMinInterval_ms = 500;

struct MVMEEventProcessorPrivate
{
    MVMEStreamProcessor streamProcessor;
    MVMEContext *context = nullptr;
    u32 m_listFileVersion = 1;

    //std::atomic<RunAction> m_runAction;
    volatile RunAction m_runAction;
    EventProcessorState m_state = EventProcessorState::Idle;
};

MVMEEventProcessor::MVMEEventProcessor(MVMEContext *context)
    : m_d(new MVMEEventProcessorPrivate)
{
    m_d->m_runAction = KeepRunning;
    m_d->context = context;
}

MVMEEventProcessor::~MVMEEventProcessor()
{
    //delete m_d->diag; // FIXME: why? what?
    delete m_d;
}

void MVMEEventProcessor::beginRun(const RunInfo &runInfo, VMEConfig *vmeConfig)
{
    m_d->streamProcessor.beginRun(
        runInfo,
        m_d->context->getAnalysis(),
        m_d->context->getVMEConfig(),
        m_d->m_listFileVersion,
        [this](const QString &msg) { m_d->context->logMessage(msg); });

    auto &counters = m_d->streamProcessor.getCounters();
    counters.startTime = QDateTime::currentDateTime();
    counters.stopTime  = QDateTime();
}

/* Used at the start of a run after beginRun() has been called and to resume from
 * paused state.
 * Does a2_begin_run() and a2_end_run() (threading stuff if enabled). */
void MVMEEventProcessor::startProcessing()
{
    qDebug() << __PRETTY_FUNCTION__ << "begin";
    Q_ASSERT(m_freeBuffers);
    Q_ASSERT(m_fullBuffers);
    Q_ASSERT(m_d->m_state == EventProcessorState::Idle);

    auto &counters = m_d->streamProcessor.getCounters();
    counters.startTime = QDateTime::currentDateTime();
    counters.stopTime  = QDateTime();

    emit started();
    emit stateChanged(m_d->m_state = EventProcessorState::Running);

    QCoreApplication::processEvents();

    QElapsedTimer timeSinceLastProcessEvents;
    timeSinceLastProcessEvents.start();

    m_d->m_runAction = KeepRunning;

    auto analysis = m_d->context->getAnalysis();

    if (analysis)
    {
        if (auto a2State = analysis->getA2AdapterState())
        {
            // This is here instead of in Analysis::beginRun() because the
            // latter is called way too much from everywhere and I don't want
            // to rebuild the a2 system all the time.
            a2::a2_begin_run(a2State->a2);
        }
    }

    while (m_d->m_runAction != StopImmediately)
    {
        DataBuffer *buffer = nullptr;

        {
            QMutexLocker lock(&m_fullBuffers->mutex);

            if (m_fullBuffers->queue.isEmpty())
            {
                if (m_d->m_runAction == StopIfQueueEmpty)
                    break;

                m_fullBuffers->wc.wait(&m_fullBuffers->mutex, FilledBufferWaitTimeout_ms);
            }

            if (!m_fullBuffers->queue.isEmpty())
            {
                buffer = m_fullBuffers->queue.dequeue();
            }
        }
        // The mutex is unlocked again at this point

        if (buffer)
        {
            m_d->streamProcessor.processDataBuffer(buffer);

            // Put the buffer back into the free queue
            enqueue(m_freeBuffers, buffer);
        }

        // Process Qt events to be able to "receive" queued calls to our slots.
        if (timeSinceLastProcessEvents.elapsed() > ProcessEventsMinInterval_ms)
        {
            QCoreApplication::processEvents();
            timeSinceLastProcessEvents.restart();
        }
    }

    counters.stopTime = QDateTime::currentDateTime();

    if (analysis)
    {
        if (auto a2State = analysis->getA2AdapterState())
        {
            a2::a2_end_run(a2State->a2);
        }
    }

    emit stopped();
    emit stateChanged(m_d->m_state = EventProcessorState::Idle);

    qDebug() << __PRETTY_FUNCTION__ << "end";
}

void MVMEEventProcessor::stopProcessing(bool whenQueueEmpty)
{
    qDebug() << QDateTime::currentDateTime().toString("HH:mm:ss")
        << __PRETTY_FUNCTION__ << (whenQueueEmpty ? "when empty" : "immediately");

    m_d->m_runAction = whenQueueEmpty ? StopIfQueueEmpty : StopImmediately;
}

EventProcessorState MVMEEventProcessor::getState() const
{
    return m_d->m_state;
}

const MVMEStreamProcessorCounters &MVMEEventProcessor::getCounters() const
{
    return m_d->streamProcessor.getCounters();
}

void MVMEEventProcessor::setListFileVersion(u32 version)
{
    qDebug() << __PRETTY_FUNCTION__ << version;

    m_d->m_listFileVersion = version;
}

void MVMEEventProcessor::setDiagnostics(MesytecDiagnostics *diag)
{
    qDebug() << __PRETTY_FUNCTION__ << diag;
    // FIXME: owernship? maybe make it shared?
    delete m_d->streamProcessor.getDiagnostics();
    m_d->streamProcessor.attachDiagnostics(diag);
}

MesytecDiagnostics *MVMEEventProcessor::getDiagnostics() const
{
    return m_d->streamProcessor.getDiagnostics();
}

void MVMEEventProcessor::removeDiagnostics()
{
    m_d->streamProcessor.removeDiagnostics();
}

