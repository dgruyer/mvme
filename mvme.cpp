#include "mvme.h"
#include "ui_mvme.h"
#include "vmusb.h"
#include "mvmecontrol.h"
#include "ui_mvmecontrol.h" // FIXME
#include "mvme_context.h"
#include "vme_module.h"
#include "twodimwidget.h"
#include "diagnostics.h"
#include "realtimedata.h"
#include "channelspectro.h"
#include "histogram.h"
#include "datacruncher.h"
#include "datathread.h"
#include "mvmedefines.h"
#include "mvme_context_widget.h"
#include "ui_moduleconfig_widget.h"
#include "vmusb_readout_worker.h"

#include <QDockWidget>
#include <QFileDialog>
#include <QMdiSubWindow>
#include <QMessageBox>
#include <QtGui>
#include <QTimer>
#include <QToolBar>
#include <QTextEdit>
#include <QFont>
#include <QList>
#include <QTextBrowser>

#include <qwt_plot_curve.h>

EventConfigWidget::EventConfigWidget(EventConfig *config, QWidget *parent)
    : QWidget(parent)
    , m_config(config)
{
}

enum class ModuleListType
{
    Parameters,
    Readout,
    StartDAQ,
    StopDAQ,
    Reset,
    ReadoutStack
};

QString *getConfigString(ModuleListType type, ModuleConfig *config)
{
    switch (type)
    {
        case ModuleListType::Parameters:
            return &config->initParameters;
        case ModuleListType::Readout:
            return &config->initReadout;
        case ModuleListType::StartDAQ:
            return &config->initStartDaq;
        case ModuleListType::StopDAQ:
            return &config->initStopDaq;
        case ModuleListType::Reset:
            return &config->initReset;
        case ModuleListType::ReadoutStack:
            return &config->readoutStack;
    }

    return nullptr;
}

ModuleConfigWidget::ModuleConfigWidget(MVMEContext *context, ModuleConfig *config, QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::ModuleConfigWidget)
    , m_context(context)
    , m_config(config)
{
    ui->setupUi(this);
    setWindowTitle(QString("Module Config for %1").arg(m_config->name));

    connect(context, &MVMEContext::moduleAboutToBeRemoved, [=](ModuleConfig *module) {
        if (module == config)
        {
            auto pw = parentWidget();
            if (pw)
                pw->close();
        }
    });



    ui->combo_listType->addItem("Module Init", QVariant::fromValue(static_cast<int>(ModuleListType::Parameters)));
    ui->combo_listType->addItem("Readout Settings", QVariant::fromValue(static_cast<int>(ModuleListType::Readout)));
    ui->combo_listType->addItem("Readout Stack (VM_USB)", QVariant::fromValue(static_cast<int>(ModuleListType::ReadoutStack)));
    ui->combo_listType->addItem("Start DAQ", QVariant::fromValue(static_cast<int>(ModuleListType::StartDAQ)));
    ui->combo_listType->addItem("Stop DAQ", QVariant::fromValue(static_cast<int>(ModuleListType::StopDAQ)));
    ui->combo_listType->addItem("Module Reset", QVariant::fromValue(static_cast<int>(ModuleListType::Reset)));

    connect(ui->combo_listType, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &ModuleConfigWidget::handleListTypeIndexChanged);

    ui->label_type->setText(VMEModuleTypeNames[config->type]);

    ui->le_name->setText(config->name);
    connect(ui->le_name, &QLineEdit::editingFinished, this, &ModuleConfigWidget::onNameEditFinished);

    ui->le_address->setInputMask("\\0\\xHHHH\\0\\0\\0\\0");
    ui->le_address->setText(QString().sprintf("0x%08x", config->baseAddress));
    connect(ui->le_address, &QLineEdit::editingFinished, this, &ModuleConfigWidget::onAddressEditFinished);

    ui->editor->setPlainText(*getConfigString(ModuleListType::Parameters, config));
    ui->editor->document()->setModified(false);

    connect(ui->editor->document(), &QTextDocument::contentsChanged,
            this, &ModuleConfigWidget::editorContentsChanged);

    actLoadFile = new QAction("from file", this);
    actLoadTemplate = new QAction("from template", this);

    auto menu = new QMenu(ui->pb_load);
    menu->addAction(actLoadFile);
    menu->addAction(actLoadTemplate);
    ui->pb_load->setMenu(menu);

    connect(actLoadFile, &QAction::triggered, this, &ModuleConfigWidget::loadFromFile);
    connect(actLoadTemplate, &QAction::triggered, this, &ModuleConfigWidget::loadFromTemplate);
    connect(ui->pb_save, &QPushButton::clicked, this, &ModuleConfigWidget::saveToFile);
    connect(ui->pb_exec, &QPushButton::clicked, this, &ModuleConfigWidget::execList);

    ui->splitter->setSizes({1, 0});
}

void ModuleConfigWidget::handleListTypeIndexChanged(int index)
{
    if (m_lastListTypeIndex >= 0 && ui->editor->document()->isModified())
    {
        auto type = static_cast<ModuleListType>(ui->combo_listType->itemData(m_lastListTypeIndex).toInt());

        QString *dest = getConfigString(type, m_config);
        if (dest)
        {
            *dest = ui->editor->toPlainText();
        }
    }

    m_lastListTypeIndex = index;

    m_ignoreEditorContentsChange = true;
    ui->editor->clear();
    ui->editor->document()->clearUndoRedoStacks();
    auto type = static_cast<ModuleListType>(ui->combo_listType->itemData(index).toInt());
    QString *contents = getConfigString(type, m_config);
    if (contents)
    {
        ui->editor->setPlainText(*contents);
    }
    ui->editor->document()->setModified(false);
    m_ignoreEditorContentsChange = false;

    switch (type)
    {
        case ModuleListType::ReadoutStack:
            ui->pb_exec->setText("Exec");
            break;
        default:
            ui->pb_exec->setText("Run");
            break;
    }
}

void ModuleConfigWidget::editorContentsChanged()
{
    if (m_ignoreEditorContentsChange)
        return;

    auto type = static_cast<ModuleListType>(ui->combo_listType->itemData(m_lastListTypeIndex).toInt());
    QString *dest = getConfigString(type, m_config);
    if (dest)
    {
        *dest = ui->editor->toPlainText();
    }
}

void ModuleConfigWidget::onNameEditFinished()
{
    QString name = ui->le_name->text();
    if (ui->le_name->hasAcceptableInput() && name.size())
    {
        m_config->name = name;
        m_context->notifyConfigModified();
    }
    else
    {
        ui->le_name->setText(m_config->name);
    }
}

void ModuleConfigWidget::onAddressEditFinished()
{
    if (ui->le_address->hasAcceptableInput())
    {
        bool ok;
        m_config->baseAddress = ui->le_address->text().toUInt(&ok, 16);
        m_context->notifyConfigModified();
    }
    else
    {
        auto text = QString().sprintf("0x%08x", m_config->baseAddress);
        ui->le_address->setText(text);
    }
}

void ModuleConfigWidget::closeEvent(QCloseEvent *event)
{
    //qDebug() << __PRETTY_FUNCTION__;
    QWidget::closeEvent(event);
}

void ModuleConfigWidget::loadFromFile()
{
}

void ModuleConfigWidget::loadFromTemplate()
{
    QString templatePath = QCoreApplication::applicationDirPath() + "/templates";
    QString fileName = QFileDialog::getOpenFileName(this, "Load init file", templatePath,
                                                    "MVME Config Files (*.mvmecfg);; All Files (*.*)");
}

void ModuleConfigWidget::saveToFile()
{
}

void ModuleConfigWidget::execList()
{
    auto controller = m_context->getController();

    if (controller && controller->isOpen())
    {
        auto type = static_cast<ModuleListType>(ui->combo_listType->currentData().toInt());
        QString listContents = ui->editor->toPlainText();

        switch (type)
        {
            case ModuleListType::Parameters:
            case ModuleListType::Readout:
            case ModuleListType::StartDAQ:
            case ModuleListType::StopDAQ:
            case ModuleListType::Reset:
                {
                    u8 ignored[100];
                    auto cmdList = VMECommandList::fromInitList(parseInitList(listContents), m_config->baseAddress);
                    controller->executeCommands(&cmdList, ignored, sizeof(ignored));
                } break;
            case ModuleListType::ReadoutStack:
                {
                    auto vmusb = dynamic_cast<VMUSB *>(controller);
                    if (vmusb)
                    {
                        QVector<u32> result = vmusb->stackExecute(parseStackFile(listContents), 1<<16);
                        QString buf;
                        QTextStream stream(&buf);
                        for (int idx=0; idx<result.size(); ++idx)
                        {
                            u32 value = result[idx];

                            stream
                                << qSetFieldWidth(4) << qSetPadChar(' ') << dec << idx
                                << qSetFieldWidth(0) << ": 0x"
                                << hex << qSetFieldWidth(8) << qSetPadChar('0') << value
                                << qSetFieldWidth(0) 
                                << endl;
                        }
                        ui->output->setPlainText(buf);
                        ui->splitter->setSizes({1, 1});
                    }
                } break;
        }
    }
}

template<typename T>
QList<T *> getSubwindowWidgetsByType(QMdiArea *mdiArea)
{
    QList<T *> ret;

    for (auto subwin: mdiArea->subWindowList())
    {
        auto w = qobject_cast<T *>(subwin->widget());
        if (w)
        {
            ret.append(w);
        }
    }

    return ret;
}

static const int DrawTimerInterval = 1000;

template<typename T>
QList<QMdiSubWindow *> getSubwindowsByWidgetType(QMdiArea *mdiArea)
{
    QList<QMdiSubWindow *> ret;

    for (auto subwin: mdiArea->subWindowList())
    {
        auto w = qobject_cast<T *>(subwin->widget());
        if (w)
        {
            ret.append(subwin);
        }
    }

    return ret;
}
mvme::mvme(QWidget *parent) :
    QMainWindow(parent),
    vu(0),
    mctrl(0),
    dt(0),
    dc(0),
    diag(0),
    rd(0),
    m_channelSpectro(new ChannelSpectro(1024, 1024)),
    ui(new Ui::mvme)
    , m_context(new MVMEContext(this))
    , m_logView(new QTextBrowser)
{

    qDebug() << "main thread: " << QThread::currentThread();

    m_histogram[0] = new Histogram(this, 42, 8192);
    m_histogram[0]->initHistogram();

    m_channelSpectro->setXAxisChannel(0);
    m_channelSpectro->setYAxisChannel(1);

    // create and initialize displays
    ui->setupUi(this);
    auto contextWidget = new MVMEContextWidget(m_context);
    m_contextWidget = contextWidget;
    connect(contextWidget, &MVMEContextWidget::eventClicked, this, &mvme::handleEventConfigClicked);
    connect(contextWidget, &MVMEContextWidget::moduleClicked, this, &mvme::handleModuleConfigClicked);
    connect(contextWidget, &MVMEContextWidget::moduleDoubleClicked, this, &mvme::handleModuleConfigDoubleClicked);

    connect(contextWidget, &MVMEContextWidget::deleteEvent, this, &mvme::handleDeleteEventConfig);
    connect(contextWidget, &MVMEContextWidget::deleteModule, this, &mvme::handleDeleteModuleConfig);

    connect(contextWidget, &MVMEContextWidget::histogramClicked, this, &mvme::handleHistogramClicked);
    connect(contextWidget, &MVMEContextWidget::histogramDoubleClicked, this, &mvme::handleHistogramDoubleClicked);
    connect(contextWidget, &MVMEContextWidget::showHistogram, this, &mvme::openHistogramView);

    auto contextDock = new QDockWidget();
    contextDock->setObjectName("MVMEContextDock");
    contextDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    contextDock->setWidget(contextWidget);
    addDockWidget(Qt::LeftDockWidgetArea, contextDock);

    //createNewHistogram();
    //createNewChannelSpectrogram();

    rd = new RealtimeData;
    diag = new Diagnostics;

    // check and initialize VME interface
    vu = new VMUSB;
    m_context->setController(vu);
    vu->getUsbDevices();
    vu->openFirstUsbDevice();


    mctrl = new mvmeControl(this);
    mctrl->show();
#if 0
    {
        auto subwin = new QMdiSubWindow(ui->mdiArea);
        subwin->setWidget(mctrl);
        subwin->resize(mctrl->size());
        subwin->show();
    }
#endif

    // read current configuration
    mctrl->getValues();

    drawTimer = new QTimer(this);
    connect(drawTimer, SIGNAL(timeout()), SLOT(drawTimerSlot()));
    drawTimer->start(DrawTimerInterval);

    //initThreads();

    QSettings settings;
    restoreGeometry(settings.value("mainWindowGeometry").toByteArray());
    restoreState(settings.value("mainWindowState").toByteArray());

    if (settings.contains("Files/LastConfigFile"))
    {
        auto fileName = settings.value("Files/LastConfigFile").toString();
        QFile inFile(fileName);
        if (inFile.open(QIODevice::ReadOnly))
        {
            auto data = inFile.readAll();
            QJsonDocument doc(QJsonDocument::fromJson(data));
            m_context->getConfig()->read(doc.object());
            m_context->m_configFilename = fileName;
            m_contextWidget->reloadConfig();
        }
    }

    m_logView->setWindowTitle("Log View");
    m_logView->setFont(QFont("MonoSpace"));
    m_logView->document()->setMaximumBlockCount(10 * 1024 * 1024);
    m_logViewSubwin = new QMdiSubWindow;
    m_logViewSubwin->setWidget(m_logView);
    m_logViewSubwin->setAttribute(Qt::WA_DeleteOnClose, false);
    ui->mdiArea->addSubWindow(m_logViewSubwin);

    //connect(m_context->m_dataProcessor, &DataProcessor::eventFormatted,
    //        textView, &QTextEdit::append);
    connect(m_context, &MVMEContext::daqStateChanged, [=](DAQState state) {
        if (state == DAQState::Starting)
        {
            m_logView->clear();
        }
        else if (state == DAQState::Running)
        {
            m_logView->setText(m_context->getReadoutWorker()->getStartupDebugString());
        }
    });
}

mvme::~mvme()
{
    delete vu;
    delete mctrl;
    delete ui;
//    delete hist;
    delete m_histogram.value(0);
    delete dt;
    delete dc;
    delete rd;
    delete m_channelSpectro;
    delete m_context;
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
    auto tdw = new TwoDimWidget(m_context, m_histogram.value(0));
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
#if 0
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
#endif
}

Histogram *mvme::getHist(quint16 mod)
{
    Q_ASSERT(mod == 0);
    return m_histogram.value(0);
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
    auto subwin = ui->mdiArea->currentSubWindow();
    auto widget = subwin ? subwin->widget() : nullptr;
    auto tdw = qobject_cast<TwoDimWidget *>(widget);

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

    auto subwin = ui->mdiArea->currentSubWindow();
    auto widget = subwin ? subwin->widget() : nullptr;
    auto tdw = qobject_cast<TwoDimWidget *>(widget);

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
    auto subwin = ui->mdiArea->currentSubWindow();
    auto widget = subwin ? subwin->widget() : nullptr;
    auto tdw = qobject_cast<TwoDimWidget *>(widget);

    if (tdw)
    {
        tdw->exportPlot();
    }
}

void mvme::on_actionExport_Spectrogram_triggered()
{
    auto subwin = ui->mdiArea->currentSubWindow();
    auto widget = subwin ? subwin->widget() : nullptr;
    auto spectroWidget = qobject_cast<ChannelSpectroWidget *>(widget);

    if (spectroWidget)
    {
        spectroWidget->exportPlot();
    }
}

void mvme::on_actionNewConfig_triggered()
{
    if (m_context->getConfig()->isModified)
    {
        auto msgBox = new QMessageBox(QMessageBox::Question, "Configuration modified",
                                      "The current configuration has modifications. Do you want to save it?",
                                      QMessageBox::Save | QMessageBox::Cancel | QMessageBox::Discard);
        int result = msgBox->exec();

        if (result == QMessageBox::Save)
        {
            on_actionSaveConfig_triggered();
        }
        else if (result == QMessageBox::Cancel)
        {
            return;
        }
    }

    m_context->setConfig(new DAQConfig);
}

void mvme::on_actionLoadConfig_triggered()
{
    QString fileName = QFileDialog::getOpenFileName(this, "Load MVME Config",
                                                   QString(),
                                                    "MVME Config Files (*.mvmecfg);; All Files (*.*)");
    if (fileName.isEmpty())
        return;

    QFile inFile(fileName);
    if (!inFile.open(QIODevice::ReadOnly))
    {
        QMessageBox::critical(0, "Error", QString("Error reading from %1").arg(fileName));
        return;
    }

    for (auto win: ui->mdiArea->subWindowList())
    {
        if(qobject_cast<EventConfigWidget *>(win->widget()) ||
           qobject_cast<ModuleConfigWidget *>(win->widget()))
        {
            win->close();
        }
    }

    auto data = inFile.readAll();
    QJsonDocument doc(QJsonDocument::fromJson(data));
    m_context->getConfig()->read(doc.object());
    m_context->m_configFilename = fileName;

    QSettings settings;
    settings.setValue("Files/LastConfigFile", fileName);

    m_contextWidget->reloadConfig();
}

void mvme::on_actionSaveConfig_triggered()
{
    if (m_context->m_configFilename.isEmpty())
    {
        on_actionSaveConfigAs_triggered();
        return;
    }

    QFile outFile(m_context->m_configFilename);
    if (!outFile.open(QIODevice::WriteOnly))
    {
        QMessageBox::critical(0, "Error", QString("Error writing to %1").arg(m_context->m_configFilename));
    }

    outFile.write(m_context->getConfig()->toJson());
}

void mvme::on_actionSaveConfigAs_triggered()
{
    QString fileName = QFileDialog::getSaveFileName(this, "Save Config As", QString(),
                                                    "MVME Config Files (*.mvmecfg);; All Files (*.*)");

    if (fileName.isEmpty())
        return;

    QFile outFile(fileName);
    if (!outFile.open(QIODevice::WriteOnly))
    {
        QMessageBox::critical(0, "Error", QString("Error writing to %1").arg(fileName));
        return;
    }

    outFile.write(m_context->getConfig()->toJson());

    m_context->m_configFilename = fileName;
}

void mvme::on_actionShowLogWindow_triggered()
{
    m_logViewSubwin->widget()->show();
    m_logViewSubwin->show();
    m_logViewSubwin->showNormal();
    m_logViewSubwin->raise();
}

void mvme::on_mdiArea_subWindowActivated(QMdiSubWindow *subwin)
{
    auto widget = subwin ? subwin->widget() : nullptr;
    auto tdw = qobject_cast<TwoDimWidget *>(widget);

    ui->actionExport_Histogram->setVisible(tdw);
    ui->actionLoad_Histogram->setVisible(tdw);
    ui->actionSave_Histogram->setVisible(tdw);

    auto spectroWidget = qobject_cast<ChannelSpectroWidget *>(widget);

    ui->actionExport_Spectrogram->setVisible(spectroWidget);
}

void mvme::handleEventConfigClicked(EventConfig *config)
{
}

void mvme::handleModuleConfigClicked(ModuleConfig *config)
{
    QMdiSubWindow *subwin = 0;
    for (auto win: ui->mdiArea->subWindowList())
    {
        auto widget = qobject_cast<ModuleConfigWidget *>(win->widget());
        if (widget && widget->getConfig() == config)
        {
            subwin = win;
            break;
        }
    }

    if (subwin)
    {
        subwin->show();
        if (subwin->isMinimized())
            subwin->showNormal();
        subwin->raise();
    }
    ui->mdiArea->setActiveSubWindow(subwin);
}

void mvme::handleModuleConfigDoubleClicked(ModuleConfig *config)
{
    for (auto win: ui->mdiArea->subWindowList())
    {
        auto widget = qobject_cast<ModuleConfigWidget *>(win->widget());
        if (widget && widget->getConfig() == config)
        {
            return;
        }
    }

    auto widget = new ModuleConfigWidget(m_context, config);
    auto subwin = new QMdiSubWindow;
    subwin->setAttribute(Qt::WA_DeleteOnClose);
    subwin->setWidget(widget);
    subwin->show();
    ui->mdiArea->addSubWindow(subwin);
    ui->mdiArea->setActiveSubWindow(subwin);
}

void mvme::handleDeleteEventConfig(EventConfig *event)
{
}

void mvme::handleDeleteModuleConfig(ModuleConfig *module)
{
}

void mvme::handleHistogramClicked(const QString &name, Histogram *histo)
{
    QMdiSubWindow *subwin = 0;
    for (auto win: ui->mdiArea->subWindowList())
    {
        auto widget = qobject_cast<TwoDimWidget *>(win->widget());
        if (widget && widget->getHistogram() == histo)
        {
            subwin = win;
            break;
        }
    }

    if (subwin)
    {
        subwin->show();
        if (subwin->isMinimized())
            subwin->showNormal();
        subwin->raise();
    }
    ui->mdiArea->setActiveSubWindow(subwin);
}

void mvme::handleHistogramDoubleClicked(const QString &name, Histogram *histo)
{
    for (auto win: ui->mdiArea->subWindowList())
    {
        auto widget = qobject_cast<TwoDimWidget *>(win->widget());
        if (widget && widget->getHistogram() == histo)
        {
            return;
        }
    }

    openHistogramView(histo);
}

void mvme::openHistogramView(Histogram *histo)
{
    if (histo)
    {
        auto widget = new TwoDimWidget(m_context, histo);
        auto subwin = new QMdiSubWindow(ui->mdiArea);
        subwin->setWidget(widget);
        subwin->setAttribute(Qt::WA_DeleteOnClose);
        ui->mdiArea->addSubWindow(subwin);
        subwin->show();
        ui->mdiArea->setActiveSubWindow(subwin);

        qDebug() << ui->mdiArea->subWindowList();
    }
}
