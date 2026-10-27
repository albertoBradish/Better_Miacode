# Better Miacode 开发与验收规格

Better Miacode 是操作体验改进版本，基线为 Team-MiaCode/MiaCode 的
`main@7feadff91644fea4b1f8e2c1c0de71b3f35233da`。独立仓库不包含手序版提交。
本文描述验收目标；实际验证结果另记交付报告。

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
