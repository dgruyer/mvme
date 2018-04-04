#ifndef __EXPORTSINK_CODEGEN_H__
#define __EXPORTSINK_CODEGEN_H__

#include <functional>
#include <memory>
#include <QMap>
#include <QString>

namespace analysis
{

class ExportSink;

class ExportSinkCodeGenerator
{
    public:
        using Logger = std::function<void (const QString &)>;

        ExportSinkCodeGenerator(ExportSink *sink);
        ~ExportSinkCodeGenerator();

        void generateFiles(Logger logger = Logger());
        QMap<QString, QString> generateMap() const;
        QStringList getOutputFilenames() const;

    private:
        struct Private;
        std::unique_ptr<Private> m_d;
};

bool is_valid_identifier(const QString &str);

// Highly sophisticated variable name generation ;-)
QString variablify(QString str);

}

#endif /* __EXPORTSINK_CODEGEN_H__ */
