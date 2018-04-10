#include "mpmc_queue.cc"
#if 1
#include "a2_exprtk.h"
#endif
#include "a2_impl.h"
#include "util/assert.h"
#include "util/perf.h"
#include "cpp11-on-multicore/common/benaphore.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include <zstr/src/zstr.hpp>

#define ArrayCount(x) (sizeof(x) / sizeof(*x))

//#ifndef NDEBUG
#if 0

// printf style trace macro
#define a2_trace(fmt, ...)\
do\
{\
    fprintf(stderr, "a2::%s() " fmt, __FUNCTION__, ##__VA_ARGS__);\
} while (0);

// "NoPrefix" verison of the trace macro
#define a2_trace_np(fmt, ...)\
do\
{\
    fprintf(stderr, fmt, ##__VA_ARGS__);\
} while (0);
#else
#define a2_trace(...)
#define a2_trace_np(...)
#endif

#include <iostream>

using std::cerr;
using std::endl;

namespace a2
{

using namespace data_filter;
using namespace memory;

/* TODO list
 * - Add tests for range_filter_step(). Test it in mvme.
 * - Test aggregate mean and meanx
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
 */

/* Alignment in bytes of all double vectors created by the system.
 * SSE requires 16 byte alignment (128 bit registers).
 * AVX wants 32 bytes (256 bit registers).
 *
 * Another factor is the cache line size. On Skylake it's 64 bytes.
 */
static const size_t ParamVecAlignment = 64;

/* Asserted in extractor_process_module_data(). */
static const size_t ModuleDataAlignment = alignof(u32);

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

void assign_input(Operator *op, PipeVectors input, s32 inputIndex)
{
    assert(inputIndex < op->inputCount);
    op->inputs[inputIndex] = input.data;
    op->inputLowerLimits[inputIndex] = input.lowerLimits;
    op->inputUpperLimits[inputIndex] = input.upperLimits;
}

/* ===============================================
 * Extractors
 * =============================================== */
static std::uniform_real_distribution<double> RealDist01(0.0, 1.0);

size_t get_address_count(DataSource *ds)
{
    switch (static_cast<DataSourceType>(ds->type))
    {
        case DataSource_Extractor:
            {
                auto ex = reinterpret_cast<Extractor *>(ds->d);
                return get_address_count(ex);
            } break;

        case DataSource_ListFilterExtractor:
            {
                auto ex = reinterpret_cast<ListFilterExtractor *>(ds->d);
                return get_address_count(ex);
            } break;
    }

    return 0u;
}

size_t get_address_count(Extractor *ex)
{
    u16 bits = get_extract_bits(&ex->filter, MultiWordFilter::CacheA);
    return 1u << bits;
}

size_t get_base_address_bits(ListFilterExtractor *ex)
{
    size_t baseAddressBits = get_extract_bits(&ex->listFilter, MultiWordFilter::CacheA);
    return baseAddressBits;
}

size_t get_repetition_address_bits(ListFilterExtractor *ex)
{
    size_t result = static_cast<size_t>(std::ceil(std::log2(ex->repetitions)));
    return result;
}

size_t get_address_bits(ListFilterExtractor *ex)
{
    size_t baseAddressBits = get_base_address_bits(ex);
    size_t repAddressBits  = get_repetition_address_bits(ex);
    size_t bits = baseAddressBits + repAddressBits;
    return bits;
}

size_t get_address_count(ListFilterExtractor *ex)
{
    return 1u << get_address_bits(ex);;
}

// Extractor

Extractor make_extractor(
    data_filter::MultiWordFilter filter,
    u32 requiredCompletions,
    u64 rngSeed,
    DataSourceOptions::opt_t options)
{
    Extractor ex = {};

    ex.filter = filter;
    ex.requiredCompletions = requiredCompletions;
    ex.currentCompletions = 0;
    ex.rng.seed(rngSeed);
    ex.options = options;

    return ex;
}

DataSource make_datasource_extractor(
    Arena *arena,
    MultiWordFilter filter,
    u32 requiredCompletions,
    u64 rngSeed,
    int moduleIndex,
    DataSourceOptions::opt_t options)
{
    DataSource result = {};
    result.type = DataSource_Extractor;

    auto ex = arena->pushStruct<Extractor>();
    *ex = make_extractor(filter, requiredCompletions, rngSeed, options);
    result.d = ex;

    result.moduleIndex = moduleIndex;

    size_t addrCount = get_address_count(&result);

    // The highest value the filter will yield is ((2^bits) - 1) but we're
    // adding a random in [0.0, 1.0) so the actual exclusive upper limit is
    // (2^bits).
    double upperLimit = std::pow(2.0, get_extract_bits(&ex->filter, MultiWordFilter::CacheD));

    result.output.data = push_param_vector(arena, addrCount, invalid_param());
    result.output.lowerLimits = push_param_vector(arena, addrCount, 0.0);
    result.output.upperLimits = push_param_vector(arena, addrCount, upperLimit);

    result.hitCounts = push_param_vector(arena, addrCount, 0.0);

    return  result;
}

void extractor_begin_event(DataSource *ds)
{
    assert(ds->type == DataSource_Extractor);
    auto ex = reinterpret_cast<Extractor *>(ds->d);
    clear_completion(&ex->filter);
    ex->currentCompletions = 0;
    invalidate_all(ds->output.data);
}

void extractor_process_module_data(DataSource *ds, u32 *data, u32 size)
{
    assert(memory::is_aligned(data, ModuleDataAlignment));
    assert(ds->type == DataSource_Extractor);

    auto ex = reinterpret_cast<Extractor *>(ds->d);

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
                u64  address = extract(&ex->filter, MultiWordFilter::CacheA);
                double value = static_cast<double>(extract(&ex->filter, MultiWordFilter::CacheD));

                assert(address < static_cast<u64>(ds->output.data.size));

                if (!is_param_valid(ds->output.data[address]))
                {
                    if (!(ex->options & DataSourceOptions::NoAddedRandom))
                        value += RealDist01(ex->rng);

                    ds->output.data[address] = value;
                    ds->hitCounts[address]++;
                }
            }

            clear_completion(&ex->filter);
        }
    }
}

// ListFilterExtractor
ListFilterExtractor make_listfilter_extractor(
    data_filter::ListFilter listFilter,
    u8 repetitions,
    u64 rngSeed,
    DataSourceOptions::opt_t options)
{
    ListFilterExtractor ex = {};

    ex.listFilter = listFilter;
    ex.rng.seed(rngSeed);
    ex.repetitions = repetitions;
    ex.options = options;

    return ex;
}

DataSource make_datasource_listfilter_extractor(
    memory::Arena *arena,
    data_filter::ListFilter listFilter,
    u8 repetitions,
    u64 rngSeed,
    u8 moduleIndex,
    DataSourceOptions::opt_t options)
{
    DataSource result = {};
    result.type = DataSource_ListFilterExtractor;

    auto ex = arena->pushStruct<ListFilterExtractor>();
    *ex = make_listfilter_extractor(listFilter, repetitions, rngSeed, options);
    result.d = ex;

    result.moduleIndex = moduleIndex;

    // This call works as listFilter and repetitionAddressCache have been
    // initialzed at this point.
    size_t addressCount = get_address_count(&result);

    auto databits = get_extract_bits(&listFilter.extractionFilter,
                                     MultiWordFilter::CacheD);

    double upperLimit = std::pow(2.0, databits);

    result.output.data = push_param_vector(arena, addressCount, invalid_param());
    result.output.lowerLimits = push_param_vector(arena, addressCount, 0.0);
    result.output.upperLimits = push_param_vector(arena, addressCount, upperLimit);

    result.hitCounts = push_param_vector(arena, addressCount, 0.0);

    return result;
}

void listfilter_extractor_begin_event(DataSource *ds)
{
    assert(ds->type == DataSource_ListFilterExtractor);
    auto ex = reinterpret_cast<Extractor *>(ds->d);
    invalidate_all(ds->output.data);
}

u32 *listfilter_extractor_process_module_data(DataSource *ds, u32 *data, u32 dataSize)
{
    assert(ds->type == DataSource_ListFilterExtractor);

    u32 *curPtr = data;
    u32 curSize = dataSize;

    auto ex = reinterpret_cast<ListFilterExtractor *>(ds->d);

    const u16 baseAddressBits = get_base_address_bits(ex);
    const u16 repetitionBits  = get_repetition_address_bits(ex);

    assert(ex->repetitions <= (1u << repetitionBits));

    for (u32 rep = 0; rep < ex->repetitions; rep++)
    {
        // Combine input data words and extract address and data values.
        u64 combined = combine(&ex->listFilter, curPtr, curSize);
        curPtr += ex->listFilter.wordCount;
        curSize -= ex->listFilter.wordCount;

        auto result = extract_address_and_value_from_combined(&ex->listFilter, combined);

        //printf("combined=%lx, addr=%lx, data=%lx\n", combined, result.address, result.value);

        if (!result.matched)
            continue;

        u64  address = result.address;
        double value = result.value;

        // Make the address bits from the repetition number contribute to the
        // final address value.
        if (ex->options & DataSourceOptions::RepetitionContributesLowAddressBits)
        {
            address = (address << repetitionBits) | rep;
        }
        else
        {
            address |= (rep << baseAddressBits);
        }

        assert(address < static_cast<u64>(ds->output.data.size));

        if (!is_param_valid(ds->output.data[address]))
        {
            if (!(ex->options & DataSourceOptions::NoAddedRandom))
                value += RealDist01(ex->rng);

            ds->output.data[address] = value;
            ds->hitCounts[address]++;
        }

        if (curPtr >= data + dataSize)
            break;
    }

    return curPtr;
}

/* ===============================================
 * Operators
 * =============================================== */
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

struct CalibrationData_idx
{
    s32 inputIndex;
    double calibFactor;
};

Operator make_calibration_idx(
    Arena *arena,
    PipeVectors input,
    s32 inputIndex,
    double unitMin,
    double unitMax)
{
    assert(inputIndex < input.data.size);

    auto result = make_operator(arena, Operator_Calibration_idx, 1, 1);

    assign_input(&result, input, 0);

    push_output_vectors(arena, &result, 0, 1, unitMin, unitMax);

    auto d = arena->pushStruct<CalibrationData_idx>();
    result.d = d;

    double calibRange = unitMax - unitMin;
    double paramRange = input.upperLimits[inputIndex] - input.lowerLimits[inputIndex];

    d->inputIndex = inputIndex;
    d->calibFactor = calibRange / paramRange;

    return result;
}

void calibration_step_idx(Operator *op)
{
    a2_trace("\n");
    assert(op->inputCount == 1);
    assert(op->outputCount == 1);
    assert(op->outputs[0].size == 1);
    assert(op->type == Operator_Calibration_idx);

    auto d = reinterpret_cast<CalibrationData_idx *>(op->d);

    assert(d->inputIndex < op->inputs[0].size);

    op->outputs[0][0] = calibrate(
        op->inputs[0][d->inputIndex], op->inputLowerLimits[0][d->inputIndex],
        op->outputLowerLimits[0][0], d->calibFactor);

    if (!is_param_valid(op->inputs[0][d->inputIndex]))
    {
        assert(!is_param_valid(op->outputs[0][0]));
    }
}

struct KeepPreviousData
{
    ParamVec previousInput;
    u8 keepValid;
};

struct KeepPreviousData_idx: public KeepPreviousData
{
    s32 inputIndex;
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

void keep_previous_step_idx(Operator *op)
{
    assert(op->inputCount == 1);
    assert(op->outputCount == 1);
    assert(op->outputs[0].size == 1);
    assert(op->type == Operator_KeepPrevious_idx);

    auto d = reinterpret_cast<KeepPreviousData_idx *>(op->d);

    op->outputs[0][0] = d->previousInput[0];

    double in = op->inputs[0][d->inputIndex];

    if (!d->keepValid || is_param_valid(in))
    {
        d->previousInput[0] = in;
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

Operator make_keep_previous_idx(
    memory::Arena *arena, PipeVectors inPipe,
    s32 inputIndex, bool keepValid)
{
    auto result = make_operator(arena, Operator_KeepPrevious_idx, 1, 1);

    auto d = arena->pushStruct<KeepPreviousData_idx>();
    d->previousInput = push_param_vector(arena, 1, invalid_param());
    d->keepValid = keepValid;
    d->inputIndex = inputIndex;
    result.d = d;

    assign_input(&result, inPipe, 0);
    push_output_vectors(arena, &result, 0, 1);

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

struct DifferenceData_idx
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

    result.d = arena->push<DifferenceData_idx>({indexA, indexB});

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

    auto d = reinterpret_cast<DifferenceData_idx *>(op->d);

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
    assert(!std::isnan(thresholds.min));
    assert(!std::isnan(thresholds.max));

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

//
// aggregate_sum
//
Operator make_aggregate_sum(
    Arena *arena,
    PipeVectors input,
    Thresholds thresholds)
{
    a2_trace("thresholds: %lf, %lf\n", thresholds.min, thresholds.max);

    auto result = make_aggregate_op(arena, input, Operator_Aggregate_Sum, thresholds);

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
    auto output = op->outputs[0];
    auto thresholds = *reinterpret_cast<Thresholds *>(op->d);

    double theSum = 0.0;
    bool validSeen = false;

    for (s32 i = 0; i < input.size; i++)
    {
        //a2_trace("i=%d, input[i]=%lf, thresholds.min=%lf, thresholds.max=%lf, is_valid_and_inside()=%d\n",
        //         i, input[i], thresholds.min, thresholds.max, is_valid_and_inside(input[i], thresholds));

        if (is_valid_and_inside(input[i], thresholds))
        {
            theSum += input[i];
            validSeen = true;
        }
    }

    output[0] = validSeen ? theSum : invalid_param();
}

//
// aggregate_multiplicity
//
Operator make_aggregate_multiplicity(
    Arena *arena,
    PipeVectors input,
    Thresholds thresholds)
{
    a2_trace("thresholds: %lf, %lf\n", thresholds.min, thresholds.max);
    auto result = make_aggregate_op(arena, input, Operator_Aggregate_Multiplicity, thresholds);

    result.outputLowerLimits[0][0] = 0.0;
    result.outputUpperLimits[0][0] = input.data.size;

    return result;
}

void aggregate_multiplicity_step(Operator *op)
{
    auto input = op->inputs[0];
    auto output = op->outputs[0];
    auto thresholds = *reinterpret_cast<Thresholds *>(op->d);

    output[0] = 0.0;

    for (s32 i = 0; i < input.size; i++)
    {
        if (is_valid_and_inside(input[i], thresholds))
        {
            output[0]++;
        }
    }
}

//
// aggregate_min
//
Operator make_aggregate_min(
    memory::Arena *arena,
    PipeVectors input,
    Thresholds thresholds)
{
    a2_trace("thresholds: %lf, %lf\n", thresholds.min, thresholds.max);

    auto result = make_aggregate_op(arena, input, Operator_Aggregate_Min, thresholds);

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

void aggregate_min_step(Operator *op)
{
    a2_trace("\n");
    auto input = op->inputs[0];
    auto output = op->outputs[0];
    auto thresholds = *reinterpret_cast<Thresholds *>(op->d);

    double result = invalid_param();

    for (s32 i = 0; i < input.size; i++)
    {
        if (is_valid_and_inside(input[i], thresholds))
        {
            if (!is_param_valid(result))
            {
                double result = std::numeric_limits<double>::max();
            }

            result = std::min(result, input[i]);
        }
    }

    output[0] = result;
}

//
// aggregate_max
//
Operator make_aggregate_max(
    Arena *arena,
    PipeVectors input,
    Thresholds thresholds)
{
    a2_trace("thresholds: %lf, %lf\n", thresholds.min, thresholds.max);
    auto result = make_aggregate_op(arena, input, Operator_Aggregate_Max, thresholds);

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
    auto output = op->outputs[0];
    auto thresholds = *reinterpret_cast<Thresholds *>(op->d);

    double result = invalid_param();

    for (s32 i = 0; i < input.size; i++)
    {
        if (is_valid_and_inside(input[i], thresholds))
        {
            if (!is_param_valid(result))
            {
                result = std::numeric_limits<double>::lowest();
            }

            result = std::max(result, input[i]);
        }
    }

    output[0] = result;
}

//
// aggregate_mean
//

struct SumAndValidCount
{
    double sum;
    u32 validCount;

    inline double mean()
    {
        return sum / static_cast<double>(validCount);
    }
};

inline SumAndValidCount calculate_sum_and_valid_count(ParamVec input, Thresholds thresholds)
{
    SumAndValidCount result = {};

    for (s32 i = 0; i < input.size; i++)
    {
        if (is_valid_and_inside(input[i], thresholds))
        {
            result.sum += input[i];
            result.validCount++;
        }
    }

    return result;
}

// mean = (sum(x for x in input) / validCount)
Operator make_aggregate_mean(
    Arena *arena,
    PipeVectors input,
    Thresholds thresholds)
{
    a2_trace("thresholds: %lf, %lf\n", thresholds.min, thresholds.max);
    auto result = make_aggregate_op(arena, input, Operator_Aggregate_Mean, thresholds);

    double outputLowerLimit = 0.0;
    double outputUpperLimit = 0.0;

    for (s32 i = 0; i < input.data.size; i++)
    {
        auto mm = std::minmax(input.lowerLimits[i], input.upperLimits[i]);

        outputLowerLimit += mm.first;
        outputUpperLimit += mm.second;
    }

    outputLowerLimit /= input.data.size;
    outputUpperLimit /= input.data.size;

    result.outputLowerLimits[0][0] = outputLowerLimit;
    result.outputUpperLimits[0][0] = outputUpperLimit;

    return result;
}

void aggregate_mean_step(Operator *op)
{
    auto input = op->inputs[0];
    auto output = op->outputs[0];
    auto thresholds = *reinterpret_cast<Thresholds *>(op->d);

    auto sv = calculate_sum_and_valid_count(input, thresholds);

    output[0] = sv.validCount ? sv.mean() : invalid_param();
}

//
// aggregate_sigma
//
Operator make_aggregate_sigma(
    memory::Arena *arena,
    PipeVectors input,
    Thresholds thresholds)
{
    a2_trace("thresholds: %lf, %lf\n", thresholds.min, thresholds.max);

    auto result = make_aggregate_op(arena, input, Operator_Aggregate_Sigma, thresholds);

    double llMin = std::numeric_limits<double>::max();
    double ulMax = std::numeric_limits<double>::lowest();

    for (s32 i = 0; i < input.data.size; i++)
    {
        llMin = std::min(llMin, std::min(input.lowerLimits[i], input.upperLimits[i]));
        ulMax = std::max(ulMax, std::max(input.lowerLimits[i], input.upperLimits[i]));
    }

    result.outputLowerLimits[0][0] = 0.0;
    result.outputUpperLimits[0][0] = std::sqrt(ulMax - llMin);

    return result;
}

void aggregate_sigma_step(Operator *op)
{
    a2_trace("\n");
    auto input = op->inputs[0];
    auto output = op->outputs[0];
    auto thresholds = *reinterpret_cast<Thresholds *>(op->d);

    auto sv = calculate_sum_and_valid_count(input, thresholds);
    double mean = sv.mean();
    double sigma = 0.0;

    for (s32 i = 0; i < input.size; i++)
    {
        if (is_valid_and_inside(input[i], thresholds))
        {
            double d = input[i] - mean;
            sigma += d * d;
        }
    }

    if (sv.validCount)
    {
        sigma = std::sqrt(sigma / static_cast<double>(sv.validCount));
        output[0] = sigma;
    }
    else
    {
        output[0] = invalid_param();
    }
}

//
// aggregate_minx
//
Operator make_aggregate_minx(
    memory::Arena *arena,
    PipeVectors input,
    Thresholds thresholds)
{
    a2_trace("thresholds: %lf, %lf\n", thresholds.min, thresholds.max);

    auto result = make_aggregate_op(arena, input, Operator_Aggregate_MinX, thresholds);

    result.outputLowerLimits[0][0] = 0.0;
    result.outputUpperLimits[0][0] = input.data.size;

    return result;
}

void aggregate_minx_step(Operator *op)
{
    a2_trace("\n");
    auto input = op->inputs[0];
    auto output = op->outputs[0];
    auto thresholds = *reinterpret_cast<Thresholds *>(op->d);

    output[0] = invalid_param();
    s32 minIndex = 0;

    for (s32 i = 0; i < input.size; i++)
    {
        if (is_valid_and_inside(input[i], thresholds))
        {
            if (input[i] < input[minIndex] || std::isnan(input[minIndex]))
            {
                minIndex = i;
            }
        }
    }

    if (is_valid_and_inside(input[minIndex], thresholds))
        output[0] = minIndex;
}

//
// aggregate_maxx
//
Operator make_aggregate_maxx(
    memory::Arena *arena,
    PipeVectors input,
    Thresholds thresholds)
{
    a2_trace("thresholds: %lf, %lf\n", thresholds.min, thresholds.max);

    auto result = make_aggregate_op(arena, input, Operator_Aggregate_MaxX, thresholds);

    result.outputLowerLimits[0][0] = 0.0;
    result.outputUpperLimits[0][0] = input.data.size;

    return result;
}

void aggregate_maxx_step(Operator *op)
{
    a2_trace("\n");
    auto input = op->inputs[0];
    auto output = op->outputs[0];
    auto thresholds = *reinterpret_cast<Thresholds *>(op->d);

    output[0] = invalid_param();
    s32 maxIndex = 0;

    for (s32 i = 0; i < input.size; i++)
    {
        if (is_valid_and_inside(input[i], thresholds))
        {
            if (input[i] > input[maxIndex] || std::isnan(input[maxIndex]))
            {
                maxIndex = i;
            }
        }
    }

    if (is_valid_and_inside(input[maxIndex], thresholds))
    {
        output[0] = maxIndex;
    }
}

//
// aggregate_meanx
//
Operator make_aggregate_meanx(
    Arena *arena,
    PipeVectors input,
    Thresholds thresholds)
{
    a2_trace("thresholds: %lf, %lf\n", thresholds.min, thresholds.max);
    auto result = make_aggregate_op(arena, input, Operator_Aggregate_MeanX, thresholds);

    result.outputLowerLimits[0][0] = 0.0;
    result.outputUpperLimits[0][0] = input.data.size;

    return result;
}

/*
 * meanx = sum(A * x) / sum(A)
 * meanx = sum(input[i] * i) / sum(input[i])
 */
struct MeanXResult
{
    double meanx;
    double sumx;
};

inline MeanXResult calculate_meanx(ParamVec input, Thresholds thresholds)
{
    MeanXResult result = {};

    double numerator   = 0.0;
    double denominator = 0.0;
    bool validSeen = false;

    for (s32 x = 0; x < input.size; x++)
    {
        double A = input[x];

        if (is_valid_and_inside(A, thresholds))
        {
            numerator += A * x;
            denominator += A;
            validSeen = true;
        }
    }

    if (validSeen)
    {
        result.meanx = numerator / denominator;
        result.sumx  = denominator;
    }
    else
    {
        result.meanx = result.sumx = invalid_param();
    }

    return result;
}

void aggregate_meanx_step(Operator *op)
{
    a2_trace("\n");
    auto input = op->inputs[0];
    auto output = op->outputs[0];
    auto thresholds = *reinterpret_cast<Thresholds *>(op->d);

    output[0] = calculate_meanx(input, thresholds).meanx;
}

//
// aggregate_sigmax
//
Operator make_aggregate_sigmax(
    memory::Arena *arena,
    PipeVectors input,
    Thresholds thresholds)
{
    a2_trace("thresholds: %lf, %lf\n", thresholds.min, thresholds.max);

    auto result = make_aggregate_op(arena, input, Operator_Aggregate_SigmaX, thresholds);

    result.outputLowerLimits[0][0] = 0.0;
    result.outputUpperLimits[0][0] = input.data.size;

    return result;
}

void aggregate_sigmax_step(Operator *op)
{
    a2_trace("\n");
    auto input = op->inputs[0];
    auto output = op->outputs[0];
    auto thresholds = *reinterpret_cast<Thresholds *>(op->d);

    assert(input.size);
    assert(output.size);
    assert(!std::isnan(thresholds.min));
    assert(!std::isnan(thresholds.max));

    double sigmax = invalid_param();
    auto meanxResult = calculate_meanx(input, thresholds);

    if (is_param_valid(meanxResult.meanx))
    {
        sigmax = 0.0;

        for (s32 x = 0; x < input.size; x++)
        {
            double A = input[x];

            if (is_valid_and_inside(A, thresholds))
            {
                double d = x - meanxResult.meanx;
                d *= d;
                sigmax += d * A;
            }
        }

        sigmax = std::sqrt(sigmax / meanxResult.sumx);
    }

    output[0] = sigmax;
}

//
// range_filter
//
struct RangeFilterData
{
    Thresholds thresholds;
    bool invert;
};


struct RangeFilterData_idx
{
    Thresholds thresholds;
    bool invert;
    s32 inputIndex;
};

Operator make_range_filter(
    Arena *arena,
    PipeVectors input,
    Thresholds thresholds,
    bool invert)
{
    auto result = make_operator(arena, Operator_RangeFilter, 1, 1);

    auto d = arena->push<RangeFilterData>({ thresholds, invert });
    result.d = d;

    assign_input(&result, input, 0);

    push_output_vectors(arena, &result, 0, input.data.size);


    for (s32 pi = 0; pi < input.data.size; pi++)
    {
        if (invert)
        {
            result.outputLowerLimits[0][pi] = input.lowerLimits[pi];
            result.outputUpperLimits[0][pi] = input.upperLimits[pi];
        }
        else
        {
            result.outputLowerLimits[0][pi] = thresholds.min;
            result.outputUpperLimits[0][pi] = thresholds.max;
        }
    }

    return result;
}

Operator make_range_filter_idx(
    Arena *arena,
    PipeVectors input,
    s32 inputIndex,
    Thresholds thresholds,
    bool invert)
{
    assert(0 <= inputIndex && inputIndex < input.data.size);

    auto result = make_operator(arena, Operator_RangeFilter_idx, 1, 1);

    auto d = arena->push<RangeFilterData_idx>({ thresholds, invert, inputIndex });
    result.d = d;

    assign_input(&result, input, 0);

    push_output_vectors(arena, &result, 0, 1);

    if (invert)
    {
        result.outputLowerLimits[0][0] = input.lowerLimits[inputIndex];
        result.outputUpperLimits[0][0] = input.upperLimits[inputIndex];
    }
    else
    {
        result.outputLowerLimits[0][0] = thresholds.min;
        result.outputUpperLimits[0][0] = thresholds.max;
    }

    return result;
}

void range_filter_step(Operator *op)
{
    a2_trace("\n");
    assert(op->inputCount == 1);
    assert(op->outputCount == 1);
    assert(op->inputs[0].size == op->outputs[0].size);
    assert(op->type == Operator_RangeFilter);

    const double invalid_p = invalid_param();
    auto input = op->inputs[0];
    auto output = op->outputs[0];
    auto data = *reinterpret_cast<RangeFilterData *>(op->d);

    if (data.invert)
    {
        for (s32 pi = 0; pi < input.size; pi++)
        {
            if (!in_range(data.thresholds, input[pi]))
            {
                output[pi] = input[pi];
            }
            else
            {
                output[pi] = invalid_p;
            }
        }
    }
    else
    {
        for (s32 pi = 0; pi < input.size; pi++)
        {
            if (in_range(data.thresholds, input[pi]))
            {
                output[pi] = input[pi];
            }
            else
            {
                output[pi] = invalid_p;
            }
        }
    }
}

void range_filter_step_idx(Operator *op)
{
    a2_trace("\n");
    assert(op->inputCount == 1);
    assert(op->outputCount == 1);
    assert(op->outputs[0].size == 1);
    assert(op->type == Operator_RangeFilter_idx);

    auto input = op->inputs[0];
    auto output = op->outputs[0];
    auto data = *reinterpret_cast<RangeFilterData_idx *>(op->d);

    if (data.invert)
    {
        if (!in_range(data.thresholds, input[data.inputIndex]))
        {
            output[0] = input[data.inputIndex];
        }
        else
        {
            output[0] = invalid_param();
        }
    }
    else
    {
        if (in_range(data.thresholds, input[data.inputIndex]))
        {
            output[0] = input[data.inputIndex];
        }
        else
        {
            output[0] = invalid_param();
        }
    }
}

//
// RectFilter
//

struct RectFilterData
{
    Thresholds xThresholds;
    Thresholds yThresholds;
    s32 xIndex;
    s32 yIndex;
    RectFilterOperation filterOp;
};

Operator make_rect_filter(
    memory::Arena *arena,
    PipeVectors xInput,
    PipeVectors yInput,
    s32 xIndex,
    s32 yIndex,
    Thresholds xThresholds,
    Thresholds yThresholds,
    RectFilterOperation filterOp)
{
    assert(0 <= xIndex && xIndex < xInput.data.size);
    assert(0 <= yIndex && yIndex < yInput.data.size);

    auto result = make_operator(arena, Operator_RectFilter, 2, 1);

    auto d = arena->push<RectFilterData>({ xThresholds, yThresholds, xIndex, yIndex, filterOp });
    result.d = d;

    assign_input(&result, xInput, 0);
    assign_input(&result, yInput, 1);

    push_output_vectors(arena, &result, 0, 1);

    return result;
}

void rect_filter_step(Operator *op)
{
    a2_trace("\n");
    assert(op->inputCount == 2);
    assert(op->outputCount == 1);
    assert(op->type == Operator_RectFilter);

    auto xInput = op->inputs[0];
    auto yInput = op->inputs[1];
    auto output = op->outputs[0];
    auto d = reinterpret_cast<RectFilterData *>(op->d);

    double x = xInput[d->xIndex];
    double y = yInput[d->yIndex];

    bool xInside = in_range(d->xThresholds, x);
    bool yInside = in_range(d->yThresholds, y);

    bool valid = (d->filterOp == RectFilterOperation::And
                  ? (xInside && yInside)
                  : (xInside || yInside));

    output[0] = valid ? 0.0 : invalid_param();
}

//
// ConditionFilter
//

struct ConditionFilterData
{
    s32 dataIndex;
    s32 condIndex;
    bool inverted;
};

Operator make_condition_filter(
    memory::Arena *arena,
    PipeVectors dataInput,
    PipeVectors condInput,
    bool inverted,
    s32 dataIndex,
    s32 condIndex)
{
    assert(dataIndex < 0 || dataIndex < dataInput.data.size);
    assert(condIndex < 0 || condIndex < condInput.data.size);

    if (dataIndex >= 0 && condIndex < 0)
    {
        /* Data is a single element, condition an array. Multiple things could
         * be done:
         * 1) Use the dataIndex to index into the condition array if the
         *    condition array is big enough. Otherwise error out.
         * 2) Use the first parameter of the condition array.
         * 3) Error out.
         * This code implements the second version.  */
        assert(condInput.data.size >= 1);
        condIndex = 0;
    }

    auto result = make_operator(arena, Operator_ConditionFilter, 2, 1);

    auto d = arena->push<ConditionFilterData>({ dataIndex, condIndex, inverted });
    result.d = d;

    assign_input(&result, dataInput, 0);
    assign_input(&result, condInput, 1);

    // either the whole input or the selected element
    s32 outSize = (dataIndex < 0 ? dataInput.data.size : 1);

    push_output_vectors(arena, &result, 0, outSize);

    if (dataIndex < 0)
    {
        for (s32 i = 0; i < outSize; i++)
        {
            result.outputLowerLimits[0][i] = dataInput.lowerLimits[i];
            result.outputUpperLimits[0][i] = dataInput.upperLimits[i];
        }
    }
    else
    {
        result.outputLowerLimits[0][0] = dataInput.lowerLimits[dataIndex];
        result.outputUpperLimits[0][0] = dataInput.upperLimits[dataIndex];
    }

    return result;
}

void condition_filter_step(Operator *op)
{
    a2_trace("\n");
    assert(op->inputCount == 2);
    assert(op->outputCount == 1);
    assert(op->type == Operator_ConditionFilter);

    auto dataInput = op->inputs[0];
    auto condInput = op->inputs[1];
    auto output = op->outputs[0];
    auto d = reinterpret_cast<ConditionFilterData *>(op->d);

    if (d->dataIndex < 0)
    {
        // data input is an array
        assert(output.size == dataInput.size);

        for (s32 pi = 0; pi < dataInput.size; pi++)
        {
            /* The index into the condition array can be out of range if the
             * condition array is smaller than the data array. In that case an
             * invalid_param() is used. */
            double condParam = invalid_param();

            if (d->condIndex < 0 && pi < condInput.size)
            {
                condParam = condInput[pi];
            }
            else if (d->condIndex >= 0)
            {
                assert(d->condIndex < condInput.size);
                condParam = condInput[d->condIndex];
            }

            bool condValid = is_param_valid(condParam);

            if (condValid && !d->inverted)
            {
                output[pi] = dataInput[pi];
            }
            else if (!condValid && d->inverted)
            {
                output[pi] = dataInput[pi];
            }
            else
            {
                output[pi] = invalid_param();
            }
        }
    }
    else
    {
        /* Data input is a single value. Condition can be a single value or an
         * array. If it was an array d->condIndex will have been set to 0 in
         * make_condition_filter(). */
        assert(d->dataIndex < dataInput.size);
        assert(d->condIndex < condInput.size);
        assert(output.size == 1);

        double condParam = condInput[d->condIndex];
        bool condValid   = is_param_valid(condParam);

        if (condValid && !d->inverted)
        {
            output[0] = dataInput[d->dataIndex];
        }
        else if (!condValid && d->inverted)
        {
            output[0] = dataInput[d->dataIndex];
        }
        else
        {
            output[0] = invalid_param();
        }
    }
}

/* ===============================================
 * Expression Operator
 * =============================================== */

#if 1
Operator make_expression_operator(
    memory::Arena *arena,
    PipeVectors inPipe,
    const std::string &begin_expr,
    const std::string &step_expr)
{
    auto result = make_operator(arena, Operator_Expression, 1, 1);
    assign_input(&result, inPipe, 0);

    expr_create(arena, &result, begin_expr, step_expr);

    return result;
}

void expression_operator_step(Operator *op)
{
    assert(op->type == Operator_Expression);

    auto d = reinterpret_cast<ExpressionOperatorData *>(op->d);

    /* References to the input and output have been bound in
     * make_expression_operator(). No need to pass anything here, just evaluate
     * the step expression. */
    expr_eval_step(d);
}
#endif

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
    assert(histo);

    /* Instead of calculating the bin and then checking if it under/overflows
     * this code decides by comparing x to the binnings min and max values.
     * This is faster. */

    if (x < histo->binning.min)
    {
#if 0
        cerr << __PRETTY_FUNCTION__
            << " histo=" << histo << ", x < min, x=" << x << ", get_bin=" << get_bin(*histo, x) << endl;
#endif

        assert(get_bin(*histo, x) == Binning::Underflow);
        histo->underflow++;
    }
    else if (x >= histo->binning.min + histo->binning.range)
    {
#if 0
        if (get_bin(*histo, x) != Binning::Overflow)
        {
            cerr << __PRETTY_FUNCTION__
                << " histo=" << histo << ", x >= max, x=" << x << ", get_bin=" << get_bin(*histo, x)
                << ", binning.min=" << histo->binning.min
                << ", binning.range=" << histo->binning.range
                << " => binning.max=" << histo->binning.min + histo->binning.range
                << endl;
        }
#endif

        assert(histo->binning.range == 0.0 || get_bin(*histo, x) == Binning::Overflow);
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

        histo->data[bin]++;
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
        histo->entryCount++;
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

//
// RateMonitor
//

static OperatorType operator_type(RateMonitorType rateMonitorType)
{
    switch (rateMonitorType)
    {
        case RateMonitorType::CounterDifference:
            return Operator_RateMonitor_CounterDifference;

        case RateMonitorType::PrecalculatedRate:
            return Operator_RateMonitor_PrecalculatedRate;

        case RateMonitorType::FlowRate:
            return Operator_RateMonitor_FlowRate;

        InvalidDefaultCase;
    };

    return OperatorTypeCount;
}

struct RateMonitorData
{
    TypedBlock<RateSampler *, s32> samplers;
};

struct RateMonitorData_FlowRate: public RateMonitorData
{
    ParamVec hitCounts;
};

static void debug_samplers(const TypedBlock<RateSampler *, s32> &samplers, const char *prefix)
{
    for (s32 i = 0; i < samplers.size; i++)
    {
        RateSampler *sampler = samplers[i];

        a2_trace("%s: sampler[%d]@%p, rateHistory@%p, capacity=%lu, size=%lu\n",
                prefix,
                i,
                sampler,
                &sampler->rateHistory,
                sampler->rateHistory.capacity(),
                sampler->rateHistory.size()
               );
    }
}

Operator make_rate_monitor(
    memory::Arena *arena,
    PipeVectors inPipe,
    TypedBlock<RateSampler *, s32> samplers,
    RateMonitorType type)
{
    assert(inPipe.data.size == samplers.size);

    //debug_samplers(samplers, "input");

    auto result = make_operator(arena, operator_type(type), 1, 0);
    assign_input(&result, inPipe, 0);

    auto debug_in_out_samplers = [](const auto &inSamplers, const auto &outSamplers)
    {
        assert(inSamplers.size == outSamplers.size);

        //debug_samplers(inSamplers,  "input");
        //debug_samplers(outSamplers, "output");
    };

    switch (type)
    {
        case RateMonitorType::CounterDifference:
        case RateMonitorType::PrecalculatedRate:
            {
                auto d = arena->pushStruct<RateMonitorData>();
                result.d = d;
                d->samplers = push_copy_typed_block(arena, samplers);

                debug_in_out_samplers(samplers, d->samplers);

            } break;

        case RateMonitorType::FlowRate:
            {
                auto d = arena->pushStruct<RateMonitorData_FlowRate>();
                result.d = d;
                //d->samplers = push_copy_typed_block(arena, samplers);
                d->samplers = push_typed_block<RateSampler *, s32>(arena, samplers.size);
                for (s32 i = 0; i < samplers.size; i++)
                {
                    d->samplers[i] = samplers[i];
                }
                d->hitCounts = push_param_vector(arena, samplers.size, 0.0);

                debug_in_out_samplers(samplers, d->samplers);
            } break;
    }

    return result;
}

void rate_monitor_step(Operator *op)
{
    a2_trace("\n");

    s32 maxIdx = op->inputs[0].size;

    switch (op->type)
    {
        case Operator_RateMonitor_PrecalculatedRate:
            {
                auto d = reinterpret_cast<RateMonitorData *>(op->d);

                a2_trace("recording %d precalculated rates\n", maxIdx);

                for (s32 idx = 0; idx < maxIdx; idx++)
                {
                    double value = op->inputs[0][idx];

                    a2_trace_np("  [%d] recording value %lf\n", idx, value);

                    d->samplers[idx]->record_rate(value);
                }
            } break;

        case Operator_RateMonitor_CounterDifference:
            {
                auto d = reinterpret_cast<RateMonitorData *>(op->d);

                a2_trace("recording %d counter differences\n", maxIdx);

                for (s32 idx = 0; idx < maxIdx; idx++)
                {
                    a2_trace_np("  [%d] sampling value %lf, lastValue=%lf, delta=%lf\n",
                                idx, op->inputs[0][idx],
                                d->samplers[idx]->lastValue,
                                op->inputs[0][idx] - d->samplers[idx]->lastValue
                               );

                    d->samplers[idx]->sample(op->inputs[0][idx]);
                }
            } break;

        case Operator_RateMonitor_FlowRate:
            {
                auto d = reinterpret_cast<RateMonitorData_FlowRate *>(op->d);

                a2_trace("incrementing %d hitCounts\n", maxIdx);

                for (s32 idx = 0; idx < maxIdx; idx++)
                {
                    if (is_param_valid(op->inputs[0][idx]))
                    {
                        d->hitCounts[idx]++;
                    }
                }
            } break;

        InvalidDefaultCase;
    };
}

void rate_monitor_sample_flow(Operator *op)
{
    assert(op->type == Operator_RateMonitor_FlowRate);

    auto d = reinterpret_cast<RateMonitorData_FlowRate *>(op->d);

    assert(d->hitCounts.size == d->samplers.size);

    a2_trace("recording %d flow rates\n", d->hitCounts.size);

    for (s32 idx = 0; idx < d->hitCounts.size; idx++)
    {
        auto sampler = d->samplers[idx];
        auto count   = d->hitCounts[idx];

        sampler->sample(count);

        a2_trace_np("  [%d] lastRate=%lf, history size =%lf, history capacity=%lf\n",
                    idx, sampler->lastRate,
                    static_cast<double>(sampler->rateHistory.size()),
                    static_cast<double>(sampler->rateHistory.capacity())
                   );
    }
}

//
// ExportSink
//

Operator make_export_sink(
    memory::Arena *arena,
    const std::string &output_filename,
    int compressionLevel,
    ExportSinkFormat format,
    TypedBlock<PipeVectors, s32> dataInputs
    )
{
    return make_export_sink(
        arena,
        output_filename,
        compressionLevel,
        format,
        dataInputs,
        PipeVectors(),
        -1);
}

Operator make_export_sink(
    memory::Arena *arena,
    const std::string &output_filename,
    int compressionLevel,
    ExportSinkFormat format,
    TypedBlock<PipeVectors, s32> dataInputs,
    PipeVectors condInput,
    s32 condIndex
    )
{
    Operator result = {};

    s32 inputCount = dataInputs.size;

    if (condIndex >= 0)
        inputCount++;

    switch (format)
    {
        case ExportSinkFormat::Full:
            result = make_operator(arena, Operator_ExportSinkFull, inputCount, 0);
            break;
        case ExportSinkFormat::Sparse:
            result = make_operator(arena, Operator_ExportSinkSparse, inputCount, 0);
            break;
    }

    auto d = arena->pushObject<ExportSinkData>();
    result.d = d;

    d->filename         = output_filename;
    d->compressionLevel = compressionLevel;
    d->condIndex        = condIndex;

    // Assign data inputs.
    for (s32 ii = 0; ii < dataInputs.size; ii++)
    {
        assign_input(&result, dataInputs[ii], ii);
    }

    // The optional condition input is the last input. It's only used if
    // condIndex is valid.
    if (condIndex >= 0)
    {
        assign_input(&result, condInput, inputCount - 1);
    }

    return result;
}

static const size_t CompressionBufferSize = 1u << 20;

// TODO: make sure zlib error handling and reporting works. exceptions need to be caught (and maybe wrapped)!

/* NOTE: About error handling in the ExportSink:
 * - std::ofstream by default has exceptions disabled. The method rdstate() can
 *   be used to query the status of the error bits after each operation.
 *
 * - The zstr implementation enables exceptions by default and from looking at
 *   the code the implementation assumes that exceptions stay enabled.
 *
 * > The export sink code enables exceptions both for the low level ofstream
 *   and for the zstr ostream.
 *
 *   All I/O operations must be wrapped in a try/catch block. Additionally any
 *   further I/O operations are only performed if the good() method of the
 *   stream returns true. This means after the first I/O exception is caught no
 *   further attempts at writing to the file are performed.
 */

void export_sink_begin_run(Operator *op, Logger logger)
{
    a2_trace("\n");
    assert(op->type == Operator_ExportSinkFull
           || op->type == Operator_ExportSinkSparse);

    auto d = reinterpret_cast<ExportSinkData *>(op->d);

    d->ostream.reset(new std::ofstream(d->filename, std::ios::binary | std::ios::trunc));

    // Enable ios exceptions for the lowest level output stream. This operation can throw.
    try
    {
        d->ostream->exceptions(std::ios::failbit | std::ios::badbit);

        if (d->compressionLevel != 0)
        {
            d->z_ostream.reset(new zstr::ostream(*d->ostream));
        }

        std::ostringstream ss;
        ss << "File Export: Opened output file " << d->filename;
        logger(ss.str());
    }
    catch (const std::exception &e)
    {
        std::ostringstream ss;
        ss << "File Export: Error opening output file " << d->filename << ": " << e.what();
        logger(ss.str());
        d->setLastError(ss.str());
    }
}

void export_sink_full_step(Operator *op)
{
    a2_trace("\n");
    assert(op->type == Operator_ExportSinkFull);

    auto d = reinterpret_cast<ExportSinkData *>(op->d);

    std::ostream *outp = (d->compressionLevel != 0
                          ? d->z_ostream.get()
                          : d->ostream.get());

    if (!outp) return;

    s32 dataInputCount = op->inputCount;

    // Test the condition input if it's used
    if (d->condIndex >= 0)
    {
        assert(d->condIndex < op->inputs[op->inputCount - 1].size);

        if (!is_param_valid(op->inputs[op->inputCount - 1][d->condIndex]))
            return;

        dataInputCount = op->inputCount - 1;
    }

    try
    {
        if (outp->good())
        {
            for (s32 inputIndex = 0;
                 inputIndex < dataInputCount && outp->good();
                 inputIndex++)
            {
                auto input = op->inputs[inputIndex];
                assert(input.size <= std::numeric_limits<u16>::max());

                size_t bytes = input.size * sizeof(double);

                outp->write(reinterpret_cast<char *>(input.data), bytes);

                d->bytesWritten += bytes;
            }

            d->eventsWritten++;
        }
    }
    catch (const std::exception &e)
    {
        std::ostringstream ss;
        ss << "Error writing to output file " << d->filename << ": " << e.what();
        d->setLastError(ss.str());
    }
}

static size_t write_indexed_parameter_vector(std::ostream &out, const ParamVec &vec)
{
    assert(vec.size >= 0);
    assert(vec.size <= std::numeric_limits<u16>::max());

    size_t bytesWritten = 0;
    u16 validCount = 0;

    for (s32 i = 0; i < vec.size; i++)
    {
        if (is_param_valid(vec[i]))
            validCount++;
    }

    // Write a size prefix and two arrays with length 'validCount', one
    // containing 16-bit index values, the other containing the corresponding
    // parameter values.
    out.write(reinterpret_cast<char *>(&validCount), sizeof(validCount));
    bytesWritten += sizeof(validCount);

    for (u16 i = 0; i < static_cast<u16>(vec.size); i++)
    {
        if (is_param_valid(vec[i]))
        {
            // 16-bit index value
            out.write(reinterpret_cast<char *>(&i), sizeof(i));
            bytesWritten += sizeof(i);
        }
    }

    for (u16 i = 0; i < static_cast<u16>(vec.size); i++)
    {
        if (is_param_valid(vec[i]))
        {
            // 64-bit double value
            out.write(reinterpret_cast<char *>(vec.data + i), sizeof(double));
            bytesWritten += sizeof(double);
        }
    }

    return bytesWritten;
}

void export_sink_sparse_step(Operator *op)
{
    a2_trace("\n");
    assert(op->type == Operator_ExportSinkSparse);

    auto d = reinterpret_cast<ExportSinkData *>(op->d);

    std::ostream *outp = (d->compressionLevel != 0
                          ? d->z_ostream.get()
                          : d->ostream.get());

    if (!outp) return;

    s32 dataInputCount = op->inputCount;

    // Test the condition input if it's used
    if (d->condIndex >= 0)
    {
        assert(d->condIndex < op->inputs[op->inputCount - 1].size);

        if (!is_param_valid(op->inputs[op->inputCount - 1][d->condIndex]))
            return;

        dataInputCount = op->inputCount - 1;
    }

    try
    {
        if (outp->good())
        {
            for (s32 inputIndex = 0;
                 inputIndex < dataInputCount && outp->good();
                 inputIndex++)
            {
                auto input = op->inputs[inputIndex];
                assert(input.size <= std::numeric_limits<u16>::max());

                size_t bytes = write_indexed_parameter_vector(*outp, input);
                d->bytesWritten += bytes;
            }

            d->eventsWritten++;
        }
    }
    catch (const std::exception &e)
    {
        std::ostringstream ss;
        ss << "Error writing to output file " << d->filename << ": " << e.what();
        d->setLastError(ss.str());
    }
}

void export_sink_end_run(Operator *op)
{
    a2_trace("\n");
    assert(op->type == Operator_ExportSinkFull
           || op->type == Operator_ExportSinkSparse);

    auto d = reinterpret_cast<ExportSinkData *>(op->d);

    // The destructors being called as a result of clearing the unique_ptrs
    // should not throw.
    d->z_ostream = {};
    d->ostream   = {};
}

/* ===============================================
 * A2 implementation
 * =============================================== */

struct OperatorFunctions
{
    using StepFunction      = void (*)(Operator *op);
    using BeginRunFunction  = void (*)(Operator *op, Logger logger);
    using EndRunFunction    = void (*)(Operator *op);

    StepFunction step;
    BeginRunFunction begin_run = nullptr;
    EndRunFunction end_run = nullptr;
};

static const OperatorFunctions OperatorTable[OperatorTypeCount] =
{
    [Operator_Calibration] = { calibration_step },
    [Operator_Calibration_sse] = { calibration_sse_step },
    [Operator_Calibration_idx] = { calibration_step_idx },
    [Operator_KeepPrevious] = { keep_previous_step },
    [Operator_KeepPrevious_idx] = { keep_previous_step_idx },
    [Operator_Difference] = { difference_step },
    [Operator_Difference_idx] = { difference_step_idx },
    [Operator_ArrayMap] = { array_map_step },
    [Operator_BinaryEquation] = { binary_equation_step },

    [Operator_H1DSink] = { h1d_sink_step },
    [Operator_H1DSink_idx] = { h1d_sink_step_idx },
    [Operator_H2DSink] = { h2d_sink_step },

    [Operator_RateMonitor_PrecalculatedRate] = { rate_monitor_step },
    [Operator_RateMonitor_CounterDifference] = { rate_monitor_step },
    [Operator_RateMonitor_FlowRate] = { rate_monitor_step },

    [Operator_ExportSinkFull]   = { export_sink_full_step,   export_sink_begin_run, export_sink_end_run },
    [Operator_ExportSinkSparse] = { export_sink_sparse_step, export_sink_begin_run, export_sink_end_run },

    [Operator_RangeFilter] = { range_filter_step },
    [Operator_RangeFilter_idx] = { range_filter_step_idx },
    [Operator_RectFilter] = { rect_filter_step },
    [Operator_ConditionFilter] = { condition_filter_step },

    [Operator_Aggregate_Sum] = { aggregate_sum_step },
    [Operator_Aggregate_Multiplicity] = { aggregate_multiplicity_step },

    [Operator_Aggregate_Min] = { aggregate_min_step },
    [Operator_Aggregate_Max] = { aggregate_max_step },
    [Operator_Aggregate_Mean] = { aggregate_mean_step },
    [Operator_Aggregate_Sigma] = { aggregate_sigma_step },

    [Operator_Aggregate_MinX] = { aggregate_minx_step },
    [Operator_Aggregate_MaxX] = { aggregate_maxx_step },
    [Operator_Aggregate_MeanX] = { aggregate_meanx_step },
    [Operator_Aggregate_SigmaX] = { aggregate_sigmax_step },

#if 1
    [Operator_Expression] = { expression_operator_step },
#else
    [Operator_Expression] = { nullptr },
#endif
};

inline void step_operator(Operator *op)
{
    OperatorTable[op->type].step(op);
}

A2 make_a2(
    Arena *arena,
    std::initializer_list<u8> dataSourceCounts,
    std::initializer_list<u8> operatorCounts)
{
    assert(dataSourceCounts.size() < MaxVMEEvents);
    assert(operatorCounts.size() < MaxVMEEvents);

    A2 result = {};

    result.dataSourceCounts.fill(0);
    result.dataSources.fill(nullptr);
    result.operatorCounts.fill(0);
    result.operators.fill(nullptr);
    result.operatorRanks.fill(0);

    const u8 *ec = dataSourceCounts.begin();

    for (size_t ei = 0; ei < dataSourceCounts.size(); ++ei, ++ec)
    {
        //printf("%s: %lu -> %u\n", __PRETTY_FUNCTION__, ei, (u32)*ec);
        result.dataSources[ei] = arena->pushArray<DataSource>(*ec);
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

    int srcCount = a2->dataSourceCounts[eventIndex];

    a2_trace("ei=%d, dataSources=%d\n", eventIndex, srcCount);

    for (int srcIdx = 0; srcIdx < srcCount; srcIdx++)
    {
        DataSource *ds = a2->dataSources[eventIndex] + srcIdx;

        switch (static_cast<DataSourceType>(ds->type))
        {
            case DataSource_Extractor:
                extractor_begin_event(ds);
                break;

            case DataSource_ListFilterExtractor:
                listfilter_extractor_begin_event(ds);
                break;
        }
    }
}

// hand module data to all sources for eventIndex and moduleIndex
void a2_process_module_data(A2 *a2, int eventIndex, int moduleIndex, u32 *data, u32 dataSize)
{
    assert(eventIndex < MaxVMEEvents);
    assert(moduleIndex < MaxVMEModules);

#ifndef NDEBUG
    int nprocessed = 0;
#endif

    const int srcCount = a2->dataSourceCounts[eventIndex];

    // State for the data consuming ListFilterExtractors
    u32 *curPtr = data;
    const u32 *endPtr = data + dataSize;

    for (int srcIdx = 0; srcIdx < srcCount; srcIdx++)
    {
        DataSource *ds = a2->dataSources[eventIndex] + srcIdx;

        if (ds->moduleIndex != moduleIndex)
            continue;

        switch (static_cast<DataSourceType>(ds->type))
        {
            case DataSource_Extractor:
                {
                    extractor_process_module_data(ds, data, dataSize);
                } break;
            case DataSource_ListFilterExtractor:
                {
                    if (curPtr < endPtr)
                    {
                        curPtr = listfilter_extractor_process_module_data(ds, curPtr, endPtr - curPtr);
                    }
                } break;
        }
#ifndef NDEBUG
        nprocessed++;
#endif
    }

#ifndef NDEBUG
    a2_trace("ei=%d, mi=%d, processed %d dataSources\n", eventIndex, moduleIndex, nprocessed);
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

struct OperatorRangeWork
{
    Operator *begin = nullptr;
    Operator *end = nullptr;
};

static const s32 WorkQueueSize = 32;

struct OperatorRangeWorkQueue
{
    mpmc_bounded_queue<OperatorRangeWork> queue;
    //NonRecursiveBenaphore mutex;
    LightweightSemaphore taskSem;
    LightweightSemaphore tasksDoneSem;
    //std::atomic<int> tasksDone;

    using Guard = std::lock_guard<NonRecursiveBenaphore>;

    explicit OperatorRangeWorkQueue(size_t size)
        : queue(size)
    {}
};

struct ThreadInfo
{
    int id = 0;
};

OperatorRangeWork dequeue(OperatorRangeWorkQueue *queue, ThreadInfo threadInfo)
{
#if 0
    std::unique_lock<std::mutex> lock(queue->mutex);

    while (queue->queue.empty())
    {
        queue->cv_notEmpty.wait(lock);
    }

    OperatorRangeWork result = queue->queue.front();
    queue->queue.pop();
#else
    OperatorRangeWork result = {};

    for (;;)
    {
        a2_trace("a2 worker %d waiting for taskSem\n",
                 threadInfo.id);

        // block here and try the queue until we get an item
        queue->taskSem.wait();

        a2_trace("a2 worker %d taking the lock\n",
                 threadInfo.id);

        //OperatorRangeWorkQueue::Guard guard(queue->mutex);
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

void a2_worker_loop(OperatorRangeWorkQueue *queue, ThreadInfo threadInfo)
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

u32 step_operator_range_threaded(OperatorRangeWorkQueue *queue, Operator *first, Operator *last)
{

    const s32 opCount = (s32)(last - first);
    s32 tasksQueued = 0;
    s32 opsQueued = 0;

    a2_trace("about to step %d operators\n", opCount);

    {
        // Workers should spin or wait in queue->taskSem.wait() so the rwLock
        // should always be uncontested.
        a2_trace("main a2 worker taking lock before enqueueing work\n");
        //OperatorRangeWorkQueue::Guard guard(queue->mutex);

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
        OperatorRangeWork task = {};
        {
            a2_trace("main a2 worker taking the lock\n");
            // Contend with the workers for the rwLock
            //OperatorRangeWorkQueue::Guard guard(queue->mutex);

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

static OperatorRangeWorkQueue A2WorkQueue(WorkQueueSize);

static std::vector<std::thread> A2Threads = {};

void a2_begin_run(A2 *a2, Logger logger)
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

    // call begin_run functions stored in the OperatorTable
    for (s32 ei = 0; ei < MaxVMEEvents; ei++)
    {
        const int opCount = a2->operatorCounts[ei];

        for (int opIdx = 0; opIdx < opCount; opIdx++)
        {
            Operator *op = a2->operators[ei] + opIdx;

            assert(op);
            assert(op->type < ArrayCount(OperatorTable));

            if (OperatorTable[op->type].begin_run)
            {
                OperatorTable[op->type].begin_run(op, logger);
            }
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
            //OperatorRangeWorkQueue::Guard guard(queue->mutex);
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

    a2_trace("done joining threads\n");

    // call end_run functions stored in the OperatorTable
    for (s32 ei = 0; ei < MaxVMEEvents; ei++)
    {
        const int opCount = a2->operatorCounts[ei];

        for (int opIdx = 0; opIdx < opCount; opIdx++)
        {
            Operator *op = a2->operators[ei] + opIdx;

            assert(op);
            assert(op->type < ArrayCount(OperatorTable));

            if (OperatorTable[op->type].end_run)
            {
                OperatorTable[op->type].end_run(op);
            }
        }
    }

    a2_trace("done");
    fprintf(stderr, "a2::%s() done\n", __FUNCTION__);
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

void a2_timetick(A2 *a2)
{
    a2_trace("\n");

    for (int ei = 0; ei < MaxVMEEvents; ei++)
    {
        const int opCount = a2->operatorCounts[ei];

        for (int opIdx = 0; opIdx < opCount; opIdx++)
        {
            Operator *op = a2->operators[ei] + opIdx;

            assert(op);
            assert(op->type < ArrayCount(OperatorTable));

            if (op->type == Operator_RateMonitor_FlowRate)
            {
                rate_monitor_sample_flow(op);
            }
        }
    }
}

/* Threaded histosink implementation.
-----------------------------------------------------------------------------

Push Histo1DSink or Histo2DSink work on a bounded queue. If the enqueue
blocks it means histo filling is slowing us down.

Somehow bundle more insert work together.

Then try to sort by increasing memory address of insertions.
Sort by histo address, then by bin number.
Start inserting.

Bundling.
-----------------------------

Collect lots of values to insert into the same sink (meaning array of H1D or a
single H2D).  Values are consecutive steps of the sinks input. Always the same
size.

Buffering is always bad in the case of low rates. Could introduce a clock to
the a2 system which would have to be periodically triggered between calls to
a2_begin_event() and a2_end_event(). On clock tick buffers would be flushed.

The clock tick could be set to 1.0s. We'd buffer up data in the system until
the buffers are capped and thus handed off to the workers, or the clock tick
interval has passed and the buffers are flushed.

In single step mode a clock tick would be generated after each step, not
periodically.

For a batch run no ticks would need to be generated until the very end. A
final tick is needed to flush the remaining buffer contents out.
This final tick should probably be done in Analysis::endRun().

If the analysis system takes longer than the set clock interval to process
one event, a single "late" tick is enough to make the event visible in the
histograms. No additional ticks need to be performed.

More bundling and buffering.
-----------------------------

Just collect all the work from all the sinks in a step.

Known at build time, fixed size at runtime:

The number of sinks. The histo addresses. The numbers of histos and their sizes.

Have space for N times the input size of a sink. Reserve that space for all the sinks.
Sort sinks by histo address and assign pointers into the sink buffer in that order.

So for each sink there's a begin pointer, a current pointer, the sinks size
and the constant N.

Sink buffer sizes:
- Histo1DSink: input.size * sizeof(double) * N
- Histo2DSink: 2 * sizeof(double) * N

On a2_end_event()

 */

} // namespace a2
