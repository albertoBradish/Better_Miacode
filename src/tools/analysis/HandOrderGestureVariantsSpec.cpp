#include "core/analysis/hand_order/PatternHandOrderOptimization.h"
#include "core/chart/transform/BasicHandOrderLayout.h"
#include "core/chart/document/SimaiDocument.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/model/HandOrderHoldLock.h"
#include <QCoreApplication>
#include <QFile>
#include <QCryptographicHash>
#include <QTextStream>
using namespace miacode;
using namespace miacode::hand_order;
QString apply(QString text, const QVector<chart_transform::HandOrderEditPlan::Patch>& patches) {
    for (int i=patches.size()-1;i>=0;--i) { const auto& p=patches[i]; text.replace(p.start,p.end-p.start,p.text); } return text;
}
QString hands(const SimaiParseResult& parsed) {
    QString result; for (const auto& m:parsed.noteMarkers) result += m.handOrderHead==HandOrderHand::Left ? 'l' : m.handOrderHead==HandOrderHand::Right ? 'r' : '?'; return result;
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); bool ok=true;
    const auto expect=[&](bool pass,const char* message) { ok &= pass; QTextStream(pass?stdout:stderr) << (pass?"PASS ":"FAIL ") << message << '\n'; };
    const auto check=[&](const QString& text,const QString& expected,const char* message,bool infer=true) {
        auto parsed=SimaiParser::validateSyntax(text);
        auto scan=optimizeHandOrderPatterns(text,parsed,0,text.size());
        const auto proposed=SimaiParser::validateSyntax(apply(text,scan.patches));
        const auto layout=chart_transform::basicHandOrderLayoutInSelection(text,0,text.size());
        const auto laid=SimaiParser::validateSyntax(apply(text,layout.edit.patches));
        const auto matches=[](const QString& actual,const QString& mask) { if(actual.size()!=mask.size())return false; for(int i=0;i<actual.size();++i)if(mask[i]!='?'&&actual[i]!=mask[i])return false; return true; };
        bool pass=parsed.ok && proposed.ok && matches(hands(proposed),expected) && matches(hands(laid),expected);
        if (!pass) QTextStream(stderr) << "expected=" << expected << " scan=" << hands(proposed) << " layout=" << hands(laid) << '\n';
        expect(pass,message);
        if(infer) {
            BasicParameters p; p.overwriteExisting=true;p.counterfactualBudget=0;
            const auto result=inferBasicHandOrder(text,parsed,0,text.size(),p);
            const auto inferred=SimaiParser::validateSyntax(apply(text,result.patches));
            if(!matches(hands(inferred),expected)) QTextStream(stderr) << "inference=" << hands(inferred) << " error=" << result.error << '\n';
            expect(matches(hands(inferred),expected),"inference preserves the structural gesture default");
        }
    };
    check("(201){16}4,4,5,,6,6,5,","rrlllr","burst pairs distribute their endings across hands");
    check("(201){16}3,4,1h[32:1],3,4,1,3,4,1,","rlrlrlrlr","a short Hold inside a triangle preserves alternation and its full tail");
    check("(198){8}E6/3,3,E7/3,3,","rlrrlr","mixed Each returns keep the key hand",true);
    for (int bpm:{170,171,200}) {
        const QString text=QString("(%1){8}7-5[8:1],8,7,2-5[8:1],1,2,7-4[8:1],8,7,").arg(bpm);
        check(text,bpm>170?"lrlrlrlrl":"lllrrrlll","separated triplets share the middle only above 170 BPM",true);
    }
    for (const QString& launch:{QString("7"),QString("7$"),QString("A7")})
        check("(200){8}7-4[8:1],8,"+launch+",,,,","l?l","launch types follow the sliding hand",true);
    // Isolated launch candidates intentionally do not annotate the unrelated neighbour.
    const QString stroke="(201){8}7<4[8:1],4-7[8:1],7V51[4:1],2,,3,3,,";
    const auto sr=optimizeHandOrderPatterns(stroke,SimaiParser::validateSyntax(stroke),0,stroke.size());
    const auto sp=SimaiParser::validateSyntax(apply(stroke,sr.patches));
    bool continuous=sr.patternSuggestions.size()>0;
    for(const auto& m:sp.noteMarkers) if(m.type=="slide") continuous &= !m.handOrderTrackStops.isEmpty() && m.handOrderTrackStops.first().hand==HandOrderHand::Left;
    continuous &= sp.noteMarkers.size()>=3 && sp.noteMarkers[2].handOrderHead==HandOrderHand::Left;
    expect(continuous,"one-stroke reversal keeps tracks and launch star on the continuous hand");
    BasicParameters strokeParameters; strokeParameters.overwriteExisting=true; strokeParameters.counterfactualBudget=0;
    const auto strokeInference=inferBasicHandOrder(stroke,SimaiParser::validateSyntax(stroke),0,stroke.size(),strokeParameters);
    const auto si=SimaiParser::validateSyntax(apply(stroke,strokeInference.patches));
    bool inferredStroke=si.noteMarkers.size()>=3 && si.noteMarkers[2].handOrderHead==HandOrderHand::Left;
    for(const auto& m:si.noteMarkers) if(m.type=="slide") inferredStroke &= !m.handOrderTrackStops.isEmpty() && m.handOrderTrackStops.first().hand==HandOrderHand::Left;
    expect(inferredStroke,"inference preserves the stroke track and its launch star hand");
    for (int rotation = 0; rotation < 8; ++rotation) for (int mirror : {1, -1}) {
        const auto lane = [&](int value) { return (rotation + mirror * (value - 1) + 16) % 8 + 1; };
        const QString text = QString("(150){4}%1-%2[4:1],%1/%3,,,,")
            .arg(lane(1)).arg(lane(5)).arg(lane(2));
        const auto plan = optimizeHandOrderPatterns(text, SimaiParser::validateSyntax(text), 0, text.size());
        bool found = false;
        for (const auto& group : plan.patternSuggestions) if (group.registryId == "S01") {
            found = group.preferred == 0 && group.variants.size() >= 2;
            if (group.variants.size() < 2) continue;
            const auto& first = group.variants[0]; const auto& second = group.variants[1];
            found &= first.strategy == "launch_track" && second.strategy == "handoff";
            const auto& star = first.markers[0]; const auto& launch = first.markers[1]; const auto& tap = first.markers[2];
            found &= star.handOrderHead == launch.handOrderHead && star.handOrderTrackStops.size() == 1
                && star.handOrderHead == star.handOrderTrackStops.first().hand
                && tap.handOrderHead != launch.handOrderHead;
            found &= second.markers[0].handOrderHead == star.handOrderHead
                && second.markers[0].handOrderTrackStops.first().hand != star.handOrderHead;
            found &= stripAnnotations(apply(text, first.patches)) == text;
        }
        expect(found, "coordinated launches prefer one-hand strike-and-trace, with handoff second under every rotation and reflection");
    }
    for (const QString& crossed : {QString("(150){4}1~l/8~r,"), QString("(150){4}4~l/5~r,")}) {
        const auto layout = chart_transform::basicHandOrderLayoutInSelection(crossed, 0, crossed.size());
        const auto parsed = SimaiParser::validateSyntax(apply(crossed, layout.edit.patches));
        bool natural = parsed.noteMarkers.size() == 2;
        for (const auto& marker : parsed.noteMarkers)
            natural &= marker.handOrderHead == (marker.lane <= 4 ? HandOrderHand::Right : HandOrderHand::Left);
        expect(natural, "near-centre adjacent Each pairs clear stale crossed simple labels");
    }
    const QString returnEach = "(221){16}3~r/4~l,,,4/5,";
    const auto returnLayout = chart_transform::basicHandOrderLayoutInSelection(returnEach, 0, returnEach.size());
    const auto returnParsed = SimaiParser::validateSyntax(apply(returnEach, returnLayout.edit.patches));
    expect(returnParsed.noteMarkers.size() == 4 && returnParsed.noteMarkers[2].handOrderHead == HandOrderHand::Right
        && returnParsed.noteMarkers[3].handOrderHead == HandOrderHand::Left,
        "a relaxed return does not carry the old crossed phase into the next opposite-side Each");
    const QString paused="(201){8}8-4[8:1],,4-8[8:1],,,,";
    const auto pr=optimizeHandOrderPatterns(paused,SimaiParser::validateSyntax(paused),0,paused.size());
    const auto pp=SimaiParser::validateSyntax(apply(paused,pr.patches));
    expect(pp.noteMarkers.size()==2 && !pp.noteMarkers[0].handOrderTrackStops.isEmpty()
        && !pp.noteMarkers[1].handOrderTrackStops.isEmpty()
        && pp.noteMarkers[0].handOrderTrackStops.first().hand==pp.noteMarkers[1].handOrderTrackStops.first().hand,
        "a brief endpoint dwell remains a continuous stroke");
    for (int rotation=0;rotation<8;++rotation) for (int mirror:{1,-1}) {
        const auto lane=[&](int value) { return (rotation+mirror*(value-1)+16)%8+1; };
        const QString reversing=QString("(144){4}%1-%2[8:1],%2-%1[8:1],%1-%2[8:1],%2-%1[8:1],,,,")
            .arg(lane(1)).arg(lane(5));
        const auto native=SimaiParser::validateSyntax(reversing);
        const auto plan=optimizeHandOrderPatterns(reversing,native,0,reversing.size());
        const auto output=apply(reversing,plan.patches);
        const auto annotated=SimaiParser::validateSyntax(output);
        const auto hand=lane(1)>=5?HandOrderHand::Left:HandOrderHand::Right;
        const auto free=hand==HandOrderHand::Left?HandOrderHand::Right:HandOrderHand::Left;
        bool coherent=annotated.noteMarkers.size()==4;
        for (int n=0;n<annotated.noteMarkers.size();++n) {
            const auto& marker=annotated.noteMarkers[n];
            coherent &= marker.handOrderHead==(n?free:hand)
                && marker.handOrderTrackStops.size()==1 && marker.handOrderTrackStops.first().hand==hand;
        }
        expect(coherent && stripAnnotations(output)==reversing,
            "rotated and mirrored reversing strokes give off-path stars to the free hand");
    }
    const QString fan="(144){4}2-6[4:1],2<7[8:3]*<6[2:1]*<5[8:5],,,,,";
    const auto fanPlan=optimizeHandOrderPatterns(fan,SimaiParser::validateSyntax(fan),0,fan.size());
    const auto fanParsed=SimaiParser::validateSyntax(apply(fan,fanPlan.patches));
    bool sharedFan=fanParsed.noteMarkers.size()==4;
    for (int n=1;n<fanParsed.noteMarkers.size();++n)
        sharedFan &= fanParsed.noteMarkers[n].handOrderHead==HandOrderHand::Right
            && fanParsed.noteMarkers[n].handOrderTrackStops.size()==1
            && fanParsed.noteMarkers[n].handOrderTrackStops.first().hand==HandOrderHand::Left;
    expect(sharedFan,"a shared launch star can strike with the old stroke hand and start its co-traced fan with the free hand");
    const QString differentSpeeds="(144){4}2-6[4:1],2<7[8:3]*<6[8:3],,,,,";
    const auto speedPlan=optimizeHandOrderPatterns(differentSpeeds,SimaiParser::validateSyntax(differentSpeeds),0,differentSpeeds.size());
    const auto speedOutput=apply(differentSpeeds,speedPlan.patches);
    const auto speedParsed=SimaiParser::validateSyntax(speedOutput);
    bool forcedSharedFan=speedParsed.noteMarkers.size()==3;
    for (int n=1;n<speedParsed.noteMarkers.size();++n)
        forcedSharedFan &= speedParsed.noteMarkers[n].handOrderTrackStops.size()==1
            && speedParsed.noteMarkers[n].handOrderTrackStops.first().hand==HandOrderHand::Left;
    const bool distinctSpeeds=speedParsed.ok && speedParsed.noteMarkers.size()==3 && !forcedSharedFan
        && stripAnnotations(speedOutput)==differentSpeeds;
    if (!distinctSpeeds) QTextStream(stderr) << "different-speeds result=" << speedOutput << '\n';
    expect(distinctSpeeds,
        "different branch speeds do not receive the co-traced fan default");
    for (const QString& fork:{QString("(144){4}4>5[2:1]*q5[2:1],,,,"),QString("(144){4}4q5[2:1]*>5[2:1],,,,")}) {
        const auto forkPlan=optimizeHandOrderPatterns(fork,SimaiParser::validateSyntax(fork),0,fork.size());
        const auto forkParsed=SimaiParser::validateSyntax(apply(fork,forkPlan.patches));
        bool split=forkPlan.patternSuggestions.size()==1 && forkPlan.patternSuggestions.first().registryId=="S11"
            && forkPlan.patternSuggestions.first().preferred==0;
        for (const auto& marker:forkParsed.noteMarkers)
            split &= !marker.slideSegmentKeys.isEmpty() && marker.handOrderTrackStops.size()==1
                && marker.handOrderTrackStops.first().hand==(marker.slideSegmentKeys.first().contains('>')?HandOrderHand::Left:HandOrderHand::Right);
        expect(split,"equal-endpoint ring and inner-curve branches split by their late paths independently of spelling order");
    }
    const QString lockedFan="(144){4}8h[1:1]~l/2-6[4:1],2<7[8:3]*<6[2:1]*<5[8:5],,,,,";
    const auto lockedParsed=SimaiParser::validateSyntax(lockedFan);
    const auto lockedPlan=optimizeHandOrderPatterns(lockedFan,lockedParsed,0,lockedFan.size());
    bool fanLockSafe=true;
    for(const auto& group:lockedPlan.patternSuggestions) for(const auto& choice:group.variants)
        fanLockSafe &= respectsHoldLocks(lockedParsed.noteMarkers,choice.markers);
    expect(fanLockSafe,"stroke and fan variants preserve an external Hold through its full native tail");
    const QString moving="(200){16}5,4,6,4,6,4,7,4,7,4,8,4,8,4,8,3,8,3,8,2,8,2,8,1,8,1,{8}2/7,";
    const auto mr=optimizeHandOrderPatterns(moving,SimaiParser::validateSyntax(moving),0,moving.size());
    bool alternating=false;
    for(const auto& group:mr.patternSuggestions) if(group.registryId=="T04") {
        const auto& notes=group.variants.first().markers; alternating=notes.size()>=26;
        for(int i=1;i<26 && i<notes.size();++i) alternating &= notes[i].handOrderHead!=notes[i-1].handOrderHead;
    }
    expect(alternating,"shifting interaction rails are not classified as a one-hand sweep");
    const QString holdLaunch="(200){8}7-4[8:1],8,7h[8:1],,,,";
    const auto hr=optimizeHandOrderPatterns(holdLaunch,SimaiParser::validateSyntax(holdLaunch),0,holdLaunch.size());
    const auto hp=SimaiParser::validateSyntax(apply(holdLaunch,hr.patches));
    expect(hp.noteMarkers.size()==3 && hp.noteMarkers[2].type=="hold" && hp.noteMarkers[2].handOrderHead==HandOrderHand::Left
        && !hp.noteMarkers[0].handOrderTrackStops.isEmpty() && hp.noteMarkers[0].handOrderTrackStops.first().hand==HandOrderHand::Right
        && respectsHoldLocks(hp.noteMarkers,hp.noteMarkers),"a Hold launch remains locked and gives the moving trace to the other hand");
    const QString lock="(150){8}2h[2:1]~r,,1,8,1,";
    const auto lr=optimizeHandOrderPatterns(lock,SimaiParser::validateSyntax(lock),0,lock.size());
    bool safe=true; for(const auto& g:lr.patternSuggestions)for(const auto& v:g.variants) safe &= respectsHoldLocks(SimaiParser::validateSyntax(lock).noteMarkers,v.markers);
    expect(safe,"every candidate keeps an external Hold locked");
    const QString alias="(150){8}C1xh[4:1]~l,1~r,";
    const auto a=SimaiParser::validateSyntax(alias), c=SimaiParser::validateSyntax(QString(alias).replace("C1xh","Ch"));
    const auto timeline=SimaiParser::parseForTimeline(alias);
    expect(a.ok && a.warnings.isEmpty() && a.noteMarkers.size()==2 && a.noteMarkers[0].type=="touch_hold"
        && a.noteMarkers[0].touchPad=="C" && a.noteMarkers[0].endSecond==c.noteMarkers[0].endSecond
        && timeline.noteMarkers[0].endSecond==a.noteMarkers[0].endSecond
        && a.sourceTokens[0].length==QString("C1xh[4:1]~l").size(),"C1xh aliases Ch in validation and timeline without altering its source span");
    const QString shapes="(150){16}3,4,1,3,4,1,,,,{16}6,5,8,6,5,8,";
    const auto sh=optimizeHandOrderPatterns(shapes,SimaiParser::validateSyntax(shapes),0,shapes.size());
    expect(sh.patternSuggestions.size()==2 && sh.patternSuggestions[0].similarityKey==sh.patternSuggestions[1].similarityKey,
        "mirrored triangles share a native shape family");
    if(argc>1) {
        QFile f(QString::fromLocal8Bit(argv[1])); expect(f.open(QIODevice::ReadOnly),"reference chart opens read-only");
        const auto bytes=f.readAll(); const auto document=SimaiDocument::fromText(QString::fromUtf8(bytes));
        const auto* difficulty=document.difficulty(5); expect(difficulty!=nullptr,"reference Master exists");
        if(difficulty) {
            const auto native=SimaiParser::validateSyntax(difficulty->chart);
            const auto scan=optimizeHandOrderPatterns(difficulty->chart,native,0,difficulty->chart.size());
            const auto after=SimaiParser::validateSyntax(apply(difficulty->chart,scan.patches));
            int returns=0, strokes=0, interaction=0;
            for(const auto& g:scan.patternSuggestions) { returns+=g.registryId=="T24";strokes+=g.registryId=="S07";interaction+=g.registryId=="T04"; }
            expect(native.ok && after.ok && stripAnnotations(apply(difficulty->chart,scan.patches))==difficulty->chart,"reference scan preserves native chart data");
            const QString annotated=apply(difficulty->chart,scan.patches);
            expect(annotated.contains("7V51[4:1]~[head=l;track=0:l]")
                && annotated.contains("2V48[4:1]~[head=r;track=0:r]"),"reference launch stars match their initiating track hands");
            expect(returns>=15 && strokes>=2 && interaction>=1,"reference scan recalls mixed returns, strokes and moving interactions");
            QTextStream(stdout)<<"reference_sha256="<<QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()<<" patterns="<<scan.patternSegments<<" returns="<<returns<<" strokes="<<strokes<<" moving="<<interaction<<'\n';
        }
        f.close(); expect(f.open(QIODevice::ReadOnly) && f.readAll()==bytes,"reference chart remains byte-identical");
    }
    return ok?0:1;
}
