#ifndef UUID_9196420f_dd04_4572_8e4b_952039634913
#define UUID_9196420f_dd04_4572_8e4b_952039634913

#include <QList>
#include <QWidget>
#include <QFuture>
#include "globals.h"
#include "vmecommandlist.h"
#include "vme_module.h"
#include "databuffer.h"
#include "mvme_config.h"

class VMEController;
class VMUSBReadoutWorker;
class VMUSBBufferProcessor;
class MVMEEventProcessor;
class Histogram;

class QTimer;
class QThread;

class MVMEContext: public QObject
{
    static const size_t dataBufferCount = 20;
    static const size_t dataBufferSize  = 30 * 1024;

    Q_OBJECT
    signals:
        void vmeControllerSet(VMEController *controller);
        void eventConfigAdded(EventConfig *eventConfig);
        void moduleAdded(EventConfig *eventConfig, ModuleConfig *module);
        void daqStateChanged(const DAQState &state);
        void configChanged();
        void histogramAdded(ModuleConfig *module, Histogram *histo);

    public:
        MVMEContext(QObject *parent = 0);
        ~MVMEContext();

        void addModule(EventConfig *eventConfig, ModuleConfig *module);
        void addEventConfig(EventConfig *eventConfig);
        void setController(VMEController *controller);

        int getTotalModuleCount() const
        {
            int ret = 0;
            for (auto eventConfig: m_config->eventConfigs)
                ret += eventConfig->modules.size();
            return ret;
        }

        VMEController *getController() const { return m_controller; }
        VMUSBReadoutWorker *getReadoutWorker() const { return m_readoutWorker; }
        VMUSBBufferProcessor *getBufferProcessor() const { return m_bufferProcessor; }
        DAQConfig *getConfig() { return m_config; }
        void setConfig(DAQConfig *config);
        QList<EventConfig *> getEventConfigs() const { return m_config->eventConfigs; }
        DataBufferQueue *getFreeBuffers() { return &m_freeBuffers; }
        DAQState getDAQState() const;

        QMap<ModuleConfig *, Histogram *> getHistograms() { return m_histograms; }
        bool addHistogram(ModuleConfig *module, Histogram *histo)
        {
            if (m_histograms.contains(module))
                return false;
            m_histograms[module] = histo;
            emit histogramAdded(module, histo);
            return true;
        }

        friend class mvme;

    private slots:
        void tryOpenController();

    private:
        DAQConfig *m_config;
        VMEController *m_controller = nullptr;
        QTimer *m_ctrlOpenTimer;
        QFuture<void> m_ctrlOpenFuture;
        QThread *m_readoutThread;

        VMUSBReadoutWorker *m_readoutWorker;
        VMUSBBufferProcessor *m_bufferProcessor;

        QThread *m_eventProcessorThread;
        MVMEEventProcessor *m_eventProcessor;

        DataBufferQueue m_freeBuffers;
        QString m_configFilename;
        QMap<ModuleConfig *, Histogram *> m_histograms;
};

#endif
