#ifndef __MVME_CONDITION_UI_H__
#define __MVME_CONDITION_UI_H__

class MVMEContext;

#include <memory>
#include <QWidget>

#include "analysis_fwd.h"

namespace analysis
{
namespace ui
{

class ConditionWidget: public QWidget
{
    Q_OBJECT
    signals:
        void conditionLinkSelected(const ConditionLink &cl);
        void applyConditionAccept();
        void applyConditionReject();
        void editCondition(const ConditionLink &cond);
        void objectSelected(const AnalysisObjectPtr &obj);

    public:
        ConditionWidget(MVMEContext *ctx, QWidget *parent = nullptr);
        virtual ~ConditionWidget() override;

    public slots:
        void repopulate();
        void repopulate(int eventIndex);
        void repopulate(const QUuid &eventId);
        void doPeriodicUpdate();

        void selectEvent(int eventIndex);
        void selectEventById(const QUuid &eventId);
        void clearTreeSelections();
        void clearTreeHighlights();

        void highlightConditionLink(const ConditionLink &cl);
        void setModificationButtonsVisible(const ConditionLink &cl, bool visible);


    private:
        struct Private;
        std::unique_ptr<Private> m_d;
};

} // ns ui
} // ns analysis

#endif /* __MVME_CONDITION_UI_H__ */
