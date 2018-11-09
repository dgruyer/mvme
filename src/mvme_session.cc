#include "mvme_session.h"

#include <QCoreApplication>
#include <QDebug>
#include <QLibraryInfo>
#include <QLocale>

#include "mvme_stream_worker.h"
#include "vme_controller.h"

#ifdef MVME_USE_GIT_VERSION_FILE
#include "git_sha1.h"
#endif
#include "build_info.h"
#include "analysis/analysis_session.h"

void mvme_init(const QString &appName)
{
    qRegisterMetaType<DAQState>("DAQState");
    qRegisterMetaType<GlobalMode>("GlobalMode");
    qRegisterMetaType<MVMEStreamWorkerState>("MVMEStreamWorkerState");
    qRegisterMetaType<ControllerState>("ControllerState");
    qRegisterMetaType<Qt::Axis>("Qt::Axis");

    QCoreApplication::setOrganizationDomain("www.mesytec.com");
    QCoreApplication::setOrganizationName("mesytec");
    QCoreApplication::setApplicationName(appName);
    QCoreApplication::setApplicationVersion(GIT_VERSION);

    QLocale::setDefault(QLocale::c());

    qDebug() << "prefixPath = " << QLibraryInfo::location(QLibraryInfo::PrefixPath);
    qDebug() << "librariesPaths = " << QLibraryInfo::location(QLibraryInfo::LibrariesPath);
    qDebug() << "pluginsPaths = " << QLibraryInfo::location(QLibraryInfo::PluginsPath);
    qDebug() << "GIT_VERSION =" << GIT_VERSION;
    qDebug() << "BUILD_TYPE =" << BUILD_TYPE;
    qDebug() << "BUILD_CXX_FLAGS =" << BUILD_CXX_FLAGS;
}

void mvme_shutdown()
{
    // This used to contain shutdown code for the old, hdf5-based session
    // storage system.
}
