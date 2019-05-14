#include "mvlc/mvlc_dev_gui.h"

#include <QComboBox>
#include <QDebug>
#include <QFileDialog>
#include <QGridLayout>
#include <QHostAddress>
#include <QMessageBox>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>
#include <QTableWidget>

#include <iostream>
#include <cmath>

#include "ui_mvlc_dev_ui.h"

#include "mvlc/mvlc_dialog.h"
#include "mvlc/mvlc_error.h"
#include "mvlc/mvlc_script.h"
#include "mvlc/mvlc_impl_udp.h"
#include "mvlc/mvlc_impl_usb.h"
#include "mvlc/mvlc_vme_debug_widget.h"
#include "mvlc/mvlc_util.h"
#include "qt_util.h"
#include "util/counters.h"

using namespace mesytec;
using namespace mesytec::mvlc;
using namespace mesytec::mvlc::usb;

FixedSizeBuffer make_buffer(size_t capacity)
{
    FixedSizeBuffer result
    {
        .data = std::make_unique<u8[]>(capacity),
        .capacity = capacity,
        .used = 0,
    };

    result.payloadBegin = result.data.get();

    return result;
}

const char *reader_stat_name(ReaderStats::CounterEnum counter)
{
    switch (counter)
    {
        case ReaderStats::TotalBytesReceived:
            return "TotalBytesReceived";
        case ReaderStats::NumberOfAttemptedReads:
            return "NumberOfAttemptedReads";
        case ReaderStats::NumberOfTimeouts:
            return "NumberOfTimeouts";
        case ReaderStats::NumberOfErrors:
            return "NumberOfErrors";
        case ReaderStats::FramesSeen:
            return "FramesSeen";
        case ReaderStats::FramesCrossingBuffers:
            return "FramesCrossingBuffers";
        case ReaderStats::FramesWithContinueFlag:
            return "FramesWithContinueFlag";

        case ReaderStats::CountersCount:
            return "INVALID COUNTER";
    }

    return "UNKNOWN COUNTER";
}

static const QString Key_LastMVLCScriptDirectory = "Files/LastMVLCScriptDirectory";
static const QString Key_LastMVLCDataOutputDirectory = "Files/LastMVLCDataOutputDirectory";
static const QString DefaultOutputFilename = "mvlc_dev_data.bin";

//
// MVLCDataReader
//
MVLCDataReader::MVLCDataReader(QObject *parent)
    : QObject(parent)
    , m_doQuit(false)
    , m_nextBufferRequested(false)
    , m_stackFrameCheckEnabled(true)
    , m_logAllBuffers(false)
    , m_readBuffer(make_buffer(ReadBufferSize))
{
}

MVLCDataReader::~MVLCDataReader()
{
}

ReaderStats MVLCDataReader::getStats() const
{
    ReaderStats result;
    {
        QMutexLocker guard(&m_statsMutex);
        result = m_stats;
    }
    return result;
}

ReaderStats MVLCDataReader::getAndResetStats()
{
    ReaderStats result;
    {
        QMutexLocker guard(&m_statsMutex);
        result = m_stats;
        m_stats = {};
    }
    return result;
}

void MVLCDataReader::resetStats()
{
    QMutexLocker guard(&m_statsMutex);
    m_stats = {};
}

bool MVLCDataReader::isStackFrameCheckEnabled() const
{
    return m_stackFrameCheckEnabled;
}

void MVLCDataReader::setMVLC(MVLCObject *mvlc)
{
    m_mvlc = mvlc;
}

void MVLCDataReader::setOutputDevice(std::unique_ptr<QIODevice> dev)
{
    m_outDevice = std::move(dev);
}

FrameCheckResult frame_check(const FixedSizeBuffer &buffer, FrameCheckData &data)
{
    const u32 *buffp = reinterpret_cast<const u32 *>(buffer.payloadBegin);
    const u32 *endp  = reinterpret_cast<const u32 *>(buffer.data.get() + buffer.used);

    while (true)
    {
        const u32 *nextp = buffp + data.nextHeaderOffset;

        if (nextp >= endp)
        {
            data.nextHeaderOffset = nextp - endp;

            if (nextp == endp)
            {
                ++data.framesChecked;
                return FrameCheckResult::Ok;
            }

            return FrameCheckResult::NeedMoreData;
        }

        const u32 header = *nextp;

        if (!(is_stack_buffer(header) || is_stack_buffer_continuation(header)))
        {
            // leave nextHeaderOffset unmodified for inspection
            return FrameCheckResult::HeaderMatchFailed;
        }

        const u16 len = header & 0xFFFF;
        const u8 stackId = (header >> 16) & 0x0F;
        const u8 flags   = (header >> 20) & 0x0F;

        if (stackId < stacks::StackCount)
            ++data.stackHits[stackId];

        if (flags & (1u << 3))
            ++data.framesWithContinueFlag;

        ++data.framesChecked;
        data.nextHeaderOffset += 1 + len;
    }

    return {};
}

void MVLCDataReader::readoutLoop()
{
    m_doQuit = false;
    resetStats();
    m_frameCheckData = {};
    m_stackFrameCheckEnabled = true;

    emit started();

    m_mvlc->setReadTimeout(Pipe::Data, ReadTimeout_ms);

    qDebug() << __PRETTY_FUNCTION__ << "entering readout loop";
    qDebug() << __PRETTY_FUNCTION__ << "executing in" << QThread::currentThread();
    qDebug() << __PRETTY_FUNCTION__ << "read timeout is"
        << m_mvlc->getReadTimeout(Pipe::Data) << "ms";
    qDebug() << __PRETTY_FUNCTION__ << "readbuffer capacity is" << m_readBuffer.capacity;

    udp::Impl *mvlc_udp = nullptr;

    if (m_mvlc->connectionType() == ConnectionType::UDP)
    {
        mvlc_udp = reinterpret_cast<udp::Impl *>(m_mvlc->getImpl());

        emit message(QSL("Connection type is UDP. Sending initial empty request"
                         " using the data socket."));

        size_t bytesTransferred = 0;

        static const std::array<u32, 2> EmptyRequest =
        {
            0xF1000000,
            0xF2000000
        };

        if (auto ec = m_mvlc->write(Pipe::Data,
                                    reinterpret_cast<const u8 *>(EmptyRequest.data()),
                                    EmptyRequest.size() * sizeof(u32),
                                    bytesTransferred))
        {
            emit message(QSL("Error sending initial empty request using the data socket: %1")
                         .arg(ec.message().c_str()));
            emit stopped();
            return;
        }
    }

    auto tStart = std::chrono::high_resolution_clock::now();

    while (!m_doQuit)
    {
        size_t bytesTransferred = 0u;
        std::error_code ec;
        udp::PacketReadResult udp_rr = {};

        if (mvlc_udp)
        {
            // Manual locking. Maybe better to make read_packet() available in a higher layer?
            auto guard = m_mvlc->getLocks().lockData();
            auto udp_rr = mvlc_udp->read_packet(Pipe::Data, m_readBuffer.data.get(), m_readBuffer.capacity);
            ec = udp_rr.ec;
            bytesTransferred = udp_rr.bytesTransferred;
            m_readBuffer.payloadBegin = m_readBuffer.data.get() + udp::HeaderBytes;
        }
        else
        {
            ec = m_mvlc->read(Pipe::Data,
                              m_readBuffer.data.get(), m_readBuffer.capacity,
                              bytesTransferred);
        }

        m_readBuffer.used = bytesTransferred;


        if (ec == ErrorType::ConnectionError || ec == ErrorType::IOError)
        {
            emit message(QSL("Lost connection to MVLC. Leaving readout loop. Reason: %1")
                         .arg(ec.message().c_str()));
            break;
        }
        else if (ec && ec != ErrorType::Timeout)
        {
            emit message(QSL("Other error from read: %1, %2, %3")
                         .arg(ec.message().c_str())
                         .arg(ec.category().name())
                         .arg(ec.value()));
            break;
        }

        // stats
        {
            QMutexLocker guard(&m_statsMutex);

            ++m_stats.counters[ReaderStats::NumberOfAttemptedReads];
            m_stats.counters[ReaderStats::TotalBytesReceived] += bytesTransferred;
            if (bytesTransferred > 0)
                ++m_stats.readBufferSizes[bytesTransferred];

            if (ec)
            {
                if (ec == ErrorType::Timeout)
                    ++m_stats.counters[ReaderStats::NumberOfTimeouts];
                else
                    ++m_stats.counters[ReaderStats::NumberOfErrors];
            }
        }

        // FIXME: udp case needs used > udp::HeaderBytes
        if (m_readBuffer.used > 0 && m_stackFrameCheckEnabled)
        {
            auto checkResult = frame_check(m_readBuffer, m_frameCheckData);
            QMutexLocker guard(&m_statsMutex);
            m_stats.counters[ReaderStats::FramesSeen] = m_frameCheckData.framesChecked;
            m_stats.counters[ReaderStats::FramesWithContinueFlag] =
                m_frameCheckData.framesWithContinueFlag;
            m_stats.stackHits = m_frameCheckData.stackHits;

            if (checkResult == FrameCheckResult::HeaderMatchFailed)
            {
                if (mvlc_udp && udp_rr.hasHeaders())
                {
                    emit message(QSL("Adjusting FrameCheckData.nextHeaderOffset using UDP frame info"));
                    m_frameCheckData.nextHeaderOffset = udp_rr.nextHeaderPointer();
                    checkResult = frame_check(m_readBuffer, m_frameCheckData);
                }
                else
                {
                    m_stackFrameCheckEnabled = false;

                    emit message(QSL("!!! !!! !!!"));
                    emit message(QSL("Frame Check header match failed! Disabling frame check."));
                    emit message(QSL("  nextHeaderOffset=%1")
                                 .arg(m_frameCheckData.nextHeaderOffset));

                    u32 nextHeader = *reinterpret_cast<u32 *>(m_readBuffer.data.get())
                        + m_frameCheckData.nextHeaderOffset;

                    emit message(QSL("  nextHeader=0x%1")
                                 .arg(nextHeader, 8, 16, QLatin1Char('0')));
                    emit message(QSL("!!! !!! !!!"));
                }
            }
            else if (checkResult == FrameCheckResult::NeedMoreData)
            {
                ++m_stats.counters[ReaderStats::FramesCrossingBuffers];
            }
        }

        if ((m_nextBufferRequested || m_logAllBuffers) && m_readBuffer.used > 0)
        {
            QVector<u8> bufferCopy;
            bufferCopy.reserve(m_readBuffer.used);
            std::copy(m_readBuffer.data.get(),
                      m_readBuffer.data.get() + m_readBuffer.used,
                      std::back_inserter(bufferCopy));
            emit bufferReady(bufferCopy);
            m_nextBufferRequested = false;
        }

        if (m_readBuffer.used > 0 && m_outDevice)
        {
            m_outDevice->write(reinterpret_cast<const char *>(m_readBuffer.data.get()),
                               m_readBuffer.used);
        }

        {
            auto now = std::chrono::high_resolution_clock::now();
            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - tStart);

            if (dt.count() > 1000)
            {
                tStart = now;
                qDebug() << "readout is alive!";
            }
        }
    }

    qDebug() << __PRETTY_FUNCTION__ << "left readout loop";

    m_outDevice = {};

    qDebug() << __PRETTY_FUNCTION__ << "emitting stopped() signal";
    emit stopped();
}

void MVLCDataReader::stop()
{
    m_doQuit = true;
}

void MVLCDataReader::requestNextBuffer()
{
    m_nextBufferRequested = true;
}

//
// MVLCDevGUI
//
struct MVLCDevGUI::Private
{
    MVLCDevGUI *q;

    // Widgets
    QWidget *centralWidget;
    QToolBar *toolbar;
    QStatusBar *statusbar;
    MVLCRegisterWidget *registerWidget;
    VMEDebugWidget *vmeDebugWidget;

    //QAction *act_showLog,
    //        *act_showVMEDebug,
    //        *act_loadScript
    //        ;

    MVLCObject *mvlc;
    QThread readoutThread;
    MVLCDataReader *dataReader;

    // DataReader stats
    QVector<QLabel *> readerStatLabels;
    QLabel *l_statRunDuration,
           *l_statReadRate;
    QPushButton *pb_printReaderBufferSizes,
                *pb_printStackHits;

    QDateTime tReaderStarted,
              tReaderStopped,
              tLastUpdate;

    ReaderStats prevReaderStats = {};
};

MVLCDevGUI::MVLCDevGUI(MVLCObject *mvlc, QWidget *parent)
    : QMainWindow(parent)
    , m_d(std::make_unique<Private>())
    , ui(new Ui::MVLCDevGUI)
{
    assert(m_d->dataReader == nullptr);

    m_d->q = this;
    m_d->mvlc = mvlc;
    m_d->registerWidget = new MVLCRegisterWidget(m_d->mvlc, this);
    m_d->vmeDebugWidget = new VMEDebugWidget(m_d->mvlc, this);

    auto updateTimer = new QTimer(this);
    updateTimer->setInterval(1000);

    setObjectName(QSL("MVLC Dev GUI"));
    setWindowTitle(objectName());

    m_d->toolbar = new QToolBar(this);
    m_d->statusbar = new QStatusBar(this);
    m_d->centralWidget = new QWidget(this);
    ui->setupUi(m_d->centralWidget);

    setCentralWidget(m_d->centralWidget);
    addToolBar(m_d->toolbar);
    setStatusBar(m_d->statusbar);

    // MVLC Script Editor
    {
        auto font = make_monospace_font();
        font.setPointSize(8);
        ui->te_scriptInput->setFont(font);
        ui->te_udpScriptInput->setFont(font);
    }

    new vme_script::SyntaxHighlighter(ui->te_scriptInput->document());
    static const int SpacesPerTab = 4;
    int tabWidth = calculate_tab_width(ui->te_scriptInput->font(), SpacesPerTab);
    ui->te_scriptInput->setTabStopWidth(tabWidth);
    ui->te_udpScriptInput->setTabStopWidth(tabWidth);

    // Reader stats ui setup
    {
        auto l = new QFormLayout(ui->gb_readerStats);
        l->setSizeConstraint(QLayout::SetMinimumSize);

        for (int counterType = 0;
             counterType < ReaderStats::CountersCount;
             counterType++)
        {
            auto name = reader_stat_name(
                static_cast<ReaderStats::CounterEnum>(counterType));
            auto label = new QLabel();
            m_d->readerStatLabels.push_back(label);
            l->addRow(name, label);
        }

        m_d->l_statRunDuration = new QLabel();
        l->addRow("Run Duration", m_d->l_statRunDuration);

        m_d->l_statReadRate = new QLabel();
        l->addRow("Read Rate", m_d->l_statReadRate);

        m_d->pb_printReaderBufferSizes = new QPushButton("Print Incoming Buffer Sizes");
        m_d->pb_printStackHits = new QPushButton("Print Stack Hits");
        {
            auto bl = make_layout<QHBoxLayout, 0, 0>();
            bl->addWidget(m_d->pb_printReaderBufferSizes);
            bl->addWidget(m_d->pb_printStackHits);
            bl->addStretch();
            l->addRow(bl);
        }
    }

    // UDP receive stats table
    ui->gb_udpStats->hide();
    if (m_d->mvlc->connectionType() == ConnectionType::UDP)
    {
        ui->gb_udpStats->show();

        //
        // UDP pipe stats table
        //
        auto tbl = new QTableWidget(this);

        static const QStringList colTitles = {
            "Cmd(0)", "Data(1)"
        };


        tbl->setColumnCount(colTitles.size());
        tbl->setHorizontalHeaderLabels(colTitles);

        auto update_stats_table = [this, tbl]()
        {
            auto guard = m_d->mvlc->getLocks().lockBoth();
            auto udp_impl = reinterpret_cast<udp::Impl *>(m_d->mvlc->getImpl());

            static auto lastPipeStats = udp_impl->getPipeStats();

            static QDateTime lastUpdateTime;
            QDateTime now = QDateTime::currentDateTime();

            if (!lastUpdateTime.isValid())
            {
                lastUpdateTime = now;
                return;
            }

            QStringList rowTitles = {
                "rcvdPackets", "packets/s",
                "shortPackets",
                "receivedBytes", "bytesPerSecond",
                "noHeader", "headerOutOfRange"
            };

            double secondsElapsed = lastUpdateTime.msecsTo(now) / 1000.0;
            auto pipeStats = udp_impl->getPipeStats();

            for (unsigned ht = 0; ht < 256; ht++)
            {
                if (pipeStats[0].headerTypes[ht] || pipeStats[1].headerTypes[ht])
                {
                    rowTitles << QString("headerType 0x%1").arg(ht, 2, 16, QLatin1Char('0'));
                }
            }

            tbl->setRowCount(rowTitles.size());
            tbl->setVerticalHeaderLabels(rowTitles);
            int firstHeaderTypeRow = 0u;
            using QTWI = QTableWidgetItem;

            for (unsigned pipe = 0; pipe < pipeStats.size(); pipe++)
            {
                auto &lastStats = lastPipeStats[pipe];
                auto &stats = pipeStats[pipe];
                int row = 0;

                s64 deltaPackets = stats.receivedPackets - lastStats.receivedPackets;
                double packetsPerSecond = deltaPackets / secondsElapsed;
                s64 deltaBytes = stats.receivedBytes - lastStats.receivedBytes;
                double bytesPerSecond = deltaBytes / secondsElapsed;

                tbl->setItem(row++, pipe, new QTWI(QSL("%1").arg(stats.receivedPackets)));
                tbl->setItem(row++, pipe, new QTWI(QSL("%1").arg(packetsPerSecond)));
                tbl->setItem(row++, pipe, new QTWI(QSL("%1").arg(stats.shortPackets)));
                tbl->setItem(row++, pipe, new QTWI(QSL("%1").arg(stats.receivedBytes)));
                tbl->setItem(row++, pipe, new QTWI(QSL("%1").arg(bytesPerSecond)));
                tbl->setItem(row++, pipe, new QTWI(QSL("%1").arg(stats.noHeader)));
                tbl->setItem(row++, pipe, new QTWI(QSL("%1").arg(stats.headerOutOfRange)));

                firstHeaderTypeRow = row;
            }

            for (unsigned ht = 0, row = firstHeaderTypeRow; ht < 256; ht++)
            {
                if (pipeStats[0].headerTypes[ht] || pipeStats[1].headerTypes[ht])
                {
                    for (unsigned pipe = 0; pipe < pipeStats.size(); pipe++)
                    {
                        tbl->setItem(row, pipe, new QTWI(
                                QString::number(pipeStats[pipe].headerTypes[ht])));
                    }
                    row++;
                }
            }

            tbl->resizeColumnsToContents();
            tbl->resizeRowsToContents();

            lastPipeStats = pipeStats;
            lastUpdateTime = now;
        };

        connect(updateTimer, &QTimer::timeout, this, update_stats_table);

        //
        // UDP packet channel loss counters
        //
        QStringList channelNames = { "Command", "Stack", "Data" };
        std::array<QLabel *, udp::NumPacketChannels> lossLabels;
        auto l_packetLoss = new QFormLayout();
        l_packetLoss->addRow(new QLabel("Packet loss counters"));
        for (u8 chan = 0; chan < udp::NumPacketChannels; chan++)
        {
            lossLabels[chan] = new QLabel(this);
            l_packetLoss->addRow(channelNames[chan], lossLabels[chan]);
        }

        auto update_loss_labels = [this, lossLabels] ()
        {
            auto guard = m_d->mvlc->getLocks().lockBoth();
            auto udp_impl = reinterpret_cast<udp::Impl *>(m_d->mvlc->getImpl());
            auto channelStats = udp_impl->getPacketChannelStats();

            for (size_t chan = 0; chan < channelStats.size(); chan++)
            {
                auto label = lossLabels[chan];
                label->setText(QString::number(channelStats[chan].lostPackets));
            }
        };

        connect(updateTimer, &QTimer::timeout, this, update_loss_labels);

#if 0
        auto debug_print_packet_sizes = [this] ()
        {
            auto guard = m_d->mvlc->getLocks().lockBoth();
            auto udp_impl = reinterpret_cast<udp::Impl *>(m_d->mvlc->getImpl());
            auto channelStats = udp_impl->getPacketChannelStats();

            for (size_t chan = 0; chan < channelStats.size(); chan++)
            {
                const auto &pktSizes = channelStats[chan].packetSizes;

                if (pktSizes.empty())
                    continue;

                std::vector<u16> sizeVec;
                sizeVec.reserve(pktSizes.size());

                for (const auto &kv: pktSizes)
                    sizeVec.push_back(kv.first);

                std::sort(sizeVec.begin(), sizeVec.end());

                qDebug("Incoming packet sizes for packet channel %lu:", chan);

                for (u16 pktSize: sizeVec)
                {
                    qDebug("  sz=%4u, packets=%lu", pktSize, pktSizes.at(pktSize));
                }
            }
        };

        connect(updateTimer, &QTimer::timeout, this, debug_print_packet_sizes);
#endif

        auto udpStatsLayout = new QHBoxLayout(ui->gb_udpStats);
        udpStatsLayout->addWidget(tbl);
        udpStatsLayout->addLayout(l_packetLoss);
    }

    // Interactions

    // mvlc connection state changes
    connect(m_d->mvlc, &MVLCObject::stateChanged,
            this, [this] (const MVLCObject::State &oldState,
                          const MVLCObject::State &newState)
    {
        switch (newState)
        {
            case MVLCObject::Disconnected:
                ui->le_connectionStatus->setText("Disconnected");
                break;
            case MVLCObject::Connecting:
                ui->le_connectionStatus->setText("Connecting...");
                break;
            case MVLCObject::Connected:
                ui->le_connectionStatus->setText("Connected");
                logMessage("Connected to MVLC");
                break;
        }

        ui->pb_runScript->setEnabled(newState == MVLCObject::Connected);
        ui->pb_reconnect->setEnabled(newState != MVLCObject::Connecting);
    });

    // log stack error notifications published by the mvlc object
    connect(m_d->mvlc, &MVLCObject::stackErrorNotification,
            this, [this] (const QVector<u32> &buffer)
    {
        logBuffer(buffer, "Stack error notification from MVLC");
    });

    connect(ui->pb_runScript, &QPushButton::clicked,
            this, [this] ()
    {
        try
        {
            bool logRequest = ui->cb_scriptLogRequest->isChecked();
            bool logMirror  = ui->cb_scriptLogMirror->isChecked();

            auto scriptText = ui->te_scriptInput->toPlainText();
            auto cmdList = mvlc::script::parse(scriptText);
            auto cmdBuffer = mvlc::script::to_mvlc_command_buffer(cmdList);

            if (logRequest)
            {
                logBuffer(cmdBuffer, "Outgoing Request Buffer");
            }

            QVector<u32> responseBuffer;

            if (auto ec = m_d->mvlc->mirrorTransaction(cmdBuffer, responseBuffer))
            {
                logMessage(QString("Error performing MVLC mirror transaction: %1")
                           .arg(ec.message().c_str()));

                if (!logRequest)
                {
                    // In case of a mirror check error do log the request
                    // buffer but only if it has not been logged yet.
                    logBuffer(cmdBuffer, "Outgoing Request Buffer");
                }
                logBuffer(responseBuffer, "Incoming erroneous Mirror Buffer");
                return;
            }

            if (logMirror)
            {
                logBuffer(responseBuffer, "Incoming Mirror Buffer");
            }

            // Log a short message after any buffers have been logged.
            logMessage(QString("Sent %1 words, received %2 words, mirror check ok.")
                       .arg(cmdBuffer.size())
                       .arg(responseBuffer.size()));

            if (ui->cb_scriptReadStack->isChecked())
            {
                logMessage("Attempting to read stack response...");

                auto ec = m_d->mvlc->readResponse(is_stack_buffer, responseBuffer);

                if (ec && ec != ErrorType::Timeout)
                {
                    logMessage(QString("Error reading from MVLC: %1")
                               .arg(ec.message().c_str()));
                    return;
                }
                else if (responseBuffer.isEmpty())
                {
                    logMessage("Did not receive a stack response from MVLC");
                    return;
                }

                if (ec == ErrorType::Timeout)
                    logMessage("Received response but ran into a read timeout");

                logBuffer(responseBuffer, "Stack response from MVLC");

                // Same as is done in MVLCDialog::stackTransaction(): if error
                // bits are set, read in the error notification (0xF7) buffer
                // and log it.
                u32 header = responseBuffer[0];
                u8 errorBits = (header >> buffer_headers::BufferFlagsShift) & buffer_headers::BufferFlagsMask;

                if (errorBits)
                {
                    QVector<u32> tmpBuffer;
                    m_d->mvlc->readKnownBuffer(tmpBuffer);
                    if (!tmpBuffer.isEmpty())
                    {
                        u32 header = tmpBuffer[0];

                        if (is_stackerror_notification(header))
                            logBuffer(tmpBuffer, "Stack error notification from MVLC");
                        else
                            logBuffer(tmpBuffer, "Unexpected buffer contents (wanted a stack error notification (0xF7)");
                    }
                }
            }

            for (const auto &notification: m_d->mvlc->getStackErrorNotifications())
            {
                logBuffer(notification, "Error notification from MVLC");
            }
        }
        catch (const mvlc::script::ParseError &e)
        {
            logMessage("MVLC Script parse error: " + e.toString());
        }
        catch (const vme_script::ParseError &e)
        {
            logMessage("Embedded VME Script parse error: " + e.toString());
        }
    });

    connect(ui->pb_loadScript, &QPushButton::clicked,
            this, [this] ()
    {
        QString path = QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation).at(0);
        QSettings settings;
        if (settings.contains(Key_LastMVLCScriptDirectory))
        {
            path = settings.value(Key_LastMVLCScriptDirectory).toString();
        }

        QString fileName = QFileDialog::getOpenFileName(
            this, QSL("Load MVLC script file"), path,
            QSL("MVLC scripts (*.mvlcscript);; All Files (*)"));

        if (!fileName.isEmpty())
        {
            QFile file(fileName);
            if (file.open(QIODevice::ReadOnly))
            {
                QTextStream stream(&file);
                ui->te_scriptInput->setPlainText(stream.readAll());
                QFileInfo fi(fileName);
                settings.setValue(Key_LastMVLCScriptDirectory, fi.absolutePath());
            }
        }
    });

    connect(ui->pb_saveScript, &QPushButton::clicked,
            this, [this] ()
    {
        QString path = QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation).at(0);
        QSettings settings;
        if (settings.contains(Key_LastMVLCScriptDirectory))
        {
            path = settings.value(Key_LastMVLCScriptDirectory).toString();
        }

        QString fileName = QFileDialog::getSaveFileName(
            this, QSL("Save MVLC script"), path,
            QSL("MVLC scripts (*.mvlcscript);; All Files (*)"));

        if (fileName.isEmpty())
            return;

        QFileInfo fi(fileName);
        if (fi.completeSuffix().isEmpty())
        {
            fileName += ".mvlcscript";
        }

        QFile file(fileName);
        if (!file.open(QIODevice::WriteOnly))
        {
            QMessageBox::critical(this, "File error",
                                  QString("Error opening \"%1\" for writing").arg(fileName));
            return;
        }

        QTextStream stream(&file);
        stream << ui->te_scriptInput->toPlainText();

        if (stream.status() != QTextStream::Ok)
        {
            QMessageBox::critical(this, "File error",
                                  QString("Error writing to \"%1\"").arg(fileName));
            return;
        }

        settings.setValue(Key_LastMVLCScriptDirectory, fi.absolutePath());
    });

    connect(ui->pb_clearScript, &QPushButton::clicked,
            this, [this] ()
    {
        ui->te_scriptInput->clear();
    });

    connect(ui->pb_reconnect, &QPushButton::clicked,
            this, [this] ()
    {
        if (m_d->mvlc->isConnected())
        {
            if (auto ec = m_d->mvlc->disconnect())
            {
                logMessage(QString("Error from disconnect(): %1")
                           .arg(ec.message().c_str()));
            }
        }

        if (auto ec = m_d->mvlc->connect())
        {
            logMessage(QString("Error connecting to MVLC: %1")
                       .arg(ec.message().c_str()));
        }
    });


    connect(ui->pb_readCmdPipe, &QPushButton::clicked,
            this, [this] ()
    {
        static const int ManualCmdRead_WordCount = 1024;
        QVector<u32> readBuffer;
        readBuffer.resize(ManualCmdRead_WordCount);
        size_t bytesTransferred;

        auto ec = m_d->mvlc->read(
            Pipe::Command,
            reinterpret_cast<u8 *>(readBuffer.data()),
            readBuffer.size() * sizeof(u32),
            bytesTransferred);

        // IMPORTANT: This silently discards any superfluous bytes.
        readBuffer.resize(bytesTransferred / sizeof(u32));

        if (!readBuffer.isEmpty())
            logBuffer(readBuffer, "Results of manual read from Command Pipe");

        if (ec)
            logMessage(QString("Read error: %1").arg(ec.message().c_str()));
    });


    connect(ui->pb_readDataPipe, &QPushButton::clicked,
            this, [this] ()
    {
        static const int ManualDataRead_WordCount = 8192;
        QVector<u32> readBuffer;
        readBuffer.resize(ManualDataRead_WordCount);
        size_t bytesTransferred;

        auto ec = m_d->mvlc->read(
            Pipe::Data,
            reinterpret_cast<u8 *>(readBuffer.data()),
            readBuffer.size() * sizeof(u32),
            bytesTransferred);

        // IMPORTANT: This silently discards any superfluous bytes.
        readBuffer.resize(bytesTransferred / sizeof(u32));

        if (!readBuffer.isEmpty())
            logBuffer(readBuffer, "Results of manual read from Data Pipe");

        if (ec)
            logMessage(QString("Read error: %1").arg(ec.message().c_str()));
    });

    //
    // MVLCDataReader and readout thread
    //

    m_d->readoutThread.setObjectName("MVLC Readout");
    m_d->dataReader = new MVLCDataReader();
    m_d->dataReader->setMVLC(m_d->mvlc);
    m_d->dataReader->moveToThread(&m_d->readoutThread);

    connect(&m_d->readoutThread, &QThread::started,
            m_d->dataReader, &MVLCDataReader::readoutLoop);

    connect(m_d->dataReader, &MVLCDataReader::stopped,
            &m_d->readoutThread, &QThread::quit);

    connect(ui->pb_readerStart, &QPushButton::clicked,
            this, [this] ()
    {
        assert(!m_d->readoutThread.isRunning());

        logMessage("Starting readout");

        if (ui->gb_dataOutputFile->isChecked())
        {
            QString outputFilePath = ui->le_dataOutputFilePath->text();

            if (outputFilePath.isEmpty())
            {
                logMessage("Data Reader Error: output filename is empty");
            }
            else
            {
                std::unique_ptr<QIODevice> outFile = std::make_unique<QFile>(outputFilePath);

                if (!outFile->open(QIODevice::WriteOnly))
                {
                    logMessage(QString("Error opening output file '%1' for writing: %2")
                               .arg(outputFilePath)
                               .arg(outFile->errorString()));
                }
                else
                {
                    logMessage(QString("Writing incoming data to file '%1'.")
                               .arg(outputFilePath));

                    m_d->dataReader->setOutputDevice(std::move(outFile));
                }
            }
        }

        m_d->readoutThread.start();
    });

    // Populate initial output filepath using a previously saved path if
    // available
    {
        QString outDir;
        QSettings settings;

        if (settings.contains(Key_LastMVLCDataOutputDirectory))
        {
            outDir = settings.value(Key_LastMVLCDataOutputDirectory).toString();
        }
        else
        {
            outDir = QStandardPaths::standardLocations(
                QStandardPaths::DocumentsLocation).at(0);
        }

        ui->le_dataOutputFilePath->setText(outDir + "/" + DefaultOutputFilename);
    }

    connect(ui->pb_readerStop, &QPushButton::clicked,
            this, [this] ()
    {
        assert(m_d->readoutThread.isRunning());

        logMessage("Stopping readout");
        // Sets the atomic flag to make the reader break out of the loop.
        m_d->dataReader->stop();
    });

    connect(m_d->dataReader, &MVLCDataReader::started,
            this, [this] ()
    {
        qDebug() << "readout thread started";
        ui->pb_readerStart->setEnabled(false);
        ui->pb_readerStop->setEnabled(true);
        ui->le_readoutStatus->setText("Running");
        ui->pb_reconnect->setEnabled(false);
        ui->pb_readDataPipe->setEnabled(false);

        m_d->tReaderStarted = QDateTime::currentDateTime();
        m_d->tReaderStopped = {};
    });

    connect(&m_d->readoutThread, &QThread::finished,
            this, [this] ()
    {
        qDebug() << "readout thread finished";
        ui->pb_readerStart->setEnabled(true);
        ui->pb_readerStop->setEnabled(false);
        ui->le_readoutStatus->setText("Stopped");
        ui->pb_reconnect->setEnabled(true);
        ui->pb_readDataPipe->setEnabled(true);
        m_d->tReaderStopped = QDateTime::currentDateTime();
    });

    ui->pb_readerStop->setEnabled(false);

    // Reset Reader Stats
    connect(ui->pb_readerResetStats, &QPushButton::clicked,
            this, [this] ()
    {
        auto now = QDateTime::currentDateTime();
        m_d->tReaderStarted = now;
        m_d->tReaderStopped = {};
        m_d->tLastUpdate    = now;
        m_d->prevReaderStats = {};
        m_d->dataReader->resetStats();
    });

    // Request that the reader copies and sends out the next buffer it receives.
    connect(ui->pb_readerRequestBuffer, &QPushButton::clicked,
            this, [this] ()
    {
        m_d->dataReader->requestNextBuffer();
    });

    connect(ui->cb_readerLogAll, &QCheckBox::toggled,
            this, [this] (bool b)
    {
        m_d->dataReader->setLogAllBuffers(b);
    });

    connect(m_d->dataReader, &MVLCDataReader::bufferReady,
            this, [this] (QVector<u8> buffer)
    {
        logMessage(QString("Received data buffer containing %1 words (%2 bytes).")
                   .arg(buffer.size() / sizeof(u32))
                   .arg(buffer.size()));

        int maxWords = ui->spin_logReaderBufferMaxWords->value();
        int maxBytes = maxWords > 0 ? maxWords * sizeof(u32) : buffer.size();
        maxBytes = std::min(maxBytes, buffer.size());

        logMessage(QString(">>> First %1 data words:").arg(maxBytes / sizeof(u32)));

        BufferIterator iter(buffer.data(), maxBytes);
        // FIXME: don't call the global logBuffer. it prints BerrMarker and EndMarker as strings.
        ::logBuffer(iter, [this] (const QString &line)
        {
            logMessage(line);
        });

        logMessage(QString("<<< End of buffer log"));
    });

    connect(m_d->dataReader, &MVLCDataReader::message,
            this, [this] (const QString &msg)
    {
        logMessage("Readout Thread: " + msg);
    });

    connect(ui->pb_browseOutputFile, &QPushButton::clicked,
            this, [this] ()
    {
        QString startDir;
        QSettings settings;

        if (settings.contains(Key_LastMVLCDataOutputDirectory))
        {
            startDir = settings.value(Key_LastMVLCDataOutputDirectory).toString();
        }
        else
        {
            startDir = QStandardPaths::standardLocations(
                QStandardPaths::DocumentsLocation).at(0);
        }

        QString filePath = QFileDialog::getSaveFileName(
            this,                                       // parent
            "Select Data Reader Output File",           // caption
            startDir,                                   // dir
            QString(),                                  // filter
            nullptr,                                    // selectedFilter,
            QFileDialog::Options());                    // options

        qDebug() << __PRETTY_FUNCTION__ << filePath;

        if (!filePath.isEmpty())
        {
            ui->le_dataOutputFilePath->setText(filePath);
            QFileInfo fi(filePath);
            QSettings settings;
            settings.setValue(Key_LastMVLCDataOutputDirectory, fi.path());
        }
    });

    connect(m_d->pb_printReaderBufferSizes, &QPushButton::clicked,
            this, [this] ()
    {
        const auto &sizeHash = m_d->prevReaderStats.readBufferSizes;

        if (sizeHash.isEmpty())
        {
            logMessage("Reader did not receive any buffers yet.");
            return;
        }


        auto sizes = sizeHash.keys();
        std::sort(sizes.begin(), sizes.end());

        QStringList lines;
        lines.reserve(sizeHash.size() + 4);

        lines << ">>> Reader receive buffer sizes:";
        lines << "  size (Bytes) | count";
        lines << "  ------------------------";

        for (size_t size: sizes)
        {
            size_t count = sizeHash[size];

            lines << QString("  %1   | %2")
                .arg(size, 10)
                .arg(count);
        }

        lines << "<<< End receive buffer sizes";

        logMessage(lines.join("\n"));
    });

    connect(m_d->pb_printStackHits, &QPushButton::clicked,
            this, [this] ()
    {
        const auto &hits = m_d->prevReaderStats.stackHits;

        bool didPrint = false;

        for (size_t stackId = 0; stackId < hits.size(); stackId++)
        {
            if (hits[stackId])
            {
                logMessage(QSL("stackId=%1, hits=%2")
                           .arg(stackId)
                           .arg(hits[stackId]));
                didPrint = true;
            }
        }

        if (!didPrint)
            logMessage("No stack hits recorded");
    });

    //
    // UDP Debug Tab Interations
    //
    connect(ui->pb_udpSend, &QPushButton::clicked,
            this, [this] ()
    {
        try
        {
            auto scriptText = ui->te_udpScriptInput->toPlainText();
            auto cmdList = mvlc::script::parse(scriptText);
            auto cmdBuffer = mvlc::script::to_mvlc_command_buffer(cmdList);

            //for (u32 &word: cmdBuffer)
            //{
            //    word = qToBigEndian(word);
            //}


            logBuffer(cmdBuffer, "Outgoing Request Buffer");

            QHostAddress destIP(ui->le_udpDestIP->text());
            quint16 destPort = ui->spin_udpDestPort->value();

            static const qint64 MaxPacketPayloadSize = 1480;

            qint64 bytesLeft = cmdBuffer.size() * sizeof(u32);
            const char *dataPtr = reinterpret_cast<char *>(cmdBuffer.data());
            QUdpSocket sock;
            size_t packetsSent = 0;

            while (bytesLeft > 0)
            {
                qint64 bytesToWrite = std::min(bytesLeft, MaxPacketPayloadSize);
                quint64 bytesWritten = sock.writeDatagram(dataPtr, bytesToWrite, destIP, destPort);

                if (bytesWritten < 0)
                {
                    logMessage(QSL("Error from writeDatagram: %1").arg(sock.errorString()));
                    return;
                }

                bytesLeft -= bytesWritten;
                dataPtr += bytesWritten;
                packetsSent++;
            }

            logMessage(QSL("Sent command buffer using %1 UDP packets").arg(packetsSent));
        }
        catch (const mvlc::script::ParseError &e)
        {
            logMessage("MVLC Script parse error: " + e.toString());
        }
        catch (const vme_script::ParseError &e)
        {
            logMessage("Embedded VME Script parse error: " + e.toString());
        }
    });

    //
    // Register Editor Tab
    //
    {
        auto layout = qobject_cast<QGridLayout *>(ui->tab_mvlcRegisters->layout());
        layout->addWidget(m_d->registerWidget);

        connect(m_d->registerWidget, &MVLCRegisterWidget::sigLogMessage,
                this, &MVLCDevGUI::logMessage);
    }

    //
    // VME Debug Widget Tab
    //
    {
        auto layout = qobject_cast<QGridLayout *>(ui->tab_vmeDebug->layout());
        layout->addWidget(m_d->vmeDebugWidget);

        connect(m_d->vmeDebugWidget, &VMEDebugWidget::sigLogMessage,
                this, &MVLCDevGUI::logMessage);
    }

    //
    // Periodic updates
    //

    // Pull ReaderStats from MVLCDataReader
    connect(updateTimer, &QTimer::timeout,
            this, [this] ()
    {
        auto stats = m_d->dataReader->getStats();
        auto &labels = m_d->readerStatLabels;

        for (int counterType = 0;
             counterType < ReaderStats::CountersCount;
             counterType++)
        {
            QString text;
            size_t value = stats.counters[counterType];

            if (counterType == ReaderStats::TotalBytesReceived)
            {
                text = (QString("%1 B, %2 MB")
                        .arg(value)
                        .arg(value / (double)Megabytes(1)));
            }
            else
            {
                text = QString::number(stats.counters[counterType]);
            }

            m_d->readerStatLabels[counterType]->setText(text);
        }

        auto endTime = (m_d->readoutThread.isRunning()
                        ?  QDateTime::currentDateTime()
                        : m_d->tReaderStopped);

        s64 secondsElapsed = m_d->tReaderStarted.msecsTo(endTime) / 1000.0;
        auto durationString = makeDurationString(secondsElapsed);

        m_d->l_statRunDuration->setText(durationString);

        ReaderStats &prevStats = m_d->prevReaderStats;

        double dt = (m_d->tLastUpdate.isValid()
                     ? m_d->tLastUpdate.msecsTo(endTime)
                     : m_d->tReaderStarted.msecsTo(endTime)) / 1000.0;

        u64 deltaBytesRead = calc_delta0(
            stats.counters[ReaderStats::TotalBytesReceived],
            prevStats.counters[ReaderStats::TotalBytesReceived]);

        double bytesPerSecond = deltaBytesRead / dt;
        double mbPerSecond = bytesPerSecond / Megabytes(1);
        if (std::isnan(mbPerSecond))
            mbPerSecond = 0.0;

        u64 deltaFramesSeen = calc_delta0(
            stats.counters[ReaderStats::FramesSeen],
            prevStats.counters[ReaderStats::FramesSeen]);
        double framesPerSecond = deltaFramesSeen / dt;
        if (std::isnan(framesPerSecond))
            framesPerSecond = 0.0;

        m_d->l_statReadRate->setText(QString("%1 MB/s, %2 Frames/s, frameCheckEnabled=%3")
                                     .arg(mbPerSecond, 0, 'g', 4)
                                     .arg(framesPerSecond, 0, 'g', 4)
                                     .arg(m_d->dataReader->isStackFrameCheckEnabled())
                                     );

        m_d->prevReaderStats = stats;
        m_d->tLastUpdate = QDateTime::currentDateTime();
    });

    // Poll the read queue size for both pipes
    connect(updateTimer, &QTimer::timeout,
            this, [this] ()
    {
        u32 cmdQueueSize = 0;
        u32 dataQueueSize = 0;

        auto tStart = std::chrono::high_resolution_clock::now();

        m_d->mvlc->getReadQueueSize(Pipe::Command, cmdQueueSize);
        auto tCmd = std::chrono::high_resolution_clock::now();

        m_d->mvlc->getReadQueueSize(Pipe::Data, dataQueueSize);
        auto tData = std::chrono::high_resolution_clock::now();

        auto dtCmd = std::chrono::duration_cast<std::chrono::milliseconds>(tCmd - tStart);
        auto dtData = std::chrono::duration_cast<std::chrono::milliseconds>(tData - tStart);

        ui->le_usbCmdReadQueueSize->setText(QString::number(cmdQueueSize));
        ui->le_usbDataReadQueueSize->setText(QString::number(dataQueueSize));

        ui->label_queueSizePollTime->setText(
            QString("Cmd: %1ms, Data: %2ms, now=%3")
            .arg(dtCmd.count())
            .arg(dtData.count())
            .arg(QTime::currentTime().toString())
            );
    });

    updateTimer->start();

    // load default mvlcscript from resources
    {
        QFile input(":/mvlc/scripts/0-init-mtdcs.mvlcscript");
        input.open(QIODevice::ReadOnly);
        QTextStream inputStream(&input);
        ui->te_scriptInput->setPlainText(inputStream.readAll());
    }

    // Code to run on entering the event loop
    QTimer::singleShot(0, [this]() {
        this->raise(); // Raise this main window
        m_d->mvlc->connect();
    });
}

MVLCDevGUI::~MVLCDevGUI()
{
    m_d->dataReader->stop();
    m_d->readoutThread.quit();
    m_d->readoutThread.wait();
}

void MVLCDevGUI::logMessage(const QString &msg)
{
    emit sigLogMessage(msg);
}

void MVLCDevGUI::logBuffer(const QVector<u32> &buffer, const QString &info)
{
    QStringList strBuffer;
    strBuffer.reserve(buffer.size() + 2);

    strBuffer << QString(">>> %1, size=%2").arg(info).arg(buffer.size());

    for (int i = 0; i < buffer.size(); i++)
    {
        u32 value = buffer.at(i);

        auto str = QString("%1: 0x%2 (%3 dec)")
            .arg(i, 3)
            .arg(value, 8, 16, QLatin1Char('0'))
            .arg(value)
            ;

        if (is_known_buffer_header(value))
        {
            str += " " + decode_response_header(value);
        }

        strBuffer << str;
    }

    strBuffer << "<<< " + info;

    emit sigLogMessage(strBuffer.join("\n"));
}

//
// MVLCRegisterWidget
//

struct RegisterEditorWidgets
{
    QSpinBox *spin_address;

    QLineEdit *le_value;

    QLabel *l_readResult_hex,
           *l_readResult_dec;

    QPushButton *pb_write,
                *pb_read;
};

MVLCRegisterWidget::MVLCRegisterWidget(MVLCObject *mvlc, QWidget *parent)
    : QWidget(parent)
    , m_mvlc(mvlc)
{
    auto layout = new QGridLayout(this);
    int row = 0;

    layout->addWidget(new QLabel("Address"), row, 0);
    layout->addWidget(new QLabel("Value"), row, 1);
    layout->addWidget(new QLabel("Read Result"), row, 2);
    ++row;

    for (int editorIndex = 0; editorIndex < 3; ++editorIndex)
    {
        RegisterEditorWidgets widgets;
        widgets.spin_address = new QSpinBox(this);
        widgets.spin_address->setMinimumWidth(150);
        widgets.spin_address->setMinimum(0x0);
        widgets.spin_address->setMaximum(0xffff);
        widgets.spin_address->setSingleStep(2);
        widgets.spin_address->setDisplayIntegerBase(16);
        widgets.spin_address->setPrefix("0x");
        widgets.spin_address->setValue(0x1200 + 4 * editorIndex);

        widgets.le_value = new QLineEdit(this);
        widgets.l_readResult_hex = new QLabel(this);
        widgets.l_readResult_dec = new QLabel(this);
        widgets.l_readResult_hex->setMinimumWidth(60);

        for (auto label: {widgets.l_readResult_hex, widgets.l_readResult_dec})
        {
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        }

        widgets.pb_write = new QPushButton("Write", this);
        widgets.pb_read = new QPushButton("Read", this);

        auto resultLabelLayout = make_layout<QVBoxLayout, 0>();
        resultLabelLayout->addWidget(widgets.l_readResult_hex);
        resultLabelLayout->addWidget(widgets.l_readResult_dec);

        auto buttonLayout = make_layout<QVBoxLayout, 0>();
        buttonLayout->addWidget(widgets.pb_read);
        buttonLayout->addWidget(widgets.pb_write);

        layout->addWidget(widgets.spin_address, row, 0);
        layout->addWidget(widgets.le_value, row, 1);
        layout->addLayout(resultLabelLayout, row, 2);
        layout->addLayout(buttonLayout, row, 3);

        connect(widgets.pb_read, &QPushButton::clicked,
                this, [this, widgets] ()
        {
            bool ok = true;
            u16 address = widgets.spin_address->value();

            u32 result = readRegister(address);
            //widgets.le_value->setText(QString("0x%1").arg(result, 8, 16, QLatin1Char('0')));
            widgets.l_readResult_hex->setText(QString("0x%1").arg(result, 8, 16, QLatin1Char('0')));
            widgets.l_readResult_dec->setText(QString::number(result));
        });

        connect(widgets.pb_write, &QPushButton::clicked,
                this, [this, widgets] ()
        {
            bool ok = true;
            u16 address = widgets.spin_address->value();
            u32 value   = widgets.le_value->text().toUInt(&ok, 0);
            writeRegister(address, value);
        });

        ++row;
    }

    layout->addWidget(make_separator_frame(), row++, 0, 1, 4); // row- and colspan

    // Stack Info
    {
        auto spin_stackId = new QSpinBox();
        spin_stackId->setMinimum(0);
        spin_stackId->setMaximum(mvlc::stacks::StackCount - 1);

        auto pb_readStackInfo = new QPushButton("Read Info");

        auto l = new QHBoxLayout;
        l->addWidget(new QLabel("Stack Info"));
        l->addWidget(spin_stackId);
        l->addWidget(pb_readStackInfo);
        l->addStretch(1);
        layout->addLayout(l, row++, 0, 1, 4);

        connect(pb_readStackInfo, &QPushButton::clicked,
                this, [this, spin_stackId] ()
        {
            u8 stackId = static_cast<u8>(spin_stackId->value());
            readStackInfo(stackId);
        });

    }

    layout->addWidget(make_separator_frame(), row++, 0, 1, 4); // row- and colspan
    ++row;

    {
        struct RegAndLabel
        {
            u16 reg;
            const char *label;
        };

        // IP-Address Registers
        static const std::vector<RegAndLabel> IPData =
        {
            { 0x4400, "Own IP"},
            { 0x4408, "Own IP DHCP" },
            { 0x440C, "Dest IP Cmd" },
            { 0x4410, "Dest IP Data" },
        };

        auto gb = new QGroupBox("IP Address Settings");
        auto grid = make_layout<QGridLayout, 2, 4>(gb);

        static const int NumCols = 2;
        int gridRow = 0, gridCol = 0;

        for (const auto &ral: IPData)
        {
            auto ipRegWidget = new IPv4RegisterWidget(ral.reg);
            auto gb_inner = new QGroupBox(ral.label);
            auto gb_inner_layout = make_layout<QHBoxLayout>(gb_inner);
            gb_inner_layout->addWidget(ipRegWidget);

            grid->addWidget(gb_inner, gridRow, gridCol++);

            if (gridCol >= NumCols)
            {
                gridRow++;
                gridCol = 0;
            }

            connect(ipRegWidget, &IPv4RegisterWidget::write,
                    this, &MVLCRegisterWidget::writeRegister);

            connect(ipRegWidget, &IPv4RegisterWidget::read,
                    this, [this, ipRegWidget] (u16 reg)
            {
                u32 result = readRegister(reg);
                ipRegWidget->setRegisterValue(reg, result);
            });

            connect(ipRegWidget, &IPv4RegisterWidget::sigLogMessage,
                    this, &MVLCRegisterWidget::sigLogMessage);

        }

        // Dest Port Registers
        static const std::vector<RegAndLabel> PortData =
        {
            { 0x441A,  "Dest Port Cmd" },
            { 0x441C,  "Dest Port Data" },
        };

        gridCol = 0;

        for (const auto &ral: PortData)
        {
            auto le_input = new QLineEdit(this);
            auto pb_read = new QPushButton("Read", this);
            auto pb_write = new QPushButton("Write", this);

            auto gb_inner = new QGroupBox(ral.label, this);
            auto gb_inner_layout = make_layout<QGridLayout>(gb_inner);
            auto reg_str = QString("0x%1").arg(ral.reg, 4, 16, QLatin1Char('0'));
            gb_inner_layout->addWidget(new QLabel(reg_str), 0, 0, 2, 1);
            gb_inner_layout->addWidget(le_input, 0, 1, 2, 1);
            gb_inner_layout->addWidget(pb_read, 0, 2);
            gb_inner_layout->addWidget(pb_write, 1, 2);
            gb_inner_layout->setColumnStretch(1, 1);

            connect(pb_read, &QPushButton::clicked,
                    this, [this, ral, le_input] ()
            {
                u16 value = readRegister(ral.reg);
                le_input->setText(QString("%1").arg(value));
            });

            connect(pb_write, &QPushButton::clicked,
                    this, [this, ral, le_input] ()
            {
                u16 value = le_input->text().toUInt(nullptr, 0);
                writeRegister(ral.reg, value);
            });

            grid->addWidget(gb_inner, gridRow, gridCol++);
        }

        gridRow++;
        gridCol = 0;

        // Add the groupbox to the outer layout
        layout->addWidget(gb, row++, 0, 1, 4);
    }

    layout->setRowStretch(row, 1);
    layout->setColumnStretch(0, 1);
    layout->setColumnStretch(1, 1);
    layout->setColumnStretch(2, 1);
}

MVLCRegisterWidget::~MVLCRegisterWidget()
{}

void MVLCRegisterWidget::writeRegister(u16 address, u32 value)
{
    if (auto ec = m_mvlc->writeRegister(address, value))
        emit sigLogMessage(QString("Write Register Error: %1").arg(ec.message().c_str()));

    for (const auto &notification: m_mvlc->getStackErrorNotifications())
    {
        emit sigLogBuffer(notification, "Error notification from MVLC");
    }
}

u32 MVLCRegisterWidget::readRegister(u16 address)
{
    u32 value = 0u;
    if (auto ec = m_mvlc->readRegister(address, value))
        emit sigLogMessage(QString("Read Register Error: %1").arg(ec.message().c_str()));

    for (const auto &notification: m_mvlc->getStackErrorNotifications())
    {
        emit sigLogBuffer(notification, "Error notification from MVLC");
    }

    return value;
}

void MVLCRegisterWidget::readStackInfo(u8 stackId)
{
    assert(stackId < stacks::StackCount);

    u16 offsetRegister = stacks::Stack0OffsetRegister + stackId * AddressIncrement;
    u16 triggerRegister = stacks::Stack0TriggerRegister + stackId * AddressIncrement;

    u32 stackOffset = 0u;
    u32 stackTriggers = 0u;

    if (auto ec = m_mvlc->readRegister(offsetRegister, stackOffset))
    {
        emit sigLogMessage(QString("Read Stack Info Error: %1").arg(ec.message().c_str()));
        return;
    }

    stackOffset &= stacks::StackOffsetBitMaskWords;

    if (auto ec = m_mvlc->readRegister(triggerRegister, stackTriggers))
    {
        emit sigLogMessage(QString("Read Stack Info Error: %1").arg(ec.message().c_str()));
        return;
    }

    QStringList strings;
    strings.reserve(1024);

    strings << QString(">>> Info for stack %1").arg(static_cast<int>(stackId));
    strings << QString("  Offset:   0x%1 = 0x%2, %3 dec")
        .arg(offsetRegister, 4, 16, QLatin1Char('0'))
        .arg(stackOffset, 4, 16, QLatin1Char('0'))
        .arg(stackOffset);
    strings << QString("  Triggers: 0x%1 = 0x%2, %3 dec")
        .arg(triggerRegister, 4, 16, QLatin1Char('0'))
        .arg(stackTriggers, 4, 16, QLatin1Char('0'))
        .arg(stackTriggers);

    u16 reg = stacks::StackMemoryBegin + stackOffset;
    u32 stackHeader = 0u;

    if (auto ec = m_mvlc->readRegister(reg, stackHeader))
    {
        emit sigLogMessage(QString("Read Stack Info Error: %1").arg(ec.message().c_str()));
        return;
    }

    if ((stackHeader & 0xFF000000) != 0xF3000000)
    {
        strings << QString("    Invalid stack header @0x%1: 0x%2")
            .arg(reg, 4, 16, QLatin1Char('0'))
            .arg(stackHeader, 8, 16, QLatin1Char('0'));
    }
    else
    {
        strings << "  Stack Contents:";

        static const int StackMaxSize = 128;
        int stackSize = 0;

        while (stackSize <= StackMaxSize && reg < stacks::StackMemoryEnd)
        {
            u32 value = 0u;
            if (auto ec = m_mvlc->readRegister(reg, value))
            {
                emit sigLogMessage(QString("Read Stack Info Error: %1")
                                   .arg(ec.message().c_str()));
                return;
            }

            strings << QString("   [0x%4, %3] 0x%1: 0x%2")
                .arg(reg, 4, 16, QLatin1Char('0'))
                .arg(value, 8, 16, QLatin1Char('0'))
                .arg(stackSize, 3)
                .arg(stackSize, 3, 16, QLatin1Char('0'))
                ;

            if ((value & 0xFF000000) == 0xF4000000)
            {
                break;
            }

            reg += AddressIncrement;
            stackSize++;
        }
    }

    strings << QString("<<< End stack %1 info").arg(static_cast<int>(stackId));

    emit sigLogMessage(strings.join("\n"));

    for (const auto &notification: m_mvlc->getStackErrorNotifications())
    {
        emit sigLogBuffer(notification, "Error notification from MVLC");
    }
}

//
// LogWidget
//
LogWidget::LogWidget(QWidget *parent)
    : QWidget(parent)
    , te_log(new QPlainTextEdit(this))
    , pb_clearLog(new QPushButton("Clear", this))
{
    setWindowTitle("MVLC Dev Tool Log Window");
    auto font = make_monospace_font();
    font.setPointSize(8);
    te_log->setFont(font);

    auto bottomLayout = make_layout<QHBoxLayout>();
    bottomLayout->addWidget(pb_clearLog);
    bottomLayout->addStretch(1);

    auto widgetLayout = make_layout<QVBoxLayout>(this);
    widgetLayout->addWidget(te_log);
    widgetLayout->addLayout(bottomLayout);
    widgetLayout->setStretch(0, 1);

    connect(pb_clearLog, &QPushButton::clicked,
            this, &LogWidget::clearLog);
}

LogWidget::~LogWidget()
{
}

void LogWidget::logMessage(const QString &msg)
{
    te_log->appendPlainText(msg);
    auto bar = te_log->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void LogWidget::clearLog()
{
    te_log->clear();
}

//
// IPv4RegisterWidget
//
IPv4RegisterWidget::IPv4RegisterWidget(u16 regLo, const QString &regName, QWidget *parent)
    : IPv4RegisterWidget(regLo, regLo + sizeof(u16), regName, parent)
{}

IPv4RegisterWidget::IPv4RegisterWidget(u16 regLo, u16 regHi, const QString &regName, QWidget *parent)
    : QWidget(parent)
    , m_regLo(regLo)
    , m_regHi(regHi)
    , le_valLo(new QLineEdit(this))
    , le_valHi(new QLineEdit(this))
    , le_addressInput(new QLineEdit(this))
{
    auto l_regLo = new QLabel(QSL("0x%1").arg(m_regLo, 4, 16, QLatin1Char('0')));
    auto l_regHi = new QLabel(QSL("0x%1").arg(m_regHi, 4, 16, QLatin1Char('0')));
    auto pb_read = new QPushButton(QSL("Read"));
    auto pb_write = new QPushButton(QSL("Write"));

    for (auto le: {le_valLo, le_valHi})
    {
        auto pal = le->palette();
        pal.setColor(QPalette::Base, QSL("#efebe7"));
        le->setPalette(pal);
        le->setReadOnly(true);
    }

    auto layout = new QGridLayout(this);

    int col = 0;

    if (!regName.isEmpty())
        layout->addWidget(new QLabel(regName), 0, col++, 2, 1);

    layout->addWidget(l_regLo, 0, col);
    layout->addWidget(l_regHi, 1, col++);
    layout->addWidget(le_valLo, 0, col);
    layout->addWidget(le_valHi, 1, col++);
    layout->addWidget(le_addressInput, 0, col, 2, 1);
    layout->setColumnStretch(col++, 1);
    layout->addWidget(pb_read, 0, col);
    layout->addWidget(pb_write, 1, col++);

    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);

    connect(pb_read, &QPushButton::clicked,
            this, [this] ()
    {
        emit read(m_regLo);
        emit read(m_regHi);
    });

    connect(pb_write, &QPushButton::clicked,
            this, [this] ()
    {
        // - take input from le_addressInput
        // - convert to 32-bit value either by numeric conversion or by parsing
        //   IPv4 notation
        // - split into hi and lo parts
        // - emit write for both parts with the corresponding register address

        static const QRegularExpression re(
            R"(^([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})$)");
        auto input = le_addressInput->text();

        auto match = re.match(input);

        u32 ipAddressValue = 0u;

        if (match.hasMatch())
        {
            for (int i=1; i<=4; i++)
            {
                u32 part = match.captured(i).toUInt();
                qDebug() << "i=" << i << "part=" << part;
                ipAddressValue <<= 8;
                ipAddressValue |= part;
            }
        }
        else
        {
            bool ok = false;
            ipAddressValue = input.toUInt(&ok, 0);

            if (!ok)
            {
                emit sigLogMessage("Invalid IP address entered");
                return;
            }
        }

        u16 loPart = (ipAddressValue >>  0) & 0xffff;
        u16 hiPart = (ipAddressValue >> 16) & 0xffff;

        emit sigLogMessage(QString("Parsed IP Address: %1, setting hi=0x%2, lo=0x%3")
                           .arg(format_ipv4(ipAddressValue))
                           .arg(hiPart, 4, 16, QLatin1Char('0'))
                           .arg(loPart, 4, 16, QLatin1Char('0'))
                           );

        le_valLo->clear();
        le_valHi->clear();

        emit write(m_regLo, loPart);
        emit write(m_regHi, hiPart);
    });
}

void IPv4RegisterWidget::setRegisterValue(u16 reg, u16 value)
{
    QLineEdit *le_val = nullptr;

    if (reg == m_regLo)
        le_val = le_valLo;
    else if (reg == m_regHi)
        le_val = le_valHi;
    else
        return;

    le_val->setText(QString("0x%1").arg(value, 4, 16, QLatin1Char('0')));

    u32 loPart = le_valLo->text().toUInt(nullptr, 0);
    u32 hiPart = le_valHi->text().toUInt(nullptr, 0);
    u32 ipAddressValue = (hiPart << 16) | loPart;

    le_addressInput->setText(format_ipv4(ipAddressValue));
}

QString format_ipv4(u32 address)
{
    return QString("%1.%2.%3.%4")
        .arg((address >> 24) & 0xFF)
        .arg((address >> 16) & 0xFF)
        .arg((address >>  8) & 0xFF)
        .arg((address >>  0) & 0xFF);
}
