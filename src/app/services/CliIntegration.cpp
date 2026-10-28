#include "app/services/CliIntegration.h"
#include "app/services/PreferenceDocument.h"
#include "common/RuntimePaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QSaveFile>
#include <QJsonArray>
#include <functional>
#include <vector>
#include <algorithm>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
struct FileState {
    QString path;
    bool exists = false;
    QByteArray bytes;
    QFileDevice::Permissions permissions;
};
bool snapshotFile(const QString& path, FileState* state)
{
    const QFileInfo info(path);
    if (info.isSymLink() || (info.exists() && !info.isFile())) return false;
    *state = {path, info.exists(), {}, info.permissions()};
    if (!state->exists) return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    state->bytes = file.readAll();
    return file.error() == QFileDevice::NoError;
}
bool writeFile(const QString& path, const QByteArray& bytes)
{
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
bool restoreFile(const FileState& state)
{
    if (!state.exists) return !QFileInfo::exists(state.path) || QFile::remove(state.path);
    return writeFile(state.path, state.bytes) && QFile::setPermissions(state.path, state.permissions);
}
struct Transaction {
    std::vector<std::function<bool()>> rollbackActions;
    bool rollback()
    {
        bool ok = true;
        for (auto it = rollbackActions.rbegin(); it != rollbackActions.rend(); ++it) ok = (*it)() && ok;
        rollbackActions.clear();
        return ok;
    }
};
QString quoteShell(QString value)
{
    value.replace('\'', QStringLiteral("'\"'\"'"));
    return '\'' + value + '\'';
}
QString binDirectory(const QString& testDirectory)
{
    if (!testDirectory.isEmpty()) return testDirectory;
#ifdef Q_OS_WIN
    return QDir(miacode::runtime_paths::root()).filePath(QStringLiteral("cli"));
#else
    return QDir::home().filePath(QStringLiteral(".local/bin"));
#endif
}

#ifdef Q_OS_WIN
void announcePathChange()
{
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
        reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 1000, &ignored);
}
bool updateWindowsPath(const QString& bin, bool install, bool* added, Transaction* transaction)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Environment", 0, nullptr, 0,
        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    DWORD type = REG_EXPAND_SZ, size = 0;
    const LSTATUS queried = RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &size);
    const bool existed = queried == ERROR_SUCCESS;
    if ((!existed && queried != ERROR_FILE_NOT_FOUND)
        || (existed && type != REG_SZ && type != REG_EXPAND_SZ)) { RegCloseKey(key); return false; }
    if (!existed) type = REG_EXPAND_SZ;
    if (size % sizeof(wchar_t) != 0) { RegCloseKey(key); return false; }
    QByteArray raw(size, '\0');
    if (existed && RegQueryValueExW(key, L"Path", nullptr, &type,
        reinterpret_cast<BYTE*>(raw.data()), &size) != ERROR_SUCCESS) { RegCloseKey(key); return false; }
    const auto* characters = reinterpret_cast<const wchar_t*>(raw.constData());
    int length = raw.size() / sizeof(wchar_t);
    while (length > 0 && characters[length - 1] == L'\0') --length;
    const QString original = existed ? QString::fromWCharArray(characters, length) : QString();
    auto entries = original.split(';', Qt::KeepEmptyParts);
    const auto matches = [&bin](const QString& value) {
        return QDir::cleanPath(QDir::fromNativeSeparators(value)).compare(
            QDir::cleanPath(bin), Qt::CaseInsensitive) == 0;
    };
    const bool present = std::any_of(entries.cbegin(), entries.cend(), matches);
    QString replacement = original;
    if (install && !present) {
        replacement += (original.isEmpty() ? QString() : QStringLiteral(";"))
            + QDir::toNativeSeparators(bin);
        *added = true;
    } else if (!install && *added) {
        for (qsizetype i = entries.size(); i > 0; --i) {
            if (!matches(entries[i - 1])) continue;
            entries.removeAt(i - 1); // Remove only the entry this installation added.
            break;
        }
        replacement = entries.join(';');
        *added = false;
    }
    if (replacement == original) { RegCloseKey(key); return true; }
    const QByteArray data(reinterpret_cast<const char*>(replacement.utf16()), (replacement.size() + 1) * sizeof(wchar_t));
    const bool written = RegSetValueExW(key, L"Path", 0, type,
        reinterpret_cast<const BYTE*>(data.constData()), data.size()) == ERROR_SUCCESS;
    RegCloseKey(key);
    if (!written) return false;
    transaction->rollbackActions.push_back([raw, type, existed]() {
        HKEY rollbackKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &rollbackKey) != ERROR_SUCCESS) return false;
        const LSTATUS result = existed ? RegSetValueExW(rollbackKey, L"Path", 0, type,
            reinterpret_cast<const BYTE*>(raw.constData()), raw.size()) : RegDeleteValueW(rollbackKey, L"Path");
        RegCloseKey(rollbackKey);
        announcePathChange();
        return result == ERROR_SUCCESS || (!existed && result == ERROR_FILE_NOT_FOUND);
    });
    announcePathChange();
    return true;
}
#endif

bool updateProfiles(const QString& bin, const QString& profileDirectory, bool install,
    bool* added, QJsonArray* ownedProfiles, Transaction* transaction)
{
    const QByteArray block = (QStringLiteral("\n# BEGIN ") + miacode::CliIntegration::commandName()
        + QStringLiteral(" CLI\nexport PATH=") + quoteShell(bin)
        + QStringLiteral(":\"$PATH\"\n# END ") + miacode::CliIntegration::commandName()
        + QStringLiteral(" CLI\n")).toUtf8();
    std::vector<FileState> profiles;
    const bool legacyPair = !install && *added && ownedProfiles->isEmpty();
    // Preflight every target before writing either file. A failed read is an
    // error, never an empty profile to replace.
    QStringList names;
    if (install) {
        names.append(QStringLiteral(".zprofile"));
        QString bashProfile = QStringLiteral(".bash_profile");
        // Bash reads only the first existing login profile. Creating a new
        // .bash_profile must not hide the user's existing .bash_login/.profile.
        for (const QString& candidate : {QStringLiteral(".bash_profile"), QStringLiteral(".bash_login"), QStringLiteral(".profile")}) {
            const QFileInfo info(QDir(profileDirectory).filePath(candidate));
            if (info.exists() || info.isSymLink()) { bashProfile = candidate; break; }
        }
        names.append(bashProfile);
    } else if (legacyPair) {
        names = {QStringLiteral(".zprofile"), QStringLiteral(".bash_profile")};
    } else {
        for (const auto& value : *ownedProfiles) {
            const QString name = value.toString();
            if (name != QStringLiteral(".zprofile") && name != QStringLiteral(".bash_profile")
                && name != QStringLiteral(".bash_login") && name != QStringLiteral(".profile")) return false;
            if (!names.contains(name)) names.append(name);
        }
    }
    for (const QString& name : names) {
        FileState state;
        if (!snapshotFile(QDir(profileDirectory).filePath(name), &state)) return false;
        profiles.push_back(std::move(state));
    }
    // Legacy installations tracked the pair with one boolean.
    if (!install && *added && ownedProfiles->isEmpty())
        for (const auto& profile : profiles) ownedProfiles->append(QFileInfo(profile.path).fileName());
    for (const auto& profile : profiles) {
        const QString name = QFileInfo(profile.path).fileName();
        QByteArray bytes = profile.bytes;
        if (install && !bytes.contains(block)) {
            bytes.append(block);
            if (!ownedProfiles->contains(name)) ownedProfiles->append(name);
        } else if (!install && ownedProfiles->contains(name)) {
            const auto at = bytes.lastIndexOf(block);
            if (at >= 0) bytes.remove(at, block.size());
        }
        if (bytes == profile.bytes) continue;
        transaction->rollbackActions.push_back([profile] { return restoreFile(profile); });
        if (!writeFile(profile.path, bytes)
            || (profile.exists && !QFile::setPermissions(profile.path, profile.permissions))) return false;
    }
    if (!install) *ownedProfiles = {};
    *added = install && !ownedProfiles->isEmpty();
    return true;
}
}

namespace miacode {
CliIntegration::CliIntegration(QObject* parent, QString testDirectory, QString testProfileDirectory)
    : QObject(parent), testDirectory_(std::move(testDirectory)), testProfileDirectory_(std::move(testProfileDirectory)) {}

QString CliIntegration::commandName()
{
    const QString app = QCoreApplication::applicationName();
    return app.contains(QStringLiteral("HandOrder"), Qt::CaseInsensitive)
        || app.contains(QStringLiteral("Sticking"), Qt::CaseInsensitive)
        ? QStringLiteral("miacode-handorder") : QStringLiteral("better-miacode");
}
bool CliIntegration::tasksEnabled()
{
    return PreferenceDocument::loadPreferencesObject().value(QStringLiteral("command_line"))
        .toObject().value(QStringLiteral("enabled")).toBool(false);
}
bool CliIntegration::enabled() const { return tasksEnabled(); }
QString CliIntegration::commandPath() const
{
    QString name = commandName();
#ifdef Q_OS_WIN
    name += QStringLiteral(".cmd");
#endif
    return QDir(binDirectory(testDirectory_)).filePath(name);
}
bool CliIntegration::setEnabled(bool enable)
{
    auto preferences = PreferenceDocument::loadPreferencesObject();
    const auto originalPreferences = preferences;
    auto settings = preferences.value(QStringLiteral("command_line")).toObject();
    const QString path = commandPath();
    FileState previous;
    Transaction transaction;
    const auto fail = [this, &transaction](QString message) {
        if (!transaction.rollback()) message += tr(" Some changes could not be rolled back.");
        status_ = message; emit changed(); return false;
    };
    const bool readable = snapshotFile(path, &previous);
    if (!readable && enable)
        return fail(tr("Cannot safely read the existing command: %1").arg(path));
    const QByteArray owned = settings.value(QStringLiteral("contents")).toString().toUtf8();
    const bool owns = readable && settings.value(QStringLiteral("path")).toString() == path && previous.bytes == owned;
    if (enable && (QFileInfo(path).isSymLink() || (QFileInfo::exists(path) && !owns)))
        return fail(tr("A different command already exists at %1. It was not changed.").arg(path));

    bool pathAdded = settings.value(QStringLiteral("pathAdded")).toBool();
    QJsonArray ownedProfiles = settings.value(QStringLiteral("pathProfiles")).toArray();
    const auto updatePath = [&](bool install) {
        if (!testProfileDirectory_.isEmpty())
            return updateProfiles(QFileInfo(path).absolutePath(), testProfileDirectory_, install, &pathAdded, &ownedProfiles, &transaction);
        if (!testDirectory_.isEmpty()) return true;
#ifdef Q_OS_WIN
        return updateWindowsPath(QFileInfo(path).absolutePath(), install, &pathAdded, &transaction);
#else
        return updateProfiles(QFileInfo(path).absolutePath(), QDir::homePath(), install, &pathAdded, &ownedProfiles, &transaction);
#endif
    };
    if (enable) {
        const QString executable = QCoreApplication::applicationFilePath();
        const QString runtime = runtime_paths::root();
        if (executable.contains('\n') || runtime.contains('\n'))
            return fail(tr("The application path cannot contain a newline."));
        QByteArray content;
#ifdef Q_OS_WIN
        auto batch = [](QString value) { return value.replace('%', QStringLiteral("%%")); };
        const QString runtimeVariable = commandName() == QStringLiteral("miacode-handorder")
            ? QStringLiteral("MIACODE_HAND_ORDER_RUNTIME_ROOT") : QStringLiteral("BETTER_MIACODE_RUNTIME_ROOT");
        content = (QStringLiteral("@echo off\r\nrem Managed by ") + commandName()
            + QStringLiteral("\r\nsetlocal DisableDelayedExpansion\r\nset \"_BETTER_MIACODE_CP=\"\r\n"
                "for /f \"tokens=2 delims=:\" %%C in ('chcp') do for %%D in (%%C) do set \"_BETTER_MIACODE_CP=%%D\"\r\n"
                "chcp 65001 >nul\r\nset \"") + runtimeVariable + '='
            + batch(QDir::toNativeSeparators(runtime)) + QStringLiteral("\"\r\n\"")
            + batch(QDir::toNativeSeparators(executable)) + QStringLiteral("\" %*\r\nset \"_BETTER_MIACODE_EXIT=%errorlevel%\"\r\n"
                "if defined _BETTER_MIACODE_CP chcp %_BETTER_MIACODE_CP% >nul\r\nexit /b %_BETTER_MIACODE_EXIT%\r\n")).toUtf8();
#else
        const QString runtimeVariable = commandName() == QStringLiteral("miacode-handorder")
            ? QStringLiteral("MIACODE_HAND_ORDER_RUNTIME_ROOT") : QStringLiteral("BETTER_MIACODE_RUNTIME_ROOT");
        content = (QStringLiteral("#!/bin/sh\n# Managed by ") + commandName()
            + QStringLiteral("\nexport ") + runtimeVariable + '=' + quoteShell(runtime)
            + QStringLiteral("\nexec ") + quoteShell(executable) + QStringLiteral(" \"$@\"\n")).toUtf8();
#endif
        transaction.rollbackActions.push_back([previous] { return restoreFile(previous); });
        if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !writeFile(path, content))
            return fail(tr("Cannot write the user command: %1").arg(path));
#ifndef Q_OS_WIN
        if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
            | QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther))
            return fail(tr("Cannot make the command executable."));
#endif
        if (!updatePath(true)) return fail(tr("Cannot register the command in the user PATH."));
        settings.insert(QStringLiteral("contents"), QString::fromUtf8(content));
        settings.insert(QStringLiteral("path"), path);
        settings.insert(QStringLiteral("pathAdded"), pathAdded);
        settings.insert(QStringLiteral("pathProfiles"), ownedProfiles);
    } else {
        // A different installation's registration must never be removed.
        if (settings.value(QStringLiteral("path")).toString() == path && pathAdded && !updatePath(false))
            return fail(tr("Cannot remove the user PATH registration."));
        if (owns && previous.exists) {
            transaction.rollbackActions.push_back([previous] { return restoreFile(previous); });
            if (!QFile::remove(path)) return fail(tr("Cannot remove the user command: %1").arg(path));
        }
        settings.remove(QStringLiteral("contents"));
        settings.remove(QStringLiteral("path"));
        settings.remove(QStringLiteral("pathAdded"));
        settings.remove(QStringLiteral("pathProfiles"));
    }
    settings.insert(QStringLiteral("enabled"), enable);
    preferences.insert(QStringLiteral("command_line"), settings);
    if (!PreferenceDocument::savePreferencesObject(preferences)) {
        // Repository::replace also changes its in-memory snapshot on IO failure.
        // Restore that snapshot as well as the external files/registration.
        PreferenceDocument::savePreferencesObject(originalPreferences);
        return fail(tr("The command preference could not be saved."));
    }
    transaction.rollbackActions.clear();
    status_ = enable ? tr("Command installed. Open a new terminal: %1").arg(path) : tr("Command-line tasks disabled.");
    emit changed();
    return true;
}
}
