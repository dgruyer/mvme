#ifndef MVME_H
#define MVME_H

#include <QMainWindow>
#ifdef VME_CONTROLLER_WIENER
#include "vmusb.h"
#endif
#ifdef VME_CONTROLLER_CAEN
#include "caenusb.h"
#endif
#include <QFile>
#include <QTextStream>
#include <QMap>

class Hist2D;
class DataCruncher;
class DataThread;
class Diagnostics;
class Histogram;
class MVMEContext;
class mvmeControl;
class RealtimeData;
class VirtualMod;
class vmedevice;
class EventConfig;
class ModuleConfig;
class MVMEContextWidget;
class DAQConfig;

class QMdiSubWindow;
class QThread;
class QTimer;
class QwtPlotCurve;
class QTextBrowser;


namespace Ui {
class mvme;
class ModuleConfigWidget;
}

class mvme : public QMainWindow
{
    Q_OBJECT

public:
    explicit mvme(QWidget *parent = 0);
    ~mvme();
#if 1
    void startDatataking(quint16 period, bool multi, quint16 readLen, bool mblt, bool daqMode);
    void stopDatataking();
    void initThreads();
#endif
    Histogram * getHist(quint16 mod);
    bool clearAllHist();
    Histogram* getHist();

    VMUSB* vu;
    //caenusb* cu;
    mvmeControl *mctrl;
    DataThread *dt;
    DataCruncher *dc;
    Diagnostics* diag;
    RealtimeData* rd;
    Hist2D *m_channelSpectro;


    void closeEvent(QCloseEvent *event);
    void restoreSettings();

public slots:
    void replot();
    void drawTimerSlot();
    void displayAbout();
    void createNewHistogram();
    void createNewChannelSpectrogram();

    void openHistogramView(Histogram *histo);
    void openHist2DView(Hist2D *hist2d);

private slots:
    void on_actionSave_Histogram_triggered();
    void on_actionLoad_Histogram_triggered();
    void on_actionExport_Histogram_triggered();
    void on_actionExport_Spectrogram_triggered();

    void on_actionNewConfig_triggered();
    void on_actionLoadConfig_triggered();
    bool on_actionSaveConfig_triggered();
    bool on_actionSaveConfigAs_triggered();

    bool loadConfig(const QString &fileName);

    void on_actionOpen_Listfile_triggered();
    //void on_actionClose_Listfile_triggered();

    void on_actionShowLogWindow_triggered();

    void on_mdiArea_subWindowActivated(QMdiSubWindow *);

    void handleEventConfigClicked(EventConfig *event);
    void handleModuleConfigClicked(ModuleConfig *module);
    void handleModuleConfigDoubleClicked(ModuleConfig *module);

    void handleDeleteEventConfig(EventConfig *event);
    void handleDeleteModuleConfig(ModuleConfig *module);

    void handleHistogramClicked(const QString &name, Histogram *histo);
    void handleHistogramDoubleClicked(const QString &name, Histogram *histo);

    void handleHist2DClicked(Hist2D *hist2d);
    void handleHist2DDoubleClicked(Hist2D *hist2d);

    void appendToLog(const QString &);
    void updateWindowTitle();
    void onConfigChanged(DAQConfig *config);

private:
    Ui::mvme *ui;
    bool datataking;
    QTimer* drawTimer;

    // list of possibly connected VME devices
    //QMap<int, vmedevice *> m_vmeDev;
    QMap<int, Histogram *> m_histogram;
    //QMap<int, VirtualMod *> m_virtualMod;

    MVMEContext *m_context;
    MVMEContextWidget *m_contextWidget = 0;
    QTextBrowser *m_logView;
    QMdiSubWindow *m_logViewSubwin;
    QMap<ModuleConfig *, QWidget *> m_configDialogs;
};

#endif // MVME_H
