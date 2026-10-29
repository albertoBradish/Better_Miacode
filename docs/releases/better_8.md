# Better Miacode better.8

## 启动拍子选项

新增“启动拍前已进入 Perfect 时，从头拍全亮”，位于“有启动拍时从启动拍开始”下方，默认关闭。只有两个上层开关都开启时可勾选。

勾选后，若 slide 匹配到启动拍，且最早 Perfect 时刻早于启动拍，则跳过亮度补间，从原 star 头拍全亮；之后按当前模式的正常判定逻辑消失。恰好在启动拍进入 Perfect、稍后进入 Perfect、没有启动拍或关闭父选项时，保持现有行为。关闭父选项会保留子选项的勾选状态。

预览、视频导出及 worker 共用此规则，默认继承预览设置，支持独立调整。CLI 参数为 `--slide-timing-early-perfect-from-head`，需搭配 `--slide-timing-from-launch-beat`；视频还需开启 `--slide-judgement-timing`。时值统计提供 `fullBrightFromHead`，跳过补间时起点和全亮时刻均等于原头拍。

详见 [功能规格](../specs/preview/SLIDE_JUDGEMENT_TIMING_ZH.md)。

## 本地验证

2026 年 10 月 29 日，Windows x64 Release，仅使用工作区合成谱面及独立 runtime。

- 9 项聚焦 Spec 通过，覆盖 616 条形状、提前/恰好/晚于启动拍进入 Perfect、负偏移、父选项依赖、缓存切换、三种渲染模式的正常擦除，以及导出页子选项状态。
- OpenGL、D3D11 各 15 类真实 QSG 样例通过。跳过补间后的头拍、等待期和后续擦除帧均与正常材质逐像素一致；不符合条件的样例保留原补间，片段导出保留完整谱面启动拍。
- CLI 合成谱面验证 tap、hold 头、star、A 区 touch 四类启动拍，原 Perfect 区间和偏移不变，结构化请求的 false 值及无父选项回退通过。
- 子选项开关前后各 360 帧的导出有 62 帧产生预期变化；120 FPS 片段截断在启动拍之前仍正确应用子选项。
- 关闭辅助的 120 帧与固定编码配置的原始基线一致；整段、120 FPS 片段导出及运行目录隔离拒绝检查通过。

本次未进行主程序手动 GUI 验收；Windows ARM64、macOS 尚未完成本地验收。本地包不等同于已发布 GitHub Release。
