#ifndef __MVLC_DIALOG_H__
#define __MVLC_DIALOG_H__

#include <functional>
#include <QVector>
#include "mvlc/mvlc_qt_object.h"

namespace mesytec
{
namespace mvlc
{

std::error_code check_mirror(const QVector<u32> &request, const QVector<u32> &response);

class MVLCDialog
{
    public:
        using BufferHeaderValidator = std::function<bool (u32 header)>;

        MVLCDialog(MVLCObject *mvlc);

        // MVLC register access
        std::error_code readRegister(u32 address, u32 &value);
        std::error_code writeRegister(u32 address, u32 value);

        // Higher level VME access
        std::error_code vmeSingleRead(u32 address, u32 &value, AddressMode amod,
                                      VMEDataWidth dataWidth);

        std::error_code vmeSingleWrite(u32 address, u32 value, AddressMode amod,
                                       VMEDataWidth dataWidth);

        std::error_code vmeBlockRead(u32 address, AddressMode amod, u16 maxTransfers,
                                     QVector<u32> &dest);

        // Lower level utilities

        // Read a full response buffer into dest. The buffer header is passed
        // to the validator before attempting to read the rest of the response.
        // If validation fails no more data is read.
        std::error_code readResponse(BufferHeaderValidator bhv, QVector<u32> &dest);

        // Send the given cmdBuffer to the MVLC and read and verify the mirror
        // response. The buffer must start with CmdBufferStart and end with
        // CmdBufferEnd, otherwise the MVLC cannot interpret it.
        std::error_code mirrorTransaction(const QVector<u32> &cmdBuffer,
                                          QVector<u32> &responseDest);

        // Sends the given stack data (which must include upload commands),
        // reads and verifies the mirror response, and executes the stack.
        // IMPORTANT: Stack0 is used and offset 0 into stack memory is assumed.
        std::error_code stackTransaction(const QVector<u32> &stackUploadData,
                                         QVector<u32> &responseDest);

        // Returns the response buffer which will contain the contents of the
        // last read from the MVLC.
        // After mirrorTransaction() the buffer will contain the mirror
        // response. After stackTransaction() the buffer will contain the
        // response from executing the stack.
        QVector<u32> getResponseBuffer() const { return m_responseBuffer; }

        QVector<QVector<u32>> getStackErrorNotifications() const
        {
            return m_stackErrorNotifications;
        }

        void clearStackErrorNotifications()
        {
            m_stackErrorNotifications.clear();
        }

    private:
        std::error_code doWrite(const QVector<u32> &buffer);
        std::error_code readWords(u32 *dest, size_t count, size_t &wordsTransferred);
        std::error_code readKnownBuffer(QVector<u32> &dest);

        void logBuffer(const QVector<u32> &buffer, const QString &info);

        MVLCObject *m_mvlc;
        u32 m_referenceWord = 1;
        QVector<u32> m_responseBuffer;
        QVector<QVector<u32>> m_stackErrorNotifications;
};

// BufferHeaderValidators

inline bool is_super_buffer(u32 header)
{
    return (header >> buffer_types::TypeShift) == buffer_types::SuperBuffer;
}

inline bool is_stack_buffer(u32 header)
{
    return (header >> buffer_types::TypeShift) == buffer_types::StackBuffer;
}

inline bool is_blockread_buffer(u32 header)
{
    return (header >> buffer_types::TypeShift) == buffer_types::BlockRead;
}

inline bool is_stackerror_notification(u32 header)
{
    return (header >> buffer_types::TypeShift) == buffer_types::StackError;
}

inline bool is_known_buffer(u32 header)
{
    const u8 type = (header >> buffer_types::TypeShift);

    return (type == buffer_types::SuperBuffer
            || type == buffer_types::StackBuffer
            || type == buffer_types::BlockRead
            || type == buffer_types::StackError);
}

} // end namespace mvlc
} // end namespace mesytec

#endif /* __MVLC_DIALOG_H__ */
