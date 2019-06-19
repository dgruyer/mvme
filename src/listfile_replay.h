#ifndef __MVME_LISTFILE_REPLAY_H__
#define __MVME_LISTFILE_REPLAY_H__

#include "libmvme_export.h"

#include <memory>
#include <quazip.h>
#include <QDebug>

#include "globals.h"
#include "vme_config.h"

struct LIBMVME_EXPORT ListfileReplayHandle
{
    // The ZIP archive containing the listfile or nullptr if playing directly
    // from a listfile.
    std::unique_ptr<QuaZip> archive;

    // The actual listfile. This is a file inside the archive if replaying from
    // ZIP. As long as this file is open no other file member of the archive
    // can be opened. This is a restriction of the ZIP library.
    // If replaying from flat file this is a plain QFile instance.
    std::unique_ptr<QIODevice> listfile;

    // Format of the data stored in the listfile. Detected by looking at the
    // first 8 bytes of the file. Defaults to the old MVMELST format if none of
    // the newer MVLC types match.
    ListfileBufferFormat format;

    QString inputFilename;      // For ZIP archives this is the name of the ZIP file.
                                // For raw listfiles it's the filename that was
                                // passed to open_listfile().

    QString listfileFilename;   // For ZIP archives it's the name of the
                                // listfile inside the archive. Otherwise the
                                // same as listfileFilename.

    QByteArray messages;        // Contents of messages.log if found
    QByteArray analysisBlob;    // Analysis config contents if present in the archive

    ListfileReplayHandle() = default;
    ~ListfileReplayHandle()
    {
        qDebug() << __PRETTY_FUNCTION__ << this;

        if (listfile)
        {
            qDebug() << "  " << __PRETTY_FUNCTION__ << "closing" << listfile.get();
            listfile->close();
        }
    }

    ListfileReplayHandle(ListfileReplayHandle &&) = default;
    ListfileReplayHandle &operator=(ListfileReplayHandle &&) = default;

    ListfileReplayHandle(const ListfileReplayHandle &) = delete;
    ListfileReplayHandle &operator=(const ListfileReplayHandle &) = delete;
};

// IMPORTANT: throws QString on error :-(
ListfileReplayHandle open_listfile(const QString &filename);

std::pair<std::unique_ptr<VMEConfig>, std::error_code> LIBMVME_EXPORT
    read_vme_config_from_listfile(ListfileReplayHandle &handle);

#endif /* __MVME_LISTFILE_REPLAY_H__ */
