# Slide 判定时值与亮度补间

## 功能

画面选项中的“显示 Slide 判定时值”默认关闭。打开后，从谱面 star 头拍立即开始，将低亮轨迹沿真实美术路径按弧长匀速恢复为正常亮度。尚未判定消失的轨迹在最早可获得 Perfect 的时刻恰好恢复正常亮度。初始等待计入动画时长，原引导星星仍按谱面时序移动。

普通、Each、BREAK 及第三方皮肤保留原材质和透明轮廓；低亮为正常 RGB 的 25%，不降低轨迹 alpha。亮度补间附加在当前渲染模式保留下来的轨迹上，照常复用常规擦除、逐区消失以及无理检测的实际判定完成时间。已消失区域不因补间重新出现。关闭开关恢复原渲染。新功能不改变音符判定、正解音、SFX 或无理检测策略。

视频导出提供独立开关，默认取预览值，导出试听结束后恢复预览值。设置通过导出快照传给 worker。缺少新字段的旧快照默认关闭。

## 计算

`h` 为头拍，`s` 为起滑，`e` 为几何终点，末段移动时长为 `D_last`，末段形状系数为 `p_last`：

```text
T = D_last × (1-p_last)
c = e-T
b = max(h, PerfectStart)
u(t) = clamp((t-h)/(b-h), 0, 1)  （b>h）
恢复正常亮度的路径长度 = 整条路径弧长 × u(t)
```

单段时 `D_last=e-s`。fes 链使用末段时长计算最终 `c`；亮度按整条累计弧长推进，接点不重置。无头 slide 使用虚拟头拍 `h`。同头分叉各自独立计算。

普通 slide 的参考 Perfect 窗口，`F=1/60 秒`：

```text
H = min(36F,14F+T/4)
PerfectStart = c-max(17F,H)
PerfectEnd = c+H
```

`c` 保留为尾判正解基准，补间终点改用 Perfect 窗口起点 `PerfectStart`。如果该时刻不晚于头拍，则按用户确认的规则在头拍直接全亮，头拍前保持低亮；不计算零或负时长除法。计算不依赖 Muri 的放宽窗口、24 小时过期或自身收束兜底。WiFi 保留当前资料的独立对称参考窗口 `c±H`；其额外 Fast 区间尚待机台核实，不宣称为所有版本机台规范。BREAK 不设特殊细分区间。

形状系数沿用内嵌 `slide_data.json`，不是按屏幕 A 区碰撞重算。系数与几何入 A 边界有偏差时，以尾判系数为准。系数缺失、非有限时间、无有效弧长或 `c≤h` 时跳过辅助轨迹，并通过 Runtime 调试日志报告，不生成瞬间补间。

## 渲染与缓存

`core/chart/SlideJudgementTiming` 提供纯时间模型。`core/scene/PreviewSlideTiming` 在 prepared scene cache 中保存箭头的累计弧长投影与切线。箭头方向用于区分自交和同路折返；不能按数组顺序或采样下标代替弧长。

GPU 在每个原皮肤图元内按弧长前沿连续调节亮度，使用屏幕导数抗锯齿。到达 `b` 时，当前仍可见的所有图元（包括美术外伸部分）全部恢复正常亮度。轨迹是否可见由原判定擦除链路决定，补间不另建轨迹生命周期。正常和低亮图元共享纹理与批次，不逐帧生成染色图片。

预览、OpenGL 和 D3D11 导出共用场景状态，按显式 chart time 采样。seek、倍速和片段首帧不依赖先前播放历史。视频帧率仅影响可见帧，不对 `b` 取整。

## CLI

```text
better-miacode stats --chart maidata.txt --slide-judgement-timing --json
better-miacode export video --chart maidata.txt --output movie.mp4 --slide-judgement-timing
```

`stats` 的 `slideJudgementTimings` 提供逐条 shape、源码行列、`h/s/e/c`、头拍至正解时长、末区时长、Perfect 起止时刻和实际全亮时刻 `brightnessCompleteSecond=b`。绝对时刻统一应用 `&first` 一次。WiFi 返回 `wifi-symmetric-reference` 模型标识。默认 stats 输出保持原字段集。

## 验证入口

- `preview_slide_timing_spec`：全 616 条形状、fes 尾判、快速/零等待、窗口上限、折返投影、缓存、精确终点、缺数据处理。
- `video_export_visual_aids_spec`：新开关快照、旧快照、用户设置复制、两个导出后端的显式帧状态。
- `miacode_slide_timing_render --skin <目录> --output <目录> --backend opengl|d3d11`：真实 QSG 图元截图，验证 `b` 处剩余轨迹与正常材质逐像素一致、后续擦除一致及 seek 重放一致。

机台入判前导、WiFi 容错与大 V 开头 fes 的跳区特例仍属于单独研究；本功能不模拟这些输入队列行为。
