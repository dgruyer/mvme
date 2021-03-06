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
#ifndef __MVME_REMOTE_CONTROL_H__
#define __MVME_REMOTE_CONTROL_H__

#include <QHostInfo>
#include <QObject>
#include "mvme_context.h"

namespace remote_control
{

enum ErrorCodes: s32
{
    NotInDAQMode                = 101,
    ReadoutWorkerBusy           = 102,
    AnalysisWorkerBusy          = 103,
    ControllerNotConnected      = 104,

    NoVMEControllerFound        = 201,
};

class RemoteControl: public QObject
{
    Q_OBJECT
    public:
        RemoteControl(MVMEContext *context, QObject *parent = nullptr);
        ~RemoteControl();

        void setListenAddress(const QString &address);
        void setListenPort(int port);

        QString getListenAddress() const;
        int getListenPort() const;

        MVMEContext *getContext() const;

    public slots:
        /** Opens the listening socket and starts accepting clients. */
        void start();
        /** Closes the listening socket. */
        void stop();

    private:
        struct Private;
        std::unique_ptr<Private> m_d;
};

class DAQControlService: public QObject
{
    Q_OBJECT
    public:
        explicit DAQControlService(MVMEContext *context);

    public slots:
        QString getDAQState();
        bool startDAQ();
        bool stopDAQ();
        QString reconnectVMEController();

    private:
        MVMEContext *m_context;
};

class InfoService: public QObject
{
    Q_OBJECT
    public:
        explicit InfoService(MVMEContext *context);

    public slots:
        QString getVersion();
        QStringList getLogMessages();
        QVariantMap getDAQStats();
        QString getVMEControllerType();
        QVariantMap getVMEControllerStats();
        QString getVMEControllerState();

    private:
        MVMEContext *m_context;
};

/* The static method QHostInfo::lookupHost only supports callbacks with the old
 * SLOT syntax. This wrapper class allows passing a std::function object to be
 * used as the completion callback. */
class HostInfoWrapper: public QObject
{
    Q_OBJECT
    public:
        using Callback = std::function<void (const QHostInfo &)>;

        HostInfoWrapper(Callback callback, QObject *parent = nullptr);

        void lookupHost(const QString &name);

    private slots:
        void lookedUp(const QHostInfo &hi);

    private:
        Callback m_callback;
};

} // end namespace remote_control

#endif /* __MVME_REMOTE_CONTROL_H__ */
