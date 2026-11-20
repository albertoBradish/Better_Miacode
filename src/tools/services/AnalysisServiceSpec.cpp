#include "app/services/AnalysisService.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTextStream>
#include <QTimer>

namespace {

bool expect(bool condition, const QString& message, QTextStream& out)
{
    if (!condition) out << "FAIL: " << message << Qt::endl;
    return condition;
}

QString sourceText()
{
    return QStringLiteral(
        "&first=1.25\n"
        "&lv_5=12\n"
        "&inote_5=(120){4}1,2,\n");
}

bool verifyRevisionStampedAnalysis(QTextStream& out)
{
    miacode::ChartWorkspace workspace;
    const auto opened = workspace.openSource(sourceText());
    const auto first = miacode::AnalysisService::analyze(workspace);
    bool ok = expect(opened.accepted && first.available && first.revision == opened.revision
                         && first.difficultyId == 5 && first.validation.ok
                         && !first.noteMarkers.isEmpty() && !first.noteMarkerSignature.isEmpty(),
                     QStringLiteral("analysis is derived from one identified workspace snapshot"), out);

    const auto edited = workspace.replaceActiveDifficultyChart(QStringLiteral("(120){4}3,"));
    const auto second = miacode::AnalysisService::analyze(workspace);
    ok &= expect(edited.accepted && second.available && second.revision == edited.revision
                     && second.revision != first.revision
                     && second.noteMarkerSignature != first.noteMarkerSignature,
                 QStringLiteral("a newer workspace revision yields a distinguishable analysis snapshot"), out);
    return ok;
}

bool verifyHandOrderOnlyEditInvalidatesPreview(QTextStream& out)
{
    miacode::ChartWorkspace workspace;
    workspace.openSource(QStringLiteral("&lv_5=12\n&inote_5=(120){4}1-5[4:1]~[head=l;track=0:l|0.5:r],\n"));
    const auto leftThenRight = miacode::AnalysisService::analyze(workspace);
    workspace.replaceActiveDifficultyChart(
        QStringLiteral("(120){4}1-5[4:1]~[head=l;track=0:r|0.5:l],"));
    const auto rightThenLeft = miacode::AnalysisService::analyze(workspace);
    return expect(leftThenRight.available && rightThenLeft.available
                      && leftThenRight.noteMarkers.size() == rightThenLeft.noteMarkers.size()
                      && leftThenRight.noteMarkerSignature != rightThenLeft.noteMarkerSignature
                      && leftThenRight.noteMarkers.constFirst().second
                          == rightThenLeft.noteMarkers.constFirst().second,
                  QStringLiteral("annotation-only edits refresh preview marker identity without changing time"), out);
}

bool verifyProductionPublicationDropsStaleWork(QTextStream& out)
{
    miacode::ChartWorkspace workspace;
    miacode::AnalysisService service(workspace);
    QVector<miacode::AnalysisSnapshot> publications;
    QObject::connect(
        &service, &miacode::AnalysisService::snapshotChanged,
        &service, [&] { publications.append(service.snapshot()); });

    const auto opened = workspace.openSource(sourceText());
    const auto edited = workspace.replaceActiveDifficultyChart(QStringLiteral("(120){4}3,"));
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(
        &service, &miacode::AnalysisService::analysisReady,
        &loop, [&](int difficultyId, quint64 revision) {
            if (difficultyId == 5 && revision == edited.revision) loop.quit();
        });
    timeout.start(10000);
    loop.exec();

    const miacode::AnalysisSnapshot current = service.snapshot();
    bool oldAvailable = false;
    bool openedPending = false;
    bool editedPending = false;
    for (const auto& publication : publications) {
        openedPending = openedPending
            || (publication.revision == opened.revision && publication.pending
                && !publication.available);
        editedPending = editedPending
            || (publication.revision == edited.revision && publication.pending
                && !publication.available);
        oldAvailable = oldAvailable
            || (publication.revision == opened.revision && publication.available);
    }
    return expect(opened.accepted && edited.accepted && openedPending && editedPending,
                  QStringLiteral("each workspace identity atomically publishes pending before analysis"), out)
        && expect(!oldAvailable,
                  QStringLiteral("a completed old worker never publishes after a newer identity is requested"), out)
        && expect(current.available && !current.pending && current.difficultyId == 5
                      && current.revision == edited.revision && !current.noteMarkers.isEmpty()
                      && !current.noteMarkerSignature.isEmpty(),
                  QStringLiteral("the latest identity publishes validation, markers, Muri, and static references as one available snapshot"), out);
}

bool verifyPlayAnalysisUsesCurrentProfileAndChart(QTextStream& out)
{
    miacode::ChartWorkspace workspace;
    miacode::AnalysisService service(workspace);
    MuriRenderOptions settings;
    settings.renderMode = RenderMode::SimulatedPlay;
    service.setMuriParameters(settings, -1.0);
    workspace.openSource(QStringLiteral("&first=1.25\n&lv_5=12\n&inote_5=(120){16}8h[4:1]~l,2~r,E\n"));
    const auto edited = workspace.replaceActiveDifficultyChart(QStringLiteral("(120){16}8h[4:1]~l,2~l,E"));
    settings.playProfile.standingDistanceCm = 42;
    service.setMuriParameters(settings, -1.0);
    const auto generation = service.playSnapshot().generation;
    QEventLoop loop;
    bool stale = false;
    QObject::connect(&service, &miacode::AnalysisService::playSnapshotChanged, &loop, [&] {
        const auto& result = service.playSnapshot();
        if (!result.pending) {
            stale |= result.generation != generation || result.revision != edited.revision;
            loop.quit();
        }
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    const auto& current = service.playSnapshot();
    bool found = false;
    for (const auto& issue : current.issues)
        found |= issue.kind == miacode::hand_order::PlayIssueKind::Occupied && qAbs(issue.second - 1.375) < 0.001;
    bool ok = expect(current.enabled && !current.pending && !stale && found,
        QStringLiteral("play diagnostics publish the last settings and chart identity at the offset-adjusted time"), out);
    settings.renderMode = RenderMode::Native;
    service.setMuriParameters(settings, -1.0);
    ok &= expect(!service.playSnapshot().enabled && service.playSnapshot().issues.isEmpty(),
        QStringLiteral("leaving simulated play clears its diagnostic rows"), out);
    return ok;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stderr);
    if (!verifyRevisionStampedAnalysis(out)
        || !verifyHandOrderOnlyEditInvalidatesPreview(out)
        || !verifyProductionPublicationDropsStaleWork(out)
        || !verifyPlayAnalysisUsesCurrentProfileAndChart(out)) return 1;
    QTextStream result(stdout);
    result << "Analysis service checks passed." << Qt::endl;
    return 0;
}
