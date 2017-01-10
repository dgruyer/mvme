#include "daqconfig_tree.h"
#include "mvme.h"
#include "mvme_config.h"
#include "mvme_context.h"
#include "config_ui.h"
#include "treewidget_utils.h"
#include "mvme_event_processor.h"
#include "vmusb.h"

#include <QDebug>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QToolButton>
#include <QTreeWidget>

using namespace std::placeholders;

enum NodeType
{
    NodeType_Event = QTreeWidgetItem::UserType,
    NodeType_Module,
    NodeType_ModuleReset,
    NodeType_EventModulesInit,
    NodeType_EventReadoutLoop,
    NodeType_EventStartStop,
};

enum DataRole
{
    DataRole_Pointer = Qt::UserRole,
    DataRole_ScriptCategory,
};

class TreeNode: public QTreeWidgetItem
{
    public:
        using QTreeWidgetItem::QTreeWidgetItem;
};

class EventNode: public TreeNode
{
    public:
        EventNode()
            : TreeNode(NodeType_Event)
        {}

        TreeNode *modulesNode = nullptr;
        TreeNode *readoutLoopNode = nullptr;
        TreeNode *daqStartStopNode = nullptr;
};

class ModuleNode: public TreeNode
{
    public:
        ModuleNode()
            : TreeNode(NodeType_Module)
        {}

        TreeNode *readoutNode = nullptr;
};

DAQConfigTreeWidget::DAQConfigTreeWidget(MVMEContext *context, QWidget *parent)
    : QWidget(parent)
    , m_context(context)
    , m_tree(new QTreeWidget(this))
    , m_nodeEvents(new TreeNode)
    , m_nodeManual(new TreeNode)
    , m_nodeStart(new TreeNode)
    , m_nodeStop(new TreeNode)
    , m_nodeScripts(new TreeNode)
{
    m_tree->setColumnCount(2);
    m_tree->setExpandsOnDoubleClick(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setIndentation(10);
    m_tree->setItemDelegateForColumn(1, new NoEditDelegate(this));
    m_tree->setEditTriggers(QAbstractItemView::EditKeyPressed);

    auto headerItem = m_tree->headerItem();
    headerItem->setText(0, QSL("Object"));
    headerItem->setText(1, QSL("Info"));

    m_nodeEvents->setText(0,        QSL("Events"));
    m_nodeScripts->setText(0,       QSL("Global Scripts"));

    m_nodeStart->setText(0, QSL("DAQ Start"));
    m_nodeStart->setData(0, DataRole_ScriptCategory, "daq_start");

    m_nodeStop->setText(0, QSL("DAQ Stop"));
    m_nodeStop->setData(0, DataRole_ScriptCategory, "daq_stop");

    m_nodeManual->setText(0,  QSL("Manual"));
    m_nodeManual->setData(0,  DataRole_ScriptCategory, "manual");

    m_tree->addTopLevelItem(m_nodeEvents);
    m_tree->addTopLevelItem(m_nodeScripts);

    m_nodeScripts->addChild(m_nodeStart);
    m_nodeScripts->addChild(m_nodeStop);
    m_nodeScripts->addChild(m_nodeManual);

    auto nodes = QList<TreeNode *>({ m_nodeEvents, m_nodeScripts });
    for (auto node: nodes)
        node->setExpanded(true);

    m_tree->resizeColumnToContents(0);

    // Toolbar buttons
    auto makeToolButton = [](const QString &icon, const QString &text)
    {
        auto result = new QToolButton;
        result->setIcon(QIcon(icon));
        result->setText(text);
        result->setStatusTip(text);
        //result->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        result->setToolButtonStyle(Qt::ToolButtonIconOnly);
        auto font = result->font();
        font.setPointSize(8);
        result->setFont(font);
        return result;
    };
    pb_new = makeToolButton(QSL(":/document-new.png"), QSL("New"));
    pb_load = makeToolButton(QSL(":/document-open.png"), QSL("Open"));
    pb_save = makeToolButton(QSL(":/document-save.png"), QSL("Save"));
    pb_saveAs = makeToolButton(QSL(":/document-save-as.png"), QSL("Save As"));

    connect(pb_new, &QAbstractButton::clicked, this, &DAQConfigTreeWidget::newConfig);
    connect(pb_load, &QAbstractButton::clicked, this, &DAQConfigTreeWidget::loadConfig);
    connect(pb_save, &QAbstractButton::clicked, this, &DAQConfigTreeWidget::saveConfig);
    connect(pb_saveAs, &QAbstractButton::clicked, this, &DAQConfigTreeWidget::saveConfigAs);

    QToolButton *pb_treeSettings = nullptr;

    {
        auto menu = new QMenu;
        action_showAdvanced = menu->addAction(QSL("Show advanced objects"));
        action_showAdvanced->setCheckable(true);
        connect(action_showAdvanced, &QAction::changed, this, &DAQConfigTreeWidget::onActionShowAdvancedChanged);

        auto action_dumpVMUSBRegisters = menu->addAction(QSL("Dump VMUSB Registers"));
        connect(action_dumpVMUSBRegisters, &QAction::triggered, this, &DAQConfigTreeWidget::dumpVMUSBRegisters);

        //pb_treeSettings = new QPushButton(QIcon(":/tree-settings.png"), QSL(""));
        pb_treeSettings = makeToolButton(QSL(":/tree-settings.png"), QSL("More"));
        pb_treeSettings->setMenu(menu);
        pb_treeSettings->setPopupMode(QToolButton::InstantPopup);

        QSettings settings;
        action_showAdvanced->setChecked(settings.value("DAQTree/ShowAdvanced", false).toBool());
        onActionShowAdvancedChanged();
    }

    auto buttonLayout = new QHBoxLayout;
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(2);
    buttonLayout->addWidget(pb_new);
    buttonLayout->addWidget(pb_load);
    buttonLayout->addWidget(pb_save);
    buttonLayout->addWidget(pb_saveAs);
    buttonLayout->addWidget(pb_treeSettings);
    buttonLayout->addStretch(1);

    // filename label
    le_fileName = new QLineEdit;
    le_fileName->setReadOnly(true);
    auto pal = le_fileName->palette();
    pal.setBrush(QPalette::Base, QColor(239, 235, 231));
    le_fileName->setPalette(pal);

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(buttonLayout);
    layout->addWidget(le_fileName);
    layout->addWidget(m_tree);

    connect(m_tree, &QTreeWidget::itemClicked, this, &DAQConfigTreeWidget::onItemClicked);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &DAQConfigTreeWidget::onItemDoubleClicked);
    connect(m_tree, &QTreeWidget::itemChanged, this, &DAQConfigTreeWidget::onItemChanged);
    connect(m_tree, &QTreeWidget::itemExpanded, this, &DAQConfigTreeWidget::onItemExpanded);
    connect(m_tree, &QWidget::customContextMenuRequested, this, &DAQConfigTreeWidget::treeContextMenu);

    connect(m_context, &MVMEContext::daqConfigChanged, this, &DAQConfigTreeWidget::setConfig);
    connect(m_context, &MVMEContext::daqConfigFileNameChanged, this, [this](const QString &) {
        updateConfigLabel();
    });

    setConfig(m_context->getDAQConfig());
}

void DAQConfigTreeWidget::setConfig(DAQConfig *cfg)
{
    qDeleteAll(m_nodeManual->takeChildren());
    qDeleteAll(m_nodeStart->takeChildren());
    qDeleteAll(m_nodeStop->takeChildren());
    qDeleteAll(m_nodeEvents->takeChildren());
    m_treeMap.clear();

    m_config = cfg;

    if (cfg)
    {
        for (auto category: cfg->vmeScriptLists.keys())
            for (auto script: cfg->vmeScriptLists[category])
                onScriptAdded(script, category);

        for (auto event: cfg->eventConfigs)
            onEventAdded(event);

        connect(cfg, &DAQConfig::eventAdded, this, &DAQConfigTreeWidget::onEventAdded);
        connect(cfg, &DAQConfig::eventAboutToBeRemoved, this, &DAQConfigTreeWidget::onEventAboutToBeRemoved);
        connect(cfg, &DAQConfig::globalScriptAdded, this, &DAQConfigTreeWidget::onScriptAdded);
        connect(cfg, &DAQConfig::globalScriptAboutToBeRemoved, this, &DAQConfigTreeWidget::onScriptAboutToBeRemoved);
    }

    m_tree->resizeColumnToContents(0);
}

DAQConfig *DAQConfigTreeWidget::getConfig() const
{
    return m_config;
}

template<typename T>
TreeNode *makeNode(T *data, int type = QTreeWidgetItem::Type)
{
    auto ret = new TreeNode(type);
    ret->setData(0, DataRole_Pointer, Ptr2Var(data));
    return ret;
}

TreeNode *DAQConfigTreeWidget::addScriptNode(TreeNode *parent, VMEScriptConfig* script, bool canDisable)
{
    auto node = new TreeNode;
    node->setData(0, DataRole_Pointer, Ptr2Var(script));
    node->setText(0, script->objectName());
    node->setIcon(0, QIcon(":/vme_script.png"));
    node->setFlags(node->flags() | Qt::ItemIsEditable);
    if (canDisable)
    {
        //node->setCheckState(0, script->isEnabled() ? Qt::Checked : Qt::Unchecked);
    }
    m_treeMap[script] = node;
    parent->addChild(node);

    return node;
}

TreeNode *DAQConfigTreeWidget::addEventNode(TreeNode *parent, EventConfig *event)
{
    auto eventNode = new EventNode;
    eventNode->setData(0, DataRole_Pointer, Ptr2Var(event));
    eventNode->setText(0, event->objectName());
    //eventNode->setCheckState(0, Qt::Checked);
    eventNode->setFlags(eventNode->flags() | Qt::ItemIsEditable);
    m_treeMap[event] = eventNode;
    parent->addChild(eventNode);
    eventNode->setExpanded(true);

    eventNode->modulesNode = new TreeNode(NodeType_EventModulesInit);
    auto modulesNode = eventNode->modulesNode;
    modulesNode->setText(0, QSL("Modules Init"));
    modulesNode->setIcon(0, QIcon(":/config_category.png"));
    eventNode->addChild(modulesNode);
    modulesNode->setExpanded(true);

    eventNode->readoutLoopNode = new TreeNode(NodeType_EventReadoutLoop);
    auto readoutLoopNode = eventNode->readoutLoopNode;
    readoutLoopNode->setText(0, QSL("Readout Loop"));
    readoutLoopNode->setIcon(0, QIcon(":/config_category.png"));
    eventNode->addChild(readoutLoopNode);

    {
        auto node = makeNode(event->vmeScripts["readout_start"]);
        node->setText(0, QSL("Cycle Start"));
        node->setIcon(0, QIcon(":/vme_script.png"));
        readoutLoopNode->addChild(node);
    }

    {
        auto node = makeNode(event->vmeScripts["readout_end"]);
        node->setText(0, QSL("Cycle End"));
        node->setIcon(0, QIcon(":/vme_script.png"));
        readoutLoopNode->addChild(node);
    }

    eventNode->daqStartStopNode = new TreeNode(NodeType_EventStartStop);
    auto daqStartStopNode = eventNode->daqStartStopNode;
    daqStartStopNode->setText(0, QSL("Multicast DAQ Start/Stop"));
    daqStartStopNode->setIcon(0, QIcon(":/config_category.png"));
    eventNode->addChild(daqStartStopNode);

    {
        auto node = makeNode(event->vmeScripts["daq_start"]);
        node->setText(0, QSL("DAQ Start"));
        node->setIcon(0, QIcon(":/vme_script.png"));
        daqStartStopNode->addChild(node);
    }

    {
        auto node = makeNode(event->vmeScripts["daq_stop"]);
        node->setText(0, QSL("DAQ Stop"));
        node->setIcon(0, QIcon(":/vme_script.png"));
        daqStartStopNode->addChild(node);
    }

    return eventNode;
}

TreeNode *DAQConfigTreeWidget::addModuleNodes(EventNode *parent, ModuleConfig *module)
{
    auto moduleNode = new ModuleNode;
    moduleNode->setData(0, DataRole_Pointer, Ptr2Var(module));
    moduleNode->setText(0, module->objectName());
    //moduleNode->setCheckState(0, Qt::Checked);
    moduleNode->setIcon(0, QIcon(":/vme_module.png"));
    moduleNode->setFlags(moduleNode->flags() | Qt::ItemIsEditable);
    m_treeMap[module] = moduleNode;
    parent->modulesNode->addChild(moduleNode);

    {
        auto parametersNode = makeNode(module->vmeScripts["parameters"]);
        parametersNode->setText(0, QSL("Module Init"));
        parametersNode->setIcon(0, QIcon(":/vme_script.png"));
        moduleNode->addChild(parametersNode);
    }

    {
        auto readoutSettingsNode = makeNode(module->vmeScripts["readout_settings"]);
        readoutSettingsNode->setText(0, QSL("VME Interface Settings"));
        readoutSettingsNode->setIcon(0, QIcon(":/vme_script.png"));
        moduleNode->addChild(readoutSettingsNode);
    }

    {
        auto resetNode = makeNode(module->vmeScripts["reset"], NodeType_ModuleReset);
        resetNode->setText(0, QSL("Module Reset"));
        resetNode->setIcon(0, QIcon(":/vme_script.png"));
        moduleNode->addChild(resetNode);
    }

    {
        auto readoutNode = makeNode(module->vmeScripts["readout"]);
        moduleNode->readoutNode = readoutNode;
        readoutNode->setText(0, module->objectName());
        readoutNode->setIcon(0, QIcon(":/vme_module.png"));

        auto readoutLoopNode = parent->readoutLoopNode;
        readoutLoopNode->insertChild(readoutLoopNode->childCount() - 1, readoutNode);
    }

    return moduleNode;
}

void DAQConfigTreeWidget::onItemClicked(QTreeWidgetItem *item, int column)
{
    auto configObject = Var2Ptr<ConfigObject>(item->data(0, DataRole_Pointer));

    qDebug() << "clicked" << item << configObject;

    if (configObject)
    {
        emit configObjectClicked(configObject);
    }
}

void DAQConfigTreeWidget::onItemDoubleClicked(QTreeWidgetItem *item, int column)
{
    auto configObject = Var2Ptr<ConfigObject>(item->data(0, DataRole_Pointer));
    auto scriptConfig = qobject_cast<VMEScriptConfig *>(configObject);

    if (scriptConfig)
    {
        emit configObjectDoubleClicked(scriptConfig);
    }
}

void DAQConfigTreeWidget::onItemChanged(QTreeWidgetItem *item, int column)
{
    auto obj = Var2Ptr<ConfigObject>(item->data(0, DataRole_Pointer));

    if (obj)
    {
        //if (item->flags() & Qt::ItemIsUserCheckable)
        //    obj->setEnabled(item->checkState(0) != Qt::Unchecked);

        if (item->flags() & Qt::ItemIsEditable)
        {
            obj->setObjectName(item->text(0));
        }

        m_tree->resizeColumnToContents(0);
    }
}

void DAQConfigTreeWidget::onItemExpanded(QTreeWidgetItem *item)
{
    m_tree->resizeColumnToContents(0);
}

void DAQConfigTreeWidget::treeContextMenu(const QPoint &pos)
{
    auto node = m_tree->itemAt(pos);
    auto parent = node ? node->parent() : nullptr;
    auto obj = node ? Var2Ptr<ConfigObject>(node->data(0, DataRole_Pointer)) : nullptr;

    QMenu menu;

    //
    // Script nodes
    //
    if (qobject_cast<VMEScriptConfig *>(obj))
    {
        menu.addAction(QSL("Run Script"), this, &DAQConfigTreeWidget::runScripts);
    }

    //
    // Events
    //
    if (node == m_nodeEvents)
    {
        menu.addAction(QSL("Add Event"), this, &DAQConfigTreeWidget::addEvent);
    }

    if (node && node->type() == NodeType_Event)
    {
        menu.addAction(QSL("Edit Event"), this, &DAQConfigTreeWidget::editEvent);
        menu.addAction(QSL("Add Module"), this, &DAQConfigTreeWidget::addModule);
        menu.addAction(QSL("Rename Event"), this, &DAQConfigTreeWidget::editName);
        menu.addSeparator();
        menu.addAction(QSL("Remove Event"), this, &DAQConfigTreeWidget::removeEvent);
    }

    if (node && node->type() == NodeType_EventModulesInit)
    {
        menu.addAction(QSL("Add Module"), this, &DAQConfigTreeWidget::addModule);
    }

    if (node && node->type() == NodeType_Module)
    {
        menu.addAction(QSL("Init Module"), this, &DAQConfigTreeWidget::initModule);
        menu.addAction(QSL("Edit Module"), this, &DAQConfigTreeWidget::editModule);
        menu.addAction(QSL("Rename Module"), this, &DAQConfigTreeWidget::editName);
        menu.addSeparator();
        menu.addAction(QSL("Remove Module"), this, &DAQConfigTreeWidget::removeModule);

        if (!m_context->getEventProcessor()->getDiagnostics())
            menu.addAction(QSL("Show Diagnostics"), this, &DAQConfigTreeWidget::handleShowDiagnostics);
    }

    //
    // Global scripts
    //
    if (node == m_nodeStart || node == m_nodeStop || node == m_nodeManual)
    {
        if (node->childCount() > 0)
            menu.addAction(QSL("Run scripts"), this, &DAQConfigTreeWidget::runScripts);

        menu.addAction(QSL("Add script"), this, &DAQConfigTreeWidget::addGlobalScript);

    }

    if (parent == m_nodeStart || parent == m_nodeStop || parent == m_nodeManual)
    {
        menu.addAction(QSL("Rename Script"), this, &DAQConfigTreeWidget::editName);
        menu.addSeparator();
        menu.addAction(QSL("Remove Script"), this, &DAQConfigTreeWidget::removeGlobalScript);
    }

    if (!menu.isEmpty())
    {
        menu.exec(m_tree->mapToGlobal(pos));
    }
}

void DAQConfigTreeWidget::onEventAdded(EventConfig *eventConfig)
{
    addEventNode(m_nodeEvents, eventConfig);

    for (auto module: eventConfig->modules)
        onModuleAdded(module);

    connect(eventConfig, &EventConfig::moduleAdded, this, &DAQConfigTreeWidget::onModuleAdded);
    connect(eventConfig, &EventConfig::moduleAboutToBeRemoved, this, &DAQConfigTreeWidget::onModuleAboutToBeRemoved);

    auto updateEventNode = [eventConfig, this](bool isModified) {
        auto node = static_cast<EventNode *>(m_treeMap.value(eventConfig, nullptr));

        if (!isModified || !node)
            return;

        node->setText(0, eventConfig->objectName());
        //node->setCheckState(0, eventConfig->isEnabled() ? Qt::Checked : Qt::Unchecked);

        QString infoText;

        switch (eventConfig->triggerCondition)
        {
            case TriggerCondition::Interrupt:
                {
                    infoText = QString("Trigger=IRQ, lvl=%2, vec=%3")
                        .arg(eventConfig->irqLevel)
                        .arg(eventConfig->irqVector);
                } break;
            case TriggerCondition::NIM1:
                {
                    infoText = QSL("Trigger=NIM");
                } break;
            case TriggerCondition::Periodic:
                {
                    infoText = QSL("Trigger=Periodic");
                } break;
        }

        node->setText(1, infoText);
    };

    updateEventNode(true);

    connect(eventConfig, &EventConfig::modified, this, updateEventNode);
    onActionShowAdvancedChanged();
}

void DAQConfigTreeWidget::onEventAboutToBeRemoved(EventConfig *config)
{
    for (auto module: config->modules)
    {
        onModuleAboutToBeRemoved(module);
    }

    delete m_treeMap.take(config);
}

void DAQConfigTreeWidget::onModuleAdded(ModuleConfig *module)
{
    auto eventNode = static_cast<EventNode *>(m_treeMap[module->parent()]);
    addModuleNodes(eventNode, module);

    auto updateModuleNodes = [module, this](bool isModified) {
        auto node = static_cast<ModuleNode *>(m_treeMap.value(module, nullptr));

        if (!isModified || !node)
            return;

        node->setText(0, module->objectName());
        node->readoutNode->setText(0, module->objectName());
        //node->setCheckState(0, module->isEnabled() ? Qt::Checked : Qt::Unchecked);

        QString infoText = QString("Type=%1, Address=0x%2")
            .arg(VMEModuleTypeNames.value(module->type, QSL("unknown")))
            .arg(module->getBaseAddress(), 8, 16, QChar('0'));

        node->setText(1, infoText);
    };

    updateModuleNodes(true);

    connect(module, &ModuleConfig::modified, this, updateModuleNodes);
    onActionShowAdvancedChanged();
}

void DAQConfigTreeWidget::onModuleAboutToBeRemoved(ModuleConfig *module)
{
    auto moduleNode = static_cast<ModuleNode *>(m_treeMap[module]);
    delete moduleNode->readoutNode;
    delete m_treeMap.take(module);
}

void DAQConfigTreeWidget::onScriptAdded(VMEScriptConfig *script, const QString &category)
{
    TreeNode *parentNode = nullptr;
    bool canDisable = true;

    if (category == QSL("daq_start"))
        parentNode = m_nodeStart;
    else if (category == QSL("daq_stop"))
        parentNode = m_nodeStop;
    else if (category == QSL("manual"))
    {
        parentNode = m_nodeManual;
        canDisable = false;
    }

    if (parentNode)
    {
        addScriptNode(parentNode, script, canDisable);
        m_tree->resizeColumnToContents(0);
    }
}

void DAQConfigTreeWidget::onScriptAboutToBeRemoved(VMEScriptConfig *script)
{
    delete m_treeMap.take(script);
}

//
// context menu action implementations
//
void DAQConfigTreeWidget::addEvent()
{
    auto config = new EventConfig;
    config->setObjectName(QString("event%1").arg(m_config->getEventConfigs().size()));
    EventConfigDialog dialog(m_context, config);
    int result = dialog.exec();

    if (result == QDialog::Accepted)
    {
        TemplateLoader loader;
        connect(&loader, &TemplateLoader::logMessage, m_context, &MVMEContext::logMessage);

        if (config->triggerCondition != TriggerCondition::Periodic)
        {
            config->vmeScripts["daq_start"]->setScriptContents(loader.readTemplate(QSL("event_daq_start.vme")));
            config->vmeScripts["daq_stop"]->setScriptContents(loader.readTemplate(QSL("event_daq_stop.vme")));
            config->vmeScripts["readout_start"]->setScriptContents(loader.readTemplate(QSL("readout_cycle_start.vme")));
            config->vmeScripts["readout_end"]->setScriptContents(loader.readTemplate(QSL("readout_cycle_end.vme")));
        }

        m_config->addEventConfig(config);
        auto node = m_treeMap.value(config, nullptr);
        if (node)
            node->setExpanded(true);
    }
    else
    {
        delete config;
    }
}

void DAQConfigTreeWidget::removeEvent()
{
    auto node = m_tree->currentItem();

    if (node && node->type() == NodeType_Event)
    {
        auto event = Var2Ptr<EventConfig>(node->data(0, DataRole_Pointer));
        m_config->removeEventConfig(event);
        event->deleteLater();
    }
}

void DAQConfigTreeWidget::editEvent()
{
    auto node = m_tree->currentItem();

    if (node && node->type() == NodeType_Event)
    {
        auto eventConfig = Var2Ptr<EventConfig>(node->data(0, DataRole_Pointer));
        EventConfigDialog dialog(m_context, eventConfig);
        dialog.exec();
    }
}

void DAQConfigTreeWidget::addModule()
{
    auto node = m_tree->currentItem();

    while (node && node->type() != NodeType_Event)
    {
        node = node->parent();
    }

    if (node)
    {
        auto event = Var2Ptr<EventConfig>(node->data(0, DataRole_Pointer));
        bool doExpand = (event->modules.size() == 0);

        auto module = new ModuleConfig;
        ModuleConfigDialog dialog(m_context, module);
        int result = dialog.exec();

        if (result == QDialog::Accepted)
        {
            TemplateLoader loader;
            connect(&loader, &TemplateLoader::logMessage, m_context, &MVMEContext::logMessage);
            
            const QString shortName = VMEModuleShortNames.value(module->type, QSL("unknown"));

            module->vmeScripts["parameters"]->setScriptContents(loader.readTemplate(
                    QString("%1_parameters.vme").arg(shortName)));

            // Generic module scripts for mesytec modules
            if (isMesytecModule(module->type))
            {
                module->vmeScripts["readout_settings"]->setScriptContents(loader.readTemplate(QSL("mesytec_readout_settings.vme")));
                module->vmeScripts["readout"]->setScriptContents(loader.readTemplate(QSL("mesytec_readout.vme")));
                module->vmeScripts["reset"]->setScriptContents(loader.readTemplate(QSL("mesytec_reset.vme")));
            }

            // Scripts for the specific module type. The override the generic ones loaded above.
            QString tmpl = loader.readTemplate(QString("%1_readout_settings.vme").arg(shortName));
            if (!tmpl.isEmpty())
                module->vmeScripts["readout_settings"]->setScriptContents(tmpl);

            tmpl = loader.readTemplate(QString("%1_readout.vme").arg(shortName));
            if (!tmpl.isEmpty())
                module->vmeScripts["readout"]->setScriptContents(tmpl);

            tmpl = loader.readTemplate(QString("%1_reset.vme").arg(shortName));
            if (!tmpl.isEmpty())
                module->vmeScripts["reset"]->setScriptContents(tmpl);


            event->addModuleConfig(module);

            if (doExpand)
                static_cast<EventNode *>(node)->modulesNode->setExpanded(true);
        }
        else
        {
            delete module;
        }
    }
}

void DAQConfigTreeWidget::removeModule()
{
    auto node = m_tree->currentItem();

    while (node && node->type() != NodeType_Module)
    {
        node = node->parent();
    }

    if (node)
    {
        auto module = Var2Ptr<ModuleConfig>(node->data(0, DataRole_Pointer));
        auto event = qobject_cast<EventConfig *>(module->parent());
        if (event)
        {
            event->removeModuleConfig(module);
            module->deleteLater();
        }
    }
}

void DAQConfigTreeWidget::editModule()
{
    auto node = m_tree->currentItem();

    while (node && node->type() != NodeType_Module)
    {
        node = node->parent();
    }

    if (node)
    {
        auto moduleConfig = Var2Ptr<ModuleConfig>(node->data(0, DataRole_Pointer));
        ModuleConfigDialog dialog(m_context, moduleConfig);
        dialog.exec();
    }
}

void DAQConfigTreeWidget::addGlobalScript()
{
    auto node = m_tree->currentItem();
    auto category = node->data(0, DataRole_ScriptCategory).toString();
    auto script = new VMEScriptConfig;

    script->setObjectName("new vme script");
    bool doExpand = (node->childCount() == 0);
    m_config->addGlobalScript(script, category);
    if (doExpand)
        node->setExpanded(true);

    auto scriptNode = m_treeMap.value(script, nullptr);
    if (scriptNode)
    {
        m_tree->editItem(scriptNode, 0);
    }
}

void DAQConfigTreeWidget::removeGlobalScript()
{
    auto node = m_tree->currentItem();
    auto script = Var2Ptr<VMEScriptConfig>(node->data(0, DataRole_Pointer));
    m_config->removeGlobalScript(script);
}

void DAQConfigTreeWidget::runScripts()
{
    auto node = m_tree->currentItem();
    auto obj  = Var2Ptr<ConfigObject>(node->data(0, DataRole_Pointer));

    QVector<VMEScriptConfig *> scriptConfigs;

    auto scriptConfig = qobject_cast<VMEScriptConfig *>(obj);

    if (scriptConfig)
    {
        scriptConfigs.push_back(scriptConfig);
    }
    else
    {
        for (int i=0; i<node->childCount(); ++i)
        {
            obj = Var2Ptr<ConfigObject>(node->child(i)->data(0, DataRole_Pointer));
            scriptConfig = qobject_cast<VMEScriptConfig *>(obj);
            scriptConfigs.push_back(scriptConfig);
        }
    }

    runScriptConfigs(scriptConfigs);
}

void DAQConfigTreeWidget::editName()
{
    m_tree->editItem(m_tree->currentItem(), 0);
}

void DAQConfigTreeWidget::initModule()
{
    auto node = m_tree->currentItem();
    auto module = Var2Ptr<ModuleConfig>(node->data(0, DataRole_Pointer));

    QVector<VMEScriptConfig *> scriptConfigs;
    scriptConfigs.push_back(module->vmeScripts["parameters"]);
    scriptConfigs.push_back(module->vmeScripts["readout_settings"]);

    runScriptConfigs(scriptConfigs);
}

void DAQConfigTreeWidget::runScriptConfigs(const QVector<VMEScriptConfig *> &scriptConfigs)
{
    for (auto scriptConfig: scriptConfigs)
    {
        auto moduleConfig = qobject_cast<ModuleConfig *>(scriptConfig->parent());

        m_context->logMessage(QSL("Running script ") + scriptConfig->getVerboseTitle());

        try
        {
            auto logger = [this](const QString &str) { m_context->logMessage(QSL("  ") + str); };

            auto results = m_context->runScript(
                scriptConfig->getScript(moduleConfig ? moduleConfig->getBaseAddress() : 0),
                logger);

            for (auto result: results)
                logger(format_result(result));
        }
        catch (const vme_script::ParseError &e)
        {
            m_context->logMessage(QSL("Parse error: ") + e.what());
        }
    }
}


void DAQConfigTreeWidget::onActionShowAdvancedChanged()
{
    auto nodes = findItems(m_nodeEvents, [](QTreeWidgetItem *node) {
        return node->type() == NodeType_EventReadoutLoop
            || node->type() == NodeType_EventStartStop
            || node->type() == NodeType_ModuleReset;
    });

    nodes.push_back(m_nodeScripts);

    bool showAdvanced = action_showAdvanced->isChecked();

    for (auto node: nodes)
        node->setHidden(!showAdvanced);

    QSettings settings;
    settings.setValue("DAQTree/ShowAdvanced", showAdvanced);
};

void DAQConfigTreeWidget::handleShowDiagnostics()
{
    auto node = m_tree->currentItem();
    auto module = Var2Ptr<ModuleConfig>(node->data(0, DataRole_Pointer));
    emit showDiagnostics(module);
}

void DAQConfigTreeWidget::dumpVMUSBRegisters()
{
    auto vmusb = dynamic_cast<VMUSB *>(m_context->getController());

    if (vmusb && m_context->getDAQState() == DAQState::Idle)
    {
        dump_registers(vmusb, [this] (const QString &line) { m_context->logMessage(line); });
    }
}

static const QString fileFilter = QSL("Config Files (*.mvmecfg);; All Files (*.*)");
static const QString settingsPath = QSL("Files/LastConfigFile");

void DAQConfigTreeWidget::newConfig()
{
    m_context->getMainWindow()->on_actionNewConfig_triggered();
}

void DAQConfigTreeWidget::loadConfig()
{
    m_context->getMainWindow()->on_actionLoadConfig_triggered();
}

bool DAQConfigTreeWidget::saveConfig()
{
    return m_context->getMainWindow()->on_actionSaveConfig_triggered();
}

bool DAQConfigTreeWidget::saveConfigAs()
{
    return m_context->getMainWindow()->on_actionSaveConfigAs_triggered();
}

void DAQConfigTreeWidget::updateConfigLabel()
{
    QString fileName = m_context->getConfigFileName();

    if (fileName.isEmpty())
        fileName = QSL("<not saved>");

    if (m_context->getDAQConfig()->isModified())
        fileName += QSL(" *");

    le_fileName->setText(fileName);
    le_fileName->setToolTip(fileName);
    le_fileName->setStatusTip(fileName);
}
