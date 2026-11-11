#include "app/runtime/shell/ShellHost.h"

#include "app/runtime/Session.h"
#include "app/runtime/document/DocumentSessionHost.h"
#include "app/runtime/export/VideoExportHost.h"
#include "app/runtime/playback/PlaybackCoordinator.h"

#include "common/CrashRecovery.h"
#include "common/DebugLog.h"
#include "common/DebugOptions.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>

namespace {

constexpr char kReservesPlainSpaceProperty[] = "reservesPlainSpace";

bool objectReservesPlainSpace(QObject* object)
{
    for (QObject* current = object; current != nullptr; current = current->parent()) {
        if (current->property(kReservesPlainSpaceProperty).toBool()) {
            return true;
        }
    }

    auto* item = qobject_cast<QQuickItem*>(object);
    for (QQuickItem* current = item; current != nullptr; current = current->parentItem()) {
        if (current->property(kReservesPlainSpaceProperty).toBool()) {
            return true;
        }
    }
    return false;
}

bool windowReservesPlainSpace(QQuickWindow* window)
{
    return objectReservesPlainSpace(QGuiApplication::focusObject())
        || (window != nullptr && (window->property("sourceEditorFocused").toBool()
                                  || objectReservesPlainSpace(window->activeFocusItem())));
}

}  // namespace

miacode::runtime::ShellHost::ShellHost(::Session& session)
    : session_(session)
{}

void Session::attachRootWindow(QWindow* window)
{
    QCoreApplication::instance()->removeEventFilter(this);

    rootWindow_ = window;

    if (window != nullptr) {
        QCoreApplication::instance()->installEventFilter(this);
    }
}

bool Session::eventFilter(QObject*, QEvent* event)
{
    if (event->type() == QEvent::ApplicationDeactivate) {
        // Alt+Tab eats the Ctrl release, so leaving the app ends the hold too.
        if (editorSyncController_ != nullptr) {
            editorSyncController_->setTouchPadControlHold(false);
        }
        return false;
    }

    if (event->type() == QEvent::ShortcutOverride
        || event->type() == QEvent::KeyPress
        || event->type() == QEvent::KeyRelease) {
        const auto* keyEvent = static_cast<const QKeyEvent*>(event);
        auto* rootWindow = qobject_cast<QQuickWindow*>(rootWindow_.data());
        QWindow* focusWindow = QGuiApplication::focusWindow();
        // The detached preview is an independent top-level window owned by the QML tree.
        const bool focusOnWorkbench = rootWindow != nullptr
            && (focusWindow == rootWindow
                || (focusWindow != nullptr && (focusWindow->transientParent() == rootWindow
                    || focusWindow == rootWindow->findChild<QWindow*>(
                        QStringLiteral("detachedPreviewWindow")))));
        if (focusOnWorkbench
            && keyEvent->key() == Qt::Key_Space
            && keyEvent->modifiers() == Qt::NoModifier
            && rootWindow->property("playbackCommandsEnabled").toBool()
            && !windowReservesPlainSpace(qobject_cast<QQuickWindow*>(focusWindow))) {
            if (event->type() == QEvent::KeyPress
                && !keyEvent->isAutoRepeat()
                && playback_ != nullptr) {
                playback_->togglePlayback();
            }
            event->accept();
            return true;
        }

        // Ctrl (Command on macOS) arms touch-pad authoring on the preview from
        // anywhere in the root window, not only from the focused editor: the
        // click it enables lands on the preview, which does not keep the
        // editor focused. This filter only observes — Ctrl shortcuts still run.
        bool controlArmsTouchAuthoring = false;
        if (keyEvent->key() == Qt::Key_Control
            && !keyEvent->isAutoRepeat()
            && editorSyncController_ != nullptr) {
            if (event->type() == QEvent::KeyPress) {
                controlArmsTouchAuthoring = previewTouchPadAuthoringShortcutEnabled_
                    && focusOnWorkbench
                    && editorAuthoringContextActive();
                if (controlArmsTouchAuthoring) {
                    editorSyncController_->setTouchPadControlHold(true);
                }
            } else if (event->type() == QEvent::KeyRelease) {
                editorSyncController_->setTouchPadControlHold(false);
            }
        }

        if (focusOnWorkbench && rootWindow->property("disableAltMenuActivation").toBool()
            && keyEvent->key() == Qt::Key_Alt) {
            event->accept();
            return true;
        }
    }

    return false;
}

void Session::setRootWindowFrameGeometry(const QRect& geometry)
{
    rootWindowFrameGeometry_ = geometry;
    setProperty("miacode.quick_root_window_frame_geometry", geometry);
}

bool Session::rootWindowFrameGeometryAvailable() const
{
    return rootWindowFrameGeometry_.isValid();
}

QRect Session::rootWindowFrameGeometry() const
{
    return rootWindowFrameGeometry_;
}

void Session::noteRootWindowReady()
{
    noteQuickShellStartupUiReady();
}

void Session::configureRuntimeDebugOutput()
{
    runtimeDebugOutputEnabled_ = miacode::debug_options::runtimeDebugOutputEnabled();
}

void miacode::runtime::ShellHost::requestShellClose(std::function<void(bool)> onDecided)
{
    QElapsedTimer totalTimer;
    totalTimer.start();
    session_.documents_->requestLeaveDocument(
        [this, onDecided = std::move(onDecided), totalTimer](bool canClose) mutable {
            const bool confirmed = canClose && finishShellClose(totalTimer);
            if (onDecided) {
                onDecided(confirmed);
            }
        });
}

bool miacode::runtime::ShellHost::finishShellClose(QElapsedTimer totalTimer)
{
    session_.documents_->cleanupCrashRecoveryForCleanExit();
    miacode::crash_recovery::clearSessionMarker();

    QElapsedTimer savePortableTimer;
    savePortableTimer.start();
    session_.savePortableState();
    miacode::debug_log::appendTimingLine(
        miacode::debug_log::Channel::Runtime,
        QStringLiteral("close_timing/window"),
        QStringLiteral("save_portable_state"),
        savePortableTimer.elapsed());

    QElapsedTimer exportCleanupTimer;
    exportCleanupTimer.start();
    session_.videoExport_->clearVideoExportWorkerState();
    miacode::debug_log::appendTimingLine(
        miacode::debug_log::Channel::Runtime,
        QStringLiteral("close_timing/window"),
        QStringLiteral("clear_video_export_worker_state"),
        exportCleanupTimer.elapsed());

    miacode::debug_log::appendTimingLine(
        miacode::debug_log::Channel::Runtime,
        QStringLiteral("close_timing/window"),
        QStringLiteral("confirm_shell_close"),
        totalTimer.elapsed(),
        QStringLiteral("result=confirmed"));
    return true;
}

void miacode::runtime::ShellHost::appendOutput(const QString& scope, const QString& payload) const
{
    if (!session_.runtimeDebugOutputEnabled_) {
        return;
    }
    miacode::debug_log::appendLine(
        miacode::debug_log::Channel::Runtime,
        scope,
        payload);
}
