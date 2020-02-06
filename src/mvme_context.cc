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

// Includes winsock2.h so it's placed at the top to avoid warnings.
#include "mvlc/mvlc_impl_eth.h"

#include "mvme_context.h"

#include "analysis/a2_adapter.h"
#include "analysis/a2/memory.h"
#include "analysis/analysis.h"
#include "analysis/analysis_session.h"
#include "analysis/analysis_ui.h"
#include "event_server/server/event_server.h"
#include "file_autosaver.h"
#include "mvlc_listfile.h"
#include "mvlc_listfile_worker.h"
#include "mvlc/mvlc_error.h"
#include "mvlc/mvlc_vme_controller.h"
#include "mvlc_stream_worker.h"
#include "mvme_context_lib.h"
#include "mvme.h"
#include "mvme_listfile_worker.h"
#include "mvme_stream_worker.h"
#include "mvme_workspace.h"
#include "remote_control.h"
#include "sis3153.h"
#include "util/ticketmutex.h"
#include "vme_analysis_common.h"
#include "vme_config_ui.h"
#include "vme_controller_factory.h"
#include "vmusb_buffer_processor.h"
#include "vmusb.h"
#include "vmusb_readout_worker.h"

#include "git_sha1.h"

#include <QHostAddress>
#include <QMessageBox>
#include <QProgressDialog>
#include <QtConcurrent>
#include <QThread>
#include <QTimer>
#include <tuple>

namespace
{

/* Buffers to pass between DAQ/replay and the analysis. The buffer size should
 * be at least twice as big as the max VMUSB buffer size (2 * 64k).
 *
 * Note that increasing the size will make stopping a replay/daq session
 * mid-run take longer. This is because in the current implementation the
 * analysis system has to fully process the last buffer it pulled from the
 * shared queue.
 */
static const size_t DataBufferCount = 25;
static const size_t DataBufferSize = Megabytes(1);

static const int TryOpenControllerInterval_ms = 1000;
static const int PeriodicLoggingInterval_ms = 5000;

static const ListFileFormat DefaultListFileFormat = ListFileFormat::ZIP;
static const int DefaultListFileCompression = 1;
static const QString DefaultVMEConfigFileName = QSL("vme.vme");
static const QString DefaultAnalysisConfigFileName  = QSL("analysis.analysis");

static const QString VMEConfigAutoSaveFilename = QSL(".vme_autosave.vme");
static const QString AnalysisAutoSaveFilename  = QSL(".analysis_autosave.analysis");
static const int DefaultConfigFileAutosaveInterval_ms = 1 * 60 * 1000;

/* Maximum number of connection attempts to the current VMEController before
 * giving up. */
static const int VMECtrlConnectMaxRetryCount = 3;

/* Maximum number of entries to keep in the logbuffer. Once this is exceeded
 * the oldest entries will be removed. */
static const s64 LogBufferMaxEntries = 100 * 1000;

static const int JSON_RPC_DefaultListenPort = 13800;
static const int EventServer_DefaultListenPort = 13801;

class VMEConfigSerializer
{
    public:
        VMEConfigSerializer(MVMEContext *context)
            : m_context(context)
        { }

        QByteArray operator()()
        {
            QJsonObject contents;
            m_context->getVMEConfig()->write(contents);

            QJsonObject container;
            container["DAQConfig"] = contents;

            QJsonDocument doc(container);
            return doc.toJson();
        }

    private:
        MVMEContext *m_context;
};

class AnalysisSerializer
{
    public:
        AnalysisSerializer(MVMEContext *context)
            : m_context(context)
        { }

        QByteArray operator()()
        {
            auto vmeConfig = m_context->getVMEConfig();
            auto analysis = m_context->getAnalysis();

            vme_analysis_common::add_vme_properties_to_analysis(vmeConfig, analysis);

            QJsonObject contents;
            analysis->write(contents);

            QJsonObject container;
            container["AnalysisNG"] = contents;

            QJsonDocument doc(container);
            return doc.toJson();
        }

    private:
        MVMEContext *m_context;
};

} // end anon namespace

using remote_control::RemoteControl;

struct MVMEContextPrivate
{
    MVMEContext *m_q;
    QStringList m_logBuffer;
    QMutex m_logBufferMutex;
    ListFileOutputInfo m_listfileOutputInfo = {};
    RunInfo m_runInfo;
    u32 m_ctrlOpenRetryCount = 0;
    bool m_isFirstConnectionAttempt = false;
    mesytec::mvme::TicketMutex tryOpenControllerMutex;

    std::unique_ptr<FileAutoSaver> m_vmeConfigAutoSaver;
    std::unique_ptr<FileAutoSaver> m_analysisAutoSaver;

    std::unique_ptr<RemoteControl> m_remoteControl;
    // owned by the MVMEStreamWorker
    EventServer *m_eventServer = nullptr;

    ListfileReplayHandle listfileReplayHandle;
    std::unique_ptr<ListfileReplayWorker> listfileReplayWorker;

    void stopDAQ();
    void pauseDAQ();
    void resumeDAQ(u32 nEvents);

    // DAQ - Readout
    void stopDAQReadout();
    void pauseDAQReadout();
    void resumeDAQReadout(u32 nEvents);

    // DAQ - Replay
    void stopDAQReplay();
    void pauseDAQReplay();
    void resumeDAQReplay(u32 nEvents);

    // Analysis
    void stopAnalysis();
    void resumeAnalysis(analysis::Analysis::BeginRunOption option);

    void clearLog();
};

void MVMEContextPrivate::stopDAQ()
{
    switch (m_q->m_mode)
    {
        case GlobalMode::DAQ:       stopDAQReadout(); break;
        case GlobalMode::ListFile:  stopDAQReplay(); break;
    }
}

void MVMEContextPrivate::pauseDAQ()
{
    switch (m_q->m_mode)
    {
        case GlobalMode::DAQ:       pauseDAQReadout(); break;
        case GlobalMode::ListFile:  pauseDAQReplay(); break;
    }
}

void MVMEContextPrivate::resumeDAQ(u32 nEvents)
{
    switch (m_q->m_mode)
    {
        case GlobalMode::DAQ:       resumeDAQReadout(nEvents); break;
        case GlobalMode::ListFile:  resumeDAQReplay(nEvents); break;
    }
}

/* Stops both the readout and the analysis workers. */
void MVMEContextPrivate::stopDAQReadout()
{
    QProgressDialog progressDialog("Stopping Data Acquisition", QString(), 0, 0);
    progressDialog.setWindowModality(Qt::ApplicationModal);
    progressDialog.setCancelButton(nullptr);
    progressDialog.show();

    QEventLoop localLoop;

    if (m_q->m_readoutWorker->isRunning())
    {
        m_q->m_readoutWorker->stop();
        auto con = QObject::connect(m_q->m_readoutWorker, &VMEReadoutWorker::daqStopped,
                                    &localLoop, &QEventLoop::quit);
        localLoop.exec();
        QObject::disconnect(con);
    }

    qDebug() << __PRETTY_FUNCTION__ << "readout worker stopped";

    if (m_q->m_streamWorker->getState() != MVMEStreamWorkerState::Idle)
    {
        m_q->m_streamWorker->stop(false);
        auto con = QObject::connect(m_q->m_streamWorker.get(), &MVMEStreamWorker::stopped,
                                    &localLoop, &QEventLoop::quit);
        localLoop.exec();
        QObject::disconnect(con);
    }

    qDebug() << __PRETTY_FUNCTION__ << "stream worker stopped";

    Q_ASSERT(m_q->m_readoutWorker->getState() == DAQState::Idle);
    m_q->onDAQStateChanged(DAQState::Idle);
}

void MVMEContextPrivate::pauseDAQReadout()
{
    QProgressDialog progressDialog("Pausing Data Acquisition", QString(), 0, 0);
    progressDialog.setWindowModality(Qt::ApplicationModal);
    progressDialog.setCancelButton(nullptr);
    progressDialog.show();

    QEventLoop localLoop;

    if (m_q->m_readoutWorker->getState() == DAQState::Running)
    {
        m_q->m_readoutWorker->pause();
        auto con = QObject::connect(m_q->m_readoutWorker, &VMEReadoutWorker::daqPaused,
                                    &localLoop, &QEventLoop::quit);
        localLoop.exec();
        QObject::disconnect(con);
    }

    Q_ASSERT(m_q->m_readoutWorker->getState() == DAQState::Paused
             || m_q->m_readoutWorker->getState() == DAQState::Idle);
}

void MVMEContextPrivate::resumeDAQReadout(u32 nEvents)
{
    m_q->m_readoutWorker->resume();
}

void MVMEContextPrivate::stopDAQReplay()
{
    QProgressDialog progressDialog("Stopping Replay", QString(), 0, 0);
    progressDialog.setWindowModality(Qt::ApplicationModal);
    progressDialog.setCancelButton(nullptr);
    progressDialog.show();

    QEventLoop localLoop;

    // First stop the ListfileReplayWorker

    // The timer is used to avoid a race between the worker stopping and the
    // progress dialog entering its eventloop. (Probably not needed, see the
    // explanation about not having a race condition below.)

    if (listfileReplayWorker->getState() == DAQState::Running
        || listfileReplayWorker->getState() == DAQState::Paused)
    {
        listfileReplayWorker->stop();

        auto con = QObject::connect(
            listfileReplayWorker.get(), &ListfileReplayWorker::replayStopped,
            &localLoop, &QEventLoop::quit);

        localLoop.exec();
        QObject::disconnect(con);
    }

    // At this point the ListfileReplayWorker is stopped and will not produce any
    // more buffers. Now tell the MVMEStreamWorker to stop after finishing
    // the current queue.

    // There should be no race here. If the analysis is running we will stop it
    // and receive the stopped() signal.  If it just now stopped on its own
    // (e.g. end of replay) the signal is pending and will be delivered as soon
    // as we enter the event loop.
    if (m_q->m_streamWorker->getState() != MVMEStreamWorkerState::Idle)
    {
        m_q->m_streamWorker->stop();
        auto con = QObject::connect(m_q->m_streamWorker.get(), &MVMEStreamWorker::stopped,
                                    &localLoop, &QEventLoop::quit);
        localLoop.exec();
        QObject::disconnect(con);
    }

    Q_ASSERT(m_q->m_readoutWorker->getState() == DAQState::Idle);

    if (auto mvmeStreamWorker = qobject_cast<MVMEStreamWorker *>(m_q->m_streamWorker.get()))
    {
        mvmeStreamWorker->setListFileVersion(CurrentListfileVersion);
    }
    m_q->onDAQStateChanged(DAQState::Idle);
}

void MVMEContextPrivate::pauseDAQReplay()
{
    QProgressDialog progressDialog("Stopping Replay", QString(), 0, 0);
    progressDialog.setWindowModality(Qt::ApplicationModal);
    progressDialog.setCancelButton(nullptr);
    progressDialog.show();

    QEventLoop localLoop;

    if (listfileReplayWorker->getState() == DAQState::Running)
    {
        listfileReplayWorker->pause();

        auto con = QObject::connect(
            listfileReplayWorker.get(), &ListfileReplayWorker::replayPaused,
            &localLoop, &QEventLoop::quit);
        localLoop.exec();

        QObject::disconnect(con);

        Q_ASSERT(listfileReplayWorker->getState() == DAQState::Paused);
    }

    Q_ASSERT(m_q->m_readoutWorker->getState() == DAQState::Paused
             || m_q->m_readoutWorker->getState() == DAQState::Idle);
}

void MVMEContextPrivate::resumeDAQReplay(u32 nEvents)
{
    listfileReplayWorker->setEventsToRead(nEvents);
    listfileReplayWorker->resume();
}

void MVMEContextPrivate::stopAnalysis()
{
    QProgressDialog progressDialog("Stopping Analysis", QString(), 0, 0);
    progressDialog.setWindowModality(Qt::ApplicationModal);
    progressDialog.setCancelButton(nullptr);
    progressDialog.show();

    QEventLoop localLoop;

    if (m_q->m_streamWorker->getState() != MVMEStreamWorkerState::Idle)
    {
        // Tell the analysis top stop immediately
        m_q->m_streamWorker->stop(false);
        QObject::connect(m_q->m_streamWorker.get(), &MVMEStreamWorker::stopped,
                         &localLoop, &QEventLoop::quit);
        localLoop.exec();
    }

    qDebug() << __PRETTY_FUNCTION__ << "analysis stopped";
}

void MVMEContextPrivate::resumeAnalysis(analysis::Analysis::BeginRunOption runOption)
{
    assert(m_q->m_streamWorker->getState() == MVMEStreamWorkerState::Idle);

    if (m_q->m_streamWorker->getState() == MVMEStreamWorkerState::Idle)
    {
        // TODO: merge with the build  code in prepareStart().
        auto analysis = m_q->getAnalysis();
        analysis->beginRun(runOption, [this] (const QString &msg) { m_q->logMessage(msg); });

        bool invoked = QMetaObject::invokeMethod(m_q->m_streamWorker.get(), "start",
                                                 Qt::QueuedConnection);

        assert(invoked);

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

static void writeToSettings(const ListFileOutputInfo &info, QSettings &settings)
{
    settings.setValue(QSL("WriteListFile"),             info.enabled);
    settings.setValue(QSL("ListFileFormat"),            toString(info.format));
    settings.setValue(QSL("ListFileDirectory"),         info.directory);
    settings.setValue(QSL("ListFileCompressionLevel"),  info.compressionLevel);
    settings.setValue(QSL("ListFilePrefix"),            info.prefix);
    settings.setValue(QSL("ListFileRunNumber"),         info.runNumber);
    settings.setValue(QSL("ListFileOutputFlags"),       info.flags);
}

static ListFileOutputInfo readFromSettings(QSettings &settings)
{
    ListFileOutputInfo result;
    result.enabled          = settings.value(QSL("WriteListFile"), QSL("true")).toBool();
    result.format           = ListFileFormat::ZIP; // XXX: Forcing ListFileFormat::ZIP since 0.9.x
    result.directory        = settings.value(QSL("ListFileDirectory"), QSL("listfiles")).toString();
    result.compressionLevel = settings.value(QSL("ListFileCompressionLevel"), DefaultListFileCompression).toInt();
    result.prefix           = settings.value(QSL("ListFilePrefix"), QSL("mvmelst")).toString();
    result.runNumber        = settings.value(QSL("ListFileRunNumber"), 1u).toUInt();
    result.flags            = settings.value(QSL("ListFileOutputFlags"), ListFileOutputInfo::UseRunNumber).toUInt();

    return result;
}

MVMEContext::MVMEContext(MVMEMainWindow *mainwin, QObject *parent)
    : QObject(parent)
    , m_d(new MVMEContextPrivate)
    , m_listFileFormat(ListFileFormat::ZIP)
    , m_ctrlOpenTimer(new QTimer(this))
    , m_logTimer(new QTimer(this))
    , m_readoutThread(new QThread(this))
    , m_analysisThread(new QThread(this))
    , m_mainwin(mainwin)

    , m_mode(GlobalMode::DAQ)
    , m_daqState(DAQState::Idle)
    , m_analysis(std::make_unique<analysis::Analysis>())
{
    m_d->m_q = this;
    m_d->m_remoteControl = std::make_unique<RemoteControl>(this);

    for (size_t i=0; i<DataBufferCount; ++i)
    {
        enqueue(&m_freeBuffers, new DataBuffer(DataBufferSize));
    }

#if 0
    auto bufferQueueDebugTimer = new QTimer(this);
    bufferQueueDebugTimer->start(5000);
    connect(bufferQueueDebugTimer, &QTimer::timeout, this, [this] () {
        qDebug() << "MVMEContext:"
            << "free buffers:" << queue_size(&m_freeBuffers)
            << "filled buffers:" << queue_size(&m_fullBuffers);
    });
#endif

    connect(m_ctrlOpenTimer, &QTimer::timeout, this, &MVMEContext::tryOpenController);
    m_ctrlOpenTimer->setInterval(TryOpenControllerInterval_ms);
    m_ctrlOpenTimer->start();

    connect(&m_ctrlOpenWatcher, &QFutureWatcher<VMEError>::finished,
            this, &MVMEContext::onControllerOpenFinished);

    connect(m_logTimer, &QTimer::timeout, this, &MVMEContext::logModuleCounters);
    m_logTimer->setInterval(PeriodicLoggingInterval_ms);


    // Setup the readout side: readout thread and listfile reader.
    // The vme controller specific readout worker is created and setup in
    // setVMEController().
    m_readoutThread->setObjectName("readout");
    m_readoutThread->start();

    // Setup the analysis/data processing side.
    m_analysisThread->setObjectName("analysis");
    m_analysisThread->start();

    qDebug() << __PRETTY_FUNCTION__ << "startup: using a default constructed VMEConfig";

    setMode(GlobalMode::DAQ);
    setVMEConfig(new VMEConfig(this));

    qDebug() << __PRETTY_FUNCTION__ << "startup done, contents of logbuffer:";
    qDebug() << getLogBuffer();
}

MVMEContext::~MVMEContext()
{
    if (getDAQState() != DAQState::Idle)
    {
        qDebug() << __PRETTY_FUNCTION__ << "waiting for DAQ/Replay to stop";

        if (getMode() == GlobalMode::DAQ)
        {
            m_readoutWorker->stop();
        }
        else if (getMode() == GlobalMode::ListFile)
        {
            m_d->listfileReplayWorker->stop();
        }

        while ((getDAQState() != DAQState::Idle))
        {
            processQtEvents();
            QThread::msleep(50);
        }
    }

    if (m_streamWorker->getState() != MVMEStreamWorkerState::Idle)
    {
        qDebug() << __PRETTY_FUNCTION__ << "waiting for event processing to stop";

        m_streamWorker->stop(false);

        while (getMVMEStreamWorkerState() != MVMEStreamWorkerState::Idle)
        {
            processQtEvents();
            QThread::msleep(50);
        }
    }

    m_readoutThread->quit();
    m_readoutThread->wait();
    m_analysisThread->quit();
    m_analysisThread->wait();

    // Wait for possibly active VMEController::open() to return before deleting
    // the controller object.
    m_ctrlOpenFuture.waitForFinished();

    // Disconnect controller signals so that we're not emitting our own
    // controllerStateChanged anymore.
    disconnect(m_controller, &VMEController::controllerStateChanged,
               this, &MVMEContext::controllerStateChanged);

    // Same for daqStateChanged() and mvmeStreamWorkerStateChanged
    disconnect(m_readoutWorker, &VMEReadoutWorker::stateChanged,
               this, &MVMEContext::onDAQStateChanged);

    disconnect(m_d->listfileReplayWorker.get(), &ListfileReplayWorker::stateChanged,
               this, &MVMEContext::onDAQStateChanged);

    disconnect(m_streamWorker.get(), &MVMEStreamWorker::stateChanged,
               this, &MVMEContext::onMVMEStreamWorkerStateChanged);

    delete m_controller;
    delete m_readoutWorker;

    Q_ASSERT(queue_size(&m_freeBuffers) + queue_size(&m_fullBuffers) == DataBufferCount);
    qDeleteAll(m_freeBuffers.queue);
    qDeleteAll(m_fullBuffers.queue);

    cleanupWorkspaceAutoSaveFiles();

    delete m_d;

    qDebug() << __PRETTY_FUNCTION__ << "context being destroyed";
}

void MVMEContext::setVMEConfig(VMEConfig *config)
{
    emit vmeConfigAboutToBeSet(m_vmeConfig, config);

    if (m_vmeConfig)
    {
        for (auto eventConfig: m_vmeConfig->getEventConfigs())
            onEventAboutToBeRemoved(eventConfig);

        auto scripts = m_vmeConfig->getGlobalObjectRoot().findChildren<VMEScriptConfig *>();

        for (auto script: scripts)
            emit objectAboutToBeRemoved(script);

        m_vmeConfig->deleteLater();
    }

    m_vmeConfig = config;
    config->setParent(this);

    for (auto event: config->getEventConfigs())
        onEventAdded(event);

    connect(m_vmeConfig, &VMEConfig::eventAdded,
            this, &MVMEContext::onEventAdded);

    connect(m_vmeConfig, &VMEConfig::eventAboutToBeRemoved,
            this, &MVMEContext::onEventAboutToBeRemoved);

    connect(m_vmeConfig, &VMEConfig::globalScriptAboutToBeRemoved,
            this, &MVMEContext::onGlobalScriptAboutToBeRemoved);

    if (m_readoutWorker)
    {
        VMEReadoutWorkerContext workerContext = m_readoutWorker->getContext();
        workerContext.vmeConfig = m_vmeConfig;
        m_readoutWorker->setContext(workerContext);
    }

    setVMEController(config->getControllerType(), config->getControllerSettings());

    if (m_d->m_vmeConfigAutoSaver)
    {
        // (re)start the autosaver
        m_d->m_vmeConfigAutoSaver->start();
    }

    emit vmeConfigChanged(config);
}

// Return value is (enabled, listen_hostinfo, listen_port)
static std::tuple<bool, QHostInfo, int> get_event_server_listen_info(const QSettings &workspaceSettings)
{
    if (!workspaceSettings.value(QSL("EventServer/Enabled")).toBool())
        return std::make_tuple(false, QHostInfo(), 0);

    auto addressString = workspaceSettings.value(QSL("EventServer/ListenAddress")).toString();
    auto port = workspaceSettings.value(QSL("EventServer/ListenPort")).toInt();

    if (!addressString.isEmpty())
    {
        return std::make_tuple(true, QHostInfo::fromName(addressString), port);
    }

    return std::make_tuple(true, QHostInfo::fromName("127.0.0.1"), port);
}

bool MVMEContext::setVMEController(VMEController *controller, const QVariantMap &settings)
{
    //qDebug() << __PRETTY_FUNCTION__ << "begin";
    Q_ASSERT(getDAQState() == DAQState::Idle);
    Q_ASSERT(getMVMEStreamWorkerState() == MVMEStreamWorkerState::Idle);

    if (getDAQState() != DAQState::Idle
        || getMVMEStreamWorkerState() != MVMEStreamWorkerState::Idle)
    {
        return false;
    }

    qDebug() << __PRETTY_FUNCTION__
        << "current type =" << (m_controller ? to_string(m_controller->getType()) : QSL("none"))
        << ", new type   =" << (controller ? to_string(controller->getType()) : QSL("none"))
        ;

    /* Wait for possibly active VMEController::open() to return before deleting
     * the controller object. This can take a long time if e.g. DNS lookups are
     * performed when trying to open the current controller. This is the reason
     * for using an event loop instead of directly calling
     * m_ctrlOpenFuture.waitForFinished(). */
    //qDebug() << __PRETTY_FUNCTION__ << "before waitForFinished";

    if (m_ctrlOpenFuture.isRunning())
    {
        QProgressDialog progressDialog("Changing VME Controller", QString(), 0, 0);
        progressDialog.setWindowModality(Qt::ApplicationModal);
        progressDialog.setCancelButton(nullptr);
        progressDialog.show();

        QEventLoop localLoop;
        connect(&m_ctrlOpenWatcher, &QFutureWatcher<VMEError>::finished,
                &localLoop, &QEventLoop::quit);
        localLoop.exec();
    }
    //qDebug() << __PRETTY_FUNCTION__ << "after waitForFinished";

    emit vmeControllerAboutToBeChanged();

    //qDebug() << __PRETTY_FUNCTION__ << "after emit vmeControllerAboutToBeChanged";

    // Delete objects inside the event loop via deleteLater() because Qt events
    // may still be scheduled which may make use of the objects.
    m_readoutWorker->deleteLater();
    m_readoutWorker = nullptr;

    m_controller->deleteLater();
    m_controller = controller;

    if (m_vmeConfig->getControllerType() != controller->getType()
        || m_vmeConfig->getControllerSettings() != settings)
    {
        m_vmeConfig->setVMEController(controller->getType(), settings);
    }

    m_d->m_ctrlOpenRetryCount = 0;

    connect(controller, &VMEController::controllerStateChanged,
            this, &MVMEContext::controllerStateChanged);
    connect(controller, &VMEController::controllerStateChanged,
            this, &MVMEContext::onControllerStateChanged);

    // readout worker (readout side)
    VMEControllerFactory factory(controller->getType());
    m_readoutWorker = factory.makeReadoutWorker();
    m_readoutWorker->moveToThread(m_readoutThread);
    connect(m_readoutWorker, &VMEReadoutWorker::stateChanged, this, &MVMEContext::onDAQStateChanged);
    connect(m_readoutWorker, &VMEReadoutWorker::daqStopped, this, &MVMEContext::onDAQDone);

    VMEReadoutWorkerContext readoutWorkerContext;

    readoutWorkerContext.controller         = controller;
    readoutWorkerContext.daqStats           = {};
    readoutWorkerContext.vmeConfig          = m_vmeConfig;
    readoutWorkerContext.freeBuffers        = &m_freeBuffers;
    readoutWorkerContext.fullBuffers        = &m_fullBuffers;
    readoutWorkerContext.listfileOutputInfo = &m_d->m_listfileOutputInfo;
    readoutWorkerContext.runInfo            = &m_d->m_runInfo;

    readoutWorkerContext.logger             = [this](const QString &msg) { logMessage(msg); };
    readoutWorkerContext.getLogBuffer       = [this]() { return getLogBuffer(); };
    readoutWorkerContext.getAnalysisJson    = [this]() { return getAnalysisJsonDocument(); };

    m_readoutWorker->setContext(readoutWorkerContext);

    // replay worker (running on the readout thread)
    m_d->listfileReplayWorker = std::unique_ptr<ListfileReplayWorker>(
        factory.makeReplayWorker(&m_freeBuffers, &m_fullBuffers));

    m_d->listfileReplayWorker->setLogger([this](const QString &msg) { this->logMessage(msg); });
    m_d->listfileReplayWorker->moveToThread(m_readoutThread);

    connect(m_d->listfileReplayWorker.get(), &ListfileReplayWorker::stateChanged,
            this, &MVMEContext::onDAQStateChanged);

    connect(m_d->listfileReplayWorker.get(), &ListfileReplayWorker::replayStopped,
            this, &MVMEContext::onReplayDone);

    // Create a stream worker (analysis side). The concrete type depends on the
    // VME controller type.
    //
    // Delete the streamWorker in the event loop. This will also delete the
    // EventServer child.
    if (m_streamWorker)
    {
        auto streamWorker = m_streamWorker.release();
        streamWorker->deleteLater(); // Will also delete the current EventServer instance
        m_d->m_eventServer = nullptr;
        processQtEvents();
    }

    switch (controller->getType())
    {
        case VMEControllerType::SIS3153:
        case VMEControllerType::VMUSB:
            m_streamWorker = std::make_unique<MVMEStreamWorker>(
                this, &m_freeBuffers, &m_fullBuffers);
            break;

        case VMEControllerType::MVLC_ETH:
        case VMEControllerType::MVLC_USB:
            m_streamWorker = std::make_unique<MVLC_StreamWorker>(
                this, &m_freeBuffers, &m_fullBuffers);
            break;
    }

    assert(m_streamWorker);


    // EventServer setup
    {
        m_d->m_eventServer = new EventServer(m_streamWorker.get());; // Note: this is a non-owning pointer
        m_streamWorker->attachModuleConsumer(m_d->m_eventServer);

        auto eventServer = m_d->m_eventServer;

        eventServer->setLogger([this](const QString &msg) { this->logMessage(msg); });

        auto settings = makeWorkspaceSettings();
        bool enabled = false;
        QHostInfo hostInfo;
        int port = 0;

        if (settings)
            std::tie(enabled, hostInfo, port) = get_event_server_listen_info(*settings);

        if (enabled && (hostInfo.error() || hostInfo.addresses().isEmpty()))
        {
            logMessage(QSL("EventServer error: could not resolve listening address ")
                       + hostInfo.hostName() + ": " + hostInfo.errorString());
        }
        else if (enabled && !hostInfo.addresses().isEmpty())
        {
            eventServer->setListeningInfo(hostInfo.addresses().first(), port);
        }

        bool invoked = QMetaObject::invokeMethod(m_d->m_eventServer,
                                                 "setEnabled",
                                                 Qt::QueuedConnection,
                                                 Q_ARG(bool, enabled));
        assert(invoked);
        (void) invoked;
    }

    m_streamWorker->moveToThread(m_analysisThread);

    // Run the StreamWorkerBase::startupConsumers in the worker thread. This is
    // used to e.g. get the EventServer to accept client connections while the
    // system is idle. Starting to accept connections at the point when a run
    // is starting does not work because clients would have no time window to
    // connect to the service.
    // TODO: add a way to wait for completion of the startup.
    {
        bool invoked = QMetaObject::invokeMethod(
            m_streamWorker.get(), "startupConsumers", Qt::QueuedConnection);
        assert(invoked);
    }

    connect(m_streamWorker.get(), &StreamWorkerBase::stateChanged,
            this, &MVMEContext::onMVMEStreamWorkerStateChanged);

    connect(m_streamWorker.get(), &StreamWorkerBase::sigLogMessage,
            this, &MVMEContext::logMessage);

    emit vmeControllerSet(controller);

    //qDebug() << __PRETTY_FUNCTION__ << "end";
    return true;
}

bool MVMEContext::setVMEController(VMEControllerType type, const QVariantMap &settings)
{
    VMEControllerFactory factory(type);

    auto controller = std::unique_ptr<VMEController>(factory.makeController(settings));

    if (setVMEController(controller.get(), settings))
    {
        controller.release();
        return true;
    }

    return false;
}

ControllerState MVMEContext::getControllerState() const
{
    auto result = ControllerState::Disconnected;
    if (m_controller)
        result = m_controller->getState();
    return result;
}

void MVMEContext::onControllerStateChanged(ControllerState state)
{
    qDebug() << __PRETTY_FUNCTION__ << to_string(state) << (u32) state;

    if (state == ControllerState::Connected)
    {
        m_d->m_ctrlOpenRetryCount = 0;
    }
}

// Slot invoked by QFutureWatcher::finished from the m_ctrlOpenWatcher
void MVMEContext::onControllerOpenFinished()
{
    auto result = m_ctrlOpenWatcher.result();

    //qDebug() << __PRETTY_FUNCTION__ << "result =" << result.toString();

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
        else if (auto sis = dynamic_cast<SIS3153 *>(m_controller))
        {
            u32 moduleIdAndFirmware;
            auto error = sis->readRegister(SIS3153Registers::ModuleIdAndFirmware,
                                           &moduleIdAndFirmware);

            if (!error.isError())
            {
                logMessage(QString("Opened VME controller %1 - Firmware 0x%2")
                           .arg(m_controller->getIdentifyingString())
                           .arg(moduleIdAndFirmware & 0xffff, 4, 16, QLatin1Char('0'))
                          );

                QSettings appSettings;
                appSettings.setValue("VME/LastConnectedSIS3153", sis->getAddress());
            }
            else
            {
                logMessage(QString("Error reading firmware from VME controller %1: %2")
                           .arg(m_controller->getIdentifyingString())
                           .arg(error.toString())
                          );
            }
        }
        else if (auto mvlc = dynamic_cast<mesytec::mvlc::MVLC_VMEController *>(m_controller))
        {
            using namespace mesytec::mvlc;

            logMessage(QString("Opened VME Controller %1 (%2)")
                       .arg(mvlc->getIdentifyingString())
                       .arg(mvlc->getMVLCObject()->getConnectionInfo()));
        }
        else // generic case
        {
            logMessage(QString("Opened VME controller %1")
                       .arg(m_controller->getIdentifyingString()));
        }

        return;
    }
    else if (result.error() != VMEError::DeviceIsOpen)
    {
        assert(result.isError());

        /* Could not connect. Inc retry count and log a hopefully user
         * friendly message about what went wrong. */

        m_d->m_ctrlOpenRetryCount++;

        // The result indicates an error. First handle the MVLCs "InUse" case.
        if (result.getStdErrorCode() == mesytec::mvlc::MVLCErrorCode::InUse)
        {
            if (m_d->m_ctrlOpenRetryCount >= VMECtrlConnectMaxRetryCount)
            {
                //if (!m_d->m_isFirstConnectionAttempt)
                {
                    auto msg = QSL(
                        "The MVLC controller seems to be in use (at least one of the"
                        " triggers is enabled).\n"
                        "Use the \"Force Reset\" button to disable the triggers on the next"
                        " connection attempt.");

                    logMessage(msg);
                }
            }
        }

        if (m_d->m_ctrlOpenRetryCount >= VMECtrlConnectMaxRetryCount)
        {

            //if (!m_d->m_isFirstConnectionAttempt)
            {
                logMessage(QString("Could not open VME controller %1: %2")
                           .arg(m_controller->getIdentifyingString())
                           .arg(result.toString())
                          );
            }
            m_d->m_isFirstConnectionAttempt = false;
        }
    }
}

void MVMEContext::reconnectVMEController()
{
    if (!m_controller)
    {
        return;
    }

    /* VMEController::close() should lock the controllers mutex just as
     * VMEController::open() should. This means if a long lasting open()
     * operation is in progress (e.g. due to DNS lookup) the call will block.
     * Solution: wait for any open() calls to finish, then call close on the
     * controller. */

    qDebug() << __PRETTY_FUNCTION__ << "before m_controller->close()";
    if (m_ctrlOpenFuture.isRunning())
    {
        QProgressDialog progressDialog("Reconnecting to VME Controller", QString(), 0, 0);
        progressDialog.setWindowModality(Qt::ApplicationModal);
        progressDialog.setCancelButton(nullptr);
        progressDialog.show();

        QEventLoop localLoop;
        connect(&m_ctrlOpenWatcher, &QFutureWatcher<VMEError>::finished,
                &localLoop, &QEventLoop::quit);
        localLoop.exec();
    }

    m_controller->close();
    m_d->m_ctrlOpenRetryCount = 0;
    m_d->m_isFirstConnectionAttempt = true; // FIXME: add a note on why this is done

    qDebug() << __PRETTY_FUNCTION__ << "after m_controller->close()";
}

void MVMEContext::forceResetVMEController()
{
    if (auto sis = qobject_cast<SIS3153 *>(getVMEController()))
    {
        sis->setResetOnConnect(true);
    }
    else if (auto mvlc = qobject_cast<mesytec::mvlc::MVLC_VMEController *>(getVMEController()))
    {
        if (mvlc->connectionType() == mesytec::mvlc::ConnectionType::ETH)
        {
            auto mvlc_eth = reinterpret_cast<mesytec::mvlc::eth::Impl *>(mvlc->getImpl());
            mvlc_eth->setDisableTriggersOnConnect(true);
        }
    }

    reconnectVMEController();
}

void MVMEContext::dumpVMEControllerRegisters()
{
    // XXX: only VMUSB right now
    if (auto vmusb = qobject_cast<VMUSB *>(getVMEController()))
    {
        if (getDAQState() == DAQState::Idle)
        {
            dump_registers(vmusb, [this] (const QString &line)
                           {
                               logMessage(line);
                           });
        }
    }
    else
    {
        logMessage("'Dump Registers' not implemented for VME controller type " +
                   to_string(getVMEController()->getType()));
    }
}

QString MVMEContext::getUniqueModuleName(const QString &prefix) const
{
    return make_unique_module_name(prefix, m_vmeConfig);
}

void MVMEContext::tryOpenController()
{
    // Mutex to avoid entering and possibly starting the opening code multiple
    // times.
    std::unique_lock<mesytec::mvme::TicketMutex> guard(m_d->tryOpenControllerMutex);

    if (m_controller
        && !m_ctrlOpenFuture.isRunning()
        && m_d->m_ctrlOpenRetryCount < VMECtrlConnectMaxRetryCount)
    {
        auto try_open = [] (VMEController *ctrl)
        {
            if (!ctrl->isOpen())
            {
                //qDebug() << "tryOpenController" << QThread::currentThread() << "calling open()";
                return ctrl->open();
            }

            //qDebug() << "tryOpenController" << QThread::currentThread() << "ctrl is open, returning";
            return VMEError(VMEError::DeviceIsOpen);
        };

        m_ctrlOpenFuture = QtConcurrent::run(try_open, m_controller);
        m_ctrlOpenWatcher.setFuture(m_ctrlOpenFuture);
    }
}

void MVMEContext::logModuleCounters()
{
#if 0 // TODO: rebuild this once stats tracks these numbers again

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

void MVMEContext::onMVMEStreamWorkerStateChanged(MVMEStreamWorkerState state)
{
    emit mvmeStreamWorkerStateChanged(state);

    switch (state)
    {
        case MVMEStreamWorkerState::Idle:
            break;

        default:
            break;
    }
}

// Called on VMUSBReadoutWorker::daqStopped()
void MVMEContext::onDAQDone()
{
    // stops the analysis side thread
    m_streamWorker->stop();

    // The readout worker might have modified the ListFileOutputInfo structure.
    // Write it out to the workspace.
    qDebug() << __PRETTY_FUNCTION__ << "writing listfile output info to workspace";
    writeToSettings(m_d->m_listfileOutputInfo, *makeWorkspaceSettings());
}

// Called on ListfileReplayWorker::replayStopped()
void MVMEContext::onReplayDone()
{
    // Tell the analysis side to stop once the filled buffer queue is empty.
    m_streamWorker->stop();

    auto stats = getDAQStats();

    qDebug() << __PRETTY_FUNCTION__
        << stats.startTime
        << stats.endTime
        << stats.totalBuffersRead
        << stats.totalBytesRead;

    double secondsElapsed = m_replayTime.elapsed() / 1000.0;
    u64 replayBytes = getDAQStats().totalBytesRead;
    double replayMB = (double)replayBytes / (1024.0 * 1024.0);
    double mbPerSecond = 0.0;
    if (secondsElapsed > 0)
    {
        mbPerSecond = replayMB / secondsElapsed;
    }

    QString msg = QString("Replay finished: Read %1 MB in %2 s, %3 MB/s\n")
        .arg(replayMB)
        .arg(secondsElapsed)
        .arg(mbPerSecond)
        ;

    logMessage(msg);
    qDebug().noquote() << msg;
}

DAQState MVMEContext::getDAQState() const
{
    return m_daqState;
}

MVMEStreamWorkerState MVMEContext::getMVMEStreamWorkerState() const
{
    if (m_streamWorker)
        return m_streamWorker->getState();

    return MVMEStreamWorkerState::Idle;
}

DAQStats MVMEContext::getDAQStats() const
{
    switch (getMode())
    {
        case GlobalMode::DAQ:
            return m_readoutWorker->getDAQStats();

        case GlobalMode::ListFile:
            return m_d->listfileReplayWorker->getStats();
    }

    return {};
}

static bool handle_vme_analysis_assignment(
    const VMEConfig *vmeConfig,
    analysis::Analysis *analysis,
    analysis::ui::AnalysisWidget *analysisUi = nullptr,
    QWidget *assignmentUiParent = nullptr)
{
    using namespace analysis;
    using namespace vme_analysis_common;

    if (!auto_assign_vme_modules(vmeConfig, analysis))
    {
        if (!run_vme_analysis_module_assignment_ui(vmeConfig, analysis, assignmentUiParent))
            return false;
    }

    add_vme_properties_to_analysis(vmeConfig, analysis);

    // This is deliberately commented out. It would remove all non-assigned
    // objects from the analysis and thus potentially free up memory (mostly
    // from histograms).
    // Instead of doing this the objects are kept now and the analysis is not
    // modified. This was done to not confuse users with analysis setups being
    // magially set to 'modified' all the time.
    //remove_analysis_objects_unless_matching(analysis, vmeConfig);

    if (analysisUi)
        analysisUi->repopulate();

    return true;
}

bool MVMEContext::setReplayFileHandle(ListfileReplayHandle handle)
{
    using namespace vme_analysis_common;

    if (!handle.listfile)
        return false;

    if (getDAQState() != DAQState::Idle)
        stopDAQ();

    std::unique_ptr<VMEConfig> vmeConfig;
    std::error_code ec;

    std::tie(vmeConfig, ec) = read_vme_config_from_listfile(handle);

    if (ec)
    {
        QMessageBox::critical(nullptr,
                              QSL("Error loading VME config"),
                              QSL("Error loading VME config from %1: %2")
                              .arg(handle.inputFilename)
                              .arg(ec.message().c_str()));
        return false;
    }

    m_d->m_isFirstConnectionAttempt = true; // FIXME: add a note on why this is done
    setVMEConfig(vmeConfig.release());

    m_d->listfileReplayHandle = std::move(handle);
    m_d->listfileReplayWorker->setListfile(m_d->listfileReplayHandle.listfile.get());

    setConfigFileName(QString(), false);
    setMode(GlobalMode::ListFile);

    bool ret = handle_vme_analysis_assignment(
        getVMEConfig(), getAnalysis(),
        getAnalysisUi(), getMainWindow());

    return ret;
}

const ListfileReplayHandle &MVMEContext::getReplayFileHandle() const
{
    return m_d->listfileReplayHandle;
}

ListfileReplayHandle &MVMEContext::getReplayFileHandle()
{
    return m_d->listfileReplayHandle;
}

void MVMEContext::closeReplayFileHandle()
{
    if (getMode() != GlobalMode::ListFile)
        return;

    stopDAQ();

    m_d->listfileReplayHandle = {};
    m_d->m_isFirstConnectionAttempt = true;

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
            makeWorkspaceSettings()->setValue(
                QSL("LastVMEConfig"), name.remove(getWorkspaceDirectory() + '/'));
        }
        emit vmeConfigFilenameChanged(name);
    }
}

void MVMEContext::setAnalysisConfigFileName(QString name, bool updateWorkspace)
{
    if (m_analysisConfigFileName != name)
    {
        m_analysisConfigFileName = name;
        if (updateWorkspace)
        {
            makeWorkspaceSettings()->setValue(
                QSL("LastAnalysisConfig"), name.remove(getWorkspaceDirectory() + '/'));
        }
        emit analysisConfigFileNameChanged(name);
    }
}

// Rebuilds the analyis in the main thread and then tells the stream worker that
// a run is about to start.
bool MVMEContext::prepareStart()
{
#ifndef NDEBUG
    // Use this to force a crash in case deleted objects remain in the object set.
    for (auto it=m_objects.begin(); it!=m_objects.end(); ++it)
    {
        qDebug() << reinterpret_cast<void *>(*it);
        qDebug() << *it;
    }
#endif

    // add info from the workspace to the current RunInfo object
    auto wsSettings = makeWorkspaceSettings();
    auto experimentName = wsSettings->value("Experiment/Name").toString();
    if (experimentName.isEmpty())
        experimentName = "Experiment";
    m_d->m_runInfo.infoDict["ExperimentName"] = experimentName;
    m_d->m_runInfo.infoDict["ExperimentTitle"] = wsSettings->value("Experiment/Title");
    m_d->m_runInfo.infoDict["MVMEWorkspace"] = getWorkspaceDirectory();
    m_d->m_runInfo.ignoreStartupErrors = wsSettings->value(
        "Experiment/IgnoreVMEStartupErrors").toBool();

    qDebug() << __PRETTY_FUNCTION__
        << "free buffers:" << queue_size(&m_freeBuffers)
        << "filled buffers:" << queue_size(&m_fullBuffers);

    // Discard any filled buffers from a previous run, moving them back to the
    // free queue. This way the analysis side won't get stale data in case it
    // previously quit without consuming all enqueued buffers.
    while (auto buffer = dequeue(&m_fullBuffers))
        enqueue(&m_freeBuffers, buffer);

    assert(queue_size(&m_freeBuffers) == DataBufferCount);
    assert(queue_size(&m_fullBuffers) == 0);

    assert(m_streamWorker->getState() == MVMEStreamWorkerState::Idle);

    if (m_streamWorker->getState() == MVMEStreamWorkerState::Idle)
    {
        qDebug() << __PRETTY_FUNCTION__ << "building analysis in main thread";

        auto indexMapping = vme_analysis_common::build_id_to_index_mapping(getVMEConfig());
        auto analysis = getAnalysis();
        analysis->beginRun(getRunInfo(), indexMapping,
                           [this] (const QString &msg) { logMessage(msg); });

        qDebug() << __PRETTY_FUNCTION__ << "starting mvme stream worker";

        // Use a local event loop to wait here until the stream worker thread
        // signals that startup is complete.
        QEventLoop localLoop;
        auto con_started = QObject::connect(
            m_streamWorker.get(), &MVMEStreamWorker::started,
            &localLoop, &QEventLoop::quit);

        auto con_stopped = QObject::connect(
            m_streamWorker.get(), &MVMEStreamWorker::stopped,
            &localLoop, &QEventLoop::quit);

        bool invoked = QMetaObject::invokeMethod(m_streamWorker.get(), "start",
                                                 Qt::QueuedConnection);
        assert(invoked);

        localLoop.exec();

        QObject::disconnect(con_started);
        QObject::disconnect(con_stopped);

        auto workerState = m_streamWorker->getState();

        if (workerState == MVMEStreamWorkerState::Running
            || workerState == MVMEStreamWorkerState::Paused)
        {
            qDebug() << __PRETTY_FUNCTION__ << "started mvme stream worker";
            return true;
        }
        else
        {
            qDebug() << __PRETTY_FUNCTION__ << "mvme stream worker did not start up correctly";
            return false;
        }
    }

    return false;
}

void MVMEContext::startDAQReadout(quint32 nCycles, bool keepHistoContents)
{
    Q_ASSERT(getDAQState() == DAQState::Idle);
    Q_ASSERT(getMVMEStreamWorkerState() == MVMEStreamWorkerState::Idle);

    if (m_mode != GlobalMode::DAQ
        || getDAQState() != DAQState::Idle
        || getMVMEStreamWorkerState() != MVMEStreamWorkerState::Idle)
    {
        return;
    }

    // Can be used for last minute changes before the actual startup. Used in
    // the GUI to ask the user if modifications to VME scripts should be applied
    // and used for the run.
    emit daqAboutToStart();

    // Generate new RunInfo here. Has to happen before prepareStart() calls
    // MVMEStreamWorker::beginRun()
    // FIXME: does this still do anything? runId is still used in histo1d_widget?
    // FIXME: the output filename generation (based on run settings, etc) must
    // be done before filling this runinfo. Meaning move the output file
    // creation and error handling from the readout worker into this thread and
    // pass the open output file descriptor the readout worker instead.
    auto now = QDateTime::currentDateTime();
    m_d->m_runInfo.runId = now.toString("yyMMdd_HHmmss");
    m_d->m_runInfo.keepAnalysisState = keepHistoContents;
    m_d->m_runInfo.isReplay = false;

    // TODO: use ListFileOutputInfo to generate the runId for the analysis
    // FIXME: we don't actually know the runId before the listfile is successfully opened.
    // E.g. the runNumber might be incremented due to run files already existing.

    m_d->clearLog();
    logMessage(QSL("DAQ starting"));

    // Log mvme version and bitness and runtime cpu architecture
    logMessage(QString(QSL("mvme %1 (%2) running on %3 (%4)\n"))
               .arg(GIT_VERSION)
               .arg(get_bitness_string())
               .arg(QSysInfo::prettyProductName())
               .arg(QSysInfo::currentCpuArchitecture()));

    if (!prepareStart())
    {
        logMessage("Failed to start stream worker (analysis side). Aborting startup");
        return;
    }

    qDebug() << __PRETTY_FUNCTION__ << "starting readout worker";

    bool invoked = QMetaObject::invokeMethod(
        m_readoutWorker, "start", Qt::QueuedConnection,
        Q_ARG(quint32, nCycles));
    assert(invoked);
}

void MVMEContext::stopDAQ()
{
    m_d->stopDAQ();
}

void MVMEContext::pauseDAQ()
{
    m_d->pauseDAQ();
}

void MVMEContext::resumeDAQ(u32 nCycles)
{
    m_d->resumeDAQ(nCycles);
}

void MVMEContext::startDAQReplay(quint32 nEvents, bool keepHistoContents)
{
    Q_ASSERT(getDAQState() == DAQState::Idle);
    Q_ASSERT(getMVMEStreamWorkerState() == MVMEStreamWorkerState::Idle);

    if (m_mode != GlobalMode::ListFile || !m_d->listfileReplayHandle.listfile
        || getDAQState() != DAQState::Idle
        || getMVMEStreamWorkerState() != MVMEStreamWorkerState::Idle)
    {
        return;
    }

    // Extract a runId from the listfile filename.
    QFileInfo fi(m_d->listfileReplayHandle.inputFilename);
    m_d->m_runInfo.runId = fi.completeBaseName();
    m_d->m_runInfo.keepAnalysisState = keepHistoContents;
    m_d->m_runInfo.isReplay = true;
    m_d->m_runInfo.infoDict["replaySourceFile"] = fi.fileName();

    m_d->listfileReplayWorker->setEventsToRead(nEvents);

    if (auto mvmeStreamWorker = qobject_cast<MVMEStreamWorker *>(m_streamWorker.get()))
    {
        ListFile lf(m_d->listfileReplayHandle.listfile.get());
        lf.open();
        mvmeStreamWorker->setListFileVersion(lf.getFileVersion());
    }

    if (!prepareStart())
    {
        logMessage("Failed to start stream worker (analysis side). Aborting startup");
        return;
    }

    qDebug() << __PRETTY_FUNCTION__ << "starting listfile reader";

    bool invoked = QMetaObject::invokeMethod(
        m_d->listfileReplayWorker.get(), "start", Qt::QueuedConnection);
    assert(invoked);

    m_replayTime.restart();
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

    if (m_d->m_logBuffer.size() >= LogBufferMaxEntries)
        m_d->m_logBuffer.pop_front();

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

void MVMEContext::logError(const QString &msg)
{
    QString fullMessage(QString("%1: %2")
             .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
             .arg(msg));

    {
        QMutexLocker lock(&m_d->m_logBufferMutex);

        if (m_d->m_logBuffer.size() >= LogBufferMaxEntries)
            m_d->m_logBuffer.pop_front();

        m_d->m_logBuffer.append(msg);
    }

    emit sigLogError(fullMessage);
}

QStringList MVMEContext::getLogBuffer() const
{
    QMutexLocker lock(&m_d->m_logBufferMutex);
    return m_d->m_logBuffer;
}

void MVMEContext::onEventAdded(EventConfig *event)
{
    emit eventAdded(event);
    for (auto module: event->getModuleConfigs())
        onModuleAdded(module);

    connect(event, &EventConfig::moduleAdded, this, &MVMEContext::onModuleAdded);
    connect(event, &EventConfig::moduleAboutToBeRemoved, this, &MVMEContext::onModuleAboutToBeRemoved);
}

void MVMEContext::onEventAboutToBeRemoved(EventConfig *config)
{
    for (auto module: config->getModuleConfigs())
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

vme_script::ResultList
MVMEContext::runScript(const vme_script::VMEScript &script,
                       vme_script::LoggerFun logger,
                       bool logEachResult)
{
    vme_script::ResultList results;

    if (!m_controller->isOpen())
    {
        logger("VME Script Error: VME controller not connected");
        return { vme_script::Result{VMEError::NotOpen} };
    }

    // The MVLC can execute commands while the DAQ is running, other controllers
    // cannot so the DAQ has to be paused and resumed if needed.
    if (is_mvlc_controller(m_controller->getType()))
    {
        auto mvlc = qobject_cast<mesytec::mvlc::MVLC_VMEController *>(m_controller);
        assert(mvlc);

        mvlc->disableNotificationPolling();

        // The below code should be equivalent to
        // results = vme_script::run_script(m_controller, script, logger, logEachResult);
        // but moves the run_script call into a different thread and shows a
        // progress dialog during execution.
        //
        // IMPORTANT: if logEachResult is true, then the logger will be invoked
        // from within the thread chosen by QtConcurrent::run(). This means
        // direct modification of GUI elements from within the logger code is
        // not allowed and will most likely lead to a crash!

        using Watcher = QFutureWatcher<vme_script::ResultList>;

        QProgressDialog pd;
        pd.setLabelText("VME Script execution in progress");
        pd.setMaximum(0);
        pd.setCancelButton(nullptr);

        Watcher fw;

        connect(&fw, &Watcher::finished, &pd, &QProgressDialog::accept);

        auto f = QtConcurrent::run(
            [=] () -> vme_script::ResultList {
                auto result = vme_script::run_script(m_controller, script, logger, logEachResult);
                return result;
            });

        // Note: this call can lead to immediate signal emission from the
        // watcher.
        fw.setFuture(f);

        // No qt signal processing can be done between f.isFinished() and
        // pd.exec() so this should be fine. Either the future is finished and
        // we never enter the dialog event loop or we do enter the loop and
        // will get a finished signal from the Watcher.
        if (!f.isFinished())
            pd.exec();

        results = f.result();

        mvlc->enableNotificationPolling();
    }
    else
    {
        DAQPauser pauser(this);

        // TODO: move this into a separate thread like in the MVLC case above.
        results = vme_script::run_script(m_controller, script, logger, logEachResult);
    }

    // Check for errors indicating connection loss and call close() on the VME
    // controller to update its status.
    static const unsigned TimeoutConnectionLossThreshold = 3;
    unsigned timeouts = 0;

    for (const auto &result: results)
    {
#if 0
        qDebug() << __PRETTY_FUNCTION__ << result.error.isError()
            << result.error.error()
            << result.error.getStdErrorCode().value()
            << result.error.getStdErrorCode().category().name()
            << (result.error.getStdErrorCode() == mesytec::mvlc::ErrorType::ConnectionError);
#endif

        if (result.error.isError())
        {
            if (result.error.error() == VMEError::NotOpen ||
                result.error.getStdErrorCode() == mesytec::mvlc::ErrorType::ConnectionError)
            {
                emit logMessage("ConnectionError during VME script execution,"
                                " closing connection to VME Controller");
                m_controller->close();
                break;
            }
        }
    }

    return results;
}

//
// Workspace handling
//

void make_empty_file(const QString &filePath)
{
    QFile file(filePath);

    if (file.exists())
        throw QString(QSL("File exists"));

    if (!file.open(QIODevice::WriteOnly))
        throw file.errorString();
}

void MVMEContext::newWorkspace(const QString &dirName)
{
    QDir destDir(dirName);

    // If the INI file exists assume this is a proper workspace and open it.
    if (destDir.exists(WorkspaceIniName))
    {
        openWorkspace(dirName);
        return;
    }

    // cleanup autosaves in the previous workspace
    cleanupWorkspaceAutoSaveFiles();

    auto workspaceSettings(makeWorkspaceSettings(dirName));
    workspaceSettings->setValue(QSL("LastVMEConfig"), DefaultVMEConfigFileName);
    workspaceSettings->setValue(QSL("LastAnalysisConfig"), DefaultAnalysisConfigFileName);
    workspaceSettings->setValue(QSL("WriteListFile"), true);

    workspaceSettings->setValue(QSL("Experiment/Name"), "Experiment");
    workspaceSettings->setValue(QSL("Experiment/Title"), QString());

    workspaceSettings->setValue(QSL("JSON-RPC/Enabled"), false);
    workspaceSettings->setValue(QSL("JSON-RPC/ListenAddress"), QString());
    workspaceSettings->setValue(QSL("JSON-RPC/ListenPort"), JSON_RPC_DefaultListenPort);

    workspaceSettings->setValue(QSL("EventServer/Enabled"), false);
    workspaceSettings->setValue(QSL("EventServer/ListenAddress"), QString());
    workspaceSettings->setValue(QSL("EventServer/ListenPort"), EventServer_DefaultListenPort);


    // Force sync to create the mvmeworkspace.ini file
    workspaceSettings->sync();

    if (workspaceSettings->status() != QSettings::NoError)
    {
        throw QString("Error writing workspace settings to %1")
            .arg(workspaceSettings->fileName());
    }

    if (!destDir.exists(DefaultVMEConfigFileName))
    {
        try
        {
            make_empty_file(QDir(dirName).filePath(DefaultVMEConfigFileName));
        }
        catch (const QString &e)
        {
            throw QString("Error creating VME config file %1: %2")
                .arg(DefaultVMEConfigFileName)
                .arg(e);
        }
    }

    if (!destDir.exists(DefaultAnalysisConfigFileName))
    {
        try
        {
            make_empty_file(QDir(dirName).filePath(DefaultAnalysisConfigFileName));
        }
        catch (const QString &e)
        {
            throw QString("Error creating Analysis config file %1: %2")
                .arg(DefaultAnalysisConfigFileName)
                .arg(e);
        }
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
        // cleanup files in the previous workspace that's being closed
        cleanupWorkspaceAutoSaveFiles();

        setWorkspaceDirectory(dirName);
        auto workspaceSettings(makeWorkspaceSettings(dirName));

        // settings defaults
        if (!workspaceSettings->contains(QSL("JSON-RPC/Enabled")))
            workspaceSettings->setValue(QSL("JSON-RPC/Enabled"), false);
        if (!workspaceSettings->contains(QSL("JSON-RPC/ListenAddress")))
            workspaceSettings->setValue(QSL("JSON-RPC/ListenAddress"), QString());
        if (!workspaceSettings->contains(QSL("JSON-RPC/ListenPort")))
            workspaceSettings->setValue(QSL("JSON-RPC/ListenPort"), JSON_RPC_DefaultListenPort);

        if (!workspaceSettings->contains(QSL("EventServer/Enabled")))
            workspaceSettings->setValue(QSL("EventServer/Enabled"), false);
        if (!workspaceSettings->contains(QSL("EventServer/ListenAddress")))
            workspaceSettings->setValue(QSL("EventServer/ListenAddress"), QString());
        if (!workspaceSettings->contains(QSL("EventServer/ListenPort")))
            workspaceSettings->setValue(QSL("EventServer/ListenPort"), EventServer_DefaultListenPort);

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

        // sessions subdir
        {
            QDir dir(getWorkspacePath(QSL("SessionDirectory"), QSL("sessions")));

            if (!QDir::root().mkpath(dir.absolutePath()))
            {
                throw QString(QSL("Error creating sessions directory %1.")).arg(dir.path());
            }
        }

        // exports subdir
        {
            QDir dir(getWorkspacePath(QSL("ExportsDirectory"), QSL("exports")));

            if (!QDir::root().mkpath(dir.absolutePath()))
            {
                throw QString(QSL("Error creating exports directory %1.")).arg(dir.path());
            }
        }

        // special listfile output directory handling. TODO: this might not
        // actually be needed anymore
        {
            ListFileOutputInfo info = readFromSettings(*workspaceSettings);


            QDir listFileOutputDir(info.directory);

            if (listFileOutputDir.isAbsolute() && !listFileOutputDir.exists())
            {
                /* A non-existant absolute path was loaded from the INI -> go back
                 * to the default of "listfiles". */
                logMessage(QString("Warning: Stored Listfile directory %1 does not exist. "
                                   "Reverting back to default of \"listfiles\".")
                           .arg(info.directory));
                info.directory = QSL("listfiles");

                // create the default listfile directory if it does not exist
                listFileOutputDir = QDir(info.directory);
                if (!listFileOutputDir.exists())
                {
                    if (!QDir::root().mkpath(dir.absolutePath()))
                    {
                        throw QString(QSL("Error creating listfiles directory %1.")).arg(dir.path());
                    }
                }
            }

            info.fullDirectory = getListFileOutputDirectoryFullPath(info.directory);
            m_d->m_listfileOutputInfo = info;
            writeToSettings(info, *workspaceSettings);
        }

        //
        // VME config
        //
        auto lastVMEConfig = workspaceSettings->value(QSL("LastVMEConfig")).toString();

        if (dir.exists(VMEConfigAutoSaveFilename))
        {
            qDebug() << __PRETTY_FUNCTION__ << "found VMEConfig autosave";

            auto tLast = QFileInfo(lastVMEConfig).lastModified();
            auto tAuto = QFileInfo(dir.filePath(VMEConfigAutoSaveFilename)).lastModified();

            if (tLast < tAuto)
            {
                QMessageBox mb(
                    QMessageBox::Question, QSL("VME autosave file found"),
                    QSL("A VME config autosave file from a previous mvme session"
                        " was found in %1.<br>"
                        "Do you want to open the autosave?")
                    .arg(dirName),
                    QMessageBox::Open | QMessageBox::Cancel);

                mb.button(QMessageBox::Cancel)->setText(QSL("Ignore"));

                int choice = mb.exec();

                switch (choice)
                {
                    case QMessageBox::Open:
                        loadVMEConfig(dir.filePath(VMEConfigAutoSaveFilename));
                        getVMEConfig()->setModified(true);
                        setConfigFileName(lastVMEConfig);
                        break;

                    case QMessageBox::Cancel:
                        loadVMEConfig(dir.filePath(lastVMEConfig));
                        break;

                    InvalidDefaultCase;
                }
            }
        }
        else
        {
            // Load the last used vme config
            if (!lastVMEConfig.isEmpty())
            {
                qDebug() << __PRETTY_FUNCTION__ << "loading vme config" << lastVMEConfig
                    << " (INI)";

                loadVMEConfig(dir.filePath(lastVMEConfig));
            }
            // Check if a file with the default name exists and if so load it.
            else if (QFile::exists(dir.filePath(DefaultVMEConfigFileName)))
            {
                qDebug() << __PRETTY_FUNCTION__ << "loading vme config" << lastVMEConfig
                    << " (DefaultName)";

                loadVMEConfig(dir.filePath(DefaultVMEConfigFileName));
            }
            // Neither last nor default files exist => create empty default
            else
            {
                qDebug() << __PRETTY_FUNCTION__ << "setting default vme filename";
                // No previous filename is known so use a default name without updating
                // the workspace settings.
                setConfigFileName(DefaultVMEConfigFileName, false);
            }
        }

        //
        // Analysis config
        //
        auto lastAnalysisConfig = workspaceSettings->value(QSL("LastAnalysisConfig")).toString();

        if (dir.exists(AnalysisAutoSaveFilename))
        {
            qDebug() << __PRETTY_FUNCTION__ << "found Analysis autosave";

            auto tLast = QFileInfo(lastAnalysisConfig).lastModified();
            auto tAuto = QFileInfo(dir.filePath(AnalysisAutoSaveFilename)).lastModified();


            if (tLast < tAuto)
            {
                QMessageBox mb(
                    QMessageBox::Question, QSL("Analysis autosave file found"),
                    QSL("An Analysis autosave file from a previous mvme session"
                        " was found in %1.<br>"
                        "Do you want to open the autosave?")
                    .arg(dirName),
                    QMessageBox::Open | QMessageBox::Cancel);

                mb.button(QMessageBox::Cancel)->setText(QSL("Ignore"));

                int choice = mb.exec();

                switch (choice)
                {
                    case QMessageBox::Open:
                        loadAnalysisConfig(dir.filePath(AnalysisAutoSaveFilename));
                        getAnalysis()->setModified(true);
                        setAnalysisConfigFileName(lastAnalysisConfig);
                        break;

                    case QMessageBox::Cancel:
                        loadAnalysisConfig(dir.filePath(lastAnalysisConfig));
                        break;

                    InvalidDefaultCase;
                }
            }
        }
        else
        {
            if (!lastAnalysisConfig.isEmpty())
            {
                qDebug() << __PRETTY_FUNCTION__ << "loading analysis config" <<
                    lastAnalysisConfig << " (INI)";

                bool couldLoad = loadAnalysisConfig(dir.filePath(lastAnalysisConfig));

                if (!couldLoad)
                {
                    setAnalysisConfigFileName(DefaultAnalysisConfigFileName);
                    getAnalysis()->setModified();
                }
            }
            else if (QFile::exists(dir.filePath(DefaultAnalysisConfigFileName)))
            {
                qDebug() << __PRETTY_FUNCTION__ << "loading analysis config" <<
                    lastAnalysisConfig << " (DefaultName)";
            }
            else
            {
                qDebug() << __PRETTY_FUNCTION__ << "setting default analysis filename";
                setAnalysisConfigFileName(DefaultAnalysisConfigFileName, false);
            }

            // No exceptions thrown -> store workspace directory in global settings
            QSettings settings;
            settings.setValue(QSL("LastWorkspaceDirectory"), getWorkspaceDirectory());

            //
            // Load analysis session auto save
            //

            /* Try to load an analysis session auto save. Only loads analysis data, not
             * the analysis itself from the file.
             * Does not have an effect if there's a mismatch between the current analysis
             * and the one stored in the session as operator ids will be different so no
             * data will be loaded.
             * NOTE: the session auto save is done at the end of
             * MVMEStreamWorker::start().
             */
            auto sessionPath = getWorkspacePath(QSL("SessionDirectory"));
            QFileInfo fi(sessionPath + "/last_session" + analysis::SessionFileExtension);

            if (fi.exists())
            {
                //logMessage(QString("Loading analysis session auto save %1").arg(fi.filePath()));
                load_analysis_session(fi.filePath(), getAnalysis());
            }
        }

        //
        // Create the autosavers here as the workspace specific autosave
        // directory is known at this point.
        //

        // vme
        m_d->m_vmeConfigAutoSaver = std::make_unique<FileAutoSaver>(
            VMEConfigSerializer(this),
            dir.filePath(VMEConfigAutoSaveFilename),
            DefaultConfigFileAutosaveInterval_ms);

        m_d->m_vmeConfigAutoSaver->setObjectName(QSL("VmeConfigAutoSaver"));
        m_d->m_vmeConfigAutoSaver->start();

        connect(m_d->m_vmeConfigAutoSaver.get(), &FileAutoSaver::writeError,
                this, [this] (const QString &filename, const QString &errorMessage) {
            logMessage(errorMessage);
        });

        // analysis
        m_d->m_analysisAutoSaver = std::make_unique<FileAutoSaver>(
            AnalysisSerializer(this),
            dir.filePath(AnalysisAutoSaveFilename),
            DefaultConfigFileAutosaveInterval_ms);

        m_d->m_analysisAutoSaver->setObjectName(QSL("AnalysisAutoSaver"));
        m_d->m_analysisAutoSaver->start();

        connect(m_d->m_analysisAutoSaver.get(), &FileAutoSaver::writeError,
                this, [this] (const QString &filename, const QString &errorMessage) {
            logMessage(errorMessage);
        });

        reapplyWorkspaceSettings();
    }
    catch (const QString &)
    {
        // Restore previous workspace directory as the load was not successfull
        setWorkspaceDirectory(lastWorkspaceDirectory);
        throw;
    }
}

void MVMEContext::cleanupWorkspaceAutoSaveFiles()
{
    if (isWorkspaceOpen())
    {
        qDebug() << __PRETTY_FUNCTION__ << "removing autosaves";

        QDir wsDir(getWorkspaceDirectory());
        QFile::remove(wsDir.filePath(VMEConfigAutoSaveFilename));
        QFile::remove(wsDir.filePath(AnalysisAutoSaveFilename));
    }
    else
    {
        qDebug() << __PRETTY_FUNCTION__ << "no workspace open, nothing to do";
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
    return make_workspace_settings(workspaceDirectory);
}

QString MVMEContext::getWorkspacePath(const QString &settingsKey,
                                      const QString &defaultValue,
                                      bool setIfDefaulted) const
{
    auto settings = makeWorkspaceSettings();

    if (!settings)
    {
        return QSL("");
    }

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

void MVMEContext::reapplyWorkspaceSettings()
{
    qDebug() << __PRETTY_FUNCTION__;

    auto settings = makeWorkspaceSettings();

    m_d->m_remoteControl->stop();

    if (settings->value(QSL("JSON-RPC/Enabled")).toBool())
    {
        m_d->m_remoteControl->setListenAddress(
            settings->value(QSL("JSON-RPC/ListenAddress")).toString());

        m_d->m_remoteControl->setListenPort(
            settings->value(QSL("JSON-RPC/ListenPort")).toInt());

        m_d->m_remoteControl->start();
    }

    // EventServer
    {
        bool enabled;
        QHostInfo hostInfo;
        int port;

        std::tie(enabled, hostInfo, port) = get_event_server_listen_info(*settings);

        if (enabled && (hostInfo.error() || hostInfo.addresses().isEmpty()))
        {
            logMessage(QSL("EventServer error: could not resolve listening address ")
                       + hostInfo.hostName() + ": " + hostInfo.errorString());
        }
        else if (enabled && !hostInfo.addresses().isEmpty())
        {
            m_d->m_eventServer->setListeningInfo(hostInfo.addresses().first(), port);
        }

        bool invoked = QMetaObject::invokeMethod(m_d->m_eventServer,
                                                 "setEnabled",
                                                 Qt::QueuedConnection,
                                                 Q_ARG(bool, enabled));
        assert(invoked);
        (void) invoked;
    }
}

void MVMEContext::loadVMEConfig(const QString &fileName)
{
    QJsonDocument doc(gui_read_json_file(fileName));

    auto jsonRoot = doc.object();

    if (!doc.isNull() && !jsonRoot.contains("DAQConfig"))
    {
        QMessageBox::critical(
            nullptr,
            QSL("Error loading VME config"),
            QSL("The file '%1' does not contain an mvme VMEConfig object.")
            .arg(fileName)
            );

        return;
    }

    auto vmeConfig = std::make_unique<VMEConfig>();
    if (auto ec = vmeConfig->readVMEConfig(doc.object()["DAQConfig"].toObject()))
    {
        QMessageBox::critical(nullptr,
                              QSL("Error loading VME config"),
                              QSL("Error loading VME config from file %1: %2")
                              .arg(fileName)
                              .arg(ec.message().c_str()));
        return;
    }

    setVMEConfig(vmeConfig.get());
    setConfigFileName(fileName);
    setMode(GlobalMode::DAQ);
    setVMEController(vmeConfig->getControllerType(), vmeConfig->getControllerSettings());
    vmeConfig.release();

    if (m_d->m_vmeConfigAutoSaver)
    {
        // Config load was successful. If an autosave exists it will contain the
        // (now obsolete) contents of the previous vme config so we remove it
        // and then restart the autosaver.
        m_d->m_vmeConfigAutoSaver->removeOutputFile();
        m_d->m_vmeConfigAutoSaver->start();
    }
}

void MVMEContext::vmeConfigWasSaved()
{
    if (m_d->m_vmeConfigAutoSaver)
    {
        // (re)start the autosaver
        m_d->m_vmeConfigAutoSaver->start();
    }
}

bool MVMEContext::loadAnalysisConfig(const QString &fileName)
{
    qDebug() << "loadAnalysisConfig from" << fileName;

    QJsonDocument doc(gui_read_json_file(fileName));

    if (doc.isNull())
        return false;

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

    if (doc.isNull())
        return false;

    if (loadAnalysisConfig(doc, inputInfo))
    {
        setAnalysisConfigFileName(QString());
        return true;
    }

    return false;
}

bool MVMEContext::loadAnalysisConfig(const QByteArray &blob, const QString &inputInfo)
{
    auto doc = QJsonDocument::fromJson(blob);

    return loadAnalysisConfig(doc, inputInfo);
}

bool MVMEContext::loadAnalysisConfig(const QJsonDocument &doc, const QString &inputInfo,
                                     AnalysisLoadFlags flags)
{
    using namespace analysis;
    using namespace vme_analysis_common;

    QJsonObject json = doc.object();

    if (json.contains("AnalysisNG"))
    {
        json = json["AnalysisNG"].toObject();
    }

    auto analysis_ng = std::make_unique<Analysis>();

    if (auto ec = analysis_ng->read(json, getVMEConfig()))
    {
        QMessageBox::critical(nullptr,
                              QSL("Error loading analysis"),
                              QSL("Error loading analysis from file %1: %2")
                              .arg(inputInfo)
                              .arg(ec.message().c_str()));

        return false;
    }

    if (!handle_vme_analysis_assignment(
            getVMEConfig(), analysis_ng.get(),
            nullptr, getMainWindow()))
    {
        return false;
    }

    try
    {
        bool was_running = isAnalysisRunning();

        if (was_running)
        {
            stopAnalysis();
        }

        auto oldAnalysis = m_analysis.release();
        oldAnalysis->deleteLater();

        m_analysis = std::move(analysis_ng);

        m_analysis->beginRun(getRunInfo(),
                             vme_analysis_common::build_id_to_index_mapping(getVMEConfig()),
                             [this](const QString &msg) { this->logMessage(msg); });

        if (m_d->m_analysisAutoSaver)
        {
            // (re)start the autosaver
            m_d->m_analysisAutoSaver->removeOutputFile();
            m_d->m_analysisAutoSaver->start();
        }

        emit analysisChanged(m_analysis.get());

        logMessage(QString("Loaded %1 from %2")
                   .arg(info_string(m_analysis.get()))
                   .arg(inputInfo)
                   );

        if (was_running && !flags.NoAutoResume)
        {
            resumeAnalysis(Analysis::ClearState);
        }
    }
    catch (const std::bad_alloc &e)
    {
        if (m_analysis)
            m_analysis->clear();
        setAnalysisConfigFileName(QString());
        QMessageBox::critical(m_mainwin, QSL("Error"),
                              QString("Out of memory when creating analysis objects."));
        emit analysisChanged(m_analysis.get());

        return false;
    }

    return true;
}

void MVMEContext::analysisWasCleared()
{
    if (m_d->m_analysisAutoSaver)
    {
        // (re)start the autosaver
        m_d->m_analysisAutoSaver->start();
    }
}

void MVMEContext::analysisWasSaved()
{
    if (m_d->m_analysisAutoSaver)
    {
        // (re)start the autosaver
        m_d->m_analysisAutoSaver->start();
    }
}

void MVMEContext::setListFileOutputInfo(const ListFileOutputInfo &info)
{
    m_d->m_listfileOutputInfo = info;

    auto settings = makeWorkspaceSettings();

    writeToSettings(info, *settings);

    emit ListFileOutputInfoChanged(info);
}

ListFileOutputInfo MVMEContext::getListFileOutputInfo() const
{
    // Tracing an issue on exit where m_d has already been destroyed but
    // MVMEMainWindow is calling this method.
    assert(m_d);
    return m_d->m_listfileOutputInfo;
}

QString MVMEContext::getListFileOutputDirectoryFullPath(const QString &directory) const
{
    QDir dir(directory);

    if (dir.isAbsolute())
        return dir.path();

    dir = QDir(getWorkspaceDirectory());
    return dir.filePath(directory);
}

/** True if at least one of VME-config and analysis-config is modified. */
bool MVMEContext::isWorkspaceModified() const
{
    return ((m_vmeConfig && m_vmeConfig->isModified())
            || (m_analysis && m_analysis->isModified())
           );
}

bool MVMEContext::isAnalysisRunning()
{
    return (getMVMEStreamWorkerState() != MVMEStreamWorkerState::Idle);
}

void MVMEContext::stopAnalysis()
{
    m_d->stopAnalysis();
}

void MVMEContext::resumeAnalysis(analysis::Analysis::BeginRunOption runOption)
{
    m_d->resumeAnalysis(runOption);
}

QJsonDocument MVMEContext::getAnalysisJsonDocument() const
{
    QJsonObject dest, json;
    getAnalysis()->write(dest);
    json[QSL("AnalysisNG")] = dest;
    QJsonDocument doc(json);
    return doc;
}

void MVMEContext::addAnalysisOperator(QUuid eventId,
                                      const std::shared_ptr<analysis::OperatorInterface> &op,
                                      s32 userLevel)
{
    if (auto eventConfig = m_vmeConfig->getEventConfig(eventId))
    {
        AnalysisPauser pauser(this);
        getAnalysis()->addOperator(eventId, userLevel, op);
        getAnalysis()->beginRun(analysis::Analysis::KeepState);

        if (m_analysisUi)
        {
            m_analysisUi->operatorAddedExternally(op);
        }
    }
}

void MVMEContext::analysisOperatorEdited(const std::shared_ptr<analysis::OperatorInterface> &op)
{
    AnalysisPauser pauser(this);
    getAnalysis()->setOperatorEdited(op);
    getAnalysis()->beginRun(analysis::Analysis::KeepState);

    if (m_analysisUi)
    {
        m_analysisUi->operatorEditedExternally(op);
    }
}

RunInfo MVMEContext::getRunInfo() const
{
    return m_d->m_runInfo;
}

// DAQPauser
DAQPauser::DAQPauser(MVMEContext *context)
    : context(context)
{
    was_running = (context->getDAQState() == DAQState::Running);

    qDebug() << __PRETTY_FUNCTION__ << "was_running =" << was_running;

    if (was_running)
    {
        context->pauseDAQ();
    }
}

DAQPauser::~DAQPauser()
{
    qDebug() << __PRETTY_FUNCTION__ << "was_running =" << was_running;
    if (was_running)
    {
        context->resumeDAQ();
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
