#ifndef UUID_0ba96895_944a_4a3b_9601_c7cae6a84867
#define UUID_0ba96895_944a_4a3b_9601_c7cae6a84867

#include "util.h"
#include <QVector>
class QTextStream;

struct VMECommand
{
    enum Type
    {
        NotSet,
        Write32,
        Write16,
        Read32,
        Read16,
        BlockRead32,
        FifoRead32,
        BlockCountRead16,
        BlockCountRead32,
        MaskedCountBlockRead32,
        MaskedCountFifoRead32,
        Delay,
        Marker,
    };

    Type type = NotSet;
    uint32_t address = 0;
    uint32_t value = 0;
    uint8_t  amod = 0;
    size_t transfers = 0;
    uint32_t blockCountMask = 0;
    uint8_t delay200nsClocks = 0;

    QString toString() const;
};

class VMECommandList
{
    public:
        void addWrite32(uint32_t address, uint8_t amod, uint32_t value)
        {
            VMECommand cmd;
            cmd.type = VMECommand::Write32;
            cmd.address = address;
            cmd.value   = value;
            cmd.amod    = amod;
            commands.push_back(cmd);
        }

        void addWrite16(uint32_t address, uint8_t amod, uint16_t value)
        {
            VMECommand cmd;
            cmd.type = VMECommand::Write16;
            cmd.address = address;
            cmd.value   = value;
            cmd.amod    = amod;
            commands.push_back(cmd);
        }

        void addRead32(uint32_t address, uint8_t amod = 0x09)
        {
            VMECommand cmd;
            cmd.type = VMECommand::Read32;
            cmd.address = address;
            cmd.amod    = amod;
            commands.push_back(cmd);
        }
        void addRead16(uint32_t address, uint8_t amod = 0x09)
        {
            VMECommand cmd;
            cmd.type = VMECommand::Read16;
            cmd.address = address;
            cmd.amod    = amod;
            commands.push_back(cmd);
        }

        void addBlockRead32(uint32_t baseAddress, uint8_t amod, size_t transfers)
        {
            VMECommand cmd;
            cmd.type = VMECommand::BlockRead32;
            cmd.address = baseAddress;
            cmd.amod    = amod;
            cmd.transfers = transfers;
            commands.push_back(cmd);
        }
        void addFifoRead32(uint32_t  baseAddress, uint8_t amod, size_t transfers)
        {
            VMECommand cmd;
            cmd.type = VMECommand::FifoRead32;
            cmd.address = baseAddress;
            cmd.amod    = amod;
            cmd.transfers = transfers;
            commands.push_back(cmd);
        }

        void addBlockCountRead16(uint32_t address, uint16_t mask, uint8_t amod)
        {
            VMECommand cmd;
            cmd.type = VMECommand::BlockCountRead16;
            cmd.address = address;
            cmd.amod    = amod;
            cmd.blockCountMask = mask;
            commands.push_back(cmd);
        }
        void addBlockCountRead32(uint32_t address, uint32_t mask, uint8_t amod)
        {
            VMECommand cmd;
            cmd.type = VMECommand::BlockRead32;
            cmd.address = address;
            cmd.amod    = amod;
            cmd.blockCountMask = mask;
            commands.push_back(cmd);
        }

        void addMaskedCountBlockRead32(uint32_t address, uint8_t amod)
        {
            VMECommand cmd;
            cmd.type = VMECommand::MaskedCountBlockRead32;
            cmd.address = address;
            cmd.amod    = amod;
            commands.push_back(cmd);
        }
        void addMaskedCountFifoRead32(uint32_t address, uint8_t amod)
        {
            VMECommand cmd;
            cmd.type = VMECommand::MaskedCountFifoRead32;
            cmd.address = address;
            cmd.amod    = amod;
            commands.push_back(cmd);
        }

        void addDelay(uint8_t delay200nsClocks)
        {
            VMECommand cmd;
            cmd.type = VMECommand::Delay;
            cmd.delay200nsClocks = delay200nsClocks;
            commands.push_back(cmd);
        }
        void addMarker(uint32_t marker)
        {
            VMECommand cmd;
            cmd.type = VMECommand::Marker;
            cmd.value   = marker;
            commands.push_back(cmd);
        }

        void append(const VMECommandList &other)
        {
            commands += other.commands;
        }

        size_t size() const { return (size_t) commands.size(); }

        static VMECommandList fromInitList(const InitList &initList, uint32_t baseAddress, uint8_t amod = 0x09)
        {
            VMECommandList ret;
            for (auto p: initList)
            {
                ret.addWrite16(baseAddress + p.first, amod, p.second);
            }
            return ret;
        }

        QVector<VMECommand> commands;

        QTextStream &dump(QTextStream &out);
};


#endif
