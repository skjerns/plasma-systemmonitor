/*
 * SPDX-FileCopyrightText: 2026 skjerns <14980558+skjerns@users.noreply.github.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "WindowPicker.h"

#include <limits>
#include <utility>

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>
#include <QStandardPaths>
#include <QTemporaryFile>

using namespace std::chrono_literals;

constexpr auto KWinService = QLatin1StringView("org.kde.KWin");

static qint64 s_forwardedPid = 0;

WindowPicker::WindowPicker(QObject *parent)
    : QObject(parent)
{
    static int instanceCount = 0;
    m_objectPath = QStringLiteral("/org/kde/plasmasystemmonitor/WindowPicker%1").arg(instanceCount++);

    auto bus = QDBusConnection::sessionBus();
    m_available = bus.interface() && bus.interface()->isServiceRegistered(KWinService).value();
    if (m_available) {
        bus.registerObject(m_objectPath, this, QDBusConnection::ExportScriptableSlots);
    }

    m_scriptTimeout.setSingleShot(true);
    m_scriptTimeout.setInterval(5s);
    connect(&m_scriptTimeout, &QTimer::timeout, this, [this]() {
        qWarning() << "Timed out waiting for KWin to report the process ID of the selected window";
        finish();
        Q_EMIT failed();
    });
}

WindowPicker::~WindowPicker()
{
    unloadScript();
    QDBusConnection::sessionBus().unregisterObject(m_objectPath);
}

bool WindowPicker::isAvailable() const
{
    return m_available;
}

bool WindowPicker::isActive() const
{
    return m_active;
}

void WindowPicker::pick()
{
    if (m_active) {
        return;
    }

    setActive(true);

    auto message = QDBusMessage::createMethodCall(KWinService, QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"), QStringLiteral("queryWindowInfo"));
    // The selection lasts until the user clicks a window or presses Escape, so never time out.
    auto call = QDBusConnection::sessionBus().asyncCall(message, std::numeric_limits<int>::max());
    auto watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
        watcher->deleteLater();

        const QDBusPendingReply<QVariantMap> reply = *watcher;
        if (reply.isError()) {
            finish();
            if (reply.error().name() != QStringLiteral("org.kde.KWin.Error.UserCancel")) {
                qWarning() << "Could not query window information from KWin:" << reply.error().message();
                Q_EMIT failed();
            }
            return;
        }

        const auto info = reply.value();

        // Should KWin ever include the process ID itself, there is no need for a script.
        bool ok = false;
        const auto pid = info.value(QStringLiteral("pid")).toLongLong(&ok);
        if (ok && pid > 0) {
            finish();
            Q_EMIT picked(pid, info);
            return;
        }

        const QUuid uuid(info.value(QStringLiteral("uuid")).toString());
        if (uuid.isNull()) {
            qWarning() << "KWin did not return an ID for the selected window";
            finish();
            Q_EMIT failed();
            return;
        }

        m_windowInfo = info;
        queryPid(uuid);
    });
}

void WindowPicker::forwardToProcessTable(qint64 pid)
{
    s_forwardedPid = pid;
}

qint64 WindowPicker::takeForwardedPid()
{
    return std::exchange(s_forwardedPid, 0);
}

void WindowPicker::windowPidFound(const QString &uuid, const QString &pid)
{
    if (m_pendingUuid.isNull() || QUuid(uuid) != m_pendingUuid) {
        return;
    }

    const auto windowInfo = m_windowInfo;
    finish();

    bool ok = false;
    const auto value = pid.toLongLong(&ok);
    if (ok && value > 0) {
        Q_EMIT picked(value, windowInfo);
    } else {
        qWarning() << "KWin could not find the process ID of window" << uuid;
        Q_EMIT failed();
    }
}

void WindowPicker::queryPid(const QUuid &uuid)
{
    const auto runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    m_scriptFile = std::make_unique<QTemporaryFile>(runtimeDir + QStringLiteral("/plasma-systemmonitor-windowpicker-XXXXXX.js"));
    if (!m_scriptFile->open()) {
        qWarning() << "Could not create KWin script file:" << m_scriptFile->errorString();
        finish();
        Q_EMIT failed();
        return;
    }

    const auto script = QStringLiteral(R"(
const w = workspace.windowList().find(w => w.internalId.toString() === "%1");
callDBus("%2", "%3", "org.kde.plasmasystemmonitor.WindowPicker", "windowPidFound", "%1", w ? String(w.pid) : "");
)")
                            .arg(uuid.toString(), QDBusConnection::sessionBus().baseService(), m_objectPath);
    m_scriptFile->write(script.toUtf8());
    m_scriptFile->close();

    m_pendingUuid = uuid;
    m_scriptName = QStringLiteral("plasma-systemmonitor-windowpicker-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    m_scriptTimeout.start();

    auto message = QDBusMessage::createMethodCall(KWinService, QStringLiteral("/Scripting"), QStringLiteral("org.kde.kwin.Scripting"), QStringLiteral("loadScript"));
    message << m_scriptFile->fileName() << m_scriptName;
    auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, scriptName = m_scriptName](QDBusPendingCallWatcher *watcher) {
        watcher->deleteLater();

        // We may have timed out in the meantime.
        if (scriptName != m_scriptName) {
            return;
        }

        const QDBusPendingReply<int> reply = *watcher;
        if (reply.isError() || reply.value() < 0) {
            qWarning() << "Could not load KWin script:" << reply.error().message();
            finish();
            Q_EMIT failed();
            return;
        }

        auto message = QDBusMessage::createMethodCall(KWinService,
                                                      QStringLiteral("/Scripting/Script%1").arg(reply.value()),
                                                      QStringLiteral("org.kde.kwin.Script"),
                                                      QStringLiteral("run"));
        QDBusConnection::sessionBus().send(message);
    });
}

void WindowPicker::finish()
{
    m_scriptTimeout.stop();
    m_pendingUuid = QUuid();
    m_windowInfo.clear();
    unloadScript();
    setActive(false);
}

void WindowPicker::unloadScript()
{
    if (!m_scriptName.isEmpty()) {
        auto message =
            QDBusMessage::createMethodCall(KWinService, QStringLiteral("/Scripting"), QStringLiteral("org.kde.kwin.Scripting"), QStringLiteral("unloadScript"));
        message << m_scriptName;
        QDBusConnection::sessionBus().send(message);
        m_scriptName.clear();
    }
    m_scriptFile.reset();
}

void WindowPicker::setActive(bool newActive)
{
    if (newActive == m_active) {
        return;
    }
    m_active = newActive;
    Q_EMIT activeChanged();
}

#include "moc_WindowPicker.cpp"
