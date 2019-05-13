#include "mvlc/mvlc_impl_udp.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>

#include <QDebug>

#ifndef __WIN32
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include <ws2tcpip.h>
#include <stdio.h>
#include <fcntl.h>
#endif

#include "mvlc/mvlc_error.h"

#define LOG_LEVEL_WARN  100
#define LOG_LEVEL_INFO  200
#define LOG_LEVEL_DEBUG 300
#define LOG_LEVEL_TRACE 400

#ifndef MVLC_UDP_LOG_LEVEL
#define MVLC_UDP_LOG_LEVEL LOG_LEVEL_WARN
#endif

#define LOG_LEVEL_SETTING MVLC_UDP_LOG_LEVEL

#define DO_LOG(level, prefix, fmt, ...)\
do\
{\
    if (LOG_LEVEL_SETTING >= level)\
    {\
        fprintf(stderr, prefix "%s(): " fmt "\n", __FUNCTION__, ##__VA_ARGS__);\
    }\
} while (0);

#define LOG_WARN(fmt, ...)  DO_LOG(LOG_LEVEL_WARN,  "WARN - mvlc_udp ", fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  DO_LOG(LOG_LEVEL_INFO,  "INFO - mvlc_udp ", fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) DO_LOG(LOG_LEVEL_DEBUG, "DEBUG - mvlc_udp ", fmt, ##__VA_ARGS__)
#define LOG_TRACE(fmt, ...) DO_LOG(LOG_LEVEL_TRACE, "TRACE - mvlc_udp ", fmt, ##__VA_ARGS__)

namespace
{


// Does IPv4 host lookup for a UDP socket. On success the resulting struct
// sockaddr_in is copied to dest.
std::error_code lookup(const std::string &host, u16 port, sockaddr_in &dest)
{
    using namespace mesytec::mvlc;

    dest = {};
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    struct addrinfo *result = nullptr, *rp = nullptr;

    int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(),
                         &hints, &result);

    // TODO: check getaddrinfo specific error codes. make and use getaddrinfo error category
    if (rc != 0)
    {
        qDebug("%s: HostLookupError, host=%s, error=%s", __PRETTY_FUNCTION__, host.c_str(), gai_strerror(rc));
        return make_error_code(MVLCErrorCode::HostLookupError);
    }

    for (rp = result; rp != NULL; rp = rp->ai_next)
    {
        if (rp->ai_addrlen == sizeof(dest))
        {
            std::memcpy(&dest, rp->ai_addr, rp->ai_addrlen);
            break;
        }
    }

    freeaddrinfo(result);

    if (!rp)
    {
        qDebug("%s: HostLookupError, host=%s, no result found", __PRETTY_FUNCTION__, host.c_str());
        return make_error_code(MVLCErrorCode::HostLookupError);
    }

    return {};
}

struct timeval ms_to_timeval(unsigned ms)
{
    unsigned seconds = ms / 1000;
    ms -= seconds * 1000;

    struct timeval tv;
    tv.tv_sec  = seconds;
    tv.tv_usec = ms * 1000;

    return tv;
}

#ifndef __WIN32
std::error_code set_socket_timeout(int optname, int sock, unsigned ms)
{
    struct timeval tv = ms_to_timeval(ms);

    int res = setsockopt(sock, SOL_SOCKET, optname, &tv, sizeof(tv));

    if (res != 0)
        return std::error_code(errno, std::system_category());

    return {};
}
#else
std::error_code set_socket_timeout(int optname, int sock, unsigned ms)
{
    DWORD optval = ms;
    int res = setsockopt(sock, SOL_SOCKET, optname,
                         reinterpret_cast<const char *>(optval),
                         sizeof(optval));

    if (res != 0)
        return std::error_code(errno, std::system_category());

    return {};
}
#endif

std::error_code set_socket_write_timeout(int sock, unsigned ms)
{
    return set_socket_timeout(SO_SNDTIMEO, sock, ms);
}

std::error_code set_socket_read_timeout(int sock, unsigned ms)
{
    return set_socket_timeout(SO_RCVTIMEO, sock, ms);
}

const u16 FirstDynamicPort = 49152;
static const int SocketReceiveBufferSize = 1024 * 1024 * 10;

} // end anon namespace

namespace mesytec
{
namespace mvlc
{
namespace udp
{

Impl::Impl(const std::string &host)
    : m_host(host)
{
#ifdef __WIN32
    WORD wVersionRequested;
    WSADATA wsaData;
    wVersionRequested = MAKEWORD(2, 1);
    WSAStartup( wVersionRequested, &wsaData );
#endif
}

Impl::~Impl()
{
    disconnect();

#ifdef __WIN32
    WSACleanup();
#endif
}

// A note about using ::bind() and then ::connect():
//
// Under linux this has the effect of changing the local bound address from
// INADDR_ANY to the address of the interface that's used to reach the remote
// address. E.g. when connecting to localhost the following will happen: after
// the bind() call the local "listen" address will be 0.0.0.0, after the
// connect() call this will change to 127.0.0.1. The local port specified in
// the bind() call will be kept. This is nice.

// Things happening in Impl::connect:
// * Remote host lookup to get the IPv4 address of the MVLC.
// * Create two UDP sockets and bind them to two consecutive local ports.
//   Ports are tried starting from FirstDynamicPort (49152).
// * Use ::connect() on both sockets with the MVLC address and the default
//   command and data ports. This way the sockets will only receive datagrams
//   originating from the MVLC.
// * TODO: Send an initial request and read the response. Preferably this
//   should tell us if another client is currently using the MVLC. It could be
//   some sort of "DAQ mode register" or a way to check where the MVLC is
//   currently sending its data output.
std::error_code Impl::connect()
{
    if (isConnected())
        return make_error_code(MVLCErrorCode::IsConnected);

    m_cmdSock = -1;
    m_dataSock = -1;
    m_pipeStats = {};
    m_packetChannelStats = {};
    std::fill(m_lastPacketNumbers.begin(), m_lastPacketNumbers.end(), -1);

    if (auto ec = lookup(m_host, CommandPort, m_cmdAddr))
        return ec;

    assert(m_cmdAddr.sin_port == htons(CommandPort));

    // Copy address and replace the port with DataPort
    m_dataAddr = m_cmdAddr;
    m_dataAddr.sin_port = htons(DataPort);

    // Lookup succeeded and we have now have two remote addresses, one for the
    // command and one for the data pipe.
    //
    // Now create two IPv4 UDP sockets and try to bind them to two consecutive
    // local ports.
    for (u16 localCmdPort = FirstDynamicPort;
         // Using 'less than' to leave one spare port for the data pipe
         localCmdPort < std::numeric_limits<u16>::max();
         localCmdPort++)
    {
        assert(m_cmdSock < 0 && m_dataSock < 0);

        // Not being able to create the sockets is considered a fatal error.

        m_cmdSock = socket(AF_INET, SOCK_DGRAM, 0);
        if (m_cmdSock < 0)
            return std::error_code(errno, std::system_category());

        m_dataSock = socket(AF_INET, SOCK_DGRAM, 0);
        if (m_dataSock < 0)
        {
            if (m_cmdSock >= 0)
            {
                ::close(m_cmdSock);
                m_cmdSock = -1;
            }
            return std::error_code(errno, std::system_category());
        }

        assert(m_cmdSock >= 0 && m_dataSock >= 0);

        // Setup the local address structures using two consecutive port
        // numbers.
        struct sockaddr_in cmdLocal = {};
        cmdLocal.sin_family = AF_INET;
        cmdLocal.sin_addr.s_addr = INADDR_ANY;
        cmdLocal.sin_port = htons(localCmdPort);

        struct sockaddr_in dataLocal = cmdLocal;
        dataLocal.sin_port = htons(localCmdPort + 1);

        // Bind both sockets. In case of an error close the sockets and
        // continue with the loop.
        if (int res = ::bind(m_cmdSock, reinterpret_cast<struct sockaddr *>(&cmdLocal),
                             sizeof(cmdLocal)))
        {
            goto try_again;
        }

        if (int res = ::bind(m_dataSock, reinterpret_cast<struct sockaddr *>(&dataLocal),
                             sizeof(dataLocal)))
        {
            goto try_again;
        }

#if 0//#ifdef __WIN32
        {
            // Make the sockets non-blocking. Errors are considered fatal ->
            // return immediately.

            u_long iMode = 1;
            int res = ioctlsocket(m_cmdSock, FIONBIO, &iMode);
            if (res != 0)
                return make_error_code(MVLCErrorCode::SocketError);

            res = ioctlsocket(m_dataSock, FIONBIO, &iMode);
            if (res != 0)
                return make_error_code(MVLCErrorCode::SocketError);
        }
#endif

        break;

        try_again:
        {
            ::close(m_cmdSock);
            ::close(m_dataSock);
            m_cmdSock = -1;
            m_dataSock = -1;
        }
    }

    if (m_cmdSock < 0 || m_dataSock < 0)
        return make_error_code(MVLCErrorCode::BindLocalError);

    // Call connect on the sockets so that we receive only datagrams
    // originating from the MVLC.
    if (int res = ::connect(m_cmdSock, reinterpret_cast<struct sockaddr *>(&m_cmdAddr),
                            sizeof(m_cmdAddr)))
    {
        ::close(m_cmdSock);
        ::close(m_dataSock);
        m_cmdSock = -1;
        m_dataSock = -1;
        return std::error_code(errno, std::system_category());
    }

    if (int res = ::connect(m_dataSock, reinterpret_cast<struct sockaddr *>(&m_dataAddr),
                            sizeof(m_dataAddr)))
    {
        ::close(m_cmdSock);
        ::close(m_dataSock);
        m_cmdSock = -1;
        m_dataSock = -1;
        return std::error_code(errno, std::system_category());
    }

    // Set read and write timeouts
    for (auto pipe: { Pipe::Command, Pipe::Data })
    {
        if (auto ec = set_socket_write_timeout(getSocket(pipe), getWriteTimeout(pipe)))
            return ec;

        if (auto ec = set_socket_read_timeout(getSocket(pipe), getReadTimeout(pipe)))
            return ec;
    }

    // Set socket receive buffer size
    for (auto pipe: { Pipe::Command, Pipe::Data })
    {
#ifndef __WIN32
        int res = setsockopt(getSocket(pipe), SOL_SOCKET, SO_RCVBUF,
                             &SocketReceiveBufferSize,
                             sizeof(SocketReceiveBufferSize));
#else
        int res = setsockopt(getSocket(pipe), SOL_SOCKET, SO_RCVBUF,
                             reinterpret_cast<const char *>(&SocketReceiveBufferSize),
                             sizeof(SocketReceiveBufferSize));
#endif
        assert(res == 0);

        if (res != 0)
            return std::error_code(errno, std::system_category());
    }

    // TODO: send some initial request to verify there's an MVLC on the other side
    // Note: this should not interfere with any other active client.

    assert(m_cmdSock >= 0 && m_dataSock >= 0);
    return {};
}

std::error_code Impl::disconnect()
{
    if (!isConnected())
        return make_error_code(MVLCErrorCode::IsDisconnected);

    ::close(m_cmdSock);
    ::close(m_dataSock);
    m_cmdSock = -1;
    m_dataSock = -1;
    return {};
}

bool Impl::isConnected() const
{
    return m_cmdSock >= 0 && m_dataSock >= 0;
}

std::error_code Impl::setWriteTimeout(Pipe pipe, unsigned ms)
{
    auto p = static_cast<unsigned>(pipe);

    if (p >= PipeCount)
        return make_error_code(MVLCErrorCode::InvalidPipe);

    m_writeTimeouts[p] = ms;

    if (isConnected())
        return set_socket_write_timeout(getSocket(pipe), ms);

    return {};
}

std::error_code Impl::setReadTimeout(Pipe pipe, unsigned ms)
{
    auto p = static_cast<unsigned>(pipe);

    if (p >= PipeCount)
        return make_error_code(MVLCErrorCode::InvalidPipe);

    m_readTimeouts[p] = ms;

    if (isConnected())
        return set_socket_read_timeout(getSocket(pipe), ms);

    return {};
}

unsigned Impl::getWriteTimeout(Pipe pipe) const
{
    if (static_cast<unsigned>(pipe) >= PipeCount) return 0u;
    return m_writeTimeouts[static_cast<unsigned>(pipe)];
}

unsigned Impl::getReadTimeout(Pipe pipe) const
{
    if (static_cast<unsigned>(pipe) >= PipeCount) return 0u;
    return m_readTimeouts[static_cast<unsigned>(pipe)];
}

// Standard MTU is 1500 bytes
// Jumbos Frames are usually 9000 bytes
// IPv4 header is 20 bytes
// UDP header is 8 bytes
static const size_t MaxOutgoingPayloadSize = 1500 - 20 - 8;
//static const size_t MaxIncomingPayloadSIze = MaxOutgoingPayloadSize;

std::error_code Impl::write(Pipe pipe, const u8 *buffer, size_t size,
                            size_t &bytesTransferred)
{
    // Note: it is not necessary to split this into multiple calls to send()
    // because outgoing MVLC command buffers have to be smaller than the
    // maximum, non-jumbo ethernet MTU.
    // The send() call should return EMSGSIZE if the payload is too large to be
    // atomically transmitted.
    assert(size <= MaxOutgoingPayloadSize);
    assert(static_cast<unsigned>(pipe) < PipeCount);

    if (static_cast<unsigned>(pipe) >= PipeCount)
        return make_error_code(MVLCErrorCode::InvalidPipe);

    bytesTransferred = 0;

    if (!isConnected())
        return make_error_code(MVLCErrorCode::IsDisconnected);

    ssize_t res = ::send(getSocket(pipe), reinterpret_cast<const char *>(buffer), size, 0);

    if (res < 0)
        return std::error_code(errno, std::system_category());

    bytesTransferred = res;
    return {};
}

#ifdef __WIN32
// FIXME: use WSAGetLastError here once the std;:error_code infrastructure exists
static inline std::error_code receive_one_packet(int sockfd, u8 *dest, size_t size,
                                                 size_t &bytesTransferred, int timeout_ms)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sockfd, &fds);

    struct timeval tv = ms_to_timeval(timeout_ms);

    int sres = ::select(0, &fds, nullptr, nullptr, &tv);

    if (sres == 0)
        return make_error_code(MVLCErrorCode::SocketTimeout);

    if (sres == SOCKET_ERROR)
        return make_error_code(MVLCErrorCode::SocketError);

    ssize_t res = ::recv(sockfd, reinterpret_cast<char *>(dest), size, 0);

    if (res < 0)
        return make_error_code(MVLCErrorCode::SocketError);

    bytesTransferred = res;
    return {};
}
#else
static inline std::error_code receive_one_packet(int sockfd, u8 *dest, size_t size,
                                                 size_t &bytesTransferred, int)
{
    bytesTransferred = 0u;

    ssize_t res = ::recv(sockfd, reinterpret_cast<char *>(dest), size, 0);

    if (res < 0)
        return std::error_code(errno, std::system_category());

    bytesTransferred = res;
    return {};
}
#endif

/* initial:
 *   next_header_pointer = 0
 *   packet_number = 0
 *
 *   - receive one packet
 *   - make sure there are two header words
 *   - extract packet_number and number_of_data_words
 *   - record possible packet loss or ordering problems based on packet number
 *   - check to make sure timestamp is incrementing (packet ordering) (not implemented yet)
 *   -
 */

std::error_code Impl::read(Pipe pipe_, u8 *buffer, size_t size,
                           size_t &bytesTransferred)
{
    unsigned pipe = static_cast<unsigned>(pipe_);

    assert(buffer);
    assert(pipe < PipeCount);

    const size_t requestedSize = size;
    bytesTransferred = 0u;

    if (pipe >= PipeCount)
        return make_error_code(MVLCErrorCode::InvalidPipe);

    if (!isConnected())
        return make_error_code(MVLCErrorCode::IsDisconnected);

    auto &receiveBuffer = m_receiveBuffers[pipe];

    // Copy from receiveBuffer into the dest buffer while updating local
    // variables.
    auto copy_and_update = [&buffer, &size, &bytesTransferred, &receiveBuffer] ()
    {
        if (size_t toCopy = std::min(receiveBuffer.available(), size))
        {
            memcpy(buffer, receiveBuffer.start, toCopy);
            buffer += toCopy;
            size -= toCopy;
            receiveBuffer.start += toCopy;
            bytesTransferred += toCopy;
        }
    };

    LOG_TRACE("+ pipe=%u, size=%zu, bufferAvail=%zu",
              pipe, requestedSize, receiveBuffer.available());

    copy_and_update();

    if (size == 0)
    {
        LOG_TRACE("  pipe=%u, size=%zu, read request satisfied from buffer, new buffer size=%zu",
                  pipe, requestedSize, receiveBuffer.available());
        return {};
    }

    // All data from the read buffer should have been consumed at this point.
    // It's time to issue actual read requests.
    assert(receiveBuffer.available() == 0);

    size_t readCount = 0u;
    auto &pipeStats = m_pipeStats[pipe];
    const auto tStart = std::chrono::high_resolution_clock::now();

    while (size > 0)
    {
        assert(receiveBuffer.available() == 0);
        receiveBuffer.reset();

        LOG_TRACE("  pipe=%u, requestedSize=%zu, remainingSize=%zu, reading from MVLC...",
                  pipe, requestedSize, size);

        size_t transferred = 0;

        auto ec = receive_one_packet(
            getSocket(pipe_),
            receiveBuffer.buffer.data(),
            receiveBuffer.buffer.size(),
            transferred,
            getReadTimeout(pipe_));

        ++readCount;

        LOG_TRACE("  pipe=%u, received %zu bytes, ec=%s",
                  pipe, transferred, ec.message().c_str());

        if (ec)
            return ec;

        const u16 packetSize = transferred;

        ++pipeStats.receivedPackets;
        pipeStats.receivedBytes += packetSize;
        ++pipeStats.packetSizes[packetSize];

        if (packetSize < HeaderBytes)
        {
            ++pipeStats.shortPackets;
            LOG_WARN("  pipe=%u, received data is smaller than the MVLC UDP header size", pipe);

            return make_error_code(MVLCErrorCode::ShortRead);
        }

        receiveBuffer.start = receiveBuffer.buffer.data() + HeaderBytes;
        receiveBuffer.end   = receiveBuffer.buffer.data() + packetSize;

        u32 pkt_header0 = receiveBuffer.header0();
        u32 pkt_header1 = receiveBuffer.header1();

        u16 packetChannel       = (pkt_header0 >> header0::PacketChannelShift)  & header0::PacketChannelMask;
        u16 packetNumber        = (pkt_header0 >> header0::PacketNumberShift)  & header0::PacketNumberMask;
        u16 dataWordCount       = (pkt_header0 >> header0::NumDataWordsShift)  & header0::NumDataWordsMask;

        u32 udpTimestamp        = (pkt_header1 >> header1::TimestampShift)     & header1::TimestampMask;
        u16 nextHeaderPointer   = (pkt_header1 >> header1::HeaderPointerShift) & header1::HeaderPointerMask;

        LOG_TRACE("  pipe=%u, header0=0x%08x -> packetChannel=%u, packetNumber=%u, wordCount=%u",
                  pipe, pkt_header0, packetChannel, packetNumber, dataWordCount);

        LOG_TRACE("  pipe=%u, header1=0x%08x -> udpTimestamp=%u, nextHeaderPointer=%u",
                  pipe, pkt_header1, udpTimestamp, nextHeaderPointer);

        const u16 availableDataWords = receiveBuffer.available() / sizeof(u32);
        const u16 leftoverBytes = receiveBuffer.available() % sizeof(u32);

        LOG_TRACE("  pipe=%u, calculated available data words = %u, leftover bytes = %u",
                  pipe, availableDataWords, leftoverBytes);

        if (leftoverBytes > 0)
        {
            LOG_WARN("  pipe=%u, %u leftover bytes in received packet",
                     pipe, leftoverBytes);
        }

        if (packetChannel >= NumPacketChannels)
        {
            LOG_WARN("  pipe=%u, packet channel number out of range: %u", pipe, packetChannel);
            ++pipeStats.packetChannelOutOfRange;
            return make_error_code(MVLCErrorCode::UDPPacketChannelOutOfRange);
        }

        auto &channelStats = m_packetChannelStats[packetChannel];

        ++channelStats.receivedPackets;
        channelStats.receivedBytes += packetSize;

        {
            auto &lastPacketNumber = m_lastPacketNumbers[packetChannel];

            LOG_TRACE("  pipe=%u, packetChannel=%u, packetNumber=%u, lastPacketNumber=%d",
                      pipe, packetChannel, packetNumber, lastPacketNumber);

            // Packet loss calculation. The initial lastPacketNumber value is -1.
            if (lastPacketNumber >= 0)
            {
                auto loss = calc_packet_loss(lastPacketNumber, packetNumber);

                if (loss > 0)
                {
                    LOG_WARN("  pipe=%u, lastPacketNumber=%u, packetNumber=%u, loss=%d",
                             pipe, lastPacketNumber, packetNumber, loss);
                }

                pipeStats.lostPackets += loss;
                channelStats.lostPackets += loss;
            }

            lastPacketNumber = packetNumber;
            ++channelStats.packetSizes[packetSize];
        }

        // Check where nextHeaderPointer is pointing to
        if (nextHeaderPointer != 0xffff)
        {
            u32 *start = reinterpret_cast<u32 *>(receiveBuffer.start);
            u32 *end   = reinterpret_cast<u32 *>(receiveBuffer.end);
            u32 *headerp = start + nextHeaderPointer;

            if (headerp >= end)
            {
                ++pipeStats.headerOutOfRange;
                ++channelStats.headerOutOfRange;

                LOG_WARN("  pipe=%u, nextHeaderPointer out of range: nHPtr=%u, "
                         "availDataWords=%u, pktChan=%u, pktNum=%d, pktSize=%u bytes",
                         pipe, nextHeaderPointer, availableDataWords, packetChannel,
                         packetNumber, packetSize);
            }
            else
            {
                u32 header = *headerp;
                LOG_TRACE("  pipe=%u, nextHeaderPointer=%u -> header=0x%08x",
                          pipe, nextHeaderPointer, header);
                u32 type = (header >> 24) & 0xff;
                ++pipeStats.headerTypes[type];
                ++channelStats.headerTypes[type];
            }
        }
        else
        {
            ++pipeStats.noHeader;
            ++channelStats.noHeader;
        }

        // Copy to destination buffer
        copy_and_update();

        auto tEnd = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart);

        if (elapsed.count() >= getReadTimeout(pipe_))
        {
            LOG_TRACE("  pipe=%u, read of size=%zu completes with %zu bytes and timeout"
                      " after %zu reads, remaining bytes in buffer=%zu",
                      pipe, requestedSize, bytesTransferred, readCount,
                      receiveBuffer.available());

            return make_error_code(MVLCErrorCode::SocketTimeout);
        }
    }

    LOG_TRACE("  pipe=%u, read of size=%zu completed using %zu reads, remaining bytes in buffer=%zu",
              pipe, requestedSize, readCount, receiveBuffer.available());

    return {};
}

std::error_code Impl::getReadQueueSize(Pipe pipe_, u32 &dest)
{
    auto pipe = static_cast<unsigned>(pipe_);
    assert(pipe < PipeCount);

    if (pipe < PipeCount)
        dest = m_receiveBuffers[static_cast<unsigned>(pipe)].available();

    return make_error_code(MVLCErrorCode::InvalidPipe);
}

std::array<PipeStats, PipeCount> Impl::getPipeStats() const
{
    return m_pipeStats;
}

std::array<PacketChannelStats, NumPacketChannels> Impl::getPacketChannelStats() const
{
    return m_packetChannelStats;
}

s32 calc_packet_loss(u16 lastPacketNumber, u16 packetNumber)
{
    static const s32 PacketNumberMax = udp::header0::PacketNumberMask;

    s32 diff = packetNumber - lastPacketNumber;

    if (diff < 1)
    {
        diff = PacketNumberMax + diff;
        return diff;
    }

    return diff - 1;
}

} // end namespace udp
} // end namespace mvlc
} // end namespace mesytec
