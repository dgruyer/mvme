#ifndef UUID_2aee2ea6_9760_46db_8d90_4dad1e4d019f
#define UUID_2aee2ea6_9760_46db_8d90_4dad1e4d019f

#include "typedefs.h"
#include "threading.h"
#include <QHash>
#include <QObject>
#include <QVector>

class DataBuffer;
class MVMEContext;
class MesytecDiagnostics;
class DualWordDataFilterConfig;

class MVMEEventProcessorPrivate;

using DualWordFilterValues = QHash<DualWordDataFilterConfig *, u64>;
using DualWordFilterDiffs  = QHash<DualWordDataFilterConfig *, double>;

class MVMEEventProcessor: public QObject
{
    Q_OBJECT
    signals:
#ifdef OLD_STYLE_THREADING
        void bufferProcessed(DataBuffer *buffer);
#endif
        void logMessage(const QString &);

    public:
        MVMEEventProcessor(MVMEContext *context);
        ~MVMEEventProcessor();

        bool isProcessingBuffer() const;

        void setDiagnostics(MesytecDiagnostics *diag);
        MesytecDiagnostics *getDiagnostics() const;

        // Returns a deep copy of the hash to avoid threading issues.
        DualWordFilterValues getDualWordFilterValues() const;

        // Returns a hash of the most recent differences of dual word filter values.
        DualWordFilterDiffs getDualWordFilterDiffs() const;

        ThreadSafeDataBufferQueue *m_freeBufferQueue = nullptr;
        ThreadSafeDataBufferQueue *m_filledBufferQueue = nullptr;

    public slots:
        void removeDiagnostics();
        void newRun();
        void processDataBuffer(DataBuffer *buffer);

        void startProcessing();
        void stopProcessing();

    private:
        MVMEEventProcessorPrivate *m_d;
};

#endif
