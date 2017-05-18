#ifndef UUID_364b82ee_241c_4c09_acbf_f7e36698fb74
#define UUID_364b82ee_241c_4c09_acbf_f7e36698fb74

#include "globals.h"
#include "vme_script.h"
#include "template_system.h"
#include <QObject>
#include <QUuid>
#include <qwt_scale_map.h>

class QJsonObject;

class ConfigObject: public QObject
{
    Q_OBJECT
    signals:
        void modifiedChanged(bool);
        void modified(bool);
        void enabledChanged(bool);

    public:
        ConfigObject(QObject *parent = 0);

        QUuid getId() const { return m_id; }

        virtual void setModified(bool b = true);
        bool isModified() const { return m_modified; }

        void setEnabled(bool b);
        bool isEnabled() const { return m_enabled; }

        QString getObjectPath() const;

        void read(const QJsonObject &json);
        void write(QJsonObject &json) const;

        ConfigObject *findChildById(const QUuid &id, bool recurse=true) const
        {
            return findChildById<ConfigObject *>(id, recurse);
        }

        template<typename T> T findChildById(const QUuid &id, bool recurse=true) const
        {
            for (auto child: children())
            {
                auto configObject = qobject_cast<ConfigObject *>(child);

                if (configObject)
                {
                    if (configObject->getId() == id)
                        return qobject_cast<T>(configObject);
                }

                if (recurse)
                {
                    if (auto obj = configObject->findChildById<T>(id, recurse))
                        return obj;
                }
            }

            return {};
        }

        template<typename T, typename Predicate>
        T findChildByPredicate(Predicate p, bool recurse=true) const
        {
            for (auto child: children())
            {
                auto asT = qobject_cast<T>(child);

                if (asT && p(asT))
                    return asT;

                if (recurse)
                {
                    if (auto cfg = qobject_cast<ConfigObject *>(child))
                    {
                        if (auto obj = cfg->findChildByPredicate<T>(p, recurse))
                            return obj;
                    }
                }
            }
            return {};
        }

    protected:
        ConfigObject(QObject *parent, bool watchDynamicProperties);
        bool eventFilter(QObject *obj, QEvent *event) override;
        void setWatchDynamicProperties(bool doWatch);

        virtual void read_impl(const QJsonObject &json) = 0;
        virtual void write_impl(QJsonObject &json) const = 0;


        QUuid m_id;
        bool m_modified = false;
        bool m_enabled = true;
        bool m_eventFilterInstalled = false;
};

class VMEScriptConfig: public ConfigObject
{
    Q_OBJECT
    public:
        VMEScriptConfig(QObject *parent = 0);
        VMEScriptConfig(const QString &name, const QString &contents, QObject *parent = 0);

        QString getScriptContents() const
        { return m_script; }

        void setScriptContents(const QString &);

        vme_script::VMEScript getScript(u32 baseAddress = 0) const;

        QString getVerboseTitle() const;

    protected:
        virtual void read_impl(const QJsonObject &json) override;
        virtual void write_impl(QJsonObject &json) const override;

    private:
        QString m_script;
};

class ModuleConfig: public ConfigObject
{
    Q_OBJECT
    public:
        ModuleConfig(QObject *parent = 0);

        uint32_t getBaseAddress() const { return m_baseAddress; }
        void setBaseAddress(uint32_t address);

        const VMEModuleMeta getModuleMeta() const { return m_meta; }
        void setModuleMeta(const VMEModuleMeta &meta) { m_meta = meta; }

        VMEScriptConfig *getResetScript() const { return m_resetScript; }
        VMEScriptConfig *getReadoutScript() const { return m_readoutScript; }

        QVector<VMEScriptConfig *>  getInitScripts() const { return m_initScripts; }
        VMEScriptConfig *getInitScript(const QString &scriptName) const;
        VMEScriptConfig *getInitScript(s32 scriptIndex) const;

        void addInitScript(VMEScriptConfig *script);

    protected:
        virtual void read_impl(const QJsonObject &json) override;
        virtual void write_impl(QJsonObject &json) const override;

    private:
        uint32_t m_baseAddress = 0;
        VMEScriptConfig *m_resetScript;
        VMEScriptConfig *m_readoutScript;
        QVector<VMEScriptConfig *> m_initScripts;

        VMEModuleMeta m_meta;
};

class EventConfig: public ConfigObject
{
    Q_OBJECT
    signals:
    void moduleAdded(ModuleConfig *module);
    void moduleAboutToBeRemoved(ModuleConfig *module);

    public:
        EventConfig(QObject *parent = nullptr);

        void addModuleConfig(ModuleConfig *config)
        {
            config->setParent(this);
            modules.push_back(config);
            emit moduleAdded(config);
            setModified();
        }

        bool removeModuleConfig(ModuleConfig *config)
        {
            bool ret = modules.removeOne(config);
            if (ret)
            {
                emit moduleAboutToBeRemoved(config);
                setModified();
            }

            return ret;
        }

        QList<ModuleConfig *> getModuleConfigs() const { return modules; }

        TriggerCondition triggerCondition = TriggerCondition::NIM1;
        uint8_t irqLevel = 0;
        uint8_t irqVector = 0;
        // Maximum time between scaler stack executions in units of 0.5s
        uint8_t scalerReadoutPeriod = 0;
        // Maximum number of events between scaler stack executions
        uint16_t scalerReadoutFrequency = 0;

        QList<ModuleConfig *> modules;
        /** Known keys for an event:
         * "daq_start", "daq_stop", "readout_start", "readout_end"
         */
        QMap<QString, VMEScriptConfig *> vmeScripts;

        /* Set by the readout worker and then used by the buffer
         * processor to map from stack ids to event configs. */
        // Maybe should move this elsewhere as it is vmusb specific
        uint8_t stackID;

    protected:
        virtual void read_impl(const QJsonObject &json) override;
        virtual void write_impl(QJsonObject &json) const override;
};

class VMEConfig: public ConfigObject
{
    Q_OBJECT
    signals:
        void eventAdded(EventConfig *config);
        void eventAboutToBeRemoved(EventConfig *config);

        void globalScriptAdded(VMEScriptConfig *config, const QString &category);
        void globalScriptAboutToBeRemoved(VMEScriptConfig *config);

    public:
        VMEConfig(QObject *parent = 0);

        void addEventConfig(EventConfig *config);
        bool removeEventConfig(EventConfig *config);
        bool contains(EventConfig *config);
        QList<EventConfig *> getEventConfigs() const { return eventConfigs; }
        EventConfig *getEventConfig(int eventIndex) { return eventConfigs.value(eventIndex); }
        EventConfig *getEventConfig(const QString &name) const;
        EventConfig *getEventConfig(const QUuid &id) const;

        ModuleConfig *getModuleConfig(int eventIndex, int moduleIndex);
        QList<ModuleConfig *> getAllModuleConfigs() const;
        QPair<int, int> getEventAndModuleIndices(ModuleConfig *cfg) const;

        void addGlobalScript(VMEScriptConfig *config, const QString &category);
        bool removeGlobalScript(VMEScriptConfig *config);

        /** Known keys for a DAQConfig:
         * "daq_start", "daq_stop", "manual"
         */
        QMap<QString, QList<VMEScriptConfig *>> vmeScriptLists;
        QList<EventConfig *> eventConfigs;

    protected:
        virtual void read_impl(const QJsonObject &json) override;
        virtual void write_impl(QJsonObject &json) const override;
};

#endif
