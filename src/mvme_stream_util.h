#ifndef __MVME_STREAM_UTIL_H__
#define __MVME_STREAM_UTIL_H__

#include <cassert>
#include <stdexcept>

#include "libmvme_export.h"
#include "databuffer.h"
#include "mvme_listfile.h"
#include "mvme_stream_iter.h"

/* Utility class to be used by readout workers to ease and unify listfile
 * generation. */
class LIBMVME_EXPORT MVMEStreamWriterHelper
{
    public:
        using LF = listfile_v1;

        enum Flags
        {
            ResultOk,
            EventSizeExceeded,
            ModuleSizeExceeded,
            NestingError
        };

        struct CloseResult
        {
            Flags flags = ResultOk;
            u32 sectionBytes = 0;
        };

        MVMEStreamWriterHelper(DataBuffer *outputBuffer = nullptr)
            : m_outputBuffer(outputBuffer)
            , m_eventSize(0)
            , m_moduleSize(0)
            , m_eventHeaderOffset(-1)
            , m_moduleHeaderOffset(-1)
        { }

        void setOutputBuffer(DataBuffer *outputBuffer)
        {
            m_outputBuffer = outputBuffer;
        }

        DataBuffer *outputBuffer() const { return m_outputBuffer; }

        inline u32 eventSize() const { return m_eventSize; }
        inline u32 moduleSize() const { return m_moduleSize; }
        inline s32 eventHeaderOffset() const { return m_eventHeaderOffset; }
        inline s32 moduleHeaderOffset() const { return m_moduleHeaderOffset; }

        inline Flags openEventSection(int eventIndex)
        {
            assert(m_outputBuffer);

            if (hasOpenEventSection() || hasOpenModuleSection())
                return NestingError;

            m_eventHeaderOffset = m_outputBuffer->used;
            u32 *eventHeader = m_outputBuffer->asU32();
            m_outputBuffer->used += sizeof(u32);
            m_eventSize = 0;

            *eventHeader = ((ListfileSections::SectionType_Event << LF::SectionTypeShift) & LF::SectionTypeMask)
                | ((eventIndex << LF::EventTypeShift) & LF::EventTypeMask);

            return ResultOk;
        }

        inline CloseResult closeEventSection()
        {
            assert(m_outputBuffer);

            if (!hasOpenEventSection() || hasOpenModuleSection())
                return { NestingError, 0 };

            u32 *eventHeader = m_outputBuffer->asU32(m_eventHeaderOffset);
            *eventHeader |= (m_eventSize << LF::SectionSizeShift) & LF::SectionSizeMask;
            m_eventHeaderOffset = -1;

            return { ResultOk, static_cast<u32>(m_eventSize * sizeof(u32)) };
        }

        inline Flags openModuleSection(u32 moduleType)
        {
            assert(m_outputBuffer);

            if (!hasOpenEventSection() || hasOpenModuleSection())
                return NestingError;

            if (m_eventSize >= LF::SectionMaxWords)
                return EventSizeExceeded;

            m_moduleHeaderOffset = m_outputBuffer->used;
            u32 *moduleHeader = m_outputBuffer->asU32();
            m_outputBuffer->used += sizeof(u32);
            m_eventSize++;
            m_moduleSize = 0;

            *moduleHeader = (moduleType << LF::ModuleTypeShift) & LF::ModuleTypeMask;

            return ResultOk;
        }

        inline CloseResult closeModuleSection()
        {
            assert(m_outputBuffer);

            if (!hasOpenEventSection() || !hasOpenModuleSection())
                return { NestingError, 0 };

            u32 *moduleHeader = m_outputBuffer->asU32(m_moduleHeaderOffset);
            *moduleHeader |= (m_moduleSize << LF::SubEventSizeShift) & LF::SubEventSizeMask;
            m_moduleHeaderOffset = -1;

            return { ResultOk, static_cast<u32>(m_moduleSize * sizeof(u32)) };
        }

        inline Flags writeEventData(u32 dataWord)
        {
            assert(m_outputBuffer);

            if (!hasOpenEventSection() || hasOpenModuleSection())
                return NestingError;

            if (m_eventSize >= LF::SectionMaxWords)
                return EventSizeExceeded;

            *m_outputBuffer->asU32() = dataWord;
            m_outputBuffer->used += sizeof(u32);
            m_eventSize++;

            return ResultOk;
        }

        inline Flags writeModuleData(u32 dataWord)
        {
            assert(m_outputBuffer);

            if (!hasOpenEventSection() || !hasOpenModuleSection())
                return NestingError;

            if (m_eventSize >= LF::SectionMaxWords)
                return EventSizeExceeded;

            if (m_moduleSize >= LF::SubEventMaxWords)
                return ModuleSizeExceeded;

            *m_outputBuffer->asU32() = dataWord;
            m_outputBuffer->used += sizeof(u32);
            m_eventSize++;
            m_moduleSize++;

            return ResultOk;
        }

        inline bool hasOpenEventSection() const { return m_eventHeaderOffset >= 0; }
        inline bool hasOpenModuleSection() const { return m_moduleHeaderOffset >= 0; }

    private:
        DataBuffer *m_outputBuffer;
        u32 m_eventSize;
        u32 m_moduleSize;
        s32 m_eventHeaderOffset;
        s32 m_moduleHeaderOffset;
};

LIBMVME_EXPORT mvme_stream::StreamInfo streaminfo_from_vmeconfig(VMEConfig *vmeConfig, u32 listfileVersion = CurrentListfileVersion);
LIBMVME_EXPORT mvme_stream::StreamInfo streaminfo_from_listfile(ListFile *listfile);

#endif /* __MVME_STREAM_UTIL_H__ */
