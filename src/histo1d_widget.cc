/* mvme - Mesytec VME Data Acquisition
 *
 * Copyright (C) 2016-2020 mesytec GmbH & Co. KG <info@mesytec.com>
 *
 * Author: Florian Lüke <f.lueke@mesytec.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#include "histo1d_widget.h"
#include "histo1d_widget_p.h"

#include <qwt_painter.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_histogram.h>
#include <qwt_plot_magnifier.h>
#include <qwt_plot_marker.h>
#include <qwt_plot_panner.h>
#include <qwt_plot_renderer.h>
#include <qwt_plot_textlabel.h>
#include <qwt_plot_zoneitem.h>
#include <qwt_point_data.h>
#include <qwt_scale_engine.h>
#include <qwt_scale_widget.h>
#include <qwt_text.h>

#include <qwt_widget_overlay.h>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>

#include "analysis/analysis.h"
#include "histo1d_util.h"
#include "histo_gui_util.h"
#include "mvme_context.h"
#include "mvme_context_lib.h"
#include "mvme_qwt.h"
#include "scrollzoomer.h"
#include "util.h"
#include "util/qt_monospace_textedit.h"

#include "git_sha1.h"

// TODO Mon Nov 26 2018
// 1) Combine sink, calibration, context, etc. Having those set on the widget
//    only makes sense in combination. If we have a sink but no context, we can't
//    get to the analysis and thus cannot react to changes.
//
// 2) Find a better way to split up the widget. Isolate core parts that do not
//    need anything from the analysis and only do basic qwt rendering, axis
//    scaling, etc.
// 3) Build a layer using the analysis related parts.

/*
Rate Estimation

Picker with action on point selection

data:
  isvisible, x1, x2
  markers for x1, x2
  plot curve with corresponding curve data (QwtPlotCurve and own subclass of QwtSyntheticPointData)

logic:
  swap x positions to have x1 < x2
  decision on number of points picked (nan is used as a special value)


Gauss
  toggle on/off, no picker involved
  plot curve and data
  The curve data needs access to the most recently calculated histo stats
  */

static const s32 ReplotPeriod_ms = 1000;
static const u32 RRFMin = 2;

class Histo1DIntervalData: public QwtSeriesData<QwtIntervalSample>
{
    public:
        explicit Histo1DIntervalData(Histo1D *histo)
            : QwtSeriesData<QwtIntervalSample>()
            , m_histo(histo)
        {
            assert(histo);
        }

        virtual size_t size() const override
        {
            return m_histo->getNumberOfBins(m_rrf);
        }

        virtual QwtIntervalSample sample(size_t i) const override
        {
            auto result = QwtIntervalSample(
                m_histo->getBinContent(i, m_rrf),
                m_histo->getBinLowEdge(i, m_rrf),
                m_histo->getBinLowEdge(i+1, m_rrf));

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
                m_histo->getWidth(), m_histo->getMaxValue(m_rrf));

            return result;
        }

        void setResolutionReductionFactor(u32 rrf) { m_rrf = rrf; }
        u32 getResolutionReductionFactor() const { return m_rrf; }

    private:
        Histo1D *m_histo;
        u32 m_rrf = Histo1D::NoRR;
};

/* Calculates a gauss fit using the currently visible maximum histogram value.
 *
 * Note: The resolution is independent of the underlying histograms resolution.
 * Instead NumberOfPoints samples are used at all zoom levels.
 */
static const double FWHMSigmaFactor = 2.3548;

static inline double squared(double x)
{
    return x * x;
}

class Histo1DGaussCurveData: public QwtSyntheticPointData
{
    static const size_t NumberOfPoints = 500;

    public:
        Histo1DGaussCurveData()
            : QwtSyntheticPointData(NumberOfPoints)
        {
        }

        virtual double y(double x) const override
        {
            double s = m_stats.fwhm / FWHMSigmaFactor;
            // Instead of using the center of the max bin the center point
            // between the fwhm edges is used. This makes the curve remain in a
            // much more stable x-position.
            double a = m_stats.fwhmCenter;

            // This is (1.0 / (SqrtPI2 * s)) if the resulting area should be 1.
            double firstTerm  = m_stats.maxValue;
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
        Histo1DStatistics m_stats;
};

struct CalibUi
{
    QDoubleSpinBox *actual1, *actual2,
                   *target1, *target2,
                   *lastFocusedActual;
    QPushButton *applyButton,
                *fillMaxButton,
                *resetToFilterButton,
                *closeButton;

    QDialog *window;
};

struct RateEstimationData
{
    bool visible = false;
    double x1 = make_quiet_nan();
    double x2 = make_quiet_nan();
};

static const double E1 = std::exp(1.0);

class RateEstimationCurveData: public QwtSyntheticPointData
{
    static const size_t NumberOfPoints = 500;

    public:
        RateEstimationCurveData(Histo1D *histo, RateEstimationData *data)
            : QwtSyntheticPointData(NumberOfPoints)
            , m_histo(histo)
            , m_data(data)
        {}

        virtual double y(double x) const override
        {
            auto xy1 = m_histo->getValueAndBinLowEdge(m_data->x1, m_rrf);
            auto xy2 = m_histo->getValueAndBinLowEdge(m_data->x2, m_rrf);

            double x1 = xy1.first;
            double y1 = xy1.second;
            double x2 = xy2.first;
            double y2 = xy2.second;

            double tau = (x2 - x1) / log(y1 / y2);

#if 0
            double norm = y1 / (( 1.0 / tau) * pow(E1, -x1 / tau));

            qDebug() << __PRETTY_FUNCTION__
                << "graphical norm =" << norm
                << "bin / units adjusted norm"
                << norm * m_histo->getAxisBinning(Qt::XAxis).getBinsToUnitsRatio()
                ;
#endif

            double result = y1 * (pow(E1, -x/tau) / pow(E1, -x1/tau));

            return result;
        }

        void setResolutionReductionFactor(u32 rrf) { m_rrf = rrf; }
        u32 getResolutionReductionFactor() const { return m_rrf; }

    private:
        Histo1D *m_histo;
        RateEstimationData *m_data;
        u32 m_rrf = Histo1D::NoRR;
};

static const double PlotTextLayerZ  = 1000.0;
static const double PlotAdditionalCurvesLayerZ = 1010.0;

struct Histo1DWidgetPrivate
{
    Histo1DWidget *m_q;

    QToolBar *m_toolBar;
    QwtPlot *m_plot;
    QStatusBar *m_statusBar;

    QLabel *m_labelCursorInfo;
    QLabel *m_labelHistoInfo;
    QWidget *m_infoContainer;

    s32 m_labelCursorInfoMaxWidth  = 0;
    s32 m_labelCursorInfoMaxHeight = 0;

    QAction *m_actionRateEstimation,
            *m_actionSubRange,
            *m_actionChangeRes,
            *m_actionGaussFit,
            *m_actionCalibUi,
            *m_actionInfo,
            *m_actionHistoListStats
            ;
            //*m_actionCuts;

    // rate estimation
    RateEstimationData m_rateEstimationData;
    QwtPlotPicker *m_rateEstimationPointPicker;
    QwtPlotMarker *m_rateEstimationX1Marker;
    QwtPlotMarker *m_rateEstimationX2Marker;
    QwtPlotMarker *m_rateEstimationFormulaMarker;
    RateEstimationCurveData *m_plotRateEstimationData = nullptr; // owned by qwt

    // active picker
    QwtPlotPicker *m_activePlotPicker;

    void activatePlotPicker(QwtPlotPicker *picker);
    QwtPlotPicker *getActivePlotPicker() const;

    // text items / stats
    mvme_qwt::TextLabelItem *m_globalStatsTextItem;
    mvme_qwt::TextLabelItem *m_gaussStatsTextItem;
    mvme_qwt::TextLabelRowLayout m_textLabelLayout;
    std::unique_ptr<QwtText> m_globalStatsText;
    std::unique_ptr<QwtText> m_gaussStatsText;

    Histo1DIntervalData *m_plotHistoData = nullptr; // owned by qwt
    QwtPlotCurve *m_gaussCurve = nullptr;
    QwtPlotCurve *m_rateEstimationCurve = nullptr;

    QwtText m_waterMarkText;
    QwtPlotTextLabel *m_waterMarkLabel;

    QComboBox *m_yScaleCombo;
    QSlider *m_rrSlider;
    QLabel *m_rrLabel;
    u32 m_rrf = Histo1D::NoRR;

    CalibUi m_calibUi;

    // Cut creation / editing
    IntervalEditor *m_intervalCutEditor;
    analysis::ConditionLink m_editingCondition;
    QVector<QwtInterval> m_cutIntervals;

    // Histo and stats
    Histo1DStatistics m_stats;

    Histo1DWidget::HistoList m_histos;
    s32 m_histoIndex = 0;

    QSpinBox *m_histoSpin;

    QwtPlotHistogram *m_plotHisto;

    ScrollZoomer *m_zoomer;
    QTimer *m_replotTimer;
    QPointF m_cursorPosition;

    std::shared_ptr<analysis::CalibrationMinMax> m_calib;
    MVMEContext *m_context = nullptr;

    Histo1DWidget::SinkPtr m_sink;
    Histo1DWidget::HistoSinkCallback m_sinkModifiedCallback;

    void setCalibUiVisible(bool b)
    {
        auto window = m_calibUi.window;
        window->setVisible(b);

        s32 x = m_q->width() - window->width();
        s32 y = m_toolBar->height() + 15;
        m_calibUi.window->move(m_q->mapToGlobal(QPoint(x, y)));
    }

    void onActionChangeResolution()
    {
        auto combo_xBins = make_resolution_combo(Histo1DMinBits, Histo1DMaxBits, Histo1DDefBits);
        select_by_resolution(combo_xBins, m_sink->m_bins);

        QDialog dialog(m_q);
        auto buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        QObject::connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

        auto layout = new QFormLayout(&dialog);
        layout->setContentsMargins(2, 2, 2, 2);
        layout->addRow(QSL("X Resolution"), combo_xBins);
        layout->addRow(buttonBox);

        if (dialog.exec() == QDialog::Accepted)
        {
            if (m_sink->m_bins != combo_xBins->currentData().toInt())
            {
                AnalysisPauser pauser(m_context);
                m_sink->m_bins = combo_xBins->currentData().toInt();
                m_sink->setResolutionReductionFactor(Histo1D::NoRR);
                m_context->analysisOperatorEdited(m_sink);

                qDebug() << __PRETTY_FUNCTION__ << "setting rrSlider to max";
                m_rrSlider->setMaximum(std::log2(getCurrentHisto()->getNumberOfBins()));
                m_rrSlider->setValue(m_rrSlider->maximum());
            }
        }
    }

    void displayChanged();
    void updateStatistics(u32 rrf);
    void updateAxisScales();
    bool yAxisIsLog();
    bool yAxisIsLin();
    void updateCursorInfoLabel(u32 rrf);
    void calibApply();
    void calibFillMax();
    void calibResetToFilter();
    void onRRSliderValueChanged(int sliderValue);
    void exportPlot();
    void exportPlotToClipboard();
    void saveHistogram();
    void onActionHistoListStats();

    void onActionNewCutTriggered();
    void onCutIntervalSelected();
    void onEditCutAccept();
    void onEditCutReject();

    IntervalEditor *getIntervalCutEditor() const
    {
        return m_intervalCutEditor;
    }

    void onCutEditorIntervalCreated(const QwtInterval &interval);
    void onCutEditorIntervalModified(const QwtInterval &interval);

    Histo1DPtr getCurrentHisto()
    {
        return m_histos.value(m_histoIndex, {});
    }

    ~Histo1DWidgetPrivate()
    {
        qDebug() << __PRETTY_FUNCTION__ << this;
    }
};

enum class AxisScaleType
{
    Linear,
    Logarithmic
};

Histo1DWidget::Histo1DWidget(const Histo1DPtr &histo, QWidget *parent)
    : Histo1DWidget(HistoList{ histo }, parent)
{
}

Histo1DWidget::Histo1DWidget(const HistoList &histos, QWidget *parent)
    : QWidget(parent)
    , m_d(std::make_unique<Histo1DWidgetPrivate>())
{
    assert(!histos.isEmpty());

    m_d->m_q = this;
    m_d->m_histos = histos;
    m_d->m_plotHisto = new QwtPlotHistogram;
    m_d->m_replotTimer = new QTimer(this);
    m_d->m_cursorPosition = { make_quiet_nan(), make_quiet_nan() };
    m_d->m_plot = new QwtPlot;
    m_d->m_intervalCutEditor = new IntervalEditor(this);
    m_d->m_histoSpin = new QSpinBox(this);
    m_d->m_histoSpin->setMaximum(histos.size() - 1);
    set_widget_font_pointsize_relative(m_d->m_histoSpin, -2);

    // Toolbar and actions
    m_d->m_toolBar = new QToolBar();
    auto tb = m_d->m_toolBar;
    {
        tb->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        tb->setIconSize(QSize(16, 16));
        set_widget_font_pointsize(tb, 7);
    }

    QAction *action = nullptr;

    // Y-Scale Selection
    {
        m_d->m_yScaleCombo = new QComboBox;
        auto yScaleCombo = m_d->m_yScaleCombo;

        yScaleCombo->addItem(QSL("Lin"), static_cast<int>(AxisScaleType::Linear));
        yScaleCombo->addItem(QSL("Log"), static_cast<int>(AxisScaleType::Logarithmic));

        connect(yScaleCombo, static_cast<void (QComboBox::*) (int)>(&QComboBox::currentIndexChanged),
                this, [this]() { m_d->displayChanged(); });

        tb->addWidget(make_vbox_container(QSL("Y-Scale"), yScaleCombo, 2, -2)
                      .container.release());
    }

    m_d->m_actionGaussFit = tb->addAction(QIcon(":/generic_chart_with_pencil.png"), QSL("Gauss"));
    m_d->m_actionGaussFit->setCheckable(true);
    connect(m_d->m_actionGaussFit, &QAction::toggled, this, &Histo1DWidget::on_tb_gauss_toggled);

    m_d->m_actionRateEstimation = tb->addAction(QIcon(":/generic_chart_with_pencil.png"),
                                                QSL("Rate Est."));
    m_d->m_actionRateEstimation->setStatusTip(QSL("Rate Estimation"));
    m_d->m_actionRateEstimation->setCheckable(true);
    connect(m_d->m_actionRateEstimation, &QAction::toggled,
            this, &Histo1DWidget::on_tb_rate_toggled);

    tb->addAction(QIcon(":/clear_histos.png"), QSL("Clear"), this, [this]() {
        m_d->getCurrentHisto()->clear();
        replot();
    });

    // export plot to file / clipboard
    {
        auto menu = new QMenu(this);
        menu->addAction(QSL("to file"), this, [this] { m_d->exportPlot(); });
        menu->addAction(QSL("to clipboard"), this, [this] { m_d->exportPlotToClipboard(); });

        auto button = make_toolbutton(QSL(":/document-pdf.png"), QSL("Export"));
        button->setStatusTip(QSL("Export plot to file or clipboard"));
        button->setMenu(menu);
        button->setPopupMode(QToolButton::InstantPopup);

        tb->addWidget(button);
    }

    action = tb->addAction(QIcon(":/document-save.png"), QSL("Save"),
                           this, [this] { m_d->saveHistogram(); });
    action->setStatusTip(QSL("Save the histogram to a text file"));

    m_d->m_actionSubRange = tb->addAction(QIcon(":/histo_subrange.png"), QSL("Subrange"),
                                          this, &Histo1DWidget::on_tb_subRange_clicked);
    m_d->m_actionSubRange->setStatusTip(QSL("Limit the histogram to a specific X-Axis range"));
    m_d->m_actionSubRange->setEnabled(false);

    m_d->m_actionChangeRes = tb->addAction(QIcon(":/histo_resolution.png"), QSL("Resolution"),
                                           this, [this]() { m_d->onActionChangeResolution(); });
    m_d->m_actionChangeRes->setStatusTip(QSL("Change histogram resolution"));
    m_d->m_actionChangeRes->setEnabled(false);

    m_d->m_actionCalibUi = tb->addAction(QIcon(":/operator_calibration.png"), QSL("Calibration"));
    m_d->m_actionCalibUi->setCheckable(true);
    m_d->m_actionCalibUi->setVisible(false);
    m_d->m_actionCalibUi->setStatusTip(QSL("Edit the histograms input calibration"));
    connect(m_d->m_actionCalibUi, &QAction::toggled,
            this, [this](bool b) { m_d->setCalibUiVisible(b); });

    m_d->m_actionInfo = tb->addAction(QIcon(":/info.png"), QSL("Info"));
    m_d->m_actionInfo->setCheckable(true);

    connect(m_d->m_actionInfo, &QAction::toggled, this, [this](bool b) {
        /* I did not manage to get the statusbar to resize to the smallest
         * possible size after hiding m_infoContainer. I tried
         * - setSizeConstraint(QLayout::SetFixedSize) on the container layout and on the statusbar layout
         * - calling adjustSize() on container and statusbar
         * but neither did resize the statusbar once the container was hidden.
         *
         * The quick fix is to hide/show the containers children explicitly.
         */
        for (auto childWidget: m_d->m_infoContainer->findChildren<QWidget *>())
        {
            childWidget->setVisible(b);
        }
    });

    // Resolution Reduction
    {
        m_d->m_rrSlider = make_res_reduction_slider();
        set_widget_font_pointsize_relative(m_d->m_rrSlider, -2);
        //m_d->m_rrSlider->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Maximum);
        auto boxStruct = make_vbox_container(QSL("Res"), m_d->m_rrSlider, 0, -2);
        m_d->m_rrLabel = boxStruct.label;
        set_widget_font_pointsize_relative(m_d->m_rrLabel, -2);
        tb->addWidget(boxStruct.container.release());

        connect(m_d->m_rrSlider, &QSlider::valueChanged, this, [this] (int sliderValue) {
            m_d->onRRSliderValueChanged(sliderValue);
        });
    }

#if 0
    //
    // Cuts
    //
    {
        tb->addSeparator();
        m_d->m_actionCuts = tb->addAction(QIcon(QSL(":/scissors.png")), QSL("Cuts"));
        m_d->m_actionCuts->setEnabled(false);

        connect(m_d->m_actionCuts, &QAction::triggered,
                this, [this] () {

            auto cutDialog = new IntervalCutDialog(this);

            connect(cutDialog, &QDialog::accepted,
                    m_d->m_actionCuts, [this] () {
                        m_d->m_actionCuts->setEnabled(true);
                        replot();
                    });

            connect(cutDialog, &QDialog::rejected,
                    m_d->m_actionCuts, [this] () {
                        m_d->m_actionCuts->setEnabled(true);
                        replot();
                    });

            cutDialog->setAttribute(Qt::WA_DeleteOnClose);
            cutDialog->show();
            // Enabled in setSink()
            m_d->m_actionCuts->setEnabled(false);
        });
    }
#endif

    m_d->m_actionHistoListStats = tb->addAction(
        QIcon(QSL(":/document-text.png")),
        QSL("Print Stats"));

    connect(m_d->m_actionHistoListStats, &QAction::triggered,
            this, [this] () { m_d->onActionHistoListStats(); });

    // Final, right-side spacer. The listwidget adds the histo selection spinbox after
    // this.
    tb->addWidget(make_spacer_widget());

    // Setup the plot

    m_d->m_plotHisto->setStyle(QwtPlotHistogram::Outline);
    m_d->m_plotHisto->attach(m_d->m_plot);

    m_d->m_plot->axisWidget(QwtPlot::yLeft)->setTitle("Counts");

    connect(m_d->m_replotTimer, SIGNAL(timeout()), this, SLOT(replot()));
    m_d->m_replotTimer->start(ReplotPeriod_ms);

    m_d->m_plot->canvas()->setMouseTracking(true);

    m_d->m_zoomer = new ScrollZoomer(m_d->m_plot->canvas());

    m_d->m_zoomer->setVScrollBarMode(Qt::ScrollBarAlwaysOff);
    m_d->m_zoomer->setZoomBase();

    TRY_ASSERT(connect(m_d->m_zoomer, SIGNAL(zoomed(const QRectF &)),
                       this, SLOT(zoomerZoomed(const QRectF &))));
    TRY_ASSERT(connect(m_d->m_zoomer, &ScrollZoomer::mouseCursorMovedTo,
                       this, &Histo1DWidget::mouseCursorMovedToPlotCoord));
    TRY_ASSERT(connect(m_d->m_zoomer, &ScrollZoomer::mouseCursorLeftPlot,
                       this, &Histo1DWidget::mouseCursorLeftPlot));

    TRY_ASSERT(connect(m_d->getCurrentHisto().get(), &Histo1D::axisBinningChanged,
                       this, [this] (Qt::Axis) {
        // Handle axis changes by zooming out fully. This will make sure
        // possible axis scale changes are immediately visible and the zoomer
        // is in a clean state.
        m_d->m_zoomer->setZoomStack(QStack<QRectF>(), -1);
        m_d->m_zoomer->zoom(0);
        replot();
    }));

    //
    // Global stats box
    //
    m_d->m_globalStatsText = make_qwt_text_box(Qt::AlignTop | Qt::AlignRight);
    m_d->m_globalStatsTextItem = new mvme_qwt::TextLabelItem();
    /* Margin added to contentsMargins() of the canvas. This is (mis)used to
     * not clip the top scrollbar. */
    //m_d->m_globalStatsTextItem->setMargin(25);

    //
    // Gauss stats box
    //
    m_d->m_gaussStatsText = make_qwt_text_box(Qt::AlignTop | Qt::AlignRight);

    m_d->m_gaussStatsTextItem = new mvme_qwt::TextLabelItem();
    m_d->m_gaussStatsTextItem->setVisible(false);

    m_d->m_textLabelLayout.addTextLabel(m_d->m_globalStatsTextItem);
    m_d->m_textLabelLayout.addTextLabel(m_d->m_gaussStatsTextItem);
    m_d->m_textLabelLayout.attachAll(m_d->m_plot);

    //
    // Calib Ui
    //
    m_d->m_calibUi.actual1 = new QDoubleSpinBox;
    m_d->m_calibUi.actual2 = new QDoubleSpinBox;
    m_d->m_calibUi.target1 = new QDoubleSpinBox;
    m_d->m_calibUi.target2 = new QDoubleSpinBox;
    m_d->m_calibUi.applyButton = new QPushButton(QSL("Apply"));
    m_d->m_calibUi.fillMaxButton = new QPushButton(QSL("Vis. Max"));
    m_d->m_calibUi.fillMaxButton->setToolTip(QSL("Fill the last focused actual value with the visible maximum histogram value"));

    /* FIXME: disabled due to Calibration not storing "global values" anymore
     * and thus resetting will reset to the input parameters (lowerLimit,
     * upperLimit[ interval. */
#if 0
    m_d->m_calibUi.resetToFilterButton = new QPushButton(QSL("Restore"));
    m_d->m_calibUi.resetToFilterButton->setToolTip(QSL("Restore base unit values from source calibration"));
#endif
    m_d->m_calibUi.closeButton = new QPushButton(QSL("Close"));

    m_d->m_calibUi.lastFocusedActual = m_d->m_calibUi.actual2;
    m_d->m_calibUi.actual1->installEventFilter(this);
    m_d->m_calibUi.actual2->installEventFilter(this);

    connect(m_d->m_calibUi.applyButton, &QPushButton::clicked,
            this, [this] { m_d->calibApply(); });
    connect(m_d->m_calibUi.fillMaxButton, &QPushButton::clicked,
            this, [this] { m_d->calibFillMax(); });

    //connect(m_d->m_calibUi.resetToFilterButton, &QPushButton::clicked, this, &Histo1DWidget::calibResetToFilter);
    connect(m_d->m_calibUi.closeButton, &QPushButton::clicked,
            this, [this]() { m_d->m_calibUi.window->reject(); });

    QVector<QDoubleSpinBox *> spins = {
        m_d->m_calibUi.actual1, m_d->m_calibUi.actual2,
        m_d->m_calibUi.target1, m_d->m_calibUi.target2
    };

    for (auto spin: spins)
    {
        spin->setDecimals(4);
        spin->setSingleStep(0.0001);
        spin->setMinimum(std::numeric_limits<double>::lowest());
        spin->setMaximum(std::numeric_limits<double>::max());
        spin->setValue(0.0);
    }

    m_d->m_calibUi.window = new QDialog(this);
    m_d->m_calibUi.window->setWindowTitle(QSL("Adjust Calibration"));
    connect(m_d->m_calibUi.window, &QDialog::rejected, this, [this]() {
        m_d->m_actionCalibUi->setChecked(false);
    });

    {
        auto font = m_d->m_calibUi.window->font();
        font.setPointSize(7);
        m_d->m_calibUi.window->setFont(font);
    }

    auto calibLayout = new QGridLayout(m_d->m_calibUi.window);
    calibLayout->setContentsMargins(3, 3, 3, 3);
    calibLayout->setSpacing(2);

    calibLayout->addWidget(new QLabel(QSL("Actual")), 0, 0, Qt::AlignHCenter);
    calibLayout->addWidget(new QLabel(QSL("Target")), 0, 1, Qt::AlignHCenter);

    calibLayout->addWidget(m_d->m_calibUi.actual1, 1, 0);
    calibLayout->addWidget(m_d->m_calibUi.target1, 1, 1);

    calibLayout->addWidget(m_d->m_calibUi.actual2, 2, 0);
    calibLayout->addWidget(m_d->m_calibUi.target2, 2, 1);

    calibLayout->addWidget(m_d->m_calibUi.fillMaxButton, 3, 0, 1, 1);
    calibLayout->addWidget(m_d->m_calibUi.applyButton, 3, 1, 1, 1);

    //calibLayout->addWidget(m_d->m_calibUi.resetToFilterButton, 4, 0, 1, 1);
    calibLayout->addWidget(m_d->m_calibUi.closeButton, 4, 1, 1, 1);

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

    m_d->m_rateEstimationX1Marker = make_position_marker(m_d->m_plot);
    m_d->m_rateEstimationX2Marker = make_position_marker(m_d->m_plot);

    m_d->m_rateEstimationFormulaMarker = new QwtPlotMarker;
    m_d->m_rateEstimationFormulaMarker->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
    m_d->m_rateEstimationFormulaMarker->setZ(PlotTextLayerZ);
    m_d->m_rateEstimationFormulaMarker->attach(m_d->m_plot);
    m_d->m_rateEstimationFormulaMarker->hide();

    m_d->m_rateEstimationPointPicker = new QwtPlotPicker(QwtPlot::xBottom, QwtPlot::yLeft,
                                               QwtPicker::VLineRubberBand, QwtPicker::ActiveOnly,
                                               m_d->m_plot->canvas());
    QPen pickerPen(Qt::red);
    m_d->m_rateEstimationPointPicker->setTrackerPen(pickerPen);
    m_d->m_rateEstimationPointPicker->setRubberBandPen(pickerPen);
    m_d->m_rateEstimationPointPicker->setStateMachine(new AutoBeginClickPointMachine);
    m_d->m_rateEstimationPointPicker->setEnabled(false);

    TRY_ASSERT(connect(m_d->m_rateEstimationPointPicker, SIGNAL(selected(const QPointF &)),
                       this, SLOT(on_ratePointerPicker_selected(const QPointF &))));

    auto make_plot_curve = [](QColor penColor, double penWidth,
                              double zLayer, QwtPlot *plot = nullptr, bool hide = true)
    {
        auto result = new QwtPlotCurve;

        result->setZ(zLayer);
        result->setPen(penColor, penWidth);
        result->setRenderHint(QwtPlotItem::RenderAntialiased, true);

        if (plot)
            result->attach(plot);

        if (hide)
            result->hide();

        return result;
    };

    m_d->m_rateEstimationCurve = make_plot_curve(Qt::red, 2.0, PlotAdditionalCurvesLayerZ, m_d->m_plot);

    //
    // Gauss Curve
    //
    m_d->m_gaussCurve = make_plot_curve(Qt::green, 2.0, PlotAdditionalCurvesLayerZ, m_d->m_plot);

    //
    // Watermark text when exporting
    //
    {
        m_d->m_waterMarkText = QwtText();
        m_d->m_waterMarkText.setRenderFlags(Qt::AlignRight | Qt::AlignBottom);
        m_d->m_waterMarkText.setColor(QColor(0x66, 0x66, 0x66, 0x40));

        QFont font;
        font.setPixelSize(16);
        font.setBold(true);
        m_d->m_waterMarkText.setFont(font);

        m_d->m_waterMarkText.setText(QString("mvme-%1").arg(GIT_VERSION_TAG));

        m_d->m_waterMarkLabel = new QwtPlotTextLabel;
        m_d->m_waterMarkLabel->setMargin(10);
        m_d->m_waterMarkLabel->setText(m_d->m_waterMarkText);
        m_d->m_waterMarkLabel->attach(m_d->m_plot);
        m_d->m_waterMarkLabel->hide();
    }

    //
    // Histo selection spinbox
    //
    connect(m_d->m_histoSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &Histo1DWidget::onHistoSpinBoxValueChanged);

    m_d->m_toolBar->addWidget(make_vbox_container(QSL("Histogram #"),
                                                  m_d->m_histoSpin, 2, -2)
                      .container.release());

    //
    // StatusBar and info widgets
    //
    m_d->m_statusBar = make_statusbar();

    m_d->m_labelCursorInfo = new QLabel;
    m_d->m_labelHistoInfo = new QLabel;

    for (auto label: { m_d->m_labelCursorInfo, m_d->m_labelHistoInfo})
    {
        label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        set_widget_font_pointsize(label, 7);
    }

    {
        m_d->m_infoContainer = new QWidget;

        auto layout = new QHBoxLayout(m_d->m_infoContainer);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(1);
        layout->addWidget(m_d->m_labelCursorInfo);
        layout->addWidget(m_d->m_labelHistoInfo);

        for (auto childWidget: m_d->m_infoContainer->findChildren<QWidget *>())
        {
            childWidget->setVisible(false);
        }

        m_d->m_statusBar->addPermanentWidget(m_d->m_infoContainer);
    }

    // Main Widget Layout

    auto toolBarFrame = new QFrame;
    toolBarFrame->setFrameStyle(QFrame::StyledPanel);
    {
        auto layout = new QHBoxLayout(toolBarFrame);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(m_d->m_toolBar);
    }

    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(2, 2, 2, 2);
    mainLayout->setSpacing(2);
    mainLayout->addWidget(toolBarFrame);
    mainLayout->addWidget(m_d->m_plot);
    mainLayout->addWidget(m_d->m_statusBar);
    mainLayout->setStretch(1, 1);

    resize(600, 400);
    selectHistogram(0);
}

Histo1DWidget::~Histo1DWidget()
{
    delete m_d->m_plotHisto;
}

void Histo1DWidget::setHistogram(const Histo1DPtr &histo)
{
    m_d->m_histos = { histo };
    m_d->m_calib = {};
    m_d->m_sink = {};
    m_d->m_sinkModifiedCallback = {};
    m_d->m_actionSubRange->setEnabled(false);
    m_d->m_actionChangeRes->setEnabled(false);
    m_d->m_actionCalibUi->setVisible(false);
    selectHistogram(0);
}

void Histo1DWidgetPrivate::updateAxisScales()
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
    m_plot->setAxisScale(QwtPlot::yLeft, base, maxValue);

    // xAxis
    if (m_zoomer->zoomRectIndex() == 0)
    {
        // fully zoomed out -> set to full resolution
        m_plot->setAxisScale(QwtPlot::xBottom, getCurrentHisto()->getXMin(),
                             getCurrentHisto()->getXMax());
        m_zoomer->setZoomBase();
    }

    m_plot->updateAxes();
}

void Histo1DWidget::replot()
{
    // ResolutionReduction
    const u32 rrf = m_d->m_rrf;

    m_d->updateStatistics(rrf);
    m_d->updateAxisScales();
    m_d->updateCursorInfoLabel(rrf);

    m_d->m_plotHistoData->setResolutionReductionFactor(rrf);
    m_d->m_plotRateEstimationData->setResolutionReductionFactor(rrf);

    auto xBinning = m_d->getCurrentHisto()->getAxisBinning(Qt::XAxis);

    // update histo info label
    auto infoText = QString("Underflow:  %1\n"
                            "Overflow:   %2\n"
                            "VisBins:    %3\n"
                            "BinWidth:   %4\n"
                            "Bins/Units: %5\n"
                            "PhysBins:   %6\n"
                            )
        .arg(m_d->getCurrentHisto()->getUnderflow())
        .arg(m_d->getCurrentHisto()->getOverflow())
        .arg(xBinning.getBins(rrf))
        .arg(xBinning.getBinWidth(rrf))
        .arg(xBinning.getBinsToUnitsRatio(rrf))
        .arg(xBinning.getBins());
        ;

    m_d->m_labelHistoInfo->setText(infoText);

    // rate and efficiency estimation
    if (m_d->m_rateEstimationData.visible)
    {
        /* This code tries to interpolate the exponential function formed by
         * the two selected data points. */

        auto xy1  = m_d->getCurrentHisto()->getValueAndBinLowEdge(m_d->m_rateEstimationData.x1, rrf);
        auto xy2  = m_d->getCurrentHisto()->getValueAndBinLowEdge(m_d->m_rateEstimationData.x2, rrf);

        double x1 = xy1.first;
        double y1 = xy1.second;
        double x2 = xy2.first;
        double y2 = xy2.second;

        double tau      = (x2 - x1) / log(y1 / y2);
        double freeRate = 1.0 / tau; // 1/x-axis unit

        double nom      = m_d->getCurrentHisto()->calcStatistics(x1, x2, rrf).entryCount;
        double denom    = ( (pow(E1, -x1/tau) - pow(E1, -x2/tau)));
        double factor   = (1.0 - pow(E1, -x1/tau));

        double norm                 = nom / denom;
        double freeCounts_0_x1      = norm * factor;
        double histoCounts_0_x1     = m_d->getCurrentHisto()->calcStatistics(0.0, x1, rrf).entryCount;
        double freeCounts_x1_inf    = norm * pow(E1, -x1 / tau);
        double freeCounts_0_inf     = norm;
        double efficiency           = (histoCounts_0_x1 + freeCounts_x1_inf) / freeCounts_0_inf;

        // Same values as used in RateEstimationCurveData but printed here to avoid spamming
        double norm_fitCurve          = y1 / (( 1.0 / tau) * pow(E1, -x1 / tau));
        double norm_fitCurve_adjusted =
            norm_fitCurve * xBinning.getBinsToUnitsRatio(rrf);

#if 0
        qDebug() << __PRETTY_FUNCTION__ << endl
            << "run =" << m_d->m_context->getRunInfo().runId << endl
            << "  x1,y1 =" << x1 << y1 << endl
            << "  x2,y2 =" << x2 << y2 << endl
            << "  tau   =" << tau << endl
            << "  freeRate (1/tau)   =" << freeRate << endl
            << "  nom (counts x1_x2) =" << nom << endl
            << "  denom  =" << denom << endl
            << "  factor =" << factor << endl
            << "  norm = freeCounts_0_inf =" << norm << endl
            << "  freeCounts_0_x1   =" << freeCounts_0_x1 << endl
            << "  histoCounts_0_x1  =" << histoCounts_0_x1 << endl
            << "  histoCounts_total ="
            << m_d->getCurrentHisto()->calcStatistics(m_d->getCurrentHisto()->getXMin(),
                                            m_d->getCurrentHisto()->getXMax(),
                                            rrf).entryCount
            << endl
            << "  efficiency        =" << efficiency << endl
            << endl
            << "  norm_fitCurve          =" << norm_fitCurve << endl
            << "  norm_fitCurve_adjusted =" << norm_fitCurve_adjusted
            ;
#else
        (void) freeCounts_0_x1;
        (void) norm_fitCurve_adjusted;
#endif

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

        if (!std::isnan(tau) && !std::isnan(efficiency))
        {
            auto unitX = m_d->getCurrentHisto()->getAxisInfo(Qt::XAxis).unit;
            if (unitX.isEmpty())
                unitX = QSL("x");

            markerText = QString(QSL("freeRate=%1 <sup>1</sup>&frasl;<sub>%2</sub>; eff=%3")
                                 .arg(freeRate, 0, 'g', 4)
                                 .arg(unitX)
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
        m_d->m_rateEstimationFormulaMarker->setXValue(x1);

        /* The goal is to draw the marker at 0.9 of the plot height. Doing this
         * in plot coordinates does work for a linear y-axis scale but
         * positions the text way too high for a logarithmic scale. Instead of
         * using plot coordinates directly we're using 0.9 of the canvas height
         * and transform that pixel coordinate to a plot coordinate.
         */
        s32 canvasHeight = m_d->m_plot->canvas()->height();
        s32 pixelY = canvasHeight - canvasHeight * 0.9;
        static const s32 minPixelY = 50;
        if (pixelY < minPixelY)
            pixelY = minPixelY;
        double plotY = m_d->m_plot->canvasMap(QwtPlot::yLeft).invTransform(pixelY);

        m_d->m_rateEstimationFormulaMarker->setYValue(plotY);
        m_d->m_rateEstimationFormulaMarker->setLabel(rateFormulaText);
        m_d->m_rateEstimationFormulaMarker->show();

        //qDebug() << __PRETTY_FUNCTION__ << "rate estimation formula marker x,y =" << x1 << plotY;
    }

    // window and axis titles
    auto name = m_d->getCurrentHisto()->objectName();
    setWindowTitle(QString("Histogram %1").arg(name));

    auto axisInfo = m_d->getCurrentHisto()->getAxisInfo(Qt::XAxis);
    m_d->m_plot->axisWidget(QwtPlot::xBottom)->setTitle(make_title_string(axisInfo));

    m_d->m_plot->replot();

#if 0
    // prints plot item pointers and their z value
    for (auto item: m_d->m_plot->itemList())
    {
        qDebug() << __PRETTY_FUNCTION__ << item << item->z();
    }
#endif
}

void Histo1DWidgetPrivate::displayChanged()
{
    auto scaleType = static_cast<AxisScaleType>(m_yScaleCombo->currentData().toInt());

    if (scaleType == AxisScaleType::Linear && !yAxisIsLin())
    {
        m_plot->setAxisScaleEngine(QwtPlot::yLeft, new QwtLinearScaleEngine);
        m_plot->setAxisAutoScale(QwtPlot::yLeft, true);
    }
    else if (scaleType == AxisScaleType::Logarithmic && !yAxisIsLog())
    {
        auto scaleEngine = new QwtLogScaleEngine;
        scaleEngine->setTransformation(new MinBoundLogTransform);
        m_plot->setAxisScaleEngine(QwtPlot::yLeft, scaleEngine);
    }

    m_q->replot();
}

void Histo1DWidget::zoomerZoomed(const QRectF &)
{
    if (m_d->m_zoomer->zoomRectIndex() == 0)
    {
        // fully zoomed out -> set to full resolution
        m_d->m_plot->setAxisScale(QwtPlot::xBottom,
                                  m_d->getCurrentHisto()->getXMin(), m_d->getCurrentHisto()->getXMax());
        m_d->m_plot->replot();
        m_d->m_zoomer->setZoomBase();
    }

    // do not zoom outside the histogram range
    auto scaleDiv = m_d->m_plot->axisScaleDiv(QwtPlot::xBottom);
    double lowerBound = scaleDiv.lowerBound();
    double upperBound = scaleDiv.upperBound();

    if (lowerBound <= upperBound)
    {
        if (lowerBound < m_d->getCurrentHisto()->getXMin())
        {
            scaleDiv.setLowerBound(m_d->getCurrentHisto()->getXMin());
        }

        if (upperBound > m_d->getCurrentHisto()->getXMax())
        {
            scaleDiv.setUpperBound(m_d->getCurrentHisto()->getXMax());
        }
    }
    else
    {
        if (lowerBound > m_d->getCurrentHisto()->getXMin())
        {
            scaleDiv.setLowerBound(m_d->getCurrentHisto()->getXMin());
        }

        if (upperBound < m_d->getCurrentHisto()->getXMax())
        {
            scaleDiv.setUpperBound(m_d->getCurrentHisto()->getXMax());
        }
    }

    m_d->m_plot->setAxisScaleDiv(QwtPlot::xBottom, scaleDiv);

    replot();
}

void Histo1DWidget::mouseCursorMovedToPlotCoord(QPointF pos)
{
    m_d->m_cursorPosition = pos;
    m_d->updateCursorInfoLabel(m_d->m_rrf);
}

void Histo1DWidget::mouseCursorLeftPlot()
{
    m_d->m_cursorPosition = QPointF(make_quiet_nan(), make_quiet_nan());
    m_d->updateCursorInfoLabel(m_d->m_rrf);
}

void Histo1DWidgetPrivate::updateStatistics(u32 rrf)
{
    double lowerBound = m_plot->axisScaleDiv(QwtPlot::xBottom).lowerBound();
    double upperBound = m_plot->axisScaleDiv(QwtPlot::xBottom).upperBound();

    //
    // global stats
    //
    m_stats = getCurrentHisto()->calcStatistics(lowerBound, upperBound, rrf);

    static const QString globalStatsTemplate = QSL(
        "<table>"
        "<tr><td align=\"left\">Mean   </td><td>%L1</td></tr>"
        "<tr><td align=\"left\">RMS    </td><td>%L2</td></tr>"
        "<tr><td align=\"left\">Max    </td><td>%L3</td></tr>"
        "<tr><td align=\"left\">Max Y  </td><td>%L4</td></tr>"
        "<tr><td align=\"left\">Counts </td><td>%L5</td></tr>"
        "</table>"
        );

    double maxBinCenter = ((m_stats.entryCount > 0)
                           ? getCurrentHisto()->getBinCenter(m_stats.maxBin, rrf)
                           : 0.0);

    static const int fieldWidth = 0;

    QString buffer = globalStatsTemplate
        .arg(m_stats.mean, fieldWidth)
        .arg(m_stats.sigma, fieldWidth)
        .arg(maxBinCenter, fieldWidth)
        .arg(m_stats.maxValue, fieldWidth)
        .arg(m_stats.entryCount, fieldWidth, 'f', 0)
        ;

    m_globalStatsText->setText(buffer, QwtText::RichText);
    m_globalStatsTextItem->setText(*m_globalStatsText);

    //
    // set updated histo stats on the gauss curve object
    //
    auto gaussCurveData = reinterpret_cast<Histo1DGaussCurveData *>(m_gaussCurve->data());
    gaussCurveData->setStats(m_stats);

    static const double Sqrt2Pi = std::sqrt(2 * M_PI);

    double a = m_stats.maxValue;
    double s = m_stats.fwhm / FWHMSigmaFactor;
    double scaleFactor = getCurrentHisto()->getAxisBinning(Qt::XAxis).getBinsToUnitsRatio(rrf);


    double thingsBelowGauss = a * s * Sqrt2Pi;
    // Scale with the max y value. This is the value of the maxBin
    thingsBelowGauss *= scaleFactor;

    static const QString gaussStatsTemplate = QSL(
        "<table>"
        "<tr><th>Gauss</th></tr>"
        "<tr><td align=\"left\">FWHM  </td><td>%L1</td></tr>"
        "<tr><td align=\"left\">Center</td><td>%L2</td></tr>"
        "<tr><td align=\"left\">Counts</td><td>%L3</td></tr>"
        "</table>"
        );

    buffer = gaussStatsTemplate
        .arg(m_stats.fwhm)
        .arg(m_stats.fwhmCenter)
        .arg(thingsBelowGauss)
        ;

    m_gaussStatsText->setText(buffer, QwtText::RichText);
    m_gaussStatsTextItem->setText(*m_gaussStatsText);
}

bool Histo1DWidgetPrivate::yAxisIsLog()
{
    return dynamic_cast<QwtLogScaleEngine *>(m_plot->axisScaleEngine(QwtPlot::yLeft));
}

bool Histo1DWidgetPrivate::yAxisIsLin()
{
    return dynamic_cast<QwtLinearScaleEngine *>(m_plot->axisScaleEngine(QwtPlot::yLeft));
}

void Histo1DWidgetPrivate::exportPlot()
{
    QString fileName = getCurrentHisto()->objectName();
    fileName.replace("/", "_");
    fileName.replace("\\", "_");
    fileName += QSL(".pdf");

    assert(m_context);

    if (m_context)
    {
        fileName = QDir(m_context->getWorkspacePath(QSL("PlotsDirectory"))).filePath(fileName);
    }

    m_plot->setTitle(getCurrentHisto()->getTitle());

    QString footerString = getCurrentHisto()->getFooter();
    QwtText footerText(footerString);
    footerText.setRenderFlags(Qt::AlignLeft);
    m_plot->setFooter(footerText);
    m_waterMarkLabel->show();

    QwtPlotRenderer renderer;
    renderer.setDiscardFlags(QwtPlotRenderer::DiscardBackground | QwtPlotRenderer::DiscardCanvasBackground);
    renderer.setLayoutFlag(QwtPlotRenderer::FrameWithScales);
    renderer.exportTo(m_plot, fileName);

    m_plot->setTitle(QString());
    m_plot->setFooter(QString());
    m_waterMarkLabel->hide();
}

void Histo1DWidgetPrivate::exportPlotToClipboard()
{
    m_plot->setTitle(getCurrentHisto()->getTitle());

    QString footerString = getCurrentHisto()->getFooter();
    QwtText footerText(footerString);
    footerText.setRenderFlags(Qt::AlignLeft);
    m_plot->setFooter(footerText);
    m_waterMarkLabel->show();

    QSize size(1024, 768);
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QwtPlotRenderer renderer;
#ifndef Q_OS_WIN
    // Enabling this leads to black pixels when pasting the image into windows
    // paint.
    renderer.setDiscardFlags(QwtPlotRenderer::DiscardBackground
                             | QwtPlotRenderer::DiscardCanvasBackground);
#endif
    renderer.setLayoutFlag(QwtPlotRenderer::FrameWithScales);
    renderer.renderTo(m_plot, image);

    m_plot->setTitle(QString());
    m_plot->setFooter(QString());
    m_waterMarkLabel->hide();

    auto clipboard = QApplication::clipboard();
    clipboard->clear();
    clipboard->setImage(image);
}

void Histo1DWidgetPrivate::saveHistogram()
{
    QString path = QSettings().value("Files/LastHistogramExportDirectory").toString();

    if (path.isEmpty())
    {
        path = QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation).at(0);
    }

    auto name = getCurrentHisto()->objectName();

    QString fileName = QString("%1/%2.txt")
        .arg(path)
        .arg(name);

    qDebug() << fileName;

    fileName = QFileDialog::getSaveFileName(
        m_q, "Save Histogram", fileName, "Text Files (*.histo1d);; All Files (*.*)");

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
    writeHisto1D(out, getCurrentHisto().get());

    if (out.status() == QTextStream::Ok)
    {
        fi.setFile(fileName);
        QSettings().setValue("Files/LastHistogramExportDirectory", fi.absolutePath());
    }
}

void Histo1DWidgetPrivate::updateCursorInfoLabel(u32 rrf)
{
    double plotX = m_cursorPosition.x();
    double plotY = m_cursorPosition.y();
    auto binning = getCurrentHisto()->getAxisBinning(Qt::XAxis);
    s64 binX = binning.getBin(plotX, rrf);

    QString text;

    if (!qIsNaN(plotX) && !qIsNaN(plotY) && binX >= 0)
    {
        double x = plotX;
        double y = getCurrentHisto()->getBinContent(binX, rrf);
        double binLowEdge = binning.getBinLowEdge((u32)binX, rrf);

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
            << QString("Y=%1").arg(getCurrentHisto()->getBinContent(binX))
            << QString("minX=%1, maxX=%2").arg(getCurrentHisto()->getXMin()).arg(getCurrentHisto()->getXMax());
        ;

        QString text = sl.join("\n");
#endif
    }

    // update the label which will calculate a new width
    m_labelCursorInfo->setText(text);

    // use the largest width and height the label ever had to stop the label from constantly changing its width
    if (m_labelCursorInfo->isVisible())
    {
        m_labelCursorInfoMaxWidth = std::max(m_labelCursorInfoMaxWidth,
                                             m_labelCursorInfo->width());
        m_labelCursorInfo->setMinimumWidth(m_labelCursorInfoMaxWidth);

        m_labelCursorInfo->setMinimumHeight(m_labelCursorInfoMaxHeight);
        m_labelCursorInfoMaxHeight = std::max(m_labelCursorInfoMaxHeight,
                                              m_labelCursorInfo->height());
    }
}

void Histo1DWidget::setContext(MVMEContext *context)
{
    m_d->m_context = context;
}

MVMEContext *Histo1DWidget::getContext() const
{
    return m_d->m_context;
}

void Histo1DWidgetPrivate::calibApply()
{
    Q_ASSERT(m_calib);
    Q_ASSERT(m_context);

    using namespace analysis;

    double a1 = m_calibUi.actual1->value();
    double a2 = m_calibUi.actual2->value();
    double t1 = m_calibUi.target1->value();
    double t2 = m_calibUi.target2->value();

    if (a1 - a2 == 0.0 || t1 == t2)
        return;

    double a = (t1 - t2) / (a1 - a2);
    double b = t1 - a * a1;

    u32 address = m_histoIndex;

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

    m_calibUi.actual1->setValue(m_calibUi.target1->value());
    m_calibUi.actual2->setValue(m_calibUi.target2->value());

    AnalysisPauser pauser(m_context);

    m_calib->setCalibration(address, targetMin, targetMax);
    m_context->getAnalysis()->setOperatorEdited(m_calib);

    m_q->on_tb_rate_toggled(m_rateEstimationData.visible);
}

void Histo1DWidgetPrivate::calibResetToFilter()
{
    Q_ASSERT(m_calib);
    Q_ASSERT(m_context);

    using namespace analysis;

    Pipe *inputPipe = m_calib->getSlot(0)->inputPipe;
    if (inputPipe)
    {
        Parameter *inputParam = inputPipe->getParameter(m_histoIndex);
        if (inputParam)
        {
            double minValue = inputParam->lowerLimit;
            double maxValue = inputParam->upperLimit;
            AnalysisPauser pauser(m_context);
            m_calib->setCalibration(m_histoIndex, minValue, maxValue);
            analysis::do_beginRun_forward(m_calib.get());
        }
    }
}

void Histo1DWidgetPrivate::calibFillMax()
{
    // The values in m_stats are calculated in terms of the current res
    // reduction factor in replot(). This means m_stats.maxBin can be used
    // unmodified in the call to getBinLowEdge().
    double maxAt = getCurrentHisto()->getBinLowEdge(m_stats.maxBin, m_rrf);
    m_calibUi.lastFocusedActual->setValue(maxAt);
}

bool Histo1DWidget::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == m_d->m_calibUi.actual1 || watched == m_d->m_calibUi.actual2)
        && (event->type() == QEvent::FocusIn))
    {
        m_d->m_calibUi.lastFocusedActual = qobject_cast<QDoubleSpinBox *>(watched);
    }
    return QWidget::eventFilter(watched, event);
}

bool Histo1DWidget::event(QEvent *e)
{
    if (e->type() == QEvent::StatusTip)
    {
        m_d->m_statusBar->showMessage(reinterpret_cast<QStatusTipEvent *>(e)->tip());
        return true;
    }

    return QWidget::event(e);
}

void Histo1DWidgetPrivate::onRRSliderValueChanged(int sliderValue)
{
#if 0
    qDebug() << __PRETTY_FUNCTION__
        << "rrS min/max =" << m_rrSlider->minimum() << m_rrSlider->maximum()
        << ", new rrS value =" << sliderValue
        << ", current rrf =" << m_rrf
        ;
#endif

    // (phys number of bins) / (res reduction number of bins)
    u32 physBins = getCurrentHisto()->getNumberOfBins();
    u32 visBins   = 1u << sliderValue;
    m_rrf = physBins / visBins;

    m_rrLabel->setText(QSL("Res: %1, %2 bit")
                       .arg(visBins)
                       .arg(std::log2(visBins))
                      );

#if 0
    qDebug() << __PRETTY_FUNCTION__
        << "physBins =" << physBins
        << ", visBins =" << visBins
        << ", new rrf =" << m_rrf;
#endif

    if (m_sink)
    {
        //qDebug() << "  updating sink" << m_sink.get();

        m_sink->setResolutionReductionFactor(m_rrf);

        // FIXME: this code results in the analysis being marked modified on opening a
        // histogram for the first time as the rrf changes from 0 (NoRR) to 1 (physBins ==
        // visBins)
        // Also invoking the callback here rebuilds the analysis immediately. This is not
        // needed at all as the res reduction is a display only thing and doesn't interact
        // with the a2 runtime in any way.
#if 0
        if (m_sinkModifiedCallback)
        {
            qDebug() << "  invoking sinkModifiedCallback";
            m_sinkModifiedCallback(m_sink);
        }
#endif
    }

    m_q->replot();

    //qDebug() << __PRETTY_FUNCTION__ << "<<<<< end of function";
}

void Histo1DWidget::setSink(const SinkPtr &sink, HistoSinkCallback sinkModifiedCallback)
{
    Q_ASSERT(sink);

    m_d->m_sink = sink;
    m_d->m_sinkModifiedCallback = sinkModifiedCallback;
    m_d->m_actionSubRange->setEnabled(true);
    m_d->m_actionChangeRes->setEnabled(true);
#if 0
    m_d->m_actionCuts->setEnabled(true);
#endif

    auto rrf = sink->getResolutionReductionFactor();

    if (rrf == Histo1D::NoRR)
    {
#if 0
        qDebug() << __PRETTY_FUNCTION__
            << "setting rrSlider value to max" << m_d->m_rrSlider->maximum();
#endif

        m_d->m_rrSlider->setValue(m_d->m_rrSlider->maximum());
    }
    else
    {
        u32 visBins = m_d->getCurrentHisto()->getNumberOfBins(rrf);
        int sliderValue = std::log2(visBins);

#if 0
        qDebug() << __PRETTY_FUNCTION__
            << "sink rrf =" << rrf
            << ", -> setting slider value to" << sliderValue;
#endif

        m_d->m_rrSlider->setValue(sliderValue);
    }
}

Histo1DWidget::SinkPtr Histo1DWidget::getSink() const
{
    return m_d->m_sink;
}

void Histo1DWidget::setResolutionReductionFactor(u32 rrf)
{
    if (rrf == 0)
        rrf = 1;
    u32 physBins = m_d->getCurrentHisto()->getNumberOfBins();
    u32 visBins  = physBins / rrf;
    int sliderValue = std::log2(visBins);
    m_d->m_rrSlider->setValue(sliderValue);
}

void Histo1DWidget::setResolutionReductionSliderEnabled(bool b)
{
    m_d->m_rrSlider->setEnabled(b);
}

void Histo1DWidget::on_tb_subRange_clicked()
{
    Q_ASSERT(m_d->m_sink);

    double visibleMinX = m_d->m_plot->axisScaleDiv(QwtPlot::xBottom).lowerBound();
    double visibleMaxX = m_d->m_plot->axisScaleDiv(QwtPlot::xBottom).upperBound();

    Histo1DSubRangeDialog dialog(m_d->m_sink, m_d->m_sinkModifiedCallback,
                                 visibleMinX, visibleMaxX, this);
    dialog.exec();
}

void Histo1DWidget::on_tb_rate_toggled(bool checked)
{
    if (checked)
    {
        m_d->m_rateEstimationData = RateEstimationData();
        m_d->activatePlotPicker(m_d->m_rateEstimationPointPicker);
    }
    else
    {

        m_d->m_rateEstimationData.visible = false;
        m_d->m_rateEstimationX1Marker->hide();
        m_d->m_rateEstimationX2Marker->hide();
        m_d->m_rateEstimationFormulaMarker->hide();
        m_d->m_rateEstimationCurve->hide();
        m_d->activatePlotPicker(m_d->m_zoomer);
    }

    replot();
}

void Histo1DWidget::on_tb_gauss_toggled(bool checked)
{
    if (checked)
    {
        m_d->m_gaussCurve->show();
        m_d->m_gaussStatsTextItem->setVisible(true);
    }
    else
    {
        m_d->m_gaussCurve->hide();
        m_d->m_gaussStatsTextItem->setVisible(false);
    }

    replot();
}

void Histo1DWidget::on_tb_test_clicked()
{
}

void Histo1DWidget::on_ratePointerPicker_selected(const QPointF &pos)
{
    if (std::isnan(m_d->m_rateEstimationData.x1))
    {
        m_d->m_rateEstimationData.x1 = pos.x();

        double x1 = m_d->getCurrentHisto()->getValueAndBinLowEdge(
            m_d->m_rateEstimationData.x1, m_d->m_rrf).first;

        m_d->m_rateEstimationX1Marker->setXValue(m_d->m_rateEstimationData.x1);
        m_d->m_rateEstimationX1Marker->setLabel(QString("    x1=%1").arg(x1));
        m_d->m_rateEstimationX1Marker->show();
    }
    else if (std::isnan(m_d->m_rateEstimationData.x2))
    {
        m_d->m_rateEstimationData.x2 = pos.x();

        if (m_d->m_rateEstimationData.x1 > m_d->m_rateEstimationData.x2)
        {
            std::swap(m_d->m_rateEstimationData.x1, m_d->m_rateEstimationData.x2);
        }

        m_d->m_rateEstimationData.visible = true;
        m_d->activatePlotPicker(m_d->m_zoomer);

        double x1 = m_d->getCurrentHisto()->getValueAndBinLowEdge(m_d->m_rateEstimationData.x1,
                                                                  m_d->m_rrf).first;

        double x2 = m_d->getCurrentHisto()->getValueAndBinLowEdge(m_d->m_rateEstimationData.x2,
                                                                  m_d->m_rrf).first;

        // set both x1 and x2 as they might have been swapped above
        m_d->m_rateEstimationX1Marker->setXValue(m_d->m_rateEstimationData.x1);
        m_d->m_rateEstimationX1Marker->setLabel(QString("    x1=%1").arg(x1));
        m_d->m_rateEstimationX2Marker->setXValue(m_d->m_rateEstimationData.x2);
        m_d->m_rateEstimationX2Marker->setLabel(QString("    x2=%1").arg(x2));
        m_d->m_rateEstimationX2Marker->show();
        m_d->m_rateEstimationCurve->show();
    }
    else
    {
        InvalidCodePath;
    }

    replot();
}

void Histo1DWidgetPrivate::onActionHistoListStats()
{
    if (m_histos.isEmpty() || !getCurrentHisto())
        return;

    double lowerBound = m_plot->axisScaleDiv(QwtPlot::xBottom).lowerBound();
    double upperBound = m_plot->axisScaleDiv(QwtPlot::xBottom).upperBound();

    QString title = m_sink ? m_sink->objectName() : getCurrentHisto()->objectName();

    QString buffer;
    QTextStream stream(&buffer);
    mvme::print_histolist_stats(
        stream, m_histos, lowerBound, upperBound, m_rrf, title);

    auto te = mesytec::mvme::util::make_monospace_plain_textedit().release();
    te->setWindowTitle(QSL("Stats for histogram array '%1'").arg(title));
    te->setAttribute(Qt::WA_DeleteOnClose);
    te->resize(1100, 600);
    te->setPlainText(buffer);
    te->show();
    te->raise();

    add_widget_close_action(te);
    auto geometrySaver = new WidgetGeometrySaver(te);
    geometrySaver->addAndRestore(te, QSL("WindowGeometries/HistoListStats"));
}

void Histo1DWidgetPrivate::onEditCutAccept()
{
    qDebug() << __PRETTY_FUNCTION__;
    m_intervalCutEditor->endEdit();
}

void Histo1DWidgetPrivate::onEditCutReject()
{
    qDebug() << __PRETTY_FUNCTION__;
    m_intervalCutEditor->endEdit();
}

using analysis::ConditionInterval;

bool Histo1DWidget::setEditCondition(const analysis::ConditionLink &cl)
{
    qDebug() << __PRETTY_FUNCTION__ << cl;

    auto cond = std::dynamic_pointer_cast<ConditionInterval>(cl.condition);

    if (!cond || cl.subIndex > m_d->m_histos.size())
    {
        // Error was either the Condition is not an interval cut or it's null.
        m_d->m_editingCondition = {};
        m_d->m_intervalCutEditor->endEdit();
        qDebug() << __PRETTY_FUNCTION__ << "condlink subindex out of range:"
            << "histoCount =" << m_d->m_histos.size();
        return false;
    }

    m_d->m_editingCondition = cl;
    // create a copy of the intervals. that's the core data we're editing
    m_d->m_cutIntervals = cond->getIntervals();

    selectHistogram(cl.subIndex);

    auto interval = m_d->m_cutIntervals.value(cl.subIndex);
    m_d->m_intervalCutEditor->setInterval(interval);
    m_d->m_intervalCutEditor->show();
    return true;
}

analysis::ConditionLink Histo1DWidget::getEditCondition() const
{
    return m_d->m_editingCondition;
}

void Histo1DWidget::beginEditCondition()
{
}

void Histo1DWidgetPrivate::onCutEditorIntervalCreated(const QwtInterval &interval)
{
    assert(m_context);
    assert(m_sink);

    // TODO: make unique name
    QString cutName = QSL("New Cut");

    // cut name dialog
    {
        auto le_cutName = new QLineEdit;
        le_cutName->setText(cutName);

        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

        QDialog dialog(m_q);
        auto layout = new QFormLayout(&dialog);
        layout->addRow("Cut Name", le_cutName);
        layout->addRow(buttons);

        QObject::connect(buttons, &QDialogButtonBox::accepted,
                         &dialog, &QDialog::accept);

        QObject::connect(buttons, &QDialogButtonBox::rejected,
                         &dialog, &QDialog::reject);

        if (dialog.exec() == QDialog::Rejected)
        {
            m_intervalCutEditor->hide();
            m_q->replot();
            return;
        }

        cutName = le_cutName->text();
    }

    // Create the ConditionInterval analysis object. The number of intervals will
    // be the same as the number of histograms in the Histo1DSink belonging to
    // the histogram currently being displayed. Each interval of the condition
    // will be set to the current intervals values.
    QVector<QwtInterval> intervals(m_sink->getNumberOfHistos(), interval);
    auto cond = std::make_shared<analysis::ConditionInterval>();
    cond->setIntervals(intervals);
    cond->setObjectName(cutName);

    {
        auto xInput = m_sink->getSlot(0)->inputPipe;
        auto xIndex = m_sink->getSlot(0)->paramIndex;

        AnalysisPauser pauser(m_context);
        cond->connectInputSlot(0, xInput, xIndex);

        // FIXME: remove this once conditions do not show up in the event trees anymore
        const int userLevel = 3;

        m_context->getAnalysis()->addOperator(
            m_sink->getEventId(),
            userLevel,
            cond);

        m_q->setEditCondition({ cond, m_histoIndex });
    }
}

void Histo1DWidgetPrivate::onCutEditorIntervalModified(const QwtInterval &)
{
}

void Histo1DWidget::activatePlotPicker(QwtPlotPicker *picker)
{
    m_d->activatePlotPicker(picker);
}

void Histo1DWidgetPrivate::activatePlotPicker(QwtPlotPicker *picker)
{
    if (m_activePlotPicker)
    {
        m_activePlotPicker->setEnabled(false);
    }

    if ((m_activePlotPicker = picker))
    {
        m_activePlotPicker->setEnabled(true);
    }

}

QwtPlotPicker *Histo1DWidget::getActivePlotPicker() const
{
    return m_d->getActivePlotPicker();
}

QwtPlot *Histo1DWidget::getPlot() const
{
    return m_d->m_plot;
}

// XXX: was listwidget
void Histo1DWidget::onHistoSpinBoxValueChanged(int index)
{
    selectHistogram(index);
}

void Histo1DWidget::selectHistogram(int index)
{
    if (0 <= index && index < m_d->m_histos.size())
    {
        m_d->m_histoIndex = index;

        assert(m_d->getCurrentHisto() == m_d->m_histos.value(index));

        m_d->m_plotHistoData = new Histo1DIntervalData(m_d->getCurrentHisto().get());
        m_d->m_plotHisto->setData(m_d->m_plotHistoData); // ownership goes to qwt

        m_d->m_gaussCurve->setData(new Histo1DGaussCurveData());

        m_d->m_plotRateEstimationData = new RateEstimationCurveData(m_d->getCurrentHisto().get(),
                                                                    &m_d->m_rateEstimationData);
        // ownership goes to qwt
        m_d->m_rateEstimationCurve->setData(m_d->m_plotRateEstimationData);

        m_d->m_rrSlider->setMaximum(std::log2(m_d->getCurrentHisto()->getNumberOfBins()));

        if (m_d->m_rrf == Histo1D::NoRR)
            m_d->m_rrSlider->setValue(m_d->m_rrSlider->maximum());

        //qDebug() << __PRETTY_FUNCTION__ << "new RRSlider max" << m_d->m_rrSlider->maximum();

        m_d->activatePlotPicker(m_d->m_zoomer);
        m_d->displayChanged();

        QSignalBlocker sb(m_d->m_histoSpin);
        m_d->m_histoSpin->setValue(index);
    }




#if 0
    if (auto histo = m_d->m_histos.value(index))
    {
        m_d->m_histoWidget->setHistogram(histo.get());
        m_d->m_histoWidget->setContext(m_d->m_context);

        if (m_d->m_calib)
        {
            m_d->m_histoWidget->setCalibrationInfo(m_d->m_calib, index);
        }

        if (m_d->m_sink)
        {
            m_d->m_histoWidget->setSink(m_d->m_sink, m_d->m_sinkModifiedCallback);
        }

        if (auto cl = m_d->m_histoWidget->getEditCondition())
        {
            cl.subIndex = index;
            m_d->m_histoWidget->setEditCondition(cl);
        }

        // Update spinbox value to show the current index. Needed in case
        // selectHistogram() was called from the outside instead of from
        // the user interacting with the spinbox.
        QSignalBlocker sb(m_d->m_histoSpin);
        m_d->m_histoSpin->setValue(index);
    }
#endif
}

void Histo1DWidget::setCalibration(const std::shared_ptr<analysis::CalibrationMinMax> &calib)
{
    m_d->m_calib = calib;
    m_d->m_actionCalibUi->setVisible(m_d->m_calib != nullptr);
}

QwtPlotPicker *Histo1DWidgetPrivate::getActivePlotPicker() const
{
    return m_activePlotPicker;
}


//
// Histo1DListWidget
//

using namespace analysis;

#if 0
struct Histo1DListWidget::Private
{
    Private(Histo1DListWidget *q) : m_q(q) {}

    Histo1DListWidget *m_q;
    HistoList m_histos;
    Histo1DWidget *m_histoWidget;
    QSpinBox *m_histoSpin;
    s32 m_histoIndex = 0;
    std::shared_ptr<analysis::CalibrationMinMax> m_calib;
    MVMEContext *m_context = nullptr;
    SinkPtr m_sink;
    HistoSinkCallback m_sinkModifiedCallback;
};

Histo1DListWidget::HistoList Histo1DListWidget::getHistograms() const
{
    return m_d->m_histos;
}

Histo1DListWidget::Histo1DListWidget(const HistoList &histos, QWidget *parent)
    : QWidget(parent)
    , m_d(std::make_unique<Private>(this))
{
    m_d->m_histos = histos;
    m_d->m_histoSpin = new QSpinBox(this);
    m_d->m_histoIndex = 0;

    resize(600, 400);
    Q_ASSERT(histos.size());

    auto histo = histos[0].get();
    m_d->m_histoWidget = new Histo1DWidget(histo, this);

    connect(m_d->m_histoWidget, &QWidget::windowTitleChanged, this, &QWidget::setWindowTitle);

    /* Add the histo index spinbox to the Histo1DWidget toolbar. */

    m_d->m_histoSpin->setMaximum(histos.size() - 1);
    set_widget_font_pointsize_relative(m_d->m_histoSpin, -2);

    connect(m_d->m_histoSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &Histo1DListWidget::onHistoSpinBoxValueChanged);

    m_d->m_histoWidget->m_d->m_toolBar->addWidget(make_vbox_container(QSL("Histogram #"),
                                                                 m_d->m_histoSpin, 2, -2)
                      .container.release());

    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(m_d->m_histoWidget);

    setWindowTitle(m_d->m_histoWidget->windowTitle());
    selectHistogram(0);
}

Histo1DListWidget::~Histo1DListWidget()
{}

void Histo1DListWidget::setContext(MVMEContext *context)
{
    m_d->m_context = context;
}


void Histo1DListWidget::setSink(const SinkPtr &sink, HistoSinkCallback sinkModifiedCallback)
{
    Q_ASSERT(sink);
    m_d->m_sink = sink;
    m_d->m_sinkModifiedCallback = sinkModifiedCallback;

    onHistoSpinBoxValueChanged(m_d->m_histoIndex);
}

void Histo1DListWidget::selectHistogram(int index)
{
    m_d->m_histoIndex = index;

    if (auto histo = m_d->m_histos.value(index))
    {
        m_d->m_histoWidget->setHistogram(histo.get());
        m_d->m_histoWidget->setContext(m_d->m_context);

        if (m_d->m_calib)
        {
            m_d->m_histoWidget->setCalibrationInfo(m_d->m_calib, index);
        }

        if (m_d->m_sink)
        {
            m_d->m_histoWidget->setSink(m_d->m_sink, m_d->m_sinkModifiedCallback);
        }

        if (auto cl = m_d->m_histoWidget->getEditCondition())
        {
            cl.subIndex = index;
            m_d->m_histoWidget->setEditCondition(cl);
        }

        // Update spinbox value to show the current index. Needed in case
        // selectHistogram() was called from the outside instead of from
        // the user interacting with the spinbox.
        QSignalBlocker sb(m_d->m_histoSpin);
        m_d->m_histoSpin->setValue(index);
    }
}

bool Histo1DListWidget::setEditCondition(const analysis::ConditionLink &cl)
{
    qDebug() << __PRETTY_FUNCTION__ << this << cl.condition.get();

    auto cond = std::dynamic_pointer_cast<ConditionInterval>(cl.condition);

    if (!cond || cl.subIndex < 0 || cl.subIndex >= m_d->m_histos.size())
        return false;

    selectHistogram(cl.subIndex);
    return m_d->m_histoWidget->setEditCondition(cl);
}

analysis::ConditionLink Histo1DListWidget::getEditCondition() const
{
    return m_d->m_histoWidget->getEditCondition();
}

void Histo1DListWidget::beginEditCondition()
{
    m_d->m_histoWidget->beginEditCondition();
}

#endif
