/* mvme - Mesytec VME Data Acquisition
 *
 * Copyright (C) 2016-2020 mesytec GmbH & Co. KG <info@mesytec.com>
 *
 * Author: Florian Lüke <f.lueke@mesytec.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#ifndef __MVME_MVLC_READOUT_PARSERS_H__
#define __MVME_MVLC_READOUT_PARSERS_H__

#include "libmvme_mvlc_export.h"
#include "vme_script.h"
#include "mvlc/mvlc_threading.h"
#include "mvlc/mvlc_util.h"
#include "databuffer.h"
#include <array>

namespace mesytec
{
namespace mvme_mvlc
{

/* analysis calls:
 * getVMEObjectSettings(eventConfig | moduleConfig)
 *  -> multi event processing enabled for an event? module enabled? module header filter.
 *     Multi-Event processing will move into a second stage after event reassembly (linearization).
 * processTimetick
 * beginEvent(eventIndex)
 *   processModuleData(eventIndex, moduleIndex, data *, size) // needs linear data
 * endEvent
 *
 * to be added: processModulePrefix(ei, mi, data *, size);
 *              processModuleSuffix(ei, mi, data *, size);
 *
 *
 * Purpose: The reaodut_parser system is used to parse a possibly lossfull
 * sequence of MVLC readout buffers, reassemble complete readout events and
 * make the reassembled data available to a consumer.
 *
 * Commands that produce output:
 *   marker         -> one word
 *   single_read    -> one word
 *   block_read     -> dynamic part (0xF5 framed)
 *
 * Restrictions per module:
 * - one fixed part
 * - one dynamic block part
 * - one fixed part
 *
* For each Event and Module in the VMEConfig build a ModuleReadoutParts
 * structure from the modules VME readout script.
 * The readout for each module must consist of three parts:
 * a fixed size prefix, a single block transfer and a fixed size suffix. Each
 * of the parts is optional.
 */

struct ModuleReadoutParts
{
    u8 prefixLen; // length in words of the fixed part prefix
    u8 suffixLen; // length in words of the fixed part suffix
    bool hasDynamic; // true if a dynamic part (block read) is present
};

inline bool is_empty(const ModuleReadoutParts &mrp)
{
    return mrp.prefixLen == 0 && mrp.suffixLen == 0 && !mrp.hasDynamic;
}

// VME module readout scripts indexed by event and module
using VMEConfReadoutScripts = std::vector<std::vector<vme_script::VMEScript>>;

// ModuleReadoutParts indexed by event and module
using VMEConfReadoutInfo    = std::vector<std::vector<ModuleReadoutParts>>;

LIBMVME_MVLC_EXPORT ModuleReadoutParts parse_module_readout_script(
    const vme_script::VMEScript &readoutScript);

LIBMVME_MVLC_EXPORT VMEConfReadoutInfo parse_vme_readout_info(
    const VMEConfReadoutScripts &rdoScripts);

struct Span
{
    u32 offset;
    u32 size;
};

struct ModuleReadoutSpans
{
    Span prefixSpan;
    Span dynamicSpan;
    Span suffixSpan;
};

inline bool is_empty(const ModuleReadoutSpans &spans)
{
    return (spans.prefixSpan.size == 0
            && spans.dynamicSpan.size == 0
            && spans.suffixSpan.size == 0);
}

struct end_of_frame: public std::exception {};

struct ReadoutParserCallbacks
{
    // functions taking an event index
    std::function<void (int ei)>
        beginEvent = [] (int) {},
        endEvent   = [] (int) {};

    // Parameters: event index, module index, pointer to first word, number of words
    std::function<void (int ei, int mi, const u32 *data, u32 size)>
        modulePrefix  = [] (int, int, const u32*, u32) {},
        moduleDynamic = [] (int, int, const u32*, u32) {},
        moduleSuffix  = [] (int, int, const u32*, u32) {};

    // Parameters: pointer to first word of the system event data, number of words
    std::function<void (u32 *header, u32 size)>
        systemEvent = [] (const u32 *, u32) {};
};

enum class ParseResult
{
    Ok,
    NoHeaderPresent,
    NoStackFrameFound,

    NotAStackFrame,
    NotABlockFrame,
    NotAStackContinuation,
    StackIndexChanged,
    EventIndexOutOfRange,
    ModuleIndexOutOfRange,
    EmptyStackFrame,
    UnexpectedOpenBlockFrame,

    // IMPORTANT: These should not happen and be fixed in the code if they
    // happen. They indicate that the parser algorithm did not advance through
    // the buffer but is stuck in place, parsing the same data again.
    ParseReadoutContentsNotAdvancing,
    ParseEthBufferNotAdvancing,
    ParseEthPacketNotAdvancing,

    ParseResultMax
};

LIBMVME_MVLC_EXPORT const char *get_parse_result_name(const ParseResult &pr);

struct LIBMVME_MVLC_EXPORT ReadoutParserCounters
{
    u32 internalBufferLoss;
    u32 buffersProcessed;
    u64 unusedBytes;

    u32 ethPacketLoss;
    u32 ethPacketsProcessed;

    std::array<u32, system_event::subtype::SubtypeMax + 1> systemEventTypes;

    using ParseResultArray = std::array<u32, static_cast<size_t>(ParseResult::ParseResultMax)>;
    ParseResultArray parseResults;
    u32 parserExceptions;
};

struct LIBMVME_MVLC_EXPORT ReadoutParserState
{
    // Helper structure keeping track of the number of words left in a MVLC
    // style data frame.
    struct FrameParseState
    {
        FrameParseState(u32 frameHeader = 0)
            : header(frameHeader)
            , wordsLeft(extract_frame_info(frameHeader).len)
        {}

        inline explicit operator bool() const { return wordsLeft; }
        inline FrameInfo info() const { return extract_frame_info(header); }

        inline void consumeWord()
        {
            if (wordsLeft == 0)
                throw end_of_frame();
            --wordsLeft;
        }

        u32 header;
        u16 wordsLeft;
    };

    enum ModuleParseState { Prefix, Dynamic, Suffix };

    // The readout workers start with buffer number 1 so buffer 0 can only
    // occur after wrapping the counter. By using 0 as a starting value the
    // buffer loss calculation will work without special cases.
    u32 lastBufferNumber = 0;

    // Space to assemble linear readout data.
    DataBuffer workBuffer;

    // Current output offset into the workbuffer
    u32 workBufferOffset = 0;

    // Per module offsets and sizes into the workbuffer. This is a map of the
    // current layout of the workbuffer.
    std::vector<ModuleReadoutSpans> readoutDataSpans;

    // Per event preparsed module readout info.
    VMEConfReadoutInfo readoutInfo;

    int eventIndex = -1;
    int moduleIndex = -1;
    ModuleParseState moduleParseState = Prefix;

    // Parsing state of the current 0xF3 stack frame. This is always active
    // when parsing readout data.
    FrameParseState curStackFrame = {};

    // Parsing state of the current 0xF5 block readout frame. This is only
    // active when parsing the dynamic part of a module readout.
    FrameParseState curBlockFrame = {};

    // ETH parsing only. The transmitted packet number type is u16. Using an
    // s32 here to represent the "no previous packet" case by storing a -1.
    s32 lastPacketNumber = -1;

    ReadoutParserCounters counters = {};
};

LIBMVME_MVLC_EXPORT ReadoutParserState make_readout_parser(const VMEConfReadoutScripts &readoutScripts);

LIBMVME_MVLC_EXPORT ParseResult parse_readout_buffer_eth(
    ReadoutParserState &state,
    ReadoutParserCallbacks &callbacks,
    u32 bufferNumber, u8 *buffer, size_t bufferSize);

LIBMVME_MVLC_EXPORT ParseResult parse_readout_buffer_usb(
    ReadoutParserState &state,
    ReadoutParserCallbacks &callbacks,
    u32 bufferNumber, u8 *buffer, size_t bufferSize);

} // end namespace mesytec
} // end namespace mvme_mvlc

#endif /* __MVME_MVLC_READOUT_PARSERS_H__ */
