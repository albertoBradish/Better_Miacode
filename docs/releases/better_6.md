# Better Miacode better.6

## Slide 补间修正

亮度补间现在复用原有轨迹绘制与擦除链路：常规、逐区擦除和无理检测模式均照常移除已判定区域，实际判定完成后不会残留整条低亮轨迹。

全亮终点改为最早 Perfect 时刻，保留头拍开始和按弧长匀速推进的行为。若头拍时已在 Perfect 范围内，则头拍直接全亮，头拍前仍保持低亮。fes 链使用整条链最终的 Perfect 窗口，不在接点重置。

预览和视频导出共用这两项修正。CLI 统计增加 `brightnessCompleteSecond`，并保留 `judgementSecond` 作为尾判正解基准。

详见 [Slide 判定时值规格](../specs/preview/SLIDE_JUDGEMENT_TIMING_ZH.md)。

## 本地验证

2026 年 10 月 28 日，Windows x64 Release，仅使用工作区合成谱面与独立 runtime。

- 9 项聚焦 Spec 通过，覆盖 616 条形状、最早 Perfect 终点、头拍直接全亮的边界、各渲染模式的可见轨迹一致性，以及普通 slide 和 WiFi 的判定完成消失。
- OpenGL、D3D11 各 10 类原皮肤样例通过：全亮时刻的剩余轨迹与原材质逐像素一致，原有擦除、实际判定完成消失、回拖重放均通过。
- 60 FPS 整段、120 FPS 片段视频导出成功；CLI 实际全亮时刻与 `max(头拍, Perfect 起点)` 一致，偏移只应用一次。
- 固定编码配置下，关闭辅助功能的 120 帧与原版对比一致；运行目录隔离拒绝检查通过。

Windows ARM64、macOS 尚未完成本版本本地验收；本地包不等同于已发布 GitHub Release。
