#ifndef __MVLC_ERROR_H__
#define __MVLC_ERROR_H__

#include <system_error>
#include "libmvme_mvlc_export.h"

namespace mesytec
{
namespace mvlc
{

/* Lower level MVLC specific error codes. In addition to this the specific
 * implementations (USB, UDP) use their own detailed error codes. */
enum class MVLCErrorCode
{
    NoError,
    IsConnected,
    IsDisconnected,
    ShortWrite,
    ShortRead,
    MirrorEmptyRequest,  // size of the request < 1
    MirrorEmptyResponse, // size of the mirror response < 1
    MirrorShortResponse,
    MirrorNotEqual,
    InvalidBufferHeader,
    NoResponseReceived,
    UnexpectedResponseSize, // wanted N words, got M words
    CommandArgOutOfRange,
    InvalidPipe,
    NoVMEResponse,
    HostLookupError,
    EmptyHostname,
    BindLocalError,
    SocketError,
    SocketReadTimeout,
    SocketWriteTimeout,
    UDPPacketChannelOutOfRange,
    StackCountExceeded,
    StackMemoryExceeded,
    StackSyntaxError,
    InvalidStackHeader,

    // Readout setup releated (e.g. mvlc_daq.cc)
    TimerCountExceeded,
    ReadoutSetupError,

    // TODO: move these into a readout worker error enum. This is not really
    // MVLC layer stuff
    StackIndexOutOfRange,
    UnexpectedBufferHeader,
    NeedMoreData,

    // Returned by the ETH implementation on connect if it detects that any of
    // the triggers are enabled.
    InUse,

    // USB specific error code to indicate that the FTDI chip configuration is
    // not correct.
    USBChipConfigError,
};

LIBMVME_MVLC_EXPORT std::error_code make_error_code(MVLCErrorCode error);

/* The higher level error condition used to categorize the errors coming from
 * the MVLC logic code and the low level implementations. */
enum class ErrorType
{
    Success,
    ConnectionError,
    IOError,
    Timeout,
    ShortTransfer,
    ProtocolError,
    VMEError
};

LIBMVME_MVLC_EXPORT std::error_condition make_error_condition(ErrorType et);

} // end namespace mvlc
} // end namespace mesytec

namespace std
{
    template<> struct is_error_code_enum<mesytec::mvlc::MVLCErrorCode>: true_type {};

    template<> struct is_error_condition_enum<mesytec::mvlc::ErrorType>: true_type {};
} // end namespace std

#endif /* __MVLC_ERROR_H__ */
