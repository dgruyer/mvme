#include "mvlc_readout_worker.h"

#include <QCoreApplication>
#include <QtConcurrent>
#include <QThread>

#include <quazipfile.h>
#include <quazip.h>

#include "mvlc/mvlc_error.h"
#include "mvlc/mvlc_vme_controller.h"
#include "mvlc/mvlc_util.h"
#include "mvlc/mvlc_impl_usb.h"
#include "mvlc/mvlc_impl_eth.h"
#include "mvlc_daq.h"
#include "vme_analysis_common.h"

// =========================
//    MVLC readout outline
// =========================
//
// * Two different formats depending on connection type.
// * Pass only complete frames around. For readout the detection has to be done
//   anyways so that system frames can be properly inserted.
// * Do not try to hit exactly 1s between SoftwareTimeticks. This will
//   complicate the code a lot and is not really needed if some form of timestamp
//   and/or duration is stored in the timetick event.
//
//
// ETH
// -------------------------
// Small packets of 1500 or 8192 bytes. Two header words for packet loss detection
// and handling (resume processing after loss).
//
// - Strategy
//
//   1) start with a fresh buffer
//
//   2) while free space in buffer > 8k:
//     read packet and append to buffer
//     if (flush timeout elapsed)
//         flush buffer
//     if (time for timetick)
//         insert timetick frame
//
//   3) flush buffer
//
// => Inserting system frames is allowed at any point.
//
// - Replay from file:
//   Read any amount of data from file into memory. If a word is not a system
//   frame then it must be header0() of a previously received packet. Follow
//   the header framing via the header0::NumDataWords value. This way either
//   end up on the next header0() or at the start of a system frame.
//   If part of a packet is at the end of the buffer read from disk store the part
//   temporarily and truncate the buffer. Then when doing the next read add the
//   partial packet to the front fo the new buffer.
//   -> Packet boundaries can be restored and it can be guaranteed that only full
//   packets worth of data are passed internally.
//
//
// USB
// -------------------------
// Stream of data. Reads do not coincide with buffer framing. The exception is the
// very first read which starts with an 0xF3 frame.
// To be able to insert system frames (e.g. timeticks) and to make the analysis
// easier to write, internal buffers must contain complete frames only. To make
// this work the readout code has to follow the 0xF3 data framing. Extract the
// length to be able to jump to the next frame start. Store partial data at the
// end and truncate the buffer before flushing it.
//
// - Replay:
//   Starts with a system or a readout frame. Follow frame structure doing
//   truncation and copy of partial frames.
//
// Note: max amount to copy is the max length of a frame. That's 2^13 words
// (32k bytes) for readout frames.

    // TODO: where to handle error notifications? The GUI should display their
    // rates by stack somewhere.
    // Use the notification poller somewhere outside of this readout worker and
    // display the counts somewhere during a readout.
#if 0
    connect(mvlc, &MVLC_VMEController::stackErrorNotification,
            this, [this] (const QVector<u32> &notification)
    {
        bool wasLogged = logMessage(QSL("Stack Error Notification from MVLC (size=%1)")
                                    .arg(notification.size()), true);

        if (!wasLogged)
            return;

        for (const auto &word: notification)
        {
            logMessage(QSL("  0x%1").arg(word, 8, 16, QLatin1Char('0')),
                       false);
        }

        logMessage(QSL("End of Stack Error Notification"), false);
    });
#endif


using namespace mesytec::mvlc;

static const size_t LocalEventBufferSize = Megabytes(1);
static const size_t ReadBufferSize = Megabytes(1);

namespace
{

struct ListfileOutput
{
    QString outFilename;
    std::unique_ptr<QuaZip> archive;
    std::unique_ptr<QIODevice> outdev;

    inline bool isOpen()
    {
        return outdev && outdev->isOpen();
    }
};

struct listfile_write_error: public std::runtime_error
{
    listfile_write_error(const char *arg): std::runtime_error(arg) {}
    listfile_write_error(): std::runtime_error("listfile_write_error") {}
};

ListfileOutput listfile_open(ListFileOutputInfo &outinfo,
                             std::function<void (const QString &)> logger)
{
    ListfileOutput result;

    if (!outinfo.enabled)
        return result;

    if (outinfo.fullDirectory.isEmpty())
        throw QString("Error: listfile output directory is not set");

    result.outFilename = make_new_listfile_name(&outinfo);

    switch (outinfo.format)
    {
        case ListFileFormat::Plain:
            {
                result.outdev = std::make_unique<QFile>(result.outFilename);
                auto outFile = reinterpret_cast<QFile *>(result.outdev.get());

                logger(QString("Writing to listfile %1").arg(result.outFilename));

                if (!outFile->open(QIODevice::WriteOnly))
                {
                    throw QString("Error opening listFile %1 for writing: %2")
                        .arg(outFile->fileName())
                        .arg(outFile->errorString())
                        ;
                }

            } break;

        case ListFileFormat::ZIP:
            {
                /* The name of the listfile inside the zip archive. */
                QFileInfo fi(result.outFilename);
                QString listfileFilename(QFileInfo(result.outFilename).completeBaseName());
                listfileFilename += QSL(".mvmelst");

                result.archive = std::make_unique<QuaZip>();
                result.archive->setZipName(result.outFilename);
                result.archive->setZip64Enabled(true);

                logger(QString("Writing listfile into %1").arg(result.outFilename));

                if (!result.archive->open(QuaZip::mdCreate))
                {
                    throw make_zip_error(result.archive->getZipName(), *result.archive);
                }

                result.outdev = std::make_unique<QuaZipFile>(result.archive.get());
                auto outFile = reinterpret_cast<QuaZipFile *>(result.outdev.get());

                QuaZipNewInfo zipFileInfo(listfileFilename);
                zipFileInfo.setPermissions(static_cast<QFile::Permissions>(0x6644));

                bool res = outFile->open(QIODevice::WriteOnly, zipFileInfo,
                                         // password, crc
                                         nullptr, 0,
                                         // method (Z_DEFLATED or 0 for no compression)
                                         Z_DEFLATED,
                                         // level
                                         outinfo.compressionLevel
                                        );

                if (!res)
                {
                    result.outdev.reset();
                    throw make_zip_error(result.archive->getZipName(), *result.archive);
                }
            } break;

        case ListFileFormat::Invalid:
            assert(false);
    }

    return result;
}

void listfile_close(ListfileOutput &lf_out)
{
    if (!lf_out.isOpen())
        return;

    lf_out.outdev->close();

    if (lf_out.archive)
    {
        lf_out.archive->close();

        if (lf_out.archive->getZipError() != UNZ_OK)
            throw make_zip_error(lf_out.archive->getZipName(), *lf_out.archive.get());
    }
}

inline void listfile_write_raw(ListfileOutput &lf_out, const char *buffer, size_t size)
{
    if (!lf_out.isOpen())
        return;

    if (lf_out.outdev->write(buffer, size) != static_cast<qint64>(size))
        throw listfile_write_error();
}

inline void listfile_write_raw(ListfileOutput &lf_out, const u8 *buffer, size_t size)
{
    listfile_write_raw(lf_out, reinterpret_cast<const char *>(buffer), size);
}

void listfile_write_magic(ListfileOutput &lf_out, const MVLCObject &mvlc)
{
    const char *magic = nullptr;

    switch (mvlc.connectionType())
    {
        case ConnectionType::ETH:
            magic = "MVLC_ETH";
            break;

        case ConnectionType::USB:
            magic = "MVLC_USB";
            break;
    }

    listfile_write_raw(lf_out, magic, std::strlen(magic));
}

void listfile_write_system_sections(ListfileOutput &lf_out, u8 subtype, const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;

    if (bytes.size() % sizeof(u32))
        throw listfile_write_error("unpadded system section data");

    unsigned totalWords   = bytes.size() / sizeof(u32);

    const u32 *buffp = reinterpret_cast<const u32 *>(bytes.data());
    const u32 *endp  = buffp + totalWords;

    while (buffp < endp)
    {
        unsigned wordsLeft = endp - buffp;
        unsigned wordsInSection = std::min(
            wordsLeft, static_cast<unsigned>(system_event::LengthMask));

        bool isLastSection = (wordsInSection == wordsLeft);

        u32 sectionHeader = (frame_headers::SystemEvent << frame_headers::TypeShift)
            | ((subtype & system_event::SubTypeMask) << system_event::SubTypeShift);

        if (!isLastSection)
            sectionHeader |= 0b1 << system_event::ContinueShift;

        sectionHeader |= (wordsInSection & system_event::LengthMask) << system_event::LengthShift;

        listfile_write_raw(lf_out, reinterpret_cast<const u8 *>(&sectionHeader),
                           sizeof(sectionHeader));

        listfile_write_raw(lf_out, reinterpret_cast<const u8 *>(buffp),
                           wordsInSection * sizeof(u32));

        buffp += wordsInSection;
    }

    assert(buffp == endp);
}

void listfile_write_vme_config(ListfileOutput &lf_out, const VMEConfig &vmeConfig)
{
    if (!lf_out.isOpen())
        return;

    QJsonObject json;
    vmeConfig.write(json);
    QJsonObject parentJson;
    parentJson["VMEConfig"] = json;
    QJsonDocument doc(parentJson);
    QByteArray bytes(doc.toJson());

    // Pad using spaces. The Qt JSON parser will handle this without error when
    // reading it back.
    while (bytes.size() % sizeof(u32))
        bytes.append(' ');

    listfile_write_system_sections(lf_out, system_event::VMEConfig, bytes);
}

void listfile_write_preamble(ListfileOutput &lf_out, const MVLCObject &mvlc,
                             const VMEConfig &vmeConfig)
{
    listfile_write_magic(lf_out, mvlc);
    listfile_write_vme_config(lf_out, vmeConfig);
}

} // end anon namespace

struct MVLCReadoutWorker::Private
{
    MVLCReadoutWorker *q = nullptr;;

    // lots of mvlc api layers
    MVLC_VMEController *mvlcCtrl = nullptr;;
    MVLCObject *mvlcObj = nullptr;;
    eth::Impl *mvlc_eth = nullptr;;
    usb::Impl *mvlc_usb = nullptr;;
    ListfileOutput listfileOut;
    u32 nextOutputBufferNumber = 1u;;

    void preRunClear()
    {
        nextOutputBufferNumber = 1u;
    }
};

MVLCReadoutWorker::MVLCReadoutWorker(QObject *parent)
    : VMEReadoutWorker(parent)
    , d(std::make_unique<Private>())
    , m_state(DAQState::Idle)
    , m_desiredState(DAQState::Idle)
    , m_readBuffer(ReadBufferSize)
    , m_previousData(ReadBufferSize)
    , m_localEventBuffer(LocalEventBufferSize)
{
    d->q = this;
}

MVLCReadoutWorker::~MVLCReadoutWorker()
{
}

void MVLCReadoutWorker::start(quint32 cycles)
{
    if (m_state != DAQState::Idle)
    {
        logMessage("Readout state != Idle, aborting startup");
        return;
    }

    // Setup the Private struct members. All layers of the MVLC impl are used
    // in here: MVLC_VMEController to execute vme scripts, MVLCObject to setup
    // stacks and triggers and the low level implementations for fast
    // packet(ET)/buffer(USB) reads.
    d->mvlcCtrl = qobject_cast<MVLC_VMEController *>(getContext().controller);

    if (!d->mvlcCtrl)
    {
        logMessage("MVLC controller required");
        InvalidCodePath;
        return;
    }

    d->mvlcObj = d->mvlcCtrl->getMVLCObject();

    switch (d->mvlcObj->connectionType())
    {
        case ConnectionType::ETH:
            d->mvlc_eth = reinterpret_cast<eth::Impl *>(d->mvlcObj->getImpl());
            d->mvlc_usb = nullptr;
            break;

        case ConnectionType::USB:
            d->mvlc_eth = nullptr;
            d->mvlc_usb = reinterpret_cast<usb::Impl *>(d->mvlcObj->getImpl());
            break;
    }

    m_cyclesToRun = cycles;
    m_logBuffers = (cycles > 0); // log buffers to GUI if number of cycles has been passed in

    try
    {
        auto logger = [this](const QString &msg)
        {
            this->logMessage(msg);
        };

        setState(DAQState::Starting);

        // vme init sequence
        auto results = vme_daq_init(getContext().vmeConfig, d->mvlcCtrl, logger);
        log_errors(results, logger);

        if (d->mvlc_eth)
        {
            logMessage(QSL("MVLC connection type is UDP. Sending initial empty request"
                           " via the data socket."));

            static const std::array<u32, 2> EmptyRequest = { 0xF1000000, 0xF2000000 };
            size_t bytesTransferred = 0;

            if (auto ec = d->mvlcObj->write(
                    Pipe::Data,
                    reinterpret_cast<const u8 *>(EmptyRequest.data()),
                    EmptyRequest.size() * sizeof(u32),
                    bytesTransferred))
            {
                throw ec;
            }
        }

        logMessage("Initializing MVLC");

        // Stack and trigger setup. Triggers are enabled immediately, this
        // means data will start coming in right away.
        if (auto ec = setup_mvlc(*d->mvlcObj, *getContext().vmeConfig, logger))
            throw ec;

        // listfile handling
        d->listfileOut = listfile_open(*m_workerContext.listfileOutputInfo, logger);
        listfile_write_preamble(d->listfileOut, *d->mvlcObj, *m_workerContext.vmeConfig);
        m_workerContext.daqStats->listfileFilename = d->listfileOut.outFilename;

        d->preRunClear();

        logMessage("");
        logMessage(QSL("Entering readout loop"));
        m_workerContext.daqStats->start();

        readoutLoop();

        logMessage(QSL("Leaving readout loop"));
        logMessage(QSL(""));

        vme_daq_shutdown(getContext().vmeConfig, d->mvlcCtrl, logger);

        // Note: endRun() collects the log contents, which means it should be one of the
        // last actions happening in here. Log messages generated after this point won't
        // show up in the listfile.
        //m_listfileHelper->endRun();
        m_workerContext.daqStats->stop();
    }
    catch (const std::error_code &ec)
    {
        logError(ec.message().c_str());
    }
    catch (const std::runtime_error &e)
    {
        logError(e.what());
    }
    catch (const VMEError &e)
    {
        logError(e.toString());
    }
    catch (const QString &e)
    {
        logError(e);
    }
    catch (const vme_script::ParseError &e)
    {
        logError(QSL("VME Script parse error: ") + e.what());
    }

    setState(DAQState::Idle);
}

void MVLCReadoutWorker::readoutLoop()
{
    using vme_analysis_common::TimetickGenerator;

    setState(DAQState::Running);

    TimetickGenerator timetickGen;
    //m_listfileHelper->writeTimetickSection();

    while (true)
    {
        int elapsedSeconds = timetickGen.generateElapsedSeconds();

        while (elapsedSeconds >= 1)
        {
            //m_listfileHelper->writeTimetickSection();
            elapsedSeconds--;
        }

        // stay in running state
        if (likely(m_state == DAQState::Running && m_desiredState == DAQState::Running))
        {
            size_t bytesTransferred = 0u;
            auto ec = readAndProcessBuffer(bytesTransferred);

            if (ec == ErrorType::ConnectionError)
            {
                logMessage(QSL("Lost connection to MVLC. Leaving readout loop. Error=%1")
                           .arg(ec.message().c_str()));
                break;
            }

            if (unlikely(m_cyclesToRun > 0))
            {
                if (m_cyclesToRun == 1)
                {
                    break;
                }
                m_cyclesToRun--;
            }
        }
        // pause
        else if (m_state == DAQState::Running && m_desiredState == DAQState::Paused)
        {
            pauseDAQ();
        }
        // resume
        else if (m_state == DAQState::Paused && m_desiredState == DAQState::Running)
        {
            resumeDAQ();
        }
        // stop
        else if (m_desiredState == DAQState::Stopping)
        {
            logMessage(QSL("MVLC readout stopping"));
            break;
        }
        // paused
        else if (m_state == DAQState::Paused)
        {
            static const unsigned PauseSleepDuration_ms = 100;
            QThread::msleep(PauseSleepDuration_ms);
        }
        else
        {
            InvalidCodePath;
        }
    }

    setState(DAQState::Stopping);

    // Do two things in parallel: disable the triggers while also reading and
    // processing data from the data pipe.
    auto f = QtConcurrent::run([this]()
    {
        return disable_all_triggers(*d->mvlcObj);
    });

    size_t bytesTransferred = 0u;
    // FIXME: the code can hang here forever if disabling the readout triggers
    // does not work.  Measure total time spent in the loop and break out
    // after a threshold has been reached.
    do
    {
        readAndProcessBuffer(bytesTransferred);
    } while (bytesTransferred > 0);

    maybePutBackBuffer();

    if (auto ec = f.result())
    {
        logMessage(QSL("MVLC Readout: Error disabling triggers: %1")
                   .arg(ec.message().c_str()));
    }

    qDebug() << __PRETTY_FUNCTION__ << "at end";
}

std::error_code MVLCReadoutWorker::readAndProcessBuffer(size_t &bytesTransferred)
{
    // TODO: rename this once I can come up with a better name. depending on
    // the connection type and the amount of incoming data, etc. this does
    // different things.

    // Return ConnectionError if the whole readout should be aborted.
    // Other error codes do canceling of the run.
    // If no data was read within some interval that fact should be logged.
    //
    // What this should do:
    // grab a fresh output buffer
    // read into that buffer until either the buffer is full and can be flushed
    // or a certain time has passed and we want to flush a buffer to stay
    // responsive (the low data rate case).
    // If the format needs it do perform consistency checks on the incoming data.
    // For usb: follow the F3 framing.

    bytesTransferred = 0u;
    std::error_code ec;

    if (d->mvlc_eth)
        ec = readout_eth(bytesTransferred);
    else
        ec = readout_usb(bytesTransferred);

    if (getOutputBuffer()->used > 0)
        flushCurrentOutputBuffer();

    return ec;
}

/* Steps for the ETH readout:
 * get  buffer
 * fill  buffer until full or flush timeout elapsed
 * flush buffer
 */
// Tunable. Effects time to stop/pause and analysis buffer fill-level/count.
// 1000/FlushBufferTimeout is the minimum number of buffers the analysis will
// get per second assuming that we receive any data at all and that the
// analysis can keep up.
// If set too low buffers won't be completely filled even at high data rates
// and queue load will increase.
static const std::chrono::milliseconds FlushBufferTimeout(500);

std::error_code MVLCReadoutWorker::readout_eth(size_t &totalBytesTransferred)
{
    assert(d->mvlc_eth);

    auto destBuffer = getOutputBuffer();
    auto tStart = std::chrono::steady_clock::now();
    auto &mvlcLocks = d->mvlcObj->getLocks();
    auto &daqStats = m_workerContext.daqStats;

    while (destBuffer->free() >= eth::JumboFrameMaxSize)
    {
        size_t bytesTransferred = 0u;

        auto dataGuard = mvlcLocks.lockData();
        auto result = d->mvlc_eth->read_packet(
            Pipe::Data, destBuffer->asU8(), destBuffer->free());
        dataGuard.unlock();

        daqStats->totalBytesRead += result.bytesTransferred;

        // ShortRead means that the received packet length was non-zero but
        // shorter than the two ETH header words. Overwrite this short data on
        // the next iteration so that the framing structure stays intact.
        // Also do not count these short reads in totalBytesTransferred as that
        // would suggest we actually did receive valid data.
        if (result.ec == MVLCErrorCode::ShortRead)
        {
            daqStats->buffersWithErrors++;
            continue;
        }

        destBuffer->used += result.bytesTransferred;
        totalBytesTransferred += result.bytesTransferred;

        // A crude way of handling packets with residual bytes at the end. Just
        // subtract the residue from buffer->used which means the residual
        // bytes will be overwritten by the next packets data. This will at
        // least keep the structure somewhat intact assuming that the
        // dataWordCount in header0 is correct. Note that this case does not
        // happen, the MVLC never generates packets with residual bytes.
        if (unlikely(result.leftoverBytes()))
            destBuffer->used -= result.leftoverBytes();

        auto elapsed = std::chrono::steady_clock::now() - tStart;

        if (elapsed >= FlushBufferTimeout)
            break;
    }

    return {};
}

// TODO: perform checks for frame header validity. allow StackFrame,
// StackContinuation and SystemEvent.
// If the check fails perform a recovery procedure by trying if the chaining of
// N (2 to 3?) possible frame headers is ok. In this case assume that we're in
// a good state again.
inline void fixup_usb_buffer(DataBuffer &readBuffer, DataBuffer &tempBuffer)
{
    BufferIterator iter(readBuffer.data, readBuffer.used);

    auto move_bytes = [] (DataBuffer &sourceBuffer, DataBuffer &destBuffer,
                          const u8 *sourceBegin, size_t bytes)
    {
        assert(sourceBegin >= sourceBuffer.data);
        assert(sourceBegin + bytes <= sourceBuffer.endPtr());

        destBuffer.ensureCapacity(bytes);
        std::memcpy(destBuffer.endPtr(), sourceBegin, bytes);
        destBuffer.used   += bytes;
        sourceBuffer.used -= bytes;
    };

    while (!iter.atEnd())
    {
        if (iter.longwordsLeft())
        {
            // Can extract and check the next frame header
            u32 frameHeader = iter.peekU32();
            auto frameInfo = extract_frame_info(frameHeader);

            // Check if the full frame is in the readBuffer. If not move the
            // trailing data to the tempBuffer.
            if (frameInfo.len + 1u > iter.longwordsLeft())
            {
                auto trailingBytes = iter.bytesLeft();
                move_bytes(readBuffer, tempBuffer, iter.asU8(), trailingBytes);
                return;
            }

            // Skip over the frameHeader and the frame contents.
            iter.skip(frameInfo.len + 1, sizeof(u32));
        }
        else
        {
            // Not enough data left to get the next frame header. Move trailing
            // bytes to the tempBuffer.
            auto trailingBytes = iter.bytesLeft();
            move_bytes(readBuffer, tempBuffer, iter.asU8(), trailingBytes);
            return;
        }
    }
}

static const size_t USBReadMinBytes = Kilobytes(256);

std::error_code MVLCReadoutWorker::readout_usb(size_t &totalBytesTransferred)
{
    assert(d->mvlc_usb);

    auto destBuffer = getOutputBuffer();
    auto tStart = std::chrono::steady_clock::now();
    auto &mvlcLocks = d->mvlcObj->getLocks();
    auto &daqStats = m_workerContext.daqStats;
    std::error_code ec;

    if (m_previousData.used)
    {
        destBuffer->ensureCapacity(m_previousData.used);
        std::memcpy(destBuffer->endPtr(), m_previousData.data, m_previousData.used);
        destBuffer->used += m_previousData.used;
        m_previousData.used = 0u;
    }

    while (destBuffer->free() >= USBReadMinBytes)
    {
        size_t bytesTransferred = 0u;

        auto dataGuard = mvlcLocks.lockData();
        ec = d->mvlc_usb->read_unbuffered(
            Pipe::Data, destBuffer->asU8(), destBuffer->free(), bytesTransferred);
        dataGuard.unlock();

        if (ec == ErrorType::ConnectionError)
            break;

        daqStats->totalBytesRead += bytesTransferred;
        destBuffer->used += bytesTransferred;
        totalBytesTransferred += bytesTransferred;

        auto elapsed = std::chrono::steady_clock::now() - tStart;

        if (elapsed >= FlushBufferTimeout)
            break;
    }

    fixup_usb_buffer(*destBuffer, m_previousData);

    return ec;
}

DataBuffer *MVLCReadoutWorker::getOutputBuffer()
{
    DataBuffer *outputBuffer = m_outputBuffer;

    if (!outputBuffer)
    {
        outputBuffer = dequeue(m_workerContext.freeBuffers);

        if (!outputBuffer)
        {
            outputBuffer = &m_localEventBuffer;
        }

        // Reset a fresh buffer
        outputBuffer->used = 0;
        outputBuffer->id   = d->nextOutputBufferNumber++;
        outputBuffer->tag  = static_cast<int>((d->mvlc_eth
                                               ? DataBufferFormatTags::MVLC_ETH
                                               : DataBufferFormatTags::MVLC_USB));
        m_outputBuffer = outputBuffer;
    }

    return outputBuffer;
}

void MVLCReadoutWorker::maybePutBackBuffer()
{
    if (m_outputBuffer && m_outputBuffer != &m_localEventBuffer)
    {
        // put the buffer back onto the free queue
        enqueue(m_workerContext.freeBuffers, m_outputBuffer);
    }

    m_outputBuffer = nullptr;
}

void MVLCReadoutWorker::flushCurrentOutputBuffer()
{
    auto outputBuffer = m_outputBuffer;

    if (outputBuffer)
    {
        m_workerContext.daqStats->totalBuffersRead++;

        if (d->listfileOut.outdev)
        {
            // write to listfile
            qint64 bytesWritten = d->listfileOut.outdev->write(
                reinterpret_cast<const char *>(outputBuffer->data),
                outputBuffer->used);

            if (bytesWritten != static_cast<qint64>(outputBuffer->used))
                throw_io_device_error(d->listfileOut.outdev);

            m_workerContext.daqStats->listFileBytesWritten += bytesWritten;
        }

        if (outputBuffer != &m_localEventBuffer)
        {
            enqueue_and_wakeOne(m_workerContext.fullBuffers, outputBuffer);
        }
        else
        {
            m_workerContext.daqStats->droppedBuffers++;
        }
        m_outputBuffer = nullptr;
    }
}

void MVLCReadoutWorker::pauseDAQ()
{
    auto f = QtConcurrent::run([this]()
    {
        return disable_all_triggers(*d->mvlcObj);
    });

    size_t bytesTransferred = 0u;

    do
    {
        readAndProcessBuffer(bytesTransferred);
    } while (bytesTransferred > 0);


    //m_listfileHelper->writePauseSection();

    setState(DAQState::Paused);
    logMessage(QString(QSL("MVLC readout paused")));
}

void MVLCReadoutWorker::resumeDAQ()
{
    auto mvlc = qobject_cast<MVLC_VMEController *>(getContext().controller);
    assert(mvlc);

    enable_triggers(*mvlc->getMVLCObject(), *getContext().vmeConfig);

    //m_listfileHelper->writeResumeSection();

    setState(DAQState::Running);
    logMessage(QSL("MVLC readout resumed"));
}

void MVLCReadoutWorker::stop()
{
    if (m_state == DAQState::Running || m_state == DAQState::Paused)
        m_desiredState = DAQState::Stopping;
}

void MVLCReadoutWorker::pause()
{
    if (m_state == DAQState::Running)
        m_desiredState = DAQState::Paused;
}

void MVLCReadoutWorker::resume(quint32 cycles)
{
    if (m_state == DAQState::Paused)
    {
        m_cyclesToRun = cycles;
        m_logBuffers = (cycles > 0); // log buffers to GUI if number of cycles has been passed in
        m_desiredState = DAQState::Running;
    }
}

bool MVLCReadoutWorker::isRunning() const
{
    return m_state != DAQState::Idle;
}

void MVLCReadoutWorker::setState(const DAQState &state)
{
    qDebug() << __PRETTY_FUNCTION__ << DAQStateStrings[m_state] << "->" << DAQStateStrings[state];
    m_state = state;
    m_desiredState = state;
    emit stateChanged(state);

    switch (state)
    {
        case DAQState::Idle:
            emit daqStopped();
            break;

        case DAQState::Paused:
            emit daqPaused();
            break;

        case DAQState::Starting:
        case DAQState::Stopping:
            break;

        case DAQState::Running:
            emit daqStarted();
    }

    QCoreApplication::processEvents();
}

DAQState MVLCReadoutWorker::getState() const
{
    return m_state;
}

void MVLCReadoutWorker::logError(const QString &msg)
{
    logMessage(QSL("MVLC Readout Error: %1").arg(msg));
}
