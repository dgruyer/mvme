#include "expression_operator_dialog.h"
#include "expression_operator_dialog_p.h"

#include "a2_adapter.h"
#include "analysis_ui_p.h"

#include <QSplitter>
#include <QTabWidget>

/* NOTES:
 - new slot grid implementation with space for the input variable name column.
   2nd step: try to abstract this so the slot grid can be instantiated and customized and is reusable

 - workflow:
   select inputs, write init script, run init script, check output definition is as desired,
   write step script, test with sample data, accept changes

 - required:
   clickable error display for scripts. errors can come from expr_tk (wrapped
   in a2_exprtk layer) and from the ExpressionOperator itself (e.g. malformed beginExpr output, SemanticError).

 - utility: symbol table inspection

 */

namespace analysis
{

//
// InputSelectButton
//

InputSelectButton::InputSelectButton(Slot *destSlot, s32 userLevel,
                                     EventWidget *eventWidget, QWidget *parent)
    : QPushButton(QSL("<select>"), parent)
    , m_eventWidget(eventWidget)
    , m_destSlot(destSlot)
{
    setCheckable(true);
    setMouseTracking(true);
    installEventFilter(this);
}

bool InputSelectButton::eventFilter(QObject *watched, QEvent *event)
{
    assert(watched == this);

    if (!isChecked() && (event->type() == QEvent::Enter || event->type() == QEvent::Leave))
    {
        m_eventWidget->highlightInputOf(m_destSlot, event->type() == QEvent::Enter);
    }

    return false; // Do not filter the event out.
}

//
// ExpressionOperatorPipeView
//
ExpressionOperatorPipeView::ExpressionOperatorPipeView(QWidget *parent)
    : QWidget(parent)
    , m_tableWidget(new QTableWidget(this))
    , m_a1Pipe(nullptr)
    , m_pipeAdapterStorage(Kilobytes(4))
{
    auto layout = new QVBoxLayout(this);
    s32 row = 0;
    s32 nCols = 1;

    layout->addWidget(m_tableWidget, row++, 0);

    // columns:
    // Valid, Value, lower Limit, upper Limit
    m_tableWidget->setColumnCount(4);
    m_tableWidget->setHorizontalHeaderLabels({"Valid", "Value", "Lower Limit", "Upper Limit"});

    refresh();
}

void ExpressionOperatorPipeView::setPipe(Pipe *a1_pipe)
{
    m_a1Pipe = a1_pipe;
    refresh();
}

void ExpressionOperatorPipeView::setPipe(const a2::PipeVectors &a2_pipe)
{
    m_a1Pipe = nullptr;
    m_a2Pipe = a2_pipe;
    refresh();
}

void ExpressionOperatorPipeView::refresh()
{
    if (m_a1Pipe)
    {
        /* Create an a2 pipe inside the local storage and copy the values
         * from the a1 side pipe over. */
        m_pipeAdapterStorage.reset();
        make_a2_pipe_from_a1_pipe(&m_pipeAdapterStorage, m_a1Pipe);
    }

    const auto &pipe = m_a2Pipe;

    if (pipe.data.size < 0)
    {
        m_tableWidget->setRowCount(0);
        return;
    }

    m_tableWidget->setRowCount(pipe.data.size);

    for (s32 pi = 0; pi < pipe.data.size; pi++)
    {
        double param = pipe.data[pi];
        double lowerLimit = pipe.lowerLimits[pi];
        double upperLimit = pipe.upperLimits[pi];

        QStringList columns =
        {
            a2::is_param_valid(param) ? QSL("Y") : QSL("N"),
            a2::is_param_valid(param) ? QString::number(param) : QSL(""),
            QString::number(lowerLimit),
            QString::number(upperLimit),
        };

        for (s32 ci = 0; ci < columns.size(); ci++)
        {
            auto item = m_tableWidget->item(pi, ci);
            if (!item)
            {
                item = new QTableWidgetItem;
                m_tableWidget->setItem(pi, ci, item);
            }

            item->setText(columns[ci]);
            item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        }

        if (!m_tableWidget->verticalHeaderItem(pi))
        {
            m_tableWidget->setVerticalHeaderItem(pi, new QTableWidgetItem);
        }

        m_tableWidget->verticalHeaderItem(pi)->setText(QString::number(pi));
    }

    m_tableWidget->resizeColumnsToContents();
    m_tableWidget->resizeRowsToContents();
}

//
// ExpressionOperatorPipesToolBox
//
ExpressionOperatorPipesToolBox::ExpressionOperatorPipesToolBox(QWidget *parent)
    : QToolBox(parent)
{
}

int ExpressionOperatorPipesToolBox::addPipe(Pipe *a1_pipe,
                                             const QString &title)
{
    auto widget = new ExpressionOperatorPipeView(this);
    widget->setPipe(a1_pipe);

    return this->addItem(widget, title.isEmpty() ? QSL("Pipe #%1").arg(this->count()) : title);
}

int ExpressionOperatorPipesToolBox::addPipe(const a2::PipeVectors &a2_pipe,
                                             const QString &title)
{
    auto widget = new ExpressionOperatorPipeView(this);
    widget->setPipe(a2_pipe);

    return this->addItem(widget, title.isEmpty() ? QSL("Pipe #%1").arg(this->count()) : title);
}

int ExpressionOperatorPipesToolBox::addEmptyPipe(const QString &title)
{
    return addPipe(a2::PipeVectors{}, title);
}

void ExpressionOperatorPipesToolBox::popPipe()
{
    if (this->count() > 0)
    {
        delete this->widget(this->count() - 1);
        this->removeItem(this->count() - 1);
    }
}

//
// ExpressionErrorWidget
//
ExpressionErrorWidget::ExpressionErrorWidget(QWidget *parent)
    : QWidget(parent)
    , m_errorTable(new QTableWidget)
{
    auto layout = new QHBoxLayout(this);
    layout->addWidget(m_errorTable);
    layout->setContentsMargins(0, 0, 0, 0);
}

//
// ExpressionTextEditor
//
int calculate_tabstop_width(const QFont &font, int tabstop)
{
    QString spaces;
    for (int i = 0; i < tabstop; ++i) spaces += " ";
    QFontMetrics metrics(font);
    return metrics.width(spaces);
}

static const int TabStop = 4;

ExpressionTextEditor::ExpressionTextEditor(QWidget *parent)
    : QWidget(parent)
    , m_textEdit(new QPlainTextEdit)
{
    auto font = make_monospace_font();
    m_textEdit->setFont(font);
    m_textEdit->setTabStopWidth(calculate_tabstop_width(font, TabStop));

    auto widgetLayout = new QHBoxLayout(this);
    widgetLayout->addWidget(m_textEdit);
    widgetLayout->setContentsMargins(0, 0, 0, 0);
}

//
// ExpressionEditorWidget
//
ExpressionEditorWidget::ExpressionEditorWidget(QWidget *parent)
    : QWidget(parent)
    , m_exprEdit(new ExpressionTextEditor)
    , m_exprErrors(new ExpressionErrorWidget)
{
    auto splitter = new QSplitter(Qt::Vertical);
    splitter->setHandleWidth(0);
    splitter->addWidget(m_exprEdit);
    splitter->addWidget(m_exprErrors);
    splitter->setStretchFactor(0, 80);
    splitter->setStretchFactor(0, 20);

    auto widgetLayout = new QHBoxLayout(this);;
    widgetLayout->setContentsMargins(0, 0, 0, 0);
    widgetLayout->addWidget(splitter);
}

void ExpressionEditorWidget::setText(const QString &text)
{
    m_exprEdit->textEdit()->setPlainText(text);

}

QString ExpressionEditorWidget::text() const
{
    return m_exprEdit->textEdit()->toPlainText();
}

//
// ExpressionOperatorEditorComponent
//
ExpressionOperatorEditorComponent::ExpressionOperatorEditorComponent(QWidget *parent)
    : QWidget(parent)
    , m_toolBox_inputPipes(new ExpressionOperatorPipesToolBox)
    , m_toolBox_outputPipes(new ExpressionOperatorPipesToolBox)
    , m_editorWidget(new ExpressionEditorWidget)
    , m_evalButton(new QPushButton(QSL("&Eval")))
{
    auto buttonLayout = new QHBoxLayout;
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->addWidget(make_spacer_widget());
    buttonLayout->addWidget(m_evalButton);
    buttonLayout->addWidget(make_spacer_widget());

    auto editorFrame = new QFrame(this);
    auto editorFrameLayout = new QVBoxLayout(editorFrame);
    editorFrameLayout->setContentsMargins(0, 0, 0, 0);
    editorFrameLayout->addWidget(m_editorWidget);
    editorFrameLayout->addLayout(buttonLayout);

    auto splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(m_toolBox_inputPipes);
    splitter->addWidget(editorFrame);
    splitter->addWidget(m_toolBox_outputPipes);
    splitter->setStretchFactor(0, 20);
    splitter->setStretchFactor(1, 60);
    splitter->setStretchFactor(2, 20);

    auto widgetLayout = new QHBoxLayout(this);
    widgetLayout->addWidget(splitter);
}

void ExpressionOperatorEditorComponent::setExpressionText(const QString &text)
{
    m_editorWidget->setText(text);
}

QString ExpressionOperatorEditorComponent::expressionText() const
{
    return m_editorWidget->text();
}

//
// ExpressionOperatorDialog and Private implementation
//

namespace
{
    struct Model;
} // end anon namespace

struct ExpressionOperatorDialog::Private
{
    friend struct Model;

    Private(ExpressionOperatorDialog *q)
        : m_q(q)
    {}

    ExpressionOperatorDialog *m_q;
    std::shared_ptr<ExpressionOperator> m_op;
    int m_userLevel;
    OperatorEditorMode m_mode;
    EventWidget *m_eventWidget;
    std::unique_ptr<Model> m_model;

    QTabWidget *m_tabWidget;

    // tab0: operator name and input select
    QLineEdit *le_operatorName;
    SlotGrid m_slotGrid;

    // tab1: begin expression
    ExpressionOperatorEditorComponent *m_beginExpressionEditor;

    // tab2: step expression
    ExpressionOperatorEditorComponent *m_stepExpressionEditor;

    QDialogButtonBox *m_buttonBox;

    void repopulateSlotGrid();
    void repopulateUI();

    void loadFromOperator();

    void onInputSelected(Slot *destSlot, s32 slotIndex,
                         Pipe *sourcePipe, s32 sourceParamIndex);

    void onInputCleared(s32 slotIndex);
};

namespace
{

/* Notes about the Model structure:
 *
 * This is used to hold the current state of the ExpressionOperator UI. The GUI
 * can be populated using this information and both the a1 and a2 versions of
 * the ExpressionOperator can be created from the information in the model.
 *
 * User interactions with the gui will trigger code that updates the model.
 *
 * When the user wants to evaluate one of the expressions
 * a2::make_expression_operator() is used to create the operator. Errors can be
 * displayed in the respective ExpressionOperatorEditorComponent.
 *
 * This code is not fast. Memory allocations from std::vector happen
 * frequently!
 *
 * Remember: the operator can not be built if any of the inputs is unconnected.
 * The resulting data will be a vector with length 0 which can not be
 * registered in an exprtk symbol table.
 */

struct PipeVectorStorage
{
    std::vector<double> data;
    std::vector<double> lowerLimits;
    std::vector<double> upperLimits;
};

struct A2PipeWithStorage
{
    A2PipeWithStorage()
        : a2_pipe{}
        , storage{}
    {}

    A2PipeWithStorage(const A2PipeWithStorage &) = delete;
    A2PipeWithStorage &operator=(const A2PipeWithStorage &) = delete;

    A2PipeWithStorage(A2PipeWithStorage &&) = default;
    A2PipeWithStorage &operator=(A2PipeWithStorage &&) = default;

    a2::PipeVectors a2_pipe;
    PipeVectorStorage storage;
};

void assert_consistency(const A2PipeWithStorage &pipeWithStorage)
{
    const auto &a2_pipe = pipeWithStorage.a2_pipe;
    const auto &storage = pipeWithStorage.storage;

    qDebug() << __PRETTY_FUNCTION__ << a2_pipe.data.size;

    assert(a2_pipe.data.size == a2_pipe.lowerLimits.size);
    assert(a2_pipe.data.size == a2_pipe.upperLimits.size);

    const s32 expected_size = static_cast<s32>(storage.data.size());

    assert(a2_pipe.data.size == expected_size);
    assert(a2_pipe.lowerLimits.size == expected_size);
    assert(a2_pipe.upperLimits.size == expected_size);

    assert(a2_pipe.data.data == storage.data.data());
    assert(a2_pipe.lowerLimits.data == storage.lowerLimits.data());
    assert(a2_pipe.upperLimits.data == storage.upperLimits.data());
}

// FIXME: can't make pipe const because getParameter is not const
A2PipeWithStorage make_a2_pipe_with_storage(Pipe *pipe)
{
    A2PipeWithStorage result;

    s32 size = pipe->getSize();

    auto &storage = result.storage;
    storage.data.resize(size);
    storage.lowerLimits.resize(size);
    storage.upperLimits.resize(size);

    for (s32 pi = 0; pi < size; pi++)
    {
        const auto &a1_param = pipe->getParameter(pi);

        storage.data[pi]        = a1_param->value;
        storage.lowerLimits[pi] = a1_param->lowerLimit;
        storage.upperLimits[pi] = a1_param->upperLimit;
    }

    result.a2_pipe =
    {
        { storage.data.data(), size },
        { storage.lowerLimits.data(), size },
        { storage.upperLimits.data(), size }
    };

    assert_consistency(result);

    return result;
}

struct Model
{
    /* A clone of the original operator that's being edited. This is here so
     * that we have proper Slot pointers to pass to
     * EventWidget::selectInputFor() on input selection.
     *
     * Note that pipe -> slot connections are not made on this clone as the
     * source pipes would be modified by that operation. Once the infrastructure
     * allows making the connections without having to worry about leaving the
     * analysis in an inconsistent state in any scenario, the clone could be
     * used for pipe connections. */
    std::unique_ptr<ExpressionOperator> opClone;

    /* Pointers to the original input pipes are stored here so that the
     * analysis::ExpressionOperator can be modified properly once the user
     * accepts the changes. */
    std::vector<Pipe *> a1_inputPipes;
    std::vector<A2PipeWithStorage> inputs;
    std::vector<s32> inputIndexes;
    std::vector<std::string> inputPrefixes;
    std::vector<std::string> inputUnits;
    std::string beginExpression;
    std::string stepExpression;

    Model() = default;

    Model(const Model &) = delete;
    Model &operator=(const Model &) = delete;

    Model(Model &&) = default;
    Model &operator=(Model &&) = default;
};

void assert_internal_consistency(const Model &model)
{
    assert(model.inputs.size() == model.inputIndexes.size());
    assert(model.inputs.size() == model.inputPrefixes.size());
    assert(model.inputs.size() == model.inputUnits.size());
    assert(model.inputs.size() == model.a1_inputPipes.size());

    for (size_t ii = 0; ii < model.inputs.size(); ii++)
    {
        assert_consistency(model.inputs[ii]);
    }
}

void assert_consistency(const Model &model)
{
    assert(model.opClone);
    assert(model.opClone->getNumberOfSlots() > 0);
    assert(static_cast<size_t>(model.opClone->getNumberOfSlots()) == model.inputs.size());

    assert_internal_consistency(model);
}

/* IMPORTANT: This will potentially leave the model in an inconsistent state as
 * no slot will be added to model.opClone! */
void add_model_only_input(Model &model)
{
    model.inputs.push_back({});
    model.inputIndexes.push_back(a2::NoParamIndex);
    model.a1_inputPipes.push_back(nullptr);
    model.inputPrefixes.push_back({});
    model.inputUnits.push_back({});
}

void add_new_input_slot(Model &model)
{
    assert_consistency(model);

    s32 si = model.opClone->getNumberOfSlots();
    model.opClone->addSlot();
    add_model_only_input(model);
    qDebug() << model.opClone->getInputPrefix(si);
    model.inputPrefixes[si] = model.opClone->getInputPrefix(si).toStdString();

    assert_consistency(model);
}

void pop_input_slot(Model &model)
{
    assert_consistency(model);

    if (model.opClone->removeLastSlot())
    {
        model.inputs.pop_back();
        model.inputIndexes.pop_back();
        model.a1_inputPipes.pop_back();
        model.inputPrefixes.pop_back();
        model.inputUnits.pop_back();
    }

    assert_consistency(model);
}

void connect_input(Model &model, s32 inputIndex, Pipe *inPipe, s32 paramIndex)
{
    assert_consistency(model);

    assert(0 <= inputIndex && inputIndex < model.opClone->getNumberOfSlots());
    assert(inPipe);

    model.inputs[inputIndex] = make_a2_pipe_with_storage(inPipe);
    model.inputIndexes[inputIndex] = paramIndex;
    model.a1_inputPipes[inputIndex] = inPipe;
    model.inputPrefixes[inputIndex] = model.opClone->getInputPrefix(inputIndex).toStdString();
    model.inputUnits[inputIndex] = inPipe->getParameters().unit.toStdString();

    assert_consistency(model);
}

void disconnect_input(Model &model, s32 inputIndex)
{
    assert_consistency(model);

    assert(0 <= inputIndex && inputIndex < model.opClone->getNumberOfSlots());

    model.inputs[inputIndex] = {};
    model.inputIndexes[inputIndex] = a2::NoParamIndex;
    model.a1_inputPipes[inputIndex] = nullptr;
    model.inputUnits[inputIndex] = {};
    // Note: Keeps input prefix intact.

    assert_consistency(model);
}

void load_from_operator(Model &model, ExpressionOperator &op)
{
    model.inputs.clear();
    model.inputIndexes.clear();
    model.inputPrefixes.clear();
    model.inputUnits.clear();

    model.opClone = std::unique_ptr<ExpressionOperator>(op.cloneViaSerialization());

    assert(op.getNumberOfSlots() == model.opClone->getNumberOfSlots());

    for (s32 si = 0; si < op.getNumberOfSlots(); si++)
    {
        Slot *slot = op.getSlot(si);

        add_model_only_input(model);
        model.inputPrefixes[si] = model.opClone->getInputPrefix(si).toStdString();

        if (slot && slot->isConnected() && slot->isArrayConnection())
        {
            connect_input(model, si, slot->inputPipe, slot->paramIndex);
        }
    }

    assert_consistency(model);
}

} // end anon namespace

//
// SlotGrid
//

SlotGrid make_slotgrid(QWidget *parent = nullptr)
{
    SlotGrid sg = {};

    sg.slotFrame  = new QFrame;
    sg.slotLayout = new QGridLayout(sg.slotFrame);

    sg.slotLayout->setContentsMargins(2, 2, 2, 2);
    sg.slotLayout->setColumnStretch(0, 0); // index
    sg.slotLayout->setColumnStretch(1, 1); // select button with input name
    sg.slotLayout->setColumnStretch(2, 1); // variable name inside the script
    sg.slotLayout->setColumnStretch(3, 0); // clear selection button

    sg.addSlotButton = new QPushButton(QIcon(QSL(":/list_add.png")), QString());
    sg.addSlotButton->setToolTip(QSL("Add input"));

    sg.removeSlotButton = new QPushButton(QIcon(QSL(":/list_remove.png")), QString());
    sg.removeSlotButton->setToolTip(QSL("Remove last input"));

    auto addRemoveSlotButtonsLayout = new QHBoxLayout;
    addRemoveSlotButtonsLayout->setContentsMargins(2, 2, 2, 2);
    addRemoveSlotButtonsLayout->addStretch();
    addRemoveSlotButtonsLayout->addWidget(sg.addSlotButton);
    addRemoveSlotButtonsLayout->addWidget(sg.removeSlotButton);

    sg.outerFrame = new QFrame(parent);
    auto outerLayout = new QVBoxLayout(sg.outerFrame);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->addWidget(sg.slotFrame);
    outerLayout->addLayout(addRemoveSlotButtonsLayout);
    outerLayout->setStretch(0, 1);

    return sg;
}

void slotgrid_endInputSelect(SlotGrid *sg)
{
    for (auto &selectButton: sg->selectButtons)
    {
        selectButton->setChecked(false);
    }
}

void repopulate_slotgrid(SlotGrid *sg, Model &model, EventWidget *eventWidget, s32 userLevel)
{
    assert_consistency(model);

    // Clear the slot grid and the select buttons
    while (QLayoutItem *child = sg->slotLayout->takeAt(0))
    {
        if (auto widget = child->widget())
            delete widget;
        delete child;
    }

    Q_ASSERT(sg->slotLayout->count() == 0);

    // These have been deleted by the layout clearing code above.
    sg->selectButtons.clear();
    sg->clearButtons.clear();
    sg->varNameLineEdits.clear();

    // Repopulate

    const auto &op = model.opClone;
    assert(op);

    const s32 slotCount = op->getNumberOfSlots();
    assert(slotCount > 0);

    s32 row = 0;
    s32 col = 0;

    sg->slotLayout->addWidget(new QLabel(QSL("Input#")), row, col++);
    sg->slotLayout->addWidget(new QLabel(QSL("Select")), row, col++);
    sg->slotLayout->addWidget(new QLabel(QSL("Clear")), row, col++);
    sg->slotLayout->addWidget(new QLabel(QSL("Variable Name")), row, col++);

    row++;

    for (s32 slotIndex = 0; slotIndex < slotCount; slotIndex++)
    {
        Slot *slot = op->getSlot(slotIndex);

        auto selectButton = new InputSelectButton(slot, userLevel, eventWidget);
        sg->selectButtons.push_back(selectButton);

        auto clearButton = new QPushButton(QIcon(":/dialog-close.png"), QString());
        sg->clearButtons.push_back(clearButton);

        auto le_varName  = new QLineEdit;
        sg->varNameLineEdits.push_back(le_varName);

        if (model.a1_inputPipes[slotIndex])
        {
            QString sourceText = model.a1_inputPipes[slotIndex]->source->objectName();

            if (model.inputIndexes[slotIndex] != a2::NoParamIndex)
            {
                sourceText = (QSL("%1[%2]")
                              .arg(sourceText)
                              .arg(model.inputIndexes[slotIndex]));
            }

            selectButton->setText(sourceText);
        }

        le_varName->setText(QString::fromStdString(model.inputPrefixes[slotIndex]));

        QObject::connect(selectButton, &QPushButton::toggled,
                sg->outerFrame, [=] (bool checked) {

            // Cancel any previous input selection. Has no effect if no input
            // selection was active.
            eventWidget->endSelectInput();

            if (checked)
            {
                emit selectButton->beginInputSelect();

                eventWidget->selectInputFor(
                    slot, userLevel,
                    [selectButton, slotIndex] (Slot *destSlot,
                                               Pipe *sourcePipe, s32 sourceParamIndex) {
                    // This is the callback that will be invoked by the
                    // eventwidget when input selection is complete.

                    selectButton->setChecked(false);
                    emit selectButton->inputSelected(destSlot, slotIndex,
                                                     sourcePipe, sourceParamIndex);
                });

                // Uncheck the other buttons.
                for (s32 bi = 0; bi < sg->selectButtons.size(); bi++)
                {
                    if (bi != slotIndex)
                        sg->selectButtons[bi]->setChecked(false);
                }
            }
        });

        QObject::connect(clearButton, &QPushButton::clicked,
                         sg->outerFrame, [selectButton, eventWidget, sg]() {
            selectButton->setText(QSL("<select>"));
            selectButton->setChecked(false);
            slotgrid_endInputSelect(sg);
            eventWidget->endSelectInput();
        });

        col = 0;

        sg->slotLayout->addWidget(new QLabel(slot->name), row, col++);
        sg->slotLayout->addWidget(selectButton, row, col++);
        sg->slotLayout->addWidget(clearButton, row, col++);
        sg->slotLayout->addWidget(le_varName, row, col++);

        row++;
    }

    sg->slotLayout->setRowStretch(row, 1);

    sg->slotLayout->setColumnStretch(0, 0);
    sg->slotLayout->setColumnStretch(1, 1);
    sg->slotLayout->setColumnStretch(2, 0);
    sg->slotLayout->setColumnStretch(3, 1);

    sg->removeSlotButton->setEnabled(slotCount > 1);
}

void ExpressionOperatorDialog::Private::repopulateSlotGrid()
{
    repopulate_slotgrid(&m_slotGrid, *m_model, m_eventWidget, m_userLevel);

    for (auto &selectButton: m_slotGrid.selectButtons)
    {
        QObject::connect(selectButton, &InputSelectButton::inputSelected,
                         m_q, [this] (Slot *destSlot, s32 slotIndex,
                                      Pipe *sourcePipe, s32 sourceParamIndex) {
            this->onInputSelected(destSlot, slotIndex, sourcePipe, sourceParamIndex);
        });
    }

    for (s32 bi = 0; bi < m_slotGrid.clearButtons.size(); ++bi)
    {
        auto &clearButton = m_slotGrid.clearButtons[bi];

        QObject::connect(clearButton, &QPushButton::clicked,
                         m_q, [this, bi] () {
            this->onInputCleared(bi);
        });
    }
}

void ExpressionOperatorDialog::Private::repopulateUI()
{
    repopulateSlotGrid();

    m_beginExpressionEditor->setExpressionText(
        QString::fromStdString(m_model->beginExpression));

    m_stepExpressionEditor->setExpressionText(
        QString::fromStdString(m_model->stepExpression));
}

void ExpressionOperatorDialog::Private::loadFromOperator()
{
    load_from_operator(*m_model, *m_op);
    repopulateUI();
}

void ExpressionOperatorDialog::Private::onInputSelected(
    Slot *destSlot, s32 slotIndex,
    Pipe *sourcePipe, s32 sourceParamIndex)
{
    qDebug() << __PRETTY_FUNCTION__ << destSlot << slotIndex << sourcePipe << sourceParamIndex;
    connect_input(*m_model, slotIndex, sourcePipe, sourceParamIndex);
    repopulateUI();
}

void ExpressionOperatorDialog::Private::onInputCleared(s32 slotIndex)
{
    qDebug() << __PRETTY_FUNCTION__ << slotIndex;
    disconnect_input(*m_model, slotIndex);
}

ExpressionOperatorDialog::ExpressionOperatorDialog(
    const std::shared_ptr<ExpressionOperator> &op,
    int userLevel, OperatorEditorMode mode, EventWidget *eventWidget)

    : QDialog(eventWidget)
    , m_d(std::make_unique<Private>(this))
{
    m_d->m_op          = op;
    m_d->m_userLevel   = userLevel;
    m_d->m_mode        = mode;
    m_d->m_eventWidget = eventWidget;
    m_d->m_tabWidget   = new QTabWidget;
    m_d->m_model       = std::make_unique<Model>();

    // tab0: operator name and input select
    {
        m_d->le_operatorName = new QLineEdit;

        m_d->m_slotGrid = make_slotgrid(this);
        auto gb_slotGrid = new QGroupBox(QSL("Inputs"), this);
        auto gb_slotGridLayout = new QHBoxLayout(gb_slotGrid);
        gb_slotGridLayout->setContentsMargins(2, 2, 2, 2);
        gb_slotGridLayout->addWidget(m_d->m_slotGrid.outerFrame);

        auto page = new QWidget(this);
        auto l    = new QFormLayout(page);

        l->addRow(QSL("Operator Name"), m_d->le_operatorName);
        l->addRow(gb_slotGrid);

        m_d->m_tabWidget->addTab(page, QSL("&Inputs && Name"));
    }

    // tab1: begin expression
    {
        m_d->m_beginExpressionEditor = new ExpressionOperatorEditorComponent;

        auto page = new QWidget(this);
        auto l    = new QHBoxLayout(page);

        l->addWidget(m_d->m_beginExpressionEditor);

        m_d->m_tabWidget->addTab(page, QSL("&Begin Expression"));
    }

    // tab2: step expression
    {
        m_d->m_stepExpressionEditor = new ExpressionOperatorEditorComponent;

        auto page = new QWidget(this);
        auto l    = new QHBoxLayout(page);

        l->addWidget(m_d->m_stepExpressionEditor);

        m_d->m_tabWidget->addTab(page, QSL("&Step Expression"));
    }

    // buttonbox: ok/cancel
    m_d->m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_d->m_buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    m_d->m_buttonBox->button(QDialogButtonBox::Ok)->setDefault(true);

    connect(m_d->m_buttonBox, &QDialogButtonBox::accepted, this, &ExpressionOperatorDialog::accept);
    connect(m_d->m_buttonBox, &QDialogButtonBox::rejected, this, &ExpressionOperatorDialog::reject);

    // main layout
    auto dialogLayout = new QVBoxLayout(this);
    dialogLayout->addWidget(m_d->m_tabWidget);
    dialogLayout->addWidget(m_d->m_buttonBox);
    dialogLayout->setContentsMargins(2, 2, 2, 2);
    dialogLayout->setStretch(0, 1);

    // Slotgrid interactions
    connect(m_d->m_slotGrid.addSlotButton, &QPushButton::clicked, this, [this]() {
        add_new_input_slot(*m_d->m_model);
        m_d->repopulateSlotGrid();
        m_d->m_slotGrid.removeSlotButton->setEnabled(m_d->m_model->opClone->getNumberOfSlots() > 1);
        m_d->m_eventWidget->endSelectInput();
    });

    connect(m_d->m_slotGrid.removeSlotButton, &QPushButton::clicked, this, [this] () {
        if (m_d->m_model->opClone->getNumberOfSlots() > 1)
        {
            pop_input_slot(*m_d->m_model);
            m_d->repopulateSlotGrid();
            m_d->m_eventWidget->endSelectInput();
        }
        m_d->m_slotGrid.removeSlotButton->setEnabled(
            m_d->m_model->opClone->getNumberOfSlots() > 1);
    });

    // Initialize and misc setup
    switch (m_d->m_mode)
    {
        case OperatorEditorMode::New:
            {
                setWindowTitle(QString("New  %1").arg(m_d->m_op->getDisplayName()));
            } break;
        case OperatorEditorMode::Edit:
            {
                setWindowTitle(QString("Edit %1").arg(m_d->m_op->getDisplayName()));
            } break;
    }

    add_widget_close_action(this);
    resize(800, 600);

    m_d->loadFromOperator();
}

ExpressionOperatorDialog::~ExpressionOperatorDialog()
{
}

void ExpressionOperatorDialog::accept()
{
    QDialog::accept();
}

void ExpressionOperatorDialog::reject()
{
    QDialog::reject();
}

} // end namespace analysis
