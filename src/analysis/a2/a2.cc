#include "mpmc_queue.cc"
#include "a2_impl.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include "util/perf.h"
#include "benaphore.h" // cpp11-on-multicore

#define ArrayCount(x) (sizeof(x) / sizeof(*x))

//#ifndef NDEBUG
#if 0
#define a2_trace(fmt, ...)\
do\
{\
    fprintf(stderr, "a2::%s() " fmt, __FUNCTION__, ##__VA_ARGS__);\
} while (0);
#else
#define a2_trace(...)
#endif

namespace a2
{

using namespace data_filter;
using namespace memory;

/* TODO list
 * - Add logic to force internal input/output vectors to be rounded up to a
 *   specific power of 2. This is needed to efficiently use vector instructions
 *   in the _step() loops I think.
 *
 * - Try an extractor for single word filters. Use the same system as for
 *   operators: a global function table. This means that
 *   a2_process_module_data() has to do a lookup and dispatch instead of
 *   passing directly to the extractor.
 * - Better tests. Test edge cases using nan, inf, -inf. Document the behaviour.
 * - Test and document the behaviour for invalids.
 * - Support negative axis values (works for h2d)
 * - Think about aligning operators to cache lines (in addition to the double
 *   arrays).
 * - Arena overflow strategy.
 */

/* Alignment in bytes of all double vectors created by the system.
 * SSE requires 16 byte alignment (128 bit registers).
 * AVX wants 32 bytes (256 bit registers).
 *
 * I've seen people pad the pointer in their queue implementations to 64/128
 * bytes as that's the cache line size. Padding this way makes false-sharing
 * less of an issue.
 */
static const size_t ParamVecAlignment = 64;

static const int A2AdditionalThreads = 0;
static const int OperatorsPerThreadTask = 6;

void print_param_vector(ParamVec pv)
{
    printf("pv data@%p, size=%d, %lu bytes\n",
           pv.data, pv.size, pv.size * sizeof(double));

    for (s32 i = 0; i < pv.size; i++)
    {
        if (is_param_valid(pv.data[i]))
        {
            printf("  [%2d] %lf\n", i, pv.data[i]);
        }
        else
        {
            printf("  [%2d] %lf, payload=0x%x\n",
                   i, pv.data[i], get_payload(pv.data[i]));
        }
    }
}

ParamVec push_param_vector(Arena *arena, s32 size)
{
    assert(size >= 0);

    ParamVec result;

    result.data = arena->pushArray<double>(size, ParamVecAlignment);
    result.size = result.data ? size : 0;
    assert(is_aligned(result.data, ParamVecAlignment));

    return result;
}

ParamVec push_param_vector(Arena *arena, s32 size, double value)
{
    assert(size >= 0);
    ParamVec result = push_param_vector(arena, size);
    fill(result, value);
    return result;
}

Extractor make_extractor(
    Arena *arena,
    MultiWordFilter filter,
    u32 requiredCompletions,
    u64 rngSeed,
    int moduleIndex)
{
    Extractor result = {};

    result.filter = filter;
    result.requiredCompletions = requiredCompletions;
    result.currentCompletions = 0;
    result.rng.seed(rngSeed);
    result.moduleIndex = moduleIndex;

    size_t addrCount = 1u << get_extract_bits(&result.filter, MultiWordFilter::CacheA);

    // The highest value the filter will yield is ((2^bits) - 1) but we're
    // adding a random in [0.0, 1.0) so the actual exclusive upper limit is
    // (2^bits).
    double upperLimit = std::pow(2.0, get_extract_bits(&result.filter, MultiWordFilter::CacheD));

    result.output.data = push_param_vector(arena, addrCount, invalid_param());
    result.output.lowerLimits = push_param_vector(arena, addrCount, 0.0);
    result.output.upperLimits = push_param_vector(arena, addrCount, upperLimit);

    return  result;
}

void extractor_begin_event(Extractor *ex)
{
    clear_completion(&ex->filter);
    ex->currentCompletions = 0;
    invalidate_all(ex->output.data);
}

static std::uniform_real_distribution<double> RealDist01(0.0, 1.0);

void extractor_process_module_data(Extractor *ex, const u32 *data, u32 size)
{
    for (u32 wordIndex = 0;
         wordIndex < size;
         wordIndex++)
    {
        u32 dataWord = data[wordIndex];

        if (process_data(&ex->filter, dataWord, wordIndex))
        {
            ex->currentCompletions++;

            if (ex->currentCompletions >= ex->requiredCompletions)
            {
                ex->currentCompletions = 0;
                u64 address = extract(&ex->filter, MultiWordFilter::CacheA);
                u64 value   = extract(&ex->filter, MultiWordFilter::CacheD);

                assert(address < static_cast<u64>(ex->output.data.size));

                if (!is_param_valid(ex->output.data[address]))
                {
                    ex->output.data[address] = value + RealDist01(ex->rng);
                }
            }

            clear_completion(&ex->filter);
        }
    }
}

void assign_input(Operator *op, PipeVectors input, s32 inputIndex)
{
    assert(inputIndex < op->inputCount);
    op->inputs[inputIndex] = input.data;
    op->inputLowerLimits[inputIndex] = input.lowerLimits;
    op->inputUpperLimits[inputIndex] = input.upperLimits;
}

void push_output_vectors(
    Arena *arena,
    Operator *op,
    s32 outputIndex,
    s32 size,
    double lowerLimit = 0.0,
    double upperLimit = 0.0)
{
    op->outputs[outputIndex] = push_param_vector(arena, size);
    op->outputLowerLimits[outputIndex] = push_param_vector(arena, size, lowerLimit);
    op->outputUpperLimits[outputIndex] = push_param_vector(arena, size, upperLimit);
}

Operator make_operator(Arena *arena, u8 type, u8 inputCount, u8 outputCount)
{
    Operator result = {};

    result.inputs = arena->pushArray<ParamVec>(inputCount);
    result.inputLowerLimits = arena->pushArray<ParamVec>(inputCount);
    result.inputUpperLimits = arena->pushArray<ParamVec>(inputCount);

    result.outputs = arena->pushArray<ParamVec>(outputCount);
    result.outputLowerLimits = arena->pushArray<ParamVec>(outputCount);
    result.outputUpperLimits = arena->pushArray<ParamVec>(outputCount);

    result.type = type;
    result.inputCount = inputCount;
    result.outputCount = outputCount;
    result.d = nullptr;

    return  result;
}

struct OperatorFunctions
{
    using StepFunction = void (*)(Operator *op);

    StepFunction step;
};

/* Calibration equation:
 * paramRange  = paramMax - paramMin    (the input range)
 * calibRange  = calibMax - calibMin    (the output range)
 * calibFactor = calibRange / paramRange
 * param = (param - paramMin) * (calibMax - calibMin) / (paramMax - paramMin) + calibMin;
 *       = (param - paramMin) * calibRange / paramRange + calibMin;
 *       = (param - paramMin) * (calibRange / paramRange) + calibMin;
 *       = (param - paramMin) * calibFactor + calibMin;
 *
 * -> 1 sub, 1 mul, 1 add
 */
inline double calibrate(
    double param, double paramMin,
    double calibMin, double calibFactor)
{
    if (is_param_valid(param))
    {
        param = (param - paramMin) * calibFactor + calibMin;
    }

    return param;
}

struct CalibrationData
{
    ParamVec calibFactors;
};

void calibration_step(Operator *op)
{
    a2_trace("\n");
    assert(op->inputCount == 1);
    assert(op->outputCount == 1);
    assert(op->inputs[0].size == op->outputs[0].size);
    assert(op->type == Operator_Calibration);

    auto d = reinterpret_cast<CalibrationData *>(op->d);
    s32 maxIdx = op->inputs[0].size;

    for (s32 idx = 0; idx < maxIdx; idx++)
    {
        op->outputs[0][idx] = calibrate(
            op->inputs[0][idx], op->inputLowerLimits[0][idx],
            op->outputLowerLimits[0][idx], d->calibFactors[idx]);

        if (!is_param_valid(op->inputs[0][idx]))
        {
            assert(!is_param_valid(op->outputs[0][idx]));
        }
    }
}

void calibration_sse_step(Operator *op)
{
    /* Note: The partially transformed code below is slower than
     * calibration_step(). With the right compiler flags gcc seems to auto
     * SIMD'ize very well.
     * TODO: Finish this implementation using intrinsics and then compare
     * again.
     */
    assert(op->inputCount == 1);
    assert(op->outputCount == 1);
    assert(op->inputs[0].size == op->outputs[0].size);
    assert(op->type == Operator_Calibration_sse);

    auto d = reinterpret_cast<CalibrationData *>(op->d);
    s32 maxIdx = op->inputs[0].size;

    /* Below are attempts at transforming the loop into something that can be
     * handled with SIMD intrinsics. */
#if 0
    assert((op->inputs[0].size % 2) == 0);
    for (s32 idx = 0; idx < maxIdx; idx += 2)
    {
        double p0 = op->inputs[0][idx + 0];
        double p1 = op->inputs[0][idx + 1];
        double min0 = op->inputLowerLimits[0][idx + 0];
        double min1 = op->inputLowerLimits[0][idx + 1];
        double diff0 = p0 - min0;
        double diff1 = p1 - min1;
        double mul0 = diff0 * d->calibFactors[idx + 0];
        double mul1 = diff1 * d->calibFactors[idx + 1];
        double r0 =  mul0 + op->outputLowerLimits[0][idx + 0];
        double r1 =  mul1 + op->outputLowerLimits[0][idx + 1];

        if (is_param_valid(p0))
            op->outputs[0][idx + 0] = r0;

        if (is_param_valid(p1))
            op->outputs[0][idx + 1] = r1;
    }
#else
    assert((op->inputs[0].size % 4) == 0);
    for (s32 idx = 0; idx < maxIdx; idx += 4)
    {
        double p0 = op->inputs[0][idx + 0];
        double p1 = op->inputs[0][idx + 1];
        double p2 = op->inputs[0][idx + 2];
        double p3 = op->inputs[0][idx + 3];

        double min0 = op->inputLowerLimits[0][idx + 0];
        double min1 = op->inputLowerLimits[0][idx + 1];
        double min2 = op->inputLowerLimits[0][idx + 2];
        double min3 = op->inputLowerLimits[0][idx + 3];

        double diff0 = p0 - min0;
        double diff1 = p1 - min1;
        double diff2 = p2 - min2;
        double diff3 = p3 - min3;

        double mul0 = diff0 * d->calibFactors[idx + 0];
        double mul1 = diff1 * d->calibFactors[idx + 1];
        double mul2 = diff2 * d->calibFactors[idx + 2];
        double mul3 = diff3 * d->calibFactors[idx + 3];

        double r0 =  mul0 + op->outputLowerLimits[0][idx + 0];
        double r1 =  mul1 + op->outputLowerLimits[0][idx + 1];
        double r2 =  mul2 + op->outputLowerLimits[0][idx + 2];
        double r3 =  mul3 + op->outputLowerLimits[0][idx + 3];

        op->outputs[0][idx + 0] = is_param_valid(p0) ? r0 : p0;
        op->outputs[0][idx + 1] = is_param_valid(p1) ? r1 : p1;
        op->outputs[0][idx + 2] = is_param_valid(p2) ? r2 : p2;
        op->outputs[0][idx + 3] = is_param_valid(p3) ? r3 : p3;
    }
#endif
}

Operator make_calibration(
    Arena *arena,
    PipeVectors input,
    double unitMin, double unitMax)
{
    assert(input.data.size == input.lowerLimits.size);
    assert(input.data.size == input.upperLimits.size);

    auto result = make_operator(arena, Operator_Calibration, 1, 1);

    assign_input(&result, input, 0);
    push_output_vectors(arena, &result, 0, input.data.size, unitMin, unitMax);

    auto cdata = arena->pushStruct<CalibrationData>();
    cdata->calibFactors = push_param_vector(arena, input.data.size);

    double calibRange = unitMax - unitMin;

    for (s32 i = 0; i < input.data.size; i++)
    {
        double paramRange = input.upperLimits[i] - input.lowerLimits[i];
        cdata->calibFactors[i] = calibRange / paramRange;
    }

    result.d = cdata;

    return result;
}

Operator make_calibration(
    Arena *arena,
    PipeVectors input,
    ParamVec calibMinimums,
    ParamVec calibMaximums)
{
    a2_trace("input.lowerLimits.size=%d, input.data.size=%d\n",
             input.lowerLimits.size, input.data.size);

    a2_trace("calibMinimums.size=%d, input.data.size=%d\n",
             calibMinimums.size, input.data.size);

    assert(input.data.size == input.lowerLimits.size);
    assert(input.data.size == input.upperLimits.size);
    assert(calibMinimums.size == input.data.size);
    assert(calibMaximums.size == input.data.size);

    auto result = make_operator(arena, Operator_Calibration, 1, 1);

    assign_input(&result, input, 0);
    push_output_vectors(arena, &result, 0, input.data.size);

    auto cdata = arena->pushStruct<CalibrationData>();
    cdata->calibFactors = push_param_vector(arena, input.data.size);

    for (s32 i = 0; i < input.data.size; i++)
    {
        double calibRange = calibMaximums[i] - calibMinimums[i];
        double paramRange = input.upperLimits[i] - input.lowerLimits[i];
        cdata->calibFactors[i] = calibRange / paramRange;

        result.outputLowerLimits[0][i] = calibMinimums[i];
        result.outputUpperLimits[0][i] = calibMaximums[i];
    }

    result.d = cdata;

    return result;
}

struct KeepPreviousData
{
    ParamVec previousInput;
    u8 keepValid;
};

void keep_previous_step(Operator *op)
{
    assert(op->inputCount == 1);
    assert(op->outputCount == 1);
    assert(op->inputs[0].size == op->outputs[0].size);
    assert(op->type == Operator_KeepPrevious);

    auto d = reinterpret_cast<KeepPreviousData *>(op->d);

    s32 maxIdx = op->inputs[0].size;

    for (s32 idx = 0; idx < maxIdx; idx++)
    {
        op->outputs[0][idx] = d->previousInput[idx];
    }

    for (s32 idx = 0; idx < maxIdx; idx++)
    {
        double in = op->inputs[0][idx];

        if (!d->keepValid || is_param_valid(in))
        {
            d->previousInput[idx] = in;
        }
    }
}

Operator make_keep_previous(
    Arena *arena, PipeVectors inPipe, bool keepValid)
{
    auto result = make_operator(arena, Operator_KeepPrevious, 1, 1);

    auto d = arena->pushStruct<KeepPreviousData>();
    d->previousInput = push_param_vector(arena, inPipe.data.size, invalid_param());
    d->keepValid = keepValid;
    result.d = d;

    assign_input(&result, inPipe, 0);
    push_output_vectors(arena, &result, 0, inPipe.data.size);

    return result;
}

Operator make_difference(
    Arena *arena,
    PipeVectors inPipeA,
    PipeVectors inPipeB)
{
    assert(inPipeA.data.size == inPipeB.data.size);

    auto result = make_operator(arena, Operator_Difference, 2, 1);

    assign_input(&result, inPipeA, 0);
    assign_input(&result, inPipeB, 1);

    push_output_vectors(arena, &result, 0, inPipeA.data.size);

    for (s32 idx = 0; idx < inPipeA.data.size; idx++)
    {
        result.outputLowerLimits[0][idx] = inPipeA.lowerLimits[idx] - inPipeB.upperLimits[idx];
        result.outputUpperLimits[0][idx] = inPipeA.upperLimits[idx] - inPipeB.lowerLimits[idx];
    }

    return result;
}

struct Difference_idxData
{
    s32 indexA;
    s32 indexB;
};

Operator make_difference_idx(
    Arena *arena,
    PipeVectors inPipeA,
    PipeVectors inPipeB,
    s32 indexA,
    s32 indexB)
{
    assert(indexA < inPipeA.data.size);
    assert(indexB < inPipeB.data.size);

    auto result = make_operator(arena, Operator_Difference_idx, 2, 1);

    result.d = arena->push<Difference_idxData>({indexA, indexB});

    assign_input(&result, inPipeA, 0);
    assign_input(&result, inPipeB, 1);

    push_output_vectors(arena, &result, 0, 1);

    result.outputLowerLimits[0][0] = inPipeA.lowerLimits[indexA] - inPipeB.upperLimits[indexB];
    result.outputUpperLimits[0][0] = inPipeA.upperLimits[indexA] - inPipeB.lowerLimits[indexB];

    return result;
}

void difference_step(Operator *op)
{
    assert(op->inputCount == 2);
    assert(op->outputCount == 1);
    assert(op->inputs[0].size == op->outputs[0].size);
    assert(op->inputs[1].size == op->outputs[0].size);
    assert(op->type == Operator_Difference);


    auto inputA = op->inputs[0];
    auto inputB = op->inputs[1];
    auto maxIdx = inputA.size;

    for (auto idx = 0; idx < maxIdx; idx++)
    {
        if (is_param_valid(inputA[idx]) && is_param_valid(inputB[idx]))
        {
            op->outputs[0][idx] = inputA[idx] - inputB[idx];
        }
        else
        {
            op->outputs[0][idx] = invalid_param();
        }
    }
}

void difference_step_idx(Operator *op)
{
    assert(op->inputCount == 2);
    assert(op->outputCount == 1);
    assert(op->type == Operator_Difference_idx);

    auto inputA = op->inputs[0];
    auto inputB = op->inputs[1];

    auto d = reinterpret_cast<Difference_idxData *>(op->d);

    if (is_param_valid(inputA[d->indexA]) && is_param_valid(inputB[d->indexB]))
    {
        op->outputs[0][0] = inputA[d->indexA] - inputB[d->indexB];
    }
    else
    {
        op->outputs[0][0] = invalid_param();
    }
}

/**
 * ArrayMap: Map elements of one or more input arrays to an output array.
 *
 * Can be used to concatenate multiple arrays and/or change the order of array
 * members.
 */

void array_map_step(Operator *op)
{
    auto d = reinterpret_cast<ArrayMapData *>(op->d);

    s32 mappingCount = d->mappings.size;

    for (s32 mi = 0; mi < mappingCount; mi++)
    {
        auto mapping = d->mappings[mi];
        op->outputs[0][mi] = op->inputs[mapping.inputIndex][mapping.paramIndex];
    }
}

/* Note: mappings are deep copied, inputs are assigned. */
Operator make_array_map(
    Arena *arena,
    TypedBlock<PipeVectors, s32> inputs,
    TypedBlock<ArrayMapData::Mapping, s32> mappings)
{
    auto result = make_operator(arena, Operator_ArrayMap, inputs.size, 1);

    for (s32 ii = 0; ii < inputs.size; ii++)
    {
        assign_input(&result, inputs[ii], ii);
    }

    auto d = arena->pushStruct<ArrayMapData>();
    d->mappings = push_typed_block<ArrayMapData::Mapping, s32>(arena, mappings.size);

    push_output_vectors(arena, &result, 0, mappings.size);

    for (s32 mi = 0; mi < mappings.size; mi++)
    {
        auto m = d->mappings[mi] = mappings[mi];
        result.outputLowerLimits[0][mi] = inputs[m.inputIndex].lowerLimits[m.paramIndex];
        result.outputUpperLimits[0][mi] = inputs[m.inputIndex].upperLimits[m.paramIndex];
    }

    result.d = d;

    return result;
}

using BinaryEquationFunction = void (*)(ParamVec a, ParamVec b, ParamVec out);

#define add_binary_equation(x) \
[](ParamVec a, ParamVec b, ParamVec o)\
{\
    for (s32 i = 0; i < a.size; ++i)\
    {\
        if (is_param_valid(a[i]) && is_param_valid(b[i])) \
        {\
            x;\
        }\
        else\
        {\
            o[i] = invalid_param();\
        }\
    }\
}

static BinaryEquationFunction BinaryEquationTable[] =
{
    add_binary_equation(o[i] = a[i] + b[i]),

    add_binary_equation(o[i] = a[i] - b[i]),

    add_binary_equation(o[i] = (a[i] + b[i]) / (a[i] - b[i])),

    add_binary_equation(o[i] = (a[i] - b[i]) / (a[i] + b[i])),

    add_binary_equation(o[i] = a[i] / (a[i] - b[i])),

    add_binary_equation(o[i] = (a[i] - b[i]) / a[i]),
};
#undef add_binary_equation

static const size_t BinaryEquationCount = ArrayCount(BinaryEquationTable);

void binary_equation_step(Operator *op)
{
    // The equationIndex is stored directly in the d pointer.
    u32 equationIndex = (uintptr_t)op->d;

    BinaryEquationTable[equationIndex](
        op->inputs[0], op->inputs[1], op->outputs[0]);
}

Operator make_binary_equation(
    Arena *arena,
    PipeVectors inputA,
    PipeVectors inputB,
    u32 equationIndex,
    double outputLowerLimit,
    double outputUpperLimit)
{
    assert(equationIndex < ArrayCount(BinaryEquationTable));

    auto result = make_operator(arena, Operator_BinaryEquation, 2, 1);

    assign_input(&result, inputA, 0);
    assign_input(&result, inputB, 1);

    push_output_vectors(arena, &result, 0, inputA.data.size,
                        outputLowerLimit, outputUpperLimit);

    result.d = (void *)(uintptr_t)equationIndex;

    return result;
}

/* ===============================================
 * AggregateOps
 * =============================================== */
inline bool is_valid_and_inside(double param, Thresholds thresholds)
{
    return (is_param_valid(param)
            && thresholds.min <= param
            && thresholds.max >= param);
}

static Operator make_aggregate_op(
    Arena *arena,
    PipeVectors input,
    u8 operatorType,
    Thresholds thresholds)
{
    auto result = make_operator(arena, operatorType, 1, 1);

    a2_trace("input thresholds: %lf, %lf\n", thresholds.min, thresholds.max);

    /* The min and max values must be set to the inputs lowest/highest limits if no
     * threshold filtering is wanted. This way a isnan() test can be saved. */
    if (std::isnan(thresholds.min))
    {
        thresholds.min = *std::min_element(std::begin(input.lowerLimits), std::end(input.lowerLimits));
    }

    if (std::isnan(thresholds.max))
    {
        thresholds.max = *std::max_element(std::begin(input.upperLimits), std::end(input.upperLimits));
    }

    a2_trace("resulting thresholds: %lf, %lf\n", thresholds.min, thresholds.max);

    assert(!std::isnan(thresholds.min)); // XXX: can be nan if input limits are nan
    assert(!std::isnan(thresholds.max));

    auto d = arena->push(thresholds);
    result.d = d;
    *d = thresholds;

    assign_input(&result, input, 0);

    /* Note: output lower/upper limits are not set here. That's left to the
     * specific operatorType make_aggregate_X() implementation. */
    push_output_vectors(arena, &result, 0, 1);

    return result;
}

Operator make_aggregate_sum(
    Arena *arena,
    PipeVectors input,
    Thresholds thresholds)
{
    a2_trace("thresholds: %lf, %lf\n", thresholds.min, thresholds.max);

    auto result = make_aggregate_op(arena, input, Operator_Sum, thresholds);

    double outputLowerLimit = 0.0;
    double outputUpperLimit = 0.0;

    for (s32 i = 0; i < input.data.size; i++)
    {
        outputLowerLimit += std::min(input.lowerLimits[i], input.upperLimits[i]);
        outputUpperLimit += std::max(input.lowerLimits[i], input.upperLimits[i]);
    }

    result.outputLowerLimits[0][0] = outputLowerLimit;
    result.outputUpperLimits[0][0] = outputUpperLimit;

    return result;
}

void aggregate_sum_step(Operator *op)
{
    a2_trace("\n");
    auto input = op->inputs[0];
    auto thresholds = *reinterpret_cast<Thresholds *>(op->d);

    double theSum = 0.0;

    for (s32 i = 0; i < input.size; i++)
    {
        //a2_trace("i=%d, input[i]=%lf, thresholds.min=%lf, thresholds.max=%lf, is_valid_and_inside()=%d\n",
        //         i, input[i], thresholds.min, thresholds.max, is_valid_and_inside(input[i], thresholds));

        if (is_valid_and_inside(input[i], thresholds))
        {
            theSum += input[i];
        }
    }

    op->outputs[0][0] = theSum;
}

Operator make_aggregate_multiplicity(
    Arena *arena,
    PipeVectors input,
    Thresholds thresholds)
{
    a2_trace("thresholds: %lf, %lf\n", thresholds.min, thresholds.max);
    auto result = make_aggregate_op(arena, input, Operator_Multiplicity, thresholds);

    result.outputLowerLimits[0][0] = 0.0;
    result.outputUpperLimits[0][0] = input.data.size;

    return result;
}

void aggregate_multiplicity_step(Operator *op)
{
    auto input = op->inputs[0];
    auto thresholds = *reinterpret_cast<Thresholds *>(op->d);

    s32 result = 0;

    for (s32 i = 0; i < input.size; i++)
    {
        if (is_valid_and_inside(input[i], thresholds))
        {
            result++;
        }
    }

    op->outputs[0][0] = result;
}

Operator make_aggregate_max(
    Arena *arena,
    PipeVectors input,
    Thresholds thresholds)
{
    a2_trace("thresholds: %lf, %lf\n", thresholds.min, thresholds.max);
    auto result = make_aggregate_op(arena, input, Operator_Max, thresholds);

    double llMin = std::min(
        *std::min_element(std::begin(input.lowerLimits), std::end(input.lowerLimits)),
        *std::min_element(std::begin(input.upperLimits), std::end(input.upperLimits)));

    double llMax = std::max(
        *std::max_element(std::begin(input.lowerLimits), std::end(input.lowerLimits)),
        *std::max_element(std::begin(input.upperLimits), std::end(input.upperLimits)));

    result.outputLowerLimits[0][0] = llMin;
    result.outputUpperLimits[0][0] = llMax;

    return result;
}

void aggregate_max_step(Operator *op)
{
    auto input = op->inputs[0];
    auto thresholds = *reinterpret_cast<Thresholds *>(op->d);

    double result = std::numeric_limits<double>::lowest();

    for (s32 i = 0; i < input.size; i++)
    {
        if (is_valid_and_inside(input[i], thresholds))
        {
            result = std::max(result, input[i]);
        }
    }

    op->outputs[0][0] = result;
}

/* ===============================================
 * Histograms
 * =============================================== */

inline double get_bin_unchecked(Binning binning, s32 binCount, double x)
{
    return (x - binning.min) * binCount / binning.range;
}

// binMin = binning.min
// binFactor = binCount / binning.range
inline double get_bin_unchecked(double x, double binMin, double binFactor)
{
    return (x - binMin) * binFactor;
}

inline s32 get_bin(Binning binning, s32 binCount, double x)
{
    double bin = get_bin_unchecked(binning, binCount, x);

    if (bin < 0.0)
        return Binning::Underflow;

    if (bin >= binCount)
        return Binning::Overflow;

    return static_cast<s32>(bin);
}

inline s32 get_bin(H1D histo, double x)
{
    return get_bin(histo.binning, histo.size, x);
}

inline void fill_h1d(H1D *histo, double x)
{
    /* Instead of calculating the bin and then checking if it under/overflows
     * this code decides by comparing x to the binnings min and max values.
     * This is faster. */
    if (x < histo->binning.min)
    {
        assert(get_bin(*histo, x) == Binning::Underflow);
        histo->underflow++;
    }
    else if (x >= histo->binning.min + histo->binning.range)
    {
        assert(get_bin(*histo, x) == Binning::Overflow);
        histo->overflow++;
    }
    else if (std::isnan(x))
    {
        // pass for now
    }
    else if (likely(1))
    {
        assert(0 <= get_bin(*histo, x) && get_bin(*histo, x) < histo->size);

        //s32 bin = static_cast<s32>(get_bin_unchecked(histo->binning, histo->size, x));
        s32 bin = static_cast<s32>(get_bin_unchecked(x, histo->binning.min, histo->binningFactor));

        double value = ++histo->data[bin];
        histo->entryCount++;
    }
}

inline s32 get_bin(H2D histo, H2D::Axis axis, double v)
{
    return get_bin(histo.binnings[axis], histo.binCounts[axis], v);
}

inline void fill_h2d(H2D *histo, double x, double y)
{
    if (x < histo->binnings[H2D::XAxis].min)
    {
        assert(get_bin(*histo, H2D::XAxis, x) == Binning::Underflow);
        histo->underflow++;
    }
    else if (x >= histo->binnings[H2D::XAxis].min + histo->binnings[H2D::XAxis].range)
    {
        assert(get_bin(*histo, H2D::XAxis, x) == Binning::Overflow);
        histo->overflow++;
    }
    else if (y < histo->binnings[H2D::YAxis].min)
    {
        assert(get_bin(*histo, H2D::YAxis, y) == Binning::Underflow);
        histo->underflow++;
    }
    else if (y >= histo->binnings[H2D::YAxis].min + histo->binnings[H2D::YAxis].range)
    {
        assert(get_bin(*histo, H2D::YAxis, y) == Binning::Overflow);
        histo->overflow++;
    }
    else if (std::isnan(x) || std::isnan(y))
    {
        // pass for now
    }
    else if (likely(1))
    {
        assert(0 <= get_bin(*histo, H2D::XAxis, x)
               && get_bin(*histo, H2D::XAxis, x) < histo->binCounts[H2D::XAxis]);

        assert(0 <= get_bin(*histo, H2D::YAxis, y)
               && get_bin(*histo, H2D::YAxis, y) < histo->binCounts[H2D::YAxis]);

        s32 xBin = static_cast<s32>(get_bin_unchecked(
                x,
                histo->binnings[H2D::XAxis].min,
                histo->binningFactors[H2D::XAxis]));

        s32 yBin = static_cast<s32>(get_bin_unchecked(
                y,
                histo->binnings[H2D::YAxis].min,
                histo->binningFactors[H2D::YAxis]));

        s32 linearBin = yBin * histo->binCounts[H2D::XAxis] + xBin;

        a2_trace("x=%lf, y=%lf, xBin=%d, yBin=%d, linearBin=%d\n",
                 x, y, xBin, yBin, linearBin);


        assert(0 <= linearBin && linearBin < histo->size);

        histo->data[linearBin]++;
    }
}

inline double get_value(H1D histo, double x)
{
    s32 bin = get_bin(histo, x);
    return (bin < 0) ? 0.0 : histo.data[bin];
}

void clear_histo(H1D *histo)
{
    histo->binningFactor = 0.0;
    histo->entryCount = 0.0;
    histo->underflow = 0.0;
    histo->overflow = 0.0;
    for (s32 i = 0; i < histo->size; i++)
    {
        histo->data[i] = 0.0;
    }
}

/* Note: Histos are copied. This means during runtime only the H1D structures
 * inside H1DSinkData are updated. Histogram storage itself is not copied. It
 * is assumed that this storage is handled separately.
 * The implementation could be changed to store an array of pointers to H1D.
 * Then the caller would have to keep the H1D instances around too.
 */
Operator make_h1d_sink(
    Arena *arena,
    PipeVectors inPipe,
    TypedBlock<H1D, s32> histos)
{
    assert(inPipe.data.size == histos.size);
    auto result = make_operator(arena, Operator_H1DSink, 1, 0);
    assign_input(&result, inPipe, 0);

    auto d = arena->pushStruct<H1DSinkData>();
    result.d = d;

    d->histos = push_typed_block<H1D, s32>(arena, histos.size);

    for (s32 i = 0; i < histos.size; i++)
    {
        d->histos[i] = histos[i];
    }

    return result;
}

void h1d_sink_step(Operator *op)
{
    a2_trace("\n");
    auto d = reinterpret_cast<H1DSinkData *>(op->d);
    s32 maxIdx = op->inputs[0].size;

    for (s32 idx = 0; idx < maxIdx; idx++)
    {
        fill_h1d(&d->histos[idx], op->inputs[0][idx]);
    }
}

void h1d_sink_step_idx(Operator *op)
{
    a2_trace("\n");
    auto d = reinterpret_cast<H1DSinkData_idx *>(op->d);

    assert(d->histos.size == 1);
    assert(d->inputIndex < op->inputs[0].size);

    fill_h1d(&d->histos[0], op->inputs[0][d->inputIndex]);
}

Operator make_h1d_sink_idx(
    Arena *arena,
    PipeVectors inPipe,
    TypedBlock<H1D, s32> histos,
    s32 inputIndex)
{
    assert(histos.size == 1);
    assert(inputIndex < inPipe.data.size);

    auto result = make_operator(arena, Operator_H1DSink_idx, 1, 0);
    assign_input(&result, inPipe, 0);

    auto d = arena->pushStruct<H1DSinkData_idx>();
    result.d = d;

    d->histos = push_typed_block<H1D, s32>(arena, histos.size);
    d->inputIndex = inputIndex;

    for (s32 i = 0; i < histos.size; i++)
    {
        d->histos[i] = histos[i];
    }

    return result;
}

Operator make_h2d_sink(
    Arena *arena,
    PipeVectors xInput,
    PipeVectors yInput,
    s32 xIndex,
    s32 yIndex,
    H2D histo)
{
    assert(0 <= xIndex && xIndex < xInput.data.size);
    assert(0 <= yIndex && yIndex < yInput.data.size);

    auto result = make_operator(arena, Operator_H2DSink, 2, 0);

    assign_input(&result, xInput, 0);
    assign_input(&result, yInput, 1);

    auto d = arena->push<H2DSinkData>({ histo, xIndex, yIndex });
    result.d = d;

    return result;
};

void h2d_sink_step(Operator *op)
{
    a2_trace("\n");

    auto d = reinterpret_cast<H2DSinkData *>(op->d);

    fill_h2d(
        &d->histo,
        op->inputs[0][d->xIndex],
        op->inputs[1][d->yIndex]);
}

/* ===============================================
 * A2 implementation
 * =============================================== */

static const OperatorFunctions OperatorTable[OperatorTypeCount] =
{
    [Operator_Calibration] = { calibration_step },
    [Operator_Calibration_sse] = { calibration_sse_step },
    [Operator_KeepPrevious] = { keep_previous_step },
    [Operator_Difference] = { difference_step },
    [Operator_Difference_idx] = { difference_step_idx },
    [Operator_ArrayMap] = { array_map_step },
    [Operator_BinaryEquation] = { binary_equation_step },
    [Operator_H1DSink] = { h1d_sink_step },
    [Operator_H1DSink_idx] = { h1d_sink_step_idx },
    [Operator_H2DSink] = { h2d_sink_step },
    [Operator_RangeFilter] = { nullptr },

    [Operator_Sum] = { aggregate_sum_step },
    [Operator_Multiplicity] = { aggregate_multiplicity_step },
    [Operator_Max] = { aggregate_max_step },
};

#if 0
static const char *OperatorTypeNames[OperatorTypeCount] =
{
    [Operator_Calibration]          =
    [Operator_Calibration_sse]      =
    [Operator_KeepPrevious]         =
    [Operator_Difference]           =
    [Operator_Difference_idx]       = { difference_step_idx },
    [Operator_ArrayMap]             = { array_map_step },
    [Operator_BinaryEquation]       = { binary_equation_step },
    [Operator_H1DSink]              = { h1d_sink_step },
    [Operator_H1DSink_idx]          = { h1d_sink_step_idx },
    [Operator_H2DSink] = { h2d_sink_step },
    [Operator_RangeFilter] = { nullptr },

    [Operator_Sum] = { aggregate_sum_step },
    [Operator_Multiplicity] = { aggregate_multiplicity_step },
    [Operator_Max] = { aggregate_max_step },
};
#endif

inline void step_operator(Operator *op)
{
    OperatorTable[op->type].step(op);
}

A2 make_a2(
    Arena *arena,
    std::initializer_list<u8> extractorCounts,
    std::initializer_list<u8> operatorCounts)
{
    assert(extractorCounts.size() < MaxVMEEvents);
    assert(operatorCounts.size() < MaxVMEEvents);

    A2 result = {};

    result.extractorCounts.fill(0);
    result.extractors.fill(nullptr);
    result.operatorCounts.fill(0);
    result.operators.fill(nullptr);
    result.operatorRanks.fill(0);

    const u8 *ec = extractorCounts.begin();

    for (size_t ei = 0; ei < extractorCounts.size(); ++ei, ++ec)
    {
        //printf("%s: %lu -> %u\n", __PRETTY_FUNCTION__, ei, (u32)*ec);
        result.extractors[ei] = arena->pushArray<Extractor>(*ec);
    }

    for (size_t ei = 0; ei < operatorCounts.size(); ++ei)
    {
        result.operators[ei] = arena->pushArray<Operator>(operatorCounts.begin()[ei]);
        result.operatorRanks[ei] = arena->pushArray<u8>(operatorCounts.begin()[ei]);
    }

    return result;
}

// run begin_event() on all sources for the given eventIndex
void a2_begin_event(A2 *a2, int eventIndex)
{
    assert(eventIndex < MaxVMEEvents);

    int exCount = a2->extractorCounts[eventIndex];

    a2_trace("ei=%d, extractors=%d\n", eventIndex, exCount);

    for (int exIdx = 0; exIdx < exCount; exIdx++)
    {
        Extractor *ex = a2->extractors[eventIndex] + exIdx;
        extractor_begin_event(ex);
    }
}

// hand module data to all sources for eventIndex and moduleIndex
void a2_process_module_data(A2 *a2, int eventIndex, int moduleIndex, const u32 *data, u32 dataSize)
{
    assert(eventIndex < MaxVMEEvents);
    assert(moduleIndex < MaxVMEModules);

    int exCount = a2->extractorCounts[eventIndex];
#ifndef NDEBUG
    int nprocessed = 0;
#endif

    for (int exIdx = 0; exIdx < exCount; exIdx++)
    {
        Extractor *ex = a2->extractors[eventIndex] + exIdx;
        if (ex->moduleIndex == moduleIndex)
        {
            extractor_process_module_data(ex, data, dataSize);
#ifndef NDEBUG
            nprocessed++;
#endif
        }
        else if (ex->moduleIndex > moduleIndex)
        {
            break;
        }
    }

#ifndef NDEBUG
    a2_trace("ei=%d, mi=%d, processed %d extractors\n", eventIndex, moduleIndex, nprocessed);
#endif
}

inline u32 step_operator_range(Operator *first, Operator *last)
{
    u32 opSteppedCount = 0;

    for (auto op = first; op < last; ++op)
    {
        a2_trace("    op@%p\n", op);

        assert(op);
        assert(op->type < ArrayCount(OperatorTable));
        assert(OperatorTable[op->type].step);

        OperatorTable[op->type].step(op);
        opSteppedCount++;
    }

    return opSteppedCount;
}

struct Work
{
    Operator *begin = nullptr;
    Operator *end = nullptr;
};

static const s32 WorkQueueSize = 32;

struct WorkQueue
{
    mpmc_bounded_queue<Work> queue;
    //NonRecursiveBenaphore mutex;
    LightweightSemaphore taskSem;
    LightweightSemaphore tasksDoneSem;
    //std::atomic<int> tasksDone;

    using Guard = std::lock_guard<NonRecursiveBenaphore>;

    explicit WorkQueue(size_t size)
        : queue(size)
    {}
};

struct ThreadInfo
{
    int id = 0;
};

Work dequeue(WorkQueue *queue, ThreadInfo threadInfo)
{
#if 0
    std::unique_lock<std::mutex> lock(queue->mutex);

    while (queue->queue.empty())
    {
        queue->cv_notEmpty.wait(lock);
    }

    Work result = queue->queue.front();
    queue->queue.pop();
#else
    Work result = {};

    for (;;)
    {
        a2_trace("a2 worker %d waiting for taskSem\n",
                 threadInfo.id);

        // block here and try the queue until we get an item
        queue->taskSem.wait();

        a2_trace("a2 worker %d taking the lock\n",
                 threadInfo.id);

        //WorkQueue::Guard guard(queue->mutex);
        if (queue->queue.dequeue(result))
        {
            break;
        }

        //if (!queue->queue.empty())
        //{
        //    result = queue->queue.front();
        //    queue->queue.pop();
        //    break;
        //}
    }

    a2_trace("a2 worker %d got a task\n",
             threadInfo.id);
#endif

    return result;
}

void a2_worker_loop(WorkQueue *queue, ThreadInfo threadInfo)
{
    a2_trace("worker %d starting up\n", threadInfo.id);

    for (;;)
    {
        // the deqeue() call blocks until we get an item
        auto work = dequeue(queue, threadInfo);

        if (work.begin)
        {
            a2_trace("worker %d got %d operators to step\n",
                     threadInfo.id,
                     (s32)(work.end - work.begin));

            step_operator_range(work.begin, work.end);
            //queue->tasksDone++;
            queue->tasksDoneSem.signal();
        }
        else // nullptr is the "quit message"
        {
            a2_trace("worker %d got nullptr work\n",
                     threadInfo.id);
            //queue->tasksDone++;
            queue->tasksDoneSem.signal();
            break;
        }
    }

    a2_trace("worker %d about to quit\n",
             threadInfo.id);
}

u32 step_operator_range_threaded(WorkQueue *queue, Operator *first, Operator *last)
{

    const s32 opCount = (s32)(last - first);
    s32 tasksQueued = 0;
    s32 opsQueued = 0;

    a2_trace("about to step %d operators\n", opCount);

    {
        // Workers should spin or wait in queue->taskSem.wait() so the rwLock
        // should always be uncontested.
        a2_trace("main a2 worker taking lock before enqueueing work\n");
        //WorkQueue::Guard guard(queue->mutex);

        //queue->tasksDone = 0;
        assert(queue->tasksDoneSem.count() == 0);

        auto op = first;

        while (op < last)
        {
            s32 opsToQueue = std::min(OperatorsPerThreadTask, (s32)(last - op));

            a2_trace("about to enqueue a task of %d operators\n",
                     opsToQueue);

            //queue->queue.push({ op, op + opsToQueue});
            if (queue->queue.enqueue({ op, op + opsToQueue }))
            {
                op += opsToQueue;
                tasksQueued++;
                opsQueued += opsToQueue;
            }
        }

        // workers are still in queue->taskSem.wait()
    }

    assert(opsQueued == opCount);

    a2_trace("work prepared, notifying workers: taskSem.signal(%d)\n",
             tasksQueued);
    queue->taskSem.signal(tasksQueued);

    // Workers should wake up and hit the rwLock now

    a2_trace("main a2 worker starting work\n");

    // This must atomically fetch queue->tasksDone every time through the loop.
    //while (queue->tasksDone.load() < tasksQueued)
    while (true)
    {
        Work task = {};
        {
            a2_trace("main a2 worker taking the lock\n");
            // Contend with the workers for the rwLock
            //WorkQueue::Guard guard(queue->mutex);

            a2_trace("main a2 worker got the lock, tasksQueued=%d, tasksDone=%d", //, queue.size()=%u\n|",
                     tasksQueued,
                     queue->tasksDoneSem.count());
                     //queue->tasksDone.load());
                     //(u32)queue->queue.size());

            if (!queue->queue.dequeue(task))
            {
                break;
            }

            //if (!queue->queue.empty())
            //{
            //    task = queue->queue.front();
            //    queue->queue.pop();
            //}
        }

        if (task.begin)
        {
            a2_trace("main a2 worker got %d operators to step\n",
                     (s32)(task.end - task.begin));

            step_operator_range(task.begin, task.end);

            //queue->tasksDone++;
            queue->tasksDoneSem.signal();
        }
    }

    //while (queue->tasksDone < tasksQueued);

    a2_trace("tasksQueued=%d, tasksDone=%d\n",
             tasksQueued,
             queue->tasksDoneSem.count());
             //queue->tasksDone.load());

    for (s32 i = 0; i < tasksQueued; i++)
    {
        queue->tasksDoneSem.wait();
    }

    //assert(queue->tasksDone == tasksQueued);
    assert(queue->tasksDoneSem.count() == 0);

    a2_trace("main a2 worker done\n");

    a2_trace("work was done on %d operators in %d tasks\n",
             opCount,
             tasksQueued);

    return opCount;
}

static WorkQueue A2WorkQueue(WorkQueueSize);

static std::vector<std::thread> A2Threads = {};

void a2_begin_run(A2 *a2)
{
    if (A2AdditionalThreads > 0)
    {
        A2Threads.clear();

        a2_trace("starting %d workers\n", A2AdditionalThreads);

        for (int threadId = 0; threadId < A2AdditionalThreads; threadId++)
        {
            A2Threads.emplace_back(a2_worker_loop, &A2WorkQueue, ThreadInfo{ threadId });
        }
    }
}

void a2_end_run(A2 *a2)
{
    if (A2AdditionalThreads > 0)
    {
        a2_trace("about to queue nullptr work\n");

        auto queue = &A2WorkQueue;
        const s32 threadCount = (s32)A2Threads.size();

        {
            //WorkQueue::Guard guard(queue->mutex);
            //queue->tasksDone = 0;
            assert(queue->tasksDoneSem.count() == 0);

            for (s32 i = 0; i < threadCount; i++)
            {
                //queue->queue.push({ nullptr, nullptr });
                queue->queue.enqueue({ nullptr, nullptr });
            }
        }

        a2_trace("notifying workers: taskSem.signal(%d)\n",
                 threadCount);

        queue->taskSem.signal(threadCount);

        a2_trace("waiting for workers to quit\n");

        //while (queue->tasksDone.load() < threadCount) /* spin */;

        for (s32 i = 0; i < threadCount; i++)
        {
            queue->tasksDoneSem.wait();
        }

        a2_trace("workers have quit, joining threads\n");

        for (s32 threadId = 0; threadId < threadCount; threadId++)
        {
            if (A2Threads[threadId].joinable())
            {
                a2_trace("thread %d is joinable\n", threadId);
                A2Threads[threadId].join();
            }
            else
            {
                a2_trace("thread %d was not joinable\n", threadId);
            }
        }
    }

    a2_trace("done\n");
}

// step operators for the eventIndex
// operators must be sorted by rank
void a2_end_event(A2 *a2, int eventIndex)
{
    assert(eventIndex < MaxVMEEvents);

    const int opCount = a2->operatorCounts[eventIndex];
    Operator *operators = a2->operators[eventIndex];
    u8 *ranks = a2->operatorRanks[eventIndex];
    s32 opSteppedCount = 0;

    a2_trace("ei=%d, stepping %d operators\n", eventIndex, opCount);

    if (opCount)
    {
        if (A2AdditionalThreads == 0)
        {
            for (int opIdx = 0; opIdx < opCount; opIdx++)
            {
                Operator *op = operators + opIdx;

                a2_trace("  op@%p\n", op);

                assert(op);
                assert(op->type < ArrayCount(OperatorTable));
                assert(OperatorTable[op->type].step);

                OperatorTable[op->type].step(op);
                opSteppedCount++;
            }
        }
        else
        {
            if (opCount)
            {
                const Operator *opEnd = operators + opCount;

                u8* rankBegin = ranks;
                Operator *opRankBegin = operators;

                while (opRankBegin < opEnd)
                {
                    u8* rankEnd = rankBegin;
                    Operator *opRankEnd = opRankBegin;

                    while (opRankEnd < opEnd)
                    {
                        if (*rankEnd > *rankBegin)
                        {
                            break;
                        }

                        rankEnd++;
                        opRankEnd++;
                    }

                    a2_trace("  stepping rank %d, %u operators\n",
                             (s32)*rankBegin,
                             (u32)(opRankEnd - opRankBegin));

                    auto prevCount = opSteppedCount;

                    // step the operators in [opRankBegin, opRankEnd)
                    opSteppedCount += step_operator_range_threaded(&A2WorkQueue, opRankBegin, opRankEnd);

                    a2_trace("  stepped rank %d, %u operators\n",
                             (s32)*rankBegin,
                             opSteppedCount - prevCount);

                    // advance
                    rankBegin = rankEnd;
                    opRankBegin = opRankEnd;
                }
            }
        }
    }

    assert(opSteppedCount == opCount);

    a2_trace("ei=%d, %d operators stepped\n", eventIndex, opSteppedCount);
}

} // namespace a2
