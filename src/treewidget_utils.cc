/* mvme - Mesytec VME Data Acquisition
 *
 * Copyright (C) 2016-2018 mesytec GmbH & Co. KG <info@mesytec.com>
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
#include "treewidget_utils.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QPainter>
#include <QTextDocument>
#include <QDebug>

#include "util.h"

namespace
{

struct DocAndStyleOption
{
    QTextDocument doc;
    QStyleOptionViewItem optionV4;
};

}

struct HtmlDelegate::Private
{
    // Added on the left and right of the text
    static const int ExtraHorizontalMargin = 1;

    void initDocAndStyle(DocAndStyleOption &dos,
                         const QStyleOptionViewItem &opt,
                         const QModelIndex &index)
    {
        dos.optionV4 = opt;
        m_q->initStyleOption(&dos.optionV4, index);

        dos.doc.setDefaultFont(dos.optionV4.font);
        dos.doc.setHtml(dos.optionV4.text);
        dos.doc.setDocumentMargin(1);
    }

    HtmlDelegate *m_q;
};

HtmlDelegate::HtmlDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
    , m_d(std::make_unique<Private>())
{
    m_d->m_q = this;
}

HtmlDelegate::~HtmlDelegate()
{ }

void HtmlDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                         const QModelIndex &index) const
{
    DocAndStyleOption dos;
    m_d->initDocAndStyle(dos, option, index);

    QStyle *style = dos.optionV4.widget? dos.optionV4.widget->style() : QApplication::style();

    // Unset the text and use the style to draw the item icon, checkbox, etc.
    dos.optionV4.text = QString();
    style->drawControl(QStyle::CE_ItemViewItem, &dos.optionV4, painter);

#if 0
    qDebug() << __PRETTY_FUNCTION__ << this
        << "opt.rect.width=" << dos.optionV4.rect.width()
        << ", doc.textWidth=" << dos.doc.textWidth()
        << ", doc.idealWidth=" << dos.doc.idealWidth()
        << ", doc.toPlainText=" << dos.doc.toPlainText()
        ;
#endif

    // Now manually draw the text using the supplied QPainter
    QAbstractTextDocumentLayout::PaintContext ctx;

    // Text highlighting if the item is selected
    if (dos.optionV4.state & QStyle::State_Selected)
    {
        ctx.palette.setColor(QPalette::Text,
                             dos.optionV4.palette.color(QPalette::Active,
                                                        QPalette::HighlightedText));
    }


    QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &dos.optionV4);

    auto topLeft = textRect.topLeft();
    topLeft.rx() += Private::ExtraHorizontalMargin;

    painter->save();
    painter->translate(topLeft);
    painter->setClipRect(textRect.translated(-topLeft));
    dos.doc.documentLayout()->draw(painter, ctx);
    painter->restore();
}

QSize HtmlDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    DocAndStyleOption dos;
    m_d->initDocAndStyle(dos, option, index);

#if 0
    qDebug() << __PRETTY_FUNCTION__ << this
        << "opt.rect.width=" << dos.optionV4.rect.width()
        << ", doc.textWidth=" << dos.doc.textWidth()
        << ", doc.idealWidth=" << dos.doc.idealWidth()
        << ", doc.toPlainText=" << dos.doc.toPlainText()
        ;
#endif

    int width  = dos.doc.idealWidth() + 2 * Private::ExtraHorizontalMargin;
    int height = dos.doc.size().height();

    return QSize(width, height);
}

void CanDisableItemsHtmlDelegate::initStyleOption(QStyleOptionViewItem *option,
                                                  const QModelIndex &index) const
{
    QStyledItemDelegate::initStyleOption(option, index);

    if (auto node = reinterpret_cast<QTreeWidgetItem *>(index.internalPointer()))
    {
        if (m_isItemDisabled && m_isItemDisabled(node))
        {
            option->state &= ~QStyle::State_Enabled;
        }
    }
}

void BasicTreeNode::setData(int column, int role, const QVariant &value)
{
    if (column < 0)
        return;

    if (role != Qt::DisplayRole && role != Qt::EditRole)
    {
        QTreeWidgetItem::setData(column, role, value);
        return;
    }

    if (column >= m_columnData.size())
    {
        m_columnData.resize(column + 1);
    }

    auto &entry = m_columnData[column];

    switch (role)
    {
        case Qt::DisplayRole:
            if (entry.displayData != value)
            {
                entry.displayData = value;
                entry.flags |= Data::HasDisplayData;
                emitDataChanged();
            }
            break;

        case Qt::EditRole:
            if (entry.editData != value)
            {
                entry.editData = value;
                entry.flags |= Data::HasEditData;
                emitDataChanged();
            }
            break;

            InvalidDefaultCase;
    }
}

QVariant BasicTreeNode::data(int column, int role) const
{
    if (role != Qt::DisplayRole && role != Qt::EditRole)
    {
        return QTreeWidgetItem::data(column, role);
    }

    if (0 <= column && column < m_columnData.size())
    {
        const auto &entry = m_columnData[column];

        switch (role)
        {
            case Qt::DisplayRole:
                if (entry.flags & Data::HasDisplayData)
                    return entry.displayData;
                return entry.editData;

            case Qt::EditRole:
                if (entry.flags & Data::HasEditData)
                    return entry.editData;
                return entry.displayData;

                InvalidDefaultCase;
        }
    }

    return QVariant();
}
