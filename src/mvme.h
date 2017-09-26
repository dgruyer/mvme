/* mvme - Mesytec VME Data Acquisition
 *
 * Copyright (C) 2016, 2017  Florian Lüke <f.lueke@mesytec.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#ifndef MVME_H
#define MVME_H

#include <QMainWindow>
#include <QMap>

#include "libmvme_export.h"
#include "mvme_context.h"

class ConfigObject;
class VMEConfig;
class VMEConfigTreeWidget;
class DAQControlWidget;
enum class DAQState;
class DAQStatsWidget;
class DataThread;
class Diagnostics;
class EventConfig;
class Hist2D;
class HistogramCollection;
class HistogramTreeWidget;
class ModuleConfig;
class MVMEContextWidget;
class RealtimeData;
class VirtualMod;
class VMEDebugWidget;
class vmedevice;
class VMEScriptConfig;
class WidgetGeometrySaver;

class QMdiSubWindow;
class QPlainTextEdit;
class QThread;
class QTimer;
class QwtPlotCurve;
class QNetworkAccessManager;

class MVMEWindowPrivate;

class LIBMVME_EXPORT MVMEMainWindow : public QMainWindow
{
    Q_OBJECT

public:
    //explicit MVMEMainWindow(MVMEContext *context, QWidget *parent = 0);
    explicit MVMEMainWindow(QWidget *parent = 0);
    ~MVMEMainWindow();

    virtual void closeEvent(QCloseEvent *event);
    void restoreSettings();

    MVMEContext *getContext();

    void addObjectWidget(QWidget *widget, QObject *object, const QString &stateKey);
    bool hasObjectWidget(QObject *object) const;
    QWidget *getObjectWidget(QObject *object) const;
    QList<QWidget *> getObjectWidgets(QObject *object) const;
    void activateObjectWidget(QObject *object);

    void addWidget(QWidget *widget, const QString &stateKey = QString());

public slots:
    void displayAbout();
    void displayAboutQt();
    void clearLog();

    void onActionNewVMEConfig_triggered();
    void onActionOpenVMEConfig_triggered();
    bool onActionSaveVMEConfig_triggered();
    bool onActionSaveVMEConfigAs_triggered();

    void loadConfig(const QString &fileName);

    void onActionNewWorkspace_triggered();
    void onActionOpenWorkspace_triggered();

    bool createNewOrOpenExistingWorkspace();


protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onActionOpenListfile_triggered();
    void onActionCloseListfile_triggered();

    void onActionMainWindow_triggered();
    void onActionAnalysis_UI_triggered();
    void onActionVME_Debug_triggered();
    void onActionLog_Window_triggered();
    void onActionVMUSB_Firmware_Update_triggered();
    void onActionTemplate_Info_triggered();

    void onObjectAboutToBeRemoved(QObject *obj);

    void appendToLog(const QString &);
    void updateWindowTitle();
    void onConfigChanged(VMEConfig *config);

    void onDAQAboutToStart(quint32 nCycles);
    void onDAQStateChanged(const DAQState &);

    void onShowDiagnostics(ModuleConfig *config);
    void onActionImport_Histo1D_triggered();

    void onActionVMEScriptRef_triggered();
    void onActionCheck_for_updates_triggered();
    void onActionToolAnalysisInfo_triggered();

    void updateActions();


private:
    MVMEWindowPrivate *m_d;
};

#endif // MVME_H
