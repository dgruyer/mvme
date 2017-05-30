#include "histo1d_widget.h"
#include "ui_histo1d_widget.h"
#include "histo1d_widget_p.h"
#include "scrollzoomer.h"
#include "util.h"
#include "analysis/analysis.h"
#include "qt-collapsible-section/Section.h"
#include "mvme_context.h"

#include <qwt_plot_curve.h>
#include <qwt_plot_histogram.h>
#include <qwt_plot_magnifier.h>
#include <qwt_plot_marker.h>
#include <qwt_plot_panner.h>
#include <qwt_plot_renderer.h>
#include <qwt_plot_textlabel.h>
#include <qwt_point_data.h>
#include <qwt_scale_engine.h>
#include <qwt_scale_widget.h>
#include <qwt_text.h>

#include <QFileInfo>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

static const s32 ReplotPeriod_ms = 1000;

class Histo1DPointData: public QwtSeriesData<QPointF>
{
    public:
        Histo1DPointData(Histo1D *histo)
            : m_histo(histo)
        {}

        virtual size_t size() const override
        {
            return m_histo->getNumberOfBins();
        }

        virtual QPointF sample(size_t i) const override
        {
            auto result = QPointF(
                m_histo->getBinLowEdge(i),
                m_histo->getBinContent(i));

            return result;
        }

        virtual QRectF boundingRect() const override
        {
            // Qt and Qwt have different understanding of rectangles. For Qt
            // it's top-down like screen coordinates, for Qwt it's bottom-up
            // like the coordinates in a plot.
            //auto result = QRectF(
            //    m_histo->getXMin(),  m_histo->getMaxValue(), // top-left
            //    m_histo->getWidth(), m_histo->getMaxValue());  // width, height
            auto result = QRectF(
                m_histo->getXMin(), 0.0,
                m_histo->getWidth(), m_histo->getMaxValue());

            return result;
        }

    private:
        Histo1D *m_histo;
};

static const double FWHMSigmaFactor = 2.3548;

static inline double squared(double x)
{
    return x * x;
}

#if 0
class Histo1DGaussCurveData: public QwtSeriesData<QPointF>
{
    public:
        Histo1DGaussCurveData(Histo1D *histo)
            : m_histo(histo)
        {
        }

        virtual size_t size() const override
        {
            return m_histo->getNumberOfBins();
        }

        virtual QPointF sample(size_t i) const override
        {
            qDebug() << __PRETTY_FUNCTION__ << "sample index =" << i;

            double x = m_histo->getBinLowEdge(i);
            double s = m_stats.fwhm / FWHMSigmaFactor;
            double a = m_histo->getBinLowEdge(m_stats.maxBin);

            double firstTerm  = m_stats.maxValue; // This is (1.0 / (SqrtPI2 * s)) if the resulting area should be 1.
            double exponent   = -0.5 * ((squared(x - a) / squared(s)));
            double secondTerm = std::exp(exponent);
            double yValue     = firstTerm * secondTerm;

            qDebug("i=%d, x=%lf, s=%lf, a=%lf, stats.maxBin=%d",
                   i, x, s, a, m_stats.maxBin);

            qDebug("firstTerm=%lf, exponent=%lf, secondTerm=%lf, yValue=%lf",
                   firstTerm, exponent, secondTerm, yValue);

            double y = yValue;

            QPointF result(x, yValue);
            return result;
        }

        virtual QRectF boundingRect() const override
        {
            auto result = QRectF(
                m_histo->getXMin(), 0.0,
                m_histo->getWidth(), m_histo->getMaxValue());

            return result;
        }

        void setStats(Histo1DStatistics stats)
        {
            m_stats = stats;
        }

    private:
        Histo1D *m_histo;
        Histo1DStatistics m_stats;
};
#else
/* Calculates a gauss fit using the currently visible maximum histogram value.
 *
 * Note: The resolution is independent of the underlying histograms resolution.
 * Instead NumberOfPoints samples are used at all zoom levels.
 */
class Histo1DGaussCurveData: public QwtSyntheticPointData
{
    static const size_t NumberOfPoints = 1000;

    public:
        Histo1DGaussCurveData(Histo1D *histo)
            : QwtSyntheticPointData(NumberOfPoints)
            , m_histo(histo)
        {
        }

        virtual double y(double x) const override
        {
            double s = m_stats.fwhm / FWHMSigmaFactor;
            // Instead of using the center of the max bin the center point
            // between the fwhm edges is used. This makes the curve remain in a
            // much more stable x-position.
            double a = m_stats.fwhmCenter;

            double firstTerm  = m_stats.maxValue; // This is (1.0 / (SqrtPI2 * s)) if the resulting area should be 1.
            double exponent   = -0.5 * ((squared(x - a) / squared(s)));
            double secondTerm = std::exp(exponent);
            double yValue     = firstTerm * secondTerm;

            //qDebug("x=%lf, s=%lf, a=%lf, stats.maxBin=%d",
            //       x, s, a, m_stats.maxBin);
            //qDebug("firstTerm=%lf, exponent=%lf, secondTerm=%lf, yValue=%lf",
            //       firstTerm, exponent, secondTerm, yValue);

            return yValue;
        }

        void setStats(Histo1DStatistics stats)
        {
            m_stats = stats;
        }

    private:
        Histo1D *m_histo;
        Histo1DStatistics m_stats;
};
#endif

struct CalibUi
{
    QDoubleSpinBox *actual1, *actual2,
                   *target1, *target2,
                   *lastFocusedActual;
    QPushButton *applyButton,
                *fillMaxButton,
                *resetToFilterButton;
};

struct RateEstimationData
{
    bool visible = false;
    double x1 = make_quiet_nan();
    double x2 = make_quiet_nan();

};

static const double PlotTextLayerZ  = 1000.0;
static const double PlotGaussLayerZ = 1001.0;

struct Histo1DWidgetPrivate
{
    Histo1DWidget *m_q;

    RateEstimationData m_rateEstimationData;
    QwtPlotPicker *m_ratePointPicker;
    QwtPlotMarker *m_rateX1Marker;
    QwtPlotMarker *m_rateX2Marker;
    QwtPlotMarker *m_rateFormulaMarker;

    QwtPlotCurve *m_gaussCurve = nullptr;
};

Histo1DWidget::Histo1DWidget(const Histo1DPtr &histoPtr, QWidget *parent)
    : Histo1DWidget(histoPtr.get(), parent)
{
    m_histoPtr = histoPtr;
}

Histo1DWidget::Histo1DWidget(Histo1D *histo, QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Histo1DWidget)
    , m_d(new Histo1DWidgetPrivate)
    , m_histo(histo)
    , m_plotCurve(new QwtPlotCurve)
    , m_replotTimer(new QTimer(this))
    , m_cursorPosition(make_quiet_nan(), make_quiet_nan())
    , m_labelCursorInfoWidth(-1)
    , m_context(nullptr)
{
    m_d->m_q = this;
    ui->setupUi(this);

    ui->tb_info->setEnabled(false);
    ui->tb_subRange->setEnabled(false);

    connect(ui->pb_export, &QPushButton::clicked, this, &Histo1DWidget::exportPlot);
    connect(ui->pb_save, &QPushButton::clicked, this, &Histo1DWidget::saveHistogram);

    connect(ui->pb_clear, &QPushButton::clicked, this, [this] {
        m_histo->clear();
        replot();
    });

    connect(ui->linLogGroup, SIGNAL(buttonClicked(int)), this, SLOT(displayChanged()));

    m_plotCurve->setStyle(QwtPlotCurve::Steps);
    m_plotCurve->setCurveAttribute(QwtPlotCurve::Inverted);
    m_plotCurve->attach(ui->plot);

    ui->plot->axisWidget(QwtPlot::yLeft)->setTitle("Counts");

    connect(m_replotTimer, SIGNAL(timeout()), this, SLOT(replot()));
    m_replotTimer->start(ReplotPeriod_ms);

    ui->plot->canvas()->setMouseTracking(true);

    m_zoomer = new ScrollZoomer(ui->plot->canvas());

    m_zoomer->setVScrollBarMode(Qt::ScrollBarAlwaysOff);
    m_zoomer->setZoomBase();
    qDebug() << "zoomRectIndex()" << m_zoomer->zoomRectIndex();

    connect(m_zoomer, &ScrollZoomer::zoomed, this, &Histo1DWidget::zoomerZoomed);
    connect(m_zoomer, &ScrollZoomer::mouseCursorMovedTo, this, &Histo1DWidget::mouseCursorMovedToPlotCoord);
    connect(m_zoomer, &ScrollZoomer::mouseCursorLeftPlot, this, &Histo1DWidget::mouseCursorLeftPlot);

    connect(m_histo, &Histo1D::axisBinningChanged, this, [this] (Qt::Axis) {
        // Handle axis changes by zooming out fully. This will make sure
        // possible axis scale changes are immediately visible and the zoomer
        // is in a clean state.
        m_zoomer->setZoomStack(QStack<QRectF>(), -1);
        m_zoomer->zoom(0);
        replot();
    });

#if 0
    auto plotPanner = new QwtPlotPanner(ui->plot->canvas());
    plotPanner->setAxisEnabled(QwtPlot::yLeft, false);
    plotPanner->setMouseButton(Qt::MiddleButton);
#endif

#if 0
    auto plotMagnifier = new QwtPlotMagnifier(ui->plot->canvas());
    plotMagnifier->setAxisEnabled(QwtPlot::yLeft, false);
    plotMagnifier->setMouseButton(Qt::NoButton);
#endif

    //
    // Stats text
    //
    m_statsText = new QwtText;
    /* This controls the alignment of the whole text on the canvas aswell as
     * the alignment of text itself. */
    m_statsText->setRenderFlags(Qt::AlignRight | Qt::AlignTop);

    QPen borderPen(Qt::SolidLine);
    borderPen.setColor(Qt::black);
    m_statsText->setBorderPen(borderPen);

    QBrush brush;
    brush.setColor("#e6e2de");
    brush.setStyle(Qt::SolidPattern);
    m_statsText->setBackgroundBrush(brush);

    /* The text rendered by qwt looked non-antialiased when using the RichText
     * format. Manually setting the pixelSize fixes this. */
    QFont font;
    font.setPixelSize(12);
    m_statsText->setFont(font);

    m_statsTextItem = new QwtPlotTextLabel;
    //m_statsTextItem->setRenderHint(QwtPlotItem::RenderAntialiased);
    /* Margin added to contentsMargins() of the canvas. This is (mis)used to
     * not clip the top scrollbar. */
    m_statsTextItem->setMargin(15);
    m_statsTextItem->setText(*m_statsText);
    //m_statsTextItem->setZ(42.0); // something > 0 // FIXME
    m_statsTextItem->attach(ui->plot);

    //
    // Calib Ui
    //
    m_calibUi = new CalibUi;
    m_calibUi->actual1 = new QDoubleSpinBox;
    m_calibUi->actual2 = new QDoubleSpinBox;
    m_calibUi->target1 = new QDoubleSpinBox;
    m_calibUi->target2 = new QDoubleSpinBox;
    m_calibUi->applyButton = new QPushButton(QSL("Apply"));
    m_calibUi->fillMaxButton = new QPushButton(QSL("Vis. Max"));
    m_calibUi->fillMaxButton->setToolTip(QSL("Fill the last focused actual value with the visible maximum histogram value"));
    m_calibUi->resetToFilterButton = new QPushButton(QSL("Restore"));
    m_calibUi->resetToFilterButton->setToolTip(QSL("Restore base unit values from source calibration"));

    m_calibUi->lastFocusedActual = m_calibUi->actual2;
    m_calibUi->actual1->installEventFilter(this);
    m_calibUi->actual2->installEventFilter(this);

    connect(m_calibUi->applyButton, &QPushButton::clicked, this, &Histo1DWidget::calibApply);
    connect(m_calibUi->fillMaxButton, &QPushButton::clicked, this, &Histo1DWidget::calibFillMax);
    connect(m_calibUi->resetToFilterButton, &QPushButton::clicked, this, &Histo1DWidget::calibResetToFilter);

    QVector<QDoubleSpinBox *> spins = { m_calibUi->actual1, m_calibUi->actual2, m_calibUi->target1, m_calibUi->target2 };

    for (auto spin: spins)
    {
        spin->setDecimals(4);
        spin->setSingleStep(0.0001);
        spin->setMinimum(std::numeric_limits<double>::lowest());
        spin->setMaximum(std::numeric_limits<double>::max());
        spin->setValue(0.0);
    }

    auto calibLayout = new QGridLayout;
    calibLayout->setContentsMargins(3, 3, 3, 3);
    calibLayout->setSpacing(2);

    calibLayout->addWidget(new QLabel(QSL("Actual")), 0, 0, Qt::AlignHCenter);
    calibLayout->addWidget(new QLabel(QSL("Target")), 0, 1, Qt::AlignHCenter);

    calibLayout->addWidget(m_calibUi->actual1, 1, 0);
    calibLayout->addWidget(m_calibUi->target1, 1, 1);

    calibLayout->addWidget(m_calibUi->actual2, 2, 0);
    calibLayout->addWidget(m_calibUi->target2, 2, 1);

    calibLayout->addWidget(m_calibUi->fillMaxButton, 3, 0, 1, 1);
    calibLayout->addWidget(m_calibUi->applyButton, 3, 1, 1, 1);

    calibLayout->addWidget(m_calibUi->resetToFilterButton, 4, 0, 1, 1);

    auto calibSection = new Section(QSL("Calibration"));
    calibSection->setContentLayout(*calibLayout);

    auto calibFrameLayout = new QHBoxLayout(ui->frame_calib);
    calibFrameLayout->setContentsMargins(0, 0, 0, 0);
    calibFrameLayout->addWidget(calibSection);

    // Hide the calibration UI. It will be shown if setCalibrationInfo() is called.
    ui->frame_calib->setVisible(false);

    //
    // Rate Estimation
    //
    m_d->m_rateEstimationData.visible = false;

    auto make_position_marker = [](QwtPlot *plot)
    {
        auto marker = new QwtPlotMarker;
        marker->setLabelAlignment( Qt::AlignLeft | Qt::AlignBottom );
        marker->setLabelOrientation( Qt::Vertical );
        marker->setLineStyle( QwtPlotMarker::VLine );
        marker->setLinePen( Qt::black, 0, Qt::DashDotLine );
        marker->setZ(PlotTextLayerZ);
        marker->attach(plot);
        marker->hide();
        return marker;
    };

    m_d->m_rateX1Marker = make_position_marker(ui->plot);
    m_d->m_rateX2Marker = make_position_marker(ui->plot);

    m_d->m_rateFormulaMarker = new QwtPlotMarker;
    m_d->m_rateFormulaMarker->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
    m_d->m_rateFormulaMarker->setZ(PlotTextLayerZ);
    m_d->m_rateFormulaMarker->attach(ui->plot);
    m_d->m_rateFormulaMarker->hide();

    m_d->m_ratePointPicker = new QwtPlotPicker(QwtPlot::xBottom, QwtPlot::yLeft,
                                               QwtPicker::VLineRubberBand, QwtPicker::ActiveOnly,
                                               ui->plot->canvas());
    QPen pickerPen(Qt::red);
    m_d->m_ratePointPicker->setTrackerPen(pickerPen);
    m_d->m_ratePointPicker->setRubberBandPen(pickerPen);
    m_d->m_ratePointPicker->setStateMachine(new AutoBeginClickPointMachine);
    m_d->m_ratePointPicker->setEnabled(false);

    connect(m_d->m_ratePointPicker, static_cast<void (QwtPlotPicker::*)(const QPointF &)>(&QwtPlotPicker::selected), [this](const QPointF &pos) {

        if (std::isnan(m_d->m_rateEstimationData.x1))
        {
            m_d->m_rateEstimationData.x1 = pos.x();

            m_d->m_rateX1Marker->setXValue(m_d->m_rateEstimationData.x1);
            m_d->m_rateX1Marker->setLabel(QString("    x1=%1").arg(m_d->m_rateEstimationData.x1));
            m_d->m_rateX1Marker->show();
        }
        else if (std::isnan(m_d->m_rateEstimationData.x2))
        {
            m_d->m_rateEstimationData.x2 = pos.x();

            if (m_d->m_rateEstimationData.x1 > m_d->m_rateEstimationData.x2)
            {
                std::swap(m_d->m_rateEstimationData.x1, m_d->m_rateEstimationData.x2);
            }

            m_d->m_rateEstimationData.visible = true;
            m_d->m_ratePointPicker->setEnabled(false);
            m_zoomer->setEnabled(true);

            // set both x1 and x2 as they might have been swapped above
            m_d->m_rateX1Marker->setXValue(m_d->m_rateEstimationData.x1);
            m_d->m_rateX1Marker->setLabel(QString("    x1=%1").arg(m_d->m_rateEstimationData.x1));
            m_d->m_rateX2Marker->setXValue(m_d->m_rateEstimationData.x2);
            m_d->m_rateX2Marker->setLabel(QString("    x2=%1").arg(m_d->m_rateEstimationData.x2));
            m_d->m_rateX2Marker->show();
        }
        else
        {
            InvalidCodePath;
        }

        replot();
    });

    //
    // Gauss Curve
    //
    m_d->m_gaussCurve = new QwtPlotCurve;
    m_d->m_gaussCurve->setZ(PlotGaussLayerZ);
    m_d->m_gaussCurve->setPen(Qt::green, 2.0);
    m_d->m_gaussCurve->attach(ui->plot);
    m_d->m_gaussCurve->hide();

#if 0
    connect(m_d->m_ratePointPicker, &QwtPicker::activated, this, [this](bool on) {
        qDebug() << __PRETTY_FUNCTION__ << "m_ratePointPicker activated" << on;
        if (!on)
        {
            qDebug() << __PRETTY_FUNCTION__ << "m_ratePointPicker got deactivated, selection =" << m_d->m_ratePointPicker->selection();
        }
    });

    connect(m_d->m_ratePointPicker, &QwtPlotPicker::appended, this, [](const QPointF &pos) {
        qDebug() << __PRETTY_FUNCTION__ << "m_ratePointPicker appended" << pos;
    });

    //connect(m_ratePointPicker, &QwtPlotPicker::moved, this, [](const QPointF &pos) {
    //    qDebug() << __PRETTY_FUNCTION__ << "m_ratePointPicker moved" << pos;
    //});


    connect(m_d->m_ratePointPicker, static_cast<void (QwtPlotPicker::*)(const QVector<QPointF> &)>(&QwtPlotPicker::selected), [this](const QVector<QPointF> &pa) {
        qDebug() << __PRETTY_FUNCTION__ << "m_ratePointPicker selected" << pa;
    });
#endif

    setHistogram(histo);
}

Histo1DWidget::~Histo1DWidget()
{
    delete m_plotCurve;
    delete ui;
    delete m_statsText;
    delete m_d;
}

void Histo1DWidget::setHistogram(const Histo1DPtr &histoPtr)
{
    m_histoPtr = histoPtr;

    setHistogram(histoPtr.get());
}

void Histo1DWidget::setHistogram(Histo1D *histo)
{
    m_histo = histo;
    m_plotCurve->setData(new Histo1DPointData(m_histo));
    m_d->m_gaussCurve->setData(new Histo1DGaussCurveData(m_histo));

    // Reset the zoom stack and zoom fully zoom out as the scales might be
    // completely different now.
    // FIXME: this is not good for the usage of projection widgets where the
    // histo is replaced with a similar one. The zoom level should stay the same in that case...
    // Maybe compare the axses before replacing the histo and decide based on
    // that whether to reset the zoom stack or not.
    //m_zoomer->setZoomStack(QStack<QRectF>(), -1);
    //m_zoomer->zoom(0);

    displayChanged();
    replot();
}

void Histo1DWidget::updateAxisScales()
{
    // Scale the y axis using the currently visible max value plus 20%
    double maxValue = m_stats.maxValue;

    // force a minimum of 10 units in y
    if (maxValue <= 1.0)
        maxValue = 10.0;

    double base;

    if (yAxisIsLog())
    {
        base = 1.0;
        maxValue = std::pow(maxValue, 1.2);
    }
    else
    {
        base = 0.0;
        maxValue = maxValue * 1.2;
    }

    // This sets a fixed y axis scale effectively overriding any changes made
    // by the scrollzoomer.
    ui->plot->setAxisScale(QwtPlot::yLeft, base, maxValue);

    // xAxis
    if (m_zoomer->zoomRectIndex() == 0)
    {
        // fully zoomed out -> set to full resolution
        ui->plot->setAxisScale(QwtPlot::xBottom, m_histo->getXMin(), m_histo->getXMax());
        m_zoomer->setZoomBase();
    }

    ui->plot->updateAxes();
}

void Histo1DWidget::replot()
{
    updateStatistics();
    updateAxisScales();
    updateCursorInfoLabel();

    // update histo info label
    auto infoText = QString("Underflow: %1\n"
                            "Overflow:  %2")
        .arg(m_histo->getUnderflow())
        .arg(m_histo->getOverflow());


    // rate and efficiency estimation
    if (m_d->m_rateEstimationData.visible)
    {
        /* This code tries to interpolate the exponential function formed by
         * the two selected data points. */
        double x1 = m_d->m_rateEstimationData.x1;
        double x2 = m_d->m_rateEstimationData.x2;
        double y1 = m_histo->getValue(x1);
        double y2 = m_histo->getValue(x2);

        double tau = (x2 - x1) / log(y1 / y2);
        double e = exp(1.0);
        double c = pow(e, x1 / tau) * y1;
        double c_norm = c / m_histo->getBinWidth(); // norm to x-axis scale
        double freeRate = 1.0 / tau; // 1/x-axis unit
        double freeCounts = c_norm * tau * (1 - pow(e, -(x2 / tau))); // for interval 0..x2
        double histoCounts = m_histo->calcStatistics(0.0, x2).entryCount;
        double efficiency  = histoCounts / freeCounts;

#if 0
        infoText += QString("\n"
                            "(x1, y1)=(%1, %2)\n"
                            "(x2, y2)=(%3, %4)\n"
                            "tau=%5, c=%6, c_norm=%11\n"
                            "FR=%7, FC=%8, HC=%9\n"
                            "efficiency=%10")
            .arg(x1)
            .arg(y1)
            .arg(x2)
            .arg(y2)
            .arg(tau)
            .arg(c)
            .arg(freeRate)
            .arg(freeCounts)
            .arg(histoCounts)
            .arg(efficiency)
            .arg(c_norm)
            ;
#endif

        QString markerText;

        if (!std::isnan(c) && !std::isnan(tau) && !std::isnan(efficiency))
        {
            markerText = QString(QSL("freeRate=%1 <sup>1</sup>&frasl;<sub>%2</sub>; eff=%3")
                                 .arg(freeRate, 0, 'g', 4)
                                 .arg(m_histo->getAxisInfo(Qt::XAxis).unit)
                                 .arg(efficiency, 0, 'g', 4)
                                );
        }
        else
        {
            markerText = QSL("");
        }

        QwtText rateFormulaText(markerText, QwtText::RichText);
        auto font = rateFormulaText.font();
        font.setPointSize(font.pointSize() + 1);
        rateFormulaText.setFont(font);
        m_d->m_rateFormulaMarker->setXValue(x1);

        /* The goal is to draw the marker at 0.9 of the plot height. Doing this
         * in plot coordinates does work for a linear y-axis scale but
         * positions the text way too high for a logarithmic scale. Instead of
         * using plot coordinates directly we're using 0.9 of the canvas height
         * and transform that pixel coordinate to a plot coordinate.
         */
        s32 canvasHeight = ui->plot->canvas()->height();
        s32 pixelY = canvasHeight - canvasHeight * 0.9;
        double plotY = ui->plot->canvasMap(QwtPlot::yLeft).invTransform(pixelY);

        m_d->m_rateFormulaMarker->setYValue(plotY);
        m_d->m_rateFormulaMarker->setLabel(rateFormulaText);
        m_d->m_rateFormulaMarker->show();
    }

    ui->label_histoInfo->setText(infoText);

    // window and axis titles
    auto name = m_histo->objectName();
    setWindowTitle(QString("Histogram %1").arg(name));

    auto axisInfo = m_histo->getAxisInfo(Qt::XAxis);
    ui->plot->axisWidget(QwtPlot::xBottom)->setTitle(make_title_string(axisInfo));

    ui->plot->replot();

#if 0
    // prints plot item pointers and their z value
    for (auto item: ui->plot->itemList())
    {
        qDebug() << __PRETTY_FUNCTION__ << item << item->z();
    }
#endif
}

void Histo1DWidget::displayChanged()
{
    if (ui->scaleLin->isChecked() && !yAxisIsLin())
    {
        ui->plot->setAxisScaleEngine(QwtPlot::yLeft, new QwtLinearScaleEngine);
        ui->plot->setAxisAutoScale(QwtPlot::yLeft, true);
    }
    else if (ui->scaleLog->isChecked() && !yAxisIsLog())
    {
        auto scaleEngine = new QwtLogScaleEngine;
        scaleEngine->setTransformation(new MinBoundLogTransform);
        ui->plot->setAxisScaleEngine(QwtPlot::yLeft, scaleEngine);
    }

    replot();
}

void Histo1DWidget::zoomerZoomed(const QRectF &zoomRect)
{
    if (m_zoomer->zoomRectIndex() == 0)
    {
        // fully zoomed out -> set to full resolution
        ui->plot->setAxisScale(QwtPlot::xBottom, m_histo->getXMin(), m_histo->getXMax());
        ui->plot->replot();
        m_zoomer->setZoomBase();
    }

    // do not zoom outside the histogram range
    auto scaleDiv = ui->plot->axisScaleDiv(QwtPlot::xBottom);
    double lowerBound = scaleDiv.lowerBound();
    double upperBound = scaleDiv.upperBound();

    if (lowerBound <= upperBound)
    {
        if (lowerBound < m_histo->getXMin())
        {
            scaleDiv.setLowerBound(m_histo->getXMin());
        }

        if (upperBound > m_histo->getXMax())
        {
            scaleDiv.setUpperBound(m_histo->getXMax());
        }
    }
    else
    {
        if (lowerBound > m_histo->getXMin())
        {
            scaleDiv.setLowerBound(m_histo->getXMin());
        }

        if (upperBound < m_histo->getXMax())
        {
            scaleDiv.setUpperBound(m_histo->getXMax());
        }
    }

    ui->plot->setAxisScaleDiv(QwtPlot::xBottom, scaleDiv);

    replot();
}

void Histo1DWidget::mouseCursorMovedToPlotCoord(QPointF pos)
{
    m_cursorPosition = pos;
    updateCursorInfoLabel();
}

void Histo1DWidget::mouseCursorLeftPlot()
{
    m_cursorPosition = QPointF(make_quiet_nan(), make_quiet_nan());
    updateCursorInfoLabel();
}

void Histo1DWidget::updateStatistics()
{
    double lowerBound = qFloor(ui->plot->axisScaleDiv(QwtPlot::xBottom).lowerBound());
    double upperBound = qCeil(ui->plot->axisScaleDiv(QwtPlot::xBottom).upperBound());

    m_stats = m_histo->calcStatistics(lowerBound, upperBound);

    static const QString textTemplate = QSL(
        "<table>"
        "<tr><td align=\"left\">RMS    </td><td>%L1</td></tr>"
        "<tr><td align=\"left\">FWHM   </td><td>%L2</td></tr>"
        "<tr><td align=\"left\">Mean   </td><td>%L3</td></tr>"
        "<tr><td align=\"left\">Max    </td><td>%L4</td></tr>"
        "<tr><td align=\"left\">Max Y  </td><td>%L5</td></tr>"
        "<tr><td align=\"left\">Counts </td><td>%L6</td></tr>"
        "</table>"
        );

    double maxBinCenter = (m_stats.entryCount > 0) ? m_histo->getBinCenter(m_stats.maxBin) : 0.0;

    static const int fieldWidth = 0;
    QString buffer = textTemplate
        .arg(m_stats.sigma, fieldWidth)
        .arg(m_stats.fwhm)
        .arg(m_stats.mean, fieldWidth)
        .arg(maxBinCenter, fieldWidth)
        .arg(m_stats.maxValue, fieldWidth)
        .arg(m_stats.entryCount, fieldWidth)
        ;

    m_statsText->setText(buffer, QwtText::RichText);
    m_statsTextItem->setText(*m_statsText);

    auto curveData = reinterpret_cast<Histo1DGaussCurveData *>(m_d->m_gaussCurve->data());
    curveData->setStats(m_stats);
}

bool Histo1DWidget::yAxisIsLog()
{
    return dynamic_cast<QwtLogScaleEngine *>(ui->plot->axisScaleEngine(QwtPlot::yLeft));
}

bool Histo1DWidget::yAxisIsLin()
{
    return dynamic_cast<QwtLinearScaleEngine *>(ui->plot->axisScaleEngine(QwtPlot::yLeft));
}

void Histo1DWidget::exportPlot()
{
    QString fileName = m_histo->objectName();
    fileName.replace("/", "_");
    fileName.replace("\\", "_");
    fileName += QSL(".pdf");

    if (m_context)
    {
        fileName = QDir(m_context->getWorkspacePath(QSL("PlotsDirectory"))).filePath(fileName);
    }

    ui->plot->setTitle(m_histo->getTitle());
    QwtText footerText(m_histo->getFooter());
    footerText.setRenderFlags(Qt::AlignLeft);
    ui->plot->setFooter(footerText);

    QwtPlotRenderer renderer;
    renderer.setDiscardFlags(QwtPlotRenderer::DiscardBackground | QwtPlotRenderer::DiscardCanvasBackground);
    renderer.setLayoutFlag(QwtPlotRenderer::FrameWithScales);
    renderer.exportTo(ui->plot, fileName);

    ui->plot->setTitle(QString());
    ui->plot->setFooter(QString());
}

void Histo1DWidget::saveHistogram()
{
    QString path = QSettings().value("Files/LastHistogramExportDirectory").toString();

    if (path.isEmpty())
    {
        path = QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation).at(0);
    }

    auto name = m_histo->objectName();

    QString fileName = QString("%1/%2.txt")
        .arg(path)
        .arg(name);

    qDebug() << fileName;

    fileName = QFileDialog::getSaveFileName(this, "Save Histogram", fileName, "Text Files (*.histo1d);; All Files (*.*)");

    if (fileName.isEmpty())
        return;

    QFileInfo fi(fileName);
    if (fi.completeSuffix().isEmpty())
    {
        fileName += ".histo1d";
    }

    QFile outFile(fileName);
    if (!outFile.open(QIODevice::WriteOnly))
    {
        QMessageBox::critical(0, "Error", QString("Error opening %1 for writing").arg(fileName));
        return;
    }

    QTextStream out(&outFile);
    writeHisto1D(out, m_histo);

    if (out.status() == QTextStream::Ok)
    {
        fi.setFile(fileName);
        QSettings().setValue("Files/LastHistogramExportDirectory", fi.absolutePath());
    }
}

void Histo1DWidget::updateCursorInfoLabel()
{
    double plotX = m_cursorPosition.x();
    double plotY = m_cursorPosition.y();
    auto binning = m_histo->getAxisBinning(Qt::XAxis);
    s64 binX = binning.getBin(plotX);

    QString text;

    if (!qIsNaN(plotX) && !qIsNaN(plotY) && binX >= 0)
    {
        double x = plotX;
        double y = m_histo->getBinContent(binX);
        double binLowEdge = binning.getBinLowEdge((u32)binX);

        text = QString("x=%1\n"
                       "y=%2\n"
                       "bin=%3\n"
                       "low edge=%4"
                      )
            .arg(x)
            .arg(y)
            .arg(binX)
            .arg(binLowEdge)
            ;
#if 0
        double binXUnchecked = binning.getBinUnchecked(plotX);

        auto sl = QStringList()
            << QString("cursorPlotX=%1").arg(plotX)
            << QString("binXUnchecked=%1").arg(binXUnchecked)
            << QString("binX=%1, u=%2, o=%3").arg(binX).arg(binX == AxisBinning::Underflow).arg(binX == AxisBinning::Overflow)
            << QString("nBins=%1").arg(binning.getBins())
            << QString("binXLow=%1").arg(binning.getBinLowEdge(binX))
            << QString("binXCenter=%1").arg(binning.getBinCenter(binX))
            << QString("binWidth=%1").arg(binning.getBinWidth())
            << QString("Y=%1").arg(m_histo->getBinContent(binX))
            << QString("minX=%1, maxX=%2").arg(m_histo->getXMin()).arg(m_histo->getXMax());
        ;

        QString text = sl.join("\n");
#endif
    }

    // update the label which will calculate a new width
    ui->label_cursorInfo->setText(text);
    // use the largest width the label ever had to stop the label from constantly changing its width
    m_labelCursorInfoWidth = std::max(m_labelCursorInfoWidth, ui->label_cursorInfo->width());
    ui->label_cursorInfo->setMinimumWidth(m_labelCursorInfoWidth);
}

void Histo1DWidget::setCalibrationInfo(const std::shared_ptr<analysis::CalibrationMinMax> &calib, s32 histoAddress)
{
    m_calib = calib;
    m_histoAddress = histoAddress;
    ui->frame_calib->setVisible(m_calib != nullptr);
}

void Histo1DWidget::calibApply()
{
    Q_ASSERT(m_calib);
    Q_ASSERT(m_context);

    double a1 = m_calibUi->actual1->value();
    double a2 = m_calibUi->actual2->value();
    double t1 = m_calibUi->target1->value();
    double t2 = m_calibUi->target2->value();

    if (a1 - a2 == 0.0 || t1 == t2)
        return;

    double a = (t1 - t2) / (a1 - a2);
    double b = t1 - a * a1;

    u32 address = m_histoAddress;

    double actualMin = m_calib->getCalibration(address).unitMin;
    double actualMax = m_calib->getCalibration(address).unitMax;

    double targetMin = a * actualMin + b;
    double targetMax = a * actualMax + b;

    qDebug() << __PRETTY_FUNCTION__ << endl
        << "address" << address << endl
        << "a1 a2" << a1 << a2 << endl
        << "t1 t2" << t1 << t2 << endl
        << "aMinMax" << actualMin << actualMax << endl
        << "tMinMax" << targetMin << targetMax;

    m_calibUi->actual1->setValue(m_calibUi->target1->value());
    m_calibUi->actual2->setValue(m_calibUi->target2->value());

    AnalysisPauser pauser(m_context);
    m_calib->setCalibration(address, targetMin, targetMax);
    analysis::do_beginRun_forward(m_calib.get());

    on_tb_rate_toggled(m_d->m_rateEstimationData.visible);
}

void Histo1DWidget::calibResetToFilter()
{
    Q_ASSERT(m_calib);
    Q_ASSERT(m_context);

    using namespace analysis;

    Pipe *inputPipe = m_calib->getSlot(0)->inputPipe;
    if (inputPipe)
    {
        Parameter *inputParam = inputPipe->getParameter(m_histoAddress);
        if (inputParam)
        {
            double minValue = inputParam->lowerLimit;
            double maxValue = inputParam->upperLimit;
            AnalysisPauser pauser(m_context);
            m_calib->setCalibration(m_histoAddress, minValue, maxValue);
            analysis::do_beginRun_forward(m_calib.get());
        }
    }
}

void Histo1DWidget::calibFillMax()
{
    double maxAt = m_histo->getAxisBinning(Qt::XAxis).getBinCenter(m_stats.maxBin);
    m_calibUi->lastFocusedActual->setValue(maxAt);
}

bool Histo1DWidget::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == m_calibUi->actual1 || watched == m_calibUi->actual2)
        && (event->type() == QEvent::FocusIn))
    {
        m_calibUi->lastFocusedActual = qobject_cast<QDoubleSpinBox *>(watched);
    }
    return QWidget::eventFilter(watched, event);
}

void Histo1DWidget::on_tb_info_clicked()
{
    Q_ASSERT(!"Not implemented");
}

void Histo1DWidget::setSink(const SinkPtr &sink, HistoSinkCallback sinkModifiedCallback)
{
    Q_ASSERT(sink);
    m_sink = sink;
    m_sinkModifiedCallback = sinkModifiedCallback;
    ui->tb_subRange->setEnabled(true);
}

void Histo1DWidget::on_tb_subRange_clicked()
{
    Q_ASSERT(m_sink);
    double visibleMinX = ui->plot->axisScaleDiv(QwtPlot::xBottom).lowerBound();
    double visibleMaxX = ui->plot->axisScaleDiv(QwtPlot::xBottom).upperBound();
    Histo1DSubRangeDialog dialog(m_sink, m_sinkModifiedCallback, visibleMinX, visibleMaxX, this);
    dialog.exec();
}

void Histo1DWidget::on_tb_rate_toggled(bool checked)
{
    if (checked)
    {
        m_d->m_rateEstimationData = RateEstimationData();
        m_d->m_ratePointPicker->setEnabled(true);
        m_zoomer->setEnabled(false);
    }
    else
    {
        m_d->m_rateEstimationData.visible = false;
        m_d->m_ratePointPicker->setEnabled(false);
        m_zoomer->setEnabled(true);
        m_d->m_rateX1Marker->hide();
        m_d->m_rateX2Marker->hide();
        m_d->m_rateFormulaMarker->hide();
        replot();
    }
}

void Histo1DWidget::on_tb_gauss_toggled(bool checked)
{
    if (checked)
    {
        m_d->m_gaussCurve->show();
    }
    else
    {
        m_d->m_gaussCurve->hide();
    }

    replot();
}

void Histo1DWidget::on_tb_test_clicked()
{
}

//
// Histo1DListWidget
//
Histo1DListWidget::Histo1DListWidget(const HistoList &histos, QWidget *parent)
    : QWidget(parent)
    , m_histos(histos)
    , m_currentIndex(0)
{
    Q_ASSERT(histos.size());

    auto histo = histos[0].get();
    m_histoWidget = new Histo1DWidget(histo, this);

    connect(m_histoWidget, &QWidget::windowTitleChanged, this, &QWidget::setWindowTitle);

    /* create the controls to switch the current histogram and inject into the
     * histo widget layout. */
    auto gb = new QGroupBox(QSL("Histogram"));
    auto histoSpinLayout = new QHBoxLayout(gb);
    histoSpinLayout->setContentsMargins(0, 0, 0, 0);

    auto histoSpinBox = new QSpinBox;
    histoSpinBox->setMaximum(histos.size() - 1);
    connect(histoSpinBox, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &Histo1DListWidget::onHistoSpinBoxValueChanged);

    histoSpinLayout->addWidget(histoSpinBox);

    auto controlsLayout = m_histoWidget->ui->controlsLayout;
    controlsLayout->insertWidget(0, gb);

    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(m_histoWidget);

    setWindowTitle(m_histoWidget->windowTitle());
    onHistoSpinBoxValueChanged(0);
}

void Histo1DListWidget::onHistoSpinBoxValueChanged(int index)
{
    m_currentIndex = index;
    auto histo = m_histos.value(index);

    if (histo)
    {
        m_histoWidget->setHistogram(histo.get());
        m_histoWidget->setContext(m_context);

        if (m_calib)
        {
            m_histoWidget->setCalibrationInfo(m_calib, index);
        }

        if (m_sink)
        {
            m_histoWidget->setSink(m_sink, m_sinkModifiedCallback);
        }
    }
}

void Histo1DListWidget::setCalibration(const std::shared_ptr<analysis::CalibrationMinMax> &calib)
{
    m_calib = calib;
    if (m_calib)
    {
        m_histoWidget->setCalibrationInfo(m_calib, m_currentIndex);
    }
}

void Histo1DListWidget::setSink(const SinkPtr &sink, HistoSinkCallback sinkModifiedCallback)
{
    Q_ASSERT(sink);
    m_sink = sink;
    m_sinkModifiedCallback = sinkModifiedCallback;

    onHistoSpinBoxValueChanged(m_currentIndex);
}
