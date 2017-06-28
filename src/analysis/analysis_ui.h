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
#ifndef __ANALYSIS_UI_H__
#define __ANALYSIS_UI_H__

#include <memory>
#include <QWidget>

class MVMEContext;

namespace analysis
{

class AnalysisWidgetPrivate;
class OperatorInterface;

class AnalysisWidget: public QWidget
{
    Q_OBJECT
    public:
        AnalysisWidget(MVMEContext *ctx, QWidget *parent = 0);
        ~AnalysisWidget();

        void operatorAdded(const std::shared_ptr<OperatorInterface> &op);
        void operatorEdited(const std::shared_ptr<OperatorInterface> &op);

        void updateAddRemoveUserLevelButtons();

        virtual bool event(QEvent *event) override;

    private:
        friend class AnalysisWidgetPrivate;
        AnalysisWidgetPrivate *m_d;

        void eventConfigModified();
};

} // end namespace analysis

#endif /* __ANALYSIS_UI_H__ */
