#include "histogram.h"
#include "math.h"
#include <cassert>

Histogram::Histogram(QObject *parent, quint32 channels, quint32 resolution) :
    QObject(parent)
{
    assert(channels <= MAX_CHANNEL_COUNT);
    m_data = new double[channels*resolution];
    m_axisBase = new double[resolution];
    m_channels = channels;
    m_resolution = resolution;
    initHistogram();
    qDebug("Initialized histogram with %d channels, %d resolution", channels, resolution);
}

Histogram::~Histogram()
{
    delete[] m_data;
    delete[] m_axisBase;
}

void Histogram::initHistogram(void)
{
    qDebug("initializing %d x %d", m_channels, m_resolution);
    for(quint32 i = 0; i < m_channels; i++){
        for(quint32 j = 0; j< m_resolution; j++){
            m_data[i*m_resolution+j] = 0;
            m_axisBase[j] = j;
        }
        m_mean[i] = 0;
        m_sigma[i] = 0;
        m_counts[i] = 0;
        m_maxchan[i] = 0;
        m_maximum[i] = 0;
    }
}

void Histogram::clearChan(quint32 chan)
{
    if (chan >= m_channels)
        return;

    for(quint32 i = 0; i < m_resolution; i++)
        m_data[chan * m_resolution + i] = 0;
    m_mean[chan] = 0;
    m_sigma[chan] = 0;
    m_counts[chan] = 0;
    m_maxchan[chan] = 0;
    m_maximum[chan] = 0;
}

void Histogram::clearHistogram()
{
    for(quint32 i = 0; i < m_channels; i++)
        clearChan(i);
}

// calculate counts, maximum, mean and sigma for given channel range
void Histogram::calcStatistics(quint32 chan, quint32 start, quint32 stop)
{
    if (chan >= m_channels)
        return;

    quint32 swap = 0;
    if(start > stop){
        stop = swap;
        stop = start;
        start = swap;
    }

    start = qMin(start, m_resolution);
    stop  = qMin(stop, m_resolution);

    double dval = 0;
    m_mean[chan] = 0;
    m_counts[chan] = 0;
    m_sigma[chan] = 0;
    m_maximum[chan] = 0;
    m_maxchan[chan] = 0;

    // calc mean and total counts
    for(quint32 i=start; i<stop; i++){
        m_mean[chan] += m_data[chan*m_resolution + i]*i;
        m_counts[chan] += m_data[chan*m_resolution + i];
        if(m_data[chan*m_resolution + i] > m_maximum[chan]){
            m_maxchan[chan] = i;
            m_maximum[chan] = m_data[chan*m_resolution + i];
        }
    }
//    qDebug("counts: %d",(quint32)m_counts[chan]);
    if(m_counts[chan])
        m_mean[chan] /= m_counts[chan];
    else
        m_mean[chan] = 0;

    if(m_mean[chan]){
        // calc sigma
        for(quint32 i=start; i<stop; i++){
            swap = m_data[chan*m_resolution + i];
            if(swap){
                dval = i - m_mean[chan];
                dval *= dval;
                m_sigma[chan] += dval * swap;
            }
        }
    }
    m_sigma[chan] = sqrt(m_sigma[chan]/m_counts[chan]);
}


double Histogram::getValue(quint32 channelIndex, quint32 valueIndex)
{

    if(valueIndex < m_resolution && channelIndex < m_channels)
    {
        return m_data[channelIndex * m_resolution + valueIndex];
    }

    return 0;
}


bool Histogram::incValue(quint32 channelIndex, quint32 valueIndex)
{
    if (channelIndex < m_channels && valueIndex < m_resolution)
    {
        m_data[channelIndex * m_resolution + valueIndex]++;
        return true;
    }

    return false;
}

void Histogram::setValue(quint32 channelIndex, quint32 valueIndex, double value)
{
    if (channelIndex < m_channels && valueIndex < m_resolution)
    {
        m_data[channelIndex * m_resolution + valueIndex] = value;
    }
}

void Histogram::setAxisBaseValue(quint32 valueIndex, double axisBaseValue)
{
    if (valueIndex < m_resolution)
    {
        m_axisBase[valueIndex] = axisBaseValue;
    }
}

QTextStream &writeHistogramCollection(QTextStream &out, Histogram *histo)
{
    out << "channels: " << histo->m_channels
        << " resolution: " << histo->m_resolution
        << endl;

    for (quint32 valueIndex=0; valueIndex<histo->m_resolution; ++valueIndex)
    {
        out << histo->m_axisBase[valueIndex] << " ";
        for (quint32 channelIndex=0; channelIndex < histo->m_channels; ++channelIndex)
        {
            out << histo->m_data[channelIndex * histo->m_resolution + valueIndex] << " ";
        }
        out << endl;
    }

    return out;
}

QTextStream &readHistogramCollectionInto(QTextStream &in, Histogram *histo)
{
    quint32 channels = 0;
    quint32 resolution = 0;
    QString buffer;

    in >> buffer >> channels >> buffer >> resolution;

    if (in.status() == QTextStream::Ok && channels && resolution)
    {
        histo->clearHistogram();
        for (quint32 valueIndex=0; valueIndex<resolution; ++valueIndex)
        {
            double axisBaseValue;
            in >> axisBaseValue;
            histo->setAxisBaseValue(valueIndex, axisBaseValue);

            for (quint32 channelIndex=0; channelIndex < channels; ++channelIndex)
            {
                double value;
                in >> value;
                histo->setValue(channelIndex, valueIndex, value);
            }
        }
    }

    return in;
}

QTextStream &writeHistogram(QTextStream &out, Histogram *histo, quint32 channelIndex)
{
    if (channelIndex < histo->m_channels)
    {
        out << "channel: " << channelIndex << endl;
        for (quint32 valueIndex=0; valueIndex<histo->m_resolution; ++valueIndex)
        {
            out << histo->m_axisBase[valueIndex] << " "
                << histo->getValue(channelIndex, valueIndex)
                << endl;
        }
    }

    return out;
}

QTextStream &readHistogram(QTextStream &in, Histogram *histo, quint32 *channelIndexOut)
{
    quint32 channelIndex;
    QString buffer;

    in >> buffer >> channelIndex;

    while (in.status() == QTextStream::Ok)
    {
        if (channelIndexOut)
        {
            *channelIndexOut = channelIndex;
        }

        quint32 valueIndex = 0;
        double value = 0;

        in >> valueIndex >> value;

        if (in.status() == QTextStream::Ok)
        {
            histo->setValue(channelIndex, valueIndex, value);
        }
    }

    return in;
}
