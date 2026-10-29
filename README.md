功能速览：
1. 支持谱面失分计算
2. 播放选区片段
3. 点击行号数字快速跳转时间
4. Alt+鼠标滚轮缩放时间轴
5. 渲染视频时可选择一并用已有模板导出封面
6. 渲染视频时可选择显示更多调试信息（mod，bpm，meter, cps等）
7. 华卡风格的小节对齐参考线
8. 星星实际判定区骨骼
9. 预览框可Command (Ctrl) / Shift选择对应音符（可跨选区）
10. 支持导出BPM List
11. 封面导出时的文字对象可以使用描边&阴影
12. 补充了“无判定线”皮肤
13. 在预览/时间轴中高亮被选中的note
14. 允许通过CLI调用部分功能，例如渲染对应规格的谱面
15. 显示 Slide 判定时值，按最早 Perfect 时刻完成路径亮度补间；支持启动拍及提前 Perfect 子选项
16. 🦊会跟着谱面播放摇头！！！！




# Better Miacode

基于 [Team-MiaCode/MiaCode](https://github.com/Team-MiaCode/MiaCode) 的独立 maimai 谱面编辑器，使用 Qt 6 / C++ / QML，集成源码编辑、实时预览、语法与无理检测、视频导出和封面创作。Better Miacode 从上游 main 派生，重点改进音符选择、编辑和导出操作，不包含手序版或自动手序实验代码。

**下载 Better Miacode：[GitHub Releases](https://github.com/albertoBradish/Better_Miacode/releases)。** 源码、问题反馈和构建记录均在 [Better_Miacode](https://github.com/albertoBradish/Better_Miacode)。上游官网与上游 Releases 提供的是原版 MiaCode。

当前发布目标为 **2.0.0-better.9**，包含 Windows x64、Windows ARM64 和 Apple 芯片 macOS 包。功能说明描述当前实现；各平台构建、GUI、导出和安装是否完成验证，以本次 Release 的最终验收报告、附件和 CI 结果为准。macOS 要求 13 或以上；现有签名不代表已经完成 Apple notarization。

<p align="center">
  <img src="resources/screenshots/main-dark.png" alt="MiaCode 基础编辑界面；Better Miacode 在此基础上改进操作" width="960">
</p>

## 开始使用

- Windows：完整解压对应架构的包，从根目录 `MiaCode.exe` 启动。运行数据保存在该包独立的 `runtime/`。
- macOS：完整解压 Apple 芯片包，打开 `MiaCode.app`，也可移入“应用程序”。独立运行数据位于 `~/Library/Application Support/BetterMiacode`。尚未 notarize 的包可能收到系统安全提示。
- Linux：保留源码构建路径，本次未提供 Linux 发布包或完整平台验收结论。

Better Miacode 使用独立应用身份、配置、缓存、会话和 worker 运行目录，不自动迁移标准版或手序版的数据。不要用原版更新包覆盖 Better Miacode。

详细操作和边界见 [功能说明](docs/BETTER_MIACODE_ZH.md)，本次变更见 [better.8 发布说明](docs/releases/better_8.md)。

## better.8 的主要改进

- 新增“显示 Slide 判定时值”，从 star 头拍沿路径匀速将低亮轨迹恢复为正常亮度，在最早 Perfect 时刻全亮；已判定的轨迹照常消失。
- 新增“有启动拍时从启动拍开始”及“启动拍前已进入 Perfect 时，从头拍全亮”子选项，默认关闭。预览、视频导出和 CLI 共用规则。
- 修复部分 pp/qq 多圈重叠路径的亮度跳段，片段导出保留完整谱面的启动拍匹配。

## better.4 起提供的编辑与导出功能

### 从源码、时间轴和预览选择音符

- 在源码中按住 Ctrl（macOS 为 Command）单击音符追加对象选择；时间轴支持 Ctrl/Command 追加单击、Shift 单击选择。预览中使用 Shift 拖动框选可见音符，Ctrl/Command 单击追加，Ctrl/Command + Shift 拖动追加框选。
- 选择按解析器记录的独立音符范围同步到源码、时间轴和预览；同拍音符可分别选择，链式和同头分支 Slide 保持所属完整对象。预览命中使用当前画面的音符几何，开始选择时暂停播放。
- 时间轴新增 Alt + 鼠标滚轮缩放，保留原有自定义缩放快捷键；在时间轴使用 Alt 不会触发预览判定区辅助。
- 对象选择支持复制、剪切、粘贴、删除，以及已有的镜像、旋转和属性变换。删除音符保留逗号、空拍和时序指令；普通文本选择继续使用文本编辑行为。
- 复制同时提供可读的 simai 文本和 Better Miacode 片段格式。片段保留 BPM、分音、HS、拍号及中间空拍，不携带终止 `E`。粘贴在拍边界插入一次，随后恢复目标位置的时序；不把片段重复粘贴到每个选中音符。

常用默认键位如下，可在快捷键设置中调整变换命令；macOS 的标准编辑快捷键以系统显示为准。

| 操作 | 默认键位 |
| --- | --- |
| 复制 / 剪切 / 粘贴对象片段 | Ctrl+C / Ctrl+X / Ctrl+V |
| 删除对象选择 / 清除选择 | Delete 或 Backspace / Esc |
| 左右镜像 / 上下镜像 / 旋转 180° | Ctrl+J / Ctrl+K / Ctrl+L |
| 逆时针 / 顺时针旋转 45° | Ctrl+; / Ctrl+' |
| 播放或暂停 / 停止或播放 | Ctrl+Shift+X / Ctrl+Shift+C |

对象片段粘贴会自动对齐完整音符或拍首；在音符之后插入时补充必要的拍分隔。光标位于 token、同拍分隔或注释中时也会调整插入位置，保留未被替换的原 token 和分隔字符。外部程序只保留普通文本时，粘贴按文本处理。

### DX 失分与 BPM 工具

工具菜单提供两个独立窗口。

- **DX 达成率失分**：统计 Tap、Hold、Star、Slide、Touch、TouchHold、Break、Ex、Firework 和 Total，Mine 单列。Break、Ex、Firework 是附加属性，不重复加进总数。列出单个判定失分，以及其余全部 Critical Perfect 时维持 SSS 100% / SSS+ 100.5% 的最大个数；Break 分档计算，EX 数量按可达判定约束。空谱禁算，无 Break 的实际满分为 100%；包含无官方 DX 计分定义的扩展时保留统计并禁算。
- **BPM 导出**：预览、复制或保存 `135.00-128.00`、`00:00:000/135.00 - 01:23:456/128.00` 或空 maidata。连续同值合并，同刻最后赋值生效；音频时刻包含 `&first`，支持负时间。空 maidata 保留精确 BPM、分音、显式拍号、`&first` 和源码终止拍位 `E`。删除长音符也会移除其延伸到 `E` 之后的尾部时长，不添加虚假 BPM 补长。

### 预览、封面和狐狸

- 宽屏 HUD 的 BPM、CPS、Meter、Mods 四项分别开关，默认关闭，设置进入导出快照。BPM 显示原谱数值；CPS 统计过去一秒 `(t-1, t]` 内完成的计分事件，Hold 在结束、Slide 在轨迹结束时计数，Mine 不计。Meter 仅显示显式拍号；Mods 显示实际无理检测模式 `NONE` / `DJAUTO`。
- 判定线新增 **None / 无**，暂停时的判定区显示及自定义图片回退也不会重新画出判定线。小节参考线、判定骨骼等辅助选项保持独立。
- 文字图层增加独立描边和阴影，可设置颜色、宽度、透明度、模糊和偏移，并随布局复制、保存和加载。
- 单曲视频导出可同时生成预设封面，使用触发导出时冻结的当前预览时间；片头负时间与 PV 预览时间分别换算。批量封面继续使用预设保存的图层时间。封面失败会报告错误并保留已成功的视频。
- 狐狸跟随拍位轻弹身体，两拍完成一次左右甩头、抬下巴的摇摆，头部与尾巴依次跟随，腿部补偿重心变化。点击互动后会平滑回到节奏动作；变 BPM、播放速率、seek 和暂停均使用同一播放时钟，不另行累计节拍。

此前版本的小节参考线、判定骨骼、行号定位和播放选区功能继续保留。

## 命令行

在“设置 → 关于”启用命令行集成后，重新打开终端使用 `better-miacode`。关闭集成会禁用任务执行；`--help`、`--version`、`capabilities` 仍可查询。命令注册使用用户目录和独立 runtime，不要求管理员权限。

```sh
better-miacode --help
better-miacode --version
better-miacode capabilities --json
better-miacode stats --chart "maidata.txt" --difficulty 4 --json
better-miacode presets list --json
better-miacode export video --chart "maidata.txt" --difficulty 4 --output "out.mp4" --json
better-miacode export cover --chart "maidata.txt" --difficulty 4 --output "cover.png" --preset builtin:card --time 12.5 --json
better-miacode export bpm --chart "maidata.txt" --difficulty 4 --output "bpm.txt" --format audio-time
```

`export bpm` 的格式为 `sequence`（默认）、`audio-time`、`maidata`。封面默认预设为 `builtin:card`，也支持列出的其他内置预设、`user:名称` 和 `file:预设路径`。视频导出支持 `--width`、`--height`、`--fps`、`--start`、`--duration`、`--flow-speed` 等参数；时间单位均为秒。

也可保存一份 `request.json` 后运行 `better-miacode --request request.json`：

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

请求文件中的相对路径以请求文件所在目录为基准；`--request -` 从标准输入读取，标准输入和命令行参数中的相对路径以当前工作目录为基准。已有输出需要显式 `--overwrite`；输出先写临时文件，成功后原子替换指定路径。

渲染任务加 `--json` 或 `--ndjson` 会输出多行独立 JSON：零个或多个 `progress`，最后一个 `result`。不要把整个标准输出作为单个 JSON 对象解析。统计、BPM 和元信息查询返回一个结果。

| 退出码 | 含义 |
| --- | --- |
| 0 | 成功 |
| 1 | 任务失败 |
| 2 | 请求或谱面无效 |
| 3 | 输出冲突 |
| 77 | 命令行任务未启用 |
| 78 | 独立 runtime 校验失败 |
| 130 | 已取消 |

视频和谱面帧封面仍需要可用的图形环境及相应媒体依赖；CLI 不等于无 GPU 的渲染保证。

## 构建、验证与反馈

构建依赖、平台脚本和打包入口见 [scripts/README.md](scripts/README.md)。使用 Release，并发上限 4；GUI、CLI 和 worker 均需独立且有标记的 `BETTER_MIACODE_RUNTIME_ROOT`。本地配置示例见 [开发文档](docs/BETTER_MIACODE_ZH.md)。未安装依赖或未实际运行的平台，不计为构建、安装或导出通过。

问题请提交到 [Better Miacode Issues](https://github.com/albertoBradish/Better_Miacode/issues)，附版本、平台、复现步骤和日志；日志位置与开关见 [调试指南](docs/ops/DEBUG_INDEX.md)。[Actions](https://github.com/albertoBradish/Better_Miacode/actions) 中的构建结果与 Release 附件共同记录发布状态。

## 上游与授权

基础编辑、Net 谱面下载上传、音视频工具及其他原版功能继承自 [MiaCode](https://github.com/Team-MiaCode/MiaCode)。[上游使用文档](https://miacode.net/miacode/quick-start) 可供基础操作参考；其中品牌、下载和社群信息属于上游。

项目代码采用 [GPLv3](LICENSE)。第三方授权见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)，贡献与致谢见 [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md)。
