#include "histo1d_util.h"
#include <boost/range/adaptor/indexed.hpp>

using boost::adaptors::indexed;

namespace mvme
{

QTextStream &print_histolist_stats(
    QTextStream &out,
    const QVector<std::shared_ptr<Histo1D>> &histos,
    u32 rrf,
    const QString &title)
{
    if (histos.isEmpty())
        return out;

    QVector<Histo1DStatistics> stats;
    stats.reserve(histos.size());

    for (const auto &histo: histos)
    {
        stats.push_back(histo->calcBinStatistics(
                0, histo->getBinCount(), rrf));
    }

    out.setFieldWidth(0);

    const auto &first = histos.at(0);

    out << "# Stats for histogram array '" << title << "'" << endl;
    out << "# Number of histos: " << histos.size()
        << ", bins: " << first->getAxisBinning(Qt::XAxis).getBinCount(rrf)
        << endl;

    out << endl;

    static const int FieldWidth = 14;
    out.setFieldAlignment(QTextStream::AlignLeft);

    out << qSetFieldWidth(FieldWidth)
        << "# HistoIndex" << "EntryCount" << "Mean"
        << "Max" << "MaxPos"
        << "FWHM" << "FWHMPos"
        << "XMin" << "XMax" << "BinWidth"
        << qSetFieldWidth(0) << endl;

    for (const auto &is: stats | indexed(0))
    {
        const auto &index = is.index();
        const auto &stats = is.value();
        const auto &histo = histos[index];

        double maxPos = (stats.entryCount > 0
                         ? histo->getBinCenter(stats.maxBin, rrf)
                         : 0.0);

        out << qSetFieldWidth(FieldWidth)
            << index << stats.entryCount
            << stats.mean
            << stats.maxValue << maxPos
            << stats.fwhm << stats.fwhmCenter
            << histo->getXMin() << histo->getXMax() << histo->getBinWidth(rrf)
            << qSetFieldWidth(0) << endl;
    }

    return out;
}

}
