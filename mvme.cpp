#include "mvme.h"
#include "ui_mvme.h"
#include "vmusb.h"
#include "mvmecontrol.h"
#include "ui_mvmecontrol.h"
#include <qwt_plot_curve.h>
#include "histogram.h"
#include "datacruncher.h"
#include "datathread.h"
#include "mvmedefines.h"
#include <QTimer>
#include <QtGui>
#include <QMessageBox>
#include "twodimwidget.h"
#include "diagnostics.h"
#include "realtimedata.h"
#include "channelspectro.h"
#include <QFileDialog>
#include <QMdiSubWindow>


mvme::mvme(QWidget *parent) :
    QMainWindow(parent),
    vu(0),
    mctrl(0),
    dt(0),
    dc(0),
    diag(0),
    rd(0),
    m_channelSpectro(new ChannelSpectro(1024, 1024)),
    ui(new Ui::mvme),
    m_readoutThread(new QThread)
{

    qDebug() << "main thread: " << QThread::currentThread();

    m_readoutThread->setObjectName("ReadoutThread");

    // TODO: currently not fully implemented
    loadSetup();

    // TODO: not fully implemented yet
    findController();

    // TODO: not fully implemented yet
    createHistograms();
    m_histogram[0] = new Histogram(this, 42, 8192);
    m_histogram[0]->initHistogram();

    m_channelSpectro->setXAxisChannel(0);
    m_channelSpectro->setYAxisChannel(1);

    // create and initialize displays
    ui->setupUi(this);
    createNewHistogram();
    createNewChannelSpectrogram();

    rd = new RealtimeData;
    diag = new Diagnostics;

    // check and initialize VME interface
    vu = new vmUsb;
    vu->getUsbDevices();
    if (!vu->openUsbDevice()) {
      qDebug("No VM USB controller found");
      return;
    }

    // clear the action register (makes sure daq mode is disabled)
    vu->usbRegisterWrite(1, 0);

    mctrl = new mvmeControl(this);
    mctrl->show();

    // read current configuration
    mctrl->getValues();

    //cu = new caenusb();
    //cu->openUsbDevice();
    //cu->openUsbDevice();

    // set default values
    //cu->initialize();


    drawTimer = new QTimer(this);
    connect(drawTimer, SIGNAL(timeout()), SLOT(drawTimerSlot()));

    initThreads();

    QSettings settings;
    restoreGeometry(settings.value("mainWindowGeometry").toByteArray());
    restoreState(settings.value("mainWindowState").toByteArray());
}

mvme::~mvme()
{
    m_readoutThread->quit();
    m_readoutThread->wait();
    delete m_readoutThread;

    delete vu;
    delete mctrl;
    delete ui;
//    delete hist;
    delete m_histogram.value(0);
    delete dt;
    delete dc;
    delete rd;
    delete m_channelSpectro;
}

void mvme::replot()
{
    foreach(QMdiSubWindow *w, ui->mdiArea->subWindowList())
    {
        auto tdw = qobject_cast<TwoDimWidget *>(w->widget());
        if (tdw)
        {
            tdw->plot();
        }
    }
}

void mvme::drawTimerSlot()
{
    replot();
    rd->calcData();
    mctrl->dispRt();
}

void mvme::displayAbout()
{
    QMessageBox::about(this, tr("about mvme"), tr("mvme by G. Montermann, mesytec GmbH & Co. KG"));
}

void mvme::createNewHistogram()
{
    auto tdw = new TwoDimWidget(this);
    tdw->setHistogram(m_histogram.value(0));
    tdw->plot();

    auto subwin = new QMdiSubWindow(ui->mdiArea);
    subwin->setWidget(tdw);
    subwin->show();
}

void mvme::createNewChannelSpectrogram()
{
    auto subwin = new QMdiSubWindow(ui->mdiArea);
    auto channelSpectroWidget = new ChannelSpectroWidget(m_channelSpectro);
    subwin->setWidget(channelSpectroWidget);
    subwin->show();
}

void mvme::cascade()
{
    qDebug("implement cascade");
}

void mvme::tile()
{
    qDebug("implement tile");
}

void mvme::startDatataking(quint16 period, bool multi, quint16 readLen, bool mblt, bool daqMode)
{
    QString outputFileName = mctrl->getOutputFileName();

    if (!outputFileName.isNull())
    {
        QFile *outputFile = new QFile(outputFileName);
        outputFile->open(QIODevice::WriteOnly);
        dt->setOutputFile(outputFile);
    }

    QString inputFileName = mctrl->getInputFileName();

    if (!inputFileName.isNull())
    {
        QFile *inputFile = new QFile(inputFileName);
        inputFile->open(QIODevice::ReadOnly);
        dt->setInputFile(inputFile);
    }

    dt->setReadoutmode(multi, readLen, mblt, daqMode);
    dt->startReading(period);

    drawTimer->start(750);
    datataking = true;
}

void mvme::stopDatataking()
{
    QTime timer;
    timer.start();

    dt->stopReading();
    drawTimer->stop();
    datataking = false;
    qDebug() << __PRETTY_FUNCTION__ << "elapsed:" << timer.elapsed();
}

void mvme::initThreads()
{
    dt = new DataThread;
    dt->setVu(vu);

    dt->moveToThread(m_readoutThread);

    //dt->setCu(cu);
    dc = new DataCruncher;
    connect(dt, SIGNAL(dataReady()), dc, SLOT(newEvent()));
    connect(dt, SIGNAL(bufferStatus(int)), mctrl->ui->bufferProgress, SLOT(setValue(int)));
    connect(dc, SIGNAL(bufferStatus(int)), mctrl->ui->bufferProgress, SLOT(setValue(int)));

    dc->setHistogram(m_histogram.value(0));
    dc->initRingbuffer(RINGBUFSIZE);

    dt->setRingbuffer(dc->getRingBuffer());

    dc->setRtData(rd);
    dc->setChannelSpectro(m_channelSpectro);

    m_readoutThread->start(QThread::TimeCriticalPriority);
    dc->start(QThread::NormalPriority);
}

Histogram *mvme::getHist(quint16 mod)
{
    return m_histogram.value(0);
}

bool mvme::loadSetup()
{
    return true;
}

bool mvme::findController()
{
    return true;
}

bool mvme::createHistograms()
{
    return true;
}

bool mvme::clearAllHist()
{
    m_histogram[0]->clearHistogram();
    return true;
}

Histogram *mvme::getHist()
{
    return m_histogram[0];
}

void mvme::closeEvent(QCloseEvent *event){
    qDebug("close Event");
    QSettings settings;
    settings.setValue("mainWindowGeometry", saveGeometry());
    settings.setValue("mainWindowState", saveState());
    QMainWindow::closeEvent(event);
}


void mvme::on_actionSave_Histogram_triggered()
{
    auto tdw = qobject_cast<TwoDimWidget *>(ui->mdiArea->currentSubWindow()->widget());

    if (!tdw)
        return;

    quint32 channelIndex = tdw->getSelectedChannelIndex();

    QString buffer;
    buffer.sprintf("histogram_channel%02u.txt", channelIndex);
    QString fileName = QFileDialog::getSaveFileName(this, "Save Histogram", buffer,
                                                    "Text Files (*.txt);; All Files (*.*)");

    if (fileName.isEmpty())
        return;



    QFile outFile(fileName);
    if (!outFile.open(QIODevice::WriteOnly))
        return;

    QTextStream stream(&outFile);
    writeHistogram(stream, m_histogram[0], channelIndex);
}

void mvme::on_actionLoad_Histogram_triggered()
{
    QString fileName = QFileDialog::getOpenFileName(this, "Load Histogram",
                                                   QString(),
                                                   "Text Files (*.txt);; All Files (*.*)");

    if (fileName.isEmpty())
        return;

    QFile inFile(fileName);
    if (!inFile.open(QIODevice::ReadOnly))
        return;

    QTextStream stream(&inFile);

    quint32 channelIndex = 0;

    readHistogram(stream, m_histogram[0], &channelIndex);

    auto tdw = qobject_cast<TwoDimWidget *>(ui->mdiArea->currentSubWindow()->widget());

    if (tdw)
    {
        tdw->setSelectedChannelIndex(channelIndex);
    }

    foreach(QMdiSubWindow *w, ui->mdiArea->subWindowList())
    {
        auto tdw = qobject_cast<TwoDimWidget *>(w->widget());
        if (tdw)
        {
            tdw->plot();
        }
    }
}

void mvme::on_actionExport_Histogram_triggered()
{
    auto tdw = qobject_cast<TwoDimWidget *>(ui->mdiArea->currentSubWindow()->widget());

    if (tdw)
    {
        tdw->exportPlot();
    }
}

void mvme::on_actionExport_Spectrogram_triggered()
{
    auto spectroWidget = qobject_cast<ChannelSpectroWidget *>(ui->mdiArea->currentSubWindow()->widget());

    if (spectroWidget)
    {
        spectroWidget->exportPlot();
    }
}

void mvme::on_mdiArea_subWindowActivated(QMdiSubWindow *subwin)
{
    auto tdw = qobject_cast<TwoDimWidget *>(ui->mdiArea->currentSubWindow()->widget());

    ui->actionExport_Histogram->setVisible(tdw);
    ui->actionLoad_Histogram->setVisible(tdw);
    ui->actionSave_Histogram->setVisible(tdw);

    auto spectroWidget = qobject_cast<ChannelSpectroWidget *>(ui->mdiArea->currentSubWindow()->widget());

    ui->actionExport_Spectrogram->setVisible(spectroWidget);
}
