#ifndef UUID_2e093e44_6f56_4619_8368_5b337f808db3
#define UUID_2e093e44_6f56_4619_8368_5b337f808db3

#include "util.h"
#include "vmecontroller.h"
#include "vmecommandlist.h"
#include <QMap>
#include <QVector>
#include <QString>
#include <QThread>
#include <QDebug>

enum class VMEModuleType
{
    Invalid = 0,
    MADC32  = 1,
    MQDC32  = 2,
    MTDC32  = 3,
    MDPP16  = 4,
    MDPP32  = 5,
    MDI2    = 6,

    // VMUSB_Scaler
    Generic = 1000
};

static const QMap<VMEModuleType, QString> VMEModuleTypeNames =
{
    { VMEModuleType::MADC32,    "MADC32" },
    { VMEModuleType::MQDC32,    "MQDC32" },
    { VMEModuleType::MTDC32,    "MTDC32" },
    { VMEModuleType::MDPP16,    "MDPP16" },
    { VMEModuleType::MDPP32,    "MDPP32" },
    { VMEModuleType::MDI2,      "MDI2" },
    { VMEModuleType::Generic,   "Generic" },
};

class VMEModule
{
    public:
        VMEModule(const QString &name = QString())
            : m_name(name)
        { }
        virtual ~VMEModule() {}
        virtual void resetModule(VMEController *controller) = 0;
        virtual void addInitCommands(VMECommandList *cmdList) = 0;
        virtual void addReadoutCommands(VMECommandList *cmdList) = 0;
        virtual void addStartDaqCommands(VMECommandList *cmdList) = 0;
        virtual void addStopDaqCommands(VMECommandList *cmdList) = 0;

        QString getName() const { return m_name; }
        void setName(const QString &name) { m_name = name; }
        void addMarker(uint32_t marker) { m_markers.push_back(marker); }
        QVector<uint32_t> getMarkers() const { return m_markers; }

    protected:
        QString m_name;
        QVector<uint32_t> m_markers;
};

class HardwareModule: public VMEModule
{
    public:
        HardwareModule(uint32_t baseAddress = 0, const QString &name = QString())
            : VMEModule(name)
            , baseAddress(baseAddress)
        {}

        uint32_t baseAddress = 0;
        VMEModuleType type = VMEModuleType::Generic;
};

enum class RegisterWidth
{
    Width16,
    Width32
};

class GenericModule: public HardwareModule
{
    public:
        GenericModule(uint32_t baseAddress = 0, const QString &name = QString())
            : HardwareModule(baseAddress, name)
        {}

        virtual void resetModule(VMEController *controller)
        { Q_ASSERT(!"Not implemented"); }
        virtual void addInitCommands(VMECommandList *cmdList)
        { Q_ASSERT(!"Not implemented"); }
        virtual void addReadoutCommands(VMECommandList *cmdList)
        { Q_ASSERT(!"Not implemented"); }
        virtual void addStartDaqCommands(VMECommandList *cmdList)
        { Q_ASSERT(!"Not implemented"); }
        virtual void addStopDaqCommands(VMECommandList *cmdList)
        { Q_ASSERT(!"Not implemented"); }

        //uint8_t registerAMod = 0x09;
        //uint8_t bltAMod = 0x0b;
        //uint8_t mbltAmod = 0x08;
        //RegisterWidth registerWidth = RegisterWidth::Width16;
};

class MesytecModule: public HardwareModule
{
    public:
        static const uint8_t registerAMod = 0x09;
        static const uint8_t bltAMod = 0x0b;
        static const uint8_t mbltAMod = 0x08;

        MesytecModule(uint32_t baseAddress = 0, const QString &name = QString())
            : HardwareModule(baseAddress, name)
        {
        }

        virtual void resetModule(VMEController *controller)
        {
            writeRegister(controller, 0x6008, 1);
            QThread::sleep(1);
        }

        virtual void addInitCommands(VMECommandList *cmdList)
        {
            QTextStream input(&initListString);
            InitList initList(parseInitList(input));

            for (auto registerSetting: initList)
            {
                u32 address = registerSetting.first;
                u32 value = registerSetting.second;
                cmdList->addWrite16(baseAddress + address, registerAMod, value);
            }
        }

        virtual void addReadoutCommands(VMECommandList *cmdList)
        {
            // TODO, FIXME: number of transfers?! depends on multi event mode
            cmdList->addFifoRead32(baseAddress, bltAMod, 128);
            for (u32 marker: m_markers)
            {
                cmdList->addMarker(marker);
            }
            cmdList->addWrite16(baseAddress + 0x6034, registerAMod, 1); // readout reset
        }

        virtual void addStartDaqCommands(VMECommandList *cmdList)
        {
            cmdList->addWrite16(baseAddress + 0x603c, registerAMod, 1); // FIFO reset
            cmdList->addWrite16(baseAddress + 0x6034, registerAMod, 1); // readout reset
            cmdList->addWrite16(baseAddress + 0x603a, registerAMod, 1); // start acq
        }

        virtual void addStopDaqCommands(VMECommandList *cmdList)
        {
            cmdList->addWrite16(baseAddress + 0x603a, registerAMod, 0); // stop acq
        }

        void writeRegister(VMEController *controller, uint16_t address, uint16_t value)
        {
            controller->write16(baseAddress + address, registerAMod, value);
        }

        void readRegister(VMEController *controller, uint16_t address)
        {
            controller->read16(baseAddress + address, registerAMod);
        }

        QString initListString;
};

#if 0
class MesytecChain: public VMEModule
{
    public:
        MesytecChain(uint8_t cblt_address, uint8_t mcst_address);
        QVector<MesytecModule *> members;
        uint8_t cblt_address;
        uint8_t mcst_address;
};
#endif

class MADC32: public MesytecModule
{
    public:
        MADC32(uint32_t baseAddress = 0, const QString &name = QString())
            : MesytecModule(baseAddress, name)
        {
            type = VMEModuleType::MADC32;
        }
};

class MQDC32: public MesytecModule
{
    public:
        MQDC32(uint32_t baseAddress = 0, const QString &name = QString())
            : MesytecModule(baseAddress, name)
        {
            type = VMEModuleType::MQDC32;
        }
};

class MTDC32: public MesytecModule
{
    public:
        MTDC32(uint32_t baseAddress = 0, const QString &name = QString())
            : MesytecModule(baseAddress, name)
        {
            type = VMEModuleType::MTDC32;
        }
};

class MDPP16: public MesytecModule
{
    public:
        MDPP16(uint32_t baseAddress = 0, const QString &name = QString())
            : MesytecModule(baseAddress, name)
        {
            type = VMEModuleType::MDPP16;
        }
};


class MDPP32: public MesytecModule
{
    public:
        MDPP32(uint32_t baseAddress = 0, const QString &name = QString())
            : MesytecModule(baseAddress, name)
        {
            type = VMEModuleType::MDPP32;
        }
};

class MDI2: public MesytecModule
{
    public:
        MDI2(uint32_t baseAddress = 0, const QString &name = QString())
            : MesytecModule(baseAddress, name)
        {
            type = VMEModuleType::MDI2;
        }
};

#endif
