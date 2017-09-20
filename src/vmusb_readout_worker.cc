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
#include "vmusb_readout_worker.h"

#include <functional>
#include <memory>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include "CVMUSBReadoutList.h"
#include "vme_daq.h"
#include "vmusb_buffer_processor.h"
#include "vmusb.h"

using namespace vmusb_constants;
using namespace vme_script;

namespace
{
    struct TriggerData
    {
        EventConfig *event;
        u8 irqLevel;
        u8 irqVector;
    };

    struct DuplicateTrigger: public std::runtime_error
    {
        DuplicateTrigger(TriggerCondition condition, TriggerData d1, TriggerData d2)
            : std::runtime_error("Duplicate VME Trigger Condition")
            , m_condition(condition)
            , m_data1(d1)
            , m_data2(d2)
        {
            Q_ASSERT(d1.event != d2.event);
        }

        QString toString() const
        {
            QString result(QSL("Duplicate Trigger Condition: "));

            if (m_condition == TriggerCondition::Interrupt)
            {
                result += QString("trigger=%1, level=%2, vector=%3, event1=\"%4\", event2=\"%5\"")
                    .arg(TriggerConditionNames.value(m_condition))
                    .arg(static_cast<u32>(m_data1.irqLevel))
                    .arg(static_cast<u32>(m_data1.irqVector))
                    .arg(m_data1.event->objectName())
                    .arg(m_data2.event->objectName())
                    ;
            }
            else
            {
                result += QString("trigger=%1, event1=\"%4\", event2=\"%5\"")
                    .arg(TriggerConditionNames.value(m_condition))
                    .arg(m_data1.event->objectName())
                    .arg(m_data2.event->objectName())
                    ;
            }

            return result;
        }

        TriggerCondition m_condition;
        TriggerData m_data1,
                    m_data2;
    };

    static void validate_vme_config(VMEConfig *vmeConfig)
    {
        QMultiMap<TriggerCondition, TriggerData> triggers;

        for (auto event: vmeConfig->getEventConfigs())
        {
            TriggerData data = {event, event->irqLevel, event->irqVector};
            TriggerCondition condition = event->triggerCondition;

            if (triggers.contains(condition))
            {
                auto otherDataList = triggers.values(condition);

                if (condition == TriggerCondition::Interrupt)
                {
                    for (auto otherData: otherDataList)
                    {
                        if (data.irqLevel == otherData.irqLevel
                            && data.irqVector == otherData.irqVector)
                        {
                            throw DuplicateTrigger(condition, data, otherData);
                        }
                    }
                }
                else
                {
                    throw DuplicateTrigger(condition, data, otherDataList.at(0));
                }
            }

            triggers.insert(condition, data);
        }
    }
}

VMUSBReadoutWorker::VMUSBReadoutWorker(QObject *parent)
    : VMEReadoutWorker(parent)
    , m_readBuffer(new DataBuffer(vmusb_constants::BufferMaxSize))
    , m_bufferProcessor(new VMUSBBufferProcessor(this))
{
}

VMUSBReadoutWorker::~VMUSBReadoutWorker()
{
    delete m_readBuffer;
}

void VMUSBReadoutWorker::pre_setContext(VMEReadoutWorkerContext newContext)
{
    m_bufferProcessor->m_freeBufferQueue = newContext.freeBuffers;
    m_bufferProcessor->m_filledBufferQueue = newContext.fullBuffers;
}

void VMUSBReadoutWorker::start(quint32 cycles)
{
    if (m_state != DAQState::Idle)
        return;

    auto vmusb = qobject_cast<VMUSB *>(m_workerContext.controller);
    if (!vmusb)
    {
        logError(QSL("VMUSB controller required"));
        return;
    }

    m_vmusb = vmusb;

    m_cyclesToRun = cycles;
    setState(DAQState::Starting);
    DAQStats &stats(*m_workerContext.daqStats);
    bool errorThrown = false;
    auto daqConfig = m_workerContext.vmeConfig;
    VMEError error;

    auto ctrlSettings = daqConfig->getControllerSettings();

    // Decide whether to log buffer contents.
    m_bufferProcessor->setLogBuffers(cycles == 1);

    try
    {
        logMessage(QString(QSL("VMUSB readout starting on %1"))
                   .arg(QDateTime::currentDateTime().toString())
                   );

        validate_vme_config(daqConfig); // throws on error

        //
        // Read and log firmware version
        //
        {
            u32 fwReg;
            error = vmusb->readRegister(FIDRegister, &fwReg);
            if (error.isError())
                throw QString("Error reading VMUSB firmware version: %1").arg(error.toString());

            u32 fwMajor = (fwReg & 0xFFFF);
            u32 fwMinor = ((fwReg >> 16) &  0xFFFF);

            logMessage(QString(QSL("VMUSB Firmware Version %1_%2\n"))
                       .arg(fwMajor, 4, 16, QLatin1Char('0'))
                       .arg(fwMinor, 4, 16, QLatin1Char('0'))
                      );
        }

        //
        // Reset IRQs
        //
        for (int i = StackIDMin; i <= StackIDMax; ++i)
        {
            error = vmusb->setIrq(i, 0);
            if (error.isError())
                throw QString("Resetting IRQ vectors failed: %1").arg(error.toString());
        }

        //
        // DAQ Settings Register
        //
        u32 daqSettings = 0;

        error = vmusb->setDaqSettings(daqSettings);

        if (error.isError())
            throw QString("Setting DaqSettings register failed: %1").arg(error.toString());

        //
        // Global Mode Register
        //
        int globalMode = 0;
        globalMode |= (1 << GlobalModeRegister::MixedBufferShift);
        globalMode |= GlobalModeRegister::WatchDog250; // 250ms watchdog
        //globalMode |= GlobalModeRegister::NoIRQHandshake;

        error = vmusb->setMode(globalMode);
        if (error.isError())
            throw QString("Setting VMUSB global mode failed: %1").arg(error.toString());

        //
        // USB Bulk Transfer Setup Register
        //
        u32 bulkTransfer = 0;

        error = vmusb->setUsbSettings(bulkTransfer);
        if (error.isError())
            throw QString("Setting VMUSB Bulk Transfer Register failed: %1").arg(error.toString());

        //
        // Generate and load VMUSB stacks
        //
        m_vmusbStack.resetLoadOffset(); // reset the static load offset
        int nextStackID = 2; // start at ID=2 as NIM=0 and scaler=1 (fixed)

        for (auto event: daqConfig->eventConfigs)
        {
            qDebug() << "daq event" << event->objectName();

            m_vmusbStack = VMUSBStack();
            m_vmusbStack.triggerCondition = event->triggerCondition;
            m_vmusbStack.irqLevel = event->irqLevel;
            m_vmusbStack.irqVector = event->irqVector;
            m_vmusbStack.scalerReadoutPeriod = event->scalerReadoutPeriod;
            m_vmusbStack.scalerReadoutFrequency = event->scalerReadoutFrequency;

            if (event->triggerCondition == TriggerCondition::Interrupt)
            {
                event->stackID = nextStackID; // record the stack id in the event structure
                m_vmusbStack.setStackID(nextStackID);
                ++nextStackID;
            }
            else
            {
                // for NIM1 and scaler triggers the stack knows the stack number
                event->stackID = m_vmusbStack.getStackID();
            }

            qDebug() << "event " << event->objectName() << " -> stackID =" << event->stackID;

            VMEScript readoutScript = build_event_readout_script(event);
            CVMUSBReadoutList readoutList(readoutScript);
            m_vmusbStack.setContents(QVector<u32>::fromStdVector(readoutList.get()));

            if (m_vmusbStack.getContents().size())
            {
                logMessage(QString("Loading readout stack for event \"%1\""
                                   ", stack id = %2, size= %4, load offset = %3")
                           .arg(event->objectName())
                           .arg(m_vmusbStack.getStackID())
                           .arg(VMUSBStack::loadOffset)
                           .arg(m_vmusbStack.getContents().size())
                           );

                {
                    QString tmp;
                    for (u32 line: m_vmusbStack.getContents())
                    {
                        tmp.sprintf("  0x%08x", line);
                        logMessage(tmp);
                    }
                }

                error = m_vmusbStack.loadStack(vmusb);
                if (error.isError())
                    throw QString("Error loading readout stack: %1").arg(error.toString());

                error = m_vmusbStack.enableStack(vmusb);
                if (error.isError())
                    throw QString("Error enabling readout stack: %1").arg(error.toString());
            }
            else
            {
                logMessage(QString("Empty readout stack for event \"%1\".")
                                .arg(event->objectName())
                               );
            }
        }

        //
        // DAQ Init
        //
        vme_daq_init(daqConfig, vmusb, [this] (const QString &msg) { logMessage(msg); });

        //
        // Debug Dump of all VMUSB registers
        //
        logMessage(QSL(""));
        dump_registers(vmusb, [this] (const QString &line) { this->logMessage(line); });

        //
        // Debug: record raw buffers to file
        //
        if (ctrlSettings.value("DebugRawBuffers").toBool())
        {
            m_rawBufferOut.setFileName("vmusb_raw_buffers.bin");
            if (!m_rawBufferOut.open(QIODevice::WriteOnly))
            {
                auto msg = (QString("Error opening vmusb raw buffers file for writing: %1")
                            .arg(m_rawBufferOut.errorString()));
                logMessage(msg);
                qDebug() << __PRETTY_FUNCTION__ << msg;
            }
            else
            {
                auto msg = (QString("Writing raw VMUSB buffers to %1")
                            .arg(m_rawBufferOut.fileName()));
                logMessage(msg);
                qDebug() << __PRETTY_FUNCTION__ << msg;
            }
        }

        //
        // Readout
        //
        m_bufferProcessor->beginRun();
        logMessage(QSL(""));
        logMessage(QSL("Entering readout loop"));
        stats.start();

        readoutLoop();

        stats.stop();
        logMessage(QSL("Leaving readout loop"));
        logMessage(QSL(""));

        //
        // DAQ Stop
        //
        vme_daq_shutdown(daqConfig, vmusb, [this] (const QString &msg) { logMessage(msg); });

        m_bufferProcessor->endRun();

        //
        // Debug: close raw buffers file
        //
        if (m_rawBufferOut.isOpen())
        {
            auto msg = (QString("Closing vmusb raw buffers file %1")
                        .arg(m_rawBufferOut.fileName()));
            logMessage(msg);
            qDebug() << __PRETTY_FUNCTION__ << msg;

            m_rawBufferOut.close();
        }

        logMessage(QString(QSL("VMUSB readout stopped on %1"))
                   .arg(QDateTime::currentDateTime().toString())
                   );
    }
    catch (const char *message)
    {
        logError(message);
        errorThrown = true;
    }
    catch (const QString &message)
    {
        logError(message);
        errorThrown = true;
    }
    catch (const DuplicateTrigger &e)
    {
        logError(e.toString());
        errorThrown = true;
    }
    catch (const std::runtime_error &e)
    {
        logError(e.what());
        errorThrown = true;
    }
    catch (const vme_script::ParseError &)
    {
        logError(QSL("VME Script parse error"));
        errorThrown = true;
    }

    if (errorThrown)
    {
        try
        {
            if (vmusb->isInDaqMode())
                vmusb->leaveDaqMode();
        }
        catch (...)
        {}
    }

    setState(DAQState::Idle);
    emit daqStopped();
}

void VMUSBReadoutWorker::stop()
{
    if (!(m_state == DAQState::Running || m_state == DAQState::Paused))
        return;

    m_desiredState = DAQState::Stopping;
}

void VMUSBReadoutWorker::pause()
{
    if (m_state == DAQState::Running)
        m_desiredState = DAQState::Paused;
}

void VMUSBReadoutWorker::resume()
{
    if (m_state == DAQState::Paused)
        m_desiredState = DAQState::Running;
}

static const int leaveDaqReadTimeout_ms = 500;
static const int daqReadTimeout_ms = 500; // This should be higher than the watchdog timeout which is set to 250ms.
static const int daqModeHackTimeout_ms = leaveDaqReadTimeout_ms;

/* According to Jan we need to wait at least one millisecond
 * after entering DAQ mode to make sure that the VMUSB is
 * ready.
 * Trying to see if upping this value will make the USE_DAQMODE_HACK more stable.
 * This seems to fix the problems under 32bit WinXP.
 * */
static const int PostEnterDaqModeDelay_ms = 100;
static const int PostLeaveDaqModeDelay_ms = 100;

static VMEError enter_daq_mode(VMUSB *vmusb)
{
    auto result = vmusb->enterDaqMode();

    if (!result.isError())
    {
        QThread::msleep(PostEnterDaqModeDelay_ms);
    }

    return result;
}

static VMEError leave_daq_mode(VMUSB *vmusb)
{
    auto result = vmusb->leaveDaqMode();

    if (!result.isError())
    {
        QThread::msleep(PostLeaveDaqModeDelay_ms);
    }

    return result;
}

void VMUSBReadoutWorker::readoutLoop()
{
    auto vmusb = m_vmusb;
    auto error = enter_daq_mode(vmusb);

    if (error.isError())
        throw QString("Error entering VMUSB DAQ mode: %1").arg(error.toString());

    setState(DAQState::Running);

    DAQStats &stats(*m_workerContext.daqStats);
    QTime logReadErrorTimer;
    u64 nReadErrors = 0;
    u64 nGoodReads = 0;

    QTime elapsedTime;
    elapsedTime.start();
    m_bufferProcessor->timetick();

    while (true)
    {
        // Qt event processing to handle queued slots invocations (stop, pause, resume)
        processQtEvents();

        // One timetick for every elapsed second.
        s32 elapsedSeconds = elapsedTime.elapsed() / 1000;

        if (elapsedSeconds >= 1)
        {
            do
            {
                m_bufferProcessor->timetick();
            } while (--elapsedSeconds);
            elapsedTime.restart();
        }

        // pause
        if (m_state == DAQState::Running && m_desiredState == DAQState::Paused)
        {
            error = leave_daq_mode(vmusb);
            if (error.isError())
                throw QString("Error leaving VMUSB DAQ mode: %1").arg(error.toString());

            while (readBuffer(leaveDaqReadTimeout_ms).bytesRead > 0);
            setState(DAQState::Paused);
            logMessage(QSL("VMUSB readout paused"));
        }
        // resume
        else if (m_state == DAQState::Paused && m_desiredState == DAQState::Running)
        {
            error = enter_daq_mode(vmusb);
            if (error.isError())
                throw QString("Error entering VMUSB DAQ mode: %1").arg(error.toString());

            setState(DAQState::Running);
            logMessage(QSL("VMUSB readout resumed"));
        }
        // stop
        else if (m_desiredState == DAQState::Stopping)
        {
            logMessage(QSL("VMUSB readout stopping"));
            break;
        }
        // stay in running state
        else if (m_state == DAQState::Running)
        {
            auto readResult = readBuffer(daqReadTimeout_ms);

            /* XXX: Begin hack:
             * A timeout from readBuffer() here can mean that either there was
             * an error when communicating with the vmusb or that no data is
             * available. The second case can happen if the module sends no or
             * very little data so that the internal buffer of the controller
             * does not fill up fast enough. To avoid this case a smaller
             * buffer size could be chosen but that will negatively impact
             * performance for high data rates. Another method would be to use
             * VMUSBs watchdog feature but that was never implemented despite
             * what the documentation says.
             *
             * The workaround when getting a read timeout is to leave DAQ mode,
             * which forces the controller to dump its buffer, and to then
             * resume DAQ mode.
             *
             * If we still don't receive data after this there is a
             * communication error, otherwise the data rate was just too low to
             * fill the buffer and we continue on.
             *
             * Since firmware version 0A03_010917 there is a new watchdog
             * feature, different from the one in the documentation for version
             * 0A00. It does not use the USB Bulk Transfer Setup Register but
             * the Global Mode Register. The workaround here is left active to
             * work with older firmware versions. As long as daqReadTimeout_ms
             * is higher than the watchdog timeout the watchdog will be
             * activated if it is available. */
#define USE_DAQMODE_HACK
#ifdef USE_DAQMODE_HACK
            if (readResult.error.isTimeout() && readResult.bytesRead <= 0)
            {
                qDebug() << "begin USE_DAQMODE_HACK";
                error = leave_daq_mode(vmusb);
                if (error.isError())
                    throw QString("Error leaving VMUSB DAQ mode (in timeout handling): %1").arg(error.toString());

                readResult = readBuffer(daqModeHackTimeout_ms);

                error = enter_daq_mode(vmusb);
                if (error.isError())
                    throw QString("Error entering VMUSB DAQ mode (in timeout handling): %1").arg(error.toString());
                qDebug() << "end USE_DAQMODE_HACK";
            }
#endif

            if (!readResult.error.isError())
            {
                ++nGoodReads;
            }

            if (readResult.bytesRead <= 0)
            {
                static const int LogReadErrorTimer_ms = 5000;
                ++nReadErrors;
                if (!logReadErrorTimer.isValid() || logReadErrorTimer.elapsed() >= LogReadErrorTimer_ms)
                {
                    logMessage(QString("VMUSB Warning: error from bulk read: %1, bytesReceived=%2"
                                       " (total #readErrors=%3, #goodReads=%4)")
                               .arg(readResult.error.toString())
                               .arg(readResult.bytesRead)
                               .arg(nReadErrors)
                               .arg(nGoodReads)
                               );
                    logReadErrorTimer.restart();
                }
            }

            if (m_cyclesToRun > 0)
            {
                if (m_cyclesToRun == 1)
                {
                    qDebug() << "cycles to run reached";
                    break;
                }
                --m_cyclesToRun;
            }
        }
        else if (m_state == DAQState::Paused)
        {
            // In paused state process Qt events for a maximum of 1s, then run
            // another iteration of the loop to handle timeticks.
            processQtEvents(1000);
        }
        else
        {
            Q_ASSERT(!"Unhandled case in vmusb readoutLoop");
        }
    }

    setState(DAQState::Stopping);
    processQtEvents();

    qDebug() << __PRETTY_FUNCTION__ << "left readoutLoop, reading remaining data";
    error = leave_daq_mode(vmusb);
    if (error.isError())
        throw QString("Error leaving VMUSB DAQ mode: %1").arg(error.toString());

    while (readBuffer(leaveDaqReadTimeout_ms).bytesRead > 0);
}

void VMUSBReadoutWorker::setState(DAQState state)
{
    qDebug() << __PRETTY_FUNCTION__ << DAQStateStrings[m_state] << "->" << DAQStateStrings[state];
    m_state = state;
    m_desiredState = state;
    emit stateChanged(state);
}

void VMUSBReadoutWorker::logError(const QString &message)
{
    logMessage(QString("VMUSB Error: %1").arg(message));
}

void VMUSBReadoutWorker::logMessage(const QString &message)
{
    m_workerContext.logMessage(message);
}

VMUSBReadoutWorker::ReadBufferResult VMUSBReadoutWorker::readBuffer(int timeout_ms)
{
    ReadBufferResult result = {};

    m_readBuffer->used = 0;

    result.error = m_vmusb->bulkRead(m_readBuffer->data, m_readBuffer->size, &result.bytesRead, timeout_ms);

    /* Raw buffer output for debugging purposes.
     * The file consists of a sequence of entries with each entry having the following format:
     *   s32 VMEError::errorType
     *   s32 VMEError::errorCode
     *   s32 dataBytes
     *   u8* data
     * If dataBytes is 0 the data entry will be of size 0. No byte order
     * conversion is done so the format is architecture dependent!
     */
    if (m_rawBufferOut.isOpen())
    {
        s32 errorType = static_cast<s32>(result.error.error());
        s32 errorCode = result.error.errorCode();
        s32 bytesRead = result.bytesRead;

        m_rawBufferOut.write(reinterpret_cast<const char *>(&errorType), sizeof(errorType));
        m_rawBufferOut.write(reinterpret_cast<const char *>(&errorCode), sizeof(errorCode));
        m_rawBufferOut.write(reinterpret_cast<const char *>(&bytesRead), sizeof(bytesRead));
        m_rawBufferOut.write(reinterpret_cast<const char *>(m_readBuffer->data), bytesRead);
    }

    if (result.error.isError())
    {
        qDebug() << __PRETTY_FUNCTION__
            << "vmusb bulkRead result: " << result.error.toString()
            << "bytesRead =" << result.bytesRead;
    }

    if ((!result.error.isError() || result.error.isTimeout()) && result.bytesRead > 0)
    {
        m_readBuffer->used = result.bytesRead;
        DAQStats &stats(*m_workerContext.daqStats);
        stats.addBuffersRead(1);
        stats.addBytesRead(result.bytesRead);

        const double alpha = 0.1;
        stats.avgReadSize = (alpha * result.bytesRead) + (1.0 - alpha) * stats.avgReadSize;

        if (m_bufferProcessor)
            m_bufferProcessor->processBuffer(m_readBuffer);
    }

    return result;
}
