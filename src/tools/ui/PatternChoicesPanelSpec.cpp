#include "app/services/HandOrderAnalysisService.h"
#include "app/services/EditorSyncController.h"
#include "preview/quick_scene/PatternCandidatePreview.h"
#include "preview/runtime/PreviewRuntime.h"
#include "core/scene/PreviewTrackLayerState.h"
#include <scintillaquick/scintillaquick_item.h>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QTest>
#include <QTextStream>
#include <QTranslator>
#include <QDir>
#include <QFontDatabase>
#include <QSGRendererInterface>
#include <QtPlugin>
#include <QtQml/qqmlextensionplugin.h>
#include <QFile>
#include <QXmlStreamReader>

Q_IMPORT_QML_PLUGIN(MiaCode_PreviewPlugin)

QQuickItem* visualItem(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (auto* child : item->childItems()) if (auto* found = visualItem(child,name)) return found;
    return nullptr;
}
bool visibleText(QQuickItem* item, const QString& text) {
    if (item->isVisible() && item->property("text").toString() == text) return true;
    for (auto* child : item->childItems()) if (visibleText(child,text)) return true;
    return false;
}

class PatternHost final : public QObject {
    Q_OBJECT
public:
    miacode::ChartWorkspace workspace;
    miacode::HandOrderAnalysisService service{workspace};
    ScintillaQuick_item editor;
    int applied = 0;
    miacode::EditorSyncController sync;
    int seeks = 0;
    double playhead = 99;
    QVariantMap target;
    PatternHost() {
        service.setFeaturesEnabled(true);
        connect(&sync, &miacode::EditorSyncController::navigationRequested, this,
            [this](qulonglong sequence, int, qulonglong, int first, int last, bool, bool) {
                editor.send(SCI_SETSEL, text().left(first).toUtf8().size(), text().left(last).toUtf8().size());
                sync.acknowledgeNavigation(sequence,true);
            });
        connect(&sync, &miacode::EditorSyncController::navigationFinished, this,
            [this](qulonglong, bool success) { if (success) sync.seekPreviewToEditorLocation(
                target.value("difficultyId").toInt(),target.value("revision").toULongLong(),
                target.value("line").toInt(),target.value("column").toInt()); });
        connect(&sync, &miacode::EditorSyncController::previewSeekPublished, this,
            [this](int, int, int) { ++seeks; playhead = target.value("second").toDouble(); });
    }
    Q_INVOKABLE void navigate(int group) {
        target = service.patternNavigationTarget(group);
        if (target.isEmpty()) return;
        const int difficulty = target.value("difficultyId").toInt(); const auto revision = target.value("revision").toULongLong();
        sync.setEditorReadiness(difficulty,revision,true);
        sync.requestNavigation(difficulty,revision,target.value("start").toInt(),target.value("end").toInt(),true,true);
    }
    QString text() const { return editor.property("text").toString(); }
    Q_INVOKABLE void apply(int group, int variant) {
        perform(service.takePatternEdit(group,variant,text()));
    }
    Q_INVOKABLE void applySelected(const QStringList& keys,const QString& choice) {
        perform(service.takePatternBatchEdit(keys,choice,text()));
    }
    void perform(const miacode::editor::SimaiTextEditResult& edit) {
        if (!edit.consumed) return;
        const auto& tx = edit.transaction; const auto before = text();
        editor.send(SCI_BEGINUNDOACTION);
        editor.send(SCI_SETTARGETSTART, before.left(tx.replacementStart).toUtf8().size());
        editor.send(SCI_SETTARGETEND, before.left(tx.replacementEnd).toUtf8().size());
        const auto replacement = tx.replacementText.toUtf8();
        editor.sends(SCI_REPLACETARGET, replacement.size(), replacement.constData());
        editor.send(SCI_ENDUNDOACTION);
        workspace.replaceActiveDifficultyChart(text()); ++applied;
    }
};
int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc, argv);
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    bool ok = true;
    const auto expect = [&](bool pass, const char* label) { ok &= pass; QTextStream(pass ? stdout : stderr) << (pass ? "PASS " : "FAIL ") << label << '\n'; };
    PatternHost host;
    const QString source = "(150){8}1-5[4:1],3,1/5-1[4:1],7,5/1-5[4:1],3,1,,,,";
    host.workspace.openSource("&inote_5="+source+"\n",{},5);
    host.editor.setProperty("text",host.workspace.document().difficulty(5)->chart);
    host.editor.send(SCI_EMPTYUNDOBUFFER);
    host.service.requestCurrentPatterns();
    for (int i = 0; host.service.pending() && i < 1000; ++i) QTest::qWait(5);
    PreviewRuntime runtime; runtime.setSkinDirectory(QStringLiteral(MIACODE_SOURCE_ROOT "/assets/skin/skinSD"));
    QTranslator translator;
    for (const QString& locale : {QString("en_US"),QString("zh_CN"),QString("ja_JP")}) {
        expect(translator.load(QStringLiteral(MIACODE_BUILD_ROOT "/") + locale + ".qm"), "compiled language catalog loads");
        app.installTranslator(&translator);
        QFile ts(QStringLiteral(MIACODE_SOURCE_ROOT "/translations/") + locale + ".ts");
        bool translated = ts.open(QIODevice::ReadOnly);
        QXmlStreamReader xml(&ts);
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement() || xml.name() != QStringLiteral("message")) continue;
            const QString id = xml.attributes().value(QStringLiteral("id")).toString();
            if (!id.startsWith("hand_order.pattern.")) continue;
            if (qtTrId(id.toUtf8().constData()) == id) { translated = false; QTextStream(stderr) << "MISSING_TRANSLATION " << locale << ' ' << id << '\n'; }
        }
        expect(translated && !xml.hasError(), "all pattern IDs resolve through the actual compiled catalog");
        app.removeTranslator(&translator);
    }
    expect(translator.load(QStringLiteral(MIACODE_BUILD_ROOT "/zh_CN.qm")), "Chinese catalog loads"); app.installTranslator(&translator);
    QQmlEngine engine; engine.addImportPath(QStringLiteral(MIACODE_QML_SPEC_IMPORT_ROOT));
    engine.rootContext()->setContextProperty("testService",&host.service);
    engine.rootContext()->setContextProperty("testRuntime",&runtime);
    engine.rootContext()->setContextProperty("testHost",&host);
    QQmlComponent component(&engine);
    component.setData(R"qml(
import QtQuick
import QtQuick.Controls
import MiaCode.UI
Window {
    width: 1020; height: 720; visible: true; color: "#191a1b"
    PatternSuggestionsPanel {
        id: panel; anchors.fill: parent; service: testService; previewRuntime: testRuntime
        playing: false
        Component.onCompleted: previewSecond = 0.48
        onChoiceRequested: (group, variant) => testHost.apply(group,variant)
        onChoicesRequested: (keys, choice) => testHost.applySelected(keys,choice)
        onLocationRequested: group => testHost.navigate(group)
    }
})qml", QUrl("file:///pattern-choices-panel-spec.qml"));
    std::unique_ptr<QObject> object(component.create());
    if (!object) { QTextStream(stderr) << component.errorString(); return 1; }
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    expect(window && host.service.patternCount() == 1, "real QML panel opens retained named alternatives");
    expect(!visibleText(window->contentItem(),qtTrId("hand_order.pattern.notice")),
        "the requested explanatory paragraph is absent from the live panel");
    expect(QTest::qWaitFor([&] { return !runtime.frameState().skin.tapImage.isNull(); }, 5000), "native skin assets finish loading");
    QTest::qWait(100);
    auto* preview = visualItem(window->contentItem(),"patternCandidatePreview_0");
    auto* category = visualItem(window->contentItem(),"patternCategory_umiyuri");
    const auto beforeExpansion = host.service.computationCount();
    expect(category && !visualItem(window->contentItem(),"patternSuggestionRow_0"), "category mode starts with collapsible type headers");
    if (category) QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,
        category->mapToScene(QPointF(category->width()/2,category->height()/2)).toPoint());
    QTest::qWait(30);
    expect(host.seeks == 0 && host.service.computationCount() == beforeExpansion,
        "expanding a type does not navigate the chart or recalculate candidates");
    auto* row = visualItem(window->contentItem(),"patternSuggestionRow_0");
    if (row) QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,
        row->mapToScene(QPointF(row->width()/2,row->height()/2)).toPoint());
    QTest::qWait(30);
    expect(host.seeks == 1 && host.playhead == 0 && host.editor.send(SCI_GETSELECTIONEND) > host.editor.send(SCI_GETSELECTIONSTART),
        "clicking the already-selected candidate highlights its full range and publishes a playback seek");
    auto* syntax = visualItem(window->contentItem(),"patternCandidateSyntax_0");
    if (auto* scene = qobject_cast<PatternCandidatePreview*>(preview)) {
        const auto& frame = scene->frameState();
        const auto track = miacode::preview::scene::buildPreviewTrackLayerState(frame,
            miacode::preview::scene::PreviewActiveMarkerView(frame.noteMarkers), QRectF(0,0,720,720));
        expect(frame.noteMarkers.size() == 9 && track.sprites.size() > 0,
            "opaque native markers reach the real scene and produce native sprite descriptors");
    }
    expect(preview && syntax && syntax->property("text").toString().contains("~"), "cards expose rendered native markers and their exact decomposition syntax");
    const QString output = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (!output.isEmpty()) { QDir().mkpath(output); window->grabWindow().save(output+"/pattern-candidates-wide.png"); }
    auto* button = visualItem(window->contentItem(),"applyPatternCandidate_0");
    if (button) {
        const auto at = button->mapToScene(QPointF(button->width()/2,button->height()/2));
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,at.toPoint()); QTest::qWait(30);
    }
    expect(host.applied == 1 && host.service.patternCount() == 1, "real candidate button applies a native transaction while retaining alternatives");
    host.editor.send(SCI_UNDO); host.workspace.replaceActiveDifficultyChart(host.text());
    expect(host.text() == source+"\n" || host.text() == source, "one native Undo restores the previous annotations");
    window->setWidth(620); window->setHeight(440); QTest::qWait(100);
    preview = visualItem(window->contentItem(),"patternCandidatePreview_0");
    if (!output.isEmpty()) window->grabWindow().save(output+"/pattern-candidates-compact.png");
    expect(preview && preview->width() > 100, "compact panel retains a usable native preview");
    window->setWidth(1020); window->setHeight(720); QTest::qWait(50);
    auto* ignore = visualItem(window->contentItem(),"ignorePatternSuggestion");
    expect(ignore && ignore->property("text").toString() == QStringLiteral("忽略"), "the visible Ignore button uses translated text rather than a key");
    if (ignore) QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,
        ignore->mapToScene(QPointF(ignore->width()/2,ignore->height()/2)).toPoint());
    QTest::qWait(30);
    expect(host.service.patternCount() == 0 && host.applied == 1,
        "the Ignore button hides the candidate without another annotation edit");
    auto* panel = visualItem(window->contentItem(),"patternSuggestionsPanel");
    if (panel) panel->setProperty("sortMode","time");
    expect(qtTrId("hand_order.pattern.empty") == QStringLiteral("暂无候选。"), "empty-state text does not instruct users to trigger a background or contextual recalculation");
    const QString multiple = source + "{1},,,,{16}1,2,3,4,5,6,7,8,{1},,,,{16}3,4,1,3,4,1,{1},,,,{16}1,5,1,5,";
    host.workspace.openSource("&inote_5="+multiple+"\n",{},5);
    host.editor.setProperty("text",host.workspace.document().difficulty(5)->chart);
    host.service.requestCurrentPatterns();
    for (int i = 0; host.service.pending() && i < 1000; ++i) QTest::qWait(5);
    QTest::qWait(30);
    panel = visualItem(window->contentItem(),"patternSuggestionsPanel");
    expect(host.service.patternCount() == 4 && panel, "a mixed-category chart retains time-ordered source identities");
    if (panel) panel->setProperty("sortMode","category");
    QTest::qWait(30);
    expect(panel && panel->property("selectedGroup").toInt() == 3,
        "category sorting preserves the selected named pattern by stable identity");
    auto* triangleText = visualItem(window->contentItem(),"patternCategoryText_triangle");
    auto* triangleIcon = visualItem(window->contentItem(),"patternCategoryIndicator_triangle");
    auto* trillText = visualItem(window->contentItem(),"patternCategoryText_trill");
    expect(triangleText && trillText && triangleText->property("text").toString().contains(QStringLiteral("三角交互"))
        && trillText->property("text").toString().contains(QStringLiteral("交互")), "interaction and triangle interaction are separate translated expandable groups");
    expect(triangleIcon && triangleIcon->property("status").toInt() == 1,
        "category disclosure indicators load the existing vector icons without relying on font glyphs");
    if (!output.isEmpty()) window->grabWindow().save(output+"/pattern-category-groups.png");
    category = visualItem(window->contentItem(),"patternCategory_triangle");
    if (category) QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,
        category->mapToScene(QPointF(category->width()/2,category->height()/2)).toPoint());
    QTest::qWait(30);
    expect(visualItem(window->contentItem(),"patternSuggestionRow_2") != nullptr,
        "expanding triangle interaction reveals its concrete instance");
    const auto triangle = host.service.patternSuggestions().at(2).toMap();
    expect(triangle.value("preferred").toInt() == 0 && triangle.value("variants").toList().first().toMap().value("strategy").toString()
        == QStringLiteral("保持相位的双手交替"), "triangle instances expose the requested phase-preserving default");
    category = visualItem(window->contentItem(),"patternCategory_sweep");
    if (category) QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,
        category->mapToScene(QPointF(category->width()/2,category->height()/2)).toPoint());
    QTest::qWait(30);
    row = visualItem(window->contentItem(),"patternSuggestionRow_1");
    if (row) QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,
        row->mapToScene(QPointF(row->width()/2,row->height()/2)).toPoint());
    QTest::qWait(30);
    expect(host.target.value("start") == host.service.patternNavigationTarget(1).value("start") && host.playhead > 0,
        "the first category-sorted row navigates to its actual later chart time");
    button = visualItem(window->contentItem(),"applyPatternCandidate_0");
    if (button) QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,
        button->mapToScene(QPointF(button->width()/2,button->height()/2)).toPoint());
    QTest::qWait(30);
    expect(host.applied == 2 && host.text().contains("1~r,2~r,3~r,4~r,5~r,6~r,7~r,8~r"),
        "applying after category sorting edits the selected sweep rather than the earlier named pattern");
    const auto beforeManual = host.service.computationCount();
    QString manual = host.text();
    manual.replace("1~r,2~r,3~r,4~r,5~r,6~r,7~r,8~r", "1~l,2~l,3~l,4~l,5~l,6~l,7~l,8~l");
    host.editor.setProperty("text",manual); host.workspace.replaceActiveDifficultyChart(manual);
    expect(host.service.computationCount() == beforeManual, "manual hand edits retain the displayed choices without an automatic solve");
    host.service.requestCurrentPatterns();
    for (int i = 0; host.service.pending() && i < 1000; ++i) QTest::qWait(5);
    QTest::qWait(30);
    expect(host.service.computationCount() == beforeManual + 1
        && host.service.patternSuggestions().at(1).toMap().value("variants").toList().first().toMap().value("syntax").toString()
            .contains("1~l,2~l,3~l,4~l,5~l,6~l,7~l,8~l"),
        "explicit recalculation reads the new hand labels and keeps the user's current whole-hand sweep as default");
    ignore = visualItem(window->contentItem(),"ignorePatternSuggestion");
    if (ignore) QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,
        ignore->mapToScene(QPointF(ignore->width()/2,ignore->height()/2)).toPoint());
    QTest::qWait(30);
    expect(host.service.patternCount() == 3 && host.service.patternSuggestions().first().toMap().value("registryId") == "N01",
        "ignoring after category sorting hides the selected sweep and preserves the other category");
    const QString batch="(150){16}3,4,1,3,4,1,,,,6,5,8,6,5,8,";
    host.workspace.openSource("&inote_5="+batch+"\n",{},5);host.editor.setProperty("text",host.workspace.document().difficulty(5)->chart);
    host.editor.send(SCI_EMPTYUNDOBUFFER);host.service.requestCurrentPatterns();
    for(int i=0;host.service.pending()&&i<1000;++i) QTest::qWait(5);
    QTest::qWait(40);panel=visualItem(window->contentItem(),"patternSuggestionsPanel");
    if(panel) { panel->setProperty("sortMode","category");QVariantMap expanded;
        expanded.insert("triangle",true);
        if(host.service.patternCount()>0) expanded.insert("similar:"+host.service.patternSuggestions()[0].toMap().value("similarityKey").toString(),true);
        panel->setProperty("expandedTypes",expanded);
    }
    QTest::qWait(50);
    auto* all=visualItem(window->contentItem(),"selectSimilarPatterns");
    expect(all&&all->isVisible(),"mirrored shapes expose explicit multi-selection controls");
    if(all) QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,all->mapToScene(QPointF(all->width()/2,all->height()/2)).toPoint());
    QTest::qWait(30);
    auto* batchButton=visualItem(window->contentItem(),"applySelectedPatterns_0");
    expect(panel&&panel->property("selectedKeys").toList().size()==2&&batchButton&&batchButton->isVisible(),
        "select-similar marks both concrete instances before offering batch application");
    auto* checkbox=visualItem(window->contentItem(),"selectPattern_1");
    expect(checkbox&&checkbox->isVisible()&&checkbox->width()>=13&&checkbox->property("checked").toBool(),
        "each selected instance has a visible themed checkbox");
    if(checkbox)QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,checkbox->mapToScene(QPointF(checkbox->width()/2,checkbox->height()/2)).toPoint());
    QTest::qWait(20);
    expect(panel&&panel->property("selectedKeys").toList().size()==1,"the user can exclude an individual similar instance");
    if(all)QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,all->mapToScene(QPointF(all->width()/2,all->height()/2)).toPoint());
    QTest::qWait(20);
    if(panel)panel->setProperty("previewSecond",0.35);
    if(!output.isEmpty())window->grabWindow().save(output+"/pattern-similar-selection.png");
    const auto beforeBatch=host.text();const auto applications=host.applied;
    batchButton=visualItem(window->contentItem(),"applySelectedPatterns_0");
    expect(batchButton&&batchButton->isVisible()&&batchButton->isEnabled(),"the current instance exposes an enabled batch action after reselection");
    if(batchButton)QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,batchButton->mapToScene(QPointF(batchButton->width()/2,batchButton->height()/2)).toPoint());
    QTest::qWait(40);
    expect(host.applied==applications+1&&host.text().contains("3~r,4~l,1~r")&&host.text().contains("6~l,5~r,8~l"),
        "the real batch button applies each mirrored entry through one native editor transaction");
    host.editor.send(SCI_UNDO);host.workspace.replaceActiveDifficultyChart(host.text());
    expect(host.text()==beforeBatch,"one Scintilla Undo restores all selected instances together");
    const auto applicationsBeforeIgnore = host.applied;
    ignore = visualItem(window->contentItem(), "ignorePatternSuggestion");
    if (ignore) QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
        ignore->mapToScene(QPointF(ignore->width()/2, ignore->height()/2)).toPoint());
    QTest::qWait(30);
    expect(host.service.patternCount() == 0 && host.text() == beforeBatch && host.applied == applicationsBeforeIgnore,
        "Ignore hides every selected similar instance without editing the chart or adding undo entries");
    host.service.requestCurrentPatterns();
    for (int i = 0; host.service.pending() && i < 1000; ++i) QTest::qWait(5);
    expect(host.service.patternCount() == 0, "an explicit refresh retains all batch ignores");
    return ok ? 0 : 1;
}
#include "PatternChoicesPanelSpec.moc"
