/*
 * SPDX-FileCopyrightText: 2026 skjerns <14980558+skjerns@users.noreply.github.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <memory>

#include <QObject>
#include <QTimer>
#include <QUuid>
#include <QVariantMap>
#include <qqmlregistration.h>

class QTemporaryFile;

/**
 * Lets the user click on a window and reports the process ID of its owner.
 *
 * This uses KWin's interactive window selection. KWin does not include the
 * process ID in the information it returns, so a small KWin script is used to
 * look it up, which then reports it back to us over D-Bus.
 */
class WindowPicker : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.plasmasystemmonitor.WindowPicker")
    Q_PROPERTY(bool available READ isAvailable CONSTANT)
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)
    QML_ELEMENT

public:
    explicit WindowPicker(QObject *parent = nullptr);
    ~WindowPicker() override;

    /**
     * Whether KWin is running, so picking a window is possible.
     */
    bool isAvailable() const;

    /**
     * Whether we are waiting for the user to pick a window.
     */
    bool isActive() const;
    Q_SIGNAL void activeChanged();

    /**
     * Ask the user to pick a window. Emits picked() or failed() when done.
     */
    Q_INVOKABLE void pick();

    /**
     * Emitted with the process ID of the picked window and KWin's information
     * about it, such as "desktopFile".
     */
    Q_SIGNAL void picked(qint64 pid, const QVariantMap &windowInfo);
    Q_SIGNAL void failed();

    /**
     * Hand a process to the process table, which takes it with takeForwardedPid()
     * once it is shown.
     */
    Q_INVOKABLE void forwardToProcessTable(qint64 pid);
    /**
     * Returns the process passed to forwardToProcessTable(), or 0 if there is none.
     */
    Q_INVOKABLE qint64 takeForwardedPid();

public Q_SLOTS:
    // Called over D-Bus by the KWin script started by queryPid().
    Q_SCRIPTABLE void windowPidFound(const QString &uuid, const QString &pid);

private:
    void queryPid(const QUuid &uuid);
    void finish();
    void unloadScript();
    void setActive(bool newActive);

    bool m_available = false;
    bool m_active = false;
    QString m_objectPath;
    QUuid m_pendingUuid;
    QVariantMap m_windowInfo;
    QString m_scriptName;
    std::unique_ptr<QTemporaryFile> m_scriptFile;
    QTimer m_scriptTimeout;
};
