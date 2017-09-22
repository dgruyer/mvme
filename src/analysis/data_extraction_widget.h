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
#ifndef __DATA_EXTRACTION_WIDGET_H__
#define __DATA_EXTRACTION_WIDGET_H__

#include "../data_filter.h"

#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QWidget>

namespace analysis
{

class DataExtractionEditor: public QWidget
{
    Q_OBJECT
    public:
        DataExtractionEditor(QWidget *parent = 0);
        DataExtractionEditor(const QVector<DataFilter> &subFilters, QWidget *parent = 0);

        void setSubFilters(const QVector<DataFilter> &subFilters);
        void updateDisplay();
        void apply();

        QVector<DataFilter> m_subFilters;
        QGridLayout *m_filterGrid;

        struct FilterEditElements
        {
            QLineEdit *le_filter;
            QSpinBox *spin_index;
        };

        QVector<FilterEditElements> m_filterEdits;
};

} // end namespace analysis

#endif /* __DATA_EXTRACTION_WIDGET_H__ */
