#ifndef MVME_H
#define MVME_H

#include <QMainWindow>
#include <QMap>

class Hist2D;
class DataThread;
class Diagnostics;
class HistogramCollection;
class MVMEContext;
class RealtimeData;
class VirtualMod;
class vmedevice;
class EventConfig;
class ModuleConfig;
class MVMEContextWidget;
class DAQConfig;
class DAQConfigTreeWidget;
class HistogramTreeWidget;
class VMEScriptConfig;
class ConfigObject;

class QMdiSubWindow;
class QThread;
class QTimer;
class QwtPlotCurve;
class QTextBrowser;


namespace Ui {
class mvme;
}

class mvme : public QMainWindow
{
    Q_OBJECT

public:
    explicit mvme(QWidget *parent = 0);
    ~mvme();

    virtual void closeEvent(QCloseEvent *event);
    void restoreSettings();


    MVMEContext *getContext() { return m_context; }

public slots:
    void replot();
    void drawTimerSlot();
    void displayAbout();

    //void openHistogramView(HistogramCollection *histo);
    //void openHist2DView(Hist2D *hist2d);

private slots:
    void on_actionNewConfig_triggered();
    void on_actionLoadConfig_triggered();
    bool on_actionSaveConfig_triggered();
    bool on_actionSaveConfigAs_triggered();

    bool loadConfig(const QString &fileName);

    void on_actionOpen_Listfile_triggered();

    void on_actionVME_Debug_triggered();
    void on_actionShowLogWindow_triggered();

    void onObjectClicked(QObject *obj);
    void onObjectDoubleClicked(QObject *obj);

#if 0
    void handleHistogramCollectionClicked(HistogramCollection *histo);
    void handleHistogramCollectionDoubleClicked(HistogramCollection *histo);

    void handleHist2DClicked(Hist2D *hist2d);
    void handleHist2DDoubleClicked(Hist2D *hist2d);
#endif

    void appendToLog(const QString &);
    void updateWindowTitle();
    void onConfigChanged(DAQConfig *config);


    private:
    Ui::mvme *ui;
    QTimer* drawTimer;

    // list of possibly connected VME devices

    MVMEContext *m_context;
    MVMEContextWidget *m_contextWidget = 0;
    QTextBrowser *m_logView;
    QMdiSubWindow *m_logViewSubwin;
    QMap<QObject *, QMdiSubWindow *> m_objectWindows;
    DAQConfigTreeWidget *m_daqConfigTreeWidget;
    HistogramTreeWidget *m_histogramTreeWidget;
};

#endif // MVME_H
