#include "histo1d_widget.h"
#include "ui_histo1d_widget.h"
#include "scrollzoomer.h"
#include "util.h"

#include <qwt_plot_curve.h>
#include <qwt_plot_histogram.h>
#include <qwt_plot_magnifier.h>
#include <qwt_plot_panner.h>
#include <qwt_plot_renderer.h>
#include <qwt_plot_textlabel.h>
#include <qwt_scale_engine.h>
#include <qwt_scale_widget.h>
#include <qwt_text.h>

#include <QFileInfo>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#ifdef ENABLE_CALIB_UI
#include <QLabel>
#endif

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

#if 0
            qDebug() << __PRETTY_FUNCTION__
                << "i =" << i
                << "result =" << result
                ;
#endif
            return result;
        }

        virtual QRectF boundingRect() const override
        {
            return QRectF(0, 0, m_histo->getWidth(), m_histo->getMaxValue());
        }

    private:
        Histo1D *m_histo;
};


Histo1DWidget::Histo1DWidget(Histo1D *histo, QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Histo1DWidget)
    , m_histo(histo)
    , m_plotCurve(new QwtPlotCurve)
    , m_replotTimer(new QTimer(this))
{



    ui->setupUi(this);
    ui->label_cursorInfo->setVisible(false);

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
    m_replotTimer->start(2000);

    ui->plot->canvas()->setMouseTracking(true);

    m_zoomer = new ScrollZoomer(ui->plot->canvas());

    // assign the unused yRight axis to only zoom in x
    // Note: I disabled this as it caused a wrong y value to be displayed on
    // the tracker text when creating a zoom rectangle. The effect of not
    // zooming into y is achieved by setting a fixed yAxis after a zoom
    // operation.
    //m_zoomer->setAxis(QwtPlot::xBottom, QwtPlot::yRight);
    m_zoomer->setVScrollBarMode(Qt::ScrollBarAlwaysOff);
    m_zoomer->setZoomBase();

    connect(m_zoomer, &ScrollZoomer::zoomed, this, &Histo1DWidget::zoomerZoomed);
    connect(m_zoomer, &ScrollZoomer::mouseCursorMovedTo, this, &Histo1DWidget::mouseCursorMovedToPlotCoord);
    connect(m_zoomer, &ScrollZoomer::mouseCursorLeftPlot, this, &Histo1DWidget::mouseCursorLeftPlot);

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
    //m_statsTextItem->setZ(42.0); // something > 0
    m_statsTextItem->attach(ui->plot);

    //
    // Calib Ui
    //
#ifdef ENABLE_CALIB_UI
    m_calibUi = new CalibUi;
    m_calibUi->actual1 = new QDoubleSpinBox;
    m_calibUi->actual2 = new QDoubleSpinBox;
    m_calibUi->target1 = new QDoubleSpinBox;
    m_calibUi->target2 = new QDoubleSpinBox;
    m_calibUi->applyButton = new QPushButton(QSL("Apply"));
    m_calibUi->fillMaxButton = new QPushButton(QSL("Vis. Max"));
    m_calibUi->fillMaxButton->setToolTip(QSL("Fill the last focused actual value with the visible maximum histogram value"));
    m_calibUi->resetToFilterButton = new QPushButton(QSL("Restore"));
    m_calibUi->resetToFilterButton->setToolTip(QSL("Restore base unit values from source filter"));

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
#endif

    setHistogram(histo);
    displayChanged();
}

Histo1DWidget::~Histo1DWidget()
{
    delete m_plotCurve;
    delete ui;
    delete m_statsText;
}

void Histo1DWidget::setHistogram(Histo1D *histo)
{
    m_histo = histo;
    m_plotCurve->setData(new Histo1DPointData(m_histo));

    displayChanged();
}

void Histo1DWidget::replot()
{
    updateStatistics();
    updateAxisScales();
    updateCursorInfoLabel();
    ui->plot->replot();
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

    auto name = m_histo->objectName();

    setWindowTitle(QString("Histogram %1 [double based]").arg(name));

    /* Before the scale change the zoomer might have been zoomed into negative
     * x-axis bins. This results in scaling errors and a zoom into negative
     * coordinates which we don't want to allow.
     *
     * To fix this call updateAxes() on the plot to rebuild the axes, then
     * simulate a zoom event with the current zoomRect by calling
     * zoomerZoomed(). This method will then again limit the x-axis' lower
     * bound to 0.0.
     */
    ui->plot->updateAxes();
    zoomerZoomed(m_zoomer->zoomRect());

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

    // do not zoom into negatives

    auto scaleDiv = ui->plot->axisScaleDiv(QwtPlot::xBottom);

    if (scaleDiv.lowerBound() < m_histo->getXMin())
    {
        scaleDiv.setLowerBound(m_histo->getXMin());
        ui->plot->setAxisScaleDiv(QwtPlot::xBottom, scaleDiv);
    }

    scaleDiv = ui->plot->axisScaleDiv(QwtPlot::yLeft);

    if (scaleDiv.lowerBound() < 0.0)
    {
        scaleDiv.setLowerBound(0.0);
        ui->plot->setAxisScaleDiv(QwtPlot::yLeft, scaleDiv);
    }

    replot();
}

void Histo1DWidget::mouseCursorMovedToPlotCoord(QPointF pos)
{
    ui->label_cursorInfo->setVisible(true);
    m_cursorPosition = pos;
    updateCursorInfoLabel();
}

void Histo1DWidget::mouseCursorLeftPlot()
{
    ui->label_cursorInfo->setVisible(false);
}

void Histo1DWidget::updateStatistics()
{
    auto lowerBound = qFloor(ui->plot->axisScaleDiv(QwtPlot::xBottom).lowerBound());
    auto upperBound = qCeil(ui->plot->axisScaleDiv(QwtPlot::xBottom).upperBound());

    m_stats = m_histo->calcStatistics(lowerBound, upperBound);

    static const QString textTemplate = QSL(
        "<table>"
        "<tr><td align=\"left\">Sigma  </td><td>%L1</td></tr>"
        "<tr><td align=\"left\">FWHM   </td><td>%L2</td></tr>"
        "<tr><td align=\"left\">Mean   </td><td>%L3</td></tr>"
        "<tr><td align=\"left\">Max    </td><td>%L4</td></tr>"
        "<tr><td align=\"left\">Max Y  </td><td>%L5</td></tr>"
        "<tr><td align=\"left\">Counts </td><td>%L6</td></tr>"
        "</table>"
        );

    static const int fieldWidth = 0;
    QString buffer = textTemplate
        .arg(m_stats.sigma, fieldWidth)
        .arg(m_stats.fwhm)
        .arg(m_stats.mean, fieldWidth)
        .arg(m_histo->getBinLowEdge(m_stats.maxBin), fieldWidth)
        .arg(m_stats.maxValue, fieldWidth)
        .arg(m_stats.entryCount, fieldWidth)
        ;

    m_statsText->setText(buffer, QwtText::RichText);
    m_statsTextItem->setText(*m_statsText);
}

void Histo1DWidget::updateAxisScales()
{
    // update the y axis using the currently visible max value
    double maxValue = 1.2 * m_stats.maxValue;

    if (maxValue <= 1.0)
        maxValue = 10.0;

    // this sets a fixed y axis scale effectively overriding any changes made by the scrollzoomer
    double base = yAxisIsLog() ? 1.0 : 0.0l;
    ui->plot->setAxisScale(QwtPlot::yLeft, base, maxValue);

    // xAxis
    if (m_zoomer->zoomRectIndex() == 0)
    {
        // fully zoomed out -> set to full resolution
        ui->plot->setAxisScale(QwtPlot::xBottom, m_histo->getXMin(), m_histo->getXMax());
    }
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
    QwtPlotRenderer renderer;
    renderer.exportTo(ui->plot, fileName);
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

    fileName = QFileDialog::getSaveFileName(this, "Save Histogram", fileName, "Text Files (*.txt);; All Files (*.*)");

    if (fileName.isEmpty())
        return;

    QFileInfo fi(fileName);
    if (fi.completeSuffix().isEmpty())
    {
        fileName += ".txt";
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
    if (ui->label_cursorInfo->isVisible())
    {
        u32 ix = static_cast<u32>(std::max(m_cursorPosition.x(), 0.0));

        double x = m_histo->getBinLowEdge(ix);
        double y = m_histo->getBinContent(ix);

        QString text = QString(
                               "x=%1\n"
                               "y=%2\n"
                               "bin=%3"
                               )
            .arg(x)
            .arg(y)
            .arg(ix);

        ui->label_cursorInfo->setText(text);
    }
}

void Histo1DWidget::calibApply()
{
#ifdef ENABLE_CALIB_UI
    double a1 = m_calibUi->actual1->value();
    double a2 = m_calibUi->actual2->value();
    double t1 = m_calibUi->target1->value();
    double t2 = m_calibUi->target2->value();

    if (a1 - a2 == 0.0 || t1 == t2)
        return;

    double a = (t1 - t2) / (a1 - a2);
    double b = t1 - a * a1;

    u32 address = m_histoConfig->getFilterAddress();

    double actualMin = m_sourceFilter->getUnitMin(address);
    double actualMax = m_sourceFilter->getUnitMax(address);

    double targetMin = a * actualMin + b;
    double targetMax = a * actualMax + b;

    qDebug() << __PRETTY_FUNCTION__ << endl
        << "address" << address << endl
        << "a1 a2" << a1 << a2 << endl
        << "t1 t2" << t1 << t2 << endl
        << "aMinMax" << actualMin << actualMax << endl
        << "tMinMax" << targetMin << targetMax;

    m_sourceFilter->setUnitRange(address, targetMin, targetMax);

    m_context->getAnalysisConfig()->updateHistogramsForFilter(m_sourceFilter);

    m_calibUi->actual1->setValue(m_calibUi->target1->value());
    m_calibUi->actual2->setValue(m_calibUi->target2->value());
#endif
}

void Histo1DWidget::calibResetToFilter()
{
#ifdef ENABLE_CALIB_UI
    u32 address = m_histoConfig->getFilterAddress();
    m_sourceFilter->resetToBaseUnits(address);

    m_context->getAnalysisConfig()->updateHistogramsForFilter(m_sourceFilter);
#endif
}

void Histo1DWidget::calibFillMax()
{
#ifdef ENABLE_CALIB_UI
    double maxAt = m_conversionMap.transform(m_stats.maxChannel);
    m_calibUi->lastFocusedActual->setValue(maxAt);
#endif
}

bool Histo1DWidget::eventFilter(QObject *watched, QEvent *event)
{
#ifdef ENABLE_CALIB_UI
    if ((watched == m_calibUi->actual1 || watched == m_calibUi->actual2)
        && (event->type() == QEvent::FocusIn))
    {
        m_calibUi->lastFocusedActual = qobject_cast<QDoubleSpinBox *>(watched);
    }
    return MVMEWidget::eventFilter(watched, event);
#else
    return QWidget::eventFilter(watched, event);
#endif
}

//
// Histo1DListWidget
//
Histo1DListWidget::Histo1DListWidget(const HistoList &histos, QWidget *parent)
    : QWidget(parent)
    , m_histos(histos)
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
            this, &Histo1DListWidget::onHistSpinBoxValueChanged);

    histoSpinLayout->addWidget(histoSpinBox);

    auto controlsLayout = m_histoWidget->ui->controlsLayout;
    controlsLayout->insertWidget(0, gb);

    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(m_histoWidget);

    setWindowTitle(m_histoWidget->windowTitle());
}

void Histo1DListWidget::onHistSpinBoxValueChanged(int index)
{
    auto histo = m_histos.value(index);

    if (histo)
    {
        m_histoWidget->setHistogram(histo.get());
    }
}
