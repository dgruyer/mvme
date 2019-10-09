#include "mvlc/mvlc_trigger_io_editor.h"
#include "mvlc/mvlc_trigger_io_editor_p.h"
#include "mvlc/mvlc_trigger_io_script.h"

#include <QDebug>
#include <QPushButton>
#include <QScrollBar>
#include <QTextEdit>

#include "util/algo.h"
#include "util/qt_container.h"
#include "vme_script_editor.h"

namespace mesytec
{

using namespace mvlc;
using namespace mvlc::trigger_io;
using namespace mvlc::trigger_io_config;

struct MVLCTriggerIOEditor::Private
{
    TriggerIO ioCfg;
    VMEScriptConfig *scriptConfig;
    QString initialScriptContents;
    // TODO: use a CodeEditor, not the VMEScriptEditor. The latter is too
    // specialized and not really what we want in here.
    VMEScriptEditor *scriptEditor = nullptr;
    TriggerIOGraphicsScene *scene = nullptr;
};

MVLCTriggerIOEditor::MVLCTriggerIOEditor(VMEScriptConfig *scriptConfig, QWidget *parent)
    : QWidget(parent)
    , d(std::make_unique<Private>())
{
    d->ioCfg = parse_trigger_io_script_text(scriptConfig->getScriptContents());
    d->scriptConfig = scriptConfig;
    d->initialScriptContents = scriptConfig->getScriptContents();

    auto scene = new TriggerIOGraphicsScene(d->ioCfg);
    d->scene = scene;

    // Edit LUT
    QObject::connect(scene, &TriggerIOGraphicsScene::editLUT,
                     [this] (int level, int unit)
    {
        auto &ioCfg = d->ioCfg;
        auto lutName = QString("L%1.LUT%2").arg(level).arg(unit);
        QVector<QStringList> inputNameLists;
        QStringList strobeInputChoiceNames;
        unsigned strobeConValue = 0u;
        trigger_io::IO strobeGGSettings = {};
        std::bitset<trigger_io::LUT::OutputBits> strobedOutputs;

        // specific handling for Level1
        if (level == 1
            && 0 <= unit
            && unit < static_cast<int>(Level1::StaticConnections.size()))
        {
            const auto &connections = Level1::StaticConnections[unit];

            for (auto address: connections)
            {
                // handles static Level1 -> Level0 connections
                if (address[0] == 0)
                {
                    inputNameLists.push_back(
                        {ioCfg.l0.unitNames.value(address[1])});
                }
                // handles internal Level1 connections
                else if (address[0] == 1)
                {
                    inputNameLists.push_back(
                        {ioCfg.l1.luts[address[1]].outputNames[address[2]]});
                }
            }
        }
        // Level2
        else if (level == 2
            && 0 <= unit
            && unit < static_cast<int>(Level2::StaticConnections.size()))
        {
            const auto &connections = Level2::StaticConnections[unit];
            const auto &l2InputChoices = Level2::DynamicInputChoices[unit];

            for (size_t inputIndex = 0; inputIndex < connections.size(); inputIndex++)
            {
                auto &con = connections[inputIndex];

                if (!con.isDynamic)
                {
                    auto name = lookup_name(ioCfg, con.address);
                    inputNameLists.push_back({name});
                }
                else if (inputIndex < l2InputChoices.lutChoices.size())
                {
                    auto choices = l2InputChoices.lutChoices[inputIndex];

                    QStringList choiceNames;

                    for (auto &address: choices)
                    {
                        choiceNames.push_back(lookup_name(ioCfg, address));
                    }

                    inputNameLists.push_back(choiceNames);
                }
            }

            for (const auto &address: l2InputChoices.strobeChoices)
                strobeInputChoiceNames.push_back(lookup_name(ioCfg, address));

            strobeConValue = ioCfg.l2.strobeConnections[unit];
            strobeGGSettings = ioCfg.l2.luts[unit].strobeGG;
            copy_bitset(ioCfg.l2.luts[unit].strobedOutputs, strobedOutputs);
        }


        QStringList outputNames;

        // Level1
        if (level == 1
            && 0 <= unit
            && unit < static_cast<int>(ioCfg.l1.luts.size()))
        {
            outputNames = to_qstrlist(ioCfg.l1.luts[unit].outputNames);
        }
        else if (level == 2
                 && 0 <= unit
                 && unit < static_cast<int>(ioCfg.l2.luts.size()))
        {
            outputNames = to_qstrlist(ioCfg.l2.luts[unit].outputNames);
        }

        // run the editor dialog
        std::unique_ptr<LUTEditor> lutEditor;

        if (level == 1)
        {
            lutEditor = std::make_unique<LUTEditor>(
                lutName,
                ioCfg.l1.luts[unit],
                inputNameLists, outputNames);
            lutEditor->resize(850, 650);
        }
        else if (level == 2)
        {
            lutEditor = std::make_unique<LUTEditor>(
                lutName,
                ioCfg.l2.luts[unit],
                inputNameLists,
                ioCfg.l2.lutConnections[unit],
                outputNames,
                strobeInputChoiceNames,
                strobeConValue,
                strobeGGSettings,
                strobedOutputs);
            lutEditor->resize(850, 750);
        }

        assert(lutEditor);

        connect(lutEditor.get(), &LUTEditor::outputNameEdited,
                this, [] (int outputIndex, const QString &outputName)
                {
                    qDebug() << "LUT output name edited:" << outputIndex << outputName;
                });

        auto dc = lutEditor->exec();

        // apply changes
        if (dc == QDialog::Accepted)
        {
            auto outputNames = lutEditor->getOutputNames();
            LUT *lut = nullptr;

            if (level == 1)
                lut = &ioCfg.l1.luts[unit];
            else if (level == 2)
                lut = &ioCfg.l2.luts[unit];

            size_t count = std::min(lut->outputNames.size(),
                                    static_cast<size_t>(outputNames.size()));

            std::copy_n(outputNames.begin(), count, lut->outputNames.begin());

            lut->lutContents = lutEditor->getLUTContents();

            if (level == 2)
            {
                ioCfg.l2.lutConnections[unit] = lutEditor->getDynamicConnectionValues();
                ioCfg.l2.strobeConnections[unit] = lutEditor->getStrobeConnectionValue();
                ioCfg.l2.luts[unit].strobeGG = lutEditor->getStrobeSettings();
                ioCfg.l2.luts[unit].strobedOutputs = lutEditor->getStrobedOutputMask();
            }

            configModified();
        }
    });

    // NIM IO Setup
    QObject::connect(scene, &TriggerIOGraphicsScene::editNIM_Inputs,
                     [this] ()
    {
        auto &ioCfg = d->ioCfg;

        // read names stored in the Level0 structure
        QStringList names;

        std::copy_n(ioCfg.l0.unitNames.begin() + ioCfg.l0.NIM_IO_Offset,
                    trigger_io::NIM_IO_Count,
                    std::back_inserter(names));

        // settings stored in Level0
        QVector<trigger_io::IO> settings;
        std::copy(ioCfg.l0.ioNIM.begin(), ioCfg.l0.ioNIM.end(), std::back_inserter(settings));

        NIM_IO_SettingsDialog dialog(names, settings);
        auto dc = dialog.exec();

        if (dc == QDialog::Accepted)
        {
            names = dialog.getNames();

            // Copy names to L0
            std::copy_n(names.begin(),
                        trigger_io::NIM_IO_Count,
                        ioCfg.l0.unitNames.begin() + ioCfg.l0.NIM_IO_Offset);

            settings = dialog.getSettings();
            size_t count = std::min(static_cast<size_t>(settings.size()), ioCfg.l0.ioNIM.size());

            // Copy settings to L0 and L3
            std::copy_n(settings.begin(), count, ioCfg.l0.ioNIM.begin());
            std::copy(ioCfg.l0.ioNIM.begin(), ioCfg.l0.ioNIM.end(), ioCfg.l3.ioNIM.begin());

            configModified();
        }
    });

    QObject::connect(scene, &TriggerIOGraphicsScene::editNIM_Outputs,
                     [this] ()
    {
        auto &ioCfg = d->ioCfg;

        // read names stored in the Level0 structure
         QStringList names;

         std::copy_n(ioCfg.l0.unitNames.begin() + ioCfg.l0.NIM_IO_Offset,
                     trigger_io::NIM_IO_Count,
                     std::back_inserter(names));

         // settings stored in Level3
         QVector<trigger_io::IO> settings;
         std::copy(ioCfg.l3.ioNIM.begin(), ioCfg.l3.ioNIM.end(),
                   std::back_inserter(settings));

         // build a vector of available input names for each NIM IO
         QVector<QStringList> inputChoiceNameLists;

         for (size_t io = 0; io < trigger_io::NIM_IO_Count; io++)
         {
             int idx = io + trigger_io::Level3::NIM_IO_Unit_Offset;
             const auto &choiceList = ioCfg.l3.DynamicInputChoiceLists[idx];

             QStringList nameList;

             for (const auto &address: choiceList)
                 nameList.push_back(lookup_name(ioCfg, address));

             inputChoiceNameLists.push_back(nameList);
         }

         auto connections = to_qvector(
             ioCfg.l3.connections.begin() + ioCfg.l3.NIM_IO_Unit_Offset,
             ioCfg.l3.connections.begin() + ioCfg.l3.NIM_IO_Unit_Offset + trigger_io::NIM_IO_Count);

         NIM_IO_SettingsDialog dialog(names, settings, inputChoiceNameLists, connections);
         auto dc = dialog.exec();

         if (dc == QDialog::Accepted)
         {
             names = dialog.getNames();

             // Copy names to L0
             std::copy_n(names.begin(),
                         trigger_io::NIM_IO_Count,
                         ioCfg.l0.unitNames.begin() + ioCfg.l0.NIM_IO_Offset);

             // Copy names to L3
             std::copy_n(names.begin(),
                         trigger_io::NIM_IO_Count,
                         ioCfg.l3.unitNames.begin() + ioCfg.l3.NIM_IO_Unit_Offset);

             settings = dialog.getSettings();
             {
                 size_t count = std::min(static_cast<size_t>(settings.size()),
                                         ioCfg.l0.ioNIM.size());

                 // Copy settings to L0 and L3
                 std::copy_n(settings.begin(), count, ioCfg.l0.ioNIM.begin());
                 std::copy(ioCfg.l0.ioNIM.begin(), ioCfg.l0.ioNIM.end(), ioCfg.l3.ioNIM.begin());
             }

             {
                 auto connections = dialog.getConnections();
                 auto count = std::min(static_cast<size_t>(connections.size()),
                                       trigger_io::NIM_IO_Count);
                 std::copy_n(
                     connections.begin(), count,
                     ioCfg.l3.connections.begin() + ioCfg.l3.NIM_IO_Unit_Offset);
             }

            configModified();
         }
    });

    QObject::connect(scene, &TriggerIOGraphicsScene::editECL_Outputs,
                     [this] ()
    {
        auto &ioCfg = d->ioCfg;

        QStringList names;

        std::copy_n(ioCfg.l3.unitNames.begin() + ioCfg.l3.ECL_Unit_Offset,
                    trigger_io::ECL_OUT_Count,
                    std::back_inserter(names));

        // settings stored in Level3
        QVector<trigger_io::IO> settings;
        std::copy(ioCfg.l3.ioECL.begin(), ioCfg.l3.ioECL.end(),
                  std::back_inserter(settings));

        // build a vector of available input names for each ECL IO
        QVector<QStringList> inputChoiceNameLists;

        for (size_t io = 0; io < trigger_io::ECL_OUT_Count; io++)
        {
            int idx = io + trigger_io::Level3::ECL_Unit_Offset;
            const auto &choiceList = ioCfg.l3.DynamicInputChoiceLists[idx];

            QStringList nameList;

            for (const auto &address: choiceList)
                nameList.push_back(lookup_name(ioCfg, address));

            inputChoiceNameLists.push_back(nameList);
        }

        auto connections = to_qvector(
            ioCfg.l3.connections.begin() + ioCfg.l3.ECL_Unit_Offset,
            ioCfg.l3.connections.begin() + ioCfg.l3.ECL_Unit_Offset + trigger_io::ECL_OUT_Count);

        ECL_SettingsDialog dialog(names, settings, connections, inputChoiceNameLists);
        auto dc = dialog.exec();

        if (dc == QDialog::Accepted)
        {
             names = dialog.getNames();

             // Copy names to L3
             std::copy_n(names.begin(),
                         trigger_io::ECL_OUT_Count,
                         ioCfg.l3.unitNames.begin() + ioCfg.l3.ECL_Unit_Offset);

             settings = dialog.getSettings();
             {
                 size_t count = std::min(static_cast<size_t>(settings.size()),
                                         ioCfg.l3.ioECL.size());

                 // Copy settings to L3
                 std::copy_n(settings.begin(), count, ioCfg.l3.ioECL.begin());
             }

             {
                 auto connections = dialog.getConnections();
                 auto count = std::min(static_cast<size_t>(connections.size()),
                                       trigger_io::ECL_OUT_Count);
                 std::copy_n(
                     connections.begin(), count,
                     ioCfg.l3.connections.begin() + ioCfg.l3.ECL_Unit_Offset);
             }

            configModified();
        }
    });

    QObject::connect(scene, &TriggerIOGraphicsScene::editL3Utils,
                     [this] ()
    {
        auto &ioCfg = d->ioCfg;

        QVector<QStringList> inputChoiceNameLists;

        for (int unit = 0; unit < ioCfg.l3.unitNames.size(); unit++)
        {
            const auto &choiceList = ioCfg.l3.DynamicInputChoiceLists[unit];
            QStringList nameList;

            for (const auto &address: choiceList)
                nameList.push_back(lookup_name(ioCfg, address));

            inputChoiceNameLists.push_back(nameList);
        }

        Level3UtilsDialog dialog(ioCfg.l3, inputChoiceNameLists);
        auto dc = dialog.exec();

        if (dc == QDialog::Accepted)
        {
            ioCfg.l3 = dialog.getSettings();
            configModified();
        }
    });

    QObject::connect(scene, &TriggerIOGraphicsScene::editL0Utils,
                     [this] ()
    {
        auto &ioCfg = d->ioCfg;

        Level0UtilsDialog dialog(ioCfg.l0);
        auto dc = dialog.exec();
        if (dc == QDialog::Accepted)
        {
            ioCfg.l0 = dialog.getSettings();
            configModified();
        }
    });

    auto view = new TriggerIOView(scene);

    view->setRenderHints(
        QPainter::Antialiasing | QPainter::TextAntialiasing |
        QPainter::SmoothPixmapTransform |
        QPainter::HighQualityAntialiasing);

    auto pb_clearConfig = new QPushButton("Clear Config");

    auto logicWidget = new QWidget;
    auto logicLayout = make_vbox<0, 0>(logicWidget);
    logicLayout->addWidget(view, 1);

    auto toolbar = make_toolbar();
    toolbar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);

    QAction *action = nullptr;

    action = toolbar->addAction(
        QIcon(":/script-run.png"), QSL("Run"),
        this,  &MVLCTriggerIOEditor::runScript_);

    action = toolbar->addAction(
        QIcon(":/document-open.png"), QSL("Load from file"));
    action->setEnabled(false);

    action = toolbar->addAction(
        QIcon(":/document-save-as.png"), QSL("Save to file"));
    action->setEnabled(false);

    toolbar->addSeparator();

    action = toolbar->addAction(
        QIcon(":/document-new.png"), QSL("Clear Setup"),
        this, [this] ()
        {
            d->ioCfg = {};
            configModified();
        });

    action = toolbar->addAction(
        QIcon(":/document-revert.png"), QSL("Revert to original state"),
        this, [this] ()
        {
            d->scriptConfig->setScriptContents(d->initialScriptContents);
            d->scriptConfig->setModified(false);
            d->ioCfg = parse_trigger_io_script_text(d->scriptConfig->getScriptContents());
            configModified();
        });

    action = toolbar->addAction(
        QIcon(":/dialog-close.png"), QSL("Close window"),
        this, &MVLCTriggerIOEditor::close);

    toolbar->addSeparator();

    action = toolbar->addAction(
        QIcon(":/arrow-circle-double.png"), QSL("Reparse from script"),
        this, [this] ()
        {
            d->ioCfg = parse_trigger_io_script_text(d->scriptConfig->getScriptContents());
        });

    action = toolbar->addAction(
        QIcon(":/vme_script.png"), QSL("View Script (readonly!)"),
        this, [this] ()
        {
            // FIXME: pending changes from inside the editor are  not
            // immediately propagated to the VMEScriptConfig! This is also not
            // possible at all times as the script might be in an unparseable
            // state while the user is typing a command.
            // This means we cannot just replace our script text with the one
            // in the editor.
            // How best to synchronize?

            auto widget = new VMEScriptEditor(d->scriptConfig);
            widget->setAttribute(Qt::WA_DeleteOnClose);
            d->scriptEditor = widget;

            connect(widget, &QObject::destroyed,
                    this, [this] () { d->scriptEditor = nullptr; });

            connect(widget, &VMEScriptEditor::logMessage,
                    this, &MVLCTriggerIOEditor::logMessage);

            // Note: VMEScriptEditor has a runScript() signal which passes a
            // parsed VMEScript. The reason is that the script text in the
            // editor might be modified but the modifications have not been
            // applied to the VMEScriptConfig that's being edited.
            // This design is not great and causes problems in a few places.
            // In this editor here the VMEScriptConfig is directly modified.
            // Changes can be reverted by restoring the original script text.
            // This also means we can just run the VMEScriptConfig instead of
            // the parsed VMEScript. We ignore the argument the VMEScriptEditor
            // passes in its runScript() signal.
            connect(widget, &VMEScriptEditor::runScript,
                    this, [this] () { emit runScriptConfig(d->scriptConfig); });

            // Update the editors script text on each change.
            connect(d->scriptConfig, &VMEScriptConfig::modified,
                    widget, &VMEScriptEditor::reloadFromScriptConfig);

            widget->show();
        });

    auto mainLayout = make_vbox<2, 2>(this);
    mainLayout->addWidget(toolbar);
    mainLayout->addWidget(logicWidget);

    setWindowTitle(QSL("MVLC Trigger & I/O Editor (")
                   + d->scriptConfig->getVerboseTitle() + ")");
}

MVLCTriggerIOEditor::~MVLCTriggerIOEditor() { }

void MVLCTriggerIOEditor::runScript_()
{
    emit runScriptConfig(d->scriptConfig);
}

void MVLCTriggerIOEditor::configModified()
{
    d->scene->setTriggerIOConfig(d->ioCfg);
    regenerateScript();
}

void MVLCTriggerIOEditor::regenerateScript()
{
    auto &ioCfg = d->ioCfg;
    auto scriptText = generate_trigger_io_script_text(ioCfg);
    d->scriptConfig->setScriptContents(scriptText);
}

} // end namespace mesytec
