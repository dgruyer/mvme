#include "mvme_config.h"
#include "vmecommandlist.h"
#include "CVMUSBReadoutList.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

//
// ModuleConfig
//
QString ModuleConfig::getFullPath() const
{
    if (event)
    {
        return QString("%1.%2")
            .arg(event->getName())
            .arg(m_name);
    }

    return m_name;
}

int ModuleConfig::getNumberOfChannels() const
{
    switch (type)
    {
        case VMEModuleType::MADC32:
        case VMEModuleType::MQDC32:
        case VMEModuleType::MTDC32:
        case VMEModuleType::MDI2:
            return 34; // TODO: verify

        case VMEModuleType::MDPP16:
        case VMEModuleType::MDPP32:
            return 34;

        case VMEModuleType::VHS4030p:
            return -1;

        case VMEModuleType::Invalid:
            return -1;
    }
    return -1;
}

namespace MADC
{
    static const int adc_resolution = 0x6042;
    static const int adc_override = 0x6046;
    static const std::array<int, 5> adc_bits = {
        11, // 2k
        12, // 4k
        12, // 4k hires
        13, // 8k
        13  // 8k hires
    };
    static const int adc_resolution_default = 2;
}

namespace MDPP
{
    static const int adc_resolution = 0x6046;
    static const std::array<int, 5> adc_bits = {
        16,
        15,
        14,
        13,
        12
    };
    static const int adc_resolution_default = 4;
}

void ModuleConfig::updateRegisterCache()
{
    RegisterList allRegisters;
    allRegisters += parseRegisterList(initReset);
    allRegisters += parseRegisterList(initParameters);
    allRegisters += parseRegisterList(initReadout);
    allRegisters += parseRegisterList(initStartDaq);
    allRegisters += parseRegisterList(initStopDaq);

    m_registerCache.clear();

    for (auto p: allRegisters)
    {
        m_registerCache[p.first] = p.second;
    }
}

int ModuleConfig::getDataBits() const
{
    switch (type)
    {
        case VMEModuleType::MADC32:
            {
                int regValue = m_registerCache.value(MADC::adc_resolution, MADC::adc_resolution_default).toInt();
                regValue = m_registerCache.value(MADC::adc_override, regValue).toInt();
                int bits = MADC::adc_bits.at(regValue);
                return bits;
            }

        case VMEModuleType::MDPP16:
        case VMEModuleType::MDPP32:
            {
                int index = m_registerCache.value(MDPP::adc_resolution, MDPP::adc_resolution_default).toInt();
                int bits = MDPP::adc_bits.at(index);
                return bits;
            }
        case VMEModuleType::MQDC32:
            return 12;

        case VMEModuleType::MTDC32:
            // Note: does not have an ADC resolution. Produces 16-bit wide timestamps
            return 16;

        case VMEModuleType::MDI2:
            return 12;

        case VMEModuleType::Invalid:
        case VMEModuleType::VHS4030p:
            break;
    }
    return -1;
}

u32 ModuleConfig::getDataExtractMask()
{
    switch (type)
    {
        case VMEModuleType::MADC32:
            return (1 << 13) - 1;

        case VMEModuleType::MDPP16:
        case VMEModuleType::MDPP32:
        case VMEModuleType::MTDC32:
            return (1 << 16) - 1;

        case VMEModuleType::MDI2:
        case VMEModuleType::MQDC32:
            return (1 << 12) - 1;

        case VMEModuleType::Invalid:
        case VMEModuleType::VHS4030p:
            break;
    }
    return 0;
}

u8 ModuleConfig::getRegisterAddressModifier() const
{
    if (type == VMEModuleType::VHS4030p)
    {
        return VME_AM_A16_USER;
    }

    return VME_AM_A32_USER_DATA;
}

RegisterWidth ModuleConfig::getRegisterWidth() const
{
    return RegisterWidth::W16;
}

void ModuleConfig::setModified()
{
    updateRegisterCache();

    auto eventConfig = qobject_cast<EventConfig *>(parent());

    if (eventConfig)
    {
        eventConfig->setModified();
    }

    emit modified();
}

void ModuleConfig::read(const QJsonObject &json)
{
    type = VMEModuleShortNames.key(json["type"].toString(), VMEModuleType::Invalid);
    m_id = QUuid(json["id"].toString());
    if (m_id.isNull())
        m_id = QUuid::createUuid();
    m_name = json["name"].toString();
    baseAddress = json["baseAddress"].toInt();
    mcstAddress = json["mcstAddress"].toInt();
    initReset = json["initReset"].toString();
    initParameters = json["initParameters"].toString();
    initReadout = json["initReadout"].toString();
    initStartDaq = json["initStartDaq"].toString();
    initStopDaq = json["initStopDaq"].toString();
    readoutStack = json["readoutStack"].toString();
    updateRegisterCache();
}

void ModuleConfig::write(QJsonObject &json) const
{
    json["type"] = VMEModuleShortNames.value(type, "invalid");
    json["id"] = m_id.toString();
    json["name"] = m_name;
    json["baseAddress"] = static_cast<qint64>(baseAddress);
    json["mcstAddress"] = static_cast<qint64>(mcstAddress);
    json["initReset"] = initReset;
    json["initParameters"] = initParameters;
    json["initReadout"] = initReadout;
    json["initStartDaq"] = initStartDaq;
    json["initStopDaq"] = initStopDaq;
    json["readoutStack"] = readoutStack;
}


void ModuleConfig::generateReadoutStack()
{
    if (isMesytecModule(type))
    {
        VMECommandList readoutCmds;
        readoutCmds.addFifoRead32(baseAddress, FifoReadTransferSize);
        readoutCmds.addMarker(EndMarker);
        readoutCmds.addWrite16(baseAddress + 0x6034, 1);
        CVMUSBReadoutList readoutList(readoutCmds);
        readoutStack = readoutList.toString();
        setModified();
    }

    if (type == VMEModuleType::VHS4030p)
    {
        // TODO: read channel voltages and currents here
        //VMECommandList cmds;

        //cmds.addRead16(baseAddress + 0x

    }
}

//
// EventConfig
//

void EventConfig::setModified()
{
    auto daqConfig = qobject_cast<DAQConfig *>(parent());
    if (daqConfig)
    {
        daqConfig->setModified(true);
    }
    emit modified();
}

void EventConfig::read(const QJsonObject &json)
{
    qDeleteAll(modules);
    modules.clear();

    m_id = QUuid(json["id"].toString());
    if (m_id.isNull())
        m_id = QUuid::createUuid();

    m_name = json["name"].toString();
    triggerCondition = static_cast<TriggerCondition>(json["triggerCondition"].toInt());
    irqLevel = json["irqLevel"].toInt();
    irqVector = json["irqVector"].toInt();
    scalerReadoutPeriod = json["scalerReadoutPeriod"].toInt();
    scalerReadoutFrequency = json["scalerReadoutFrequency"].toInt();

    QJsonArray moduleArray = json["modules"].toArray();
    for (int i=0; i<moduleArray.size(); ++i)
    {
        QJsonObject moduleObject = moduleArray[i].toObject();
        ModuleConfig *moduleConfig = new ModuleConfig(this);
        moduleConfig->read(moduleObject);
        moduleConfig->event = this;
        modules.append(moduleConfig);
    }
}

void EventConfig::write(QJsonObject &json) const
{
    json["name"] = m_name;
    json["id"] = m_id.toString();
    json["triggerCondition"] = static_cast<int>(triggerCondition);
    json["irqLevel"] = irqLevel;
    json["irqVector"] = irqVector;
    json["scalerReadoutPeriod"] = scalerReadoutPeriod;
    json["scalerReadoutFrequency"] = scalerReadoutFrequency;

    QJsonArray moduleArray;

    for (auto module: modules)
    {
        QJsonObject moduleObject;
        module->write(moduleObject);
        moduleArray.append(moduleObject);
    }
    json["modules"] = moduleArray;
}


//
// DAQConfig
//
void DAQConfig::setModified(bool b)
{
    if (m_isModified != b)
    {
        m_isModified = b;
        emit modifiedChanged(b);
    }
}

void DAQConfig::read(const QJsonObject &json)
{
    qDeleteAll(m_eventConfigs);
    m_eventConfigs.clear();

    m_listFileOutputDirectory = json["listFileOutputDirectory"].toString();
    m_listFileOutputEnabled = json["listFileOutputEnabled"].toBool();

    QJsonArray eventArray = json["events"].toArray();

    for (int eventIndex=0; eventIndex<eventArray.size(); ++eventIndex)
    {
        QJsonObject eventObject = eventArray[eventIndex].toObject();
        EventConfig *eventConfig = new EventConfig(this);
        eventConfig->read(eventObject);
        m_eventConfigs.append(eventConfig);
    }
}

void DAQConfig::write(QJsonObject &json) const
{
    json["listFileOutputDirectory"] = m_listFileOutputDirectory;
    json["listFileOutputEnabled"] = m_listFileOutputEnabled;

    QJsonArray eventArray;
    for (auto event: m_eventConfigs)
    {
        QJsonObject eventObject;
        event->write(eventObject);
        eventArray.append(eventObject);
    }
    json["events"] = eventArray;
}

QByteArray DAQConfig::toJson() const
{
    QJsonObject configObject;
    write(configObject);
    QJsonDocument doc(configObject);
    return doc.toJson();
}

ModuleConfig *DAQConfig::getModuleConfig(int eventIndex, int moduleIndex)
{
    ModuleConfig *result = 0;
    auto eventConfig = m_eventConfigs.value(eventIndex);

    if (eventConfig)
    {
        result = eventConfig->modules.value(moduleIndex);
    }

    return result;
}

EventConfig *DAQConfig::getEventConfig(const QString &name) const
{
    for (auto cfg: m_eventConfigs)
    {
        if (cfg->getName() == name)
            return cfg;
    }
    return nullptr;
}

QVector<ModuleConfig *> DAQConfig::getAllModuleConfigs() const
{
    QVector<ModuleConfig *> result;

    for (auto eventConfig: m_eventConfigs)
    {
        for (auto moduleConfig: eventConfig->modules)
        {
            result.push_back(moduleConfig);
        }
    }

    return result;
}
