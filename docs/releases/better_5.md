# Better Miacode better.5

## Slide 判定时值

预览“画面选项”与视频导出页新增“显示 Slide 判定时值”，默认关闭。star 头拍后立即开始，沿轨迹按弧长匀速将低亮恢复到正常亮度，并在尾判正解时刻完成。原引导星星保持谱面时序。支持 fes 整链、同路折返、Each、BREAK 和 WiFi 原皮肤材质。

CLI 的 `--slide-judgement-timing` 可用于视频导出；用于 `stats --json` 时返回逐条尾判正解时刻和参考 Perfect 窗口。WiFi 对称窗口保留参考模型标识，不增加 BREAK 专属细分判定。

详见 [功能规格与参考模型边界](../specs/preview/SLIDE_JUDGEMENT_TIMING_ZH.md)。

## 本地验证

2026 年 10 月 28 日，Windows x64 Release，构建并发上限 4。仅使用合成谱面与独立 Better Miacode runtime。

- 9 项聚焦 Spec 通过，覆盖全部 616 条形状的时间和弧长投影、fes 最终尾判、零等待、负偏移、快照、旧快照、QML 导出页、CLI、运行隔离及原有轨迹擦除。
- OpenGL、D3D11 各 10 类原生皮肤样例通过；尾判正解时刻的轨迹与正常全亮材质逐像素一致，回拖重放一致。
- 已导出 60 FPS 整段和 120 FPS 片段视频；CLI 时值统计确认 `&first` 只应用一次，未标记运行目录以退出码 78 拒绝运行。
- 已在独立便携包窗口打开合成谱面并回拖预览，核对新增开关默认继承、导出独立关闭，以及离开导出试听后恢复预览设置。
- 固定 libx264 单线程、无 B 帧用于原模式回归比较，120 帧与改动前程序一致。自动压缩编码配置不用于逐像素回归判据；逐像素检查使用原始 GPU 帧。

本版本尚未宣称完成 Windows ARM64、macOS 或第三方皮肤的跨平台验收。本地测试包不等同于已发布 GitHub Release。
