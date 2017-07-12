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
#include "mvme_context.h"
#include "mvme.h"
#include "vmusb.h"
#include "vmusb_readout_worker.h"
#include "vmusb_buffer_processor.h"
#include "mvme_event_processor.h"
#include "mvme_listfile.h"
#include "analysis/analysis.h"
#include "analysis/analysis_ui.h"
#include "config_ui.h"
#include "vme_analysis_common.h"

#include <QtConcurrent>
#include <QTimer>
#include <QThread>
#include <QProgressDialog>
#include <QMessageBox>

QString toString(const ListFileFormat &fmt)
{
    switch (fmt)
    {
        case ListFileFormat::Invalid:
            return QSL("Invalid");
        case ListFileFormat::Plain:
            return QSL("Plain");
        case ListFileFormat::ZIP:
            return QSL("ZIP");
    }

    return QString();
}

ListFileFormat fromString(const QString &str)
{
    if (str == "Plain")
        return ListFileFormat::Plain;

    if (str == "ZIP")
        return ListFileFormat::ZIP;

    return ListFileFormat::Invalid;
}

// Buffers to pass between DAQ/replay and the analysis. The buffer size should
// be at least twice as big as the max VMUSB buffer size (2 * 64k). Just using
// 1MB buffers for now as that's a good value for the listfile readout and
// doesn't affect the VMUSB readout negatively.
static const size_t DataBufferCount = 10;
static const size_t DataBufferSize = Megabytes(1);

static const int TryOpenControllerInterval_ms = 1000;
static const int PeriodicLoggingInterval_ms = 5000;
static const QString WorkspaceIniName = "mvmeworkspace.ini";
static const int ListFileDefaultCompression = 6;

static void stop_coordinated(VMUSBReadoutWorker *readoutWorker, MVMEEventProcessor *eventProcessor);

static void processQtEvents(QEventLoop::ProcessEventsFlags flags = QEventLoop::AllEvents)
{
    QCoreApplication::processEvents(flags);
}

struct MVMEContextPrivate
{
    MVMEContext *m_q;
    QStringList m_logBuffer;
    QMutex m_logBufferMutex;
    ListFileOutputInfo m_listFileOutputInfo = {};
    RunInfo m_runInfo;

    void stopDAQ();
    void stopDAQReplay();
    void stopDAQDAQ();

    void stopAnalysis();
    void resumeAnalysis();

    void clearLog();
};

void MVMEContextPrivate::stopDAQ()
{
    switch (m_q->m_mode)
    {
        case GlobalMode::DAQ: stopDAQDAQ(); break;
        case GlobalMode::ListFile: stopDAQReplay(); break;
        InvalidDefaultCase;
    }
}

void MVMEContextPrivate::stopDAQReplay()
{
    // FIXME: This is dangerous as there's no way to cancel and if one of the
    // signals below does not get emitted we're stuck here.
    QProgressDialog progressDialog("Stopping Replay", QString(), 0, 0);
    progressDialog.setWindowModality(Qt::ApplicationModal);
    progressDialog.setCancelButton(nullptr);
    progressDialog.show();

    QEventLoop localLoop;

    // First stop the ListFileReader

    // The timer is used to avoid a race between the worker stopping and the
    // progress dialog entering its eventloop. (Probably not needed, see the
    // explanation about not having a race condition below.)

    if (m_q->m_listFileWorker->isRunning())
    {
        QTimer::singleShot(0, [this]() { QMetaObject::invokeMethod(m_q->m_listFileWorker, "stop", Qt::QueuedConnection); });
        auto con = QObject::connect(m_q->m_listFileWorker, &ListFileReader::replayStopped, &localLoop, &QEventLoop::quit);
        localLoop.exec();
        QObject::disconnect(con);
    }

    // At this point the ListFileReader is stopped and will not produce any
    // more buffers. Now tell the MVMEEventProcessor to stop after finishing
    // the current queue.

    // There should be no race here. If the analysis is running we will stop it
    // and receive the stopped() signal.  If it just now stopped on its own
    // (e.g. end of replay) the signal is pending and will be delivered as soon
    // as we enter the event loop.
    if (m_q->m_eventProcessor->getState() != EventProcessorState::Idle)
    {
        QTimer::singleShot(0, [this]() { QMetaObject::invokeMethod(m_q->m_eventProcessor, "stopProcessing", Qt::QueuedConnection); });
        auto con = QObject::connect(m_q->m_eventProcessor, &MVMEEventProcessor::stopped, &localLoop, &QEventLoop::quit);
        localLoop.exec();
        QObject::disconnect(con);
    }

    m_q->m_eventProcessor->setListFileVersion(1);
    m_q->onDAQStateChanged(DAQState::Idle);
}

void MVMEContextPrivate::stopDAQDAQ()
{
    // FIXME: This is dangerous as there's no way to cancel and if one of the
    // signals below does not get emitted we're stuck here.
    QProgressDialog progressDialog("Stopping Data Acquisition", QString(), 0, 0);
    progressDialog.setWindowModality(Qt::ApplicationModal);
    progressDialog.setCancelButton(nullptr);
    progressDialog.show();

    QEventLoop localLoop;

    if (m_q->m_readoutWorker->isRunning())
    {
        QTimer::singleShot(0, [this]() { QMetaObject::invokeMethod(m_q->m_readoutWorker, "stop", Qt::QueuedConnection); });
        auto con = QObject::connect(m_q->m_readoutWorker, &VMUSBReadoutWorker::daqStopped, &localLoop, &QEventLoop::quit);
        localLoop.exec();
        QObject::disconnect(con);
    }

    if (m_q->m_eventProcessor->getState() != EventProcessorState::Idle)
    {
        QTimer::singleShot(0, [this]() { QMetaObject::invokeMethod(m_q->m_eventProcessor, "stopProcessing", Qt::QueuedConnection); });
        auto con = QObject::connect(m_q->m_eventProcessor, &MVMEEventProcessor::stopped, &localLoop, &QEventLoop::quit);
        localLoop.exec();
        QObject::disconnect(con);
    }

    m_q->onDAQStateChanged(DAQState::Idle);
}

void MVMEContextPrivate::stopAnalysis()
{
    QProgressDialog progressDialog("Stopping Analysis", QString(), 0, 0);
    progressDialog.setWindowModality(Qt::ApplicationModal);
    progressDialog.setCancelButton(nullptr);
    progressDialog.show();

    QEventLoop localLoop;

    if (m_q->m_eventProcessor->getState() != EventProcessorState::Idle)
    {
        // Tell the analysis top stop immediately
        QTimer::singleShot(0, [this]() { QMetaObject::invokeMethod(m_q->m_eventProcessor, "stopProcessing",
                                                                   Qt::QueuedConnection, Q_ARG(bool, false)); });
        QObject::connect(m_q->m_eventProcessor, &MVMEEventProcessor::stopped, &localLoop, &QEventLoop::quit);
        localLoop.exec();
    }

    qDebug() << __PRETTY_FUNCTION__ << "analysis stopped";
}

void MVMEContextPrivate::resumeAnalysis()
{
    if (m_q->m_eventProcessor->getState() == EventProcessorState::Idle)
    {
        QMetaObject::invokeMethod(m_q->m_eventProcessor, "startProcessing",
                                  Qt::QueuedConnection);

        qDebug() << __PRETTY_FUNCTION__ << "analysis resumed";
    }
}

void MVMEContextPrivate::clearLog()
{
    QMutexLocker lock(&m_logBufferMutex);
    m_logBuffer.clear();

    if (m_q->m_mainwin)
    {
        m_q->m_mainwin->clearLog();
    }
}

MVMEContext::MVMEContext(mvme *mainwin, QObject *parent)
    : QObject(parent)
    , m_d(new MVMEContextPrivate)
    , m_listFileFormat(ListFileFormat::ZIP)
    , m_ctrlOpenTimer(new QTimer(this))
    , m_logTimer(new QTimer(this))
    , m_readoutThread(new QThread(this))
    , m_readoutWorker(new VMUSBReadoutWorker(this))
    , m_bufferProcessor(new VMUSBBufferProcessor(this))
    , m_eventThread(new QThread(this))
    , m_eventProcessor(new MVMEEventProcessor(this))
    , m_mainwin(mainwin)

    , m_mode(GlobalMode::NotSet)
    , m_daqState(DAQState::Idle)
    , m_listFileWorker(new ListFileReader(m_daqStats))
    , m_analysis_ng(new analysis::Analysis)
{
    m_d->m_q = this;

    for (size_t i=0; i<DataBufferCount; ++i)
    {
        m_freeBufferQueue.queue.push_back(new DataBuffer(DataBufferSize));
    }

    // TODO: maybe hide these things a bit
    m_listFileWorker->m_freeBufferQueue = &m_freeBufferQueue;
    m_listFileWorker->m_filledBufferQueue = &m_filledBufferQueue;
    m_bufferProcessor->m_freeBufferQueue = &m_freeBufferQueue;
    m_bufferProcessor->m_filledBufferQueue = &m_filledBufferQueue;
    m_eventProcessor->m_freeBufferQueue = &m_freeBufferQueue;
    m_eventProcessor->m_filledBufferQueue = &m_filledBufferQueue;

#if 0
    auto bufferQueueDebugTimer = new QTimer(this);
    bufferQueueDebugTimer->start(5000);
    connect(bufferQueueDebugTimer, &QTimer::timeout, this, [this] () {
        qDebug() << "MVMEContext:"
            << "free buffers:" << m_freeBufferQueue.queue.size()
            << "filled buffers:" << m_filledBufferQueue.queue.size();
    });
#endif

    connect(m_ctrlOpenTimer, &QTimer::timeout, this, &MVMEContext::tryOpenController);
    m_ctrlOpenTimer->setInterval(TryOpenControllerInterval_ms);
    m_ctrlOpenTimer->start();

    connect(&m_ctrlOpenWatcher, &QFutureWatcher<VMEError>::finished, this, [this] {
        auto result = m_ctrlOpenWatcher.result();
        if (!result.isError())
        {
            if (auto vmusb = dynamic_cast<VMUSB *>(m_controller))
            {
                vmusb->readAllRegisters();

                u32 fwReg = vmusb->getFirmwareId();
                u32 fwMajor = (fwReg & 0xFFFF);
                u32 fwMinor = ((fwReg >> 16) &  0xFFFF);


                logMessage(QString("Opened VME controller %1 - Firmware Version %2_%3")
                           .arg(m_controller->getIdentifyingString())
                           .arg(fwMajor, 4, 16, QLatin1Char('0'))
                           .arg(fwMinor, 4, 16, QLatin1Char('0'))
                           );
            }
            else
            {
                logMessage(QString("Opened VME controller %1")
                           .arg(m_controller->getIdentifyingString()));
            }
        }
    });

    connect(m_logTimer, &QTimer::timeout, this, &MVMEContext::logModuleCounters);
    m_logTimer->setInterval(PeriodicLoggingInterval_ms);


    m_readoutThread->setObjectName("mvme ReadoutThread");
    m_readoutWorker->moveToThread(m_readoutThread);
    m_bufferProcessor->moveToThread(m_readoutThread);
    m_readoutWorker->setBufferProcessor(m_bufferProcessor); // FIXME: useless
    m_listFileWorker->moveToThread(m_readoutThread);

    m_readoutThread->start();

    connect(m_readoutWorker, &VMUSBReadoutWorker::stateChanged, this, &MVMEContext::onDAQStateChanged);
    connect(m_readoutWorker, &VMUSBReadoutWorker::daqStopped, this, &MVMEContext::onDAQDone);

    connect(m_listFileWorker, &ListFileReader::stateChanged, this, &MVMEContext::onDAQStateChanged);
    connect(m_listFileWorker, &ListFileReader::replayStopped, this, &MVMEContext::onReplayDone);


    m_eventThread->setObjectName("mvme AnalysisThread");
    m_eventProcessor->moveToThread(m_eventThread);
    m_eventThread->start();
    connect(m_eventProcessor, &MVMEEventProcessor::logMessage, this, &MVMEContext::logMessage);
    connect(m_eventProcessor, &MVMEEventProcessor::stateChanged, this, &MVMEContext::onEventProcessorStateChanged);

    setMode(GlobalMode::DAQ);

    setVMEConfig(new VMEConfig(this));

    tryOpenController();
}

MVMEContext::~MVMEContext()
{
    if (getDAQState() != DAQState::Idle)
    {
        qDebug() << __PRETTY_FUNCTION__ << "waiting for DAQ/Replay to stop";

        if (getMode() == GlobalMode::DAQ)
        {
            QMetaObject::invokeMethod(m_readoutWorker, "stop", Qt::QueuedConnection);
        }
        else if (getMode() == GlobalMode::ListFile)
        {
            QMetaObject::invokeMethod(m_listFileWorker, "stop", Qt::QueuedConnection);
        }

        while ((getDAQState() != DAQState::Idle))
        {
            processQtEvents();
            QThread::msleep(50);
        }
    }

    if (getEventProcessorState() != EventProcessorState::Idle)
    {
        qDebug() << __PRETTY_FUNCTION__ << "waiting for event processing to stop";

        QMetaObject::invokeMethod(m_eventProcessor, "stopProcessing", Qt::QueuedConnection, Q_ARG(bool, false));

        while (getEventProcessorState() != EventProcessorState::Idle)
        {
            processQtEvents();
            QThread::msleep(50);
        }
    }

    m_readoutThread->quit();
    m_readoutThread->wait();
    m_eventThread->quit();
    m_eventThread->wait();

    // Wait for possibly active VMEController::open() to return before deleting
    // the controller object.
    m_ctrlOpenFuture.waitForFinished();

    // Disconnect controller signals so that we're not emitting our own
    // controllerStateChanged anymore.
    disconnect(m_controller, &VMEController::controllerStateChanged, this, &MVMEContext::controllerStateChanged);
    // Same for daqStateChanged() and eventProcessorStateChanged
    disconnect(m_readoutWorker, &VMUSBReadoutWorker::stateChanged, this, &MVMEContext::onDAQStateChanged);
    disconnect(m_listFileWorker, &ListFileReader::stateChanged, this, &MVMEContext::onDAQStateChanged);
    disconnect(m_eventProcessor, &MVMEEventProcessor::stateChanged, this, &MVMEContext::onEventProcessorStateChanged);

    delete m_controller;
    delete m_analysis_ng;
    delete m_readoutWorker;
    delete m_bufferProcessor;
    delete m_eventProcessor;
    delete m_listFileWorker;
    delete m_listFile;

    Q_ASSERT(m_freeBufferQueue.queue.size() + m_filledBufferQueue.queue.size() == DataBufferCount);
    qDeleteAll(m_freeBufferQueue.queue);
    qDeleteAll(m_filledBufferQueue.queue);

    delete m_d;

    qDebug() << __PRETTY_FUNCTION__ << "context being destroyed";
}

void MVMEContext::setVMEConfig(VMEConfig *config)
{
    // TODO: create new vmecontroller and the corresponding readout worker if
    // the controller type changed.

    if (m_vmeConfig)
    {
        for (auto eventConfig: m_vmeConfig->getEventConfigs())
            onEventAboutToBeRemoved(eventConfig);

        for (auto key: m_vmeConfig->vmeScriptLists.keys())
        {
            auto scriptList = m_vmeConfig->vmeScriptLists[key];

            for (auto vmeScript: scriptList)
                emit objectAboutToBeRemoved(vmeScript);
        }

        m_vmeConfig->deleteLater();
    }

    m_vmeConfig = config;
    config->setParent(this);

    for (auto event: config->eventConfigs)
        onEventAdded(event);

    connect(m_vmeConfig, &VMEConfig::eventAdded, this, &MVMEContext::onEventAdded);
    connect(m_vmeConfig, &VMEConfig::eventAboutToBeRemoved, this, &MVMEContext::onEventAboutToBeRemoved);
    connect(m_vmeConfig, &VMEConfig::globalScriptAboutToBeRemoved, this, &MVMEContext::onGlobalScriptAboutToBeRemoved);

    emit daqConfigChanged(config);
}

void MVMEContext::setController(VMEController *controller)
{
    m_controller = controller;
    connect(m_controller, &VMEController::controllerStateChanged,
            this, &MVMEContext::controllerStateChanged);
    emit vmeControllerSet(controller);
}

ControllerState MVMEContext::getControllerState() const
{
    auto result = ControllerState::Unknown;
    if (m_controller)
        result = m_controller->getState();
    return result;
}

QString MVMEContext::getUniqueModuleName(const QString &prefix) const
{
    auto moduleConfigs = m_vmeConfig->getAllModuleConfigs();
    QSet<QString> moduleNames;

    for (auto cfg: moduleConfigs)
    {
        if (cfg->objectName().startsWith(prefix))
        {
            moduleNames.insert(cfg->objectName());
        }
    }

    QString result = prefix;
    u32 suffix = 0;
    while (moduleNames.contains(result))
    {
        result = QString("%1_%2").arg(prefix).arg(suffix++);
    }
    return result;
}

void MVMEContext::tryOpenController()
{
    if (m_controller && !m_controller->isOpen() && !m_ctrlOpenFuture.isRunning())
    {
        m_ctrlOpenFuture = QtConcurrent::run(m_controller, &VMEController::openFirstDevice);
        m_ctrlOpenWatcher.setFuture(m_ctrlOpenFuture);
    }
}

void MVMEContext::logModuleCounters()
{
#if 1

    QString buffer;
    QTextStream stream(&buffer);

    stream << endl;
    stream << "Buffers: " << m_daqStats.totalBuffersRead << endl;
    stream << "Events:  " << m_daqStats.totalEventsRead << endl;
    stream << "MVME format buffers seen: " << m_daqStats.mvmeBuffersSeen
        << ", errors: " << m_daqStats.mvmeBuffersWithErrors
        << endl;

    const auto &counters = m_daqStats.eventCounters;

    //stream << "Got " << m_daqStats.eventCounters.size() << " event counters" << endl;

    for (auto it = counters.begin();
         it != counters.end();
         ++it)
    {
        auto mod = qobject_cast<ModuleConfig *>(it.key());
        auto counters = it.value();

        if (mod)
        {
            stream << mod->objectName() << endl;
            stream << "  Events:  " << counters.events << endl;
            stream << "  Headers: " << counters.headerWords << endl;
            stream << "  Data:    " << counters.dataWords << endl;
            stream << "  EOE:     " << counters.eoeWords << endl;
            stream << "  avg event size: " << ((float)counters.dataWords / (float)counters.events) << endl;
            stream << "  data/headers: " << ((float)counters.dataWords / (float)counters.headerWords) << endl;
        }
    }

    logMessage(buffer);
#endif
}

void MVMEContext::onDAQStateChanged(DAQState state)
{
    m_daqState = state;
    emit daqStateChanged(state);

    switch (state)
    {
        case DAQState::Idle:
            {
                logModuleCounters();
            } break;

        case DAQState::Starting:
            {
                //m_logTimer->start();
            } break;

        case DAQState::Running:
        case DAQState::Paused:
            break;

        case DAQState::Stopping:
            {
                //m_logTimer.stop();
            } break;
    }
}

void MVMEContext::onEventProcessorStateChanged(EventProcessorState state)
{
    emit eventProcessorStateChanged(state);
}

// Called on VMUSBReadoutWorker::daqStopped()
void MVMEContext::onDAQDone()
{
    QMetaObject::invokeMethod(m_eventProcessor, "stopProcessing", Qt::QueuedConnection);
}

// Called on ListFileReader::replayStopped()
void MVMEContext::onReplayDone()
{
    QMetaObject::invokeMethod(m_eventProcessor, "stopProcessing", Qt::QueuedConnection);

    double secondsElapsed = m_replayTime.elapsed() / 1000.0;
    u64 replayBytes = m_daqStats.totalBytesRead;
    double replayMB = (double)replayBytes / (1024.0 * 1024.0);
    double mbPerSecond = 0.0;
    if (secondsElapsed > 0)
    {
        mbPerSecond = replayMB / secondsElapsed;
    }

    QString str = QString("Replay finished: Read %1 MB in %2 s, %3 MB/s\n")
        .arg(replayMB)
        .arg(secondsElapsed)
        .arg(mbPerSecond)
        ;

    logMessage(str);
}

DAQState MVMEContext::getDAQState() const
{
    return m_daqState;
}

EventProcessorState MVMEContext::getEventProcessorState() const
{
    // FIXME: might be better to keep a local copy which is _only_ updated
    // through the signal/slot mechanism. That way it's thread safe.
    return m_eventProcessor->getState();
}

void MVMEContext::setReplayFile(ListFile *listFile)
{
    if (getDAQState() != DAQState::Idle)
    {
        stopDAQ();
    }

    auto configJson = listFile->getDAQConfig();
    auto daqConfig = new VMEConfig;
    auto readResult = daqConfig->readVMEConfig(configJson);

    if (!readResult)
    {
        readResult.errorData["Source file"] = listFile->getFileName();
        QMessageBox::critical(nullptr,
                              QSL("Error loading VME config"),
                              readResult.toRichText());
        delete listFile;
        return;
    }

    setVMEConfig(daqConfig);

    delete m_listFile;
    m_listFile = listFile;
    m_listFileWorker->setListFile(listFile);
    setConfigFileName(QString(), false);
    setMode(GlobalMode::ListFile);
}

void MVMEContext::closeReplayFile()
{
    if (getMode() == GlobalMode::ListFile)
    {
        stopDAQ();

        delete m_listFile;
        m_listFile = nullptr;
        m_listFileWorker->setListFile(nullptr);

        /* Open the last used VME config in the workspace. Create a new VME config
         * if no previous exists. */

        QString lastVMEConfig = makeWorkspaceSettings()->value(QSL("LastVMEConfig")).toString();

        if (!lastVMEConfig.isEmpty())
        {
            QDir wsDir(getWorkspaceDirectory());
            loadVMEConfig(wsDir.filePath(lastVMEConfig));
        }
        else
        {
            setVMEConfig(new VMEConfig);
            setConfigFileName(QString());
            setMode(GlobalMode::DAQ);
        }
    }
}

void MVMEContext::setMode(GlobalMode mode)
{
    if (mode != m_mode)
    {
        m_mode = mode;
        emit modeChanged(m_mode);
    }
}

GlobalMode MVMEContext::getMode() const
{
    return m_mode;
}

void MVMEContext::addObject(QObject *object)
{
    qDebug() << __PRETTY_FUNCTION__ << object;
    m_objects.insert(object);
    emit objectAdded(object);
}

void MVMEContext::removeObject(QObject *object, bool doDeleteLater)
{
    if (m_objects.contains(object))
    {
        //qDebug() << __PRETTY_FUNCTION__ << object;
        emit objectAboutToBeRemoved(object);
        m_objects.remove(object);
        if (doDeleteLater)
            object->deleteLater();
    }
}

bool MVMEContext::containsObject(QObject *object)
{
    return m_objects.contains(object);
}

void MVMEContext::addObjectMapping(QObject *key, QObject *value, const QString &category)
{
    //qDebug() << __PRETTY_FUNCTION__ << category << key << "->" << value;
    m_objectMappings[category][key] = value;
    emit objectMappingAdded(key, value, category);
}

QObject *MVMEContext::removeObjectMapping(QObject *key, const QString &category)
{
    if (auto value = m_objectMappings[category].take(key))
    {
        //qDebug() << __PRETTY_FUNCTION__ << category << key << "->" << value;
        emit objectMappingRemoved(key, value, category);
        return value;
    }
    return nullptr;
}

QObject *MVMEContext::getMappedObject(QObject *key, const QString &category) const
{
    return m_objectMappings[category].value(key, nullptr);
}

void MVMEContext::setConfigFileName(QString name, bool updateWorkspace)
{
    if (m_configFileName != name)
    {
        m_configFileName = name;
        if (updateWorkspace)
        {
            makeWorkspaceSettings()->setValue(QSL("LastVMEConfig"), name.remove(getWorkspaceDirectory() + '/'));
        }
        emit daqConfigFileNameChanged(name);
    }
}

void MVMEContext::setAnalysisConfigFileName(QString name, bool updateWorkspace)
{
    if (m_analysisConfigFileName != name)
    {
        m_analysisConfigFileName = name;
        if (updateWorkspace)
        {
            makeWorkspaceSettings()->setValue(QSL("LastAnalysisConfig"), name.remove(getWorkspaceDirectory() + '/'));
        }
        emit analysisConfigFileNameChanged(name);
    }
}

/* Notifies MVMEEventProcessor and the analysis that a new run is going to start.
 * Reset DAQ stats. */
void MVMEContext::prepareStart()
{
#if 0
    // Use this to force a crash in case deleted objects remain in the object set.
    for (auto it=m_objects.begin(); it!=m_objects.end(); ++it)
    {
        qDebug() << reinterpret_cast<void *>(*it);
        qDebug() << *it;
    }
#endif

    m_eventProcessor->newRun();

    m_daqStats = DAQStats();

    qDebug() << __PRETTY_FUNCTION__
        << "free buffers:" << m_freeBufferQueue.queue.size()
        << "filled buffers:" << m_filledBufferQueue.queue.size();
}

void MVMEContext::startDAQ(quint32 nCycles)
{
    Q_ASSERT(getDAQState() == DAQState::Idle);
    Q_ASSERT(getEventProcessorState() == EventProcessorState::Idle);

    if (m_mode != GlobalMode::DAQ
        || getDAQState() != DAQState::Idle
        || getEventProcessorState() != EventProcessorState::Idle)
    {
        return;
    }

    emit daqAboutToStart(nCycles);

    // Generate new RunInfo here. Has to happen before prepareStart() calls
    // MVMEEventProcessor::newRun()
    auto now = QDateTime::currentDateTime();
    m_d->m_runInfo.runId = now.toString("yyMMdd_HHmmss");

    prepareStart();
    m_d->clearLog();
    logMessage(QSL("DAQ starting"));
    QMetaObject::invokeMethod(m_readoutWorker, "start",
                              Qt::QueuedConnection, Q_ARG(quint32, nCycles));
    QMetaObject::invokeMethod(m_eventProcessor, "startProcessing",
                              Qt::QueuedConnection);
}

void MVMEContext::stopDAQ()
{
    m_d->stopDAQ();
}

void MVMEContext::pauseDAQ()
{
    QMetaObject::invokeMethod(m_readoutWorker, "pause", Qt::QueuedConnection);
}

void MVMEContext::resumeDAQ()
{
    QMetaObject::invokeMethod(m_readoutWorker, "resume", Qt::QueuedConnection);
}

void MVMEContext::startReplay(u32 nEvents)
{
    Q_ASSERT(getDAQState() == DAQState::Idle);
    Q_ASSERT(getEventProcessorState() == EventProcessorState::Idle);

    if (m_mode != GlobalMode::ListFile || !m_listFile
        || getDAQState() != DAQState::Idle
        || getEventProcessorState() != EventProcessorState::Idle)
    {
        return;
    }

    // Extract a runId from the listfile filename.
    QFileInfo fi(m_listFile->getFileName());
    m_d->m_runInfo.runId = fi.completeBaseName();


    prepareStart();
    logMessage(QSL("Replay starting"));
    m_d->clearLog();

    m_listFileWorker->setEventsToRead(nEvents);
    m_eventProcessor->setListFileVersion(m_listFile->getFileVersion());

    QMetaObject::invokeMethod(m_listFileWorker, "start", Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_eventProcessor, "startProcessing", Qt::QueuedConnection);

    m_replayTime.restart();
}

void MVMEContext::pauseReplay()
{
    QMetaObject::invokeMethod(m_listFileWorker, "pause", Qt::QueuedConnection);
}

void MVMEContext::resumeReplay(u32 nEvents)
{
    Q_ASSERT(getDAQState() == DAQState::Idle || getDAQState() == DAQState::Paused);
    m_listFileWorker->setEventsToRead(nEvents);
    QMetaObject::invokeMethod(m_listFileWorker, "resume", Qt::QueuedConnection);
}

void MVMEContext::addObjectWidget(QWidget *widget, QObject *object, const QString &stateKey)
{
    if (m_mainwin)
    {
        m_mainwin->addObjectWidget(widget, object, stateKey);
    }
}

bool MVMEContext::hasObjectWidget(QObject *object) const
{
    bool result = false;

    if (m_mainwin)
    {
        result = m_mainwin->hasObjectWidget(object);
    }

    return result;
}

QWidget *MVMEContext::getObjectWidget(QObject *object) const
{
    QWidget *result = nullptr;

    if (m_mainwin)
    {
        result = m_mainwin->getObjectWidget(object);
    }

    return result;
}

QList<QWidget *> MVMEContext::getObjectWidgets(QObject *object) const
{
    QList<QWidget *> result;

    if (m_mainwin)
    {
        result = m_mainwin->getObjectWidgets(object);
    }

    return result;
}

void MVMEContext::activateObjectWidget(QObject *object)
{
    if (m_mainwin)
    {
        m_mainwin->activateObjectWidget(object);
    }
}

void MVMEContext::addWidget(QWidget *widget, const QString &stateKey)
{
    if (m_mainwin)
    {
        m_mainwin->addWidget(widget, stateKey);
    }
}

void MVMEContext::logMessageRaw(const QString &msg)
{
    QMutexLocker lock(&m_d->m_logBufferMutex);
    m_d->m_logBuffer.append(msg);
    emit sigLogMessage(msg);
}

void MVMEContext::logMessage(const QString &msg)
{
    QString fullMessage(QString("%1: %2")
             .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
             .arg(msg));

    logMessageRaw(fullMessage);
}

QStringList MVMEContext::getLogBuffer() const
{
    QMutexLocker lock(&m_d->m_logBufferMutex);
    return m_d->m_logBuffer;
}

void MVMEContext::onEventAdded(EventConfig *event)
{
    emit eventAdded(event);
    for (auto module: event->modules)
        onModuleAdded(module);

    connect(event, &EventConfig::moduleAdded, this, &MVMEContext::onModuleAdded);
    connect(event, &EventConfig::moduleAboutToBeRemoved, this, &MVMEContext::onModuleAboutToBeRemoved);
}

void MVMEContext::onEventAboutToBeRemoved(EventConfig *config)
{
    for (auto module: config->modules)
    {
        onModuleAboutToBeRemoved(module);
        emit objectAboutToBeRemoved(module);
    }

    for (auto key: config->vmeScripts.keys())
        emit objectAboutToBeRemoved(config->vmeScripts[key]);

    emit objectAboutToBeRemoved(config);
    emit eventAboutToBeRemoved(config);
}

void MVMEContext::onGlobalScriptAboutToBeRemoved(VMEScriptConfig *config)
{
    emit objectAboutToBeRemoved(config);
}

void MVMEContext::onModuleAdded(ModuleConfig *module)
{
    //qDebug() << __PRETTY_FUNCTION__ << module;
    emit moduleAdded(module);
}

void MVMEContext::onModuleAboutToBeRemoved(ModuleConfig *module)
{
    auto vmeScripts = module->findChildren<VMEScriptConfig *>();
    for (auto script: vmeScripts)
    {
        emit objectAboutToBeRemoved(script);
    }
    emit moduleAboutToBeRemoved(module);
}

//QFuture<vme_script::ResultList>
/* FIXME: this is a bad hack
 * ExcludeUserInputEvents is used to "freeze" the GUI in case the transition to
 * Paused state takes some time. This prevents the user from clicking "run"
 * multiple times (or invoking run via different gui elements)
 * What if the transition to Paused never happens? We're stuck here...
 *
 * It would probably be better to use a QProgressDialog here. Maybe the same
 * approach as in stopDAQ() works here?
 */
vme_script::ResultList
MVMEContext::runScript(const vme_script::VMEScript &script,
                       vme_script::LoggerFun logger,
                       bool logEachResult)
{
    auto may_run_script = [this]()
    {
        auto daqState = this->getDAQState();
        return (daqState == DAQState::Idle || daqState == DAQState::Paused);
    };

    bool wasPaused = (getDAQState() == DAQState::Paused);
    pauseDAQ();
    while (!may_run_script())
    {
        processQtEvents(QEventLoop::ExcludeUserInputEvents | QEventLoop::WaitForMoreEvents);
    }

    auto result = vme_script::run_script(m_controller, script, logger, logEachResult);

    if (!wasPaused)
        resumeDAQ();

    return result;
}

//
// Workspace handling
//
void MVMEContext::newWorkspace(const QString &dirName)
{
    QDir dir(dirName);

    if (!dir.entryList(QDir::AllEntries | QDir::NoDot | QDir::NoDotDot).isEmpty())
        throw QString(QSL("Selected directory is not empty"));

    setWorkspaceDirectory(dirName);

    auto workspaceSettings(makeWorkspaceSettings());
    //workspaceSettings->setValue(QSL("LastVMEConfig"), QSL("vme.mvmecfg"));
    //workspaceSettings->setValue(QSL("LastAnalysisConfig"), QSL("analysis.analysis"));
    //workspaceSettings->setValue(QSL("ListFileDirectory"), QSL("listfiles"));
    workspaceSettings->setValue(QSL("WriteListFile"), true);
    //workspaceSettings->setValue(QSL("PlotsDirectory"), QSL("plots"));
    // Creates the mvmeworkspace.ini file
    workspaceSettings->sync();

    if (workspaceSettings->status() != QSettings::NoError)
    {
        throw QString("Error writing workspace settings to %1")
            .arg(workspaceSettings->fileName());
    }

    openWorkspace(dirName);
}

void MVMEContext::openWorkspace(const QString &dirName)
{
    QDir dir(dirName);

    if (!dir.exists())
    {
        throw QString ("Workspace directory %1 does not exist.")
            .arg(dirName);
    }

    if (!dir.exists(WorkspaceIniName))
    {
        throw QString("Workspace settings file %1 not found in %2.")
            .arg(WorkspaceIniName)
            .arg(dirName);
    }

    if (!QDir::setCurrent(dirName))
    {
        throw QString("Could not change directory to workspace path %1.")
            .arg(dirName);
    }

    QString lastWorkspaceDirectory(m_workspaceDir);

    try
    {
        setWorkspaceDirectory(dirName);
        auto workspaceSettings(makeWorkspaceSettings(dirName));

        // listfile subdir
        {
            QDir dir(getWorkspacePath(QSL("ListFileDirectory"), QSL("listfiles")));

            if (!QDir::root().mkpath(dir.absolutePath()))
            {
                throw QString(QSL("Error creating listfiles directory %1.")).arg(dir.path());
            }
        }

        // plots subdir
        {
            QDir dir(getWorkspacePath(QSL("PlotsDirectory"), QSL("plots")));

            if (!QDir::root().mkpath(dir.absolutePath()))
            {
                throw QString(QSL("Error creating plots directory %1.")).arg(dir.path());
            }
        }

        {
            ListFileOutputInfo info = {};
            info.enabled   = workspaceSettings->value(QSL("WriteListFile"), QSL("true")).toBool();
            info.format    = fromString(workspaceSettings->value(QSL("ListFileFormat"), QSL("Plain")).toString());
            info.directory = workspaceSettings->value(QSL("ListFileDirectory"), QSL("listfiles")).toString();
            info.compressionLevel = workspaceSettings->value(QSL("ListFileCompressionLevel"), ListFileDefaultCompression).toInt();

            QDir listFileOutputDir(info.directory);

            if (listFileOutputDir.isAbsolute() && !listFileOutputDir.exists())
            {
                /* A non-existant absolute path was loaded from the INI -> go back
                 * to the default of "listfiles". */
                logMessage(QString("Warning: Listfile directory %1 does not exist. Reverting back to default of \"listfiles\".")
                           .arg(info.directory));
                workspaceSettings->setValue(QSL("ListFileDirectory"), QSL("listfiles"));
                info.directory = QSL("lisfiles");
                // TODO: create if it does not exist
            }

            m_d->m_listFileOutputInfo = info;
        }

        auto lastVMEConfig      = workspaceSettings->value(QSL("LastVMEConfig")).toString();

        if (!lastVMEConfig.isEmpty())
        {
            qDebug() << __PRETTY_FUNCTION__ << "loading vme config" << lastVMEConfig;
            loadVMEConfig(dir.filePath(lastVMEConfig));
        }
        else
        {
            qDebug() << __PRETTY_FUNCTION__ << "setting default vme filename";
            // No previous filename is known so use a default name without updating
            // the workspace settings.
            setConfigFileName(QSL("vme.vme"), false);
        }

        auto lastAnalysisConfig = workspaceSettings->value(QSL("LastAnalysisConfig")).toString();

        if (!lastAnalysisConfig.isEmpty())
        {
            qDebug() << __PRETTY_FUNCTION__ << "loading analysis config" << lastAnalysisConfig;
            loadAnalysisConfig(dir.filePath(lastAnalysisConfig));
        }
        else
        {
            qDebug() << __PRETTY_FUNCTION__ << "setting default analysis filename";
            setAnalysisConfigFileName(QSL("analysis.analysis"), false);
        }

        // No exceptions thrown -> store workspace directory in global settings
        QSettings settings;
        settings.setValue(QSL("LastWorkspaceDirectory"), getWorkspaceDirectory());
    }
    catch (const QString &)
    {
        // Restore previous workspace directory as the load was not successfull
        setWorkspaceDirectory(lastWorkspaceDirectory);
        throw;
    }
}

void MVMEContext::setWorkspaceDirectory(const QString &dirName)
{
    if (m_workspaceDir != dirName)
    {
        m_workspaceDir = dirName;
        emit workspaceDirectoryChanged(dirName);
    }
}

std::shared_ptr<QSettings> MVMEContext::makeWorkspaceSettings() const
{
    return makeWorkspaceSettings(getWorkspaceDirectory());
}

std::shared_ptr<QSettings> MVMEContext::makeWorkspaceSettings(const QString &workspaceDirectory) const
{
    QDir dir(workspaceDirectory);
    return std::make_shared<QSettings>(dir.filePath(WorkspaceIniName), QSettings::IniFormat);
}

QString MVMEContext::getWorkspacePath(const QString &settingsKey, const QString &defaultValue, bool setIfDefaulted) const
{
    auto settings = makeWorkspaceSettings();
    if (!settings->contains(settingsKey) && setIfDefaulted)
    {
        settings->setValue(settingsKey, defaultValue);
    }

    QString settingsValue(settings->value(settingsKey, defaultValue).toString());
    QDir dir(settingsValue);

    if (dir.isAbsolute())
    {
        return dir.path();
    }

    return QDir(getWorkspaceDirectory()).filePath(settingsValue);
}

void MVMEContext::loadVMEConfig(const QString &fileName)
{
    QJsonDocument doc(gui_read_json_file(fileName));
    auto daqConfig = new VMEConfig;
    auto readResult = daqConfig->readVMEConfig(doc.object()["DAQConfig"].toObject());

    if (!readResult)
    {
        readResult.errorData["Source file"] = fileName;
        QMessageBox::critical(nullptr,
                              QSL("Error loading VME config"),
                              readResult.toRichText());
        return;
    }

    setVMEConfig(daqConfig);
    setConfigFileName(fileName);
    setMode(GlobalMode::DAQ);
}

bool MVMEContext::loadAnalysisConfig(const QString &fileName)
{
    qDebug() << "loadAnalysisConfig from" << fileName;

    QJsonDocument doc(gui_read_json_file(fileName));

    if (loadAnalysisConfig(doc, QFileInfo(fileName).fileName()))
    {
        setAnalysisConfigFileName(fileName);
        return true;
    }

    return false;
}

bool MVMEContext::loadAnalysisConfig(QIODevice *input, const QString &inputInfo)
{
    QJsonDocument doc(gui_read_json(input));

    if (loadAnalysisConfig(doc, inputInfo))
    {
        setAnalysisConfigFileName(QString());
        return true;
    }

    return false;
}

bool MVMEContext::loadAnalysisConfig(const QJsonDocument &doc, const QString &inputInfo)
{
    using namespace analysis;
    using namespace vme_analysis_common;

    auto json = doc.object()[QSL("AnalysisNG")].toObject();

    auto analysis_ng = std::make_unique<Analysis>();
    auto readResult = analysis_ng->read(json, getVMEConfig());

    if (!readResult)
    {
        readResult.errorData["Source file"] = inputInfo;
        QMessageBox::critical(nullptr,
                              QSL("Error loading analysis"),
                              readResult.toRichText());
        return false;
    }

    if (!auto_assign_vme_modules(getVMEConfig(), analysis_ng.get()))
    {
        if (!run_vme_analysis_module_assignment_ui(getVMEConfig(), analysis_ng.get(), getMainWindow()))
            return false;
    }

    remove_analysis_objects_unless_matching(analysis_ng.get(), getVMEConfig());

    try
    {
        bool was_running = isAnalysisRunning();

        if (was_running)
        {
            stopAnalysis();
        }

        delete m_analysis_ng;
        m_analysis_ng = analysis_ng.release();

        // Prepares operators, allocates histograms, etc..
        // This should in reality be the only place to throw a bad_alloc
        m_eventProcessor->newRun();

        emit analysisChanged();

        logMessage(QString("Loaded %1 from %2")
                   .arg(info_string(m_analysis_ng))
                   .arg(inputInfo)
                   );

        if (was_running)
        {
            resumeAnalysis();
        }
    }
    catch (const std::bad_alloc &e)
    {
        m_analysis_ng->clear();
        setAnalysisConfigFileName(QString());
        QMessageBox::critical(m_mainwin, QSL("Error"), QString("Out of memory when creating analysis objects."));
        emit analysisChanged();

        return false;
    }

    return true;
}

void MVMEContext::setListFileOutputInfo(const ListFileOutputInfo &info)
{
    m_d->m_listFileOutputInfo = info;

    auto settings = makeWorkspaceSettings();
    settings->setValue(QSL("WriteListFile"), info.enabled);
    settings->setValue(QSL("ListFileFormat"), toString(info.format));
    settings->setValue(QSL("ListFileDirectory"), info.directory);
    settings->setValue(QSL("ListFileCompressionLevel"), info.compressionLevel);
}

ListFileOutputInfo MVMEContext::getListFileOutputInfo() const
{
    return m_d->m_listFileOutputInfo;
}

QString MVMEContext::getListFileOutputDirectoryFullPath() const
{
    QDir dir(getListFileOutputInfo().directory);

    if (dir.isAbsolute())
        return dir.path();

    dir = QDir(getWorkspaceDirectory());
    return dir.filePath(getListFileOutputInfo().directory);
}

/** True if at least one of VME-config and analysis-config is modified. */
bool MVMEContext::isWorkspaceModified() const
{
    return ((m_vmeConfig && m_vmeConfig->isModified())
            || (m_analysis_ng && m_analysis_ng->isModified())
           );
}

bool MVMEContext::isAnalysisRunning()
{
    return (getEventProcessorState() != EventProcessorState::Idle);
}

void MVMEContext::stopAnalysis()
{
    m_d->stopAnalysis();
}

void MVMEContext::resumeAnalysis()
{
    m_d->resumeAnalysis();
}

QJsonDocument MVMEContext::getAnalysisJsonDocument() const
{
    QJsonObject dest, json;
    getAnalysis()->write(dest);
    json[QSL("AnalysisNG")] = dest;
    QJsonDocument doc(json);
    return doc;
}

void MVMEContext::addAnalysisOperator(QUuid eventId, const std::shared_ptr<analysis::OperatorInterface> &op, s32 userLevel)
{
    auto eventConfig = m_vmeConfig->getEventConfig(eventId);
    if (eventConfig)
    {
        AnalysisPauser pauser(this);
        getAnalysis()->addOperator(eventId, op, userLevel);

        if (m_analysisUi)
        {
            m_analysisUi->operatorAdded(op);
        }
    }
}

void MVMEContext::analysisOperatorEdited(const std::shared_ptr<analysis::OperatorInterface> &op)
{
    AnalysisPauser pauser(this);
    m_analysis_ng->setModified();
    analysis::do_beginRun_forward(op.get());

    if (m_analysisUi)
    {
        m_analysisUi->operatorEdited(op);
    }
}

RunInfo MVMEContext::getRunInfo() const
{
    return m_d->m_runInfo;
}

AnalysisPauser::AnalysisPauser(MVMEContext *context)
    : context(context)
{
    was_running = context->isAnalysisRunning();

    qDebug() << __PRETTY_FUNCTION__ << was_running;

    if (was_running)
    {
        context->stopAnalysis();
    }
}

AnalysisPauser::~AnalysisPauser()
{
    qDebug() << __PRETTY_FUNCTION__ << was_running;
    if (was_running)
    {
        context->resumeAnalysis();
    }
}

QPair<bool, QString> saveAnalysisConfig(analysis::Analysis *analysis,
                                        const QString &fileName,
                                        QString startPath,
                                        QString fileFilter,
                                        MVMEContext *context)
{
    vme_analysis_common::add_vme_properties_to_analysis(context->getVMEConfig(), analysis);
    return gui_saveAnalysisConfig(analysis, fileName, startPath, fileFilter);
}

QPair<bool, QString> saveAnalysisConfigAs(analysis::Analysis *analysis,
                                          QString startPath,
                                          QString fileFilter,
                                          MVMEContext *context)
{
    vme_analysis_common::add_vme_properties_to_analysis(context->getVMEConfig(), analysis);
    return gui_saveAnalysisConfigAs(analysis, startPath, fileFilter);
}
