#include "vmusb_readout_worker.h"
#include "vmusb_buffer_processor.h"
#include "vmusb.h"
#include "CVMUSBReadoutList.h"
#include <QCoreApplication>
#include <QThread>
#include <memory>
#include <functional>

using namespace vmusb_constants;
using namespace vme_script;

static const int maxConsecutiveReadErrors = 5;

static void processQtEvents(QEventLoop::ProcessEventsFlags flags = QEventLoop::AllEvents)
{
    QCoreApplication::processEvents(flags);
}

VMUSBReadoutWorker::VMUSBReadoutWorker(MVMEContext *context, QObject *parent)
    : QObject(parent)
    , m_context(context)
    , m_readBuffer(new DataBuffer(vmusb_constants::BufferMaxSize))
{
}

VMUSBReadoutWorker::~VMUSBReadoutWorker()
{
    delete m_readBuffer;
}

void VMUSBReadoutWorker::start(quint32 cycles)
{
#if 1
    if (m_state != DAQState::Idle)
        return;

    clearError();

    auto vmusb = dynamic_cast<VMUSB *>(m_context->getController());
    if (!vmusb)
    {
        setError("VMUSB controller required");
        return;
    }

    m_cyclesToRun = cycles;
    setState(DAQState::Starting);
    DAQStats &stats(m_context->getDAQStats());
    bool error = false;
    auto daqConfig = m_context->getDAQConfig();

    try
    {
        m_vmusbStack.resetLoadOffset(); // reset the static load offset

        emit logMessage(QSL("VMUSB readout starting"));
        int result;

        for (int i=0; i<8; ++i)
        {
            result = vmusb->setIrq(i, 0);
            if (result < 0)
                throw QString("Resetting IRQ vectors failed");
        }

        vmusb->setDaqSettings(0);

        int globalMode = vmusb->getMode();
        globalMode |= (1 << GlobalModeRegister::MixedBufferShift);
        vmusb->setMode(globalMode);

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

            VMEScript readoutScript;

            readoutScript += event->vmeScripts["readout_start"]->getScript();

            for (auto module: event->modules)
            {
                readoutScript += module->vmeScripts["readout"]->getScript(module->getBaseAddress());
                Command marker;
                marker.type = CommandType::Marker;
                marker.value = EndMarker;
                readoutScript += marker;
            }

            readoutScript += event->vmeScripts["readout_end"]->getScript();

            CVMUSBReadoutList readoutList(readoutScript);
            m_vmusbStack.setContents(QVector<u32>::fromStdVector(readoutList.get()));

            if (m_vmusbStack.getContents().size())
            {

                emit logMessage(QString("Loading readout stack for event \"%1\""
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
                        emit logMessage(tmp);
                    }
                }

                result = m_vmusbStack.loadStack(vmusb);
                if (result < 0)
                    throw QString("Error loading readout stack");

                result = m_vmusbStack.enableStack(vmusb);
                if (result < 0)
                    throw QString("Error enabling readout stack");
            }
            else
            {
                emit logMessage(QString("Empty readout stack for event \"%1\".")
                                .arg(event->objectName())
                               );
            }
        }

        using namespace std::placeholders;

        vme_script::LoggerFun logger = std::bind(&VMUSBReadoutWorker::logMessage, this, _1);

        emit logMessage(QSL("Global DAQ Start:"));
        for (auto script: daqConfig->vmeScriptLists["daq_start"])
        {
            emit logMessage(QString("  %1").arg(script->objectName()));
            run_script(vmusb, script->getScript(), logger);
        }

        emit logMessage(QSL("Module Init"));
        for (auto event: daqConfig->eventConfigs)
        {
            for (auto module: event->modules)
            {
                run_script(vmusb, module->vmeScripts["parameters"]->getScript(module->getBaseAddress()), logger);
                run_script(vmusb, module->vmeScripts["readout_settings"]->getScript(module->getBaseAddress()), logger);
            }
        }

        emit logMessage(QSL("Event DAQ Start"));
        for (auto event: daqConfig->eventConfigs)
        {
            run_script(vmusb, event->vmeScripts["daq_start"]->getScript(), logger);
        }

        m_bufferProcessor->beginRun();
        emit logMessage(QSL("Entering readout loop\n"));
        stats.start();

        readoutLoop();

        stats.stop();
        emit logMessage(QSL("\nLeaving readout loop"));
        m_bufferProcessor->endRun();

        emit logMessage(QSL("Event DAQ Stop"));
        for (auto event: daqConfig->eventConfigs)
        {
            run_script(vmusb, event->vmeScripts["daq_stop"]->getScript(), logger);
        }

        emit logMessage(QSL("Global DAQ Stop:"));
        for (auto script: daqConfig->vmeScriptLists["daq_stop"])
        {
            emit logMessage(QString("  %1").arg(script->objectName()));
            run_script(vmusb, script->getScript(), logger);
        }
    }
    catch (const char *message)
    {
        setError(message);
        error = true;
    }
    catch (const QString &message)
    {
        setError(message);
        error = true;
    }
    catch (const std::runtime_error &e)
    {
        setError(e.what());
        error = true;
    }


    setState(DAQState::Idle);

    if (error)
    {
        try
        {
            if (vmusb->isInDaqMode())
                vmusb->leaveDaqMode();
        }
        catch (...)
        {}
    }
#endif
}

void VMUSBReadoutWorker::stop()
{
    if (m_state != DAQState::Running)
        return;

    setState(DAQState::Stopping);
    processQtEvents();
}

void VMUSBReadoutWorker::readoutLoop()
{
    setState(DAQState::Running);

    auto vmusb = dynamic_cast<VMUSB *>(m_context->getController());
    if (!vmusb->enterDaqMode())
    {
        throw QString("Error entering VMUSB DAQ mode");
    }

    int timeout_ms = 2000; // TODO: make this dynamic and dependent on the Bulk Transfer Setup Register timeout
    int consecutiveReadErrors = 0;

    DAQStats &stats(m_context->getDAQStats());

    while (m_state == DAQState::Running)
    {
        processQtEvents();

        m_readBuffer->used = 0;

        int bytesRead = vmusb->bulkRead(m_readBuffer->data, m_readBuffer->size, timeout_ms);

        if (bytesRead > 0)
        {
            m_readBuffer->used = bytesRead;
            stats.addBuffersRead(1);
            stats.addBytesRead(bytesRead);

            const double alpha = 0.1;
            stats.avgReadSize = (alpha * bytesRead) + (1.0 - alpha) * stats.avgReadSize;
            consecutiveReadErrors = 0;
            if (m_bufferProcessor)
            {
                m_bufferProcessor->processBuffer(m_readBuffer);
            }
        }
        else
        {
#if 0
            if (consecutiveReadErrors >= maxConsecutiveReadErrors)
            {
                emit logMessage(QString("VMUSB Error: %1 consecutive reads failed. Stopping DAQ.").arg(consecutiveReadErrors));
                break;
            }
            else
#endif


            {
                ++consecutiveReadErrors;
                emit logMessage(QString("VMUSB Warning: no data from bulk read (error=\"%1\", code=%2)")
                                .arg(strerror(-bytesRead))
                                .arg(bytesRead));
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

    processQtEvents();

    qDebug() << __PRETTY_FUNCTION__ << "left readoutLoop, reading remaining data";
    vmusb->leaveDaqMode();

    int bytesRead = 0;

    do
    {
        m_readBuffer->used = 0;

        bytesRead = vmusb->bulkRead(m_readBuffer->data, m_readBuffer->size, timeout_ms);

        if (bytesRead > 0)
        {
            m_readBuffer->used = bytesRead;
            stats.addBuffersRead(1);
            stats.addBytesRead(bytesRead);
            
            const double alpha = 0.1;
            stats.avgReadSize = (alpha * bytesRead) + (1.0 - alpha) * stats.avgReadSize;
            if (m_bufferProcessor)
            {
                m_bufferProcessor->processBuffer(m_readBuffer);
            }
        }
        processQtEvents();
    } while (bytesRead > 0);
}

void VMUSBReadoutWorker::setState(DAQState state)
{
    m_state = state;
    emit stateChanged(state);
}

void VMUSBReadoutWorker::setError(const QString &message)
{
    emit logMessage(QString("VMUSB Error: %1").arg(message));
    setState(DAQState::Idle);
}

