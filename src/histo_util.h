#ifndef __HISTO_UTIL_H__
#define __HISTO_UTIL_H__

#include "typedefs.h"

#include <cmath>
#include <qwt_scale_draw.h>
#include <qwt_scale_engine.h>
#include <qwt_scale_map.h>

class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QGroupBox;

// Adapted from: http://stackoverflow.com/a/18593942

// This uses QwtScaleMap to perform the coordinate transformations:
// scaleInterval is the raw histogram resolution
// paintInterval is the unit interval

class UnitConversionAxisScaleDraw: public QwtScaleDraw
{
    public:
        UnitConversionAxisScaleDraw(const QwtScaleMap &conversionMap)
            : m_conversionMap(conversionMap)
        {}

        virtual QwtText label(double value) const override
        {
            double labelValue = m_conversionMap.transform(value);
            auto text = QString::number(labelValue);
            return QwtText(text);
        };

    private:
        QwtScaleMap m_conversionMap;
};

class UnitConversionLinearScaleEngine: public QwtLinearScaleEngine
{
    public:
        UnitConversionLinearScaleEngine(const QwtScaleMap &conversionMap, u32 base=10)
            : QwtLinearScaleEngine(base)
            , m_conversionMap(conversionMap)
        {
        }


        virtual void autoScale(int maxNumSteps, double &x1, double &x2, double &stepSize) const override
        {
            x1 = m_conversionMap.transform(x1);
            x2 = m_conversionMap.transform(x2);
            stepSize = m_conversionMap.transform(stepSize);

            QwtLinearScaleEngine::autoScale(maxNumSteps, x1, x2, stepSize);

            x1 = m_conversionMap.invTransform(x1);
            x2 = m_conversionMap.invTransform(x1);
            stepSize = m_conversionMap.invTransform(stepSize);
        }

        virtual QwtScaleDiv divideScale(double x1, double x2, int maxMajorSteps, int maxMinorSteps, double stepSize=0.0) const override
        {
            x1 = m_conversionMap.transform(x1);
            x2 = m_conversionMap.transform(x2);
            //qDebug() << "stepSize pre" << stepSize;
            //stepSize = m_conversionMap.transform(stepSize);
            //qDebug() << "stepSize post" << stepSize;

            auto scaleDiv = QwtLinearScaleEngine::divideScale(x1, x2, maxMajorSteps, maxMinorSteps, stepSize);

            x1 = m_conversionMap.invTransform(x1);
            x2 = m_conversionMap.invTransform(x2);

            QwtScaleDiv result(x1, x2);

            for (int tickType = 0; tickType < QwtScaleDiv::NTickTypes; ++tickType)
            {
                auto ticks = scaleDiv.ticks(tickType);

                for (int i = 0; i < ticks.size(); ++i)
                    ticks[i] = m_conversionMap.invTransform(ticks[i]);

                result.setTicks(tickType, ticks);
            }

            return result;
        }

    private:
        QwtScaleMap m_conversionMap;
};

// Bounds values to 0.1 to make QwtLogScaleEngine happy
class MinBoundLogTransform: public QwtLogTransform
{
    public:
        virtual double bounded(double value) const
        {
            double result = qBound(0.1, value, QwtLogTransform::LogMax);
            return result;
        }

        virtual double transform(double value) const
        {
            double result = QwtLogTransform::transform(bounded(value));
            return result;
        }

        virtual double invTransform(double value) const
        {
            double result = QwtLogTransform::invTransform(value);
            return result;
        }

        virtual QwtTransform *copy() const
        {
            return new MinBoundLogTransform;
        }
};

QString makeAxisTitle(const QString &title, const QString &unit);

class AxisBinning
{
    public:
        static const s64 Underflow = -1;
        static const s64 Overflow = -2;


        AxisBinning()
            : m_nBins(0)
            , m_min(0.0)
            , m_max(0.0)
        {}

        AxisBinning(u32 nBins, double Min, double Max)
            : m_nBins(nBins)
            , m_min(Min)
            , m_max(Max)
        {}

        inline double getMin() const { return m_min; }
        inline double getMax() const { return m_max; }
        inline double getWidth() const { return std::abs(getMax() - getMin()); }

        inline void setMin(double min) { m_min = min; }
        inline void setMax(double max) { m_max = max; }

        inline u32 getBins() const { return m_nBins; }
        inline void setBins(u32 bins) { m_nBins = bins; }
        inline double getBinWidth() const { return getWidth() / getBins(); }
        inline double getBinLowEdge(u32 bin) const { return getMin() + bin * getBinWidth(); }
        inline double getBinCenter(u32 bin) const { return getBinLowEdge(bin) + getBinWidth() * 0.5; }

        /* Returns the bin number for the value x. Returns Underflow/Overflow
         * if x is out of range. */
        inline s64 getBin(double x) const
        {
            double bin = getBinUnchecked(x);

            if (bin < 0.0)
                return Underflow;

            if (bin >= getBins())
                return Overflow;

            return static_cast<s64>(bin);
        }

        /* Returns the bin number for the value x. No check is performed if x
         * is in range of the axis. */
        inline double getBinUnchecked(double x) const
        {
            double bin = m_nBins * (x - m_min) / (m_max - m_min);
            return bin;
        }

        inline bool operator==(const AxisBinning &other)
        {
            return (m_nBins == other.m_nBins
                    && m_min == other.m_min
                    && m_max == other.m_max);
        }

        inline bool operator!=(const AxisBinning &other)
        {
            return !(*this == other);
        }

    private:
        u32 m_nBins;
        double m_min;
        double m_max;
};

struct AxisInterval
{
    double minValue;
    double maxValue;
};

inline bool operator==(const AxisInterval &a, const AxisInterval &b)
{
    return (a.minValue == b.minValue && a.maxValue == b.maxValue);
}

struct AxisInfo
{
    QString title;
    QString unit;
};

inline
QString make_title_string(const AxisInfo &axisInfo)
{
    QString result;

    if (!axisInfo.title.isEmpty())
    {
        result = axisInfo.title;
        if (!axisInfo.unit.isEmpty())
        {
            result = QString("%1 <small>[%2]</small>").arg(axisInfo.title).arg(axisInfo.unit);
        }
    }

    return result;
}

static const s32 Histo1DMinBits = 1;
static const s32 Histo1DMaxBits = 20;
static const s32 Histo1DDefBits = 16;

static const s32 Histo2DMinBits = 1;
static const s32 Histo2DMaxBits = 13;
static const s32 Histo2DDefBits = 10;

QComboBox *make_resolution_combo(s32 minBits, s32 maxBits, s32 selectedBits);
// Assumes that selectedRes is a power of 2!
void select_by_resolution(QComboBox *combo, s32 selectedRes);

struct Histo2DAxisLimitsUI
{
    QGroupBox *groupBox;
    QFrame *limitFrame;
    QDoubleSpinBox *spin_min;
    QDoubleSpinBox *spin_max;
};

Histo2DAxisLimitsUI make_histo2d_axis_limits_ui(const QString &groupBoxTitle, double inputMin, double inputMax,
                                                double limitMin, double limitMax);

#endif /* __HISTO_UTIL_H__ */
