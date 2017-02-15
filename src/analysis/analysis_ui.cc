#include "analysis_ui.h"
#include "analysis.h"
#include "../mvme_context.h"

#include <QHBoxLayout>
#include <QMenu>
#include <QScrollArea>
#include <QSplitter>
#include <QTreeWidget>

namespace analysis
{

typedef QTreeWidgetItem TreeNode;

enum DataRole
{
    DataRole_Pointer = Qt::UserRole,
};

enum NodeType
{
    NodeType_Module = QTreeWidgetItem::UserType,
    NodeType_Source,
    NodeType_Operator,
    NodeType_Histo1DSink,
    NodeType_Histo1D,
};

template<typename T>
TreeNode *makeNode(T *data, int type = QTreeWidgetItem::Type)
{
    auto ret = new TreeNode(type);
    ret->setData(0, DataRole_Pointer, QVariant::fromValue(static_cast<void *>(data)));
    return ret;
}

QObject *getQObject(TreeNode *node)
{
    return node ? reinterpret_cast<QObject *>(node->data(0, DataRole_Pointer).value<void *>()) : nullptr;
}

inline TreeNode *makeModuleNode(ModuleConfig *mod)
{
    auto node = makeNode(mod, NodeType_Module);
    node->setText(0, mod->objectName());
    node->setIcon(0, QIcon(":/vme_module.png"));
    return node;
};

inline TreeNode *makeOperatorTreeSourceNode(SourceInterface *source)
{
    auto sourceNode = makeNode(source, NodeType_Source);
    sourceNode->setText(0, source->objectName());
    sourceNode->setIcon(0, QIcon(":/data_filter.png"));

    if (auto extractor = qobject_cast<Extractor *>(source))
    {
        u32 addressCount = (1 << extractor->getFilter().getAddressBits());

        for (u32 address = 0; address < addressCount; ++address)
        {
            auto addressNode = new TreeNode;
            addressNode->setText(0, QString::number(address));
            sourceNode->addChild(addressNode);
        }
    }

    return sourceNode;
}

inline TreeNode *makeDisplayTreeSourceNode(SourceInterface *source)
{
    auto sourceNode = makeNode(source, NodeType_Source);
    sourceNode->setText(0, source->objectName());
    sourceNode->setIcon(0, QIcon(":/data_filter.png"));

    return sourceNode;
}

inline TreeNode *makeHistoNode(Histo1DSink *op)
{
    auto node = makeNode(op, NodeType_Histo1DSink);
    node->setText(0, QString("%1 %2").arg(
            op->getDisplayName(),
            op->objectName()));
    node->setIcon(0, QIcon(":/hist1d.png"));

    if (op->histos.size() > 1)
    {
        for (s32 addr = 0; addr < op->histos.size(); ++addr)
        {
            auto histo = op->histos[addr].get();
            auto histoNode = makeNode(histo, NodeType_Histo1D);
            histoNode->setText(0, QString::number(addr));
            histoNode->setIcon(0, QIcon(":/hist1d.png"));

            node->addChild(histoNode);
        }
    }
    return node;
};

#if 1 // PIPE CHANGES
inline TreeNode *makeOperatorNode(OperatorInterface *op)
{
    auto result = makeNode(op, NodeType_Operator);
    result->setText(0, QString("%1 %2").arg(
            op->getDisplayName(),
            op->objectName()));
    result->setIcon(0, QIcon(":/analysis_operator.png"));

    auto inputsNode = new TreeNode;
    inputsNode->setText(0, "inputs");
    result->addChild(inputsNode);

    auto outputsNode = new TreeNode;
    outputsNode->setText(0, "outputs");
    result->addChild(outputsNode);

    // inputs
    for (s32 inputIndex = 0;
         inputIndex < op->getNumberOfSlots();
         ++inputIndex)
    {
        // FIXME: check for array or value slot
        // display slot acceptance type
        // display if no inputpipe connected
        // add something like NodeType_Slot
        // add something like NodeType_ArrayValue or ParamIndex VectorElement ... best name for this?
        // add NodeType_OutputPipe, Output, ...

        auto slot = op->getSlot(inputIndex);
        auto inputPipe = slot->inputPipe;
        s32 inputParamSize = inputPipe ? inputPipe->parameters.size() : 0;

        auto slotNode = new TreeNode;
        slotNode->setText(0, QString("#%1 \"%2\" (%3 elements)")
                          .arg(inputIndex)
                          .arg(slot->name)
                          .arg(inputParamSize)
                         );

        inputsNode->addChild(slotNode);

        for (s32 paramIndex = 0; paramIndex < inputParamSize; ++paramIndex)
        {
            auto paramNode = new TreeNode;
            paramNode->setText(0, QString("[%1]").arg(paramIndex));

            slotNode->addChild(paramNode);
        }
    }

    // outputs
    for (s32 outputIndex = 0;
         outputIndex < op->getNumberOfOutputs();
         ++outputIndex)
    {
        auto outputPipe = op->getOutput(outputIndex);
        s32 outputParamSize = outputPipe->parameters.size();

        auto pipeNode = new TreeNode;
        pipeNode->setText(0, QString("#%1 \"%2\" (%3 elements)")
                          .arg(outputIndex)
                          .arg(op->getOutputName(outputIndex))
                          .arg(outputParamSize)
                         );
        outputsNode->addChild(pipeNode);

        for (s32 paramIndex = 0; paramIndex < outputParamSize; ++paramIndex)
        {
            auto paramNode = new TreeNode;
            paramNode->setText(0, QString("[%1]").arg(paramIndex));

            pipeNode->addChild(paramNode);
        }
    }

    return result;
};

struct DisplayLevelTrees
{
    QTreeWidget *operatorTree;
    QTreeWidget *displayTree;
};

struct AnalysisWidgetPrivate
{
    AnalysisWidget *m_q;
    MVMEContext *context;
    QVector<DisplayLevelTrees> levelTrees;

    void createView(int eventIndex);
    DisplayLevelTrees createTrees(s32 eventIndex, s32 level);
    DisplayLevelTrees createSourceTrees(s32 eventIndex);

    void doOperatorTreeContextMenu(QTreeWidget *tree, QPoint pos);
    void doDisplayTreeContextMenu(QTreeWidget *tree, QPoint pos);
};

void AnalysisWidgetPrivate::createView(int eventIndex)
{
    auto analysis = context->getAnalysisNG();
    s32 maxUserLevel = 0;

    for (const auto &opEntry: analysis->getOperators(eventIndex))
    {
        maxUserLevel = std::max(maxUserLevel, opEntry.userLevel);
    }

    // To make an empty display for the next level a user might want use.
    ++maxUserLevel;

    for (s32 userLevel = 0; userLevel <= maxUserLevel; ++userLevel)
    {
        levelTrees.push_back(createTrees(eventIndex, userLevel));
    }
}

DisplayLevelTrees AnalysisWidgetPrivate::createTrees(s32 eventIndex, s32 level)
{
    // Level 0: special case for data sources
    if (level == 0)
    {
        return createSourceTrees(eventIndex);
    }

    DisplayLevelTrees result = { new QTreeWidget, new QTreeWidget };
    auto headerItem = result.operatorTree->headerItem();
    headerItem->setText(0, QString(QSL("L%1 Processing")).arg(level));

    headerItem = result.displayTree->headerItem();
    headerItem->setText(0, QString(QSL("L%1 Data Display")).arg(level));

    // Build a list of operators for the current level

    auto analysis = context->getAnalysisNG();
    QVector<Analysis::OperatorEntry> operators = analysis->getOperators(eventIndex, level);

    // populate the OperatorTree
    for (auto entry: operators)
    {
        auto histo1DSink = qobject_cast<Histo1DSink *>(entry.op.get());
        auto histo2DSink = qobject_cast<Histo2DSink *>(entry.op.get());
        if (!histo1DSink && !histo2DSink)
        {
            auto opNode = makeOperatorNode(entry.op.get());
            result.operatorTree->addTopLevelItem(opNode);
        }
    }

    // populate the DisplayTree
    {
        auto histo1DNode = new TreeNode({QSL("1D")});
        auto histo2DNode = new TreeNode({QSL("2D")});
        result.displayTree->addTopLevelItem(histo1DNode);
        result.displayTree->addTopLevelItem(histo2DNode);

        QVector<Histo1DSink *> histoSinks;
        for (const auto &entry: operators)
        {
            auto histo1DSink = qobject_cast<Histo1DSink *>(entry.op.get());
            if (histo1DSink)
            {
                histoSinks.push_back(histo1DSink);
            }
        }

        for (auto histo1DSink: histoSinks)
        {
                auto histoNode = makeHistoNode(histo1DSink);
                histo1DNode->addChild(histoNode);
        }
    }

    return result;
}

DisplayLevelTrees AnalysisWidgetPrivate::createSourceTrees(s32 eventIndex)
{
    auto analysis = context->getAnalysisNG();
    auto vmeConfig = context->getDAQConfig();

    auto eventConfig = vmeConfig->getEventConfig(eventIndex);
    auto modules = eventConfig->getModuleConfigs();

    DisplayLevelTrees result = { new QTreeWidget, new QTreeWidget };

    auto headerItem = result.operatorTree->headerItem();
    headerItem->setText(0, QSL("L0 Parameter Extraction"));

    headerItem = result.displayTree->headerItem();
    headerItem->setText(0, QSL("L0 Data Display"));

    // populate the OperatorTree
    int moduleIndex = 0;
    for (auto mod: modules)
    {
        auto moduleNode = makeModuleNode(mod);
        result.operatorTree->addTopLevelItem(moduleNode);

        for (auto sourceEntry: analysis->getSources(eventIndex, moduleIndex))
        {
            auto sourceNode = makeOperatorTreeSourceNode(sourceEntry.source.get());
            moduleNode->addChild(sourceNode);
        }
        ++moduleIndex;
    }

    // populate the DisplayTree
    moduleIndex = 0;
    auto opEntries = analysis->getOperators(eventIndex, 0);
    for (auto mod: modules)
    {
        auto moduleNode = makeModuleNode(mod);
        result.displayTree->addTopLevelItem(moduleNode);

        for (auto sourceEntry: analysis->getSources(eventIndex, moduleIndex))
        {
            // FIXME: might need a different node type here as this should display a HistoListWidget when clicked
            // It's a "histo grouping node" or "histo collection node" or something
            auto sourceNode = makeDisplayTreeSourceNode(sourceEntry.source.get());
            moduleNode->addChild(sourceNode);

            // TODO: move into makeDisplayTreeSourceNode!?
            // FIXME: almost the same code as in createTrees()
            // But this here is the special case for level 0 stuff
            QVector<Histo1DSink *> histoSinks;
            for (const auto &entry: opEntries)
            {
                auto histoSink = qobject_cast<Histo1DSink *>(entry.op.get());
                if (histoSink
                    && histoSink->getSlot(0)
                    && (histoSink->getSlot(0)->inputPipe == sourceEntry.source->getOutput(0)))
                {
                    histoSinks.push_back(histoSink);
                }
            }

            for (auto histoSink: histoSinks)
            {
                    auto histoNode = makeHistoNode(histoSink);
                    sourceNode->addChild(histoNode);
            }
        }
        ++moduleIndex;
    }

    return result;
}

void AnalysisWidgetPrivate::doOperatorTreeContextMenu(QTreeWidget *tree, QPoint pos)
{
    auto node = tree->itemAt(pos);
}

void AnalysisWidgetPrivate::doDisplayTreeContextMenu(QTreeWidget *tree, QPoint pos)
{
    auto node = tree->itemAt(pos);
    auto obj  = getQObject(node);

    QMenu menu;

    if (node)
    {
        switch (node->type())
        {
            case NodeType_Histo1D:
                {
                    if (auto histo = qobject_cast<Histo1D *>(obj))
                    {
                        menu.addAction(QSL("Open"), m_q, [this, histo]() {
                            context->openInNewWindow(histo);
                        });
                    }
                } break;
        }
    }

    if (!menu.isEmpty())
    {
        menu.exec(tree->mapToGlobal(pos));
    }
}
#endif

AnalysisWidget::AnalysisWidget(MVMEContext *ctx, QWidget *parent)
    : QWidget(parent)
#if 1 // PIPE CHANGES
    , m_d(new AnalysisWidgetPrivate)
#endif
{
#if 1 // PIPE CHANGES
    setMinimumSize(1000, 600); // FIXME: find another way to make the window be sanely sized at startup
    m_d->m_q = this;
    m_d->context = ctx;

    // TODO: This needs to be done when the analysis object is modified.
    auto analysis = ctx->getAnalysisNG();
    analysis->updateRanks();
    analysis->beginRun();

    auto outerLayout = new QHBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    auto scrollArea = new QScrollArea;
    outerLayout->addWidget(scrollArea);

    // FIXME: I don't get how scrollarea works
    //auto scrollWidget = new QWidget;
    //scrollArea->setWidget(scrollWidget);

    auto scrollLayout = new QHBoxLayout(scrollArea);
    scrollLayout->setContentsMargins(0, 0, 0, 0);

    // row frames and splitter
    auto rowSplitter = new QSplitter(Qt::Vertical);
    scrollLayout->addWidget(rowSplitter);

    auto operatorFrame = new QFrame;
    auto operatorFrameLayout = new QHBoxLayout(operatorFrame);
    operatorFrameLayout->setContentsMargins(2, 2, 2, 2);
    rowSplitter->addWidget(operatorFrame);

    auto displayFrame = new QFrame;
    auto displayFrameLayout = new QHBoxLayout(displayFrame);
    displayFrameLayout->setContentsMargins(2, 2, 2, 2);
    rowSplitter->addWidget(displayFrame);

    // column frames and splitters
    auto operatorFrameColumnSplitter = new QSplitter;
    operatorFrameLayout->addWidget(operatorFrameColumnSplitter);

    auto displayFrameColumnSplitter = new QSplitter;
    displayFrameLayout->addWidget(displayFrameColumnSplitter);

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

    sync_splitters(operatorFrameColumnSplitter, displayFrameColumnSplitter);

    // FIXME: create views for all events!
    // FIXME: only create views for existing events!
    if (m_d->context->getEventConfigs().size() > 0)
        m_d->createView(0);

    auto onItemClicked = [](TreeNode *node, int column)
    {
        qDebug() << "AnalysisWidget item clicked:" << node;
        qDebug() << getQObject(node);
    };

    for (int levelIndex = 0;
         levelIndex < m_d->levelTrees.size();
         ++levelIndex)
    {
        auto opTree   = m_d->levelTrees[levelIndex].operatorTree;
        auto dispTree = m_d->levelTrees[levelIndex].displayTree;
        int minTreeWidth = 200;
        opTree->setMinimumWidth(minTreeWidth);
        dispTree->setMinimumWidth(minTreeWidth);
        opTree->setContextMenuPolicy(Qt::CustomContextMenu);
        dispTree->setContextMenuPolicy(Qt::CustomContextMenu);

        operatorFrameColumnSplitter->addWidget(opTree);
        displayFrameColumnSplitter->addWidget(dispTree);

        connect(opTree, &QTreeWidget::itemClicked, this, onItemClicked);
        connect(opTree, &QWidget::customContextMenuRequested, this, [this, opTree] (QPoint pos) {
            m_d->doOperatorTreeContextMenu(opTree, pos);
        });

        connect(dispTree, &QTreeWidget::itemClicked, this, onItemClicked);
        connect(dispTree, &QWidget::customContextMenuRequested, this, [this, dispTree] (QPoint pos) {
            m_d->doDisplayTreeContextMenu(dispTree, pos);
        });
    }
#endif
}

AnalysisWidget::~AnalysisWidget()
{
#if 1 // PIPE CHANGES
    delete m_d;
#endif
}

}
