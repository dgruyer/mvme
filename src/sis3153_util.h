#ifndef __SIS3153_UTIL_H__
#define __SIS3153_UTIL_H__

#include <QWidget>

#include "mvme_context.h"

class SIS3153DebugWidget: public QWidget
{
    Q_OBJECT
    public:
        SIS3153DebugWidget(MVMEContext *context, QWidget *parent = 0);

    private:
        void refresh();
        void resetCounters();

        MVMEContext *m_context;
        QVector<QLabel *> m_labels;
};

#endif /* __SIS3153_UTIL_H__ */
