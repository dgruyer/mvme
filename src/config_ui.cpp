#include "config_ui.h"
#include "ui_event_config_dialog.h"
#include "ui_datafilter_dialog.h"
#include "mvme_config.h"
#include "mvme_context.h"
#include "vme_script.h"

#include <QMenu>
#include <QStandardPaths>
#include <QSettings>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QThread>
#include <QStandardItemModel>
#include <QCloseEvent>
#include <QScrollBar>
#include <QPushButton>
#include <QJsonObject>
#include <QJsonDocument>

//
// EventConfigDialog
//
EventConfigDialog::EventConfigDialog(MVMEContext *context, EventConfig *config, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::EventConfigDialog)
    , m_context(context)
    , m_config(config)
{
    ui->setupUi(this);
    loadFromConfig();

    auto handleContextStateChange = [this] {
        auto daqState = m_context->getDAQState();
        auto globalMode = m_context->getMode();
        setReadOnly(daqState != DAQState::Idle || globalMode == GlobalMode::ListFile);
    };

    connect(context, &MVMEContext::daqStateChanged, this, handleContextStateChange);
    connect(context, &MVMEContext::modeChanged, this, handleContextStateChange);

    handleContextStateChange();
}

EventConfigDialog::~EventConfigDialog()
{
    delete ui;
}

void EventConfigDialog::loadFromConfig()
{
    auto config = m_config;

    ui->le_name->setText(config->objectName());
    ui->combo_triggerCondition->setCurrentIndex(
        static_cast<int>(config->triggerCondition));

    ui->spin_period->setValue(config->scalerReadoutPeriod * 0.5); // TODO: why 0.5? unit for the readout period?
    ui->spin_frequency->setValue(config->scalerReadoutFrequency);
    ui->spin_irqLevel->setValue(config->irqLevel);
    ui->spin_irqVector->setValue(config->irqVector);
}

void EventConfigDialog::saveToConfig()
{
    auto config = m_config;

    config->setObjectName(ui->le_name->text());
    config->triggerCondition = static_cast<TriggerCondition>(ui->combo_triggerCondition->currentIndex());
    config->scalerReadoutPeriod = static_cast<uint8_t>(ui->spin_period->value() * 2);
    config->scalerReadoutFrequency = static_cast<uint16_t>(ui->spin_frequency->value());
    config->irqLevel = static_cast<uint8_t>(ui->spin_irqLevel->value());
    config->irqVector = static_cast<uint8_t>(ui->spin_irqVector->value());
    config->setModified(true);
}

void EventConfigDialog::accept()
{
    saveToConfig();
    QDialog::accept();
}

void EventConfigDialog::setReadOnly(bool readOnly)
{
    ui->le_name->setEnabled(!readOnly);
    ui->combo_triggerCondition->setEnabled(!readOnly);
    ui->stackedWidget->setEnabled(!readOnly);
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(!readOnly);
}

//
// ModuleConfigDialog
//
ModuleConfigDialog::ModuleConfigDialog(MVMEContext *context, ModuleConfig *module, bool isNewModule, QWidget *parent)
    : QDialog(parent)
    , m_context(context)
    , m_module(module)
{
    typeCombo = new QComboBox;

    int typeComboIndex = 0;

    for (auto type: VMEModuleTypeNames.keys())
    {
        typeCombo->addItem(VMEModuleTypeNames[type], QVariant::fromValue(static_cast<int>(type)));
        if (type == module->type)
            typeComboIndex = typeCombo->count() - 1;
    }

    typeCombo->setCurrentIndex(typeComboIndex);

    nameEdit = new QLineEdit;

    auto onTypeComboIndexChanged = [=](int index)
    {
        auto currentType = static_cast<VMEModuleType>(typeCombo->currentData().toInt());
        QString name = context->getUniqueModuleName(VMEModuleShortNames[currentType]);
        nameEdit->setText(name);
    };

    onTypeComboIndexChanged(typeComboIndex);

    if (!module->objectName().isEmpty())
    {
        nameEdit->setText(module->objectName());
    }

    connect(typeCombo, static_cast<void (QComboBox::*) (int)>(&QComboBox::currentIndexChanged),
            this, onTypeComboIndexChanged);

    typeCombo->setEnabled(isNewModule);

    addressEdit = new QLineEdit;
    addressEdit->setInputMask("\\0\\xHHHHHHHH");
    addressEdit->setText(QString("0x%1").arg(module->getBaseAddress(), 8, 16, QChar('0')));

    auto bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto layout = new QFormLayout(this);
    layout->addRow("Type", typeCombo);
    layout->addRow("Name", nameEdit);
    layout->addRow("Address", addressEdit);
    layout->addRow(bb);

    connect(addressEdit, &QLineEdit::textChanged, [=](const QString &) {
        bb->button(QDialogButtonBox::Ok)->setEnabled(addressEdit->hasAcceptableInput());
    });
}

void ModuleConfigDialog::accept()
{
    bool ok;
    m_module->type = static_cast<VMEModuleType>(typeCombo->currentData().toInt());
    m_module->setObjectName(nameEdit->text());
    m_module->setBaseAddress(addressEdit->text().toUInt(&ok, 16));
    QDialog::accept();
}

#if 0
ModuleConfigWidget::ModuleConfigWidget(MVMEContext *context, ModuleConfig *config, QWidget *parent)
    : MVMEWidget(parent)
    , ui(new Ui::ModuleConfigWidget)
    , m_context(context)
    , m_config(config)
{
    ui->setupUi(this);
    ui->gb_extra->setVisible(false);

    connect(context, &MVMEContext::moduleAboutToBeRemoved, this, [this](ModuleConfig *module) {
        if (module == m_config)
        {
            forceClose();
        }
    });

    auto handleContextStateChange = [this] {
        auto daqState = m_context->getDAQState();
        auto globalMode = m_context->getMode();
        setReadOnly(daqState != DAQState::Idle || globalMode == GlobalMode::ListFile);
    };

    connect(context, &MVMEContext::daqStateChanged, this, handleContextStateChange);
    connect(context, &MVMEContext::modeChanged, this, handleContextStateChange);

    auto model = qobject_cast<QStandardItemModel *>(ui->combo_listType->model());

    // Module initialization
    {
        ui->combo_listType->addItem("----- Initialization -----");
        auto item = model->item(ui->combo_listType->count() - 1);
        item->setFlags(item->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
    }

    ui->combo_listType->addItem("Module Reset", QVariant::fromValue(static_cast<int>(ModuleListType::Reset)));
    ui->combo_listType->addItem("Module Init", QVariant::fromValue(static_cast<int>(ModuleListType::Parameters)));
    ui->combo_listType->addItem("Readout Settings", QVariant::fromValue(static_cast<int>(ModuleListType::Readout)));

    // Readout loop
    {
        ui->combo_listType->addItem("----- Readout -----");
        auto item = model->item(ui->combo_listType->count() - 1);
        item->setFlags(item->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
    }
    ui->combo_listType->addItem("Start DAQ", QVariant::fromValue(static_cast<int>(ModuleListType::StartDAQ)));
    ui->combo_listType->addItem("Readout Stack (VM_USB)", QVariant::fromValue(static_cast<int>(ModuleListType::ReadoutStack)));
    ui->combo_listType->addItem("Stop DAQ", QVariant::fromValue(static_cast<int>(ModuleListType::StopDAQ)));

    ui->combo_listType->setCurrentIndex(2);

    connect(ui->combo_listType, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &ModuleConfigWidget::handleListTypeIndexChanged);

    connect(ui->le_name, &QLineEdit::textChanged, this, [this](const QString &) {
            if (ui->le_name->hasAcceptableInput())
            {
                setModified(true);
            }
    });

    // VME base address
    ui->le_address->setInputMask("\\0\\xHHHHHHHH");

    connect(ui->le_address, &QLineEdit::textChanged, this, [this](const QString &) {
            if (ui->le_address->hasAcceptableInput())
            {
                setModified(true);
            }
    });

    // register list / stack editor
    connect(ui->editor, &QTextEdit::textChanged, this, [this] {
            setModified(true);
    });

    loadFromConfig();

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
    connect(ui->pb_initModule, &QPushButton::clicked, this, &ModuleConfigWidget::initModule);

    ui->splitter->setSizes({1, 0});

    auto controller = m_context->getController();

    auto onControllerOpenChanged = [=] {
        bool open = controller->isOpen();
        ui->pb_exec->setEnabled(open && !m_readOnly);
        ui->pb_initModule->setEnabled(open && !m_readOnly);
        if (open)
        {
            ui->pb_exec->setToolTip(QSL(""));
            ui->pb_initModule->setToolTip(QSL(""));
        }
        else
        {
            ui->pb_exec->setToolTip(QSL("Controller not connected"));
            ui->pb_initModule->setToolTip(QSL("Controller not connected"));
        }
    };

    connect(controller, &VMEController::controllerOpened, this, onControllerOpenChanged);
    connect(controller, &VMEController::controllerClosed, this, onControllerOpenChanged);
    onControllerOpenChanged();
    handleListTypeIndexChanged(0);
    handleContextStateChange();

#if 0
    if (config->type == VMEModuleType::VHS4030p)
    {
        auto button = new QPushButton("VHS4030p Helper");
        ui->gb_extra->setVisible(true);
        auto layout = new QVBoxLayout(ui->gb_extra);
        layout->addWidget(button);

        connect(button, &QPushButton::clicked, this, [this] {
            auto w = new VHS4030pWidget(m_context, m_config, this);
            w->show();
        });
    }
#endif
}

ModuleConfigWidget::~ModuleConfigWidget()
{
    delete ui;
}

void ModuleConfigWidget::handleListTypeIndexChanged(int index)
{
    if (ui->editor->document()->isModified())
    {
        auto lastType = ui->combo_listType->itemData(m_lastListTypeIndex).toInt();
        m_configStrings[lastType] = ui->editor->toPlainText();
        setModified(true);
    }

    m_lastListTypeIndex = index;

    auto currentType = ui->combo_listType->currentData().toInt();
    {
        QSignalBlocker b(ui->editor);
        ui->editor->setPlainText(m_configStrings.value(currentType));
    }
    ui->editor->document()->setModified(false);
    ui->editor->document()->clearUndoRedoStacks();

    switch (static_cast<ModuleListType>(currentType))
    {
        case ModuleListType::ReadoutStack:
            ui->pb_exec->setText("Exec");
            ui->pb_initModule->setVisible(true);
            ui->pb_load->setVisible(false);
            ui->pb_save->setVisible(false);
            ui->splitter->setSizes({1, 1});
            ui->editor->setReadOnly(true);
            break;

        default:
            ui->pb_exec->setText("Run");
            ui->pb_initModule->setVisible(false);
            ui->pb_load->setVisible(true);
            ui->pb_save->setVisible(true);
            ui->splitter->setSizes({1, 0});
            ui->editor->setReadOnly(m_readOnly);
            break;
    }

    if (static_cast<ModuleListType>(currentType) != ModuleListType::ReadoutStack)
    {
        try
        {
            auto script = vme_script::parse(ui->editor->toPlainText());
            for (auto cmd: script.commands)
            {
                qDebug() << to_string(cmd);
            }
        } catch (const vme_script::ParseError &e)
        {
            qDebug() << e.message << e.lineNumber;
        }
    }
}

void ModuleConfigWidget::forceClose()
{
    m_forceClose = true;
    close();
}

void ModuleConfigWidget::closeEvent(QCloseEvent *event)
{
    if (m_forceClose)
    {
        event->accept();
    }
    else if (m_hasModifications)
    {
        auto response = QMessageBox::question(this, QSL("Apply changes"),
                QSL("The module configuration was modified. Do you want to apply the changes?"),
                QMessageBox::Apply | QMessageBox::Discard | QMessageBox::Cancel);

        if (response == QMessageBox::Apply)
        {
            saveToConfig();
            event->accept();
        }
        else if (response == QMessageBox::Discard)
        {
            event->accept();
        }
        else
        {
            event->ignore();
        }
    }
    else
    {
        event->accept();
    }

    if (event->isAccepted())
    {
        emit aboutToClose();
    }
}

void ModuleConfigWidget::loadFromFile()
{
    QString path = QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation).at(0);
    QSettings settings;
    if (settings.contains("Files/LastInitListDirectory"))
    {
        path = settings.value("Files/LastInitListDirectory").toString();
    }

    QString fileName = QFileDialog::getOpenFileName(this, "Load init template", path,
                                                    "Init Lists (*.init);; All Files (*)");
    if (!fileName.isEmpty())
    {
        QFile file(fileName);
        if (file.open(QIODevice::ReadOnly))
        {
            QTextStream stream(&file);
            ui->editor->setPlainText(stream.readAll());
            QFileInfo fi(fileName);
            settings.setValue("Files/LastInitListDirectory", fi.absolutePath());
        }
    }
}

void ModuleConfigWidget::loadFromTemplate()
{
    // TODO: This is duplicated in ModuleConfigDialog::accept(). Compress this!
    QStringList templatePaths;
    templatePaths << QDir::currentPath() + "/templates";
    templatePaths << QCoreApplication::applicationDirPath() + "/templates";

    QString templatePath;

    for (auto testPath: templatePaths)
    {
        if (QFileInfo(testPath).exists())
        {
            templatePath = testPath;
            break;
        }
    }


    if (templatePath.isEmpty())
    {
        QMessageBox::warning(this, QSL("Error"), QSL("No module template directory found."));
        return;
    }

    QString fileName = QFileDialog::getOpenFileName(this, "Load init template", templatePath,
                                                    "Init Lists (*.init);; All Files (*)");

    if (!fileName.isEmpty())
    {
        QFile file(fileName);
        if (file.open(QIODevice::ReadOnly))
        {
            QTextStream stream(&file);
            ui->editor->setPlainText(stream.readAll());
        }
    }
}

void ModuleConfigWidget::saveToFile()
{
    QString path = QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation).at(0);
    QSettings settings;
    if (settings.contains("Files/LastInitListDirectory"))
    {
        path = settings.value("Files/LastInitListDirectory").toString();
    }

    QString fileName = QFileDialog::getSaveFileName(this, "Load init template", path,
                                                    "Init Lists (*.init);; All Files (*)");

    if (fileName.isEmpty())
        return;

    QFileInfo fi(fileName);
    if (fi.completeSuffix().isEmpty())
    {
        fileName += ".init";
    }

    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly))
    {
        QMessageBox::critical(this, "File error", QString("Error opening \"%1\" for writing").arg(fileName));
        return;
    }

    QTextStream stream(&file);
    stream << ui->editor->toPlainText();

    if (stream.status() != QTextStream::Ok)
    {
        QMessageBox::critical(this, "File error", QString("Error writing to \"%1\"").arg(fileName));
        return;
    }

    settings.setValue("Files/LastInitListDirectory", fi.absolutePath());
}

void ModuleConfigWidget::execList()
{
    auto controller = m_context->getController();

    if (controller && controller->isOpen())
    {
        auto type = static_cast<ModuleListType>(ui->combo_listType->currentData().toInt());
        QString listContents = ui->editor->toPlainText();

        Q_ASSERT(!"not implemented");

#if 0
        switch (type)
        {
            case ModuleListType::Parameters:
            case ModuleListType::Readout:
            case ModuleListType::StartDAQ:
            case ModuleListType::StopDAQ:
            case ModuleListType::Reset:
                {
                    auto regs = parseRegisterList(listContents, m_config->baseAddress);

                    //emit logMessage(QString("%1.%2").arg(m_config->

                    static const int writeDelay_ms = 10;
                    auto result = controller->applyRegisterList(regs, 0, writeDelay_ms,
                                                           m_config->getRegisterWidth(),
                                                           m_config->getRegisterAddressModifier());

                    if (result < 0)
                    {
                        QMessageBox::warning(this,
                                             "Error running commands",
                                             QString("Error running commands (code=%1")
                                             .arg(result));
                    }
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
            case ModuleListType::TypeCount:
                break;
        }
#endif
    }
}

void ModuleConfigWidget::initModule()
{
    RegisterList regs = parseRegisterList(m_configStrings[(int)ModuleListType::Reset]);
    auto controller = m_context->getController();
    controller->applyRegisterList(regs, m_config->baseAddress);
    QThread::msleep(500);

    regs = parseRegisterList(m_configStrings[(int)ModuleListType::Parameters]);
    regs += parseRegisterList(m_configStrings[(int)ModuleListType::Readout]);
    regs += parseRegisterList(m_configStrings[(int)ModuleListType::StartDAQ]);
    controller->applyRegisterList(regs, m_config->baseAddress);
}

void ModuleConfigWidget::setModified(bool modified)
{
    m_hasModifications = modified;
    ui->buttonBox->button(QDialogButtonBox::Reset)->setEnabled(modified);
    ui->buttonBox->button(QDialogButtonBox::Apply)->setEnabled(modified && !m_readOnly);
}

void ModuleConfigWidget::on_buttonBox_clicked(QAbstractButton *button)
{
    auto buttonRole = ui->buttonBox->buttonRole(button);

    switch (buttonRole)
    {
        case QDialogButtonBox::ApplyRole:
            {
                saveToConfig();
                loadFromConfig();
                setWindowTitle(QString("Module Config %1").arg(m_config->objectName()));
            } break;

        case QDialogButtonBox::ResetRole:
            {
                loadFromConfig();
            } break;

        case QDialogButtonBox::RejectRole:
            {
                close();
            } break;

        default:
            Q_ASSERT(false);
            break;
    }
}

void ModuleConfigWidget::loadFromConfig()
{
    auto config = m_config;

    setWindowTitle(QString("Module Config %1").arg(config->objectName()));
    ui->label_type->setText(VMEModuleTypeNames.value(config->type, QSL("Unknown")));
    ui->le_name->setText(config->objectName());
    ui->le_address->setText(QString().sprintf("0x%08x", config->baseAddress));

    m_configStrings.clear();

    for (int i=0; i < static_cast<int>(ModuleListType::TypeCount); ++i)
    {
        m_configStrings[i] = *getConfigString(static_cast<ModuleListType>(i), config);
    }

    auto currentType = ui->combo_listType->currentData().toInt();

    int scrollValue = ui->editor->verticalScrollBar()->value();
    ui->editor->setPlainText(m_configStrings.value(currentType));
    ui->editor->verticalScrollBar()->setValue(scrollValue);

    ui->editor->document()->setModified(false);
    ui->editor->document()->clearUndoRedoStacks();
    setModified(false);
}

void ModuleConfigWidget::saveToConfig()
{
    auto config = m_config;

    config->setObjectName(ui->le_name->text());

    bool ok;
    m_config->baseAddress = ui->le_address->text().toUInt(&ok, 16);

    auto currentType = ui->combo_listType->currentData().toInt();
    m_configStrings[currentType] = ui->editor->toPlainText();

    for (int i=0; i < static_cast<int>(ModuleListType::TypeCount); ++i)
    {
        *getConfigString(static_cast<ModuleListType>(i), config) = m_configStrings[i];
    }
    m_config->setModified(true);
    setModified(false);
}

void ModuleConfigWidget::setReadOnly(bool readOnly)
{
    m_readOnly = readOnly;
    ui->le_name->setEnabled(!readOnly);
    ui->le_address->setEnabled(!readOnly);
    ui->pb_exec->setEnabled(!readOnly);
    ui->pb_initModule->setEnabled(!readOnly);
    ui->pb_load->setEnabled(!readOnly);
    ui->buttonBox->button(QDialogButtonBox::Apply)->setEnabled(m_hasModifications && !readOnly);

    auto currentType = ui->combo_listType->currentData().toInt();

    switch (static_cast<ModuleListType>(currentType))
    {
        case ModuleListType::ReadoutStack:
            ui->editor->setReadOnly(true);
            break;

        default:
            ui->editor->setReadOnly(readOnly);
            break;
    }
}

MVMEWidget *makeModuleConfigWidget(MVMEContext *context, ModuleConfig *config, QWidget *parent)
{
    //if (config->type == VMEModuleType::VHS4030p)
    //{
    //    return new VHS4030pWidget(context, config, parent);
    //}

    return new ModuleConfigWidget(context, config, parent);
}

#if 0
VHS4030pWidget::VHS4030pWidget(MVMEContext *context, ModuleConfig *config, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::VHS4030pWidget)
    , m_context(context)
    , m_config(config)
{
    ui->setupUi(this);

    connect(ui->pb_write, &QPushButton::clicked, this, [this] {
        bool ok;
        u32 offset = ui->le_offset->text().toUInt(&ok, 16);
        u32 value  = ui->le_value->text().toUInt(&ok, 16);
        auto ctrl = m_context->getController();

        ctrl->write16(m_config->baseAddress + offset, value, m_config->getRegisterAddressModifier());
    });

    connect(ui->pb_read, &QPushButton::clicked, this, [this] {
        bool ok;
        u32 offset = ui->le_offset->text().toUInt(&ok, 16);
        auto ctrl = m_context->getController();
        u16 value = 0;
        u32 address = m_config->baseAddress + offset;

        int result = ctrl->read16(address, &value, m_config->getRegisterAddressModifier());

        qDebug("read16: addr=%08x, value=%04x, result=%d", address, value, result);

        ui->le_value->setText(QString("0x%1")
                              .arg(static_cast<u32>(value), 4, 16, QLatin1Char('0')));
    });
}
#endif

#endif

//
// DataFilterDialog
//
DataFilterDialog::DataFilterDialog(DataFilterConfig *config, const QString &defaultFilter, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::DataFilterDialog)
    , m_config(config)
{
    ui->setupUi(this);

    QFont font("MonoSpace");
    font.setStyleHint(QFont::Monospace);

    QFontMetrics metrics(font);
    int width = metrics.width(ui->le_filter->inputMask());

    ui->le_filterKey->setFont(font);
    ui->le_filterKey->setMinimumWidth(width);
    ui->le_filter->setFont(font);
    ui->le_filter->setMinimumWidth(width);

    ui->le_filterKey->setText(defaultFilter);

    connect(ui->le_filter, &QLineEdit::textChanged, this, &DataFilterDialog::validate);
    connect(ui->le_name, &QLineEdit::textChanged, this, &DataFilterDialog::validate);
    connect(ui->le_filter, &QLineEdit::textChanged, this, &DataFilterDialog::updateUnitLimits);

    loadFromConfig();
    validate();
}

DataFilterDialog::~DataFilterDialog()
{
    delete ui;
}

void DataFilterDialog::accept()
{
    saveToConfig();
    QDialog::accept();
}

void DataFilterDialog::loadFromConfig()
{
    ui->le_name->setText(m_config->objectName());
    ui->le_filter->setText(QString::fromLocal8Bit(m_config->getFilter().getFilter()));

    ui->le_axisTitle->setText(m_config->getAxisTitle());
    ui->le_axisUnit->setText(m_config->getUnitString());

    auto minValue = m_config->getBaseUnitRange().first;
    auto maxValue = m_config->getBaseUnitRange().second;

    ui->spin_rangeMin->setValue(minValue);
    ui->spin_rangeMax->setValue(maxValue);

    if (std::abs(maxValue - minValue) == 0.0)
        updateUnitLimits();
}

// Converts input to 8 bit, removes spaces, creates filter.
DataFilter makeFilterFromString(const QString &str)
{
    auto filterDataRaw = str.toLocal8Bit();

    QByteArray filterData;

    for (auto c: filterDataRaw)
    {
        if (c != ' ')
            filterData.push_back(c);
    }

    return DataFilter(filterData);
}

void DataFilterDialog::saveToConfig()
{
    m_config->setObjectName(ui->le_name->text());
    m_config->setFilter(makeFilterFromString(ui->le_filter->text()));
    m_config->setAxisTitle(ui->le_axisTitle->text());
    m_config->setUnitString(ui->le_axisUnit->text());

    double unitMin = ui->spin_rangeMin->value();
    double unitMax = ui->spin_rangeMax->value();

    m_config->setBaseUnitRange(unitMin, unitMax);

    for (u32 addr = 0; addr < m_config->getAddressCount(); ++addr)
    {
        m_config->setUnitRange(addr, unitMin, unitMax);
    }
}

void DataFilterDialog::validate()
{
    bool isValid = ui->le_filter->hasAcceptableInput()
        && !ui->le_name->text().isEmpty();
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(isValid);
}

void DataFilterDialog::updateUnitLimits()
{
    try
    {
        auto filter = makeFilterFromString(ui->le_filter->text());
        auto dataBits = filter.getExtractBits('D');
        ui->spin_rangeMin->setValue(0.0);
        ui->spin_rangeMax->setValue((1ull << dataBits) - 1);
    }
    catch (const std::string &)
    {}
}

namespace
{
    bool saveAnalysisConfigImpl(AnalysisConfig *config, const QString &fileName)
    {
        QJsonObject json, configJson;
        config->write(configJson);
        json[QSL("AnalysisConfig")] = configJson;
        return gui_write_json_file(fileName, QJsonDocument(json));
    }

    static const QString fileFilter = QSL("Config Files (*.json);; All Files (*.*)");
    static const QString settingsPath = QSL("Files/LastAnalysisConfig");
}

bool saveAnalysisConfig(AnalysisConfig *config, const QString &fileName)
{
    if (fileName.isEmpty())
        return saveAnalysisConfigAs(config).first;

    return saveAnalysisConfigImpl(config, fileName);
}

QPair<bool, QString> saveAnalysisConfigAs(AnalysisConfig *config)
{
    QString path = QFileInfo(QSettings().value(settingsPath).toString()).absolutePath();

    if (path.isEmpty())
        path = QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation).at(0);

    path += QSL("/analysis.json");

    QString fileName = QFileDialog::getSaveFileName(nullptr, QSL("Save analysis config"), path, fileFilter);

    if (fileName.isEmpty())
        return qMakePair(false, QString());

    QFileInfo fi(fileName);
    if (fi.completeSuffix().isEmpty())
        fileName += QSL(".json");

    if (saveAnalysisConfigImpl(config, fileName))
    {
        QSettings().setValue(settingsPath, fileName);
        return qMakePair(true, fileName);
    }

    return qMakePair(false, QString());
}
