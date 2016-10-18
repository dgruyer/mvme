#ifndef UUID_9196420f_dd04_4572_8e4b_952039634913
#define UUID_9196420f_dd04_4572_8e4b_952039634913

#include "globals.h"
#include "databuffer.h"
#include "mvme_config.h"
#include "histogram.h"
#include "hist2d.h"
#include <QList>
#include <QWidget>
#include <QFuture>
#include <QSet>

class VMEController;
class VMUSBReadoutWorker;
class VMUSBBufferProcessor;
class MVMEEventProcessor;
class mvme;
class ListFile;
class ListFileReader;
class QJsonObject;

class QTimer;
class QThread;

class MVMEContext: public QObject
{
    Q_OBJECT
    signals:
        void daqStateChanged(const DAQState &state);
        void modeChanged(GlobalMode mode);

        void vmeControllerSet(VMEController *controller);

        void eventConfigAdded(EventConfig *eventConfig);
        void eventConfigAboutToBeRemoved(EventConfig *eventConfig);

        void configChanged(DAQConfig *config);
        void configFileNameChanged(const QString &fileName);

        void objectAdded(QObject *object);
        void objectAboutToBeRemoved(QObject *object);

        void sigLogMessage(const QString &);

    public:
        MVMEContext(mvme *mainwin, QObject *parent = 0);
        ~MVMEContext();

        void setController(VMEController *controller);

        QString getUniqueModuleName(const QString &prefix) const;

        VMEController *getController() const { return m_controller; }
        VMUSBReadoutWorker *getReadoutWorker() const { return m_readoutWorker; }
        VMUSBBufferProcessor *getBufferProcessor() const { return m_bufferProcessor; }
        DAQConfig *getConfig() { return m_config; }
        DAQConfig *getDAQConfig() { return m_config; }
        void setConfig(DAQConfig *config);
        QList<EventConfig *> getEventConfigs() const { return m_config->getEventConfigs(); }
        DataBufferQueue *getFreeBuffers() { return &m_freeBuffers; }
        DAQState getDAQState() const;
        const DAQStats &getDAQStats() const { return m_daqStats; }
        DAQStats &getDAQStats() { return m_daqStats; }
        void setListFile(ListFile *listFile);
        ListFile *getListFile() const { return m_listFile; }
        void setMode(GlobalMode mode);
        GlobalMode getMode() const;

        void addObject(QObject *object);
        void removeObject(QObject *object);
        bool containsObject(QObject *object);

        template<typename T>
        QVector<T> getObjects() const
        {
            QVector<T> ret;
            for (auto obj: m_objects)
            {
                auto casted = qobject_cast<T>(obj);
                if (casted)
                    ret.push_back(casted);
            }
            return ret;
        }

        template<typename T, typename Predicate>
        QVector<T> filterObjects(Predicate p) const
        {
            QVector<T> ret;
            for (auto obj: m_objects)
            {
                auto casted = qobject_cast<T>(obj);
                if (casted && p(casted))
                    ret.push_back(casted);
            }
            return ret;
        }

        void setConfigFileName(const QString &name)
        {
            m_configFileName = name;
            emit configFileNameChanged(name);
        }

        QString getConfigFileName() const
        {
            return m_configFileName;
        }

        void write(QJsonObject &json) const;
        void read(const QJsonObject &json);

        void logMessage(const QString &msg);
        void logMessages(const QStringList &mgs, const QString &prefix = QString());

        friend class mvme;

    public slots:
        void startReplay();
        void startDAQ(quint32 nCycles=0);
        void stopDAQ();

    private slots:
        void tryOpenController();
        void logModuleCounters();
        void onDAQStateChanged(DAQState state);
        void onReplayDone();

        // config related
        void onEventAdded(EventConfig *event);
        void onEventAboutToBeRemoved(EventConfig *config);
        void onModuleAdded(ModuleConfig *module);
        void onModuleAboutToBeRemoved(ModuleConfig *config);

    private:
        void prepareStart();
        void updateHistogramCollectionDefinition(ModuleConfig *module);


        DAQConfig *m_config;
        VMEController *m_controller = nullptr;
        QTimer *m_ctrlOpenTimer;
        QTimer *m_logTimer;
        QFuture<void> m_ctrlOpenFuture;
        QThread *m_readoutThread;

        VMUSBReadoutWorker *m_readoutWorker;
        VMUSBBufferProcessor *m_bufferProcessor;

        QThread *m_eventThread;
        MVMEEventProcessor *m_eventProcessor;

        DataBufferQueue m_freeBuffers;
        QString m_configFileName;
        QSet<QObject *> m_objects;
        mvme *m_mainwin;
        DAQStats m_daqStats;
        ListFile *m_listFile = nullptr;
        GlobalMode m_mode;
        DAQState m_state;
        ListFileReader *m_listFileWorker;
        QTime m_replayTime;
};

#endif
