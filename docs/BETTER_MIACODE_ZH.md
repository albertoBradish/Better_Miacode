# Better Miacode 功能说明与验收边界

better.8 为“有启动拍时从启动拍开始”新增子选项“启动拍前已进入 Perfect 时，从头拍全亮”。默认关闭；勾选后，符合条件的 slide 从原 star 头拍全亮并沿用正常判定消失。预览与视频导出均支持。

better.7 修复部分 pp/qq 重叠路径的亮度跳段，新增“有启动拍时从启动拍开始”独立选项，预览和视频导出共用匹配规则。

better.6 修正 Slide 补间的判定消失，并将全亮终点设为最早 Perfect 时刻；头拍时已经进入 Perfect 范围的情况直接全亮。

better.5 新增“显示 Slide 判定时值”：头拍起按路径弧长匀速将低亮轨迹恢复正常亮度，最早 Perfect 时刻全亮，已判定区域照常消失；预览和视频导出可分别开关，默认关闭。CLI 的 `--slide-judgement-timing` 可用于视频导出和逐条时值统计。规则与参考模型边界见 [Slide 判定时值规格](specs/preview/SLIDE_JUDGEMENT_TIMING_ZH.md)。

better.4 新增时间轴 Alt + 鼠标滚轮缩放，保留已有快捷键配置；时间轴上的 Alt 手势不会触发预览判定区临时显示。

Better Miacode 是操作体验改进版本，基线为 Team-MiaCode/MiaCode 的
`main@7feadff91644fea4b1f8e2c1c0de71b3f35233da`。独立仓库不包含手序版提交。
本文记录功能契约及分版本验证边界。当前目标版本为 `2.0.0-better.8`；下方 better.1～better.4 的历史验证结论不自动覆盖本次改动。better.8 的验证边界见 `docs/releases/better_8.md`；跨平台结果以对应平台 CI 和实际验收为准。

## 分批功能

1. 行号单击：暂停，并同步定位到解析器记录的该行起始时间；空行、注释行也可定位。书签边栏保持原操作。
2. 小节线：画面选项开关默认关闭。完整灰白细圆环随 tap 的基础流速进场，到拍位判定圆后消失。首选项支持谱面 `clock_count`（无效时 4）、自定义正整数 X 拍和源码行首（同时间去重）。BPM 变化不重置累计拍位，不改变原开场计数逻辑。
3. 选区：按独立音符的实际源码区间匹配，部分 token 选中整个音符；同拍音符独立，链式 slide 整体。黄色描边覆盖时间轴和预览中的头、身体、轨迹和移动星星。选区不写谱面、不进入导出。
4. 播放选区：右键菜单和调整菜单共用命令；无音符时禁用。保留进场预卷，播放原谱上下文，所选完整长音符与效果结束后暂停。范围在起播时固定；更改选择不改变播放边界，文本修改、换谱、手动 seek、停止会取消。
5. 判定骨骼：画面选项开关默认关闭。按实际判定区中心画青白细直线，保留候选组关系和 Wifi 分路；随轨迹出现，随判定消退，不依赖手序算法或开启无理分析。

小节线和判定骨骼使用共享场景和显式帧时间，导出可独立开关，默认继承预览；缺少可靠判定数据时不猜测骨骼。

## 隔离与本地构建

独立 Qt 身份为 BetterMiacode。GUI、CLI 和 worker 使用绝对路径
`BETTER_MIACODE_RUNTIME_ROOT`；不迁移标准版或手序版数据。便携包通过根目录
`MiaCode.exe` 启动，运行资料位于独立 `runtime/`。官方更新不可覆盖此包。

本工作区可点加载 `scripts/build/enter-better-miacode-build.ps1`，执行
`cmake --preset better_miacode` 和 `cmake --build --preset better_miacode_release`。
本地 CMakeUserPresets 不提交。构建使用 Release，并发最多 4。
源码、构建、归档、合成测试资料均位于当前工作区；不更新手序版固定测试安装。

## 验收与交付

- 隔离路径先验证，再运行应用；测试仅使用合成谱面。
- 检查行号、空行、注释、变速、折行和书签区域。
- 检查 `clock_count`、自定义间隔、行首去重、BPM 和偏移；比对预览和导出同帧。
- 检查独立/部分 token、链式/长音符、反向选择、中文注释、过期 revision、范围播放结束与取消。
- 检查直线/弧线/折线/链式/Wifi 骨骼及逐区消退。
- 按受影响模块执行聚焦测试，再做 Release 打包、归档完整性及 SHA-256 校验。
- 独立提交运行隔离与功能，推送用户仓库；不向上游推送或自动创建 PR。

## 本次交付验证（2026-10-27）

版本：`2.0.0-better.1`，Windows x64 Release；版本号沿用所选上游 main 的 2.0.0 基线。

- 8 项聚焦规格全部通过：runtime_isolation、qml_scintilla_editor、editor_sync_controller、playback_coordinator、timeline_model、preview_head_layer、preview_visual_aids、video_export_visual_aids。
- GUI 已检查行号跳至对应时间并暂停、选中 slide 在时间轴与预览同时黄色描边、右键和调整菜单的播放选区、完整尾部结束暂停、画面辅助开关及首选项三种小节线模式。
- 便携包 worker 已导出合成谱面，抽帧确认完整灰白圆环及判定中心折线。未使用真实谱面。
- 实际程序在无效运行根下以 78 拒绝启动；配置与日志位于 Better Miacode 独立目录。
- GUI 验收发现并修复了预览图层数量不足导致末尾描边层未执行的问题，增加了图层分配与使用数量一致的回归检查。
- 本机使用 FFmpeg 8.1 共享 SDK；打包清单同步到该 ABI。构建依赖的二进制文件不提交源码仓库。

此次覆盖 Windows 和默认皮肤的合成用例；其他平台和第三方皮肤未做 GUI 验收。
最终归档哈希由包旁的 `.sha256` 文件记录。先解压完整包，再启动根目录 `MiaCode.exe`。

## 2.0.0-better.2 视觉细化

- 小节参考线使用按实际帧像素计算的抗锯齿圆环，消除固定 48 段圆环的折线感；颜色、粗细和拍位语义保持原设置。
- 时间轴高亮改为约 1 DIP 的淡黄色细外轮廓，并与音符留出少量间隙。封闭内孔不再附加高亮，密集 slide 箭头的轮廓进一步减淡，避免遮住原图。
- 预览高亮改为约 1 屏幕像素的圆形细轮廓；Hold 切片接缝和分数缩放单独检查。
- Release 构建及 preview_head_layer、preview_visual_aids、video_export_visual_aids 三项聚焦规格通过。小节线完成 360、540、1080、2160 像素的离屏输出检查。
- 新版打开后由用户验收最终视觉效果。

开发工具 `miacode_measure_ring_render --guide <assets/noteguide/Normal.png> --output <输出目录>`
可输出原版 note 参考线与小节线的并排图；左侧为原版，右侧为小节线。

## 2.0.0-better.3 打开文件校验

- 打开文件时在读取前检查扩展名，只接受 `.txt` 和 `.simai`，不区分大小写；粘贴其他类型的文件路径会显示错误及文件路径，不再进入谱面解析。
- 文件夹仍解析为其中的 `maidata.txt`；保留纯谱面正文、空难度和带语法错误谱面的既有打开行为。
- 启动及会话恢复的运行时读取入口使用相同格式限制。拒绝打开时保留当前文档、未保存修改和打开代次。
- Windows x64 Release 构建通过；chart_workspace_file_service、document_model_save、runtime_isolation 三项聚焦规格通过。打开错误经界面模型入口验证，未进行原生文件对话框的手动 GUI 验收。


## 2.0.0-better.4 对象编辑、工具与导出

本节描述当前实现的操作和数据契约，不把局部核心测试通过等同于完整发布验收。发布说明见 [better.4](releases/better_4.md)，下载入口为 [Better_Miacode Releases](https://github.com/albertoBradish/Better_Miacode/releases)。

### 对象选择和编辑

源码、时间轴及预览共享解析器生成的独立音符源码范围。部分 token 的命中归属于完整音符；同拍、紧凑数字写法中的独立音符可以分别选择，链式或分支 Slide 归属于其完整源码对象。选择带难度、revision 和场景代次，源文档或场景更换后不继续应用过期结果。

| 入口 | 操作 |
| --- | --- |
| 源码 | Ctrl/Command + 单击音符，追加对象选择；普通拖选保持文本选择 |
| 时间轴音符 | Shift + 单击选择；Ctrl/Command + 单击追加 |
| 预览 | Shift + 拖动框选当前可见音符；Ctrl/Command + 单击追加；Ctrl/Command + Shift + 拖动追加框选 |
| 对象选择模式 | Delete / Backspace 删除；Esc 清除选择 |

预览按当前帧实际可见的音符几何命中，包括 Hold 身体、Slide 轨迹和移动星星。启动选择时暂停，拖动期间固定参与命中的场景；场景版本变化时拒绝该次选择。预览中仅按 Ctrl/Command 拖动时，应从音符上开始。

对象选择进入原有变换命令：镜像、旋转、分音和 Break / Ex / Firework 等属性操作。对象删除移除音符及相关同拍连接符，保留空拍、逗号、BPM、分音、HS 和拍号；普通文本选择仍走文本编辑，不自动改成对象删除。删除、剪切、粘贴及变换参与正常撤销/重做。

| 常用默认命令 | 快捷键 |
| --- | --- |
| 复制 / 剪切 / 粘贴 | Ctrl+C / Ctrl+X / Ctrl+V；macOS 标准编辑命令按系统显示 |
| 左右镜像 / 上下镜像 | Ctrl+J / Ctrl+K |
| 旋转 180° | Ctrl+L |
| 逆时针 / 顺时针旋转 45° | Ctrl+; / Ctrl+' |
| 播放/暂停 / 停止或播放 | Ctrl+Shift+X / Ctrl+Shift+C |

变换及播放快捷键使用现有注册表，可在首选项配置；不将这些既有键位描述为本次新增的快捷键。

### 带上下文的剪贴板片段

对象复制同时写入普通 simai 文本及 `application/x-better-miacode-note-fragment-v1` 数据。片段在首个选中音符前补齐生效的 BPM、分音、HS 和拍号，并保留选中范围内的时序变化及空拍；未选中的同区间音符被移除。片段不包含终止 `E`，便于粘贴到其他编辑器。

Better Miacode 识别片段后只插入一次：有对象集合时先删除该集合，再在最早选中拍前插入；文本选区涉及音符时也按完整对象替换。没有替换对象时，插入点自动对齐到完整音符或拍首：音符 token 内部移到该音符前，指令内部或同拍分隔处移到拍首，注释内移到注释前；若插入位置之前已有本拍音符，则补充必要的逗号。未被替换的原 token 和分隔字符保留。片段结束后恢复目标位置原本的 BPM、分音、HS、拍号，使后续原谱继续按目标时序解释。粘贴不是按绝对秒数铺开音符，也不会把同一片段重复插入到每个选点。

其他程序丢弃专用 MIME 数据后，剪贴板仍有可读文本；此时按普通文本粘贴，不承诺自动恢复目标时序。

### 权威时间轨

解析器提供 BPM、显式拍号标记及连续的四分音符累计拍位，HUD、BPM 导出及狐狸共用该时间轨。相同时间的赋值按源码顺序取最后值，连续相同状态合并。负时间和最后一处 BPM 之后的相位按当时 BPM 外推；显示 BPM 使用原谱值，不乘播放速率。

音符和 HUD 在播放及导出中使用含 `&first` 的统一音频时轴；BPM 文本导出在需要时把谱面时间换算到音频时间。UI 不再用一套独立正则重新推断 BPM。

### DX 达成率失分

独立工具窗口统计 Tap / Hold / Star / Slide / Touch / TouchHold 六类基础物件，以及 Break / Ex / Firework 属性和计分 Total。属性与基础类别重叠，但不会再次加进 Total；Mine 单独列出，不进入计分分母或 CPS。同头 Slide 的头部计一次，各轨迹完成事件分别计数。

普通 Tap、Star、Touch 的基础分为 500，Hold、TouchHold 为 1000，Slide 为 1500，Break 为 2500。普通物件的 Great / Good / Miss 分别损失基础分的 20% / 50% / 100%。Break 额外分母为 `100 × Break 数量`，折合总计 1 个达成率百分点。

| Break 判定档 | 基础分损失 | 额外分损失 |
| --- | ---: | ---: |
| Critical Perfect 2600 | 0 | 0 |
| Perfect 2550 | 0 | 25 |
| Perfect 2500 | 0 | 50 |
| Great 2000 | 500 | 60 |
| Great 1500 | 1000 | 60 |
| Great 1250 | 1250 | 60 |
| Good 1000 | 1500 | 70 |
| Miss | 2500 | 100 |

单个判定的失分为 `100 × 基础分损失 / 基础分总额`，Break 再加 `额外分损失 / (100 × Break 数量)`，单位为达成率百分点。SSS 100% 与 SSS+ 100.5% 的最大个数假设其余全部 Critical Perfect，以精确整数比例比较边界后再显示舍入，并以实际可达物件数量为上限。

EX 的容错数量不能直接等同于普通物件：EX Tap / Star 的非 Miss 头判提升到 Critical Perfect；长 EX Hold 仍可能因松手发生尾判降档，Break EX 也按具体可达档位限制。表格给出判定本身的单次失分，最大个数遵守这些约束。

空谱禁算；无 Break 时实际满分为 100%，不能达到 SSS+ 100.5%。存在 Mine 等无官方 DX 计分定义的扩展时仍显示统计，禁用失分计算。HS 仅影响视觉，不因此单独禁算。

### BPM 导出与宽屏 HUD

BPM 窗口支持预览、复制和另存为三种格式：

- 顺序：`135.00-128.00`。
- 音频时间：`00:00:000/135.00 - 01:23:456/128.00`，时间包含 `&first`，支持负号。
- 空 maidata：移除全部音符，保留高精度 BPM、解析器的正整数分音、显式拍号、`&first` 和源码终止拍位 `E`。

前两种合并连续同值 BPM，同刻最后赋值生效；用于显示的两位小数不用于空 maidata 重建。空 maidata 重新解析后的有效 BPM 时点及终止拍位应一致。其“结束位置”指源码 `E` 的拍位；删除长 Note 后不保留已消失的尾部时长，不人为延长渲染 duration，也不添加虚假 BPM。

宽屏的 BPM / CPS / Meter / Mods 四项各自开关，默认关闭，设置进入快照和 worker：

| HUD | 含义 |
| --- | --- |
| BPM | 当前原谱 BPM，不乘速率 |
| CPS | 过去一秒 `(t-1, t]` 内完成的计分事件数 |
| Meter | 只有谱面显式设置拍号时才显示 |
| Mods | 实际无理检测渲染为 `DJAUTO`，普通渲染为 `NONE` |

CPS 的 Hold / TouchHold 在结束时计数，Slide / Wifi 在轨迹结束时计数；头部按自身判定事件计数，Mine 不计。`MaimuriDxStyle` 对应 `DJAUTO`，`Native` 和普通区域擦除模式对应 `NONE`。

### 判定线、文字效果、同步封面与狐狸

判定线新增 None / 无。选择后，暂停判定区替换、自定义判定线图片及其回退都不能恢复该图层；小节参考线、判定骨骼和其他辅助图层仍由各自开关控制。

文字图层增加独立描边和阴影：描边开关、颜色、宽度；阴影开关、颜色、透明度、模糊、横纵偏移。参数进入图层复制、布局 JSON、保存和加载；旧布局缺少字段时默认关闭。效果距离以 1080 高画布像素换算，编辑预览与最终 PNG 使用同一组合场景；高 DPI、宽画布、大字和透明输出仍需像素验收。

单曲视频导出可选择封面预设；预设包括内置、已保存的用户预设及布局文件。触发导出时固定当前预览时间和任务数据，同一时刻的拼贴图层复用冻结图像，避免视频完成时再读取已移动的预览。普通片头的负时间、PV 预览保留段与实际 PV 采样分别换算；批量封面使用预设保存的各图层时间。视频成功后按实际视频输出名写配套 PNG；封面写入或渲染失败应报告并保留成功视频，取消或失效任务不留下配套输出。

狐狸按权威四分音符相位采样连续的两拍摇摆周期：身体每拍宽弧轻弹，头部滞后约 0.10 拍转向、抬下巴，尾巴滞后约 0.24 拍跟随；前后腿反相屈伸并补偿身体高度。启停权重用 300ms 缓动，点击互动期间降低节奏动作，2 秒后恢复，播放时不进入自主坐下或睡眠循环。变 BPM、倍速和 seek 均重新采样播放相位，宠物窗口不另行积累节拍。

开发工具 `pet_groove_preview --bpm 60,150,240 --fps 60 --output <工作区输出目录>` 使用生产 Fox 模型与 PetScene 进行离屏渲染，不显示桌面窗口。输出 PNG 序列及 manifest，检查两拍循环一致性、接缝连续性、权重归零、有限变换和像素重复；`--model` 可指定旧模型做动作对照。

### CLI 与请求协议

“设置 → 关于”提供命令行集成开关，命令为 `better-miacode`。注册在用户范围内；启用或关闭只维护本安装拥有的脚本和 PATH 登记，不覆盖同名外部命令，失败时回滚可回滚的变更。启用后重新打开终端。任务执行使用同一独立 runtime，关闭开关返回 77；元信息命令不要求开关或 runtime 已配置。

| 命令 | 主要参数 |
| --- | --- |
| `--help` / `help`、`--version` / `version`、`capabilities` | 可加 `--json` |
| `stats` | `--chart`、`--difficulty`（默认 4）、`--json` |
| `presets list` | 可加 `--json`；`presets` 也是有效写法 |
| `export video` | `--chart`、`--output`、`--difficulty`、尺寸、fps、起止区间、流速和 HUD 参数 |
| `export cover` | `--chart`、`--output`、`--difficulty`、`--preset`、`--time` |
| `export bpm` | `--chart`、`--output`、`--difficulty`、`--format sequence|audio-time|maidata` |

视频输出 `.mp4`；封面输出 `.png` / `.jpg` / `.jpeg`，默认 `builtin:card`。其他内置预设为 `builtin:card_chart_frame`、`builtin:dual_chart_frames`、`builtin:pure_chart_frame`，用户和文件预设为 `user:名称`、`file:路径`。BPM 格式默认 `sequence`。`--start`、`--duration`、`--time` 均为秒，封面 `--time` 可为片头负时间。视频宽度不能小于高度。渲染仍需要可用图形环境及媒体依赖。

`better-miacode --request request.json` 接受一个 schemaVersion 1 对象：

```json
{
  "schemaVersion": 1,
  "command": "export",
  "type": "video",
  "chart": "maidata.txt",
  "difficulty": 4,
  "output": "out.mp4"
}
```

JSON 属性名沿用参数名，例如 `width`、`height`、`fps`、`duration`、`json`、`overwrite`；不要使用另一套嵌套命令格式。文件请求内的路径相对该 JSON 文件目录；`--request -` 从 stdin 读取，stdin 与 flags 路径相对 cwd。flags 可覆盖请求中的相应值。

`--json` / `--ndjson` 的视频、封面渲染输出是 NDJSON 事件流：`type: "progress"` 的进度行，随后一个 `type: "result"`。统计、BPM、元信息返回一个结果。结果信封使用 `schemaVersion: 1`、`ok`、`exitCode` 以及 `data` 或 `error: {code, message}`。调用方应逐行解析，并检查最终退出码。

已有输出需要 `--overwrite`。任务先在临时位置完成，成功后原子替换明确指定的输出；失败保留原有文件。

| 退出码 | 含义 |
| --- | --- |
| 0 | 成功 |
| 1 | 任务失败 |
| 2 | 请求或谱面无效 |
| 3 | 输出冲突 |
| 77 | CLI 任务未启用 |
| 78 | runtime 隔离检查拒绝 |
| 130 | 取消 |

### 平台和本次验收边界

本次目标是 Windows x64、Windows ARM64 和 Apple 芯片 macOS（13+）。Windows 使用包根目录 `MiaCode.exe` 和包内 `runtime/`；macOS 使用 `MiaCode.app`，用户数据位于 `~/Library/Application Support/BetterMiacode`，不向 `.app` 内写用户资料。macOS 走现有签名流程，尚未 notarize。Linux 仍可从源码构建，不在本次发布包目标中。

以下事项不能由单个平台或部分 Spec 的通过代替：

- Windows ARM64 和 macOS 的完整依赖、Release 构建、打包及原生运行。
- 三平台的 CLI 注册/关闭/冲突回滚、路径含空格或中文、退出码及 NDJSON 消费。
- GUI 对象选择和片段编辑、第三方皮肤、不同 DPI 与自由宽屏布局。
- 文字描边/阴影透明 PNG、片头/PV 时间对齐、同步封面失败及取消。
- 预览与 worker 对应帧、最终归档完整性、文件清单与 SHA-256。

本节不预填“全部通过”。实际通过范围、未验证项、可下载附件与哈希由最终验收报告及平台 CI 补充。
