#include "listfile_browser.h"

#include <QHeaderView>
#include <QBoxLayout>
#include <QMessageBox>

#include "analysis/analysis.h"
#include "mvme_context.h"
#include "mvme_context_lib.h"
#include "mvme.h"

ListfileBrowser::ListfileBrowser(MVMEContext *context, MVMEMainWindow *mainWindow, QWidget *parent)
    : QWidget(parent)
    , m_context(context)
    , m_mainWindow(mainWindow)
    , m_fsModel(new QFileSystemModel(this))
    , m_fsView(new QTableView(this))
    , m_analysisLoadActionCombo(new QComboBox(this))
{
    setWindowTitle(QSL("Listfile Browser"));

    set_widget_font_pointsize(this, 8);

    m_fsModel->setReadOnly(true);
    m_fsModel->setFilter(QDir::NoDotAndDotDot | QDir::Files | QDir::Dirs);

    m_fsView->setModel(m_fsModel);
    m_fsView->verticalHeader()->hide();
    m_fsView->hideColumn(2); // Hides the file type column
    m_fsView->setSortingEnabled(true);

    auto widgetLayout = new QVBoxLayout(this);

    // On listfile load
    {
        auto label = new QLabel(QSL("On listfile load"));
        auto combo = m_analysisLoadActionCombo;
        combo->addItem(QSL("keep current analysis"),        0u);
        combo->addItem(QSL("load analysis from listfile"),  OpenListfileFlags::LoadAnalysis);

        auto layout = new QHBoxLayout;
        layout->addWidget(label);
        layout->addWidget(combo);
        layout->addStretch();

        widgetLayout->addLayout(layout);
    }

    widgetLayout->addWidget(m_fsView);

    connect(m_context, &MVMEContext::workspaceDirectoryChanged,
            this, [this](const QString &) { updateWidget(); });

    connect(m_fsModel, &QFileSystemModel::directoryLoaded, this, [this](const QString &) {
        m_fsView->resizeColumnsToContents();
        m_fsView->resizeRowsToContents();
    });

    connect(m_fsView, &QAbstractItemView::doubleClicked,
            this, &ListfileBrowser::onItemDoubleClicked);

    updateWidget();
}

void ListfileBrowser::updateWidget()
{
    auto workspaceDirectory = m_context->getWorkspaceDirectory();
    auto workspaceSettings  = m_context->makeWorkspaceSettings();

    QDir dir(workspaceDirectory);
    QString listfileDirectory = dir.filePath(
        workspaceSettings->value(QSL("ListFileDirectory")).toString());

    m_fsModel->setRootPath(listfileDirectory);
    m_fsView->setRootIndex(m_fsModel->index(listfileDirectory));
}

static const QString AnalysisFileFilter = QSL("MVME Analysis Files (*.analysis);; All Files (*.*)");

void ListfileBrowser::onItemDoubleClicked(const QModelIndex &mi)
{
    if (m_context->getConfig()->isModified())
    {
        QMessageBox msgBox(QMessageBox::Question, "Save configuration?",
                           "The current VME configuration has modifications. Do you want to save it?",
                           QMessageBox::Save | QMessageBox::Cancel | QMessageBox::Discard);
        int result = msgBox.exec();

        if (result == QMessageBox::Save)
        {
            if (!m_mainWindow->onActionSaveVMEConfig_triggered())
            {
                return;
            }
        }
        else if (result == QMessageBox::Cancel)
        {
            return;
        }
    }


    u16 flags = m_analysisLoadActionCombo->currentData().toUInt(0);

    if ((flags & OpenListfileFlags::LoadAnalysis) && m_context->getAnalysis()->isModified())
    {
        QMessageBox msgBox(QMessageBox::Question, QSL("Save analysis configuration?"),
                           QSL("The current analysis configuration has modifications. Do you want to save it?"),
                           QMessageBox::Save | QMessageBox::Cancel | QMessageBox::Discard);

        int result = msgBox.exec();

        if (result == QMessageBox::Save)
        {
            auto result = saveAnalysisConfig(
                m_context->getAnalysis(),
                m_context->getAnalysisConfigFileName(),
                m_context->getWorkspaceDirectory(),
                AnalysisFileFilter,
                m_context);

            if (!result.first)
            {
                m_context->logMessage(QSL("Error: ") + result.second);
                return;
            }
        }
        else if (result == QMessageBox::Cancel)
        {
            return;
        }
    }

    auto filename = m_fsModel->filePath(mi);

    try
    {
        auto openResult = open_listfile(m_context, filename, flags);

        if (openResult.listfile)
        {
            m_context->logMessageRaw(QSL(">>>>> Begin listfile log"));
            m_context->logMessageRaw(openResult.messages);
            m_context->logMessageRaw(QSL("<<<<< End listfile log"));
        }
        m_mainWindow->updateWindowTitle();
    }
    catch (const QString &e)
    {
        QMessageBox::critical(this, QSL("Error opening listfile"),
                              QString("Error opening listfile %1: %2")
                              .arg(filename)
                              .arg(e));
    }
}
