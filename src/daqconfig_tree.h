#ifndef __DAQCONFIG_TREE_H__
#define __DAQCONFIG_TREE_H__

#include <QWidget>
#include <QMap>

class QTreeWidget;
class QTreeWidgetItem;
class TreeNode;

class DAQConfig;
class EventConfig;
class ModuleConfig;
class VMEScriptConfig;
class MVMEContext;

class EventNode;

class DAQConfigTreeWidget: public QWidget
{
    Q_OBJECT
    public:
        DAQConfigTreeWidget(MVMEContext *context, QWidget *parent = 0);

        void setConfig(DAQConfig *cfg);
        DAQConfig *getConfig() const;

    private:
        TreeNode *addScriptNode(TreeNode *parent, VMEScriptConfig *script, bool canDisable = false);
        TreeNode *addEventNode(TreeNode *parent, EventConfig *event);
        TreeNode *addModuleNodes(EventNode *parent, ModuleConfig *module);

        void onItemClicked(QTreeWidgetItem *item, int column);
        void onItemDoubleClicked(QTreeWidgetItem *item, int column);
        void onItemChanged(QTreeWidgetItem *item, int column);
        void onItemExpanded(QTreeWidgetItem *item);
        void treeContextMenu(const QPoint &pos);

        void onEventAdded(EventConfig *config);
        void onEventAboutToBeRemoved(EventConfig *config);

        void onModuleAdded(ModuleConfig *config);
        void onModuleAboutToBeRemoved(ModuleConfig *config);

        void onScriptAdded(VMEScriptConfig *script, const QString &category);
        void onScriptAboutToBeRemoved(VMEScriptConfig *script);

        // context menu action implementations
        void addEvent();
        void removeEvent();
        void addModule();
        void removeModule();
        void addGlobalScript();
        void removeGlobalScript();
        void runScripts();

        MVMEContext *m_context = nullptr;
        DAQConfig *m_config = nullptr;
        QTreeWidget *m_tree;
        // Maps config objects to tree nodes
        QMap<QObject *, TreeNode *> m_treeMap;

        TreeNode *m_nodeEvents, *m_nodeManual, *m_nodeStart, *m_nodeStop,
                 *m_nodeScripts;
};

#endif /* __DAQCONFIG_TREE_H__ */
