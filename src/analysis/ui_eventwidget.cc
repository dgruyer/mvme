#include "analysis/ui_eventwidget.h"
#include "analysis/ui_eventwidget_p.h"

#include "analysis/a2_adapter.h"
#include "analysis/analysis_serialization.h"
#include "analysis/analysis_ui.h"
#include "analysis/condition_ui.h"
#include "analysis/expression_operator_dialog.h"
#include "analysis/listfilter_extractor_dialog.h"

#include "histo1d_widget.h"
#include "histo2d_widget.h"
#include "mvme_context.h"
#include "mvme_context_lib.h"
#include "rate_monitor_widget.h"

#include <boost/dynamic_bitset.hpp>
#include <QClipboard>
#include <QFileDialog>
#include <QGuiApplication>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QStandardPaths>
#include <QTimer>

namespace analysis
{
namespace ui
{

template<typename T>
inline T *get_qobject_pointer(QTreeWidgetItem *node, s32 dataRole = Qt::UserRole)
{
    if (auto qobj = get_qobject(node, dataRole))
    {
        return qobject_cast<T *>(qobj);
    }

    return nullptr;
}

AnalysisObjectPtr get_analysis_object(QTreeWidgetItem *node, s32 dataRole = Qt::UserRole)
{
    switch (node->type())
    {
        case NodeType_Source:
        case NodeType_Operator:
        case NodeType_Histo1DSink:
        case NodeType_Histo2DSink:
        case NodeType_Sink:
        case NodeType_Directory:
            {
                auto qo = get_qobject(node, dataRole);
                qDebug() << qo;
                if (auto ao = qobject_cast<AnalysisObject *>(qo))
                    return ao->shared_from_this();
            }
    }

    return AnalysisObjectPtr();
}

template<typename T>
std::shared_ptr<T> get_shared_analysis_object(QTreeWidgetItem *node,
                                              s32 dataRole = Qt::UserRole)
{
    auto objPtr = get_analysis_object(node, dataRole);
    return std::dynamic_pointer_cast<T>(objPtr);
}

//
// ObjectTree and subclasses
//

// MIME types for drag and drop operations

// SourceInterface objects only
static const QString DataSourceIdListMIMEType = QSL("application/x-mvme-analysis-datasource-id-list");

// Non datasource operators and directories
static const QString OperatorIdListMIMEType = QSL("application/x-mvme-analysis-operator-id-list");

// Sink-type operators and directories
static const QString SinkIdListMIMEType = QSL("application/x-mvme-analysis-sink-id-list");

// Generic, untyped analysis objects
static const QString ObjectIdListMIMEType = QSL("application/x-mvme-analysis-object-id-list");

namespace
{

QVector<QUuid> decode_id_list(QByteArray data)
{
    QDataStream stream(&data, QIODevice::ReadOnly);
    QVector<QByteArray> sourceIds;
    stream >> sourceIds;

    QVector<QUuid> result;
    result.reserve(sourceIds.size());

    for (const auto &idData: sourceIds)
    {
        result.push_back(QUuid(idData));
    }

    return result;
}

/* In the active set a bit is set to 1 if the candidate at the bit index
 * uses the condition link being edited.
 * The checked set is the same but contains a 1 if the node representing
 * the candidate is checked. */
struct ConditionLinkModifications
{
    ConditionLink cl;
    OperatorVector candidates;
    boost::dynamic_bitset<> active;
    boost::dynamic_bitset<> checked;

    bool hasModifications() const { return active != checked; }
};

ConditionLinkModifications get_condition_modifications(const ConditionLink &cl,
                                                       Analysis *analysis,
                                                       const ObjectToNode &objectMap)
{
    ConditionLinkModifications result;

    result.candidates = get_apply_condition_candidates(cl.condition, analysis);

    qSort(result.candidates);

    result.active.reserve(result.candidates.size());
    result.checked.reserve(result.candidates.size());

    for (const auto &candidate: result.candidates)
    {
        QTreeWidgetItem *node = nullptr;
        try
        {
            node = objectMap.at(candidate);
        } catch (const std::out_of_range &)
        {}

        result.active.push_back(analysis->getConditionLink(candidate) == cl);
        result.checked.push_back(node && node->data(0, Qt::CheckStateRole) == Qt::Checked);
    }

    assert(result.candidates.size() == static_cast<int>(result.active.size()));
    assert(result.active.size() == result.checked.size());

    return result;
}

} // end anon namespace

ObjectTree::~ObjectTree()
{
    qDebug() << __PRETTY_FUNCTION__ << this;
}

MVMEContext *ObjectTree::getContext() const
{
    assert(getEventWidget());
    return getEventWidget()->getContext();
}

Analysis *ObjectTree::getAnalysis() const
{
    assert(getContext());
    return getContext()->getAnalysis();
}

void ObjectTree::dropEvent(QDropEvent *event)
{
    /* Avoid calling the QTreeWidget reimplementation which handles internal moves
     * specially. Instead pass through to the QAbstractItemView base. */
    QAbstractItemView::dropEvent(event);
}

void ObjectTree::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Copy))
    {
        auto selectedTopLevelObjects = getEventWidget()->getTopLevelSelectedObjects();
        getEventWidget()->copyToClipboard(selectedTopLevelObjects);
    }
    else if (event->matches(QKeySequence::Paste))
    {
        getEventWidget()->pasteFromClipboard(this);
    }
    else
    {
        QTreeWidget::keyPressEvent(event);
    }
}

QList<QTreeWidgetItem *> ObjectTree::getTopLevelSelectedNodes() const
{
    QList<QTreeWidgetItem *> result;

    auto nodes = selectedItems();

    for (auto node: nodes)
    {
        if (nodes.contains(node->parent()))
            continue;

        result.push_back(node);
    }

    return result;
}

Qt::DropActions ObjectTree::supportedDropActions() const
{
    return Qt::MoveAction;
}

// DataSourceTree

DataSourceTree::~DataSourceTree()
{
    //qDebug() << __PRETTY_FUNCTION__ << this;
}

QStringList DataSourceTree::mimeTypes() const
{
    return { DataSourceIdListMIMEType };
}

QMimeData *DataSourceTree::mimeData(const QList<QTreeWidgetItem *> items) const
{
    QVector<QByteArray> idData;

    for (auto item: items)
    {
        switch (item->type())
        {
            case NodeType_Source:
                if (auto source = get_pointer<SourceInterface>(item, DataRole_AnalysisObject))
                {
                    idData.push_back(source->getId().toByteArray());
                }
                break;

            default:
                break;
        }
    }

    QByteArray buffer;
    QDataStream stream(&buffer, QIODevice::WriteOnly);
    stream << idData;

    auto result = new QMimeData;
    result->setData(DataSourceIdListMIMEType, buffer);

    return result;
}

bool DataSourceTree::dropMimeData(QTreeWidgetItem *parentItem,
                                  int parentIndex,
                                  const QMimeData *data,
                                  Qt::DropAction action)
{
    /* Drag and drop of datasources:
     * If dropped onto the tree or onto unassignedDataSourcesRoot the sources are removed
     * from their module and end up being unassigned.
     * If dropped onto a module the selected sources are (re)assigned to that module.
     */

    const auto mimeType = DataSourceIdListMIMEType;

    if (action != Qt::MoveAction)
        return false;

    if (!data->hasFormat(mimeType))
        return false;

    auto ids = decode_id_list(data->data(mimeType));

    if (ids.isEmpty())
        return false;

    bool didModify = false;
    auto analysis = getEventWidget()->getContext()->getAnalysis();

    check_directory_consistency(analysis->getDirectories(), analysis);

    AnalysisObjectVector droppedObjects;
    droppedObjects.reserve(ids.size());

    if (!parentItem || parentItem == unassignedDataSourcesRoot)
    {
        // move from module to unassigned

        AnalysisPauser pauser(getContext());

        for (auto &id: ids)
        {
            if (auto source = analysis->getSource(id))
            {
                qDebug() << __PRETTY_FUNCTION__ <<
                    "removing module assignment from data source " << source.get();

                source->setModuleId(QUuid());
                analysis->setSourceEdited(source);
                droppedObjects.append(source);
            }
        }

        didModify = true;
    }
    else if (parentItem && parentItem->type() == NodeType_Module)
    {
        // assign to module

        auto module = qobject_cast<ModuleConfig *>(get_qobject(parentItem, DataRole_RawPointer));
        assert(module);

        AnalysisPauser pauser(getContext());

        for (auto &id: ids)
        {
            if (auto source = analysis->getSource(id))
            {
                qDebug() << __PRETTY_FUNCTION__
                    << "assigning source " << source.get() << " to module " << module;

                source->setModuleId(module->getId());
                analysis->setSourceEdited(source);
                droppedObjects.append(source);
            }
        }

        didModify = true;
        // HACK: rebuild analysis here so that the changes are visible to the repopulate()
        // call below. If this is not done and the analysis is idle the pauser won't issue
        // the call to MVMEStreamWorker::start() and thus the analysis won't be rebuilt
        // until the next DAQ/replay start. Event then the UI won't be updated as it
        // doesn't know that the structure changed.
        // This is a systematic problem: the rebuild in the streamworker thread can cause
        // changes which means the GUI should be updated, but the GUI will never know.
        analysis->beginRun(Analysis::KeepState);
    }

    check_directory_consistency(analysis->getDirectories(), analysis);

    if (didModify)
    {
        analysis->setModified();
        getEventWidget()->repopulate();
        getEventWidget()->selectObjects(droppedObjects);
    }

    /* Returning false here to circumvent a crash which seems to be caused by Qt updating
     * the source of the drop operation which cannot work as the tree is rebuilt in
     * repopulate(). */
    return false;
}

// OperatorTree

OperatorTree::~OperatorTree()
{
    //qDebug() << __PRETTY_FUNCTION__ << this;
}

QStringList OperatorTree::mimeTypes() const
{
    return { OperatorIdListMIMEType };
}

QMimeData *OperatorTree::mimeData(const QList<QTreeWidgetItem *> nodes) const
{
    QVector<QByteArray> idData;

    for (auto node: nodes)
    {
        // Skip non top-level nodes
        if (nodes.contains(node->parent()))
            continue;

        switch (node->type())
        {
            case NodeType_Operator:
                {
                    if (auto op = get_pointer<OperatorInterface>(node, DataRole_AnalysisObject))
                    {
                        idData.push_back(op->getId().toByteArray());
                    }
                } break;

            case NodeType_Directory:
                {
                    if (auto dir = get_pointer<Directory>(node, DataRole_AnalysisObject))
                    {
                        idData.push_back(dir->getId().toByteArray());
                    }
                } break;

            default:
                break;
        }
    }

    QByteArray buffer;
    QDataStream stream(&buffer, QIODevice::WriteOnly);
    stream << idData;

    auto result = new QMimeData;
    result->setData(OperatorIdListMIMEType, buffer);

    return result;
}

bool OperatorTree::dropMimeData(QTreeWidgetItem *parentItem,
                                int parentIndex,
                                const QMimeData *data,
                                Qt::DropAction action)
{
    /* Note: This code assumes that only top level items are passed in via the mime data
     * object. OperatorTree::mimeData() guarantees this. */

    const auto mimeType = OperatorIdListMIMEType;

    if (action != Qt::MoveAction)
        return false;

    if (!data->hasFormat(mimeType))
        return false;

    auto ids = decode_id_list(data->data(mimeType));

    if (ids.isEmpty())
        return false;

    DirectoryPtr destDir;

    // Test if the drop is on top of a directory.
    if (parentItem && parentItem->type() == NodeType_Directory)
    {
        destDir = std::dynamic_pointer_cast<Directory>(
            get_pointer<Directory>(parentItem, DataRole_AnalysisObject)->shared_from_this());
    }

    auto analysis = getEventWidget()->getContext()->getAnalysis();

    check_directory_consistency(analysis->getDirectories(), analysis);

    AnalysisObjectSet dropSet;

    for (const auto &id: ids)
    {
        if (auto obj = analysis->getObject(id))
            dropSet.insert(obj);
    }

    if (dropSet.isEmpty()) return false;

    AnalysisObjectVector movedObjects;
    movedObjects.reserve(dropSet.size());

    const s32 destUserLevel = getUserLevel();

    for (auto &obj: dropSet)
    {
        const s32 levelDelta = destUserLevel - obj->getUserLevel();

        obj->setUserLevel(destUserLevel);

        movedObjects.append(obj);

        if (auto sourceDir = analysis->getParentDirectory(obj))
        {
            sourceDir->remove(obj);
        }

        if (destDir)
        {
            destDir->push_back(obj);
        }

        if (auto op = std::dynamic_pointer_cast<OperatorInterface>(obj))
        {
            for (auto &depRaw: collect_dependent_operators(op, CollectFlags::Operators))
            {
                auto dep = depRaw->shared_from_this();

                // avoid adjusting the same object multiple times
                if (dropSet.contains(dep))
                    continue;

#if 0
                // This code retains relative userlevel differences.
                s32 level = std::max(0, dep->getUserLevel() + levelDelta);
#else
                // This code sets the fixed destUserLevel on dependencies aswell
                s32 level = destUserLevel;
#endif

                dep->setUserLevel(level);
                movedObjects.append(dep);
            }
        }
        else if (auto dir = std::dynamic_pointer_cast<Directory>(obj))
        {
            /* NOTE: the dependees of operators inside the directory would need to have
             * their userlevel adjusted to maintain the "flow from left-to-right"
             * semantics.
             *
             * Doing the adjustment will create a problem if they have a parent
             * directory. The directory will have contents in multiple
             * userlevels and probably show up in multiple places.
             * This case should be avoided and probably detected and handled
             * somehow.
             *
             * Skipping the adjustment can lead to an operator arrangement
             * that's not supposed to be allowed. The user can still manually
             * fix that though.
             *
             * For now the adjustment is simply skipped and the user has to rearrange
             * things if they broke them.
             */

            auto childObjects = analysis->getDirectoryContentsRecursively(dir);

            for (auto &childObject: childObjects)
            {
                childObject->setUserLevel(destUserLevel);
                movedObjects.append(childObject);
            }
        }
    }

    check_directory_consistency(analysis->getDirectories(), analysis);

    analysis->setModified();
    auto eventWidget = getEventWidget();
    eventWidget->repopulate();
    eventWidget->selectObjects(movedObjects);

    if (destDir)
    {
        if (auto node = eventWidget->findNode(destDir))
        {
            node->setExpanded(true);
        }
    }

    return false;
}

// SinkTree

SinkTree::~SinkTree()
{
    //qDebug() << __PRETTY_FUNCTION__ << this;
}

QStringList SinkTree::mimeTypes() const
{
    return { SinkIdListMIMEType };
}

QMimeData *SinkTree::mimeData(const QList<QTreeWidgetItem *> nodes) const
{
    //qDebug() << __PRETTY_FUNCTION__ << this;

    QVector<QByteArray> idData;

    for (auto node: nodes)
    {
        // Skip non top-level nodes
        if (nodes.contains(node->parent()))
            continue;

        switch (node->type())
        {
            case NodeType_Histo1DSink:
            case NodeType_Histo2DSink:
            case NodeType_Sink:
                {
                    if (auto op = get_pointer<OperatorInterface>(node, DataRole_AnalysisObject))
                    {
                        idData.push_back(op->getId().toByteArray());
                    }
                } break;

            case NodeType_Directory:
                {
                    if (auto dir = get_pointer<Directory>(node, DataRole_AnalysisObject))
                    {
                        idData.push_back(dir->getId().toByteArray());
                    }
                } break;

            default:
                break;
        }
    }

    QByteArray buffer;
    QDataStream stream(&buffer, QIODevice::WriteOnly);
    stream << idData;

    auto result = new QMimeData;
    result->setData(SinkIdListMIMEType, buffer);

    return result;
}

bool SinkTree::dropMimeData(QTreeWidgetItem *parentItem,
                            int parentIndex,
                            const QMimeData *data,
                            Qt::DropAction action)
{
    qDebug() << __PRETTY_FUNCTION__ << this;

    const auto mimeType = SinkIdListMIMEType;

    if (getUserLevel() == 0)
        return false;

    if (action != Qt::MoveAction)
        return false;

    if (!data->hasFormat(mimeType))
        return false;

    auto ids = decode_id_list(data->data(mimeType));

    if (ids.isEmpty())
        return false;

    DirectoryPtr destDir;

    if (parentItem && parentItem->type() == NodeType_Directory)
    {
        destDir = std::dynamic_pointer_cast<Directory>(
            get_pointer<Directory>(parentItem, DataRole_AnalysisObject)->shared_from_this());
    }

    bool didModify = false;
    auto analysis = getEventWidget()->getContext()->getAnalysis();

    check_directory_consistency(analysis->getDirectories(), analysis);

    AnalysisObjectVector droppedObjects;
    droppedObjects.reserve(ids.size());

    for (auto &id: ids)
    {
        auto obj = analysis->getObject(id);
        droppedObjects.append(obj);

        if (auto sourceDir = analysis->getParentDirectory(obj))
        {
            sourceDir->remove(obj);
        }

        if (destDir)
        {
            destDir->push_back(obj);
        }

        obj->setUserLevel(getUserLevel());

        if (auto dir = analysis->getDirectory(id))
        {
            auto childObjects = analysis->getDirectoryContentsRecursively(dir);

            for (auto &childObject: childObjects)
            {
                childObject->setUserLevel(getUserLevel());
            }
        }

        didModify = true;
    }

    check_directory_consistency(analysis->getDirectories(), analysis);

    if (didModify)
    {
        analysis->setModified();
        auto eventWidget = getEventWidget();
        eventWidget->repopulate();
        eventWidget->selectObjects(droppedObjects);

        if (destDir)
        {
            if (auto node = eventWidget->findNode(destDir))
            {
                node->setExpanded(true);
            }
        }
    }

    return false;
}

namespace
{

template<typename T>
TreeNode *make_node(T *data, int type = QTreeWidgetItem::Type, int dataRole = DataRole_RawPointer)
{
    auto ret = new TreeNode(type);
    ret->setData(0, dataRole, QVariant::fromValue(static_cast<void *>(data)));
    ret->setFlags(ret->flags() & ~(Qt::ItemIsDropEnabled | Qt::ItemIsDragEnabled));
    return ret;
}

inline TreeNode *make_module_node(ModuleConfig *mod)
{
    auto node = make_node(mod, NodeType_Module, DataRole_RawPointer);
    node->setText(0, mod->objectName());
    node->setIcon(0, QIcon(":/vme_module.png"));
    node->setFlags(node->flags() | Qt::ItemIsDropEnabled);
    return node;
};

static QIcon make_datasource_icon(SourceInterface *source)
{
    QIcon icon;

    if (qobject_cast<ListFilterExtractor *>(source))
        icon = QIcon(":/listfilter.png");

    if (qobject_cast<Extractor *>(source))
        icon = QIcon(":/data_filter.png");

    return icon;
}

inline TreeNode *make_datasource_node(SourceInterface *source)
{
    auto sourceNode = make_node(source, NodeType_Source, DataRole_AnalysisObject);
    sourceNode->setData(0, Qt::DisplayRole, source->objectName());
    sourceNode->setData(0, Qt::EditRole, source->objectName());
    sourceNode->setFlags(sourceNode->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled);
    sourceNode->setIcon(0, make_datasource_icon(source));

    Q_ASSERT_X(source->getNumberOfOutputs() == 1,
               "make_datasource_node",
               "data sources with multiple output pipes not supported");

    if (source->getNumberOfOutputs() == 1)
    {
        Pipe *outputPipe = source->getOutput(0);
        s32 addressCount = outputPipe->parameters.size();

        for (s32 address = 0; address < addressCount; ++address)
        {
            auto addressNode = make_node(outputPipe, NodeType_OutputPipeParameter, DataRole_RawPointer);
            addressNode->setData(0, DataRole_ParameterIndex, address);
            addressNode->setText(0, QString::number(address));
            sourceNode->addChild(addressNode);
        }
    }

    return sourceNode;
}

static QIcon make_operator_icon(OperatorInterface *op)
{
    // operators
    if (qobject_cast<CalibrationMinMax *>(op))
        return QIcon(":/operator_calibration.png");

    if (qobject_cast<Difference *>(op))
        return QIcon(":/operator_difference.png");

    if (qobject_cast<PreviousValue *>(op))
        return QIcon(":/operator_previous.png");

    if (qobject_cast<Sum *>(op))
        return QIcon(":/operator_sum.png");

    if (qobject_cast<ExpressionOperator *>(op))
        return QIcon(":/function.png");

    // sinks
    if (qobject_cast<Histo1DSink *>(op))
        return QIcon(":/hist1d.png");

    if (qobject_cast<Histo2DSink *>(op))
        return QIcon(":/hist2d.png");

    if (qobject_cast<RateMonitorSink *>(op))
        return QIcon(":/rate_monitor_sink.png");

    // catchall for sinks
    if (qobject_cast<SinkInterface *>(op))
        return QIcon(":/sink.png");

    return QIcon(":/operator_generic.png");
}

inline TreeNode *make_histo1d_node(Histo1DSink *sink)
{
    auto node = make_node(sink, NodeType_Histo1DSink, DataRole_AnalysisObject);

    node->setData(0, Qt::EditRole, sink->objectName());
    node->setData(0, Qt::DisplayRole, QString("<b>%1</b> %2").arg(
            sink->getShortName(),
            sink->objectName()));


    node->setIcon(0, make_operator_icon(sink));
    node->setFlags(node->flags() | Qt::ItemIsEditable);

    if (sink->m_histos.size() > 0)
    {
        for (s32 addr = 0; addr < sink->m_histos.size(); ++addr)
        {
            auto histo = sink->m_histos[addr].get();
            auto histoNode = make_node(histo, NodeType_Histo1D, DataRole_RawPointer);
            histoNode->setData(0, DataRole_HistoAddress, addr);
            histoNode->setText(0, QString::number(addr));
            node->setIcon(0, make_operator_icon(sink));

            node->addChild(histoNode);
        }
    }

    return node;
};

inline TreeNode *make_histo2d_node(Histo2DSink *sink)
{
    auto node = make_node(sink, NodeType_Histo2DSink, DataRole_AnalysisObject);
    node->setData(0, Qt::EditRole, sink->objectName());
    node->setData(0, Qt::DisplayRole, QString("<b>%1</b> %2").arg(
            sink->getShortName(),
            sink->objectName()));
    node->setIcon(0, make_operator_icon(sink));
    node->setFlags(node->flags() | Qt::ItemIsEditable);

    return node;
}

inline TreeNode *make_sink_node(SinkInterface *sink)
{
    auto node = make_node(sink, NodeType_Sink, DataRole_AnalysisObject);
    node->setData(0, Qt::EditRole, sink->objectName());
    node->setData(0, Qt::DisplayRole, QString("<b>%1</b> %2").arg(
            sink->getShortName(),
            sink->objectName()));
    node->setIcon(0, make_operator_icon(sink));
    node->setFlags(node->flags() | Qt::ItemIsEditable);

    return node;
}

inline TreeNode *make_operator_node(OperatorInterface *op)
{
    auto result = make_node(op, NodeType_Operator, DataRole_AnalysisObject);

    result->setData(0, Qt::EditRole, op->objectName());
    result->setData(0, Qt::DisplayRole, QString("<b>%1</b> %2").arg(
            op->getShortName(),
            op->objectName()));

    result->setIcon(0, make_operator_icon(op));
    result->setFlags(result->flags() | Qt::ItemIsEditable);

    // outputs
    for (s32 outputIndex = 0;
         outputIndex < op->getNumberOfOutputs();
         ++outputIndex)
    {
        Pipe *outputPipe = op->getOutput(outputIndex);
        s32 outputParamSize = outputPipe->parameters.size();

        auto pipeNode = make_node(outputPipe, NodeType_OutputPipe, DataRole_RawPointer);
        pipeNode->setText(0, QString("#%1 \"%2\" (%3 elements)")
                          .arg(outputIndex)
                          .arg(op->getOutputName(outputIndex))
                          .arg(outputParamSize)
                         );
        result->addChild(pipeNode);

        for (s32 paramIndex = 0; paramIndex < outputParamSize; ++paramIndex)
        {
            auto paramNode = make_node(outputPipe, NodeType_OutputPipeParameter, DataRole_RawPointer);
            paramNode->setData(0, DataRole_ParameterIndex, paramIndex);
            paramNode->setText(0, QString("[%1]").arg(paramIndex));

            pipeNode->addChild(paramNode);
        }
    }

    return result;
};

inline TreeNode *make_directory_node(const DirectoryPtr &dir)
{
    auto result = make_node(dir.get(), NodeType_Directory, DataRole_AnalysisObject);

    result->setText(0, dir->objectName());
    result->setIcon(0, QIcon(QSL(":/folder_orange.png")));
    result->setFlags(result->flags()
                     | Qt::ItemIsDropEnabled
                     | Qt::ItemIsDragEnabled
                     | Qt::ItemIsEditable
                     );

    return result;
}

void add_directory_nodes(ObjectTree *tree, const DirectoryPtr &dir,
                         QHash<DirectoryPtr, TreeNode *> &nodes,
                         Analysis *analysis)
{
    if (nodes.contains(dir)) return;

    auto node = make_directory_node(dir);

    if (auto parent = analysis->getParentDirectory(dir))
    {
        add_directory_nodes(tree, parent, nodes, analysis);
        auto parentNode = nodes.value(parent);
        assert(parentNode);
        parentNode->addChild(node);
    }
    else
    {
        tree->addTopLevelItem(node);
    }

    nodes.insert(dir, node);
}

void add_directory_nodes(ObjectTree *tree, const DirectoryVector &dirs,
                         QHash<DirectoryPtr, TreeNode *> &nodes,
                         Analysis *analysis)
{
    for (const auto &dir: dirs)
    {
        add_directory_nodes(tree, dir, nodes, analysis);
    }
}



ObjectEditorDialog *datasource_editor_factory(const SourcePtr &src, s32 userLevel,
                                              ObjectEditorMode mode,
                                              ModuleConfig *moduleConfig,
                                              EventWidget *eventWidget)
{
    ObjectEditorDialog *result = nullptr;

    if (auto ex = std::dynamic_pointer_cast<Extractor>(src))
    {
        result = new AddEditExtractorDialog(ex, moduleConfig, mode, eventWidget);

    }
    else if (auto ex = std::dynamic_pointer_cast<ListFilterExtractor>(src))
    {
        auto context = eventWidget->getContext();
        auto analysis = context->getAnalysis();

        auto lfe_dialog = new ListFilterExtractorDialog(moduleConfig, analysis, context, eventWidget);
        result = lfe_dialog;

        if (!analysis->getListFilterExtractors(
                moduleConfig->getEventId(), moduleConfig->getId()).isEmpty())
        {
            lfe_dialog->newFilter();
        }
    }

    QObject::connect(result, &ObjectEditorDialog::applied,
                     eventWidget, &EventWidget::objectEditorDialogApplied);

    QObject::connect(result, &QDialog::accepted,
                     eventWidget, &EventWidget::objectEditorDialogAccepted);

    QObject::connect(result, &QDialog::rejected,
                     eventWidget, &EventWidget::objectEditorDialogRejected);

    return result;
}

ObjectEditorDialog *operator_editor_factory(const OperatorPtr &op,
                                            s32 userLevel,
                                            ObjectEditorMode mode,
                                            const DirectoryPtr &destDir,
                                            EventWidget *eventWidget)
{
    ObjectEditorDialog *result = nullptr;

    if (auto expr = std::dynamic_pointer_cast<ExpressionOperator>(op))
    {
        result = new ExpressionOperatorDialog(expr, userLevel, mode, destDir, eventWidget);
    }
    else
    {
        result = new AddEditOperatorDialog(op, userLevel, mode, destDir, eventWidget);
    }

    QObject::connect(result, &ObjectEditorDialog::applied,
                     eventWidget, &EventWidget::objectEditorDialogApplied);

    QObject::connect(result, &QDialog::accepted,
                     eventWidget, &EventWidget::objectEditorDialogAccepted);

    QObject::connect(result, &QDialog::rejected,
                     eventWidget, &EventWidget::objectEditorDialogRejected);

    return result;
}

bool may_move_into(const AnalysisObject *obj, const Directory *destDir)
{
    assert(obj);
    assert(destDir);

    if (qobject_cast<SourceInterface *>(obj))
        return false;

    if (qobject_cast<SinkInterface *>(obj))
    {
        // "raw" sinks, have to stay in userlevel 0
        if (obj->getUserLevel() == 0)
            return false;

        return destDir->getDisplayLocation() == DisplayLocation::Sink;
    }

    if (qobject_cast<OperatorInterface *>(obj))
    {
        return destDir->getDisplayLocation() == DisplayLocation::Operator;
    }

    if (auto dir = qobject_cast<const Directory *>(obj))
    {
        return destDir->getDisplayLocation() == dir->getDisplayLocation();
    }

    return false;
}

bool may_move_into(const AnalysisObjectPtr &obj, const DirectoryPtr &destDir)
{
    return may_move_into(obj.get(), destDir.get());
}

void run_userlevel_visibility_dialog(QVector<bool> &hiddenLevels, QWidget *parent = 0)
{
    auto listWidget = new QListWidget;

    for (s32 idx = 0; idx < hiddenLevels.size(); ++idx)
    {
        auto item = new QListWidgetItem(QString("Level %1").arg(idx));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(hiddenLevels[idx] ? Qt::Unchecked : Qt::Checked);
        listWidget->addItem(item);
    }

    auto buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

    auto layout = new QVBoxLayout;
    layout->addWidget(listWidget);
    layout->addWidget(buttonBox);
    layout->setStretch(0, 1);

    QDialog dialog(parent);
    dialog.setWindowTitle(QSL("Select processing levels to show"));
    dialog.setLayout(layout);
    add_widget_close_action(&dialog);

    QObject::connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted)
    {
        for (s32 idx = 0; idx < listWidget->count(); ++idx)
        {
            auto item = listWidget->item(idx);
            hiddenLevels[idx] = (item->checkState() == Qt::Unchecked);
        }
    }
}

struct Histo1DWidgetInfo
{
    QVector<std::shared_ptr<Histo1D>> histos;
    s32 histoAddress;
    std::shared_ptr<CalibrationMinMax> calib;
    std::shared_ptr<Histo1DSink> sink;
};

Histo1DWidgetInfo getHisto1DWidgetInfoFromNode(QTreeWidgetItem *node)
{
    QTreeWidgetItem *sinkNode = nullptr;
    Histo1DWidgetInfo result;

    switch (node->type())
    {
        case NodeType_Histo1D:
            {
                Q_ASSERT(node->parent() && node->parent()->type() == NodeType_Histo1DSink);
                sinkNode = node->parent();
                result.histoAddress = node->data(0, DataRole_HistoAddress).toInt();
            } break;

        case NodeType_Histo1DSink:
            {
                sinkNode = node;
                result.histoAddress = 0;
            } break;

        InvalidDefaultCase;
    }

    auto histoSink = get_pointer<Histo1DSink>(sinkNode, DataRole_AnalysisObject);
    result.histos = histoSink->m_histos;
    result.sink = std::dynamic_pointer_cast<Histo1DSink>(histoSink->shared_from_this());

    // Check if the histosinks input is a CalibrationMinMax
    if (Pipe *sinkInputPipe = histoSink->getSlot(0)->inputPipe)
    {
        if (auto calibRaw = qobject_cast<CalibrationMinMax *>(sinkInputPipe->getSource()))
        {
            result.calib = std::dynamic_pointer_cast<CalibrationMinMax>(calibRaw->shared_from_this());
        }
    }

    return result;
}

static const QColor ValidInputNodeColor         = QColor("lightgreen");
static const QColor InputNodeOfColor            = QColor(0x90, 0xEE, 0x90, 255.0/3); // lightgreen but with some alpha
static const QColor ChildIsInputNodeOfColor     = QColor(0x90, 0xEE, 0x90, 255.0/6);

static const QColor OutputNodeOfColor           = QColor(0x00, 0x00, 0xCD, 255.0/3); // mediumblue with some alpha
static const QColor ChildIsOutputNodeOfColor    = QColor(0x00, 0x00, 0xCD, 255.0/6);

static const QColor MissingInputColor           = QColor(0xB2, 0x22, 0x22, 255.0/3); // firebrick with some alpha

static const u32 PeriodicUpdateTimerInterval_ms = 1000;

} // end anon namespace

EventWidget::EventWidget(MVMEContext *ctx, const QUuid &eventId, int eventIndex,
                         AnalysisWidget *analysisWidget, QWidget *parent)
    : QWidget(parent)
    , m_d(std::make_unique<EventWidgetPrivate>())
{
    qDebug() << __PRETTY_FUNCTION__ << this << "event =" << eventId;
    *m_d = {};
    m_d->m_q = this;
    m_d->m_context = ctx;
    m_d->m_eventId = eventId;
    m_d->m_eventIndex = eventIndex;
    m_d->m_analysisWidget = analysisWidget;
    m_d->m_displayRefreshTimer = new QTimer(this);
    m_d->m_displayRefreshTimer->start(PeriodicUpdateTimerInterval_ms);

    auto outerLayout = new QHBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    // Row frames and splitter:
    // Two rows, the top one containing Modules and Operators, the bottom one
    // containing histograms.
    auto rowSplitter = new QSplitter(Qt::Vertical);
    outerLayout->addWidget(rowSplitter);

    auto operatorFrame = new QFrame;
    auto operatorFrameLayout = new QHBoxLayout(operatorFrame);
    operatorFrameLayout->setContentsMargins(2, 2, 2, 2);
    rowSplitter->addWidget(operatorFrame);

    auto displayFrame = new QFrame;
    auto displayFrameLayout = new QHBoxLayout(displayFrame);
    displayFrameLayout->setContentsMargins(2, 2, 2, 2);
    rowSplitter->addWidget(displayFrame);

    // Column frames and splitters:
    // One column for each user level
    m_d->m_operatorFrameSplitter = new QSplitter;
    m_d->m_operatorFrameSplitter->setChildrenCollapsible(false);
    operatorFrameLayout->addWidget(m_d->m_operatorFrameSplitter);

    m_d->m_displayFrameSplitter = new QSplitter;
    m_d->m_displayFrameSplitter->setChildrenCollapsible(false);
    displayFrameLayout->addWidget(m_d->m_displayFrameSplitter);

    auto sync_splitters = [](QSplitter *sa, QSplitter *sb)
    {
        auto sync_one_way = [](QSplitter *src, QSplitter *dst)
        {
            connect(src, &QSplitter::splitterMoved, dst, [src, dst](int, int) {
                dst->setSizes(src->sizes());
            });
        };

        sync_one_way(sa, sb);
        sync_one_way(sb, sa);
    };

    sync_splitters(m_d->m_operatorFrameSplitter, m_d->m_displayFrameSplitter);


    /* ToolBar creation. Note that these toolbars are not directly added to the
     * widget but instead they're handled by AnalysisWidget via getToolBar()
     * and getEventSelectAreaToolBar(). */

    // Upper ToolBar actions

    // create the upper toolbar
    {
        m_d->m_upperToolBar = make_toolbar();
        auto tb = m_d->m_upperToolBar;

        //tb->addWidget(new QLabel(QString("Hello, event! %1").arg((uintptr_t)this)));
    }

    // Lower ToolBar, to the right of the event selection combo
    m_d->m_actionSelectVisibleLevels = new QAction(QIcon(QSL(":/eye_pencil.png")),
                                                   QSL("Level Visiblity"), this);

    connect(m_d->m_actionSelectVisibleLevels, &QAction::triggered, this, [this] {

        m_d->m_hiddenUserLevels.resize(m_d->m_levelTrees.size());

        run_userlevel_visibility_dialog(m_d->m_hiddenUserLevels, this);

        for (s32 idx = 0; idx < m_d->m_hiddenUserLevels.size(); ++idx)
        {
            m_d->m_levelTrees[idx].operatorTree->setVisible(!m_d->m_hiddenUserLevels[idx]);
            m_d->m_levelTrees[idx].sinkTree->setVisible(!m_d->m_hiddenUserLevels[idx]);
        }
    });

    // Export
    {
        auto action = new QAction(QIcon(QSL(":/folder_export.png")), QSL("Export"), this);
        action->setToolTip(QSL("Export selected objects to file."));
        action->setStatusTip(action->toolTip());
        m_d->m_actionExport = action;
        connect(m_d->m_actionExport, &QAction::triggered, this, [this] {
            m_d->actionExport();
        });
    }

    // Import
    {
        auto action = new QAction(QIcon(QSL(":/folder_import.png")), QSL("Import"), this);
        action->setToolTip(QSL("Import objects from file."));
        action->setStatusTip(action->toolTip());
        m_d->m_actionImport = action;
        connect(m_d->m_actionImport, &QAction::triggered, this, [this] {
            m_d->actionImport();
        });
    }

    // Event settings action
    QAction *actionEventSettings = new QAction(
        QIcon(QSL(":/gear.png")), QSL("Event settings"), this);

    connect(actionEventSettings, &QAction::triggered, this, [this] {
        auto analysis = m_d->m_context->getAnalysis();

        EventSettingsDialog dialog(analysis->getVMEObjectSettings(m_d->m_eventId), this);

        if (dialog.exec() == QDialog::Accepted)
        {
            analysis->setVMEObjectSettings(m_d->m_eventId, dialog.getSettings());
        }
    });

    m_d->m_eventRateLabel = new QLabel;

    // create the lower toolbar
    {
        m_d->m_eventSelectAreaToolBar = make_toolbar();
        auto tb = m_d->m_eventSelectAreaToolBar;

        tb->setIconSize(QSize(16, 16));
        tb->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);

        tb->addWidget(m_d->m_eventRateLabel);

        tb->addSeparator();

        tb->addAction(m_d->m_actionSelectVisibleLevels);
        tb->addAction(actionEventSettings);

        tb->addSeparator();

        tb->addAction(m_d->m_actionExport);
        tb->addAction(m_d->m_actionImport);

        tb->addSeparator();

        tb->addAction(QSL("Conditions/Cuts"), this, [this]() {
            if (auto w = getAnalysisWidget()->getConditionWidget())
            {
                w->show();
                w->raise();
            }
        });

#ifndef QT_NO_DEBUG
        tb->addSeparator();
        tb->addAction(QSL("Repopulate (dev)"), this, [this]() {
            m_d->repopulate();
        });
#endif
    }

    m_d->repopulate();
}

EventWidget::~EventWidget()
{
    qDebug() << __PRETTY_FUNCTION__ << this << "event =" << m_d->m_eventId;

    if (m_d->m_uniqueWidget)
    {
        if (auto dialog = qobject_cast<QDialog *>(m_d->m_uniqueWidget))
        {
            dialog->reject();
        }
    }
}

void EventWidget::selectInputFor(Slot *slot, s32 userLevel, SelectInputCallback callback,
                                 QSet<PipeSourceInterface *> additionalInvalidSources)
{
    qDebug() << __PRETTY_FUNCTION__;

    m_d->m_inputSelectInfo.slot = slot;
    m_d->m_inputSelectInfo.userLevel = userLevel;
    m_d->m_inputSelectInfo.callback = callback;
    m_d->m_inputSelectInfo.additionalInvalidSources = additionalInvalidSources;

    //qDebug() << __PRETTY_FUNCTION__ << "additionalInvalidSources ="
    //    << additionalInvalidSources;

    m_d->m_mode = EventWidgetPrivate::SelectInput;
    m_d->setMode(EventWidgetPrivate::SelectInput);
    // The actual input selection is handled in onNodeClicked()
}

void EventWidget::endSelectInput()
{
    if (m_d->m_mode == EventWidgetPrivate::SelectInput)
    {
        qDebug() << __PRETTY_FUNCTION__ << "switching from SelectInput to Default mode";
        m_d->m_inputSelectInfo = {};
        m_d->setMode(EventWidgetPrivate::Default);
    }
}

void EventWidget::highlightInputOf(Slot *slot, bool doHighlight)
{
    if (!slot || !slot->isParamIndexInRange())
        return;

    QTreeWidgetItem *node = nullptr;

    if (auto source = qobject_cast<SourceInterface *>(slot->inputPipe->getSource()))
    {
        // As the input is a SourceInterface we only need to look in the source tree
        auto tree = m_d->m_levelTrees[0].operatorTree;

        node = findFirstNode(tree->invisibleRootItem(), [source](auto nodeToTest) {
            return (nodeToTest->type() == NodeType_Source
                    && get_pointer<SourceInterface>(nodeToTest, DataRole_AnalysisObject) == source);
        });

    }
    else if (qobject_cast<OperatorInterface *>(slot->inputPipe->getSource()))
    {
        // The input is another operator
        for (s32 treeIndex = 1;
             treeIndex < m_d->m_levelTrees.size() && !node;
             ++treeIndex)
        {
            auto tree = m_d->m_levelTrees[treeIndex].operatorTree;

            node = findFirstNode(tree->invisibleRootItem(), [slot](auto nodeToTest) {
                return (nodeToTest->type() == NodeType_OutputPipe
                        && get_pointer<Pipe>(nodeToTest, DataRole_RawPointer) == slot->inputPipe);
            });
        }
    }
    else
    {
        InvalidCodePath;
    }

    if (node && slot->isParameterConnection() && slot->paramIndex < node->childCount())
    {
        node = node->child(slot->paramIndex);
    }

    if (node)
    {
        auto highlight_node = [doHighlight](QTreeWidgetItem *node, const QColor &color)
        {
            if (doHighlight)
            {
                node->setBackground(0, color);
            }
            else
            {
                node->setBackground(0, QColor(0, 0, 0, 0));
            }
        };

        highlight_node(node, InputNodeOfColor);

        for (node = node->parent();
             node;
             node = node->parent())
        {
            highlight_node(node, ChildIsInputNodeOfColor);
        }
    }
}

//
// Extractor add/edit/cancel
//
void EventWidget::objectEditorDialogApplied()
{
    qDebug() << __PRETTY_FUNCTION__;
    //endSelectInput(); // FIXME: needed?
    m_d->repopulate();
    m_d->m_analysisWidget->updateAddRemoveUserLevelButtons();
}

void EventWidget::objectEditorDialogAccepted()
{
    qDebug() << __PRETTY_FUNCTION__;
    //endSelectInput(); // FIXME: needed?
    uniqueWidgetCloses();
    m_d->repopulate();
    m_d->m_analysisWidget->updateAddRemoveUserLevelButtons();
}

void EventWidget::objectEditorDialogRejected()
{
    qDebug() << __PRETTY_FUNCTION__;
    //endSelectInput(); // FIXME: needed?
    uniqueWidgetCloses();
}

void EventWidget::onConditionLinkSelected(const ConditionLink &cl)
{
    if (m_d->getMode() != EventWidgetPrivate::Default) return;
    if (cl.condition->getEventId() != getEventId()) return;
    if (cl == m_d->m_applyConditionInfo) return;

    qDebug() << __PRETTY_FUNCTION__ << this << cl.condition.get() << cl.subIndex;

    const auto &condInfo = cl;
    assert(condInfo.condition);
    assert(condInfo.subIndex < condInfo.condition->getNumberOfBits());

    m_d->removeConditionDecorations(m_d->m_applyConditionInfo);

    m_d->m_applyConditionInfo = (cl.subIndex >= 0 ? cl : ConditionLink{});

    m_d->clearAllTreeSelections();
    m_d->clearAllToDefaultNodeHighlights();
    m_d->highlightInputNodes(cl.condition.get());
    m_d->updateNodesForApplyConditionMode();
}

void EventWidget::applyConditionAccept()
{
    qDebug() << __PRETTY_FUNCTION__ << this;

    /* Collect checked nodes, get operators from these nodes. Create a
     * condition link for each operator to the current conditionInfos condition
     * and index. Then rebuild the analysis.
     */

    /* XXX: By introducing and using the analysis modification signals the
     * following changed:
     * - checked and unchecked objects have to be fetched from the trees
     *   _before_ any changes are made to the analysis.
     * - This widget and its trees are recreated for each succesfull call to
     *   setConditionLink()/clearConditionLink()
     *
     * How to avoid excessive rebuilding when using granular signals like this?
     * Signals emitted by the analysis could be blocked here but then other
     * observers won't be notified of the changes.
     *
     * A notification wrapper instance could be used in-between this widget and
     * the analysis. Then signals would only be blocked in the local wrapper
     * instance without affecting other observers.
     *
     * Another way would be to implement a delayed repopulate/repaint where
     * only a flag is set in repopulate() and the actual repop is done elsewhere
     * at a later time and only once. But of course delayed updates will then
     * be the default way of doing things, even if sometimes a direct update is
     * desired.
     */
    auto analysis         = m_d->getAnalysis();
    auto checkedObjects   = m_d->getCheckedObjects();
    auto uncheckedObjects = m_d->getCheckedObjects(Qt::Unchecked);

    for (auto &obj: checkedObjects)
    {
        if (auto op = std::dynamic_pointer_cast<OperatorInterface>(obj))
        {
            bool modified = m_d->getAnalysis()->setConditionLink(
                op, m_d->m_applyConditionInfo);

            if (modified)
            {
                qDebug() << "set condition link for" << op.get();
                analysis->setModified(true);
            }
        }
    }

    for (auto &obj: uncheckedObjects)
    {
        if (auto op = std::dynamic_pointer_cast<OperatorInterface>(obj))
        {
            bool modified = m_d->getAnalysis()->clearConditionLink(
                op, m_d->m_applyConditionInfo);

            if (modified)
            {
                qDebug() << "cleared condition link for" << op.get();
                analysis->setModified(true);
            }
        }
    }

    AnalysisPauser pauser(getContext());
    analysis->beginRun(Analysis::KeepState);
}

void EventWidget::applyConditionReject()
{
    qDebug() << __PRETTY_FUNCTION__ << this;

    auto &aci = m_d->m_applyConditionInfo;

    if (aci)
    {
        auto analysis = getAnalysis();
        auto candidates = get_apply_condition_candidates(aci.condition, analysis);

        for (const auto &op: candidates)
        {
            if (auto node = m_d->m_objectMap[op])
            {
                node->setFlags(node->flags() & ~Qt::ItemIsUserCheckable);
                node->setData(0, Qt::CheckStateRole, QVariant());
            }
        }
    }

    m_d->updateNodesForApplyConditionMode();
}

void EventWidget::removeOperator(OperatorInterface *op)
{
    AnalysisPauser pauser(m_d->m_context);
    m_d->m_context->getAnalysis()->removeOperator(op);
    m_d->repopulate();
    m_d->m_analysisWidget->updateAddRemoveUserLevelButtons();
}

void EventWidget::toggleSinkEnabled(SinkInterface *sink)
{
    AnalysisPauser pauser(m_d->m_context);
    sink->setEnabled(!sink->isEnabled());
    m_d->m_context->getAnalysis()->setModified(true);
    m_d->repopulate();
}

void EventWidget::removeSource(SourceInterface *src)
{
    AnalysisPauser pauser(m_d->m_context);
    m_d->m_context->getAnalysis()->removeSource(src);
    m_d->repopulate();
}

void EventWidget::uniqueWidgetCloses()
{
    m_d->m_uniqueWidget = nullptr;
}

void EventWidget::addUserLevel()
{
    m_d->addUserLevel();
}

void EventWidget::removeUserLevel()
{
    m_d->removeUserLevel();
}

void EventWidget::repopulate()
{
    m_d->repopulate();
}

QToolBar *EventWidget::getToolBar()
{
    return m_d->m_upperToolBar;
}

QToolBar *EventWidget::getEventSelectAreaToolBar()
{
    return m_d->m_eventSelectAreaToolBar;
}

MVMEContext *EventWidget::getContext() const
{
    return m_d->m_context;
}

AnalysisWidget *EventWidget::getAnalysisWidget() const
{
    return m_d->m_analysisWidget;
}

Analysis *EventWidget::getAnalysis() const
{
    return m_d->m_context->getAnalysis();
}

RunInfo EventWidget::getRunInfo() const
{
    return getContext()->getRunInfo();
}

VMEConfig *EventWidget::getVMEConfig() const
{
    return getContext()->getVMEConfig();
}

QTreeWidgetItem *EventWidget::findNode(const AnalysisObjectPtr &obj)
{
    return m_d->findNode(obj);
}

QUuid EventWidget::getEventId() const
{
    return m_d->m_eventId;
}

void EventWidget::selectObjects(const AnalysisObjectVector &objects)
{
    m_d->selectObjects(objects);
}

AnalysisObjectVector EventWidget::getAllSelectedObjects() const
{
    return m_d->getAllSelectedObjects();
}

AnalysisObjectVector EventWidget::getTopLevelSelectedObjects() const
{
    return m_d->getTopLevelSelectedObjects();
}

void EventWidget::copyToClipboard(const AnalysisObjectVector &objects)
{
    m_d->copyToClipboard(objects);
}

void EventWidget::pasteFromClipboard(QTreeWidget *tree)
{
    m_d->pasteFromClipboard(tree);
}

QString mode_to_string(EventWidgetPrivate::Mode mode)
{
    switch (mode)
    {
        case EventWidgetPrivate::Default:
            return QSL("Default");

        case EventWidgetPrivate::SelectInput:
            return QSL("SelectInput");

        InvalidDefaultCase;
    }

    return QSL("");
}

void EventWidgetPrivate::pasteFromClipboard(QTreeWidget *destTree)
{
    if (!canPaste()) return;

    auto tree = qobject_cast<ObjectTree *>(destTree);
    assert(tree);

    if (!tree) return;

    DirectoryPtr destDir;

    if (tree->currentItem() && tree->currentItem()->type() == NodeType_Directory)
    {
        destDir = get_shared_analysis_object<Directory>(tree->currentItem(), DataRole_AnalysisObject);
    }

    const auto mimeType = ObjectIdListMIMEType;
    auto clipboardData = QGuiApplication::clipboard()->mimeData();
    auto ids = decode_id_list(clipboardData->data(mimeType));
    auto analysis = m_context->getAnalysis();

    check_directory_consistency(analysis->getDirectories(), analysis);

    AnalysisObjectVector srcObjects;
    srcObjects.reserve(ids.size());

    for (const auto &id: ids)
    {
        if (auto srcObject = analysis->getObject(id))
            srcObjects.push_back(srcObject);
    }

    if (srcObjects.isEmpty()) return;

    srcObjects = order_objects(expand_objects(srcObjects, analysis), analysis);

    // Maps source object to cloned object
    QHash<AnalysisObjectPtr, AnalysisObjectPtr> cloneMapping;
    AnalysisObjectVector cloneVector;

#ifndef QT_NO_DEBUG
    DirectoryVector clonedDirectories;
#define check_cloned_dirs check_directory_consistency(clonedDirectories)
#else
#define check_cloned_dirs
#endif

    for (const auto &srcObject: srcObjects)
    {
        auto clone = std::shared_ptr<AnalysisObject>(srcObject->clone());
        cloneMapping.insert(srcObject, clone);
        cloneVector.push_back(clone);

#ifndef QT_NO_DEBUG
        if (auto dir = std::dynamic_pointer_cast<Directory>(clone))
        {
            clonedDirectories.push_back(dir);
            assert(dir->getMembers().isEmpty());
        }
#endif
    }

    check_cloned_dirs;

    auto namesByMetaType = group_object_names_by_metatype(analysis->getAllObjects());

    for (auto it = cloneMapping.begin();
         it != cloneMapping.end();
         it++)
    {
        auto &src   = it.key();
        auto &clone = it.value();

        auto cloneName = make_clone_name(clone->objectName(),
                                         namesByMetaType[clone->metaObject()]);
        clone->setObjectName(cloneName);
        namesByMetaType[clone->metaObject()].insert(cloneName);

        if (!(qobject_cast<SinkInterface *>(clone.get()) && clone->getUserLevel() == 0))
        {
            // Objects other than non-raw sinks have their userlevel adjusted
            clone->setUserLevel(tree->getUserLevel());
        }

        if (auto dataSource = std::dynamic_pointer_cast<SourceInterface>(clone))
        {
            // Remove cloned data sources from their module, making them unassigned.
            dataSource->setModuleId(QUuid());
        }

        if (auto srcParentDir = analysis->getParentDirectory(src))
        {
            // The source has a parent directory. Put the clone into the equivalent cloned
            // directory.
            if (auto cloneParentDir = std::dynamic_pointer_cast<Directory>(
                    cloneMapping.value(srcParentDir)))
            {
                cloneParentDir->push_back(clone);
                check_cloned_dirs;
            }
            else if (destDir && may_move_into(clone, destDir))
            {
                destDir->push_back(clone);
                check_cloned_dirs;
            }
        }
        else if (destDir && may_move_into(clone, destDir))
        {
            // The source object does not have a parent directory, meaning it's a
            // top-level item.
            // If pasting into a directory all the top-level clones have to be moved.
            destDir->push_back(clone);
            check_cloned_dirs;
        }
    }

    check_cloned_dirs;
    check_directory_consistency(analysis->getDirectories(), analysis);

    // Collect, rewrite and restore internal collections of the cloned objects
    QSet<Connection> srcConnections = collect_internal_collections(srcObjects);
    QSet<Connection> dstConnections;

    for (auto con: srcConnections)
    {
        auto srcClone = std::dynamic_pointer_cast<PipeSourceInterface>(
            cloneMapping.value(con.srcObject));

        auto dstClone = std::dynamic_pointer_cast<OperatorInterface>(
            cloneMapping.value(con.dstObject));

        if (srcClone && dstClone)
        {
            con.srcObject = srcClone;
            con.dstObject = dstClone;

            dstConnections.insert(con);
        }
    }

    establish_connections(dstConnections);

    {
        AnalysisPauser pauser(m_context);
        analysis->addObjects(cloneVector);
        check_directory_consistency(analysis->getDirectories(), analysis);
    }

    repopulate();
    selectObjects(cloneVector);
}

void EventWidgetPrivate::createView(const QUuid &eventId)
{
    auto analysis = m_context->getAnalysis();
    s32 maxUserLevel = 0;

    for (const auto &op: analysis->getOperators(eventId))
    {
        maxUserLevel = std::max(maxUserLevel, op->getUserLevel());
    }

    for (const auto &dir: analysis->getDirectories(eventId))
    {
        maxUserLevel = std::max(maxUserLevel, dir->getUserLevel());
    }

    // Level 0: special case for data sources
    m_levelTrees.push_back(createSourceTrees(eventId));

    // Level >= 1: standard trees
    for (s32 userLevel = 1; userLevel <= maxUserLevel; ++userLevel)
    {
        auto trees = createTrees(eventId, userLevel);
        m_levelTrees.push_back(trees);
    }

    auto csh = [this] (ObjectTree *tree, QTreeWidgetItem *node, const QVariant &prev)
    {
        this->onNodeCheckStateChanged(tree, node, prev);
    };

    for (auto &trees: m_levelTrees)
    {
        for (auto &tree: trees.getObjectTrees())
        {
            tree->setCheckStateChangeHandler(csh);
        }
    }
}

namespace
{

UserLevelTrees make_displaylevel_trees(const QString &opTitle, const QString &dispTitle, s32 level)
{
    const auto editTriggers = QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed;

    UserLevelTrees result = {};
    result.operatorTree = (level == 0 ? new DataSourceTree : new OperatorTree);
    result.sinkTree = new SinkTree;
    result.userLevel = level;

    result.operatorTree->setObjectName(opTitle);
    result.operatorTree->headerItem()->setText(0, opTitle);
    result.operatorTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    result.operatorTree->setEditTriggers(editTriggers);

    result.sinkTree->setObjectName(dispTitle);
    result.sinkTree->headerItem()->setText(0, dispTitle);
    result.sinkTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    result.sinkTree->setEditTriggers(editTriggers);

    auto isNodeDisabled = [](QTreeWidgetItem *node) -> bool
    {
        if (node->type() == NodeType_Module)
        {
            if (auto module = get_pointer<ModuleConfig>(node, DataRole_RawPointer))
                return !module->isEnabled();
        }

        return false;
    };

    for (auto tree: result.getObjectTrees())
    {
        tree->setExpandsOnDoubleClick(false);
        tree->setItemDelegate(new CanDisableItemsHtmlDelegate(isNodeDisabled, tree));
        tree->setDragEnabled(true);
        tree->viewport()->setAcceptDrops(true);
        tree->setDropIndicatorShown(true);
        tree->setDragDropMode(QAbstractItemView::DragDrop);
    }

    return result;
}

static const s32 minTreeWidth = 200;
static const s32 minTreeHeight = 150;

} // anon ns

UserLevelTrees EventWidgetPrivate::createSourceTrees(const QUuid &eventId)
{
    auto analysis = m_context->getAnalysis();
    auto vmeConfig = m_context->getVMEConfig();

    auto eventConfig = vmeConfig->getEventConfig(eventId);
    auto modules = eventConfig->getModuleConfigs();

    UserLevelTrees result = make_displaylevel_trees(
        QSL("L0 Parameter Extraction"),
        QSL("L0 Raw Data Display"),
        0);

    // Populate the OperatorTree (top left)
    for (const auto &mod: modules)
    {
        QObject::disconnect(mod, &ConfigObject::modified, m_q, &EventWidget::repopulate);
        QObject::connect(mod, &ConfigObject::modified, m_q, &EventWidget::repopulate);
        auto moduleNode = make_module_node(mod);
        result.operatorTree->addTopLevelItem(moduleNode);
        moduleNode->setExpanded(true);

        auto sources = analysis->getSources(eventId, mod->getId());

#if 0
//#ifndef QT_NO_DEBUG
        qDebug() << __PRETTY_FUNCTION__ << ">>>>> sources in order:";
        for (auto source: sources)
        {
            qDebug() << source.get();
        }
        qDebug() << __PRETTY_FUNCTION__ << " <<<< end sources";
#endif

        for (auto source: sources)
        {
            auto sourceNode = make_datasource_node(source.get());
            moduleNode->addChild(sourceNode);

            assert(m_objectMap.count(source) == 0);
            m_objectMap[source] = sourceNode;
        }
    }

    auto dataSourceTree = qobject_cast<DataSourceTree *>(result.operatorTree);
    assert(dataSourceTree);

    // Add unassigned data sources below a special root node
    for (const auto &source: analysis->getSourcesByEvent(eventId))
    {
        if (source->getModuleId().isNull())
        {
            if (!dataSourceTree->unassignedDataSourcesRoot)
            {
                auto node = new TreeNode({QSL("Unassigned")});
                node->setFlags(node->flags() &
                               (~Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
                node->setIcon(0, QIcon(QSL(":/exclamation-circle.png")));

                dataSourceTree->unassignedDataSourcesRoot = node;
                result.operatorTree->addTopLevelItem(node);
                node->setExpanded(true);
            }

            assert(dataSourceTree->unassignedDataSourcesRoot);

            auto sourceNode = make_datasource_node(source.get());
            dataSourceTree->unassignedDataSourcesRoot->addChild(sourceNode);

            assert(m_objectMap.count(source) == 0);
            m_objectMap[source] = sourceNode;
        }
    }

    // Populate the SinkTree (bottom left)
    // Create module nodes and nodes for the raw histograms for each data source for the module.
    QSet<QObject *> sinksAddedBelowModules;
    auto operators = analysis->getOperators(eventId, 0);

    for (const auto &mod: modules)
    {
        auto moduleNode = make_module_node(mod);
        result.sinkTree->addTopLevelItem(moduleNode);
        moduleNode->setExpanded(true);

        for (const auto &source: analysis->getSources(eventId, mod->getId()))
        {
            for (const auto &op: operators)
            {
                auto sink = qobject_cast<SinkInterface *>(op.get());

                if (sink && (sink->getSlot(0)->inputPipe == source->getOutput(0)))
                {
                    TreeNode *node = nullptr;

                    if (auto histoSink = qobject_cast<Histo1DSink *>(op.get()))
                    {
                        node = make_histo1d_node(histoSink);
                    }
                    else
                    {
                        node = make_sink_node(sink);
                    }

                    if (node)
                    {
                        moduleNode->addChild(node);
                        sinksAddedBelowModules.insert(sink);

                        assert(m_objectMap.count(op) == 0);
                        m_objectMap[op] = node;
                    }

                }
            }
        }
    }

    // This handles any "lost" display elements. E.g. raw histograms whose data
    // source has been deleted.
    for (auto &op: operators)
    {
        if (auto histoSink = qobject_cast<Histo1DSink *>(op.get()))
        {
            if (!sinksAddedBelowModules.contains(histoSink))
            {
                auto histoNode = make_histo1d_node(histoSink);
                result.sinkTree->addTopLevelItem(histoNode);

                assert(m_objectMap.count(op) == 0);
                m_objectMap[op] = histoNode;
            }
        }
        else if (auto histoSink = qobject_cast<Histo2DSink *>(op.get()))
        {
            if (!sinksAddedBelowModules.contains(histoSink))
            {
                auto histoNode = make_histo2d_node(histoSink);
                result.sinkTree->addTopLevelItem(histoNode);

                assert(m_objectMap.count(op) == 0);
                m_objectMap[op] = histoNode;
            }
        }
        else if (auto sink = qobject_cast<SinkInterface *>(op.get()))
        {
            if (!sinksAddedBelowModules.contains(sink))
            {
                auto sinkNode = make_sink_node(sink);
                result.sinkTree->addTopLevelItem(sinkNode);

                assert(m_objectMap.count(op) == 0);
                m_objectMap[op] = sinkNode;
            }
        }
    }

    result.sinkTree->sortItems(0, Qt::AscendingOrder);

    return result;
}

UserLevelTrees EventWidgetPrivate::createTrees(const QUuid &eventId, s32 level)
{
    UserLevelTrees result = make_displaylevel_trees(
        QString(QSL("L%1 Processing")).arg(level),
        QString(QSL("L%1 Data Display")).arg(level),
        level);

    auto analysis = m_context->getAnalysis();

    // create directory entries for both trees
    auto opDirs = analysis->getDirectories(eventId, level, DisplayLocation::Operator);
    auto sinkDirs = analysis->getDirectories(eventId, level, DisplayLocation::Sink);

    QHash<DirectoryPtr, TreeNode *> dirNodes;

    // Populate the OperatorTree

    add_directory_nodes(result.operatorTree, opDirs, dirNodes, analysis);

    auto operators = analysis->getOperators(eventId, level);

    for (auto op: operators)
    {
        if (qobject_cast<SinkInterface *>(op.get()))
            continue;

        std::unique_ptr<TreeNode> opNode(make_operator_node(op.get()));

        assert(m_objectMap.count(op) == 0);
        m_objectMap[op] = opNode.get();

        if (level > 0)
        {
            opNode->setFlags(opNode->flags() | Qt::ItemIsDragEnabled);
        }

        if (auto dir = analysis->getParentDirectory(op))
        {
            if (auto dirNode = dirNodes.value(dir))
            {
                dirNode->addChild(opNode.release());
            }
        }
        else
        {
            result.operatorTree->addTopLevelItem(opNode.release());
        }

    }
    result.operatorTree->sortItems(0, Qt::AscendingOrder);

    // Populate the SinkTree

    add_directory_nodes(result.sinkTree, sinkDirs, dirNodes, analysis);

    {
        for (const auto &op: operators)
        {
            std::unique_ptr<TreeNode> theNode;

            if (auto histoSink = qobject_cast<Histo1DSink *>(op.get()))
            {
                theNode.reset(make_histo1d_node(histoSink));
            }
            else if (auto histoSink = qobject_cast<Histo2DSink *>(op.get()))
            {
                theNode.reset(make_histo2d_node(histoSink));
            }
            else if (auto sink = qobject_cast<SinkInterface *>(op.get()))
            {
                theNode.reset(make_sink_node(sink));
            }

            if (theNode)
            {
                assert(m_objectMap.count(op) == 0);
                m_objectMap[op] = theNode.get();

                if (level > 0)
                    theNode->setFlags(theNode->flags() | Qt::ItemIsDragEnabled);

                if (auto dir = analysis->getParentDirectory(op))
                {
                    if (auto dirNode = dirNodes.value(dir))
                    {
                        dirNode->addChild(theNode.release());
                    }
                }
                else
                {
                    result.sinkTree->addTopLevelItem(theNode.release());
                }
            }
        }
    }

    result.sinkTree->sortItems(0, Qt::AscendingOrder);

    for (const auto &dir: dirNodes.keys())
    {
        assert(m_objectMap.count(dir) == 0);
        assert(dirNodes.value(dir));
        m_objectMap[dir] = dirNodes.value(dir);
    }

    return result;
}

void EventWidgetPrivate::appendTreesToView(UserLevelTrees trees)
{
    auto opTree   = trees.operatorTree;
    auto sinkTree = trees.sinkTree;
    s32 levelIndex = trees.userLevel;

    opTree->setMinimumWidth(minTreeWidth);
    opTree->setMinimumHeight(minTreeHeight);
    opTree->setContextMenuPolicy(Qt::CustomContextMenu);

    sinkTree->setMinimumWidth(minTreeWidth);
    sinkTree->setMinimumHeight(minTreeHeight);
    sinkTree->setContextMenuPolicy(Qt::CustomContextMenu);

    m_operatorFrameSplitter->addWidget(opTree);
    m_displayFrameSplitter->addWidget(sinkTree);

    QObject::connect(opTree, &QWidget::customContextMenuRequested,
                     m_q, [this, opTree, levelIndex] (QPoint pos) {
        doOperatorTreeContextMenu(opTree, pos, levelIndex);
    });

    QObject::connect(sinkTree, &QWidget::customContextMenuRequested,
                     m_q, [this, sinkTree, levelIndex] (QPoint pos) {
        doSinkTreeContextMenu(sinkTree, pos, levelIndex);
    });

    for (auto tree: trees.getObjectTrees())
    {
        tree->setEventWidget(m_q);
        tree->setUserLevel(levelIndex);

        // mouse interaction
        QObject::connect(tree, &QTreeWidget::itemClicked,
                         m_q, [this, levelIndex] (QTreeWidgetItem *node, int column) {
            onNodeClicked(reinterpret_cast<TreeNode *>(node), column, levelIndex);
            updateActions();
        });

        QObject::connect(tree, &QTreeWidget::itemDoubleClicked,
                         m_q, [this, levelIndex] (QTreeWidgetItem *node, int column) {
            onNodeDoubleClicked(reinterpret_cast<TreeNode *>(node), column, levelIndex);
        });

        // keyboard interaction changes the treewidgets current item
        QObject::connect(tree, &QTreeWidget::currentItemChanged,
                         m_q, [this, tree](QTreeWidgetItem *current, QTreeWidgetItem *previous) {
            qDebug() << "currentItemChanged on" << tree;
#if 0
            if (current)
            {
                clearTreeSelectionsExcept(tree);
            }
            updateActions();
#endif
        });

        // inline editing via F2
        QObject::connect(tree, &QTreeWidget::itemChanged,
                         m_q, [this, levelIndex] (QTreeWidgetItem *item, int column) {
            onNodeChanged(reinterpret_cast<TreeNode *>(item), column, levelIndex);
        });

        TreeType treeType = (tree == opTree ? TreeType_Operator : TreeType_Sink);

        QObject::connect(tree, &QTreeWidget::itemExpanded,
                         m_q, [this, treeType] (QTreeWidgetItem *node) {

            if (void *voidObj = get_pointer<void>(node, DataRole_AnalysisObject))
            {
                m_expandedObjects[treeType].insert(voidObj);
            }

            if (void *voidObj = get_pointer<void>(node, DataRole_RawPointer))
            {
                m_expandedObjects[treeType].insert(voidObj);
            }
        });

        QObject::connect(tree, &QTreeWidget::itemCollapsed,
                         m_q, [this, treeType] (QTreeWidgetItem *node) {

            if (void *voidObj = get_pointer<void>(node, DataRole_AnalysisObject))
            {
                m_expandedObjects[treeType].remove(voidObj);
            }

            if (void *voidObj = get_pointer<void>(node, DataRole_RawPointer))
            {
                m_expandedObjects[treeType].remove(voidObj);
            }
        });

        QObject::connect(tree, &QTreeWidget::itemSelectionChanged,
                         m_q, [this, tree] () {
            //qDebug() << "itemSelectionChanged on" << tree
            //    << ", new selected item count =" << tree->selectedItems().size();
            updateActions();
        });
    }
}

template<typename T>
static void expandObjectNodes(const QVector<UserLevelTrees> &treeVector, const T &objectsToExpand)
{
    const QVector<int> dataRoles = { DataRole_AnalysisObject, DataRole_RawPointer };

    for (auto trees: treeVector)
    {
        expand_tree_nodes(trees.operatorTree->invisibleRootItem(),
                          objectsToExpand[EventWidgetPrivate::TreeType_Operator],
                          0, dataRoles);

        expand_tree_nodes(trees.sinkTree->invisibleRootItem(),
                          objectsToExpand[EventWidgetPrivate::TreeType_Sink],
                          0, dataRoles);
    }
}

void EventWidgetPrivate::repopulate()
{
    qDebug() << __PRETTY_FUNCTION__ << m_q;

    auto splitterSizes = m_operatorFrameSplitter->sizes();
    // clear

    for (auto trees: m_levelTrees)
    {
        trees.operatorTree->setParent(nullptr);
        trees.operatorTree->deleteLater();

        trees.sinkTree->setParent(nullptr);
        trees.sinkTree->deleteLater();
    }
    m_levelTrees.clear();
    Q_ASSERT(m_operatorFrameSplitter->count() == 0);
    Q_ASSERT(m_displayFrameSplitter->count() == 0);

    m_objectMap.clear();

    // populate
    if (!m_eventId.isNull())
    {
        // This populates m_d->m_levelTrees
        createView(m_eventId);
    }

    for (auto trees: m_levelTrees)
    {
        // This populates the operator and display splitters
        appendTreesToView(trees);
    }

    s32 levelsToAdd = m_manualUserLevel - m_levelTrees.size();

    for (s32 i = 0; i < levelsToAdd; ++i)
    {
        s32 levelIndex = m_levelTrees.size();
        auto trees = createTrees(m_eventId, levelIndex);
        m_levelTrees.push_back(trees);
        appendTreesToView(trees);
    }

    if (splitterSizes.size() == m_operatorFrameSplitter->count())
    {
        // Restore the splitter sizes. As the splitters are synced via
        // splitterMoved() they both had the same sizes before.
        m_operatorFrameSplitter->setSizes(splitterSizes);
        m_displayFrameSplitter->setSizes(splitterSizes);
    }

    m_hiddenUserLevels.resize(m_levelTrees.size());

    for (s32 idx = 0; idx < m_hiddenUserLevels.size(); ++idx)
    {
        m_levelTrees[idx].operatorTree->setVisible(!m_hiddenUserLevels[idx]);
        m_levelTrees[idx].sinkTree->setVisible(!m_hiddenUserLevels[idx]);
    }

    expandObjectNodes(m_levelTrees, m_expandedObjects);
    clearAllToDefaultNodeHighlights();
    updateActions();

    m_analysisWidget->getConditionWidget()->repopulate(m_eventIndex);

#ifndef NDEBUG
    qDebug() << __PRETTY_FUNCTION__ << this << "_-_-_-_-_-"
        << "objectMap contains " << m_objectMap.size() << "mappings";

    for (const auto &p: m_objectMap)
    {
        if (auto obj = p.first.lock())
        {
            assert(obj->getEventId() == m_eventId);
        }
    }
#endif
}

void EventWidgetPrivate::addUserLevel()
{
    s32 levelIndex = m_levelTrees.size();
    auto trees = createTrees(m_eventId, levelIndex);
    m_levelTrees.push_back(trees);
    appendTreesToView(trees);
    m_manualUserLevel = levelIndex + 1;
}

void EventWidgetPrivate::removeUserLevel()
{
    Q_ASSERT(m_levelTrees.size() > 1);
    auto trees = m_levelTrees.last();
    m_levelTrees.pop_back();
    delete trees.operatorTree;
    delete trees.sinkTree;
    m_manualUserLevel = m_levelTrees.size();
}

s32 EventWidgetPrivate::getUserLevelForTree(QTreeWidget *tree)
{
    for (s32 userLevel = 0;
         userLevel < m_levelTrees.size();
         ++userLevel)
    {
        auto trees = m_levelTrees[userLevel];
        if (tree == trees.operatorTree || tree == trees.sinkTree)
        {
            return userLevel;
        }
    }

    return -1;
}

template<typename T, typename C>
QVector<std::shared_ptr<T>> objects_from_nodes(const C &nodes)
{
    QVector<std::shared_ptr<T>> result;

    for (const auto &node: nodes)
    {
        if (auto obj = get_shared_analysis_object<T>(node, DataRole_AnalysisObject))
            result.push_back(obj);
    }

    return result;
}

template<typename C>
AnalysisObjectVector objects_from_nodes(const C &nodes)
{
    return objects_from_nodes<AnalysisObject>(nodes);
}

/* Context menu for the operator tree views (top). */
void EventWidgetPrivate::doOperatorTreeContextMenu(QTreeWidget *tree, QPoint pos, s32 userLevel)
{
    //auto localSelectedObjects  = objects_from_nodes(tree->selectedItems());
    //auto activeObject = get_shared_analysis_object<AnalysisObject>(activeNode);
    Q_ASSERT(0 <= userLevel && userLevel < m_levelTrees.size());

    if (m_uniqueWidget) return;

    if (hasPendingConditionModifications())
    {
            qDebug() << __PRETTY_FUNCTION__ << "hasPendingConditionModifications() -> early return";
            return;
    }

    if (userLevel == 0)
    {
        doDataSourceOperatorTreeContextMenu(tree, pos, userLevel);
        return;
    }

    auto make_menu_new = [this, userLevel](QMenu *parentMenu,
                                           const DirectoryPtr &destDir = DirectoryPtr())
    {
        auto menuNew = new QMenu(parentMenu);

        auto add_newOperatorAction =
            [this, parentMenu, menuNew, userLevel] (const QString &title,
                                                    auto op,
                                                    const DirectoryPtr &destDir) {
                auto icon = make_operator_icon(op.get());
                // New Operator
                menuNew->addAction(icon, title, parentMenu, [this, userLevel, op, destDir]() {
                    auto dialog = operator_editor_factory(
                        op, userLevel, ObjectEditorMode::New, destDir, m_q);

                    //POS dialog->move(QCursor::pos());
                    dialog->setAttribute(Qt::WA_DeleteOnClose);
                    dialog->show();
                    m_uniqueWidget = dialog;
                    clearAllTreeSelections();
                    clearAllToDefaultNodeHighlights();
                });
            };

        auto objectFactory = m_context->getAnalysis()->getObjectFactory();
        OperatorVector operators;

        for (auto operatorName: objectFactory.getOperatorNames())
        {
            OperatorPtr op(objectFactory.makeOperator(operatorName));
            operators.push_back(op);
        }

        // Sort operators by displayname
        qSort(operators.begin(), operators.end(),
              [](const OperatorPtr &a, const OperatorPtr &b) {
                  return a->getDisplayName() < b->getDisplayName();
              });

        for (auto op: operators)
        {
            add_newOperatorAction(op->getDisplayName(), op, destDir);
        }

        menuNew->addSeparator();
        menuNew->addAction(
            QIcon(QSL(":/folder_orange.png")), QSL("Directory"),
            parentMenu, [this, userLevel, destDir]() {
                auto newDir = std::make_shared<Directory>();
                newDir->setObjectName("New Directory");
                newDir->setUserLevel(userLevel);
                newDir->setEventId(m_eventId);
                newDir->setDisplayLocation(DisplayLocation::Operator);
                m_context->getAnalysis()->addDirectory(newDir);
                if (destDir)
                {
                    destDir->push_back(newDir);
                }
                repopulate();

                if (auto node = findNode(newDir))
                    node->setExpanded(true);

                if (auto node = findNode(newDir))
                    node->treeWidget()->editItem(node);
            });

        return menuNew;
    };

    auto globalSelectedObjects = getAllSelectedObjects();
    auto activeNode = tree->itemAt(pos);
    QMenu menu;

    if (activeNode)
    {
        if (activeNode->type() == NodeType_OutputPipe)
        {
            auto pipe = get_pointer<Pipe>(activeNode, DataRole_RawPointer);

            menu.addAction(QIcon(":/table.png"), QSL("Show Parameters"), [this, pipe]() {
                makeAndShowPipeDisplay(pipe);
            });
        }

        if (activeNode->type() == NodeType_Operator)
        {
            if (auto op = get_shared_analysis_object<OperatorInterface>(activeNode,
                                                                        DataRole_AnalysisObject))
            {
                if (op->getNumberOfOutputs() == 1)
                {
                    Pipe *pipe = op->getOutput(0);

                    menu.addAction(QIcon(":/table.png"), QSL("Show Parameters"), [this, pipe]() {
                        makeAndShowPipeDisplay(pipe);
                    });
                }

                menu.addAction(
                    QIcon(":/pencil.png"), QSL("Edit"), [this, userLevel, op]() {
                        auto dialog = operator_editor_factory(
                            op, userLevel, ObjectEditorMode::Edit, DirectoryPtr(), m_q);

                        //POS dialog->move(QCursor::pos());
                        dialog->setAttribute(Qt::WA_DeleteOnClose);
                        dialog->show();
                        m_uniqueWidget = dialog;
                        clearAllTreeSelections();
                        clearAllToDefaultNodeHighlights();
                    });

                menu.addAction(QIcon(QSL(":/document-rename.png")), QSL("Rename"), [activeNode] () {
                    if (auto tw = activeNode->treeWidget())
                    {
                        tw->editItem(activeNode);
                    }
                });
            }
        }

        if (auto dir = get_shared_analysis_object<Directory>(activeNode,
                                                             DataRole_AnalysisObject))
        {
            auto actionNew = menu.addAction(QSL("New"));
            actionNew->setMenu(make_menu_new(&menu, dir));
            auto before = menu.actions().value(0);
            menu.insertAction(before, actionNew);

            menu.addAction(QIcon(QSL(":/document-rename.png")), QSL("Rename"), [activeNode] () {
                if (auto tw = activeNode->treeWidget())
                {
                    tw->editItem(activeNode);
                }
            });
        }
    }
    else
    {
        auto actionNew = menu.addAction(QSL("New"));
        actionNew->setMenu(make_menu_new(&menu));
        auto before = menu.actions().value(0);
        menu.insertAction(before, actionNew);
    }

    // Copy/Paste
    {
        menu.addSeparator();

        QAction *action;

        action = menu.addAction(
            QIcon::fromTheme("edit-copy"), "Copy",
            [this, globalSelectedObjects] {
                copyToClipboard(globalSelectedObjects);
            }, QKeySequence::Copy);

        action->setEnabled(!globalSelectedObjects.isEmpty());

        action = menu.addAction(
            QIcon::fromTheme("edit-paste"), "Paste",
            [this, tree] {
                pasteFromClipboard(tree);
            }, QKeySequence::Paste);

        action->setEnabled(canPaste());
    }

    if (!globalSelectedObjects.isEmpty())
    {
        menu.addSeparator();

        menu.addAction(
            QIcon::fromTheme("edit-delete"), "Remove selected",
            [this, globalSelectedObjects] {
                removeObjects(globalSelectedObjects);
            });
    }

    if (!menu.isEmpty())
    {
        menu.exec(tree->mapToGlobal(pos));
    }
}

void EventWidgetPrivate::doDataSourceOperatorTreeContextMenu(QTreeWidget *tree,
                                                             QPoint pos,
                                                             s32 userLevel)
{
    /* Context menu for the top-left tree which contains modules and their
     * datasources. */

    assert(userLevel == 0);

    if (m_uniqueWidget) return;

    auto globalSelectedObjects = getAllSelectedObjects();
    auto activeNode = tree->itemAt(pos);

    QMenu menu;

    if (activeNode)
    {
        if (activeNode->type() == NodeType_Module)
        {
            auto menuNew = new QMenu(&menu);
            auto moduleConfig = get_pointer<ModuleConfig>(activeNode, DataRole_RawPointer);

            auto add_newDataSourceAction = [this, &menu, menuNew, moduleConfig, userLevel]
                (const QString &title, auto srcPtr) {
                    auto icon = make_datasource_icon(srcPtr.get());

                    menuNew->addAction(icon, title, &menu,
                                       [this, moduleConfig, srcPtr, userLevel]() {

                                           auto dialog = datasource_editor_factory(
                                               srcPtr, userLevel, ObjectEditorMode::New, moduleConfig, m_q);

                                           assert(dialog);

                                           //POS dialog->move(QCursor::pos());
                                           dialog->setAttribute(Qt::WA_DeleteOnClose);
                                           dialog->show();
                                           m_uniqueWidget = dialog;
                                           clearAllTreeSelections();
                                           clearAllToDefaultNodeHighlights();
                                       });
                };

            // new data sources / filters
            auto objectFactory = m_context->getAnalysis()->getObjectFactory();
            QVector<SourcePtr> sources;

            for (auto sourceName: objectFactory.getSourceNames())
            {
                SourcePtr src(objectFactory.makeSource(sourceName));
                sources.push_back(src);
            }

            // Sort sources by displayname
            qSort(sources.begin(), sources.end(),
                  [](const SourcePtr &a, const SourcePtr &b) {
                      return a->getDisplayName() < b->getDisplayName();
                  });


            for (auto src: sources)
            {
                add_newDataSourceAction(src->getDisplayName(), src);
            }

            // default data filters and "raw display" creation
            auto defaultExtractors = get_default_data_extractors(
                moduleConfig->getModuleMeta().typeName);

            if (!defaultExtractors.isEmpty())
            {
                menu.addAction(QSL("Generate default filters"), [this, moduleConfig] () {

                    QMessageBox box(
                        QMessageBox::Question,
                        QSL("Generate default filters"),
                        QSL("This action will generate extraction filters,"
                            ", calibrations and histograms for the selected module."
                            " Do you want to continue?"),
                        QMessageBox::Ok | QMessageBox::No,
                        m_q);

                    box.button(QMessageBox::Ok)->setText("Yes, generate filters");

                    if (box.exec() == QMessageBox::Ok)
                    {
                        generateDefaultFilters(moduleConfig);
                    }
                });
            }

            // Module Settings
            // TODO: move Module Settings into a separate dialog that contains
            // all the multievent settings combined
            menu.addAction(
                QIcon(QSL(":/gear.png")), QSL("Module Settings"),
                &menu, [this, moduleConfig]() {

                    auto analysis = m_context->getAnalysis();
                    auto moduleSettings = analysis->getVMEObjectSettings(
                        moduleConfig->getId());

                    ModuleSettingsDialog dialog(moduleConfig, moduleSettings, m_q);

                    if (dialog.exec() == QDialog::Accepted)
                    {
                        analysis->setVMEObjectSettings(
                            moduleConfig->getId(), dialog.getSettings());
                    }
                });

            auto actionNew = menu.addAction(QSL("New"));
            actionNew->setMenu(menuNew);
            auto before = menu.actions().value(0);
            menu.insertAction(before, actionNew);
        }

        if (activeNode->type() == NodeType_Source)
        {
            if (auto srcPtr = get_shared_analysis_object<SourceInterface>(activeNode,
                                                                          DataRole_AnalysisObject))
            {
                Q_ASSERT_X(srcPtr->getNumberOfOutputs() == 1,
                           "doOperatorTreeContextMenu",
                           "data sources with multiple outputs are not supported");

                auto moduleNode = activeNode->parent();
                ModuleConfig *moduleConfig = nullptr;

                if (moduleNode && moduleNode->type() == NodeType_Module)
                    moduleConfig = get_pointer<ModuleConfig>(moduleNode, DataRole_RawPointer);

                const bool isAttachedToModule = moduleConfig != nullptr;

                auto pipe = srcPtr->getOutput(0);

                if (isAttachedToModule)
                {
                    menu.addAction(QIcon(":/table.png"), QSL("Show Parameters"), [this, pipe]() {
                        makeAndShowPipeDisplay(pipe);
                    });
                }

                if (moduleConfig)
                {
                    menu.addAction(
                        QIcon(":/pencil.png"), QSL("Edit"),
                        [this, srcPtr, moduleConfig, userLevel]() {

                            auto dialog = datasource_editor_factory(
                                srcPtr, userLevel, ObjectEditorMode::Edit, moduleConfig, m_q);

                            assert(dialog);

                            //POS dialog->move(QCursor::pos());
                            dialog->setAttribute(Qt::WA_DeleteOnClose);
                            dialog->show();
                            m_uniqueWidget = dialog;
                            clearAllTreeSelections();
                            clearAllToDefaultNodeHighlights();
                        });
                }
            }
        }
    }

    // Copy/Paste
    {
        menu.addSeparator();

        QAction *action;

        action = menu.addAction(
            QIcon::fromTheme("edit-copy"), "Copy",
            [this, globalSelectedObjects] {
                copyToClipboard(globalSelectedObjects);
            }, QKeySequence::Copy);

        action->setEnabled(!globalSelectedObjects.isEmpty());

        action = menu.addAction(
            QIcon::fromTheme("edit-paste"), "Paste",
            [this, tree] {
                pasteFromClipboard(tree);
            }, QKeySequence::Paste);

        action->setEnabled(canPaste());
    }

    if (!globalSelectedObjects.isEmpty())
    {
        menu.addSeparator();
        menu.addAction(
            QIcon::fromTheme("edit-delete"), "Remove selected",
            [this, globalSelectedObjects] {
                removeObjects(globalSelectedObjects);
            });
    }

    if (!menu.isEmpty())
    {
        menu.exec(tree->mapToGlobal(pos));
    }
}

/* Context menu for the display/sink trees (bottom). */
void EventWidgetPrivate::doSinkTreeContextMenu(QTreeWidget *tree, QPoint pos, s32 userLevel)
{
    Q_ASSERT(0 <= userLevel && userLevel < m_levelTrees.size());

    if (m_uniqueWidget) return;

    if (hasPendingConditionModifications())
    {
            qDebug() << __PRETTY_FUNCTION__ << "hasPendingConditionModifications() -> early return";
            return;
    }

    if (userLevel == 0)
    {
        doRawDataSinkTreeContextMenu(tree, pos, userLevel);
        return;
    }

    auto make_menu_new = [this, userLevel](QMenu *parentMenu,
                                           const DirectoryPtr &destDir = DirectoryPtr())
    {
        auto menuNew = new QMenu(parentMenu);

        auto add_newOperatorAction =
            [this, parentMenu, menuNew, userLevel] (const QString &title,
                                                    auto op,
                                                    const DirectoryPtr &destDir) {
                auto icon = make_operator_icon(op.get());
                // New Operator
                menuNew->addAction(icon, title, parentMenu, [this, userLevel, op, destDir]() {
                    auto dialog = operator_editor_factory(
                        op, userLevel, ObjectEditorMode::New, destDir, m_q);

                    //POS dialog->move(QCursor::pos());
                    dialog->setAttribute(Qt::WA_DeleteOnClose);
                    dialog->show();
                    m_uniqueWidget = dialog;
                    clearAllTreeSelections();
                    clearAllToDefaultNodeHighlights();
                });
            };

        auto objectFactory = m_context->getAnalysis()->getObjectFactory();
        OperatorVector operators;

        for (auto operatorName: objectFactory.getSinkNames())
        {
            OperatorPtr op(objectFactory.makeSink(operatorName));
            operators.push_back(op);
        }

        // Sort operators by displayname
        qSort(operators.begin(), operators.end(),
              [](const OperatorPtr &a, const OperatorPtr &b) {
                  return a->getDisplayName() < b->getDisplayName();
              });

        for (auto op: operators)
        {
            add_newOperatorAction(op->getDisplayName(), op, destDir);
        }

        menuNew->addSeparator();
        menuNew->addAction(
            QIcon(QSL(":/folder_orange.png")), QSL("Directory"),
            parentMenu, [this, userLevel, destDir]() {
                auto newDir = std::make_shared<Directory>();
                newDir->setObjectName("New Directory");
                newDir->setUserLevel(userLevel);
                newDir->setEventId(m_eventId);
                newDir->setDisplayLocation(DisplayLocation::Sink);
                m_context->getAnalysis()->addDirectory(newDir);
                if (destDir)
                {
                    destDir->push_back(newDir);
                }
                repopulate();

                if (auto node = findNode(newDir))
                    node->setExpanded(true);

                if (auto node = findNode(newDir))
                    node->treeWidget()->editItem(node);
            });

        return menuNew;
    };

    auto globalSelectedObjects = getAllSelectedObjects();
    auto activeNode = tree->itemAt(pos);

    QMenu menu;


    if (activeNode)
    {
        if (activeNode->type() == NodeType_Histo1D)
        {
            Histo1DWidgetInfo widgetInfo = getHisto1DWidgetInfoFromNode(activeNode);
            Q_ASSERT(widgetInfo.sink);

            if (widgetInfo.histoAddress < widgetInfo.histos.size())
            {
                menu.addAction(QSL("Open Histogram"), m_q, [this, widgetInfo]() {

                    if (!m_context->hasObjectWidget(widgetInfo.sink.get())
                        || QGuiApplication::keyboardModifiers() & Qt::ControlModifier)
                    {
                        auto widget = new Histo1DListWidget(widgetInfo.histos);
                        widget->setContext(m_context);

                        if (widgetInfo.calib)
                        {
                            widget->setCalibration(widgetInfo.calib);
                        }

                        {
                            auto context = m_context;
                            widget->setSink(widgetInfo.sink, [context]
                                            (const std::shared_ptr<Histo1DSink> &sink) {
                                                context->analysisOperatorEdited(sink);
                                            });
                        }

                        widget->selectHistogram(widgetInfo.histoAddress);

                        m_context->addObjectWidget(widget, widgetInfo.sink.get(),
                                                   widgetInfo.sink->getId().toString());
                    }
                    else if (auto widget = qobject_cast<Histo1DListWidget *>(
                            m_context->getObjectWidget(widgetInfo.sink.get())))
                    {
                        widget->selectHistogram(widgetInfo.histoAddress);
                        show_and_activate(widget);
                    }
                });

                menu.addAction(
                    QSL("Open Histogram in new window"), m_q, [this, widgetInfo]() {

                        auto widget = new Histo1DListWidget(widgetInfo.histos);
                        widget->setContext(m_context);

                        if (widgetInfo.calib)
                        {
                            widget->setCalibration(widgetInfo.calib);
                        }

                        {
                            auto context = m_context;
                            widget->setSink(widgetInfo.sink, [context]
                                            (const std::shared_ptr<Histo1DSink> &sink) {
                                                context->analysisOperatorEdited(sink);
                                            });
                        }

                        widget->selectHistogram(widgetInfo.histoAddress);

                        m_context->addObjectWidget(widget, widgetInfo.sink.get(),
                                                   widgetInfo.sink->getId().toString());
                    });
            }
        }

        if (activeNode->type() == NodeType_Histo1DSink)
        {
            Histo1DWidgetInfo widgetInfo = getHisto1DWidgetInfoFromNode(activeNode);
            Q_ASSERT(widgetInfo.sink);

            if (widgetInfo.histoAddress < widgetInfo.histos.size())
            {

                menu.addAction(QSL("Open 1D List View"), m_q, [this, widgetInfo]() {
                    // always creates a new window
                    auto widget = new Histo1DListWidget(widgetInfo.histos);
                    widget->setContext(m_context);

                    if (widgetInfo.calib)
                    {
                        widget->setCalibration(widgetInfo.calib);
                    }

                    {
                        auto context = m_context;
                        widget->setSink(widgetInfo.sink, [context]
                                        (const std::shared_ptr<Histo1DSink> &sink) {
                            context->analysisOperatorEdited(sink);
                        });
                    }

                    m_context->addObjectWidget(widget, widgetInfo.sink.get(),
                                               widgetInfo.sink->getId().toString());

                });
            }

            if (widgetInfo.histos.size())
            {
                menu.addAction(QSL("Open 2D Combined View"), m_q, [this, widgetInfo]() {
                    auto widget = new Histo2DWidget(widgetInfo.sink, m_context);
                    widget->setContext(m_context);
                    m_context->addWidget(widget,
                                         widgetInfo.sink->getId().toString() + QSL("_2dCombined"));
                });
            }
        }

        if (activeNode->type() == NodeType_Histo2DSink)
        {
            if (auto histoSink = qobject_cast<Histo2DSink *>(get_qobject(activeNode,
                                                                         DataRole_AnalysisObject)))
            {
                auto histo = histoSink->m_histo;
                if (histo)
                {
                    auto sinkPtr = std::dynamic_pointer_cast<Histo2DSink>(
                        histoSink->shared_from_this());

                    menu.addAction(QSL("Open Histogram"), m_q, [this, histo, sinkPtr, userLevel]() {

                        if (!m_context->hasObjectWidget(sinkPtr.get())
                            || QGuiApplication::keyboardModifiers() & Qt::ControlModifier)
                        {
                            auto histoPtr = sinkPtr->m_histo;
                            auto widget = new Histo2DWidget(histoPtr);

                            auto context = m_context;
                            auto eventId = m_eventId;

                            widget->setSink(
                                sinkPtr,
                                // addSinkCallback
                                [context, eventId, userLevel] (const std::shared_ptr<Histo2DSink> &sink) {
                                    context->addAnalysisOperator(eventId, sink, userLevel);
                                },
                                // sinkModifiedCallback
                                [context] (const std::shared_ptr<Histo2DSink> &sink) {
                                    context->analysisOperatorEdited(sink);
                                },
                                // makeUniqueOperatorNameFunction
                                [context] (const QString &name) {
                                    return make_unique_operator_name(context->getAnalysis(), name);
                                });

                            widget->setContext(m_context);

                            m_context->addObjectWidget(widget, sinkPtr.get(), sinkPtr->getId().toString());
                        }
                        else
                        {
                            m_context->activateObjectWidget(sinkPtr.get());
                        }
                    });

                    menu.addAction(
                        QSL("Open Histogram in new window"), m_q,
                        [this, histo, sinkPtr, userLevel]() {

                            auto histoPtr = sinkPtr->m_histo;
                            auto widget = new Histo2DWidget(histoPtr);

                            auto context = m_context;
                            auto eventId = m_eventId;

                            widget->setSink(
                                sinkPtr,
                                // addSinkCallback
                                [context, eventId, userLevel] (const std::shared_ptr<Histo2DSink> &sink) {
                                    context->addAnalysisOperator(eventId, sink, userLevel);
                                },
                                // sinkModifiedCallback
                                [context] (const std::shared_ptr<Histo2DSink> &sink) {
                                    context->analysisOperatorEdited(sink);
                                },
                                // makeUniqueOperatorNameFunction
                                [context] (const QString &name) {
                                    return make_unique_operator_name(context->getAnalysis(), name);
                                });

                            widget->setContext(m_context);

                            m_context->addObjectWidget(widget, sinkPtr.get(),
                                                       sinkPtr->getId().toString());
                        });
                }
            }
        }

        if (auto sinkPtr = get_shared_analysis_object<ExportSink>(activeNode,
                                                                  DataRole_AnalysisObject))
        {
            menu.addAction("Open Status Monitor", m_q, [this, sinkPtr]() {
                if (!m_context->hasObjectWidget(sinkPtr.get())
                    || QGuiApplication::keyboardModifiers() & Qt::ControlModifier)
                {
                    auto widget = new ExportSinkStatusMonitor(sinkPtr, m_context);
                    m_context->addObjectWidget(widget, sinkPtr.get(), sinkPtr->getId().toString());
                }
                else
                {
                    m_context->activateObjectWidget(sinkPtr.get());
                }
            });
        }

        if (auto dir = get_shared_analysis_object<Directory>(activeNode,
                                                             DataRole_AnalysisObject))
        {
            auto actionNew = menu.addAction(QSL("New"));
            actionNew->setMenu(make_menu_new(&menu, dir));
            auto before = menu.actions().value(0);
            menu.insertAction(before, actionNew);

            menu.addAction(QIcon(QSL(":/document-rename.png")), QSL("Rename"), [activeNode] () {
                if (auto tw = activeNode->treeWidget())
                {
                    tw->editItem(activeNode);
                }
            });
        }

        switch (activeNode->type())
        {
            case NodeType_Operator:
            case NodeType_Histo1DSink:
            case NodeType_Histo2DSink:
            case NodeType_Sink:
                if (auto op = get_shared_analysis_object<OperatorInterface>(activeNode,
                                                                            DataRole_AnalysisObject))
                {
                    menu.addSeparator();
                    // Edit Display Operator
                    menu.addAction(QIcon(":/pencil.png"), QSL("&Edit"), [this, userLevel, op]() {
                        auto dialog = operator_editor_factory(
                            op, userLevel, ObjectEditorMode::Edit, DirectoryPtr(), m_q);

                        //POS dialog->move(QCursor::pos());
                        dialog->setAttribute(Qt::WA_DeleteOnClose);
                        dialog->show();
                        m_uniqueWidget = dialog;
                        clearAllTreeSelections();
                        clearAllToDefaultNodeHighlights();
                    });
                }

                menu.addAction(QIcon(QSL(":/document-rename.png")), QSL("Rename"), [activeNode] () {
                    if (auto tw = activeNode->treeWidget())
                    {
                        tw->editItem(activeNode);
                    }
                });

                break;
        }
    }
    else
    {
        auto actionNew = menu.addAction(QSL("New"));
        actionNew->setMenu(make_menu_new(&menu));
        auto before = menu.actions().value(0);
        menu.insertAction(before, actionNew);
    }

    // Copy/Paste
    {
        menu.addSeparator();

        QAction *action;

        action = menu.addAction(
            QIcon::fromTheme("edit-copy"), "Copy",
            [this, globalSelectedObjects] {
                copyToClipboard(globalSelectedObjects);
            }, QKeySequence::Copy);

        action->setEnabled(!globalSelectedObjects.isEmpty());

        action = menu.addAction(
            QIcon::fromTheme("edit-paste"), "Paste",
            [this, tree] {
                pasteFromClipboard(tree);
            }, QKeySequence::Paste);

        action->setEnabled(canPaste());
    }

    // sink enable/disable
    {
        auto selectedSinks = objects_from_nodes<SinkInterface>(getAllSelectedNodes());

        if (!selectedSinks.isEmpty())
        {
            menu.addSeparator();

            menu.addAction("E&nable selected", [this, selectedSinks] {
                setSinksEnabled(selectedSinks, true);
            });

            menu.addAction("&Disable selected", [this, selectedSinks] {
                setSinksEnabled(selectedSinks, false);
            });
        }
    }

    if (!globalSelectedObjects.isEmpty())
    {
        menu.addSeparator();
        menu.addAction(
            QIcon::fromTheme("edit-delete"), "Remove selected",
            [this, globalSelectedObjects] {
                removeObjects(globalSelectedObjects);
            });
    }

    if (!menu.isEmpty())
    {
        menu.exec(tree->mapToGlobal(pos));
    }
}

void EventWidgetPrivate::doRawDataSinkTreeContextMenu(QTreeWidget *tree, QPoint pos, s32 userLevel)
{
    assert(userLevel == 0);

    if (m_uniqueWidget) return;

    auto globalSelectedObjects = getAllSelectedObjects();
    auto activeNode = tree->itemAt(pos);

    QMenu menu;

    if (!activeNode || activeNode->type() == NodeType_Module)
    {
        auto menuNew = new QMenu(&menu);

        auto add_newSinkAction =
            [this, &menu, menuNew, userLevel] (const QString &title, auto op) {
                auto icon = make_operator_icon(op.get());
                // New Display Operator
                menuNew->addAction(icon, title, &menu, [this, userLevel, op]() {
                    auto dialog = operator_editor_factory(
                        op, userLevel, ObjectEditorMode::New, DirectoryPtr(), m_q);

                    //POS dialog->move(QCursor::pos());
                    dialog->setAttribute(Qt::WA_DeleteOnClose);
                    dialog->show();
                    m_uniqueWidget = dialog;
                    clearAllTreeSelections();
                    clearAllToDefaultNodeHighlights();
                });
            };

        QVector<OperatorPtr> sinks = {
            std::make_shared<Histo1DSink>(),
            std::make_shared<RateMonitorSink>()
        };

        for (auto &sink: sinks)
        {
            add_newSinkAction(sink->getDisplayName(), sink);
        }

        auto actionNew = menu.addAction(QSL("New"));
        actionNew->setMenu(menuNew);
        auto before = menu.actions().value(0);
        menu.insertAction(before, actionNew);
    }

    if (activeNode && activeNode->type() == NodeType_Histo1D)
    {
        Histo1DWidgetInfo widgetInfo = getHisto1DWidgetInfoFromNode(activeNode);
        Q_ASSERT(widgetInfo.sink);

        if (widgetInfo.histoAddress < widgetInfo.histos.size())
        {
            menu.addAction(QSL("Open Histogram"), m_q, [this, widgetInfo]() {

                if (!m_context->hasObjectWidget(widgetInfo.sink.get())
                    || QGuiApplication::keyboardModifiers() & Qt::ControlModifier)
                {
                    auto widget = new Histo1DListWidget(widgetInfo.histos);
                    widget->setContext(m_context);

                    if (widgetInfo.calib)
                    {
                        widget->setCalibration(widgetInfo.calib);
                    }

                    {
                        auto context = m_context;
                        widget->setSink(widgetInfo.sink, [context]
                                        (const std::shared_ptr<Histo1DSink> &sink) {
                                            context->analysisOperatorEdited(sink);
                                        });
                    }

                    widget->selectHistogram(widgetInfo.histoAddress);

                    m_context->addObjectWidget(widget, widgetInfo.sink.get(),
                                               widgetInfo.sink->getId().toString());
                }
                else if (auto widget = qobject_cast<Histo1DListWidget *>(
                        m_context->getObjectWidget(widgetInfo.sink.get())))
                {
                    widget->selectHistogram(widgetInfo.histoAddress);
                    show_and_activate(widget);
                }
            });

            menu.addAction(
                QSL("Open Histogram in new window"), m_q, [this, widgetInfo]() {

                    auto widget = new Histo1DListWidget(widgetInfo.histos);
                    widget->setContext(m_context);

                    if (widgetInfo.calib)
                    {
                        widget->setCalibration(widgetInfo.calib);
                    }

                    {
                        auto context = m_context;
                        widget->setSink(widgetInfo.sink,
                                        [context] (const std::shared_ptr<Histo1DSink> &sink) {
                            context->analysisOperatorEdited(sink);
                        });
                    }

                    widget->selectHistogram(widgetInfo.histoAddress);

                    m_context->addObjectWidget(widget, widgetInfo.sink.get(),
                                               widgetInfo.sink->getId().toString());
                });
        }
    }
    else if (activeNode && activeNode->type() == NodeType_Histo1DSink)
    {
        Histo1DWidgetInfo widgetInfo = getHisto1DWidgetInfoFromNode(activeNode);
        Q_ASSERT(widgetInfo.sink);

        if (widgetInfo.histoAddress < widgetInfo.histos.size())
        {

            menu.addAction(QSL("Open 1D List View"), m_q, [this, widgetInfo]() {
                // always creates a new window
                auto widget = new Histo1DListWidget(widgetInfo.histos);
                widget->setContext(m_context);

                if (widgetInfo.calib)
                {
                    widget->setCalibration(widgetInfo.calib);
                }

                {
                    auto context = m_context;
                    widget->setSink(widgetInfo.sink,
                                    [context] (const std::shared_ptr<Histo1DSink> &sink) {
                        context->analysisOperatorEdited(sink);
                    });
                }

                m_context->addObjectWidget(widget, widgetInfo.sink.get(),
                                           widgetInfo.sink->getId().toString());
            });
        }

        if (widgetInfo.histos.size())
        {
            menu.addAction(QSL("Open 2D Combined View"), m_q, [this, widgetInfo]() {
                auto widget = new Histo2DWidget(widgetInfo.sink, m_context);
                widget->setContext(m_context);
                m_context->addWidget(widget, widgetInfo.sink->getId().toString() + QSL("_2dCombined"));
            });
        }
    }
    else if (activeNode && activeNode->type() == NodeType_Sink)
    {
        if (auto rms = get_shared_analysis_object<RateMonitorSink>(activeNode,
                                                                   DataRole_AnalysisObject))
        {
            menu.addAction(QSL("Open Rate Monitor"), m_q, [this, rms]() {

                if (!m_context->hasObjectWidget(rms.get())
                    || QGuiApplication::keyboardModifiers() & Qt::ControlModifier)
                {
                    auto context = m_context;
                    auto widget = new RateMonitorWidget(rms->getRateSamplers());

                    widget->setSink(rms, [context](const std::shared_ptr<RateMonitorSink> &sink) {
                        context->analysisOperatorEdited(sink);
                    });

                    widget->setPlotExportDirectory(
                        m_context->getWorkspacePath(QSL("PlotsDirectory")));

                    m_context->addObjectWidget(widget, rms.get(), rms->getId().toString());
                }
                else
                {
                    m_context->activateObjectWidget(rms.get());
                }
            });
        }
    }


    if (activeNode)
    {
        switch (activeNode->type())
        {
            case NodeType_Operator:
            case NodeType_Histo1DSink:
            case NodeType_Histo2DSink:
            case NodeType_Sink:
                if (auto op = get_shared_analysis_object<OperatorInterface>(activeNode,
                                                                            DataRole_AnalysisObject))
                {
                    menu.addSeparator();
                    // Edit Display Operator
                    menu.addAction(QIcon(":/pencil.png"), QSL("&Edit"), [this, userLevel, op]() {
                        auto dialog = operator_editor_factory(
                            op, userLevel, ObjectEditorMode::Edit, DirectoryPtr(), m_q);

                        //POS dialog->move(QCursor::pos());
                        dialog->setAttribute(Qt::WA_DeleteOnClose);
                        dialog->show();
                        m_uniqueWidget = dialog;
                        clearAllTreeSelections();
                        clearAllToDefaultNodeHighlights();
                    });
                }

                menu.addAction(QIcon(QSL(":/document-rename.png")), QSL("Rename"), [activeNode] () {
                    if (auto tw = activeNode->treeWidget())
                    {
                        tw->editItem(activeNode);
                    }
                });

                break;
        }
    }

    // Copy/Paste
    {
        menu.addSeparator();

        QAction *action;

        action = menu.addAction(
            QIcon::fromTheme("edit-copy"), "Copy",
            [this, globalSelectedObjects] {
                copyToClipboard(globalSelectedObjects);
            }, QKeySequence::Copy);

        action->setEnabled(!globalSelectedObjects.isEmpty());

        action = menu.addAction(
            QIcon::fromTheme("edit-paste"), "Paste",
            [this, tree] {
                pasteFromClipboard(tree);
            }, QKeySequence::Paste);

        action->setEnabled(canPaste());
    }

    // sink enable/disable
    {
        auto selectedSinks = objects_from_nodes<SinkInterface>(getAllSelectedNodes());

        if (!selectedSinks.isEmpty())
        {
            menu.addSeparator();

            menu.addAction("E&nable selected", [this, selectedSinks] {
                setSinksEnabled(selectedSinks, true);
            });

            menu.addAction("&Disable selected", [this, selectedSinks] {
                setSinksEnabled(selectedSinks, false);
            });
        }
    }

    if (!globalSelectedObjects.isEmpty())
    {
        // TODO: add a copy action
        menu.addSeparator();
        menu.addAction(
            QIcon::fromTheme("edit-delete"), "Remove selected",
            [this, globalSelectedObjects] {
                removeObjects(globalSelectedObjects);
            });
    }

    if (!menu.isEmpty())
    {
        menu.exec(tree->mapToGlobal(pos));
    }
}

void EventWidgetPrivate::setMode(Mode mode)
{
    auto oldMode = m_mode;
    m_mode = mode;
    modeChanged(oldMode, mode);
}

EventWidgetPrivate::Mode EventWidgetPrivate::getMode() const
{
    return m_mode;
}

void EventWidgetPrivate::modeChanged(Mode oldMode, Mode mode)
{
    qDebug() << __PRETTY_FUNCTION__
        << "oldMode=" << mode_to_string(oldMode)
        << "newMode=" << mode_to_string(mode);

    switch (mode)
    {
        case Default:
            {
                Q_ASSERT(m_inputSelectInfo.userLevel < m_levelTrees.size());
                clearAllToDefaultNodeHighlights();
            } break;

        case SelectInput:
            // highlight valid sources
            {
                Q_ASSERT(m_inputSelectInfo.userLevel < m_levelTrees.size());

                clearAllTreeSelections();

                const bool isSink = qobject_cast<SinkInterface *>(
                    m_inputSelectInfo.slot->parentOperator);

                for (auto &trees: m_levelTrees)
                {
                    if (isSink ||
                        (getUserLevelForTree(trees.operatorTree) <= m_inputSelectInfo.userLevel))
                    {
                        highlightValidInputNodes(trees.operatorTree->invisibleRootItem());
                    }
                }
            } break;
    }

    updateActions();
}

Analysis *EventWidgetPrivate::getAnalysis() const
{
    return m_context->getAnalysis();
}

static bool forward_path_exists(PipeSourceInterface *from, PipeSourceInterface *to)
{
    if (!from) return false;
    if (!to) return false;


    for (s32 oi = 0; oi < from->getNumberOfOutputs(); oi++)
    {
        auto outPipe = from->getOutput(oi);

        for (auto destSlot: outPipe->getDestinations())
        {
            if (destSlot->parentOperator == to)
                return true;
            if (destSlot->parentOperator && forward_path_exists(destSlot->parentOperator, to))
                return true;
        }
    }

    return false;
}

static bool is_valid_input_node(QTreeWidgetItem *node, Slot *slot,
                                QSet<PipeSourceInterface *> additionalInvalidSources)
{
    PipeSourceInterface *dstObject = slot->parentOperator;
    Q_ASSERT(dstObject);

    PipeSourceInterface *srcObject = nullptr;

    switch (node->type())
    {
        case NodeType_Operator:
            {
                srcObject = get_pointer<PipeSourceInterface>(node, DataRole_AnalysisObject);
                Q_ASSERT(srcObject);
            } break;
        case NodeType_OutputPipe:
        case NodeType_OutputPipeParameter:
            {
                auto pipe = get_pointer<Pipe>(node, DataRole_RawPointer);
                srcObject = pipe->source;
                Q_ASSERT(srcObject);
            } break;
    }

    bool result = false;

    if (srcObject == dstObject)
    {
        // do not allow direct self-connections! :)
        result = false;
    }
    else if (additionalInvalidSources.contains(srcObject))
    {
        // manually given pipe sources to ignore
        result = false;
    }
    else if (forward_path_exists(dstObject, srcObject))
    {
        result = false;
    }
    else if ((slot->acceptedInputTypes & InputType::Array)
        && (node->type() == NodeType_Operator || node->type() == NodeType_Source))
    {
        // Highlight operator and source nodes only if they have exactly a
        // single output.
        PipeSourceInterface *pipeSource = get_pointer<PipeSourceInterface>(node, DataRole_AnalysisObject);
        if (pipeSource->getNumberOfOutputs() == 1)
        {
            result = true;
        }
    }
    else if ((slot->acceptedInputTypes & InputType::Array)
             && node->type() == NodeType_OutputPipe)
    {
        result = true;
    }
    else if ((slot->acceptedInputTypes & InputType::Value)
             && node->type() == NodeType_OutputPipeParameter)
    {
        result = true;
    }

    return result;
}

void EventWidgetPrivate::highlightValidInputNodes(QTreeWidgetItem *node)
{
    if (is_valid_input_node(node, m_inputSelectInfo.slot, m_inputSelectInfo.additionalInvalidSources))
    {
        node->setBackground(0, ValidInputNodeColor);
    }

    for (s32 childIndex = 0; childIndex < node->childCount(); ++childIndex)
    {
        // recurse
        auto child = node->child(childIndex);
        highlightValidInputNodes(child);
    }
}

static bool isSourceNodeOf(QTreeWidgetItem *node, Slot *slot)
{
    PipeSourceInterface *srcObject = nullptr;

    switch (node->type())
    {
        case NodeType_Source:
        case NodeType_Operator:
            {
                srcObject = get_pointer<PipeSourceInterface>(node, DataRole_AnalysisObject);
                Q_ASSERT(srcObject);
            } break;

        case NodeType_OutputPipe:
        case NodeType_OutputPipeParameter:
            {
                auto pipe = get_pointer<Pipe>(node, DataRole_RawPointer);
                srcObject = pipe->source;
                Q_ASSERT(srcObject);
            } break;
    }

    bool result = false;

    if (slot->inputPipe->source == srcObject)
    {
        if (slot->paramIndex == Slot::NoParamIndex && node->type() != NodeType_OutputPipeParameter)
        {
            result = true;
        }
        else if (slot->paramIndex != Slot::NoParamIndex && node->type() == NodeType_OutputPipeParameter)
        {
            s32 nodeParamAddress = node->data(0, DataRole_ParameterIndex).toInt();
            result = (nodeParamAddress == slot->paramIndex);
        }
    }

    return result;
}

static bool isOutputNodeOf(QTreeWidgetItem *node, PipeSourceInterface *ps)
{
    assert(ps);
    OperatorInterface *dstObject = nullptr;

    switch (node->type())
    {
        case NodeType_Operator:
        case NodeType_Histo1DSink:
        case NodeType_Histo2DSink:
        case NodeType_Sink:
            {
                dstObject = get_pointer<OperatorInterface>(node, DataRole_AnalysisObject);
                Q_ASSERT(dstObject);
            } break;
    }

    bool result = false;

    if (dstObject)
    {
        for (s32 slotIndex = 0; slotIndex < dstObject->getNumberOfSlots(); ++slotIndex)
        {
            Slot *slot = dstObject->getSlot(slotIndex);

            if (slot->inputPipe)
            {
                for (s32 outputIndex = 0; outputIndex < ps->getNumberOfOutputs(); ++outputIndex)
                {
                    Pipe *pipe = ps->getOutput(outputIndex);
                    if (slot->inputPipe == pipe)
                    {
                        result = true;
                        break;
                    }
                }
            }
        }
    }

    return result;
}

// Returns true if this node or any of its children represent an input of the
// given operator.
static bool highlight_input_nodes(OperatorInterface *op, QTreeWidgetItem *node)
{
    assert(op);
    assert(node);

    bool result = false;

    for (s32 childIndex = 0; childIndex < node->childCount(); ++childIndex)
    {
        // recurse
        auto child = node->child(childIndex);
        result = highlight_input_nodes(op, child) || result;
    }

    if (result)
    {
        node->setBackground(0, ChildIsInputNodeOfColor);
    }

    for (s32 slotIndex = 0; slotIndex < op->getNumberOfSlots(); ++slotIndex)
    {
        Slot *slot = op->getSlot(slotIndex);
        if (slot->inputPipe && isSourceNodeOf(node, slot))
        {
            node->setBackground(0, InputNodeOfColor);
            result = true;
        }
    }

    return result;
}

// Returns true if this node or any of its children are connected to an output of the
// given pipe source.
static bool highlight_output_nodes(PipeSourceInterface *ps, QTreeWidgetItem *node)
{
    bool result = false;

    for (s32 childIndex = 0; childIndex < node->childCount(); ++childIndex)
    {
        // recurse
        auto child = node->child(childIndex);
        result = highlight_output_nodes(ps, child) || result;
    }

    if (result)
    {
        node->setBackground(0, ChildIsOutputNodeOfColor);
    }

    if (isOutputNodeOf(node, ps))
    {
        node->setBackground(0, OutputNodeOfColor);
        result = true;
    }

    return result;
}

void EventWidgetPrivate::highlightInputNodes(OperatorInterface *op)
{
    assert(op);

    for (auto trees: m_levelTrees)
    {
        highlight_input_nodes(op, trees.operatorTree->invisibleRootItem());
    }
}

void EventWidgetPrivate::highlightOutputNodes(PipeSourceInterface *ps)
{
    for (auto trees: m_levelTrees)
    {
        highlight_output_nodes(ps, trees.operatorTree->invisibleRootItem());
        highlight_output_nodes(ps, trees.sinkTree->invisibleRootItem());
    }
}

void EventWidgetPrivate::clearToDefaultNodeHighlights(QTreeWidgetItem *node)
{
    node->setBackground(0, QBrush());
    //node->setFlags(node->flags() & ~Qt::ItemIsUserCheckable);
    //node->setData(0, Qt::CheckStateRole, QVariant());

    for (s32 childIndex = 0; childIndex < node->childCount(); ++childIndex)
    {
        // recurse
        auto child = node->child(childIndex);
        clearToDefaultNodeHighlights(child);
    }

    switch (node->type())
    {
        case NodeType_Operator:
        case NodeType_Histo1DSink:
        case NodeType_Histo2DSink:
        case NodeType_Sink:
            {
                if (auto op = get_pointer<OperatorInterface>(node, DataRole_AnalysisObject))
                {
                    for (auto slotIndex = 0; slotIndex < op->getNumberOfSlots(); ++slotIndex)
                    {
                        Slot *slot = op->getSlot(slotIndex);

                        Q_ASSERT(slot);

                        if (!slot->isParamIndexInRange())
                        {
                            node->setBackground(0, MissingInputColor);
                            break;
                        }
                    }
                }
            } break;
    }

    switch (node->type())
    {
        case NodeType_Histo1DSink:
        case NodeType_Histo2DSink:
        case NodeType_Sink:
            {
                auto sink = get_pointer<SinkInterface>(node, DataRole_AnalysisObject);

                if (sink && !sink->isEnabled())
                {
                    auto font = node->font(0);
                    font.setStrikeOut(true);
                    node->setFont(0, font);
                }
            } break;
    }
}

void EventWidgetPrivate::clearAllToDefaultNodeHighlights()
{
    for (auto trees: m_levelTrees)
    {
        clearToDefaultNodeHighlights(trees.operatorTree->invisibleRootItem());
        clearToDefaultNodeHighlights(trees.sinkTree->invisibleRootItem());
    }
}

/* Adds checkboxes to the candidates of the given ConditionkLink. */
void EventWidgetPrivate::addConditionDecorations(const ConditionLink &cl)
{
    if (!cl || cl.condition->getEventId() != m_eventId) return;

    auto analysis = getAnalysis();
    auto candidates = get_apply_condition_candidates(cl.condition, analysis);

    for (const auto &op: candidates)
    {
        auto it = m_objectMap.find(op);

        if (it != m_objectMap.end())
        {
            if (!it->second)
            {
                qDebug() << __PRETTY_FUNCTION__ << op << "op eventId =" << op->getEventId()
                    << "op userlevel =" << op->getUserLevel();
                qDebug() << __PRETTY_FUNCTION__ << "this eventId =" << m_eventId;
            }
            assert(it->second);

            auto node = it->second;
            auto opCond  = analysis->getConditionLink(op);
            auto checked = ((opCond.condition == cl.condition
                            && opCond.subIndex == cl.subIndex)
                            ? Qt::Checked
                            : Qt::Unchecked);

            node->setFlags(node->flags() | Qt::ItemIsUserCheckable);
            node->setCheckState(0, checked);
        }
    }
}

/* Removes checkboxes for the candidates of the given ConditionkLink. */
void EventWidgetPrivate::removeConditionDecorations(const ConditionLink &cl)
{
    if (!cl || cl.condition->getEventId() != m_eventId) return;

    auto analysis = getAnalysis();
    auto candidates = get_apply_condition_candidates(cl.condition, analysis);

    for (const auto &op: candidates)
    {
        auto it = m_objectMap.find(op);

        if (it != m_objectMap.end())
        {
            assert(it->second);

            auto node = it->second;
            node->setFlags(node->flags() & ~Qt::ItemIsUserCheckable);
            node->setData(0, Qt::CheckStateRole, QVariant());
        }
    }
}

bool EventWidgetPrivate::hasPendingConditionModifications() const
{
    if (m_applyConditionInfo)
    {
        auto &cl  = m_applyConditionInfo;
        auto analysis = getAnalysis();
        auto clMods = get_condition_modifications(cl, analysis, m_objectMap);

        return clMods.hasModifications();
    }

    return false;
}

void EventWidgetPrivate::updateNodesForApplyConditionMode()
{
    auto &aci = m_applyConditionInfo;

    if (!aci) return;
    if (aci.condition->getEventId() != m_eventId) return;

    qDebug() << __PRETTY_FUNCTION__ << this
        << endl
        << "  condition is" << aci.condition.get()
        << endl
        << "  , with maxInputRank  =" << aci.condition->getMaximumInputRank()
        << " , with maxOutputRank =" << aci.condition->getMaximumOutputRank()
        << " , with rank =" << aci.condition->getRank()
        << endl
        << "  , objectFlags =" << to_string(aci.condition->getObjectFlags())
        << endl
        << "  candidates:"
        ;

    addConditionDecorations(aci);
}

void EventWidgetPrivate::onNodeClicked(TreeNode *node, int column, s32 userLevel)
{
    AnalysisObjectPtr obj;

    switch (node->type())
    {
        case NodeType_Source:
        case NodeType_Operator:
        case NodeType_Histo1DSink:
        case NodeType_Histo2DSink:
        case NodeType_Sink:
        case NodeType_Directory:
            if ((obj = get_analysis_object(node, DataRole_AnalysisObject)))
            {
                qDebug() << __PRETTY_FUNCTION__ << "click on object: id =" << obj->getId()
                    << ", class =" << obj->metaObject()->className()
                    << ", flags =" << to_string(obj->getObjectFlags())
                    << ", ulvl  =" << obj->getUserLevel()
                    ;
                emit m_q->objectSelected(obj);
            }
            else
            {
                emit m_q->nonObjectNodeSelected(node);
            }
            break;
    }

    m_analysisWidget->showObjectInfo(obj);

    switch (m_mode)
    {
        case Default:
            {
                auto kmods = QGuiApplication::keyboardModifiers();

                if (!(kmods & (Qt::ControlModifier | Qt::ShiftModifier)))
                {
                    clearTreeSelectionsExcept(node->treeWidget());
                }

                clearAllToDefaultNodeHighlights();

                switch (node->type())
                {
                    case NodeType_Operator:
                    case NodeType_Histo1DSink:
                    case NodeType_Histo2DSink:
                    case NodeType_Sink:
                        {
                            auto op = get_pointer<OperatorInterface>(node, DataRole_AnalysisObject);
                            highlightInputNodes(op);

                            qDebug() << "Object Info: id =" << op->getId()
                                << ", class =" << op->metaObject()->className()
                                << ", #slots =" << op->getNumberOfSlots();

                            for (s32 si = 0; si < op->getNumberOfSlots(); si++)
                            {
                                auto slot = op->getSlot(si);
                                QString inputObjectId = QSL("<none>");

                                if (slot->isConnected())
                                {
                                    inputObjectId = slot->inputPipe->getSource()->getId().toString();
                                }

                                qDebug() << " Slot" << si << ": isParamIndexInRange() =" << slot->isParamIndexInRange()
                                    << ", isConnected() =" << slot->isConnected()
                                    << ", sourceId =" << inputObjectId;
                            }


                        } break;
                }

                switch (node->type())
                {
                    case NodeType_Source:
                    case NodeType_Operator:
                        {
                            auto ps = get_pointer<PipeSourceInterface>(node, DataRole_AnalysisObject);
                            highlightOutputNodes(ps);
                        } break;
                }
            } break;

        case SelectInput:
            {
                clearTreeSelectionsExcept(node->treeWidget());

                const bool isSink = qobject_cast<SinkInterface *>(
                    m_inputSelectInfo.slot->parentOperator);

                if (is_valid_input_node(node, m_inputSelectInfo.slot,
                                     m_inputSelectInfo.additionalInvalidSources)
                    && (isSink || (getUserLevelForTree(node->treeWidget())
                                   <= m_inputSelectInfo.userLevel)))
                {
                    Slot *slot = m_inputSelectInfo.slot;
                    Q_ASSERT(slot);

                    Pipe *selectedPipe = nullptr;
                    s32 selectedParamIndex = Slot::NoParamIndex;

                    switch (node->type())
                    {
                        /* Click on a Source or Operator node: use output[0]
                         * and connect the whole array. */
                        case NodeType_Source:
                        case NodeType_Operator:
                            {
                                Q_ASSERT(slot->acceptedInputTypes & InputType::Array);

                                PipeSourceInterface *source = get_pointer<PipeSourceInterface>(
                                    node, DataRole_AnalysisObject);

                                selectedPipe       = source->getOutput(0);
                                selectedParamIndex = Slot::NoParamIndex;

                                //slot->connectPipe(source->getOutput(0), Slot::NoParamIndex);
                            } break;

                        /* Click on a specific output of an object. */
                        case NodeType_OutputPipe:
                            {
                                Q_ASSERT(slot->acceptedInputTypes & InputType::Array);
                                Q_ASSERT(slot->parentOperator);

                                selectedPipe       = get_pointer<Pipe>(node, DataRole_RawPointer);
                                selectedParamIndex = Slot::NoParamIndex;

                                //slot->connectPipe(pipe, Slot::NoParamIndex);
                            } break;

                        /* Click on a specific parameter index. */
                        case NodeType_OutputPipeParameter:
                            {
                                Q_ASSERT(slot->acceptedInputTypes & InputType::Value);

                                selectedPipe       = get_pointer<Pipe>(node, DataRole_RawPointer);
                                selectedParamIndex = node->data(0, DataRole_ParameterIndex).toInt();
                            } break;

                        InvalidDefaultCase;
                    }

                    Q_ASSERT(selectedPipe);
                    Q_ASSERT(m_inputSelectInfo.callback);

                    // tell the widget that initiated the select that we're done
                    if (m_inputSelectInfo.callback)
                    {
                        qDebug() << __PRETTY_FUNCTION__ << "invoking selectInputCallback:"
                            << slot << selectedPipe << selectedParamIndex;
                        m_inputSelectInfo.callback(slot, selectedPipe, selectedParamIndex);
                    }

                    // leave SelectInput mode
                    m_inputSelectInfo.callback = nullptr;
                    setMode(Default);
                }
            } break;
    }
}

void EventWidgetPrivate::onNodeDoubleClicked(TreeNode *node, int column, s32 userLevel)
{
    if (hasPendingConditionModifications())
    {
            qDebug() << __PRETTY_FUNCTION__ << "hasPendingConditionModifications() -> early return";
            return;
    }

    if (m_mode == Default)
    {
        switch (node->type())
        {
            case NodeType_Histo1D:
                {
                    Histo1DWidgetInfo widgetInfo = getHisto1DWidgetInfoFromNode(node);
                    Q_ASSERT(widgetInfo.sink);

                    if (widgetInfo.histoAddress >= widgetInfo.histos.size())
                        break;

                    if (!widgetInfo.histos[widgetInfo.histoAddress])
                        break;

                    if (!m_context->hasObjectWidget(widgetInfo.sink.get())
                        || QGuiApplication::keyboardModifiers() & Qt::ControlModifier)
                    {
                        auto widget = new Histo1DListWidget(widgetInfo.histos);
                        widget->setContext(m_context);

                        if (widgetInfo.calib)
                        {
                            widget->setCalibration(widgetInfo.calib);
                        }

                        {
                            auto context = m_context;
                            widget->setSink(widgetInfo.sink, [context] (
                                    const std::shared_ptr<Histo1DSink> &sink) {
                                context->analysisOperatorEdited(sink);
                            });
                        }

                        widget->selectHistogram(widgetInfo.histoAddress);

                        m_context->addObjectWidget(widget, widgetInfo.sink.get(),
                                                   widgetInfo.sink->getId().toString());
                    }
                    else if (auto widget = qobject_cast<Histo1DListWidget *>(
                            m_context->getObjectWidget(widgetInfo.sink.get())))
                    {
                        widget->selectHistogram(widgetInfo.histoAddress);
                        show_and_activate(widget);
                    }
                } break;

            case NodeType_Histo1DSink:
                {
                    Histo1DWidgetInfo widgetInfo = getHisto1DWidgetInfoFromNode(node);
                    Q_ASSERT(widgetInfo.sink);

                    if (widgetInfo.histos.size())
                    {
                        if (!m_context->hasObjectWidget(widgetInfo.sink.get())
                            || QGuiApplication::keyboardModifiers() & Qt::ControlModifier)
                        {
                            auto widget = new Histo1DListWidget(widgetInfo.histos);
                            widget->setContext(m_context);

                            if (widgetInfo.calib)
                            {
                                widget->setCalibration(widgetInfo.calib);
                            }

                            {
                                auto context = m_context;
                                widget->setSink(widgetInfo.sink,
                                                [context] (const std::shared_ptr<Histo1DSink> &sink) {
                                    context->analysisOperatorEdited(sink);
                                });
                            }

                            m_context->addObjectWidget(widget, widgetInfo.sink.get(),
                                                       widgetInfo.sink->getId().toString());
                        }
                        else
                        {
                            m_context->activateObjectWidget(widgetInfo.sink.get());
                        }
                    }
                } break;

            case NodeType_Histo2DSink:
                {
                    auto sinkPtr = std::dynamic_pointer_cast<Histo2DSink>(
                        get_pointer<Histo2DSink>(node, DataRole_AnalysisObject)->shared_from_this());

                    if (!sinkPtr->m_histo)
                        break;

                    if (!m_context->hasObjectWidget(sinkPtr.get())
                        || QGuiApplication::keyboardModifiers() & Qt::ControlModifier)
                    {
                        auto histoPtr = sinkPtr->m_histo;
                        auto widget = new Histo2DWidget(histoPtr);

                        auto context = m_context;
                        auto eventId = m_eventId;

                        widget->setSink(
                            sinkPtr,
                            // addSinkCallback
                            [context, eventId, userLevel] (const std::shared_ptr<Histo2DSink> &sink) {
                                context->addAnalysisOperator(eventId, sink, userLevel);
                            },
                            // sinkModifiedCallback
                            [context] (const std::shared_ptr<Histo2DSink> &sink) {
                                context->analysisOperatorEdited(sink);
                            },
                            // makeUniqueOperatorNameFunction
                            [context] (const QString &name) {
                                return make_unique_operator_name(context->getAnalysis(), name);
                        });

                        widget->setContext(m_context);

                        m_context->addObjectWidget(widget, sinkPtr.get(), sinkPtr->getId().toString());
                    }
                    else
                    {
                        m_context->activateObjectWidget(sinkPtr.get());
                    }
                } break;

            case NodeType_Sink:
                if (auto rms = get_shared_analysis_object<RateMonitorSink>(node,
                                                                           DataRole_AnalysisObject))
                {
                    if (!m_context->hasObjectWidget(rms.get())
                        || QGuiApplication::keyboardModifiers() & Qt::ControlModifier)
                    {
                        auto context = m_context;
                        auto widget = new RateMonitorWidget(rms->getRateSamplers());

                        widget->setSink(rms, [context](const std::shared_ptr<RateMonitorSink> &sink) {
                            context->analysisOperatorEdited(sink);
                        });

                        widget->setPlotExportDirectory(
                            m_context->getWorkspacePath(QSL("PlotsDirectory")));

                        m_context->addObjectWidget(widget, rms.get(), rms->getId().toString());
                    }
                    else
                    {
                        m_context->activateObjectWidget(rms.get());
                    }
                }
                else if (auto ex = get_shared_analysis_object<ExportSink>(node,
                                                                          DataRole_AnalysisObject))
                {
                    if (!m_context->hasObjectWidget(ex.get())
                        || QGuiApplication::keyboardModifiers() & Qt::ControlModifier)
                    {
                        auto widget = new ExportSinkStatusMonitor(ex, m_context);
                        m_context->addObjectWidget(widget, ex.get(), ex->getId().toString());
                    }
                    else
                    {
                        m_context->activateObjectWidget(ex.get());
                    }
                } break;

            case NodeType_OutputPipe:
                if (auto pipe = get_pointer<Pipe>(node, DataRole_RawPointer))
                {
                    makeAndShowPipeDisplay(pipe);
                } break;

            case NodeType_Operator:
                if (!m_uniqueWidget)
                {
                    if (auto op = get_shared_analysis_object<OperatorInterface>(
                            node, DataRole_AnalysisObject))
                    {
                        auto dialog = operator_editor_factory(
                            op, userLevel, ObjectEditorMode::Edit, DirectoryPtr(), m_q);

                        //POS dialog->move(QCursor::pos());
                        dialog->setAttribute(Qt::WA_DeleteOnClose);
                        dialog->show();
                        m_uniqueWidget = dialog;
                    }
                } break;

            case NodeType_Source:
                if (!m_uniqueWidget)
                {
                    if (auto srcPtr = get_shared_analysis_object<SourceInterface>(
                            node, DataRole_AnalysisObject))
                    {
                        Q_ASSERT_X(srcPtr->getNumberOfOutputs() == 1,
                                   "doOperatorTreeContextMenu",
                                   "data sources with multiple outputs are not supported");

                        auto moduleNode = node->parent();
                        ModuleConfig *moduleConfig = nullptr;

                        if (moduleNode && moduleNode->type() == NodeType_Module)
                            moduleConfig = get_pointer<ModuleConfig>(moduleNode, DataRole_RawPointer);

                        if (moduleConfig)
                        {
                            auto dialog = datasource_editor_factory(
                                srcPtr, userLevel, ObjectEditorMode::Edit, moduleConfig, m_q);

                            assert(dialog);

                            //POS dialog->move(QCursor::pos());
                            dialog->setAttribute(Qt::WA_DeleteOnClose);
                            dialog->show();
                            m_uniqueWidget = dialog;
                        }
                    }
                } break;
        }
    }
}

void EventWidgetPrivate::onNodeChanged(TreeNode *node, int column, s32 userLevel)
{
    //qDebug() << __PRETTY_FUNCTION__ << node << column << userLevel << node->text(0);

    if (column != 0)
        return;

    switch (node->type())
    {
        case NodeType_Source:
        case NodeType_Operator:
        case NodeType_Histo1DSink:
        case NodeType_Histo2DSink:
        case NodeType_Sink:
        case NodeType_Directory:
            break;

        default:
            return;
    }

    //qDebug() << __PRETTY_FUNCTION__
    //    << "node =" << node << ", col =" << column << ", userLevel =" << userLevel;

    if (auto obj = get_pointer<AnalysisObject>(node, DataRole_AnalysisObject))
    {
        auto value    = node->data(0, Qt::EditRole).toString();
        bool modified = value != obj->objectName();

        //qDebug() << __PRETTY_FUNCTION__
        //    << "node =" << node
        //    << ", value =" << value
        //    << ", objName =" << obj->objectName()
        //    << ", modified =" << modified;

        if (modified)
        {
            obj->setObjectName(value);
            m_q->getAnalysis()->setModified(true);

            if (auto op = qobject_cast<OperatorInterface *>(obj))
            {
                node->setData(0, Qt::DisplayRole, QString("<b>%1</b> %2").arg(
                        op->getShortName(),
                        op->objectName()));
            }
            else
            {
                node->setData(0, Qt::DisplayRole, value);
            }
        }
    }
}

void EventWidgetPrivate::onNodeCheckStateChanged(QTreeWidget *tree,
                                                 QTreeWidgetItem *node, const QVariant &prev)
{
    qDebug() << __PRETTY_FUNCTION__ << this << tree
        << node << ", checkstate =" << node->data(0, Qt::CheckStateRole)
        << ", prev =" << prev;

    assert(m_applyConditionInfo);

    if (m_applyConditionInfo)
    {
        auto &cl  = m_applyConditionInfo;
        auto analysis = getAnalysis();
        auto clMods = get_condition_modifications(cl, analysis, m_objectMap);

        emit m_q->conditionLinksModified(cl, clMods.hasModifications());
    }
}

void EventWidgetPrivate::clearAllTreeSelections()
{
    for (UserLevelTrees trees: m_levelTrees)
    {
        for (auto tree: trees.getObjectTrees())
        {
            tree->clearSelection();
        }
    }
}

void EventWidgetPrivate::clearTreeSelectionsExcept(QTreeWidget *treeNotToClear)
{
    for (UserLevelTrees trees: m_levelTrees)
    {
        for (auto tree: trees.getObjectTrees())
        {
            if (tree != treeNotToClear)
            {
                tree->clearSelection();
            }
        }
    }
}

void EventWidgetPrivate::generateDefaultFilters(ModuleConfig *module)
{
    {
        AnalysisPauser pauser(m_context);

        auto defaultFilters = get_default_data_extractors(module->getModuleMeta().typeName);

        for (auto &ex: defaultFilters)
        {
            auto dataFilter = ex->getFilter();
            double unitMin = 0.0;
            double unitMax = std::pow(2.0, dataFilter.getDataBits());
            QString name = module->getModuleMeta().typeName + QSL(".")
                + ex->objectName().section('.', 0, -1);

            RawDataDisplay rawDataDisplay = make_raw_data_display(dataFilter, unitMin, unitMax,
                                                                  name,
                                                                  ex->objectName().section('.', 0, -1),
                                                                  QString());

            add_raw_data_display(m_context->getAnalysis(), m_eventId, module->getId(), rawDataDisplay);
            m_context->getAnalysis()->beginRun(Analysis::KeepState);
        }
    }

    repopulate();
}

PipeDisplay *EventWidgetPrivate::makeAndShowPipeDisplay(Pipe *pipe)
{
    auto widget = new PipeDisplay(m_context->getAnalysis(), pipe, m_q);
    QObject::connect(m_displayRefreshTimer, &QTimer::timeout, widget, &PipeDisplay::refresh);
    QObject::connect(pipe->source, &QObject::destroyed, widget, &QWidget::close);
    add_widget_close_action(widget);
    widget->move(QCursor::pos());
    widget->setAttribute(Qt::WA_DeleteOnClose);
    widget->show();
    return widget;
}

void EventWidgetPrivate::doPeriodicUpdate()
{
    /* If it's a replay: use timeticks
     * If it's DAQ: use elapsed walltime
     * Reason: if analysis efficiency is < 1.0 timeticks will be lost. Thus
     * using timeticks with a DAQ run may lead to very confusing numbers as
     * sometimes ticks will be lost, at other times they'll appear.
     */

    auto analysis = m_context->getAnalysis();
    bool isReplay = analysis->getRunInfo().isReplay;
    double dt_s = 0.0;
    double currentAnalysisTimeticks = analysis->getTimetickCount();

    if (isReplay)
    {
        dt_s = calc_delta0(currentAnalysisTimeticks, m_prevAnalysisTimeticks);
    }
    else
    {
        dt_s = PeriodicUpdateTimerInterval_ms / 1000.0;
    }

    periodicUpdateExtractorCounters(dt_s);
    periodicUpdateHistoCounters(dt_s);
    periodicUpdateEventRate(dt_s);

    m_prevAnalysisTimeticks = currentAnalysisTimeticks;
}

void EventWidgetPrivate::periodicUpdateExtractorCounters(double dt_s)
{
    auto analysis = m_context->getAnalysis();
    auto a2State = analysis->getA2AdapterState();

    //
    // level 0: operator tree (Extractor hitcounts)
    //
    for (auto iter = QTreeWidgetItemIterator(m_levelTrees[0].operatorTree);
         *iter; ++iter)
    {
        auto node(*iter);

        if (node->type() == NodeType_Source)
        {
            auto source = qobject_cast<SourceInterface *>(get_pointer<PipeSourceInterface>(
                    node, DataRole_AnalysisObject));

            if (!source)
                continue;

            if (source->getModuleId().isNull()) // source not assigned to a module
                continue;

            auto ds_a2 = a2State->sourceMap.value(source, nullptr);

            if (!ds_a2)
                continue;

            auto hitCounts = to_qvector(ds_a2->hitCounts);

            if (hitCounts.size() != node->childCount())
                continue;

            auto &prevHitCounts = m_extractorCounters[source].hitCounts;

            prevHitCounts.resize(hitCounts.size());

            auto hitCountDeltas = calc_deltas0(hitCounts, prevHitCounts);
            auto hitCountRates = hitCountDeltas;
            std::for_each(hitCountRates.begin(), hitCountRates.end(),
                          [dt_s](double &d) { d /= dt_s; });

            Q_ASSERT(hitCounts.size() == node->childCount());

            for (s32 addr = 0; addr < node->childCount(); ++addr)
            {
                Q_ASSERT(node->child(addr)->type() == NodeType_OutputPipeParameter);

                QString addrString = QString("%1").arg(addr, 2).replace(QSL(" "), QSL("&nbsp;"));

                double hitCount = hitCounts[addr];
                auto childNode = node->child(addr);

                if (hitCount <= 0.0)
                {
                    childNode->setText(0, addrString);
                }
                else
                {
                    double rate = hitCountRates[addr];

                    if (std::isnan(rate)) rate = 0.0;

                    auto rateString = format_number(rate, QSL("cps"), UnitScaling::Decimal,
                                                    0, 'g', 3);

                    childNode->setText(0, QString("%1 (hits=%2, rate=%3, dt=%4 s)")
                                       .arg(addrString)
                                       .arg(hitCount)
                                       .arg(rateString)
                                       .arg(dt_s)
                                      );
                }
            }

            prevHitCounts = hitCounts;
        }
    }
}

void EventWidgetPrivate::periodicUpdateHistoCounters(double dt_s)
{
    auto analysis = m_context->getAnalysis();
    auto a2State = analysis->getA2AdapterState();

    //
    // level > 0: display trees (histo counts)
    //
    for (auto trees: m_levelTrees)
    {
        for (auto iter = QTreeWidgetItemIterator(trees.sinkTree);
             *iter; ++iter)
        {
            auto node(*iter);

            if (node->type() == NodeType_Histo1DSink)
            {
                auto histoSink = qobject_cast<Histo1DSink *>(get_pointer<OperatorInterface>(
                        node, DataRole_AnalysisObject));

                if (!histoSink)
                    continue;

                if (histoSink->m_histos.size() != node->childCount())
                    continue;

                QVector<double> entryCounts;

                if (a2State)
                {
                    if (auto a2_sink = a2State->operatorMap.value(histoSink, nullptr))
                    {
                        auto sinkData = reinterpret_cast<a2::H1DSinkData *>(a2_sink->d);

                        entryCounts.reserve(sinkData->histos.size);

                        for (s32 i = 0; i < sinkData->histos.size; i++)
                        {
                            entryCounts.push_back(sinkData->histos[i].entryCount);
                        }
                    }
                }

                auto &prevEntryCounts = m_histo1DSinkCounters[histoSink].hitCounts;

                prevEntryCounts.resize(entryCounts.size());

                auto entryCountDeltas = calc_deltas0(entryCounts, prevEntryCounts);
                auto entryCountRates = entryCountDeltas;
                std::for_each(entryCountRates.begin(), entryCountRates.end(),
                              [dt_s](double &d) { d /= dt_s; });

                auto maxCount = std::min(entryCounts.size(), node->childCount());

                for (s32 addr = 0; addr < maxCount; ++addr)
                {
                    Q_ASSERT(node->child(addr)->type() == NodeType_Histo1D);

                    QString numberString = QString("%1").arg(addr, 2).replace(QSL(" "), QSL("&nbsp;"));

                    double entryCount = entryCounts[addr];
                    auto childNode = node->child(addr);

                    if (entryCount <= 0.0)
                    {
                        childNode->setText(0, numberString);
                    }
                    else
                    {
                        double rate = entryCountRates[addr];
                        if (std::isnan(rate)) rate = 0.0;

                        auto rateString = format_number(rate, QSL("cps"), UnitScaling::Decimal,
                                                        0, 'g', 3);

                        childNode->setText(0, QString("%1 (entries=%2, rate=%3, dt=%4 s)")
                                           .arg(numberString)
                                           .arg(entryCount, 0, 'g', 3)
                                           .arg(rateString)
                                           .arg(dt_s)
                                          );
                    }
                }

                prevEntryCounts = entryCounts;
            }
            else if (node->type() == NodeType_Histo2DSink)
            {
                auto sink = get_pointer<Histo2DSink>(node, DataRole_AnalysisObject);
                auto histo = sink ? sink->m_histo : nullptr;

                if (histo)
                {
                    double entryCount = 0.0;

                    if (auto a2_sink = a2State->operatorMap.value(sink, nullptr))
                    {
                        auto sinkData = reinterpret_cast<a2::H2DSinkData *>(a2_sink->d);

                        entryCount = sinkData->histo.entryCount;
                    }
                    auto &prevEntryCounts = m_histo2DSinkCounters[sink].hitCounts;
                    prevEntryCounts.resize(1);

                    double prevEntryCount = prevEntryCounts[0];

                    double countDelta = calc_delta0(entryCount, prevEntryCount);
                    double countRate = countDelta / dt_s;

                    if (entryCount <= 0.0)
                    {
                        node->setText(0, QString("<b>%1</b> %2")
                                      .arg(sink->getShortName())
                                      .arg(sink->objectName())
                                     );
                    }
                    else
                    {
                        if (std::isnan(countRate)) countRate = 0.0;

                        auto rateString = format_number(countRate, QSL("cps"), UnitScaling::Decimal,
                                                        0, 'g', 3);

                        node->setText(0, QString("<b>%1</b> %2 (entries=%3, rate=%4, dt=%5)")
                                      .arg(sink->getShortName())
                                      .arg(sink->objectName())
                                      .arg(entryCount, 0, 'g', 3)
                                      .arg(rateString)
                                      .arg(dt_s)
                                     );
                    }

                    prevEntryCounts[0] = entryCount;
                }
            }
        }
    }

}

void EventWidgetPrivate::periodicUpdateEventRate(double dt_s)
{
    auto &prevCounters(m_prevStreamProcessorCounters);
    const auto &counters(m_context->getMVMEStreamWorker()->getCounters());
    Q_ASSERT(0 <= m_eventIndex && m_eventIndex < (s32)counters.eventCounters.size());

    /* Use the counters of the first module in this event as that represents
     * the event rate after multi-event splitting. */
    double deltaEvents = calc_delta0(
        counters.moduleCounters[m_eventIndex][0],
        prevCounters.moduleCounters[m_eventIndex][0]);

    double eventCount = counters.moduleCounters[m_eventIndex][0];
    double eventRate = deltaEvents / dt_s;
    if (std::isnan(eventRate)) eventRate = 0.0;

    auto labelText = (QString("count=%1\nrate=%2")
                      .arg(format_number(eventCount, QSL(""), UnitScaling::Decimal))
                      .arg(format_number(eventRate, QSL("cps"), UnitScaling::Decimal, 0, 'g', 3))
                     );

    if (m_context->getAnalysis()->getRunInfo().isReplay)
    {
        double walltimeRate = deltaEvents / (PeriodicUpdateTimerInterval_ms / 1000.0);

        labelText += (QString("\nreplayRate=%1")
                      .arg(format_number(walltimeRate, QSL("cps"), UnitScaling::Decimal, 0, 'g', 3))
                      );
    }
    else // not a replay
    {
        auto daqStats = m_context->getDAQStats();
        double efficiency = daqStats.getAnalysisEfficiency();
        efficiency = std::isnan(efficiency) ? 0.0 : efficiency;

        labelText += QSL("\nEfficiency=%1").arg(efficiency, 0, 'f', 2);
    }

#ifndef NDEBUG
    labelText += QSL("\nMode=%1").arg(mode_to_string(m_mode));
#endif

    m_eventRateLabel->setText(labelText);

    prevCounters = counters;
}


QTreeWidgetItem *EventWidgetPrivate::getCurrentNode() const
{
    QTreeWidgetItem *result = nullptr;

    if (auto activeTree = qobject_cast<QTreeWidget *>(m_q->focusWidget()))
    {
        result = activeTree->currentItem();
    }

    return result;
}

/* Returns the concatenation of the individual tree selections.
 * Note that the results are not sorted in a specific way but reflect the ordering of the
 * unterlying Qt itemview selection mechanism. */
QList<QTreeWidgetItem *> EventWidgetPrivate::getAllSelectedNodes() const
{
    QList<QTreeWidgetItem *> result;

    for (const auto &trees: m_levelTrees)
    {
        result.append(trees.operatorTree->selectedItems());
        result.append(trees.sinkTree->selectedItems());
    }

    return result;
}

/* Returns the set of selected analysis objects across all userlevel tree widgets.
 * Note that the results are not sorted in a specific way but reflect the ordering of the
 * unterlying Qt itemview selection mechanism. */
AnalysisObjectVector EventWidgetPrivate::getAllSelectedObjects() const
{
    return objects_from_nodes(getAllSelectedNodes());
}

/* Returns the concatenation of the individual tree selections. Only top level nodes are
 * returned, meaning if a tree selection contains and object and its parent directory,
 * only the parent directory is added to the result.
 * Note that the results are not sorted in a specific way but reflect the ordering of the
 * unterlying Qt itemview selection mechanism. */
QList<QTreeWidgetItem *> EventWidgetPrivate::getTopLevelSelectedNodes() const
{
    QList<QTreeWidgetItem *> result;

    for (const auto &trees: m_levelTrees)
    {
        result.append(trees.operatorTree->getTopLevelSelectedNodes());
        result.append(trees.sinkTree->getTopLevelSelectedNodes());
    }

    return result;
}

/* Returns the set of selected top-level analysis objects across all userlevel tree widgets.
 * Note that the results are not sorted in a specific way but reflect the ordering of the
 * unterlying Qt itemview selection mechanism. */
AnalysisObjectVector EventWidgetPrivate::getTopLevelSelectedObjects() const
{
    return objects_from_nodes(getTopLevelSelectedNodes());
}

QVector<QTreeWidgetItem *> EventWidgetPrivate::getCheckedNodes(
    Qt::CheckState checkState, int checkStateColumn) const
{
    QVector<QTreeWidgetItem *> result;

    for (const auto &trees: m_levelTrees)
    {
        for (const auto &tree: trees.getObjectTrees())
        {
            get_checked_nodes(result, tree->invisibleRootItem(),
                              checkState, checkStateColumn);
        }
    }

    return result;
}

AnalysisObjectVector EventWidgetPrivate::getCheckedObjects(
    Qt::CheckState checkState, int checkStateColumn) const
{
    return objects_from_nodes(getCheckedNodes(checkState, checkStateColumn));
}

void EventWidgetPrivate::clearSelections()
{
    for (const auto &trees: m_levelTrees)
    {
        for (auto tree: trees.getObjectTrees())
        {
            tree->selectionModel()->clear();
        }
    }
}

static bool select_objects(QTreeWidgetItem *root, const AnalysisObjectSet &objects)
{
    bool didSelect = false;

    switch (root->type())
    {
        case NodeType_Source:
        case NodeType_Operator:
        case NodeType_Histo1DSink:
        case NodeType_Histo2DSink:
        case NodeType_Sink:
        case NodeType_Directory:
            {
                auto obj = get_shared_analysis_object<AnalysisObject>(root);

                if (objects.contains(obj))
                {
                    root->setSelected(true);
                    didSelect = true;
                }
            }
            break;

        default:
            break;
    }

    for (int ci = 0; ci < root->childCount(); ci++)
    {
        auto child = root->child(ci);
        if (select_objects(child, objects))
            root->setExpanded(true);
    }

    return didSelect;
}

void EventWidgetPrivate::selectObjects(const AnalysisObjectVector &objects)
{
    clearSelections();

    auto objectSet = to_set(objects);

    for (const auto &trees: m_levelTrees)
    {
        for (auto tree: trees.getObjectTrees())
        {
            auto root = tree->invisibleRootItem();
            select_objects(root, objectSet);
        }
    }
}

void EventWidgetPrivate::updateActions()
{
    auto node = getCurrentNode();

    m_actionExport->setEnabled(false);

    if (m_mode == Default)
    {
        m_actionExport->setEnabled(canExport());
    }
}

bool EventWidgetPrivate::canExport() const
{
    for (const auto &node: getAllSelectedNodes())
    {
        switch (node->type())
        {
            case NodeType_Source:
            case NodeType_Operator:
            case NodeType_Histo1DSink:
            case NodeType_Histo2DSink:
            case NodeType_Sink:
            case NodeType_Directory:
                return true;
        }
    }

    return false;
}

static const char *AnalysisLibraryFileFilter =
    "MVME Analysis Library Files (*.analysislib);; All Files (*.*)";

static const char *AnalysisLibraryFileExtension = ".analysislib";

void EventWidgetPrivate::actionExport()
{
    assert(canExport());

    // Step 0) Let the user pick a file
    auto path = m_context->getWorkspaceDirectory();

    if (path.isEmpty())
        path = QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation).at(0);

    QString fileName = QFileDialog::getSaveFileName(m_q, QSL("Select file to export to"),
                                                    path, AnalysisLibraryFileFilter);

    if (fileName.isEmpty())
        return;

    QFileInfo fi(fileName);

    if (fi.completeSuffix().isEmpty())
        fileName += AnalysisLibraryFileExtension;

    // Step 1) Collect all objects that have to be written out
    auto analysis = m_context->getAnalysis();
    auto selectedObjects = getAllSelectedObjects();
    auto allObjects = order_objects(expand_objects(selectedObjects, analysis), analysis);

    qDebug() << __PRETTY_FUNCTION__
        << "#selected =" << selectedObjects.size()
        << ", #collected =" << allObjects.size();

    // Step 2) Create the JSON structures and the document
    ObjectSerializerVisitor sv;
    visit_objects(allObjects.begin(), allObjects.end(), sv);

    QJsonObject exportRoot;
    exportRoot["MVMEAnalysisExport"] = sv.finalize(analysis);

    QJsonDocument doc(exportRoot);

    qDebug() << __PRETTY_FUNCTION__
        << "exporting" << sv.objectCount() << "objects";

    // Step 3) Write to file
    // FIXME: replace with something that can give a specific error message for
    // this concrete file save operation instead of just a generic write error
    gui_write_json_file(fileName, doc);
}

void EventWidgetPrivate::actionImport()
{
    /* Global import without a specific target directory/userlevel or a
     * subselection of objects.
     * The following should happen:
     * Read in the file, check for version errors and create all contained objects.
     * Place them as is, without modifying userlevels or directories.
     * Regenerate unique IDs
     * Later: for each imported object check if an object of the same type and
     * name exists. If so append a suffix to the object name to make it unique.
     * Finally select the newly added objects.
     */
    qDebug() << __PRETTY_FUNCTION__;

    QString startPath = m_context->getWorkspaceDirectory();

    QString fileName = QFileDialog::getOpenFileName(m_q, QSL("Import analysis objects"),
                                                    startPath, AnalysisLibraryFileFilter);

    if (fileName.isEmpty())
        return;

    QJsonDocument doc(gui_read_json_file(fileName));
    auto exportRoot = doc.object();

    if (!exportRoot.contains("MVMEAnalysisExport"))
    {
        QMessageBox::critical(m_q, "File format error", "File format error");
        return;
    }

    auto importData = exportRoot["MVMEAnalysisExport"].toObject();

    try
    {
        auto analysis = m_context->getAnalysis();

        check_directory_consistency(analysis->getDirectories(), analysis);

        auto objectStore = deserialize_objects(
            importData,
            m_context->getVMEConfig(),
            analysis->getObjectFactory());

        check_directory_consistency(objectStore.directories);

        establish_connections(objectStore);

        generate_new_object_ids(objectStore.allObjects());

        // Assign all imported objects to the current event.

        for (auto &obj: objectStore.sources)
        {
            // Reset the data sources module id. This will make the source unassigned any
            // module and the user has to assign it later.
            obj->setModuleId(QUuid());
            obj->setEventId(m_eventId);
        }

        for (auto &obj: objectStore.operators)
        {
            obj->setEventId(m_eventId);
        }

        for (auto &obj: objectStore.directories)
        {
            obj->setEventId(m_eventId);
        }

        check_directory_consistency(objectStore.directories);

        AnalysisPauser pauser(m_context);
        analysis->addObjects(objectStore);

        check_directory_consistency(analysis->getDirectories(), analysis);

        repopulate();
        selectObjects(objectStore.allObjects());
    }
    catch (const std::runtime_error &e)
    {
        QMessageBox::critical(m_q, "Import error", e.what());
    }
}

void EventWidgetPrivate::setSinksEnabled(const SinkVector &sinks, bool enabled)
{
    if (sinks.isEmpty())
        return;

    AnalysisPauser pauser(m_context);

    for (auto sink: sinks)
    {
        sink->setEnabled(enabled);
    }

    m_context->getAnalysis()->setModified(true);
    repopulate();
}

void EventWidgetPrivate::removeSinks(const QVector<SinkInterface *> sinks)
{
    if (sinks.isEmpty())
        return;

    AnalysisPauser pauser(m_context);

    for (auto sink: sinks)
    {
        m_context->getAnalysis()->removeOperator(sink);
    }

    repopulate();
    m_analysisWidget->updateAddRemoveUserLevelButtons();
}

void EventWidgetPrivate::removeDirectoryRecursively(const DirectoryPtr &dir)
{
    auto analysis = m_context->getAnalysis();
    auto objects = analysis->getDirectoryContents(dir);

    if (!objects.isEmpty())
    {
        AnalysisPauser pauser(m_context);
        analysis->removeDirectoryRecursively(dir);
    }
    else
    {
        analysis->removeDirectory(dir);
    }

    repopulate();
}

void EventWidgetPrivate::removeObjects(const AnalysisObjectVector &objects)
{
    qDebug() << __PRETTY_FUNCTION__ << objects;

    if (!objects.isEmpty())
    {
        AnalysisPauser pauser(m_context);
        auto analysis = m_context->getAnalysis();
        analysis->removeObjectsRecursively(objects);
        m_analysisWidget->updateAddRemoveUserLevelButtons();
        repopulate();
    }
}

static QTreeWidgetItem *find_node(QTreeWidgetItem *root, const AnalysisObjectPtr &obj)
{
    if (root)
    {
        if (get_pointer<void>(root, DataRole_AnalysisObject) == obj.get())
            return root;

        const s32 childCount = root->childCount();

        for (s32 ci = 0; ci < childCount; ci++)
        {
            if (auto result = find_node(root->child(ci), obj))
                return result;
        }
    }

    return nullptr;
}

QTreeWidgetItem *EventWidgetPrivate::findNode(const AnalysisObjectPtr &obj)
{
    for (auto &trees: m_levelTrees)
    {
        if (auto node = find_node(trees.operatorTree->invisibleRootItem(), obj))
            return node;

        if (auto node = find_node(trees.sinkTree->invisibleRootItem(), obj))
            return node;
    }

    return nullptr;
}

void EventWidgetPrivate::copyToClipboard(const AnalysisObjectVector &objects)
{
    qDebug() << __PRETTY_FUNCTION__;

    QVector<QByteArray> idData;
    idData.reserve(objects.size());

    for (auto obj: objects)
    {
        idData.push_back(obj->getId().toByteArray());
    }

    QByteArray buffer;
    QDataStream stream(&buffer, QIODevice::WriteOnly);
    stream << idData;

    auto mimeData = new QMimeData;
    mimeData->setData(ObjectIdListMIMEType, buffer);

    QGuiApplication::clipboard()->setMimeData(mimeData);
}

bool EventWidgetPrivate::canPaste()
{
    auto clipboardData = QGuiApplication::clipboard()->mimeData();

    return clipboardData->hasFormat(ObjectIdListMIMEType);
}


} // ns ui
} // ns analysis