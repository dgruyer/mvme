#include "vme_debug_widget.h"
#include "ui_vme_debug_widget.h"
#include "mvme_context.h"
#include "vme_controller.h"

#include <QFileDialog>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QMessageBox>

static const int tabStop = 4;
static const QString scriptFileSetting = QSL("Files/LastDebugScriptDirectory");

using namespace std::placeholders;

VMEDebugWidget::VMEDebugWidget(MVMEContext *context, QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::VMEDebugWidget)
    , m_context(context)
{
    ui->setupUi(this);

    new vme_script::SyntaxHighlighter(ui->scriptInput);
    {
        QString spaces;
        for (int i = 0; i < tabStop; ++i)
            spaces += " ";
        QFontMetrics metrics(ui->scriptInput->font());
        ui->scriptInput->setTabStopWidth(metrics.width(spaces));
    }
}

VMEDebugWidget::~VMEDebugWidget()
{
    delete ui;
}

void VMEDebugWidget::on_writeLoop1_toggled(bool checked)
{
    if (checked)
        on_writeWrite1_clicked();
}

void VMEDebugWidget::on_writeLoop2_toggled(bool checked)
{
    if (checked)
        on_writeWrite2_clicked();
}

void VMEDebugWidget::on_writeLoop3_toggled(bool checked)
{
    if (checked)
        on_writeWrite3_clicked();
}

void VMEDebugWidget::on_writeWrite1_clicked()
{
    bool ok;
    u32 offset = ui->writeOffset1->text().toUInt(&ok, 0);
    u32 address = ui->writeAddress1->text().toUInt(&ok, 0);
    u32 value = ui->writeValue1->text().toUInt(&ok, 0);
    address += (offset << 16);

    doWrite(address, value);

    if (ui->writeLoop1->isChecked())
        QTimer::singleShot(100, this, &VMEDebugWidget::on_writeWrite1_clicked);
}

void VMEDebugWidget::on_writeWrite2_clicked()
{
    bool ok;
    u32 offset = ui->writeOffset2->text().toUInt(&ok, 0);
    u32 address = ui->writeAddress2->text().toUInt(&ok, 0);
    u32 value = ui->writeValue2->text().toUInt(&ok, 0);
    address += (offset << 16);

    doWrite(address, value);

    if (ui->writeLoop2->isChecked())
        QTimer::singleShot(100, this, &VMEDebugWidget::on_writeWrite2_clicked);
}

void VMEDebugWidget::on_writeWrite3_clicked()
{
    bool ok;
    u32 offset = ui->writeOffset3->text().toUInt(&ok, 0);
    u32 address = ui->writeAddress3->text().toUInt(&ok, 0);
    u32 value = ui->writeValue3->text().toUInt(&ok, 0);
    address += (offset << 16);

    doWrite(address, value);

    if (ui->writeLoop3->isChecked())
        QTimer::singleShot(100, this, &VMEDebugWidget::on_writeWrite3_clicked);
}

void VMEDebugWidget::on_readLoop1_toggled(bool checked)
{
    if (checked)
        on_readRead1_clicked();
}

void VMEDebugWidget::on_readLoop2_toggled(bool checked)
{
    if (checked)
        on_readRead2_clicked();
}

void VMEDebugWidget::on_readLoop3_toggled(bool checked)
{
    if (checked)
        on_readRead3_clicked();
}

void VMEDebugWidget::on_readRead1_clicked()
{
    bool ok;
    u32 offset = ui->readOffset1->text().toUInt(&ok, 0);
    u32 address = ui->readAddress1->text().toUInt(&ok, 0);
    address += (offset << 16);

    ui->bltResult->clear();
    ui->readResult1->clear();

    if (ui->readModeSingle->isChecked())
    {
        u16 value = doRead(address);

        ui->readResult1->setText(QString("0x%1")
                                 .arg(value, 4, 16, QChar('0'))
                                );
    } else
    {
        QVector<u32> result;
        u32 transfers = static_cast<u32>(ui->blockReadCount->value());
        u8 amod = ui->readModeBLT->isChecked()
            ?  vme_address_modes::a32UserBlock
            : vme_address_modes::a32UserBlock64;

        VMEError vmeError = m_context->getController()->blockRead(address, transfers, &result, amod, true);

        m_context->logMessage(QString("VME Debug: block read 0x%1, vmeError=%2")
                              .arg(address, 8, 16, QChar('0'))
                              .arg(vmeError.toString())
                             );

        QString buffer;
        for (int i=0; i<result.size(); ++i)
        {
            buffer += QString(QSL("%1: 0x%2\n"))
                .arg(i, 2, 10, QChar(' '))
                .arg(result[i], 8, 16, QChar('0'));
        }
        ui->bltResult->setText(buffer);
    }

    if (ui->readLoop1->isChecked())
        QTimer::singleShot(100, this, &VMEDebugWidget::on_readRead1_clicked);
}

void VMEDebugWidget::on_readRead2_clicked()
{
    bool ok;
    u32 offset = ui->readOffset2->text().toUInt(&ok, 0);
    u32 address = ui->readAddress2->text().toUInt(&ok, 0);
    address += (offset << 16);

    u16 value = doRead(address);

    ui->readResult2->setText(QString("0x%1")
                             .arg(value, 4, 16, QChar('0'))
                            );

    if (ui->readLoop2->isChecked())
        QTimer::singleShot(100, this, &VMEDebugWidget::on_readRead2_clicked);
}

void VMEDebugWidget::on_readRead3_clicked()
{
    bool ok;
    u32 offset = ui->readOffset3->text().toUInt(&ok, 0);
    u32 address = ui->readAddress3->text().toUInt(&ok, 0);
    address += (offset << 16);

    u16 value = doRead(address);

    ui->readResult3->setText(QString("0x%1")
                             .arg(value, 4, 16, QChar('0'))
                            );

    if (ui->readLoop3->isChecked())
        QTimer::singleShot(100, this, &VMEDebugWidget::on_readRead3_clicked);
}

void VMEDebugWidget::doWrite(u32 address, u32 value)
{
    auto vmeError = m_context->getController()->write16(address, value, vme_address_modes::a32UserData);

    m_context->logMessage(QString("VME Debug: write 0x%1 -> 0x%2, vmeError=%3")
                          .arg(address, 8, 16, QChar('0'))
                          .arg(value, 4, 16, QChar('0'))
                          .arg(vmeError.toString())
                         );
}

u16 VMEDebugWidget::doRead(u32 address)
{
    u16 value = 0;
    auto vmeError = m_context->getController()->read16(address, &value, vme_address_modes::a32UserData);

    m_context->logMessage(QString("VME Debug: read 0x%1 -> 0x%2, vmeError=%3")
                          .arg(address, 8, 16, QChar('0'))
                          .arg(value, 4, 16, QChar('0'))
                          .arg(vmeError.toString())
                         );

    return value;
}

void VMEDebugWidget::on_runScript_clicked()
{
    ui->scriptOutput->clear();
    //auto logger = std::bind(&QTextEdit::append, ui->scriptOutput, _1);
    auto logger = std::bind(&MVMEContext::logMessage, m_context, _1);
    try
    {
        bool ok;
        auto baseAddress = ui->scriptOffset->text().toUInt(&ok, 0);
        baseAddress <<= 16;

        auto script = vme_script::parse(ui->scriptInput->toPlainText(), baseAddress);
        auto resultList = vme_script::run_script(m_context->getController(), script, logger);

        for (auto result: resultList)
        {
            QString str = format_result(result);
            if (!str.isEmpty())
                ui->scriptOutput->append(str);
        }
    }
    catch (const vme_script::ParseError &e)
    {
        logger(QSL("Parse error: ") + e.what());
    }
}

void VMEDebugWidget::on_saveScript_clicked()
{
    QString path = QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation).at(0);
    QSettings settings;
    if (settings.contains(scriptFileSetting))
    {
        path = settings.value(scriptFileSetting).toString();
    }

    QString fileName = QFileDialog::getSaveFileName(this, QSL("Save vme script"), path,
                                                    QSL("VME scripts (*.vme);; All Files (*)"));

    if (fileName.isEmpty())
        return;

    QFileInfo fi(fileName);
    if (fi.completeSuffix().isEmpty())
    {
        fileName += ".vme";
    }

    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly))
    {
        QMessageBox::critical(this, "File error", QString("Error opening \"%1\" for writing").arg(fileName));
        return;
    }

    QTextStream stream(&file);
    stream << ui->scriptInput->toPlainText();

    if (stream.status() != QTextStream::Ok)
    {
        QMessageBox::critical(this, "File error", QString("Error writing to \"%1\"").arg(fileName));
        return;
    }

    settings.setValue(scriptFileSetting, fi.absolutePath());
}

void VMEDebugWidget::on_loadScript_clicked()
{
    QString path = QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation).at(0);
    QSettings settings;
    if (settings.contains(scriptFileSetting))
    {
        path = settings.value(scriptFileSetting).toString();
    }

    QString fileName = QFileDialog::getOpenFileName(this, QSL("Load vme script file"), path,
                                                    QSL("VME scripts (*.vme);; All Files (*)"));
    if (!fileName.isEmpty())
    {
        QFile file(fileName);
        if (file.open(QIODevice::ReadOnly))
        {
            QTextStream stream(&file);
            ui->scriptInput->setPlainText(stream.readAll());
            QFileInfo fi(fileName);
            settings.setValue(scriptFileSetting, fi.absolutePath());
            ui->scriptOutput->clear();
        }
    }
}
