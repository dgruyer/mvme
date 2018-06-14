#include "analysis_util.h"

#include <QDir>
#include <QJsonDocument>
#include <QMessageBox>

#include "../template_system.h"

namespace analysis
{

QVector<std::shared_ptr<Extractor>> get_default_data_extractors(const QString &moduleTypeName)
{
    QVector<std::shared_ptr<Extractor>> result;

    QDir moduleDir(vats::get_module_path(moduleTypeName));
    QFile filtersFile(moduleDir.filePath("analysis/default_filters.analysis"));

    if (filtersFile.open(QIODevice::ReadOnly))
    {
        auto doc = QJsonDocument::fromJson(filtersFile.readAll());
        Analysis filterAnalysis;
        /* Note: This does not do proper config conversion as no VMEConfig is
         * passed to Analysis::read().  It is assumed that the default filters
         * shipped with mvme are in the latest format (or a format that does
         * not need a VMEConfig to be upconverted). */
        auto readResult = filterAnalysis.read(doc.object()[QSL("AnalysisNG")].toObject());

        if (readResult)
        {
            for (auto source: filterAnalysis.getSources())
            {
                auto extractor = std::dynamic_pointer_cast<Extractor>(source);
                if (extractor)
                {
                    result.push_back(extractor);
                }
            }

            qSort(result.begin(), result.end(), [](const auto &a, const auto &b) {
                return a->objectName() < b->objectName();
            });
        }
        else
        {
            readResult.errorData["Source file"] = filtersFile.fileName();
            QMessageBox::critical(nullptr,
                                  QSL("Error loading default filters"),
                                  readResult.toRichText());
        }
    }

    return result;
}

} // namespace analysis
