#include "daqcontrol.h"
#include <QTimer>

DAQControl::DAQControl(MVMEContext *context, QObject *parent)
    : QObject(parent)
    , m_context(context)
{
    connect(m_context, &MVMEContext::daqStateChanged,
            this, &DAQControl::daqStateChanged);
}

DAQControl::~DAQControl()
{
}

DAQState DAQControl::getDAQState() const
{
    return m_context->getDAQState();
}

void DAQControl::startDAQ(
    u32 nCycles, bool keepHistoContents,
    const std::chrono::milliseconds &runDuration)
{
    if (m_context->getDAQState() != DAQState::Idle)
        return;

    if (m_context->getMode() == GlobalMode::DAQ)
    {
        if (runDuration != std::chrono::milliseconds::zero())
        {
            m_timedRunControl = std::make_unique<TimedRunControl>(
                this, runDuration);
        }

        m_context->startDAQReadout(nCycles, keepHistoContents);
    }
    else if (m_context->getMode() == GlobalMode::ListFile)
    {
        if (runDuration != std::chrono::milliseconds::zero())
            qWarning("DAQControl::startDAQ(): runDuration ignored for replays");

        m_context->startDAQReplay(nCycles, keepHistoContents);
    }
}

void DAQControl::stopDAQ()
{
    if (m_context->getDAQState() == DAQState::Idle)
        return;

    m_context->stopDAQ();
}

void DAQControl::pauseDAQ()
{
    if (m_context->getDAQState() != DAQState::Running)
        return;

    m_context->pauseDAQ();
}

void DAQControl::resumeDAQ(u32 nCycles)
{
    if (m_context->getDAQState() != DAQState::Paused)
        return;

    m_context->resumeDAQ(nCycles);
}

TimedRunControl::TimedRunControl(
    DAQControl *ctrl,
    const std::chrono::milliseconds &runDuration,
    QObject *parent)
: QObject(parent)
, m_ctrl(ctrl)
, m_shouldStop(false)
{
    assert(ctrl);
    assert(ctrl->getDAQState() == DAQState::Idle);

    if (ctrl->getDAQState() != DAQState::Idle)
        return;

    connect(ctrl, &DAQControl::daqStateChanged,
            this, &TimedRunControl::onDAQStateChanged);

    connect(&m_timer, &QTimer::timeout,
            this, &TimedRunControl::onTimerTimeout);

    m_timer.setInterval(runDuration);
    m_timer.setSingleShot(true);
}

void TimedRunControl::onDAQStateChanged(const DAQState &newState)
{
    switch (newState)
    {
        case DAQState::Running:
            assert(!m_timer.isActive());
            m_shouldStop = true;
            m_timer.start();
            qDebug() << __PRETTY_FUNCTION__ << "Running: started timer";
            break;

        case DAQState::Stopping:
            qDebug() << __PRETTY_FUNCTION__ << "Stopping: stopping timer";
            m_shouldStop = false;
            m_timer.stop();
            break;

        case DAQState::Idle:
        case DAQState::Starting:
        case DAQState::Paused:
            break;
    }
}

void TimedRunControl::onTimerTimeout()
{
    qDebug() << __PRETTY_FUNCTION__ << "shouldStop =" << m_shouldStop;
    if (m_shouldStop)
        m_ctrl->stopDAQ();
    m_shouldStop = false;
}
