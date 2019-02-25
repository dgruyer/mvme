#include "mvlc/mvlc_qt_object.h"
#include "mvlc/mvlc_error.h"

namespace mesytec
{
namespace mvlc
{

//
// MVLCObject
//
MVLCObject::MVLCObject(std::unique_ptr<AbstractImpl> impl, QObject *parent)
    : QObject(parent)
    , m_impl(std::move(impl))
    , m_state(Disconnected)
{
    if (m_impl->is_open())
        setState(Connected);
}

MVLCObject::~MVLCObject()
{
    disconnect();
}

std::error_code MVLCObject::connect()
{
    if (isConnected()) return make_error_code(MVLCProtocolError::IsOpen);

    std::error_code result = {};

    std::unique_lock<std::mutex> cmdLock(m_cmdMutex, std::defer_lock);
    std::unique_lock<std::mutex> dataLock(m_dataMutex, std::defer_lock);
    std::lock(cmdLock, dataLock);
    setState(Connecting);
    result = m_impl->open();

    if (result)
    {
        emit errorSignal(result);
        setState(Disconnected);
    }
    else
    {
        setState(Connected);
    }

    return result;
}

std::error_code MVLCObject::disconnect()
{
    if (!isConnected()) return make_error_code(MVLCProtocolError::IsClosed);
    // XXX: leftoff here

    std::error_code result = {};

    std::unique_lock<std::mutex> cmdLock(m_cmdMutex, std::defer_lock);
    std::unique_lock<std::mutex> dataLock(m_dataMutex, std::defer_lock);
    std::lock(cmdLock, dataLock);
    result = m_impl->close();
    setState(Disconnected);
    return result;
}

void MVLCObject::setState(const State &newState)
{
    if (m_state != newState)
    {
        auto prevState = m_state;
        m_state = newState;
        emit stateChanged(prevState, newState);
    }
};

} // end namespace mvlc
} // end namespace mesytec
