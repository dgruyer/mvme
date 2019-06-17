#ifndef __MVME_CONTEXT_LIB_H__
#define __MVME_CONTEXT_LIB_H__

#include "mvme_listfile_utils.h"
#include "mvme_stream_worker.h"
#include "listfile_replay.h"

class MVMEContext;

struct OpenListfileFlags
{
    static const u16 LoadAnalysis = 1u << 0;
};

/* IMPORTANT: Does not check if the current analysis is modified before loading
 * one from the listfile. Perform this check before calling this function!. */
const ListfileReplayHandle &context_open_listfile(
    MVMEContext *context, const QString &filename, u16 flags = 0);

struct AnalysisPauser
{
    AnalysisPauser(MVMEContext *context);
    ~AnalysisPauser();

    MVMEContext *m_context;
    MVMEStreamWorkerState m_prevState;
};

#endif /* __MVME_CONTEXT_LIB_H__ */
