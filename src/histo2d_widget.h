#ifndef __HISTO2D_WIDGET_H__
#define __HISTO2D_WIDGET_H__

#include "histo2d.h"
#include <QWidget>

class QTimer;
class QwtPlotSpectrogram;
class QwtLinearColorMap;
class QwtPlotHistogram;
class ScrollZoomer;
class MVMEContext;
class Histo1DWidget;
class WidgetGeometrySaver;

namespace analysis
{
    class Histo1DSink;
    class Histo2DSink;
};

class Histo2DWidgetPrivate;

class Histo2DWidget: public QWidget
{
    Q_OBJECT
    public:
        using SinkPtr = std::shared_ptr<analysis::Histo2DSink>;
        using HistoSinkCallback = std::function<void (const SinkPtr &)>;
        using MakeUniqueOperatorNameFunction = std::function<QString (const QString &name)>;
        using Histo1DSinkPtr = std::shared_ptr<analysis::Histo1DSink>;

        Histo2DWidget(const Histo2DPtr histoPtr, QWidget *parent = 0);
        Histo2DWidget(Histo2D *histo, QWidget *parent = 0);
        Histo2DWidget(const Histo1DSinkPtr &histo1DSink, QWidget *parent = 0);
        ~Histo2DWidget();

        void setSink(const SinkPtr &sink, HistoSinkCallback addSinkCallback, HistoSinkCallback sinkModifiedCallback,
                     MakeUniqueOperatorNameFunction makeUniqueOperatorNameFunction);

        virtual bool event(QEvent *event) override;

    private slots:
        void replot();
        void exportPlot();
        void mouseCursorMovedToPlotCoord(QPointF);
        void mouseCursorLeftPlot();
        void displayChanged();
        void zoomerZoomed(const QRectF &);
        void on_tb_info_clicked();
        void on_tb_subRange_clicked();
        void on_tb_projX_clicked();
        void on_tb_projY_clicked();

    private:
        Histo2DWidget(QWidget *parent = 0);

        bool zAxisIsLog() const;
        bool zAxisIsLin() const;
        QwtLinearColorMap *getColorMap() const;
        void updateCursorInfoLabel();
        void doXProjection();
        void doYProjection();

        Histo2DWidgetPrivate *m_d;
        friend class Histo2DWidgetPrivate;

        Histo2D *m_histo = nullptr;
        Histo2DPtr m_histoPtr;
        Histo1DSinkPtr m_histo1DSink;
        QwtPlotSpectrogram *m_plotItem;
        ScrollZoomer *m_zoomer;
        QTimer *m_replotTimer;
        QPointF m_cursorPosition;
        int m_labelCursorInfoWidth;

        std::shared_ptr<analysis::Histo2DSink> m_sink;
        HistoSinkCallback m_addSinkCallback;
        HistoSinkCallback m_sinkModifiedCallback;
        MakeUniqueOperatorNameFunction m_makeUniqueOperatorNameFunction;

        Histo1DWidget *m_xProjWidget = nullptr;
        Histo1DWidget *m_yProjWidget = nullptr;

        WidgetGeometrySaver *m_geometrySaver;
};

#endif /* __HISTO2D_WIDGET_H__ */
