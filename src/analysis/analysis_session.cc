#include "analysis/analysis_session.h"

#include <QDir>
#include <QDataStream>
#include <QFile>

#include "analysis/analysis.h"
#include "analysis/analysis_session_p.h"

namespace
{

using namespace analysis;

QJsonObject analysis_to_json(analysis::Analysis *analysis)
{
    QJsonObject result;
    analysis->write(result);
    return result;
}

template<typename T>
QByteArray to_json(T *obj)
{
    QJsonObject json;
    obj->write(json);
    return QJsonDocument(json).toBinaryData();
}

}

namespace analysis
{
namespace detail
{

// Histo1DSink save/load
void save(QDataStream &out, const Histo1DSink *obj)
{
    assert(obj);

    // number of histos
    out << static_cast<s32>(obj->getNumberOfHistos());

    // for each histo: length prefixed raw data
    for (s32 hi = 0; hi < obj->getNumberOfHistos(); hi++)
    {
        if (const auto &histo = obj->getHisto(hi).get())
        {
            out << static_cast<u32>(histo->getNumberOfBins());
            out.writeRawData(reinterpret_cast<const char *>(histo->data()),
                             histo->getNumberOfBins() * sizeof(double));
        }
        else
        {
            out << static_cast<u32>(0u);
        }
    }
}

void load(QDataStream &in, Histo1DSink *obj)
{
    assert(obj);

    s32 savedHistos = 0;
    in >> savedHistos;

    if (savedHistos != obj->getNumberOfHistos())
        throw std::runtime_error("histo count mismatch");

    for (s32 hi = 0; hi < savedHistos; hi++)
    {
        auto histo = obj->m_histos[hi];
        assert(histo);

        u32 binCount = 0;
        in >> binCount;

        if (binCount != histo->getNumberOfBins())
        {
            throw std::runtime_error("1d histo bin mismatch");
        }

        in.readRawData(reinterpret_cast<char *>(histo->data()),
                       binCount * sizeof(double));
    }
}

// Histo2DSink save/load
void save(QDataStream &out, const Histo2DSink *obj)
{
    assert(obj);

    // xBins, yBins, y * x * sizeof(double)

    if (const auto &histo = obj->getHisto().get())
    {
        out << histo->getNumberOfXBins() << histo->getNumberOfYBins();
        out.writeRawData(reinterpret_cast<const char *>(histo->data()),
                         obj->getHistoBinsX() * obj->getHistoBinsY() * sizeof(double));
    }
    else
    {
        out << static_cast<u32>(0u) << static_cast<u32>(0u);
    }
}

void load(QDataStream &in, Histo2DSink *obj)
{
    assert(obj);

    auto histo = obj->getHisto();

    u32 xBins = 0, yBins = 0;

    in >> xBins >> yBins;

    if (xBins != histo->getNumberOfXBins()
        || yBins != histo->getNumberOfYBins())
    {
        throw std::runtime_error("2d histo bin mismatch");
    }

    in.readRawData(reinterpret_cast<char *>(histo->data()),
                    xBins * yBins * sizeof(double));
}

// RateMonitorSink save/load
void save(QDataStream &out, const RateMonitorSink *obj)
{
    assert(obj);

    std::vector<double> buffer;

    // number of rate samplers
    out << static_cast<s32>(obj->rateSamplerCount());

    // for each rate sampler: totalsamples, capacity, used, used * data
    for (s32 si = 0; si < obj->rateSamplerCount(); si++)
    {
        auto sampler = obj->getRateSampler(si);
        u32 capacity = sampler->historyCapacity();
        u32 used = sampler->rateHistory.size();

        buffer.resize(used);
        std::copy(sampler->rateHistory.begin(), sampler->rateHistory.end(),
                  buffer.begin());

        out << sampler->totalSamples << capacity << used;
        out.writeRawData(reinterpret_cast<const char *>(buffer.data()),
                         buffer.size() * sizeof(double));
    }
}

void load(QDataStream &in, RateMonitorSink *obj)
{
    s32 samplerCount = 0;
    in >> samplerCount;

    if (samplerCount != obj->rateSamplerCount())
    {
        throw std::runtime_error("rate sampler count mismatch");
    }

    std::vector<double> buffer;

    for (s32 si = 0; si < samplerCount; si++)
    {
        auto sampler = obj->getRateSampler(si);
        u32 capacity = 0, used = 0;

        in >> sampler->totalSamples >> capacity >> used;

        if (capacity != sampler->historyCapacity())
            throw std::runtime_error("rate sampler capacity mismatch");

        if (used > capacity)
            throw std::runtime_error("rate sampler used exceeds capacity");

        buffer.resize(used);

        in.readRawData(reinterpret_cast<char *>(buffer.data()),
                       buffer.size() * sizeof(double));

        sampler->rateHistory.clear();
        std::copy(buffer.begin(), buffer.end(),
                  std::back_inserter(sampler->rateHistory));
        assert(sampler->rateHistory.size() == buffer.size());
    }
}

} // end namespace detail

//
// save/load vectors of objects
//
template<typename T>
void save_objects(QDataStream &out, const QVector<T *> &objects)
{
    // number of objects followed by custom data for each object
    out << objects.size();

    for (auto obj: objects)
    {
        out << obj->getId();
        detail::save(out, obj);
    }
}

template<typename T>
void load_objects(QDataStream &in, analysis::Analysis *analysis)
{
    s32 objCount = 0;
    in >> objCount;

    while (objCount--)
    {
        QUuid objId;
        in >> objId;

        if (auto dest = qobject_cast<T *>(analysis->getOperator(objId).get()))
        {
            detail::load(in, dest);
        }
    }
}

//
// save
//
QPair<bool, QString> save_analysis_session_io(
    QIODevice &outdev, analysis::Analysis *analysis)
{
    auto sinks = analysis->getSinkOperators();

    // collect objects
    QVector<Histo1DSink *> h1dvec;
    QVector<Histo2DSink *> h2dvec;
    QVector<RateMonitorSink *> rmvec;

    for (auto sink: analysis->getSinkOperators())
    {
        if (auto obj = qobject_cast<Histo1DSink *>(sink.get()))
            h1dvec.push_back(obj);
        else if (auto obj = qobject_cast<Histo2DSink *>(sink.get()))
            h2dvec.push_back(obj);
        else if (auto obj = qobject_cast<RateMonitorSink *>(sink.get()))
            rmvec.push_back(obj);
    }

    // Format:
    //   analysis config    QByteArray
    //   runId              QString
    //   1d histograms      count prefix, then custom data * count
    //   2d histograms      as above
    //   rate monitors      as above
    QDataStream out(&outdev);
    out << to_json(analysis) << analysis->getRunInfo().runId;

    save_objects(out, h1dvec);
    save_objects(out, h2dvec);
    save_objects(out, rmvec);

    return qMakePair(out.status() == QDataStream::Ok,
                     outdev.errorString());
}

//
// load
//
QPair<bool, QString> load_analysis_session_io(
    QIODevice &indev, analysis::Analysis *analysis)
{
    QDataStream in(&indev);

    // skip over the analysis config
    QByteArray rawJson;
    in >> rawJson;

    if (in.status() != QDataStream::Ok)
        return qMakePair(false, indev.errorString());

    // runid
    auto runInfo = analysis->getRunInfo();
    in >> runInfo.runId;
    analysis->setRunInfo(runInfo);

    load_objects<Histo1DSink>(in, analysis);
    load_objects<Histo2DSink>(in, analysis);
    load_objects<RateMonitorSink>(in, analysis);

    return qMakePair(
        in.status() == QDataStream::Ok,
        indev.errorString());
}

//
// load config
//
QPair<QJsonDocument, QString> load_analysis_config_from_session_file_io(
    QIODevice &indev)
{
    QDataStream in(&indev);
    QByteArray rawJson;

    in >> rawJson;

    return qMakePair(
        QJsonDocument::fromBinaryData(rawJson),
        in.status() != QDataStream::Ok ? indev.errorString() : QString());
}

//
// wrappers taking a filename instead of a QIODevice
//
QPair<bool, QString> save_analysis_session(
    const QString &filename, analysis::Analysis *analysis)
{
    try
    {
        QFile out(filename);

        if (!out.open(QIODevice::WriteOnly))
        {
            return qMakePair(false, out.errorString());
        }

        return save_analysis_session_io(out, analysis);
    }
    catch (const std::runtime_error &e)
    {
        return qMakePair(false, QString(e.what()));
    }
}

QPair<bool, QString> load_analysis_session(
    const QString &filename, analysis::Analysis *analysis)
{
    try
    {
        QFile in(filename);

        if (!in.open(QIODevice::ReadOnly))
        {
            return qMakePair(false, in.errorString());
        }

        return load_analysis_session_io(in, analysis);
    }
    catch (const std::runtime_error &e)
    {
        return qMakePair(false, QString(e.what()));
    }
}

QPair<QJsonDocument, QString> load_analysis_config_from_session_file(
    const QString &filename)
{
    try
    {
        QFile in(filename);

        if (!in.open(QIODevice::ReadOnly))
        {
            return qMakePair(QJsonDocument(), in.errorString());
        }

        return load_analysis_config_from_session_file_io(in);
    }
    catch (const std::runtime_error &e)
    {
        return qMakePair(QJsonDocument(), QString(e.what()));
    }
}

} // end namespace analysis
