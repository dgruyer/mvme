#include "data_filter.h"

#include <cctype>
#include <stdexcept>

namespace analysis
{

// Source: http://stackoverflow.com/a/757266
static inline int trailing_zeroes(uint32_t v)
{
    int r;           // result goes here
    static const int MultiplyDeBruijnBitPosition[32] = 
    {
        0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8, 
        31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
    };
    r = MultiplyDeBruijnBitPosition[((uint32_t)((v & -v) * 0x077CB531U)) >> 27];
    return r;
}

// Source: http://stackoverflow.com/a/109025 (SWAR)
static inline u32 number_of_set_bits(u32 i)
{
     // Java: use >>> instead of >>
     // C or C++: use uint32_t
     i = i - ((i >> 1) & 0x55555555);
     i = (i & 0x33333333) + ((i >> 2) & 0x33333333);
     return (((i + (i >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
}

//
// DataFilter
//
DataFilter::DataFilter(const QByteArray &filter, s32 wordIndex)
    : m_filter(filter)
    , m_matchWordIndex(wordIndex)
{
    if (filter.size() > 32)
        throw std::length_error("maximum filter size of 32 exceeded");

    compile();
}

void DataFilter::compile()
{
    m_extractCache.clear();
    m_matchMask  = 0;
    m_matchValue = 0;

    for (int i=0; i<m_filter.size(); ++i)
    {
        char c = m_filter[m_filter.size() - i - 1];

        if (c == '0' || c == '1' || c == 0 || c == 1)
            m_matchMask |= 1 << i;

        if (c == '1' || c == 1)
            m_matchValue |= 1 << i;
    }
}

u32 DataFilter::getExtractMask(char marker) const
{
    u32 result = 0;

    marker = std::tolower(marker);

    if (m_extractCache.contains(marker))
    {
        result = m_extractCache.value(marker);
    }
    else
    {
        for (int i=0; i<m_filter.size(); ++i)
        {
            char c = std::tolower(m_filter[m_filter.size() - i - 1]);

            if (c == marker)
                result |= 1 << i;
        }
        m_extractCache[marker] = result;
    }

    return result;
}

u32 DataFilter::getExtractShift(char marker) const
{
    return trailing_zeroes(getExtractMask(marker));
}

u32 DataFilter::getExtractBits(char marker) const
{
    return number_of_set_bits(getExtractMask(marker));
}

u32 DataFilter::extractData(u32 value, char marker) const
{
    u32 result = (value & getExtractMask(marker)) >> getExtractShift(marker);

    return result;
}

bool DataFilter::operator==(const DataFilter &other) const
{
    return (m_filter == other.m_filter)
        && (m_matchWordIndex == other.m_matchWordIndex);
}

QString DataFilter::toString() const
{
    return QString("DataFilter(f=%1,i=%2)")
        .arg(QString::fromLocal8Bit(getFilter()))
        .arg(m_matchWordIndex);
}

//
// MultiWordDataFilter
//
MultiWordDataFilter::MultiWordDataFilter(const QVector<DataFilter> &filters)
    : m_filters(filters)
    , m_results(filters.size())
{
}

void MultiWordDataFilter::addSubFilter(const DataFilter &filter)
{
    m_filters.push_back(filter);
    m_results.resize(m_filters.size());
    clearCompletion();
}

void MultiWordDataFilter::setSubFilters(const QVector<DataFilter> &subFilters)
{
    m_filters = subFilters;
    m_results.resize(m_filters.size());
    clearCompletion();
}

QString MultiWordDataFilter::toString() const
{
    return QString("MultiWordDataFilter(filterCount=%1)")
        .arg(m_filters.size());
}

DataFilter makeFilterFromString(const QString &str, s32 wordIndex)
{
    auto filterDataRaw = str.toLocal8Bit();

    QByteArray filterData;

    for (auto c: filterDataRaw)
    {
        if (c != ' ')
            filterData.push_back(c);
    }

    return DataFilter(filterData, wordIndex);
}

}
