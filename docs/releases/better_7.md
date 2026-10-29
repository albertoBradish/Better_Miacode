# Better Miacode better.7

## 重叠路径与启动拍

修复部分 `pp/qq` 路径亮度跳段的问题。箭头位置按完整轨迹先后顺序匹配弧长，位置和方向都重合的路径也不会被误投到另一圈。仍保留最早 Perfect 全亮和当前模式的判定擦除。

新增“有启动拍时从启动拍开始”，默认关闭，位于预览画面选项及视频导出页。开启后，同一时刻、同一起点的 star、tap、hold 头和 A 区 touch 可作为启动拍；没有匹配时沿用原 star 头拍。补间开始时已进入 Perfect 范围的情况直接全亮。

片段导出保留完整谱面的启动拍索引，不因渲染范围裁剪而丢失候选。CLI 支持 `--slide-timing-from-launch-beat`，时值统计增加 `brightnessStartSecond` 和 `usesLaunchBeat`。

详见 [功能规格](../specs/preview/SLIDE_JUDGEMENT_TIMING_ZH.md)。

## 本地验证

2026 年 10 月 29 日，Windows x64 Release，仅使用工作区合成谱面及独立 runtime。

- 9 项聚焦 Spec 通过；616 条形状的弧长匹配保持路径顺序和原始位置，包含重叠多圈与折返。
- OpenGL、D3D11 各 14 类原生皮肤样例通过，覆盖 pp/qq 复现形状、判定擦除、启动拍前低亮、全亮终点及范围外启动拍保留。
- CLI 验证 tap、hold 头、star、A 区 touch 的同位同拍匹配和无匹配回退，偏移只应用一次。
- 启动拍开关前后的 360 帧对比有 181 帧产生预期变化；整段、120 FPS 片段及截断在启动拍前的导出通过。
- 固定编码配置下，关闭辅助的 120 帧与原程序一致；运行目录隔离拒绝检查通过。

Windows ARM64、macOS 尚未完成本版本本地验收，本地包不等同于已发布 GitHub Release。
