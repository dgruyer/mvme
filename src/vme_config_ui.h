/* mvme - Mesytec VME Data Acquisition
 *
 * Copyright (C) 2016-2020 mesytec GmbH & Co. KG <info@mesytec.com>
 *
 * Author: Florian Lüke <f.lueke@mesytec.com>
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
#ifndef __CONFIG_WIDGETS_H__
#define __CONFIG_WIDGETS_H__

#include "vme_config.h"
//#include "util.h"
//#include "template_system.h"
#include <QDialog>
#include <memory>


class EventConfig;
class ModuleConfig;
class MVMEContext;
struct EventConfigDialogPrivate;

namespace analysis
{
    class Analysis;
}

namespace Ui
{
    class DataFilterDialog;
    class DualWordDataFilterDialog;
}

class EventConfigDialog: public QDialog
{
    Q_OBJECT
    public:
        EventConfigDialog(
            VMEController *controller,
            EventConfig *config,
            const VMEConfig *vmeConfig,
            QWidget *parent = 0);
        ~EventConfigDialog();

        EventConfig *getConfig() const { return m_config; }

        virtual void accept() override;

    private:
        void loadFromConfig();
        void saveToConfig();
        void setReadOnly(bool readOnly);

        EventConfigDialogPrivate *m_d;
        MVMEContext *m_context;
        VMEController *m_controller;
        EventConfig *m_config;
};

class QComboBox;
class QLineEdit;

// TODO: make members private and mode them into the Private struct
class ModuleConfigDialog: public QDialog
{
    Q_OBJECT
    public:
        ModuleConfigDialog(
            ModuleConfig *module,
            const EventConfig *parentEvent,
            const VMEConfig *vmeConfig,
            QWidget *parent = 0);
        ~ModuleConfigDialog() override;

        ModuleConfig *getModule() const { return m_module; }

        virtual void accept() override;

        QComboBox *typeCombo;
        QLineEdit *nameEdit;
        QLineEdit *addressEdit;

        ModuleConfig *m_module;
        const VMEConfig *m_vmeConfig;
        QVector<vats::VMEModuleMeta> m_moduleMetas;

    private:
        struct Private;
        std::unique_ptr<Private> m_d;
};

QPair<bool, QString> gui_saveAnalysisConfig(analysis::Analysis *analysis_ng,
                                        const QString &fileName,
                                        QString startPath,
                                        QString fileFilter);

QPair<bool, QString> gui_saveAnalysisConfigAs(analysis::Analysis *analysis_ng,
                                          QString startPath,
                                          QString fileFilter);

#endif /* __CONFIG_WIDGETS_H__ */
