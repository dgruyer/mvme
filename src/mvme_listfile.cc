#include "mvme_listfile.h"
#include "globals.h"
#include "mvme_config.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonObject>
#include <QtMath>

using namespace listfile;

void dump_mvme_buffer(QTextStream &out, const DataBuffer *eventBuffer, bool dumpData)
{
    QString buf;
    BufferIterator iter(eventBuffer->data, eventBuffer->used, BufferIterator::Align32);

    while (iter.longwordsLeft())
    {
        u32 sectionHeader = iter.extractU32();
        int sectionType   = (sectionHeader & SectionTypeMask) >> SectionTypeShift;
        u32 sectionSize   = (sectionHeader & SectionSizeMask) >> SectionSizeShift;

        out << "eventBuffer: " << eventBuffer << ", used=" << eventBuffer->used
            << ", size=" << eventBuffer->size
            << endl;
        out << buf.sprintf("sectionHeader=0x%08x, sectionType=%d, sectionSize=%u\n",
                           sectionHeader, sectionType, sectionSize);

        switch (sectionType)
        {
            case SectionType_Config:
                {
                    out << "Config section of size " << sectionSize << endl;
                    iter.skip(sectionSize * sizeof(u32));
                } break;

            case SectionType_Event:
                {
                    u32 eventType = (sectionHeader & EventTypeMask) >> EventTypeShift;
                    out << buf.sprintf("Event section: eventHeader=0x%08x, eventType=%d, eventSize=%u\n",
                                       sectionHeader, eventType, sectionSize);

                    u32 wordsLeft = sectionSize;

                    while (wordsLeft > 1)
                    {
                        u32 subEventHeader = iter.extractU32();
                        --wordsLeft;
                        u32 moduleType = (subEventHeader & ModuleTypeMask) >> ModuleTypeShift;
                        u32 subEventSize = (subEventHeader & SubEventSizeMask) >> SubEventSizeShift;
                        QString moduleString = VMEModuleShortNames.value(static_cast<VMEModuleType>(moduleType), "unknown");

                        out << buf.sprintf("  subEventHeader=0x%08x, moduleType=%u (%s), subEventSize=%u\n",
                                           subEventHeader, moduleType, moduleString.toLatin1().constData(), subEventSize);

                        for (u32 i=0; i<subEventSize; ++i)
                        {
                            u32 subEventData = iter.extractU32();
                            if (dumpData)
                                out << buf.sprintf("    %u = 0x%08x\n", i, subEventData);
                        }
                        wordsLeft -= subEventSize;
                    }

                    u32 eventEndMarker = iter.extractU32();
                    out << buf.sprintf("   eventEndMarker=0x%08x\n", eventEndMarker);
                } break;

            default:
                {
                    out << "Warning: Unknown section type " << sectionType
                        << " of size " << sectionSize
                        << ", skipping"
                        << endl;
                    iter.skip(sectionSize * sizeof(u32));
                } break;
        }
    }
}

ListFile::ListFile(const QString &fileName)
{
    m_file.setFileName(fileName);
}

bool ListFile::open()
{
    return m_file.open(QIODevice::ReadOnly);
}

QJsonObject ListFile::getDAQConfig()
{
    if (m_configJson.isEmpty())
    {
        qint64 savedPos = m_file.pos();
        m_file.seek(0);

        QByteArray configData;

        while (true)
        {
            u32 sectionHeader = 0;

            if (m_file.read((char *)&sectionHeader, sizeof(sectionHeader)) != sizeof(sectionHeader))
                break;

            int sectionType  = (sectionHeader & SectionTypeMask) >> SectionTypeShift;
            u32 sectionWords = (sectionHeader & SectionSizeMask) >> SectionSizeShift;

            qDebug() << "sectionType" << sectionType << ", sectionWords" << sectionWords;

            if (sectionType != SectionType_Config)
                break;

            if (sectionWords == 0)
                break;

            u32 sectionSize = sectionWords * sizeof(u32);

            QByteArray data = m_file.read(sectionSize);
            configData.append(data);
        }

        m_file.seek(savedPos);

        QJsonParseError parseError; // TODO: make parse error message available to the user
        m_configJson = QJsonDocument::fromJson(configData, &parseError);

        if (parseError.error != QJsonParseError::NoError)
        {
            qDebug() << "Parse error: " << parseError.errorString();
        }
    }

    if (!m_configJson.isEmpty())
    {
        qDebug() << "listfile config json:" << m_configJson.toJson();
        auto result = m_configJson.object();
        // Note: This check is for compatibilty with older listfiles.
        if (result.contains(QSL("DAQConfig")))
        {
            return result.value("DAQConfig").toObject();
        }
        return m_configJson.object();
    }

    return QJsonObject();
}

bool ListFile::seek(qint64 pos)
{
    qDebug() << &m_file << m_file.fileName() << m_file.isOpen();
    return m_file.seek(pos);
}

bool ListFile::readNextSection(DataBuffer *buffer)
{
    buffer->reserve(sizeof(u32));
    buffer->used = 0;

    if (m_file.read((char *)buffer->data, sizeof(u32)) != sizeof(u32))
        return false;

    buffer->used = sizeof(u32);

    u32 sectionHeader = *((u32 *)buffer->data);
    u32 sectionWords = (sectionHeader & SectionSizeMask) >> SectionSizeShift;
    qint64 sectionSize = sectionWords * sizeof(u32);

    if (sectionSize == 0)
        return true;

    qDebug() << sectionSize;

    buffer->reserve(sectionSize + sizeof(u32));

    if (m_file.read((char *)(buffer->data + buffer->used), sectionSize) != sectionSize)
        return false;

    buffer->used += sectionSize;
    return true;
}

s32 ListFile::readSectionsIntoBuffer(DataBuffer *buffer)
{
    s32 sectionsRead = 0;

    while (!m_file.atEnd() && buffer->free() >= sizeof(u32))
    {
        u32 sectionHeader = 0;

        qint64 savedPos = m_file.pos();

        if (m_file.read((char *)&sectionHeader, sizeof(u32)) != sizeof(u32))
            return -1;

        u32 sectionWords = (sectionHeader & SectionSizeMask) >> SectionSizeShift;

        qint64 bytesToRead = sectionWords * sizeof(u32);

        // add one u32 for the sectionHeader
        if ((qint64)buffer->free() < bytesToRead + (qint64)sizeof(u32))
        {
            // seek back to the sectionHeader
            m_file.seek(savedPos);
            break;
        }

        *(buffer->asU32()) = sectionHeader;
        buffer->used += sizeof(sectionHeader);

        if (m_file.read((char *)(buffer->data + buffer->used), bytesToRead) != bytesToRead)
            return -1;

        buffer->used += bytesToRead;
        ++sectionsRead;
    }
    return sectionsRead;
}

static const size_t ListFileBufferSize = 1 * 1024 * 1024;
static const size_t nBuffers = 2;

ListFileReader::ListFileReader(DAQStats &stats, QObject *parent)
    : QObject(parent)
    , m_stats(stats)
{
    for (size_t i=0; i<nBuffers; ++i)
    {
        m_freeBuffers.push_back(new DataBuffer(ListFileBufferSize));
    }
}

ListFileReader::~ListFileReader()
{
    qDeleteAll(m_freeBuffers);
}

void ListFileReader::setListFile(ListFile *listFile)
{
    m_listFile = listFile;
}

void ListFileReader::setState(DAQState state)
{
    m_state = state;
    emit stateChanged(state);
}

void ListFileReader::startFromBeginning(quint32 nBuffers)
{
    if (!m_listFile)
        return;

    m_buffersToRead = nBuffers;
    m_limitBuffers = (m_buffersToRead > 0);
    m_stopped = false;
    m_listFile->seek(0);
    m_bytesRead = 0;
    m_totalBytes = m_listFile->size();
    m_stats.listFileTotalBytes = m_listFile->size();

    m_stats.start();

    setState(DAQState::Running);
    emit progressChanged(m_bytesRead, m_totalBytes);

    while (m_state != DAQState::Idle)
    {
        auto buffer = getFreeBuffer();

        if (!readNextBuffer(buffer))
            addFreeBuffer(buffer);
    }

    qDebug() << __PRETTY_FUNCTION__ << "left loop";
}

// Returns true if a buffer was read, false otherwise
bool ListFileReader::readNextBuffer(DataBuffer *dest)
{
    if (!m_listFile) return false;

    dest->used = 0;
    s32 sectionsRead = 0;

    if (!m_stopped && (!m_limitBuffers || m_buffersToRead > 0)
        && ((sectionsRead = m_listFile->readSectionsIntoBuffer(dest)) > 0))
    {
        --m_buffersToRead;
        m_bytesRead += dest->used;
        emit progressChanged(m_bytesRead, m_totalBytes);
        emit mvmeEventBufferReady(dest);

        m_stats.addBuffersRead(1);
        m_stats.addBytesRead(dest->used);
        return true;
    }
    else
    {
        qDebug() << __PRETTY_FUNCTION__ << "stopping stats; setting idle";
        m_stats.stop();
        setState(DAQState::Idle);
        emit replayStopped();
    }

    return false;
}

void ListFileReader::stopReplay()
{
    qDebug() << __PRETTY_FUNCTION__;
    m_stopped = true;
}

void ListFileReader::addFreeBuffer(DataBuffer *buffer)
{
    m_freeBuffers.enqueue(buffer);
}

DataBuffer* ListFileReader::getFreeBuffer()
{
    // Check if buffers are available
    while (!m_freeBuffers.size())
    {
        QCoreApplication::processEvents();
    }

    return m_freeBuffers.dequeue();
}

//
// ListFileWriter
//
ListFileWriter::ListFileWriter(QObject *parent)
    : QObject(parent)
    , m_out(nullptr)
{ }

ListFileWriter::ListFileWriter(QIODevice *outputDevice, QObject *parent)
    : QObject(parent)
    , m_out(outputDevice)
{ }

void ListFileWriter::setOutputDevice(QIODevice *device)
{
    m_out = device;
}

bool ListFileWriter::writeConfig(QByteArray contents)
{
    while (contents.size() % sizeof(u32))
    {
        contents.append(' ');
    }

    int configSize = contents.size();
    int configWords = configSize / sizeof(u32);
    int configSections = qCeil((float)configSize / (float)SectionMaxSize);

    DataBuffer localBuffer(configSections * SectionMaxSize + // space for all config sections
                           configSections * sizeof(u32)); // space for headers

    DataBuffer *buffer = &localBuffer;

    u8 *bufferP = buffer->data;
    const char *configP = contents.constData();

    while (configSections--)
    {
        u32 *sectionHeader = (u32 *)bufferP;
        bufferP += sizeof(u32);
        *sectionHeader = (SectionType_Config << SectionTypeShift) & SectionTypeMask;
        int sectionBytes = qMin(configSize, SectionMaxSize);
        int sectionWords = sectionBytes / sizeof(u32);
        *sectionHeader |= (sectionWords << SectionSizeShift) & SectionSizeMask;

        memcpy(bufferP, configP, sectionBytes);
        bufferP += sectionBytes;
        configP += sectionBytes;
        configSize -= sectionBytes;
    }

    buffer->used = bufferP - buffer->data;

    if (m_out->write((const char *)buffer->data, buffer->used) != (qint64)buffer->used)
        return false;

    m_bytesWritten += buffer->used;

    QTextStream out(stdout);
    dump_mvme_buffer(out, buffer, false);

    return true;
}

bool ListFileWriter::writeBuffer(const char *buffer, size_t size)
{
    qint64 written = m_out->write(buffer, size);
    if (written == static_cast<qint64>(size))
    {
        m_bytesWritten += static_cast<u64>(written);
        return true;
    }
    return false;
}

bool ListFileWriter::writeEndSection()
{
    u32 header = (SectionType_End << SectionTypeShift) & SectionTypeMask;

    if (m_out->write((const char *)&header, sizeof(header)) != sizeof(header))
        return false;

    m_bytesWritten += sizeof(header);

    return true;
}
