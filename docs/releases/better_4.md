# Better Miacode 2.0.0-better.4

本次更新集中改进对象选择、片段编辑、达成率工具和视频配套输出。Better Miacode 是 [MiaCode](https://github.com/Team-MiaCode/MiaCode) 的独立派生版本，不包含手序版或自动手序实验代码。

## 编辑与选择

- 源码、时间轴和预览同步选择独立音符。Ctrl/Command 追加选择，预览 Shift 拖动框选，Ctrl/Command + Shift 拖动追加框选；预览选择开始时暂停。
- 时间轴支持 Alt + 鼠标滚轮缩放，并保留已有缩放快捷键。
- 对象选择支持复制、剪切、粘贴、删除、镜像、旋转和属性变换。Delete / Backspace 删除对象并保留空拍、逗号和时序指令，Esc 清除选择。
- 剪贴板同时保留 simai 文本及带时序上下文的对象片段。粘贴自动对齐完整音符或拍首，必要时补充拍分隔，保留未替换的原 token 和分隔字符；只插入一次，随后恢复目标 BPM、分音、HS 和拍号。片段不带终止 `E`，不在多个选点重复插入。
- 常用变换默认键：Ctrl+J / K 左右及上下镜像，Ctrl+L 旋转 180°，Ctrl+; / Ctrl+' 旋转 ±45°。可在快捷键设置中调整。

## DX 失分、BPM 与 HUD

- 新增独立 DX 达成率失分窗口，显示各类计分物件和附加属性数量、详细 Break 判定失分，以及维持 SSS 100% / SSS+ 100.5% 的最大判定个数。上限不超过实际可达数量，EX 判定有独立约束。
- 空谱不计算；无 Break 的实际满分是 100%。Mine 等无官方 DX 计分定义的扩展保留统计但禁用失分计算；纯视觉 HS 不因此禁算。
- 新增 BPM 预览、复制和保存：BPM 顺序、带 `&first` 的音频时间，以及移除音符的 maidata。空 maidata 保留源码终止拍位 `E`，不会保留被删除长音符超出该位置的尾部时长，也不会虚构 BPM 补长。
- 宽屏 BPM / CPS / Meter / Mods 四项 HUD 分别开关，默认关闭，并进入导出快照。CPS 按过去一秒 `(t-1, t]` 内完成的计分事件统计，长条结束、Slide 轨迹结束才计数，Mine 不计；BPM 不乘播放速率，Meter 只显示显式拍号。
- 判定线新增 None / 无，覆盖暂停判定区及自定义图片回退；其他辅助图层仍可单独开关。

## 封面与宠物

- 文字图层增加可保存、复制的描边和阴影参数。
- 单曲视频可同时生成预设封面，冻结触发时的预览时间，处理普通片头、PV 预览和负时间；批量封面使用各预设保存的时间。封面失败会报告，成功的视频仍保留。
- 狐狸使用播放时钟和解析器的四分音符相位做平滑节拍动作，跟随 BPM 变化、速率、seek 和暂停。

## CLI

在“设置 → 关于”启用命令行集成，重新打开终端后使用 `better-miacode`。

```sh
better-miacode --help
better-miacode --version
better-miacode capabilities --json
better-miacode stats --chart maidata.txt --difficulty 4 --json
better-miacode presets list --json
better-miacode export video --chart maidata.txt --difficulty 4 --output out.mp4 --json
better-miacode export cover --chart maidata.txt --output cover.png --preset builtin:card --time 12.5 --json
better-miacode export bpm --chart maidata.txt --output bpm.txt --format sequence
```

支持 `--request 文件|-` 的 schemaVersion 1 JSON 请求。BPM 格式为 `sequence|audio-time|maidata`，封面默认 `builtin:card`。请求文件路径相对于该 JSON 文件；stdin 和 flags 路径相对于 cwd。`--json` / `--ndjson` 渲染输出为逐行进度和最终结果。已有文件需 `--overwrite`。

退出码：`0` 成功、`1` 任务失败、`2` 请求/谱面无效、`3` 输出冲突、`77` 未启用、`78` runtime 校验失败、`130` 取消。完整请求示例见 [README](../../README.md#命令行)。

## 包与验证边界

发布目标：Windows x64、Windows ARM64、Apple 芯片 macOS（13+）。从 Windows 包根目录 `MiaCode.exe` 或 macOS 的 `MiaCode.app` 启动。Windows 数据保存在包内独立 `runtime/`，macOS 数据保存在 `~/Library/Application Support/BetterMiacode`；不迁移正式版或手序版数据。

macOS 包保留现有签名流程，尚未完成 Apple notarization。Linux 未列为本次发布包目标。

以上列出当前功能实现，不宣称所有平台、第三方皮肤、GUI 场景或导出组合均已验收。最终可用附件、SHA-256、平台 CI 和本次实际验收结果，以 Release 最终报告为准；通过部分核心规格不等于完整跨平台验收通过。

下载与反馈：[Better Miacode Releases](https://github.com/albertoBradish/Better_Miacode/releases) · [Issues](https://github.com/albertoBradish/Better_Miacode/issues)。
