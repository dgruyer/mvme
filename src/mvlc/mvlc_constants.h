#ifndef __MVLC_CONSTANTS_H__
#define __MVLC_CONSTANTS_H__

#include <cstddef>
#include "typedefs.h"

namespace mesytec
{
namespace mvlc
{
// Communication with the MVLC is done using 32-bit wide binary data words.
// Results from commands and stack executions are also 32-bit aligned.

static const u32 AddressIncrement = 4;
static const u32 ReadLocalBlockMaxWords = 768;
static const u32 BufferSizeMask = 0xFFFF;

// Super commands are commands that are directly interpreted and executed
// by the MVLC.
// The values in the SuperCommands enum contain the 2 high bytes of the
// command word.
// The output of super commands always goes to pipe 0, the CommandPipe.
static const u32 SuperCmdMask  = 0xFFFF;
static const u32 SuperCmdShift = 16;
static const u32 SuperCmdArgMask = 0xFFFF;
static const u32 SuperCmdArgShift = 0;

namespace super_commands
{
    enum SuperCommands: u32
    {
        CmdBufferStart = 0xF100,
        CmdBufferEnd   = 0xF200,
        ReferenceWord  = 0x0101,
        ReadLocal      = 0x0102,
        ReadLocalBlock = 0x0103,
        WriteLocal     = 0x0204,
        WriteReset     = 0x0206,
    };
} // end namespace supercommands

// Stack-only commands. These can be written into the stack memory area
// starting from StackMemoryBegin using WriteLocal commands.
//
// The output produced by a stack execution can go to either the
// CommandPipe or the DataPipe. This is encoded in the StackStart command.

static const u32 CmdMask      = 0xFF;
static const u32 CmdShift     = 24;
static const u32 CmdArg0Mask  = 0x00FF;
static const u32 CmdArg0Shift = 16;
static const u32 CmdArg1Mask  = 0x0000FFFF;
static const u32 CmdArg1Shift = 0;

namespace commands
{
    enum Commands: u32
    {
        StackStart      = 0xF3,
        StackEnd        = 0xF4,
        VMEWrite        = 0x23,
        VMERead         = 0x12,
        WriteMarker     = 0xC2,
        WriteSpecial    = 0xC1,
    // TODO: ScanDataRead, ReadDataLoop and masks/enums
    //static const u32 ScanDataRead      = 0x34;
    };
};

namespace buffer_types
{
    enum BufferTypes: u8
    {
        SuperBuffer = 0xF1,
        StackBuffer = 0xF3,
        BlockRead   = 0xF5,
        StackError  = 0xF7,
    };

    static const u8 TypeShift = 24;
}

enum VMEDataWidth
{
    D16 = 0x1,
    D32 = 0x2
};

enum Blk2eSSTRate: u8
{
    Rate160MB,
    Rate276MB,
    Rate300MB,
};

// Shift relative to the AddressMode argument of the read.
static const u32 Blk2eSSTRateShift = 6;

// For the WriteSpecial command
enum SpecialWord: u8
{
    Timestamp,
    StackTriggers
};

static const u32 InternalRegisterMin = 0x0001;
static const u32 InternalRegisterMax = 0x5FFF;

// Setting bit 0 to 1 enables autonomous execution of stacks in
// reaction to triggers.
// IMPORTANT: This is always active right now.
static const u32 DAQModeEnableRegister = 0x1300;

namespace stacks
{
    static const u32 StackCount = 8;
    static const u32 Stack0TriggerRegister = 0x1100;
    // Note: The stack offset registers take offsets from StackMemoryBegin,
    // not absolute memory addresses.
    static const u32 Stack0OffsetRegister  = 0x1200;
    static const u32 StackMemoryBegin      = 0x2000;
    static const u32 StackMemoryWords      = 1024;
    static const u32 StackMemoryBytes      = StackMemoryWords * 4;
    static const u32 StackMemoryEnd        = StackMemoryBegin + StackMemoryBytes;
    // Mask for the number of valid bits in the stack offset register.
    // Higher order bits outside the mask are ignored by the MVLC.
    static const u32 StackOffsetBitMask    = 0x03FF;

    static const u32 ImmediateStackID = 0;
    static const u32 ImmediateStackWords = 64;

    enum TriggerType: u8
    {
        NoTrigger,
        IRQ,
        IRQNoIACK,
        External,
        TimerUnderrun,
    };

    // IMPORTANT: The trigger bits have to be set to (IRQ - 1), e.g. value 0
    // for IRQ1!
    static const u32 TriggerBitsMask    = 0b11111;
    static const u32 TriggerBitsShift   = 0;
    static const u32 TriggerTypeMask    = 0b111;
    static const u32 TriggerTypeShift   = 5;
    static const u32 ImmediateMask      = 0b1;
    static const u32 ImmediateShift     = 8;
}

static const u32 SelfVMEAddress       = 0xFFFF0000u;

namespace usb
{
    // Limit imposed by FT_WritePipeEx and FT_ReadPipeEx under Linux
    static const size_t USBSingleTransferMaxBytes = 1 * 1024 * 1024;
    static const size_t USBSingleTransferMaxWords = USBSingleTransferMaxBytes / sizeof(u32);
} // end namespace usb

namespace udp
{
    static const u16 CommandPort = 0x8000; // 32768
    static const u16 DataPort = CommandPort + 1;
    static const u32 HeaderWords = 2;
    static const u32 HeaderBytes = HeaderWords * sizeof(u32);

    namespace header0
    {
        // 12 bit packet number
        // Pipe specific incrementing packet number.
        static const u32 PacketNumberMask  = 0xfff;
        static const u32 PacketNumberShift = 16;
        // 12 bit number of data words
        // This is the number of data words following the two header words.
        static const u32 NumDataWordsMask  = 0xfff;
        static const u32 NumDataWordsShift = 0;
    }

    namespace header1
    {
        // 20 bit UDP timestamp. Increments in 1ms steps. Wrap after 17.5
        // minutes.
        static const u32 TimestampMask      = 0xfffff;
        static const u32 TimestampShift     = 12;
        // Points to the next payload header word. The position directly after
        // this header word is 0.
        // TODO: verify and possibly reword this
        // Note that this value can point to an offset that's outside of this
        // packets range. This means the packet does not contain a payload
        // header but was the continuation of a data section started previously.
        static const u32 HeaderPointerMask  = 0xfff;
        static const u32 HeaderPointerShift = 0;
    }

    static const size_t JumboFrameMaxSize = 9000;
    static const size_t UDPSingleTransferMaxBytes = 1 * 1024 * 1024;
    static const size_t UDPSingleTransferMaxWords = UDPSingleTransferMaxBytes / sizeof(u32);
} // end namespace udp

namespace eth_registers
{
    static const u32 OwnIP_lo           = 0x4400;
    static const u32 OwnIP_hi           = 0x4402;
    static const u32 StoreIPInFlash     = 0x4404;
    // 0 = fixed IP, 1 = DHCP
    static const u32 IPMode             = 0x4406;
} // end namespace eth_registers

enum class Pipe: u8
{
    Command,
    Data
};

static const unsigned PipeCount = 2;
static const u8 CommandPipe = 0;
static const u8 DataPipe = 1;

static const unsigned DefaultWriteTimeout_ms = 1000;
static const unsigned DefaultReadTimeout_ms  = 1000;

enum class ConnectionType
{
    USB,
    UDP
};

} // end namespace mvlc
} // end namespace mesytec

#endif /* __MVLC_CONSTANTS_H__ */
