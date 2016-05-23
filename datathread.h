#ifndef DATATHREAD_H
#define DATATHREAD_H

#include <QThread>
class QTimer;
class mvme;
class vmUsb;
class caenusb;

class DataThread : public QThread
{
    Q_OBJECT
public:
    explicit DataThread(QObject *parent = 0);
    ~DataThread();
    void initBuffers();
    quint32 readData();
    void setVu(vmUsb* vu);
    void setCu(caenusb *cu);
    quint32 readFifoDirect(quint16 base, quint16 len, quint32* data);
//    quint32 readBlt32(quint16 base, quint16 len, quint32* data);
    void analyzeBuffer(quint8 type);

    bool checkData();

signals:
    void dataReady();
    void bufferStatus(int);

public slots:
    void dataTimerSlot();
    void startDataTimer(quint16 period);
    void stopDataTimer(void);
    void startReading();
    void stopReading();
    void setRingbuffer(quint32* buffer);
    void setReadoutmode(bool multi, quint16 maxlen, bool mblt);

protected:
    void run();
    QTimer* dataTimer;
    mvme* myMvme;
    vmUsb* myVu;
    caenusb* myCu;

    quint32* dataBuffer;

    quint32* m_pRingbuffer;
    quint32 m_writePointer;

    quint32 bufferCounter;


    quint32 rp;
    quint32 wordsToRead;
    quint32 eventsToRead;
    quint32 evWordsToRead;
    quint32 mEvWordsToRead;
    quint16 mBufPointer;
    quint8 readNext;

    bool m_multiEvent;
    bool m_mblt;
    quint16 readlen;
};

#endif // DATATHREAD_H
