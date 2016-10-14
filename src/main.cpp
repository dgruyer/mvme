#include "mvme.h"
#include "util.h"
#include "vmusb_stack.h"
#include "mvme_context.h"
#include "vme_controller.h"
#include "daqconfig_tree.h"
#include "mvme_config.h"

#include <QApplication>
#include <QDebug>
#include <QLibraryInfo>

#include <QJsonObject>
#include <QJsonDocument>

int main(int argc, char *argv[])
{
    qRegisterMetaType<DAQState>("DAQState");
    qRegisterMetaType<DAQState>("GlobalMode");
    qRegisterMetaType<DAQState>("ControllerState");
    QApplication a(argc, argv);

    QCoreApplication::setOrganizationDomain("www.mesytec.com");
    QCoreApplication::setOrganizationName("mesytec");
    QCoreApplication::setApplicationName("mvme");
    QCoreApplication::setApplicationVersion("0.2.0");

    qDebug() << "prefixPath = " << QLibraryInfo::location(QLibraryInfo::PrefixPath);
    qDebug() << "librariesPaths = " << QLibraryInfo::location(QLibraryInfo::LibrariesPath);
    qDebug() << "pluginsPaths = " << QLibraryInfo::location(QLibraryInfo::PluginsPath);

    mvme w;
    w.show();
    w.restoreSettings();

    int ret = a.exec();

    return ret;
}
