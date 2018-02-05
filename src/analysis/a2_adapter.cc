#include "a2_adapter.h"
#include "a2/a2_impl.h"
#include "analysis.h"
#include <algorithm>
#include <cstdio>
#include <QMetaObject>
#include <QMetaClassInfo>

//#ifndef NDEBUG
#if 0
#define LOG(fmt, ...)\
do\
{\
    fprintf(stderr, "a2_adapter::%s() " fmt, __FUNCTION__, ##__VA_ARGS__);\
    fprintf(stderr, "\n");\
} while (0)

#define LOGNN(fmt, ...)\
do\
{\
    fprintf(stderr, "a2_adapter::%s() " fmt, __FUNCTION__, ##__VA_ARGS__);\
} while (0)

#else

#define LOG(...)
#define LOGNN(...)

#endif

namespace
{

//#ifndef NDEBUG
#if 0
inline QDebug a2_adapter_qlog(const char *func)
{
    return (qDebug().nospace() << "a2_adapter::" << func << "()").space();
}

#define QLOG(x) a2_adapter_qlog(__FUNCTION__) << x
#else
#define QLOG(x)
#endif

using analysis::A2AdapterState;

inline a2::PipeVectors find_output_pipe(
    const A2AdapterState *state,
    const analysis::PipeSourceInterface *pipeSource,
    u8 outputIndex)
{
    //QLOG("pipeSource to find:" << pipeSource.get() << ", outputIndex=" << (u32)outputIndex);

    a2::PipeVectors result = {};

    if (a2::DataSource *ds_a2 = state->sourceMap.value(
            qobject_cast<analysis::SourceInterface *>(pipeSource),
            nullptr))
    {
        assert(outputIndex == 0);
        result = ds_a2->output;
    }
    else if (a2::Operator *op_a2 = state->operatorMap.value(
            qobject_cast<analysis::OperatorInterface *>(pipeSource),
            nullptr))
    {
        assert(outputIndex < op_a2->outputCount);

        result.data = op_a2->outputs[outputIndex];
        result.lowerLimits = op_a2->outputLowerLimits[outputIndex];
        result.upperLimits = op_a2->outputUpperLimits[outputIndex];
    }
    else
    {
        QLOG(pipeSource << pipeSource->getId());
        assert(!"no source mapping");
    }

    return result;
}

inline a2::PipeVectors find_output_pipe(const A2AdapterState *state, analysis::Slot *slot)
{
    return find_output_pipe(
        state,
        slot->inputPipe->source,
        slot->inputPipe->sourceOutputIndex);
}

#define assert_slot(slot)\
do\
{\
    assert(slot->inputPipe);\
    assert(slot->inputPipe->source);\
    assert(slot->inputPipe->source->getSharedPointer());\
} while (0)

using InputSlots = QVector<analysis::Slot *>;
using OutputPipes = QVector<analysis::Pipe *>;

#define DEF_OP_MAGIC(name) a2::Operator name(\
    memory::Arena *arena,\
    A2AdapterState *adapterState,\
    analysis::OperatorPtr op,\
    InputSlots inputSlots,\
    OutputPipes outputPipes)

typedef DEF_OP_MAGIC(OperatorMagic);

DEF_OP_MAGIC(calibration_magic)
{
    LOG("");
    assert(inputSlots.size() == 1);
    assert_slot(inputSlots[0]);

    auto calib = qobject_cast<analysis::CalibrationMinMax *>(op.get());

    assert(calib);

    auto a2_input = find_output_pipe(adapterState, inputSlots[0]);

    auto calibs = calib->getCalibrations();

    QVector<double> calibMinimums(calibs.size());
    QVector<double> calibMaximums(calibs.size());

    for (s32 i = 0; i < calibs.size(); i++)
    {
        calibMinimums[i] = calibs[i].unitMin;
        calibMaximums[i] = calibs[i].unitMax;
    }

    a2::Operator result = a2::make_calibration(
        arena,
        a2_input,
        { calibMinimums.data(), calibMinimums.size() },
        { calibMaximums.data(), calibMaximums.size() });

    return result;
};

DEF_OP_MAGIC(difference_magic)
{
    LOG("");
    assert(inputSlots.size() == 2);
    assert_slot(inputSlots[0]);
    assert_slot(inputSlots[1]);

    auto diff = qobject_cast<analysis::Difference *>(op.get());

    assert(diff);

    auto a2_inputA = find_output_pipe(adapterState, inputSlots[0]);
    auto a2_inputB = find_output_pipe(adapterState, inputSlots[1]);

    a2::Operator result = {};

    if (inputSlots[0]->acceptedInputTypes == analysis::InputType::Array)
    {
        assert(inputSlots[0]->paramIndex == analysis::Slot::NoParamIndex);
        assert(inputSlots[1]->paramIndex == analysis::Slot::NoParamIndex);

        result = make_difference(arena, a2_inputA, a2_inputB);
    }
    else
    {
        assert(inputSlots[0]->paramIndex != analysis::Slot::NoParamIndex);
        assert(inputSlots[0]->paramIndex < a2_inputA.data.size);
        assert(inputSlots[1]->paramIndex != analysis::Slot::NoParamIndex);
        assert(inputSlots[1]->paramIndex < a2_inputB.data.size);

        result = make_difference_idx(
            arena,
            a2_inputA,
            a2_inputB,
            inputSlots[0]->paramIndex,
            inputSlots[1]->paramIndex
            );
    }

    return result;
};

template<typename T, typename SizeType = size_t>
struct QVectorBlock
{
    a2::TypedBlock<T, SizeType> block;
    QVector<T> store;
};

DEF_OP_MAGIC(array_map_magic)
{
    LOG("");

    auto arrayMap = qobject_cast<analysis::ArrayMap *>(op.get());
    assert(arrayMap);

    QVector<a2::PipeVectors> a2_inputs(inputSlots.size());

    for (s32 si = 0; si < inputSlots.size(); si++)
    {
        a2_inputs[si] = find_output_pipe(adapterState, inputSlots[si]);
    }

    auto mappings = arrayMap->m_mappings;

    QVector<a2::ArrayMapData::Mapping> a2_data(mappings.size());

    for (s32 mi = 0; mi < mappings.size(); mi++)
    {
        assert(mappings[mi].slotIndex <= std::numeric_limits<u8>::max());

        a2::ArrayMapData::Mapping a2_mapping;
        a2_mapping.inputIndex = static_cast<u8>(mappings[mi].slotIndex);
        a2_mapping.paramIndex = mappings[mi].paramIndex;

        a2_data[mi] = a2_mapping;
    }


    a2::Operator result = a2::make_array_map(
        arena,
        { a2_inputs.data(), a2_inputs.size() },
        { a2_data.data(), a2_data.size() });

    return result;
}

DEF_OP_MAGIC(aggregate_ops_magic)
{
    LOG("");

    using analysis::AggregateOps;
    auto agOps = qobject_cast<AggregateOps *>(op.get());
    assert(agOps);

    a2::Thresholds thresholds =
    {
        agOps->getMinThreshold(),
        agOps->getMaxThreshold()
    };

    auto a2_input = find_output_pipe(adapterState, inputSlots[0]);

    a2::Operator result = {};

    switch (agOps->getOperation())
    {
        case AggregateOps::Op_Sum:
            result = make_aggregate_sum(arena, a2_input, thresholds);
            break;

        case AggregateOps::Op_Mean:
            result = make_aggregate_mean(arena, a2_input, thresholds);
            break;

        case AggregateOps::Op_Min:
            result = make_aggregate_min(arena, a2_input, thresholds);
            break;

        case AggregateOps::Op_Max:
            result = make_aggregate_max(arena, a2_input, thresholds);
            break;

        case AggregateOps::Op_Multiplicity:
            result = make_aggregate_multiplicity(arena, a2_input, thresholds);
            break;

        case AggregateOps::Op_Sigma:
            result = make_aggregate_sigma(arena, a2_input, thresholds);
            break;

        case AggregateOps::Op_MinX:
            result = make_aggregate_minx(arena, a2_input, thresholds);
            break;

        case AggregateOps::Op_MaxX:
            result = make_aggregate_maxx(arena, a2_input, thresholds);
            break;

        case AggregateOps::Op_MeanX:
            result = make_aggregate_meanx(arena, a2_input, thresholds);
            break;

        case AggregateOps::Op_SigmaX:
            result = make_aggregate_sigmax(arena, a2_input, thresholds);
            break;

        default:
            qDebug() << "analysis::AggregateOps::Operation =" << agOps->getOperation();
            assert(!"unsupported AggregateOps::Operation");
    }

    return result;
}

/* Maps analysis::Sum to a2::Operator_Aggregate_Sum or
 * a2::Operator_Aggregate_Mean depending on the setting of
 * analysis::Sum::m_calculateMean. The thresholds are set to NaN as the Sum
 * operator doesn't have thresholds. */
DEF_OP_MAGIC(sum_magic)
{
    LOG("");

    auto sumOp = qobject_cast<analysis::Sum *>(op.get());
    assert(sumOp);

    a2::Thresholds thresholds =
    {
        make_quiet_nan(),
        make_quiet_nan()
    };

    auto a2_input = find_output_pipe(adapterState, inputSlots[0]);

    a2::Operator result = {};

    if (sumOp->m_calculateMean)
    {
        result = make_aggregate_mean(arena, a2_input, thresholds);
    }
    else
    {
        result = make_aggregate_sum(arena, a2_input, thresholds);
    }

    return result;
}

DEF_OP_MAGIC(binary_equation_magic)
{
    LOG("");

    auto binSumDiff = qobject_cast<analysis::BinarySumDiff *>(op.get());
    assert(binSumDiff);

    auto a2_inputA = find_output_pipe(adapterState, inputSlots[0]);
    auto a2_inputB = find_output_pipe(adapterState, inputSlots[1]);

    /* Copy user set output limits from the analysis::BinarySumDiff output. */
    double outputLowerLimit = outputPipes[0]->parameters[0].lowerLimit;
    double outputUpperLimit = outputPipes[0]->parameters[0].upperLimit;

    a2::Operator result = make_binary_equation(
        arena,
        a2_inputA,
        a2_inputB,
        binSumDiff->getEquation(),
        outputLowerLimit,
        outputUpperLimit);

    return result;
}

DEF_OP_MAGIC(keep_previous_magic)
{
    LOG("");
    assert(inputSlots.size() == 1);
    assert_slot(inputSlots[0]);

    auto prevValue = qobject_cast<analysis::PreviousValue *>(op.get());

    assert(prevValue);

    auto a2_input = find_output_pipe(adapterState, inputSlots[0]);

    a2::Operator result = {};

    if (inputSlots[0]->paramIndex == analysis::Slot::NoParamIndex)
    {
        result = a2::make_keep_previous(
            arena,
            a2_input,
            prevValue->m_keepValid);
    }
    else
    {
        result = a2::make_keep_previous_idx(
            arena,
            a2_input,
            inputSlots[0]->paramIndex,
            prevValue->m_keepValid);
    }

    return result;
}

DEF_OP_MAGIC(range_filter_magic)
{
    LOG("");
    assert(inputSlots.size() == 1);
    assert_slot(inputSlots[0]);

    auto rangeFilter = qobject_cast<analysis::RangeFilter1D *>(op.get());

    assert(rangeFilter);

    auto a2_input = find_output_pipe(adapterState, inputSlots[0]);

    a2::Operator result = {};

    if (inputSlots[0]->paramIndex == analysis::Slot::NoParamIndex)
    {
        result = a2::make_range_filter(
            arena,
            a2_input,
            { rangeFilter->m_minValue, rangeFilter->m_maxValue },
            rangeFilter->m_keepOutside);
    }
    else
    {
        result = a2::make_range_filter_idx(
            arena,
            a2_input,
            inputSlots[0]->paramIndex,
            { rangeFilter->m_minValue, rangeFilter->m_maxValue },
            rangeFilter->m_keepOutside);
    }

    return result;
}

DEF_OP_MAGIC(rect_filter_magic)
{
    LOG("");
    assert(inputSlots.size() == 2);
    assert_slot(inputSlots[0]);
    assert_slot(inputSlots[1]);
    assert(inputSlots[0]->paramIndex != analysis::Slot::NoParamIndex);
    assert(inputSlots[1]->paramIndex != analysis::Slot::NoParamIndex);

    auto rectFilter = qobject_cast<analysis::RectFilter2D *>(op.get());

    assert(rectFilter);

    auto a2_xInput = find_output_pipe(adapterState, inputSlots[0]);
    auto a2_yInput = find_output_pipe(adapterState, inputSlots[1]);

    assert(inputSlots[0]->paramIndex < a2_xInput.data.size);
    assert(inputSlots[1]->paramIndex < a2_yInput.data.size);

    s32 xIndex = inputSlots[0]->paramIndex;
    s32 yIndex = inputSlots[1]->paramIndex;

    a2::Thresholds xThresholds =
    {
        rectFilter->getXInterval().minValue(),
        rectFilter->getXInterval().maxValue()
    };

    a2::Thresholds yThresholds =
    {
        rectFilter->getYInterval().minValue(),
        rectFilter->getYInterval().maxValue()
    };

    a2::RectFilterOperation filterOp = (rectFilter->getConditionOp() == analysis::RectFilter2D::OpAnd
                                    ? a2::RectFilterOperation::And
                                    : a2::RectFilterOperation::Or);

    a2::Operator result = make_rect_filter(
        arena,
        a2_xInput,
        a2_yInput,
        xIndex,
        yIndex,
        xThresholds,
        yThresholds,
        filterOp);

    return result;
}

DEF_OP_MAGIC(condition_filter_magic)
{
    LOG("");
    assert(inputSlots.size() == 2);
    assert_slot(inputSlots[0]);
    assert_slot(inputSlots[1]);

    auto condFilter = qobject_cast<analysis::ConditionFilter *>(op.get());

    assert(condFilter);

    auto a2_dataInput = find_output_pipe(adapterState, inputSlots[0]);
    auto a2_condInput = find_output_pipe(adapterState, inputSlots[1]);

    s32 dataIndex = inputSlots[0]->paramIndex;
    s32 condIndex = inputSlots[1]->paramIndex;

    a2::Operator result = make_condition_filter(
        arena,
        a2_dataInput,
        a2_condInput,
        dataIndex,
        condIndex);

    return result;
}

DEF_OP_MAGIC(histo1d_sink_magic)
{
    LOG("");
    assert(inputSlots.size() == 1);
    assert_slot(inputSlots[0]);

    auto histoSink = qobject_cast<analysis::Histo1DSink *>(op.get());

    assert(histoSink);

    auto a2_input = find_output_pipe(adapterState, inputSlots[0]);

    QVector<a2::H1D> histos(histoSink->m_histos.size());

    for (s32 i = 0; i < histos.size(); i++)
    {
        auto histo = histoSink->m_histos[i].get();

        assert(histo->getNumberOfBins() < a2::H1D::size_max);

        a2::H1D a2_histo = {};
        a2_histo.data = histo->data();
        a2_histo.size = histo->getNumberOfBins();
        a2_histo.binning.min = histo->getXMin();
        a2_histo.binning.range = histo->getXMax() - histo->getXMin();
        // binningFactor = binCount / binning.range
        a2_histo.binningFactor = a2_histo.size / a2_histo.binning.range;

        histos[i] = a2_histo;
    }

    a2::Operator result = {};

    if (inputSlots[0]->paramIndex == analysis::Slot::NoParamIndex)
    {
        result = a2::make_h1d_sink(
            arena,
            a2_input,
            { histos.data(), histos.size()});
    }
    else
    {
        result = a2::make_h1d_sink_idx(
            arena,
            a2_input,
            { histos.data(), histos.size()},
            inputSlots[0]->paramIndex);
    }

    return result;
};

DEF_OP_MAGIC(histo2d_sink_magic)
{
    LOG("");
    assert(inputSlots.size() == 2);
    assert_slot(inputSlots[0]);
    assert_slot(inputSlots[1]);

    auto histoSink = qobject_cast<analysis::Histo2DSink *>(op.get());

    assert(histoSink);

    auto a2_xInput = find_output_pipe(adapterState, inputSlots[0]);
    auto a2_yInput = find_output_pipe(adapterState, inputSlots[1]);

    assert(inputSlots[0]->paramIndex != analysis::Slot::NoParamIndex);
    assert(inputSlots[1]->paramIndex != analysis::Slot::NoParamIndex);

    assert(inputSlots[0]->paramIndex < a2_xInput.data.size);
    assert(inputSlots[1]->paramIndex < a2_yInput.data.size);

    s32 xIndex = inputSlots[0]->paramIndex;
    s32 yIndex = inputSlots[1]->paramIndex;

    auto histo = histoSink->m_histo;

    using a2::H2D;

    AxisBinning binnings[H2D::AxisCount] =
    {
        histo->getAxisBinning(Qt::XAxis),
        histo->getAxisBinning(Qt::YAxis)
    };

    assert(binnings[H2D::XAxis].getBins() * binnings[H2D::YAxis].getBins() < a2::H2D::size_max);

    a2::H2D a2_histo = {};

    a2_histo.data = histo->data();
    a2_histo.size = binnings[H2D::XAxis].getBins() * binnings[H2D::YAxis].getBins();

    for (s32 axis = 0; axis < H2D::AxisCount; axis++)
    {
        double absMin = std::min(binnings[axis].getMin(), binnings[axis].getMax());
        double absMax = std::max(binnings[axis].getMin(), binnings[axis].getMax());
        double absRange = std::abs(absMax - absMin);

        a2_histo.binCounts[axis] = binnings[axis].getBins();
        a2_histo.binnings[axis].min = absMin;
        a2_histo.binnings[axis].range = absRange;
        a2_histo.binningFactors[axis] = a2_histo.binCounts[axis] / a2_histo.binnings[axis].range;
    }

    a2::Operator result = a2::make_h2d_sink(
        arena,
        a2_xInput,
        a2_yInput,
        xIndex,
        yIndex,
        a2_histo);

    return result;
}

static const QHash<const QMetaObject *, OperatorMagic *> OperatorMagicTable =
{
    { &analysis::CalibrationMinMax::staticMetaObject, calibration_magic },
    { &analysis::Difference::staticMetaObject, difference_magic },
    { &analysis::ArrayMap::staticMetaObject, array_map_magic },
    { &analysis::AggregateOps::staticMetaObject, aggregate_ops_magic },
    { &analysis::BinarySumDiff::staticMetaObject, binary_equation_magic },
    { &analysis::PreviousValue::staticMetaObject, keep_previous_magic },
    { &analysis::RangeFilter1D::staticMetaObject, range_filter_magic },
    { &analysis::RectFilter2D::staticMetaObject, rect_filter_magic },
    { &analysis::ConditionFilter::staticMetaObject, condition_filter_magic },
    { &analysis::Sum::staticMetaObject, sum_magic },

    { &analysis::Histo1DSink::staticMetaObject, histo1d_sink_magic },
    { &analysis::Histo2DSink::staticMetaObject, histo2d_sink_magic },
};

a2::Operator a2_adapter_magic(memory::Arena *arena, A2AdapterState *state, analysis::OperatorPtr op)
{
    a2::Operator result = {};
    result.type = a2::OperatorTypeCount;

    assert(op->getNumberOfSlots() <= a2::Operator::MaxInputCount);
    assert(op->getNumberOfOutputs() <= a2::Operator::MaxOutputCount);

    InputSlots inputSlots(op->getNumberOfSlots());

    for (s32 slotIndex = 0; slotIndex < op->getNumberOfSlots(); slotIndex++)
    {
        inputSlots[slotIndex] = op->getSlot(slotIndex);
    }

    OutputPipes outputPipes(op->getNumberOfOutputs());

    for (s32 pipeIndex = 0; pipeIndex < op->getNumberOfOutputs(); pipeIndex++)
    {
        outputPipes[pipeIndex] = op->getOutput(pipeIndex);
    }

    auto operator_magic = OperatorMagicTable.value(op->metaObject(), nullptr);

    if (operator_magic)
    {
        LOG("found magic for %s", op->metaObject()->className());
        QLOG(op.get() << op->getId());
        result = operator_magic(arena, state, op, inputSlots, outputPipes);
    }
    else
    {
        LOG("EE no magic for %s :(", op->metaObject()->className());
    }

    return result;
}

} // end anon namespace

namespace analysis
{

a2::PipeVectors find_output_pipe(const A2AdapterState *state, analysis::Pipe *pipe)
{
    assert(pipe);
    assert(pipe->source);

    return ::find_output_pipe(
        state,
        pipe->source,
        pipe->sourceOutputIndex);
}

void a2_adapter_build_extractors(
    memory::Arena *arena,
    A2AdapterState *state,
    const QVector<Analysis::SourceEntry> &sourceEntries,
    const vme_analysis_common::VMEIdToIndex &vmeMap)
{
    struct SourceInfo
    {
        SourcePtr source;
        int moduleIndex;
    };

    std::array<QVector<SourceInfo>, a2::MaxVMEEvents> sourceInfos;

    for (auto se: sourceEntries)
    {
        auto index = vmeMap.value(se.moduleId);

        Q_ASSERT(index.eventIndex < a2::MaxVMEEvents);
        Q_ASSERT(index.moduleIndex < a2::MaxVMEModules);

        SourceInfo sourceInfo = { se.source, index.moduleIndex };

        sourceInfos[index.eventIndex].push_back(sourceInfo);
    }

    // Sort the source vector by moduleIndex
    for (s32 ei = 0; ei < a2::MaxVMEEvents; ei++)
    {
        std::stable_sort(sourceInfos[ei].begin(), sourceInfos[ei].end(), [](auto a, auto b) {
            return a.moduleIndex < b.moduleIndex;
        });
    }

    // Adapt the Extractors
    for (s32 ei = 0; ei < a2::MaxVMEEvents; ei++)
    {
        for (auto src: sourceInfos[ei])
        {
            qDebug()
                << "SourceInfo: eventIndex =" << ei
                << ", moduleIndex =" << src.moduleIndex
                << ", source =" << src.source.get();
        }

        Q_ASSERT(sourceInfos[ei].size() <= std::numeric_limits<u8>::max());

        // space for the DataSource pointers
        state->a2->dataSources[ei] = arena->pushArray<a2::DataSource>(sourceInfos[ei].size());

        // analysis::Extractor
        for (auto src: sourceInfos[ei])
        {
            a2::DataSource ds = {};

            if (auto ex = qobject_cast<analysis::Extractor *>(src.source.get()))
            {
                a2::data_filter::MultiWordFilter filter = {};
                for (auto slowFilter: ex->getFilter().getSubFilters())
                {
                    a2::data_filter::add_subfilter(
                        &filter,
                        a2::data_filter::make_filter(
                            slowFilter.getFilter().toStdString(),
                            slowFilter.getWordIndex()));
                }

                ds = a2::make_extractor(
                    arena,
                    filter,
                    ex->m_requiredCompletionCount,
                    ex->m_rngSeed,
                    src.moduleIndex);
            }
            else if (auto ex = qobject_cast<analysis::CombiningExtractor *>(src.source.get()))
            {
                ds = a2::make_combining_extractor(
                    arena,
                    ex->getExtractor().combiningFilter,
                    ex->getExtractor().repetitionAddressFilter,
                    ex->getExtractor().repetitions,
                    ex->getRngSeed(),
                    src.moduleIndex);
            }

            u8 &ds_cnt = state->a2->dataSourceCounts[ei];
            state->a2->dataSources[ei][ds_cnt] = ds;
            state->sourceMap.insert(src.source.get(), state->a2->dataSources[ei] + ds_cnt);
            ds_cnt++;
        }
    }
}

struct OperatorInfo
{
    OperatorPtr op;
    int rank;
    s32 a2OperatorType = -1;
};

using OperatorsByEventIndex = std::array<QVector<OperatorInfo>, a2::MaxVMEEvents>;

OperatorsByEventIndex group_operators_by_event(
    const QVector<Analysis::OperatorEntry> &operatorEntries,
    const vme_analysis_common::VMEIdToIndex &vmeMap)
{
    std::array<QVector<OperatorInfo>, a2::MaxVMEEvents> operators;

    for (auto oe: operatorEntries)
    {
        int eventIndex = vmeMap.value(oe.eventId).eventIndex;

        Q_ASSERT(eventIndex < a2::MaxVMEEvents);

        operators[eventIndex].push_back({ oe.op, oe.op->getMaximumInputRank(), -1 });
    }

    return operators;
}

/* Fills in state and operators. */
void a2_adapter_build_operators(
    memory::Arena *arena,
    A2AdapterState *state,
    OperatorsByEventIndex &operators)
{
    for (s32 ei = 0; ei < a2::MaxVMEEvents; ei++)
    {
        Q_ASSERT(operators[ei].size() <= std::numeric_limits<u8>::max());

        state->a2->operators[ei] = arena->pushArray<a2::Operator>(operators[ei].size());
        state->a2->operatorRanks[ei] = arena->pushArray<u8>(operators[ei].size());

        for (auto &opInfo: operators[ei])
        {
            auto a2_op = a2_adapter_magic(arena, state, opInfo.op);

            if (a2_op.type < a2::OperatorTypeCount)
            {
                opInfo.a2OperatorType = a2_op.type;
                u8 &op_cnt = state->a2->operatorCounts[ei];
                state->a2->operators[ei][op_cnt] = a2_op;
                state->a2->operatorRanks[ei][op_cnt] = opInfo.rank;
                state->operatorMap.insert(opInfo.op.get(), state->a2->operators[ei] + op_cnt);
                op_cnt++;
                LOG("a2_op.type=%d", (s32)(a2_op.type));
            }
        }
    }
}

using OperatorEntryVector = QVector<Analysis::OperatorEntry>;

void set_null_if_input_is(OperatorEntryVector &operators, OperatorInterface *inputOp, s32 startIndex)
{
    s32 operatorCount = operators.size();

    for (s32 i = startIndex;
         i < operatorCount;
         i++)
    {
        auto &entry = operators[i];

        if (entry.op)
        {
            for (s32 slotIndex = 0;
                 slotIndex < entry.op->getNumberOfSlots();
                 slotIndex++)
            {
                auto slot = entry.op->getSlot(slotIndex);

                if (slot->inputPipe && slot->inputPipe->source == inputOp)
                {
                    set_null_if_input_is(operators, entry.op.get(), i);

                    entry.op.reset();
                    break;
                }
            }
        }
    }
}

auto a2_adapter_filter_operators(QVector<Analysis::OperatorEntry> operators)
{
    QVector<Analysis::OperatorEntry> result;

    s32 operatorCount = operators.size();

    for (s32 opIndex = 0;
         opIndex < operatorCount;
         opIndex++)
    {
        auto &entry = operators[opIndex];

        if (entry.op && !all_inputs_connected(entry.op.get()))
        {
            QLOG("filtering out" << entry.op.get() << "and children");
            set_null_if_input_is(operators, entry.op.get(), opIndex + 1);
            entry.op.reset();
        }
    }

    for (s32 opIndex = 0;
         opIndex < operatorCount;
         opIndex++)
    {
        if (operators[opIndex].op)
        {
            result.push_back(operators[opIndex]);
        }
    }

    QLOG("filtered out" << operators.size() - result.size()
         << "of" << operators.size() << "operators");

    return result;
}

A2AdapterState a2_adapter_build(
    memory::Arena *arena,
    memory::Arena *workArena,
    const QVector<Analysis::SourceEntry> &sourceEntries,
    const QVector<Analysis::OperatorEntry> &allOperatorEntries,
    const vme_analysis_common::VMEIdToIndex &vmeMap)
{
    A2AdapterState result = {};
    result.a2 = arena->push<a2::A2>({});

    for (u32 i = 0; i < result.a2->dataSourceCounts.size(); i++)
    {
        assert(result.a2->dataSourceCounts[i] == 0);
        assert(result.a2->operatorCounts[i] == 0);
    }

    // -------------------------------------------
    // Source -> Extractor
    // -------------------------------------------

    a2_adapter_build_extractors(
        arena,
        &result,
        sourceEntries,
        vmeMap);

    LOG("data sources:");

    for (s32 ei = 0; ei < a2::MaxVMEEvents; ei++)
    {
        if (!result.a2->dataSourceCounts[ei])
            continue;
        LOG("  ei=%d, #ds=%d", ei, (u32)result.a2->dataSourceCounts[ei]);
    }

    // -------------------------------------------
    // Operator -> Operator
    // -------------------------------------------

    /* The problem: I want the operators for each event be sorted by rank _and_
     * by a2::OperatorType.
     *
     * The a2::OperatorType resulting from converting an analysis operator is
     * not known without actually doing the conversion.
     *
     * Step 1: Use the workArena to fill the A2 structure.
     * Step 2: Sort the operators by a2::OperatorType, preserving rank order.
     * Step 3: Clear the operator part of A2.
     * Step 4: Build again using the sorted operators information and the destination arena.
     */

    /* Filter out operators that are not fully connected. */
    auto operatorEntries = a2_adapter_filter_operators(allOperatorEntries);

    OperatorsByEventIndex operators = group_operators_by_event(
        operatorEntries,
        vmeMap);

    /* Build in work arena. Fills out result and operators. */
    a2_adapter_build_operators(
        workArena,
        &result,
        operators);

    LOG("operators before type sort:");

    for (s32 ei = 0; ei < a2::MaxVMEEvents; ei++)
    {
        if (!result.a2->operatorCounts[ei])
            continue;
        LOG("  ei=%d, #op=%d", ei, (u32)result.a2->operatorCounts[ei]);
    }

    /* Sort the operator arrays. */
    for (s32 ei = 0; ei < a2::MaxVMEEvents; ei++)
    {
        qSort(
            operators[ei].begin(),
            operators[ei].end(),
            [] (const OperatorInfo &oi1, const OperatorInfo &oi2) {
                if (oi1.rank == oi2.rank)
                {
                    return oi1.a2OperatorType < oi2.a2OperatorType;
                }
                return oi1.rank < oi2.rank;
            });
    }

    /* Clear the operator part. */
    result.a2->operatorCounts.fill(0);
    result.a2->operators.fill(nullptr);
    result.a2->operatorRanks.fill(nullptr);
    result.operatorMap.clear();

    /* Second build using the destination arena. */
    a2_adapter_build_operators(
        arena,
        &result,
        operators);

    LOG("operators after type sort:");

    for (s32 ei = 0; ei < a2::MaxVMEEvents; ei++)
    {
        if (!result.a2->operatorCounts[ei])
            continue;
        LOG("  ei=%d, #op=%d", ei, (u32)result.a2->operatorCounts[ei]);
    }

    LOG("mem=%lu, start@%p", arena->used(), arena->mem);



#define qcstr(str) (str.toLocal8Bit().constData())

    LOG(">>>>>>>> result <<<<<<<<");

    LOG("data sources:");

    for (s32 ei = 0; ei < a2::MaxVMEEvents; ei++)
    {
        auto srcCount = result.a2->dataSourceCounts[ei];

        if (srcCount)
        {
            LOG("  ei=%d", ei);

            auto dataSources = result.a2->dataSources[ei];

            for (a2::DataSource *ds = dataSources; ds < dataSources + srcCount; ds++)
            {
                analysis::SourceInterface *a1_src = result.sourceMap.value(ds, nullptr);

                LOG("    [%u] data source@%p, moduleIndex=%d, a1_type=%s, a1_name=%s",
                    (u32)(ds - dataSources),
                    ds,
                    (s32)ds->moduleIndex,
                    a1_src ? a1_src->metaObject()->className() : "nullptr",
                    a1_src ? qcstr(a1_ex->objectName()) : "nullptr");
            }
        }
    }

    LOG("operators:");

    for (s32 ei = 0; ei < a2::MaxVMEEvents; ei++)
    {
        auto opCount = result.a2->operatorCounts[ei];

        if (opCount)
        {
            LOG("  ei=%d", ei);

            auto operators = result.a2->operators[ei];
            auto ranks = result.a2->operatorRanks[ei];

            s32 opIndex = 0;
            for (auto op = operators; op < operators + opCount; op++, opIndex++)
            {
                s32 rank = ranks[opIndex];

                analysis::OperatorInterface *a1_op = result.operatorMap.value(op, nullptr);

                LOG("    [%d] operator@%p, rank=%d, type=%d, a1_type=%s, a1_name=%s",
                    opIndex,
                    op,
                    rank,
                    (s32)op->type,
                    a1_op ? a1_op->metaObject()->className() : "nullptr",
                    a1_op ? qcstr(a1_op->objectName()) : "nullptr");
            }
        }
    }

    LOG("<<<<<<<< end result >>>>>>>>");

    return result;
}

} // namespace analysis
