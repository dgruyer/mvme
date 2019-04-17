#ifndef __MVLC_UTIL_CORE_H__
#define __MVLC_UTIL_CORE_H__

#include <functional>
#include "mvlc/mvlc_constants.h"

namespace mesytec
{
namespace mvlc
{

using BufferHeaderValidator = std::function<bool (u32 header)>;

// BufferHeaderValidators

inline bool is_super_buffer(u32 header)
{
    return (header >> buffer_headers::TypeShift) == buffer_headers::SuperBuffer;
}

inline bool is_stack_buffer(u32 header)
{
    return (header >> buffer_headers::TypeShift) == buffer_headers::StackBuffer;
}

inline bool is_blockread_buffer(u32 header)
{
    return (header >> buffer_headers::TypeShift) == buffer_headers::BlockRead;
}

inline bool is_stackerror_notification(u32 header)
{
    return (header >> buffer_headers::TypeShift) == buffer_headers::StackError;
}

inline bool is_known_buffer_header(u32 header)
{
    const u8 type = (header >> buffer_headers::TypeShift);

    return (type == buffer_headers::SuperBuffer
            || type == buffer_headers::StackBuffer
            || type == buffer_headers::BlockRead
            || type == buffer_headers::StackError);
}

} // end namespace mvlc
} // end namespace mesytec

#endif /* __MVLC_UTIL_CORE_H__ */
