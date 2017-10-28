#include "a2.h"
#include "a2_impl.h"
#include "data_filter.h"
#include "memory.h"
#include "multiword_datafilter.h"
#include "util/nan.h"
#include "util/sizes.h"

#include <benchmark/benchmark.h>
#include <iostream>
#include <fstream>

using namespace a2;
using namespace data_filter;
using namespace memory;

using std::cout;
using std::endl;
using benchmark::Counter;

#define ArrayCount(x) (sizeof(x) / sizeof(*x))

static void BM_extractor_begin_event(benchmark::State &state)
{
    Arena arena(Kilobytes(256));

    MultiWordFilter filter;

    add_subfilter(&filter, make_filter("xxxx aaaa xxxx dddd"));

    auto ex = arena.push(make_extractor(&arena, filter, 1, 1234, 0));

    assert(ex->output.data.size == (1u << 4));

    double eventsProcessed = 0;

    while (state.KeepRunning())
    {
        extractor_begin_event(ex);
        eventsProcessed++;
        benchmark::DoNotOptimize(ex);
    }

    state.counters["mem"] = Counter(arena.used());
    state.counters["eR"] = Counter(eventsProcessed, Counter::kIsRate);
    //state.counters["eT"] = Counter(eventsProcessed);
}
BENCHMARK(BM_extractor_begin_event);

static void BM_extractor_process_module_data(benchmark::State &state)
{
    static u32 inputData[] =
    {
        0x0001, 0x0102, 0x0203, 0x0304,
        0x040a, 0x050f, 0x060f, 0x070e,
        0x0801, 0x0902, 0x0a03, 0x0b04,
        0x0c0a, 0x0d0f, 0x0e0f, 0x0f0e,
    };

    static const s32 inputSize = ArrayCount(inputData);
    double bytesProcessed = 0;
    double moduleCounter = 0;

    Arena arena(Kilobytes(256));

    MultiWordFilter filter;

    add_subfilter(&filter, make_filter("xxxx aaaa xxxx dddd"));

    auto ex = arena.push(make_extractor(&arena, filter, 1, 1234, 0));

    assert(ex->output.data.size == (1u << 4));

    extractor_begin_event(ex);

#ifndef NDEBUG
    auto cmp = [](double d, double expected)
    {
        return expected <= d && d <= expected + 1.0;
    };
#endif

    while (state.KeepRunning())
    {
        extractor_process_module_data(ex, inputData, inputSize);
        bytesProcessed += sizeof(inputData);
        moduleCounter++;

        for (s32 i = 0; i < inputSize; i++)
        {
            assert(cmp(ex->output.data[i], inputData[i] & 0xf));
        }
    }

    state.counters["mem"] = Counter(arena.used());
    state.counters["bR"] = Counter(bytesProcessed, Counter::kIsRate);
    //state.counters["bT"] = Counter(bytesProcessed);
    state.counters["mR"] = Counter(moduleCounter, Counter::kIsRate);
    //state.counters["mT"] = Counter(moduleCounter);

    //print_param_vector(ex->output);
}
BENCHMARK(BM_extractor_process_module_data);


static void BM_calibration_step(benchmark::State &state)
{
    Arena arena(Kilobytes(256));

    static double inputData[] =
    {
        0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
        8.0, 9.0, 10.0, 11.0, 12.0, invalid_param() /* @[13] */, 14.0, 15.0,
    };
    static const s32 inputSize = ArrayCount(inputData);
    static const s32 invalidIndex = 13;
    double bytesProcessed = 0;
    double moduleCounter = 0;

    auto calib = make_calibration(
        &arena,
        {
            ParamVec{inputData, inputSize},
            push_param_vector(&arena, inputSize, 0.0),
            push_param_vector(&arena, inputSize, 20.0),
        },
        0.0,
        200.0);

    while (state.KeepRunning())
    {
        calibration_step(&calib);
        bytesProcessed += sizeof(inputData);
        moduleCounter++;

        assert(calib.outputCount == 1);
        assert(calib.outputs[0].size == inputSize);
        assert(calib.outputs[0].data[0] == 0.0);
        assert(calib.outputs[0].data[1] == 10.0);
        assert(calib.outputs[0].data[2] == 20.0);
        assert(calib.outputs[0].data[3] == 30.0);
        assert(!is_param_valid(calib.outputs[0].data[invalidIndex]));
    }

    state.counters["mem"] = Counter(arena.used());
    state.counters["bR"] = Counter(bytesProcessed, Counter::kIsRate);
    //state.counters["bT"] = Counter(bytesProcessed);
    state.counters["mR"] = Counter(moduleCounter, Counter::kIsRate);
    //state.counters["mT"] = Counter(moduleCounter);

    //print_param_vector(calib.outputs[0]);
    //std::cout << "arena usage: " << arena.used() << std::endl;
    //std::cout << "inputData: " << inputSize << ", " << sizeof(inputData) << " bytes" << std::endl;
}
BENCHMARK(BM_calibration_step);

static void BM_calibration_SSE2_step(benchmark::State &state)
{
    Arena arena(Kilobytes(256));

    static double inputData[] =
    {
        0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
        8.0, 9.0, 10.0, 11.0, 12.0, invalid_param() /* @[13] */, 14.0, 15.0,
    };
    static const s32 inputSize = ArrayCount(inputData);
    double bytesProcessed = 0;
    double moduleCounter = 0;

    //print_param_vector({inputData, inputSize});

    auto calib = make_calibration(
        &arena,
        {   {inputData, inputSize},
            push_param_vector(&arena, inputSize, 0.0),
            push_param_vector(&arena, inputSize, 20.0),
        },
        0.0,
        200.0);
    calib.type = Operator_Calibration_sse;

    while (state.KeepRunning())
    {
        calibration_sse_step(&calib);
        bytesProcessed += sizeof(inputData);
        moduleCounter++;

        //print_param_vector(calib.outputs[0]);
        assert(calib.outputCount == 1);
        assert(calib.outputs[0].size == inputSize);
        assert(calib.outputs[0].data[0] == 0.0);
        assert(calib.outputs[0].data[1] == 10.0);
        assert(calib.outputs[0].data[2] == 20.0);
        assert(calib.outputs[0].data[3] == 30.0);
        assert(!is_param_valid(calib.outputs[0].data[13]));
    }

    state.counters["mem"] = Counter(arena.used());
    state.counters["bR"] = Counter(bytesProcessed, Counter::kIsRate);
    //state.counters["bT"] = Counter(bytesProcessed);
    state.counters["mR"] = Counter(moduleCounter, Counter::kIsRate);
    //state.counters["mT"] = Counter(moduleCounter);

    //print_param_vector(calib.outputs[0]);
    //std::cout << "arena usage: " << arena.used() << std::endl;
    //std::cout << "inputData: " << inputSize << ", " << sizeof(inputData) << " bytes" << std::endl;
}
BENCHMARK(BM_calibration_SSE2_step);

static void BM_difference_step(benchmark::State &state)
{
    Arena arena(Kilobytes(256));

    static double inputDataA[] =
    {
        0.0, 1.0, 5.0, 10.0, 4.0, 5.0, 6.0, 7.0,
        8.0, 9.0, 10.0, 11.0, 12.0, invalid_param() /* @[13] */, 14.0, 15.0,
    };
    static const s32 inputSize = ArrayCount(inputDataA);
    static const s32 invalidIndex = 13;
    double bytesProcessed = 0;
    double moduleCounter = 0;

    static double inputDataB[inputSize];
#ifndef NDEBUG
    static double resultData[inputSize];
#endif

    for (s32 i = 0; i < inputSize; ++i)
    {
        inputDataB[i] = inputDataA[i] * 2 * (i % 2 == 0 ? 1 : -1);
#ifndef NDEBUG
        resultData[i] = inputDataA[i] - inputDataB[i];
#endif
    }

    PipeVectors inputA;
    inputA.data = ParamVec{inputDataA, inputSize};
    inputA.lowerLimits = push_param_vector(&arena, inputSize, 0.0);
    inputA.upperLimits = push_param_vector(&arena, inputSize, 20.0);

    PipeVectors inputB;
    inputB.data = ParamVec{inputDataB, inputSize};
    inputB.lowerLimits = inputA.lowerLimits;
    inputB.upperLimits = inputA.upperLimits;

    auto diff = make_difference(&arena, inputA, inputB);

    while (state.KeepRunning())
    {
        difference_step(&diff);
        bytesProcessed += sizeof(inputDataA);
        moduleCounter++;

#ifndef NDEBUG
        for (s32 i = 0; i < inputSize; ++i)
        {
            if (i == invalidIndex)
            {
                assert(!is_param_valid(diff.outputs[0][i]));
            }
            else
            {
                assert(diff.outputs[0][i] == resultData[i]);
            }
        }
#endif
    }

    state.counters["mem"] = Counter(arena.used());
    state.counters["bR"] = Counter(bytesProcessed, Counter::kIsRate);
    //state.counters["bT"] = Counter(bytesProcessed);
    state.counters["mR"] = Counter(moduleCounter, Counter::kIsRate);
    //state.counters["mT"] = Counter(moduleCounter);

    //print_param_vector(diff.outputs[0]);
    //std::cout << "arena usage: " << arena.used() << std::endl;
    //std::cout << "inputDataA: " << inputSize << ", " << sizeof(inputDataA) << " bytes" << std::endl;
}
BENCHMARK(BM_difference_step);

static void BM_array_map_step(benchmark::State &state)
{
    // TODO: multi input test
    Arena arena(Kilobytes(256));

    static double inputData[] =
    {
        0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
        8.0, 9.0, 10.0, 11.0, 12.0, invalid_param() /* @[13] */, 14.0, 15.0,
    };
    static const s32 inputSize = ArrayCount(inputData);
    static const s32 invalidIndex = 13;
    double bytesProcessed = 0;
    double moduleCounter = 0;

    static const s32 MappingsCount = inputSize;
    static ArrayMapData::Mapping mappings[MappingsCount];

    for (s32 outIdx = 0; outIdx < inputSize; outIdx++)
    {
        /* [paramOutIndex] = { inputIndex, paramInIndex } */
        mappings[outIdx] = { 0, (inputSize - outIdx - 1) % inputSize};
        //cout << outIdx << " -> " << (u32)mappings[outIdx].inputIndex << ", " << mappings[outIdx].paramIndex << endl;
    }

    PipeVectors input;
    input.data = { inputData, inputSize };
    input.lowerLimits = push_param_vector(&arena, inputSize, 0.0);
    input.upperLimits = push_param_vector(&arena, inputSize, 20.0);

    auto am = make_array_map(
        &arena,
        make_typed_block(&input, 1),
        make_typed_block(mappings, MappingsCount));

    while (state.KeepRunning())
    {
        array_map_step(&am);
        bytesProcessed += sizeof(inputData);
        moduleCounter++;

        for (s32 outIdx = 0; outIdx < am.outputs[0].size; outIdx++)
        {
            assert((am.outputs[0][outIdx] == inputData[(inputSize - outIdx - 1) % inputSize])
                   || (std::isnan(am.outputs[0][outIdx])
                       && std::isnan(inputData[(inputSize - outIdx - 1) % inputSize])));
        }
        //print_param_vector(am.outputs[0]);
    }

    state.counters["mem"] = Counter(arena.used());
    state.counters["bR"] = Counter(bytesProcessed, Counter::kIsRate);
    //state.counters["bT"] = Counter(bytesProcessed);
    state.counters["mR"] = Counter(moduleCounter, Counter::kIsRate);
    //state.counters["mT"] = Counter(moduleCounter);
}
BENCHMARK(BM_array_map_step);

static void BM_keep_previous_step(benchmark::State &state)
{
    static double inputDataSets[2][16] =
    {
        {
            0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
            8.0, 9.0, 10.0, 11.0, 12.0, invalid_param() /* @[13] */, 14.0, 15.0,
        },
    };
    static const s32 inputSize = ArrayCount(inputDataSets[0]);

    for (s32 i = 0; i < inputSize; i++)
    {
        inputDataSets[1][i] = inputDataSets[0][inputSize - i - 1];
    }
    static const s32 invalidIndex = 13;
    double bytesProcessed = 0;
    double moduleCounter = 0;

    Arena arena(Kilobytes(256));

    PipeVectors input;
    input.data = { inputDataSets[0], inputSize };
    input.lowerLimits = push_param_vector(&arena, inputSize, 0.0);
    input.upperLimits = push_param_vector(&arena, inputSize, 20.0);

    // keepValid = false
    auto kp = make_keep_previous(&arena, input, false);

    assert(kp.outputs[0].size == inputSize);

    // step it once using inputDataSets[0] and make sure the output is invalid
    keep_previous_step(&kp);

    for (s32 i = 0; i < kp.outputs[0].size; i++)
    {
        assert(!is_param_valid(kp.outputs[0][i]));
    }

    u32 dataSetIndex = 1;

    while (state.KeepRunning())
    {
        kp.inputs[0].data = inputDataSets[dataSetIndex];

        keep_previous_step(&kp);

        bytesProcessed += sizeof(inputDataSets[0]);
        moduleCounter++;

        dataSetIndex ^= 1u;

        for (s32 i = 0; i < inputSize; i++)
        {
            assert((kp.outputs[0][i] == inputDataSets[dataSetIndex][i])
                   || (!is_param_valid(kp.outputs[0][i])
                       && !is_param_valid(inputDataSets[dataSetIndex][i])));
        }
    }

    state.counters["mem"] = Counter(arena.used());
    state.counters["bR"] = Counter(bytesProcessed, Counter::kIsRate);
    //state.counters["bT"] = Counter(bytesProcessed);
    state.counters["mR"] = Counter(moduleCounter, Counter::kIsRate);
    //state.counters["mT"] = Counter(moduleCounter);
}
BENCHMARK(BM_keep_previous_step);

static void BM_aggregate_sum_step(benchmark::State &state)
{
    Arena arena(Kilobytes(256));

    static double inputData[] =
    {
        0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
        8.0, 9.0, 10.0, 11.0, 12.0, invalid_param() /* @[13] */, 14.0, 15.0,
    };
    static const s32 inputSize = ArrayCount(inputData);
    static const s32 invalidIndex = 13;
    double bytesProcessed = 0;
    double moduleCounter = 0;

    PipeVectors input;
    input.data = { inputData, inputSize };
    input.lowerLimits = push_param_vector(&arena, inputSize, 0.0);
    input.upperLimits = push_param_vector(&arena, inputSize, 20.0);

    Thresholds thresholds = { 0.0, 20.0 };

    auto op = make_aggregate_sum(
        &arena,
        input,
        thresholds);

    assert(op.outputCount == 1);
    assert(op.outputs[0].size == 1);

    double expectedResult = 0.0;

    for (s32 i = 0; i < inputSize; i++)
    {
        if (!std::isnan(inputData[i]))
        {
            expectedResult += inputData[i];
        }
    }

    assert(op.outputLowerLimits[0][0] == 0.0);
    assert(op.outputUpperLimits[0][0] == inputSize * 20.0);

    while (state.KeepRunning())
    {
        aggregate_sum_step(&op);
        bytesProcessed += sizeof(inputData);
        moduleCounter++;

        assert(op.outputs[0][0] == expectedResult);

        //print_param_vector(op.outputs[0]);
    }

    state.counters["mem"] = Counter(arena.used());
    state.counters["bR"] = Counter(bytesProcessed, Counter::kIsRate);
    state.counters["mR"] = Counter(moduleCounter, Counter::kIsRate);
}
BENCHMARK(BM_aggregate_sum_step);

static void BM_aggregate_multiplicity_step(benchmark::State &state)
{
    Arena arena(Kilobytes(256));

    static double inputData[] =
    {
        0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
        8.0, 9.0, 10.0, 11.0, 12.0, invalid_param() /* @[13] */, 14.0, 15.0,
    };
    static const s32 inputSize = ArrayCount(inputData);
    static const s32 invalidIndex = 13;
    double bytesProcessed = 0;
    double moduleCounter = 0;

    PipeVectors input;
    input.data = { inputData, inputSize };
    input.lowerLimits = push_param_vector(&arena, inputSize, 0.0);
    input.upperLimits = push_param_vector(&arena, inputSize, 20.0);

    Thresholds thresholds = { 0.0, 20.0 };

    auto op = make_aggregate_multiplicity(
        &arena,
        input,
        thresholds);

    assert(op.outputCount == 1);
    assert(op.outputs[0].size == 1);

    double expectedResult = inputSize - 1;

    assert(op.outputLowerLimits[0][0] == 0.0);
    assert(op.outputUpperLimits[0][0] == inputSize);

    while (state.KeepRunning())
    {
        aggregate_multiplicity_step(&op);
        bytesProcessed += sizeof(inputData);
        moduleCounter++;

        assert(op.outputs[0][0] == expectedResult);

        //print_param_vector(op.outputs[0]);
    }

    state.counters["mem"] = Counter(arena.used());
    state.counters["bR"] = Counter(bytesProcessed, Counter::kIsRate);
    state.counters["mR"] = Counter(moduleCounter, Counter::kIsRate);
}
BENCHMARK(BM_aggregate_multiplicity_step);

static void BM_aggregate_max_step(benchmark::State &state)
{
    Arena arena(Kilobytes(256));

    static double inputData[] =
    {
        0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
        8.0, 9.0, 10.0, 11.0, 12.0, invalid_param() /* @[13] */, 14.0, 15.0,
    };
    static const s32 inputSize = ArrayCount(inputData);
    static const s32 invalidIndex = 13;
    double bytesProcessed = 0;
    double moduleCounter = 0;

    PipeVectors input;
    input.data = { inputData, inputSize };
    input.lowerLimits = push_param_vector(&arena, inputSize, 0.0);
    input.upperLimits = push_param_vector(&arena, inputSize, 20.0);

    Thresholds thresholds = { 0.0, 20.0 };

    auto op = make_aggregate_max(
        &arena,
        input,
        thresholds);

    assert(op.outputCount == 1);
    assert(op.outputs[0].size == 1);

    double expectedResult = 15;

    assert(op.outputLowerLimits[0][0] == 0.0);
    assert(op.outputUpperLimits[0][0] == 20.0);

    while (state.KeepRunning())
    {
        aggregate_max_step(&op);
        bytesProcessed += sizeof(inputData);
        moduleCounter++;

        assert(op.outputs[0][0] == expectedResult);

        //print_param_vector(op.outputs[0]);
    }

    state.counters["mem"] = Counter(arena.used());
    state.counters["bR"] = Counter(bytesProcessed, Counter::kIsRate);
    state.counters["mR"] = Counter(moduleCounter, Counter::kIsRate);
}
BENCHMARK(BM_aggregate_max_step);

static void BM_h1d_sink_step(benchmark::State &state)
{
    static double inputData[] =
    {
        0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
        8.0, 9.0, 10.0, 11.0, 12.0, invalid_param() /* @[13] */, 14.0, 15.0,
    };
    static const s32 inputSize = ArrayCount(inputData);
    static const s32 invalidIndex = 13;
    double bytesProcessed = 0;
    double moduleCounter = 0;

    static const s32 histoBins = 20;
    static H1D histograms[inputSize];

    Arena histArena(Kilobytes(256));

    for (s32 i=0; i < inputSize; i++)
    {
        histograms[i] = {};
        auto storage = push_param_vector(&histArena, histoBins, 0.0);
        histograms[i].data = storage.data;
        histograms[i].size = storage.size;
        histograms[i].binningFactor = storage.size / 20.0;
        histograms[i].binning.min = 0.0;
        histograms[i].binning.range = 20.0;
    }

    auto histos = make_typed_block(histograms, inputSize);

    Arena arena(Kilobytes(256));

    PipeVectors input;
    input.data = { inputData, inputSize };
    input.lowerLimits = push_param_vector(&arena, inputSize, 0.0);
    input.upperLimits = push_param_vector(&arena, inputSize, 20.0);

    auto sink = make_h1d_sink(&arena, input, histos);
    auto d = reinterpret_cast<H1DSinkData *>(sink.d);

    while (state.KeepRunning())
    {
        h1d_sink_step(&sink);
        bytesProcessed += sizeof(inputData);
        moduleCounter++;


        //for (s32 i = 0; i < inputSize; i++)
        //{
        //    cout << i << " -> " << d->histos[i].entryCount << endl;
        //}
        //print_param_vector(d->histos[0]);
    }

    state.counters["mem"] = Counter(arena.used());
    state.counters["hMem"] = Counter(histArena.used());
    state.counters["bR"] = Counter(bytesProcessed, Counter::kIsRate);
    //state.counters["bT"] = Counter(bytesProcessed);
    state.counters["mR"] = Counter(moduleCounter, Counter::kIsRate);
    //state.counters["mT"] = Counter(moduleCounter);

    if (state.thread_index == 0)
    {
#if 0
        auto histo = d->histos[0];
        printf("h1d data@%p, size=%d, bin.min=%lf, bin.range=%lf, uf=%lf, of=%lf\n",
               histo.data, histo.size,
               histo.binning.min, histo.binning.range,
               histo.underflow, histo.overflow);

        print_param_vector(d->histos[0]);
#endif

        std::ofstream histoOut("h1d_sink_step.histos", std::ios::binary);
        write_histo_list(histoOut, d->histos);
    }
}
BENCHMARK(BM_h1d_sink_step);

#if 0
static void BM_binary_equation_step(benchmark::State &state)
{
    Arena arena(Kilobytes(256));

    static double inputDataA[] =
    {
        0.0, 1.0, 5.0, 10.0, 4.0, 5.0, 6.0, 7.0,
        8.0, 9.0, 10.0, 11.0, 12.0, invalid_param() /* @[13] */, 14.0, 15.0,
    };
    static const s32 inputSize = ArrayCount(inputDataA);
    static const s32 invalidIndex = 13;
    double bytesProcessed = 0;
    double moduleCounter = 0;

    static double inputDataB[inputSize];
#ifndef NDEBUG
    static double resultData[inputSize];
#endif

    for (s32 i = 0; i < inputSize; ++i)
    {
        inputDataB[i] = inputDataA[i] * 2 * (i % 2 == 0 ? 1 : -1);
#ifndef NDEBUG
        resultData[i] = inputDataA[i] - inputDataB[i];
#endif
    }

    PipeVectors inputA;
    inputA.data = ParamVec{inputDataA, inputSize};
    inputA.lowerLimits = push_param_vector(&arena, inputSize, 0.0);
    inputA.upperLimits = push_param_vector(&arena, inputSize, 20.0);

    PipeVectors inputB;
    inputB.data = ParamVec{inputDataB, inputSize};
    inputB.lowerLimits = inputA.lowerLimits;
    inputB.upperLimits = inputA.upperLimits;

    auto diff = make_difference(&arena, inputA, inputB);

    auto op make_binary_equation(
        memory::Arena *arena,
        PipeVectors inputA,
        PipeVectors inputB,
        u32 equationIndex, // stored right inside the d pointer so it can be at least u32 in size
        double outputLowerLimit,
        double outputUpperLimit);

    while (state.KeepRunning())
    {
        difference_step(&diff);
        bytesProcessed += sizeof(inputDataA);
        moduleCounter++;
    }

    state.counters["mem"] = Counter(arena.used());
    state.counters["bR"] = Counter(bytesProcessed, Counter::kIsRate);
    state.counters["mR"] = Counter(moduleCounter, Counter::kIsRate);

    //print_param_vector(diff.outputs[0]);
}
BENCHMARK(BM_binary_equation_step);
#endif

#warning "missing test for binary_equation_step"

BENCHMARK_MAIN();
