#include <cassert>
#include "datathread.h"
#include "mvme.h"
#include "math.h"
#include <QTimer>
#include "mvmedefines.h"
#include "datacruncher.h"
#include "CVMUSBReadoutList.h"
#include "vme.h"
#include "util.h"
#include <QDebug>
#include <QMutexLocker>

#define DATABUFFER_SIZE 100000


DataThread::DataThread(QObject *parent) :
    QObject(parent)
{
    setObjectName("DataThread");

    myMvme = (mvme*)parent;
    dataTimer = new QTimer(this);
    m_multiEvent = false;
    m_readLength = 100;
    initBuffers();
    connect(dataTimer, SIGNAL(timeout()), SLOT(dataTimerSlot()));

}

/* This is the central data readout function. It is called
 * periodically. If data available, it reads out according to the
 * specs of the vme devices involved
*/
#ifdef VME_CONTROLLER_CAEN
void DataThread::dataTimerSlot()
{
    qDebug() << "DataThread: " << QThread::currentThread();
    quint32 i, j, len, id;
    quint32 ret;

    // read available data from controller
    // todo: implement multiple readout depending on list of devices
    // todo: implement readout routines depending on type of vme device
    ret = readData();
    qDebug("received %d words: %d", ret, m_readLength);
    if(ret <= 0)
        return;

    // todo: replace by adequate function call in vme module class
    if(!checkData())
        return;

    // look for header word:
    if((dataBuffer[0] & 0xF0000000) != 0x40000000){
        qDebug("wrong header word %lx", dataBuffer[0]);
        return;
    }
    // extract data length (# of following words):
    len = (quint32)(dataBuffer[0] & 0x00000FFF);
//    qDebug("read: %d, words: %d", ret, len);

    // copy data into datacruncher ringbuffer
    for(i = 0; i <= len; i++, m_writePointer++){
//        qDebug("%d %x",i, dataBuffer[i]);
        m_pRingbuffer[m_writePointer] = dataBuffer[i];
    }
    if(m_writePointer > RINGBUFMAX)
        m_writePointer = 0;

    emit dataReady();
}

#elif defined VME_CONTROLLER_WIENER
void DataThread::dataTimerSlot()
{
    //qDebug() << "DataThread: " << QThread::currentThread();

    // read available data from controller
    // todo: implement multiple readout depending on list of devices
    // todo: implement readout routines depending on type of vme device
    quint32 ret = readData();

    //qDebug("received %d bytes, m_readLength=%d", ret, m_readLength);

    if(ret <= 0)
        return;

    //qDebug() << __PRETTY_FUNCTION__ << QThread::currentThread();

    quint32 wordsReceived = ret / sizeof(quint32);

#if 0
    qDebug("buffer dump:");
    for(quint32 bufferIndex = 0; bufferIndex < wordsReceived; ++bufferIndex)
    {
        qDebug("  0x%08lx", dataBuffer[bufferIndex]);
    }
    qDebug("end of buffer dump");
#endif

    // todo: replace by adequate function call in vme module class
    if(!checkData())
        return;

    enum BufferState
    {
        BufferState_Header,
        BufferState_Data,
        BufferState_EOE
    };

    BufferState bufferState = BufferState_Header;
    quint32 wordsInEvent = 0;

    for(quint32 bufferIndex = 0; bufferIndex < wordsReceived; ++bufferIndex)
    {
        quint32 currentWord = dataBuffer[bufferIndex];

        // skip BERR markers inserted by VMUSB
        if (currentWord == 0xFFFFFFFF)
        {
            continue;
        }

        switch (bufferState)
        {
            // TODO: handle MDPP format
            case BufferState_Header:
            {
                if ((currentWord & 0xC0000000) == 0x40000000)
                {
                    wordsInEvent = currentWord & 0x00000FFF;
                    m_pRingbuffer[m_writePointer++] = currentWord;
                    bufferState = BufferState_Data;

                    //qDebug("found header word 0x%08lx, wordsInEvent=%u", currentWord, wordsInEvent);
                } else
                {
                    qDebug("did not find header word, skipping. got 0x%08lx", currentWord);
                    //debugOutputBuffer(dataBuffer, wordsReceived);
                }
            } break;

            case BufferState_Data:
            {
                m_pRingbuffer[m_writePointer++] = currentWord;
                if (--wordsInEvent == 1)
                {
                    bufferState = BufferState_EOE;
                }
            } break;

            case BufferState_EOE:
            {
                if ((currentWord & 0xC0000000) == 0xC0000000)
                {
                    //qDebug("found EOE: 0x%08lx", currentWord);
                } else
                {
                    qDebug("expected EOE word, got 0x%08lx, continuing regardless", currentWord);
                }
                m_pRingbuffer[m_writePointer++] = currentWord;
                bufferState = BufferState_Header;
                emit dataReady();
            } break;
        }

        if (m_writePointer > RINGBUFMAX)
        {
            m_writePointer = 0;
        }
    }
}
#endif

DataThread::~DataThread()
{
    delete dataTimer;
    delete dataBuffer;
}

void DataThread::startReading(quint16 readTimerPeriod)
{
    QMutexLocker locker(&m_controllerMutex);
#ifdef VME_CONTROLLER_CAEN
    // stop acquisition
    myCu->vmeWrite16(0x603A, 0);
    // clear FIFO
    myCu->vmeWrite16(0x603C, 1);
    // start acquisition
    myCu->vmeWrite16(0x603A, 1);
    // readout reset
    myCu->vmeWrite16(0x6034, 1);
#else
    // stop acquisition
    myVu->vmeWrite16(0x603A, 0);
    // clear FIFO
    myVu->vmeWrite16(0x603C, 1);
    // readout reset
    myVu->vmeWrite16(0x6034, 1);
    // start acquisition
    myVu->vmeWrite16(0x603A, 1);
#endif

    dataTimer->setInterval(readTimerPeriod);

    /* Start the timer in the DataThreads thread context. If start() would be
     * called directly, the timer events would be generated in the current
     * thread context (the GUI thread). Thus if the GUI is busy not enough timer events
     * would be generated, slowing down the readout. */
    QMetaObject::invokeMethod(dataTimer, "start");
}

void DataThread::stopReading()
{
    QMetaObject::invokeMethod(dataTimer, "stop");
    QMutexLocker locker(&m_controllerMutex);

#ifdef VME_CONTROLLER_CAEN
    myCu->vmeWrite16(0x603A, 0);
    myCu->vmeWrite16(0x603C, 1);
#else
    myVu->vmeWrite16(0x603A, 0);
    myVu->vmeWrite16(0x603C, 1);
#endif
}

void DataThread::setRingbuffer(quint32 *buffer)
{
    m_pRingbuffer = buffer;
    m_writePointer = 0;
    qDebug("ringbuffer initialized");
}

void DataThread::setReadoutmode(bool multi, quint16 maxlen, bool mblt)
{
    QMutexLocker locker(&m_controllerMutex);
    m_mblt = mblt;
    m_multiEvent = multi;
#ifdef VME_CONTROLLER_CAEN
    // stop acquisition
    myCu->vmeWrite16(0x603A, 0);
    // reset FIFO
    myCu->vmeWrite16(0x603C, 1);

    if(multi){
        // multievent register
        qDebug("set multi");
        myCu->vmeWrite16(0x6036, 1);
        myCu->vmeWrite16(0x601A, maxlen);
        m_readLength = maxlen + 34;
    }
    else{
        qDebug("set single");
        myCu->vmeWrite16(0x6036, 0);
    }
    // clear Fifo
    myCu->vmeWrite16(0x603C, 1);
    // reset readout
    myCu->vmeWrite16(0x6034, 1);
#elif defined VME_CONTROLLER_WIENER
    // stop acquisition
    myVu->vmeWrite16(0x603A, 0);
    // reset FIFO
    myVu->vmeWrite16(0x603C, 1);

    int am = mblt ? VME_AM_A32_USER_MBLT : VME_AM_A32_USER_BLT;

    CVMUSBReadoutList readoutList;

    if(multi){
        // multievent register
        qDebug("set multi");
        myVu->vmeWrite16(0x6036, 3);
        myVu->vmeWrite16(0x601A, maxlen); // max transfer data (0 == unlimited)
        m_readLength = maxlen;

        /* Read the mxdc fifo using a block transfer. This should result in a BERR
         * once all data in the FIFO has been read.  */
        readoutList.addFifoRead32(0x00000000, am, m_readLength);

        /* Write to the read_reset register to clear BERR and allow a new conversion. */
        readoutList.addWrite16(0x00000000 | 0x6034, VME_AM_A32_USER_PROG, 1);

    }
    else{
        qDebug("set single");
        myVu->vmeWrite16(0x6036, 0);
        am = VME_AM_A32_USER_BLT; // FIXME
        readoutList.addFifoRead32(0x00000000, am, 250);
        readoutList.addWrite16(0x00000000 | 0x6034, VME_AM_A32_USER_PROG, 1);
    }

    m_readoutPacket.reset(listToOutPacket(TAVcsWrite | TAVcsIMMED, &readoutList, &m_readoutPacketSize));
    qDebug("readoutPacketSize=%d", m_readoutPacketSize);

    // clear Fifo
    myVu->vmeWrite16(0x603C, 1);
    // reset readout
    myVu->vmeWrite16(0x6034, 1);
#endif
}

void DataThread::initBuffers()
{
    dataBuffer = new quint32[DATABUFFER_SIZE];
    qDebug("buffers initialized");
}


quint32 DataThread::readData()
{
    quint16 offset = 0;
#ifdef VME_CONTROLLER_CAEN
    quint16 ret;
    quint8 irql;
    quint16 count = m_readLength*4;

    //check for irq
    irql = myCu->Irq();
    if(irql){
        // read until no further data
        while(count == m_readLength*4){
            if(m_mblt)
                count = myCu->vmeMbltRead32(0x0, m_readLength * 4, &dataBuffer[offset]);
            else
                count = myCu->vmeBltRead32(0x0, m_readLength * 4, &dataBuffer[offset]);

//            qDebug("read %d bytes from USB - %d %d", count, offset, m_readLength*4);
            offset += count;
        }
//        qDebug("read %d bytes from USB", offset);
        // service irq
        ret = myCu->ackIrq(irql);
        // reset module readout
        myCu->vmeWrite16(0x6034, 1);
    }
    else
        offset = 0;
#elif defined VME_CONTROLLER_WIENER

    QMutexLocker locker(&m_controllerMutex);
#if 0

    int timeout_ms = 5000;

    int bytesRead = myVu->transaction(m_readoutPacket.get(), m_readoutPacketSize, dataBuffer, DATABUFFER_SIZE, timeout_ms);

    qDebug("readData: transaction returned %d", bytesRead);

    bytesRead -= bytesRead % 4; // get rid of the status code from the write command

    offset = bytesRead > 0 ? bytesRead : 0;
#endif

    if (!m_multiEvent)
    {

#if 0
    size_t bytesRead = 0;

    CVMUSBReadoutList readList;
    readList.addFifoRead32(0x00000000, VME_AM_A32_USER_BLT, 250);
    myVu->listExecute(&readList, dataBuffer, DATABUFFER_SIZE, &bytesRead);
    offset = bytesRead;
    //qDebug("readData: listExecute(readList) got %d bytes", bytesRead);

    uint32_t tempBuffer[1024];
    CVMUSBReadoutList resetList;
    resetList.addWrite16(0x00000000 | 0x6034, VME_AM_A32_USER_PROG, 1);
    myVu->listExecute(&resetList, tempBuffer, 1024, &bytesRead);
    //qDebug("readData: listExecute(resetList) got %d bytes: %04x", bytesRead, (uint16_t)*tempBuffer);
#else
    int bytesRead = myVu->vmeBltRead32(0x00000000, 250, dataBuffer + offset);
    int wordsRead = bytesRead / sizeof(uint32_t);
    //debugOutputBuffer(dataBuffer, wordsRead);
    myVu->vmeWrite16(0x00000000 | 0x6034, 1);
    offset = bytesRead > 0 ? bytesRead : 0;
#endif
    } else
    {
      size_t bytesRead = 0;
      CVMUSBReadoutList readoutList;
      readoutList.addFifoRead32(0x00000000, m_mblt ? VME_AM_A32_USER_MBLT : VME_AM_A32_USER_BLT, m_readLength);
      myVu->listExecute(&readoutList, dataBuffer, DATABUFFER_SIZE, &bytesRead);
      offset = bytesRead;

      uint32_t tempBuffer[1024];
      readoutList.clear();
      readoutList.addWrite16(0x00000000 | 0x6034, VME_AM_A32_USER_PROG, 1);
      myVu->listExecute(&readoutList, tempBuffer, 1024, &bytesRead);
    }

#endif

    return offset;
}

#ifdef VME_CONTROLLER_CAEN
void DataThread::setCu(caenusb *cu)
{
    myCu = cu;
}
#else
void DataThread::setVu(vmUsb *vu)
{
    myVu = vu;
}
#endif

#if 0
quint32 DataThread::readFifoDirect(quint16 base, quint16 len, quint32 *data)
{
    long longVal;
    quint32 i;
    for(i = 0; i < len; i++){
        myCu->vmeRead32(base, &longVal);
        data[i] = longVal;
    }
    return i;
}
#endif

bool DataThread::checkData()
{
    return true;
}

void DataThread::analyzeBuffer(quint8 type)
{
/*    unsigned int oldlen = 0;
    unsigned int totallen = 0;
    unsigned int elen = 0;
    unsigned int dataLen = 0;
    unsigned int * point = (unsigned int*) sDataBuffer;
    unsigned char evnum;
    unsigned int nEvents = 0;

    int ret = 0;
    unsigned int limit = 0;

    bool quit = false;

    QString s, s2;
    int i, j;
    unsigned int start1 = 0xFFFF;
    unsigned int start2 = 0xAAAA;
    bool flag = false;

    qDebug("now reading buffer");

    // read buffer
    ret = myCu->readBuffer(sDataBuffer);
    if(!(ret > 0))
        return;

    qDebug("read Buffer, len: %d", ret);

    rp = 0;
    wordsToRead = ret/2;
    bufferCounter++;

    // read VME buffer header
    nEvents = sDataBuffer[rp] & 0x0FFF;
    s.sprintf("Buffer #%d header: %04x, #bytes: %d, #events: %d\n", bufferCounter, sDataBuffer[rp], ret, nEvents);
    s2.sprintf("first data words (beginning with buffer header): %04x %04x %04x %04x\n", sDataBuffer[rp], sDataBuffer[rp+1], sDataBuffer[rp+2], sDataBuffer[rp+3]);
    s.append(s2);

    wordsToRead--;
    rp++;

    // next should be VME event header
    dataLen = (sDataBuffer[rp] & 0x0FFF);
    evWordsToRead = dataLen;
    s2.sprintf("VME event header: %04x, len:%d\n", sDataBuffer[rp], dataLen);
    s.append(s2);

    wordsToRead--;
    rp++;

        s2.sprintf("now listing complete buffer:\n");
        s.append(s2);
        rp = 0;
        for(unsigned int i = 0; i < (ret / 4); i++){
            dataBuffer[mBufPointer] = sDataBuffer[rp] + 0x10000 * sDataBuffer[rp+1];
            rp += 2;
            wordsToRead-=2;
            evWordsToRead-=2;
            s2.sprintf("%04d   %08x\n", i, dataBuffer[mBufPointer]);
            s.append(s2);
        }

    // next should be mesytec event headers:
    dataBuffer[mBufPointer] = sDataBuffer[rp] + 0x10000 * sDataBuffer[rp+1];
//	qDebug("header: %08x", dataBuffer[mBufPointer]);
    while((dataBuffer[mBufPointer] & 0xC0000000) == 0x40000000){
        elen = (dataBuffer[mBufPointer] & 0x000007FF);
        mEvWordsToRead = elen * 2;
        rp+=2;
        wordsToRead-=2;
        evWordsToRead-=2;
        s2.sprintf("mesytec evHead: %08x, elen:%d, Buf: %d, Ev: %d\n", dataBuffer[mBufPointer], elen, wordsToRead, evWordsToRead);
        s.append(s2);
        mBufPointer++;

        // next should be mesytec events:
        for(unsigned i = 0; i < elen; i++){
            dataBuffer[mBufPointer] = sDataBuffer[rp] + 0x10000 * sDataBuffer[rp+1];
            s2.sprintf("%08x\n", dataBuffer[mBufPointer]);
            s.append(s2);
            rp += 2;
            wordsToRead-=2;
            evWordsToRead-=2;
            mBufPointer++;
        }
        int result = myData->handover(dataBuffer);
        if(result == -1){
            qDebug("Wrong length %d in event# %d, oldlen: %d", elen, evnum, oldlen);
        }
        mBufPointer = 0;
        dataBuffer[mBufPointer] = sDataBuffer[rp] + 0x10000 * sDataBuffer[rp+1];
    }
    rp+=2;
    wordsToRead-=2;
    evWordsToRead-=2;
    if(dataBuffer[mBufPointer] == 0xFFFFFFFF){
        s2.sprintf("Terminator %08x, Buf: %d, Ev: %d\n", dataBuffer[mBufPointer], wordsToRead, evWordsToRead);
        s.append(s2);
    }

    // now wordsToRead should be = 2
    if(wordsToRead == 2){
        s2.sprintf("remaining words to read: %d\n", wordsToRead);
        s.append(s2);
    }
    else{
        s2.sprintf("ERROR: remaining words to read: %d (should be: 2)\n", wordsToRead);
        s.append(s2);
        s2.sprintf("now listing complete buffer:\n");
        s.append(s2);
        rp = 0;
        for(unsigned int i = 0; i < wordsToRead / 2; i++){
            dataBuffer[mBufPointer] = sDataBuffer[rp] + 0x10000 * sDataBuffer[rp+1];
            rp += 2;
            wordsToRead-=2;
            evWordsToRead-=2;
            s2.sprintf("%08x\n", dataBuffer[mBufPointer]);
            s.append(s2);
        }
    }
    // now evWordsToRead should be = 0
    if(evWordsToRead == 0)
        s2.sprintf("remaining event words to read: %d\n", evWordsToRead);
    else
        s2.sprintf("ERROR: remaining event words to read: %d (should be: 0)\n", wordsToRead);
    s.append(s2);

    // next should be vm-USB buffer terminator:
    // misuse dataBuffer for calculation...
    dataBuffer[mBufPointer] = sDataBuffer[rp] + 0x10000 * sDataBuffer[rp+1];
    wordsToRead -= 2;
    if(dataBuffer[mBufPointer] == 0xFFFFFFFF)
        s2.sprintf("Buffer Terminator: %08x, %d, %d\n", dataBuffer[mBufPointer], wordsToRead, evWordsToRead);
    else
        s2.sprintf("ERROR: expected FFFFFFFF, read: %08x\n", dataBuffer[mBufPointer], wordsToRead, evWordsToRead);
    s.append(s2);
*/
//    mctrl->ui->dataDisplay->setText(s);
//    str.sprintf("%d", bufferCounter);
//    mctrl->ui->buffer->setText(str);

        //debugStream << s;
}

