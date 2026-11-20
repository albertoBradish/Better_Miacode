#pragma once

#include "core/chart/document/SimaiDocument.h"
#include "core/chart/ChartBpmExport.h"
#include "core/chart/document/SimaiTimingMetadata.h"

#include <QRegularExpression>
#include <QStringList>

namespace miacode::sticking {

inline QString publicationDifficultyName(int difficultyId)
{
    return difficultyId == 6 ? QStringLiteral("REMAS")
                             : SimaiDocument::difficultyShortName(difficultyId);
}

inline QString publicationFileComponent(QString text, const QString& fallback)
{
    // Preserve Unicode song names, including surrogate pairs, in Windows filenames.
    const QString illegal = QStringLiteral("<>:\"/\\|?*");
    const QString fullwidth = QStringLiteral("＜＞：＂／＼｜？＊");
    for (qsizetype i = 0; i < text.size(); ++i) {
        const qsizetype replacement = illegal.indexOf(text.at(i));
        if (replacement >= 0) text[i] = fullwidth.at(replacement);
        else if (text.at(i).unicode() < 0x20) text[i] = QLatin1Char(' ');
    }
    text = text.trimmed();
    return text.isEmpty() ? fallback : text;
}

inline QString publicationVideoName(const SimaiDocument& document, int difficultyId)
{
    const auto* difficulty = document.difficulty(difficultyId);
    return QStringLiteral("【舞萌DX手序拆解／读谱辅助】%1 %2 %3.mp4")
        .arg(publicationFileComponent(document.title, QStringLiteral("未命名")),
             publicationDifficultyName(difficultyId),
             publicationFileComponent(difficulty ? difficulty->level : QString(), QStringLiteral("未标注")));
}

inline QString publicationInfoName(const SimaiDocument& document, int difficultyId)
{
    return QStringLiteral("%1 %2 拆分信息.txt")
        .arg(publicationFileComponent(document.title, QStringLiteral("未命名")),
             publicationDifficultyName(difficultyId));
}

inline QString publicationInfoText(const SimaiDocument& document, int difficultyId)
{
    const auto* difficulty = document.difficulty(difficultyId);
    const QString chart = difficulty ? difficulty->chart : QString();
    const auto parsed = SimaiParser::parseForTimeline(chart, miacode::simai::buildTimingMetadata(document));
    bool validFirst = false;
    const double first = document.first.toDouble(&validFirst);
    const QString bpmList = miacode::chart::exportChartBpm(
        parsed, validFirst ? first : 0.0, miacode::chart::BpmExportFormat::AudioTime);
    return QStringLiteral(
        "No.000\n"
        "游戏：舞萌DX / Maimai DX\n"
        "曲名：%1\n"
        "曲师：%2\n"
        "谱师：%3\n"
        "BPM：%4\n"
        "谱面拆解:@albertoBradish  \n"
        "谱面录制: MiaCode HandOrder by @albertoBradish （Miacode本家﻿@MiaCode_official ）\n"
        "封面制作:@albertoBradish\n\n\n"
        "视频中拆解手序仅供参考。\n"
        "蓝色为左手，红色为右手，白色为双手均可；默认主要使用正攻，除非正攻手感过差。\n"
        "星星自暗变为全亮时，就是摸掉星星会黄的时间；有启动拍时视情况决定，若启动时已经可黄则会直接标黄。上机时可合理分配是否秒划。\n"
        "谱面来源于非官方抄谱，侵删。如果觉得有用/喜欢的话请多多三连支持！\n"
        "若有想要看到拆解的谱面，请于评论区评论。若想加入拆解，请私信UP主。")
        .arg(document.title, document.artist, document.effectiveDesignerForSlot(difficultyId),
             bpmList);
}

} // namespace miacode::sticking
