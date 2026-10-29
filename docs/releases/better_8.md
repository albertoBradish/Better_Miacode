# Better Miacode 2.0.0-better.8

本次公开版本合并 better.5～better.8 的 Slide 判定时值显示、渲染修复和启动拍选项。以下为相较于上一公开版本 better.4 的变化。

## Slide 判定时值与补间

- 预览“画面选项”和视频导出页新增“显示 Slide 判定时值”，默认关闭。开启后，从 star 头拍沿路径按弧长匀速将低亮轨迹恢复为正常亮度，在最早可获得 Perfect 的时刻全亮。头拍时已进入 Perfect 的极短或零等待 slide 直接全亮。
- fes 组合 slide 按整条轨迹补间，以末段计算最终判定窗口。保留普通、Each、BREAK、WiFi 和原皮肤材质；BREAK 不增加专属细分区间。WiFi 仍使用规格中注明的对称参考模型。
- 修复补间轨迹未随判定消失的问题，沿用当前渲染模式的正常擦除；修复部分 `pp/qq` 重叠路径误投到另一圈造成的亮度跳段。
- 预览和导出共用时间、路径与显式帧采样规则，拖动进度、倒退播放和片段导出不依赖之前的播放历史。

## 启动拍与子选项

“有启动拍时从启动拍开始”默认关闭。开启后，同一时刻、同一起点的 star、tap、hold 头和 A 区 touch 可作为启动拍；没有匹配时仍从原 star 头拍开始。隐藏头、mine、hold 尾和已经按住的 hold 本体不作为启动拍。

新增“启动拍前已进入 Perfect 时，从头拍全亮”，位于“有启动拍时从启动拍开始”下方，默认关闭。只有两个上层开关都开启时可勾选。

勾选后，若 slide 匹配到启动拍，且最早 Perfect 时刻早于启动拍，则跳过亮度补间，从原 star 头拍全亮；之后按当前模式的正常判定逻辑消失。恰好在启动拍进入 Perfect、稍后进入 Perfect、没有启动拍或关闭父选项时，保持现有行为。关闭父选项会保留子选项的勾选状态。

预览、视频导出及 worker 共用此规则，默认继承预览设置，支持独立调整。CLI 参数为 `--slide-timing-early-perfect-from-head`，需搭配 `--slide-timing-from-launch-beat`；视频还需开启 `--slide-judgement-timing`。时值统计提供 `fullBrightFromHead`，跳过补间时起点和全亮时刻均等于原头拍。

详见 [功能规格](https://github.com/albertoBradish/Better_Miacode/blob/v2.0.0-better.8/docs/specs/preview/SLIDE_JUDGEMENT_TIMING_ZH.md)。

## CLI

在“设置 → 关于”启用命令行集成后，可使用：

```sh
better-miacode stats --chart maidata.txt --difficulty 5 --slide-judgement-timing --json
better-miacode export video --chart maidata.txt --output movie.mp4 --slide-judgement-timing --slide-timing-from-launch-beat --slide-timing-early-perfect-from-head
```

时值统计包含尾判正解时刻、Perfect 区间、补间起点、实际全亮时刻及启动拍和跳过补间标识；`&first` 仅应用一次。三个开关均支持结构化 JSON 请求。

## 包与验证

发布目标为 Windows x64、Windows ARM64 和 Apple 芯片 macOS（13+）。每个平台附件同时提供 SHA-256 和自动验证报告；发布流水线要求三个平台的归档校验通过，且来源为同一发布提交。

2026 年 10 月 29 日，Windows x64 Release 本地候选包已通过用户验收。以下自动检查仅使用工作区合成谱面及独立 runtime：

- 9 项聚焦 Spec 通过，覆盖 616 条形状、提前/恰好/晚于启动拍进入 Perfect、负偏移、父选项依赖、缓存切换、三种渲染模式的正常擦除，以及导出页子选项状态。
- OpenGL、D3D11 各 15 类真实 QSG 样例通过。跳过补间后的头拍、等待期和后续擦除帧均与正常材质逐像素一致；不符合条件的样例保留原补间，片段导出保留完整谱面启动拍。
- CLI 合成谱面验证 tap、hold 头、star、A 区 touch 四类启动拍，原 Perfect 区间和偏移不变，结构化请求的 false 值及无父选项回退通过。
- 子选项开关前后各 360 帧的导出有 62 帧产生预期变化；120 FPS 片段截断在启动拍之前仍正确应用子选项。
- 关闭辅助的 120 帧与固定编码配置的原始基线一致；整段、120 FPS 片段导出及运行目录隔离拒绝检查通过。

Windows ARM64、macOS 未进行人工 GUI 验收；各平台自动检查范围以附件中的验证报告为准。macOS 沿用现有签名流程，尚未完成 Apple notarization。

Windows 完整解压后从根目录 `MiaCode.exe` 启动；macOS 打开 `MiaCode.app`。运行配置继续与原版及手序版隔离，不自动迁移偏好、恢复数据或最近文件。
