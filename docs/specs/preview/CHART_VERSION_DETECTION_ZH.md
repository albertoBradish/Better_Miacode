# 从 simai 谱面识别 maimai 版本

2026-11-19 核对。自动判定线与自动完成度显示共用 `core/chart/ChartVersion`。元数据给出的版本优先；仅凭音符时，结果表示这些音符需要的最低已知版本。

## 输入与输出

先读 `&metadata={"version":"PRiSM PLUS"}` 的字符串字段，再读谱库和编辑器沿用的独立属性 `&version=maimai DX PRiSM PLUS`。解析时统一全半角、大小写、空格、`PLUS` 与 `+`，接受日文的 `でらっくす` 和常用简称 `Pri+`、`Fes`。未知版本或无版本时只检查目标难度完整解析后的音符，包含选区导出范围外的音符。同曲不同难度可以得出不同结果。

检测结果保留 PLUS。随包版本判定线按家族分图，因此 GreeN PLUS 使用 `2.GreeN.png`，PRiSM PLUS 使用 `13.PRiSM.png`。MAGiCAL 尚无随包同名图，暂用最近的机台家族素材 `14.CiRCLE.png`。FiNALE 及以前选 FiNALE Rate，DX 起选 DELUXE Rate。手动选“默认”始终显示两项。

## 已实现的元素规则

| 可从当前难度识别的特征 | 版本下限 | 核验依据 |
| --- | --- | --- |
| 普通 TAP、HOLD、BREAK TAP、基础直线和短弧 | maimai | 使用初代作为无更新特征时的基线 |
| 小写 v、p、q、s、z 或起终点相同的整圈弧 | GreeN PLUS | [SEGA 十周年 GreeN 回顾](https://maimai.sega.jp/10th/history/green/)；[simai 作者对各形状的说明](https://w.atwiki.jp/simai/pages/23.html) |
| pp、qq、大写 V、扇形 w、同起点分支、BREAK 星头 | PiNK | [SEGA 十周年 PiNK 回顾](https://maimai.sega.jp/10th/history/pink/)；[simai 作者的 PiNK 新语法与星头示例](https://w.atwiki.jp/simai/pages/23.html) |
| EX TAP/HOLD/星头、B/C/E 区 TOUCH、中央 TOUCH HOLD | DX | [SEGA 早期 DX 公告海报](https://maimai.sega.com/download/2019-11-25-5/2019-11-25-5.pdf)；[simai 的传感器与 TOUCH 记法](https://w.atwiki.jp/simai/pages/509.html) |
| 同一轨迹有多个顺序片段、BREAK 轨迹、BREAK HOLD、EX BREAK、A/D 区 TOUCH | FESTiVAL | [simai 作者对三项扩展的说明](https://w.atwiki.jp/simai/pages/937.html)；[simai 语法参考](https://w.atwiki.jp/simai/pages/1002.html) |
| A/B/D/E 区 TOUCH HOLD | PRiSM PLUS | [simai 语法参考的中央以外 TOUCH HOLD 说明](https://w.atwiki.jp/simai/pages/1002.html)；[SEGA 对 PRiSM 活动关联曲的记录](https://info-maimai.sega.jp/8802/) |

同一星头分叉的 `*` 产生多条并行轨迹。FESTiVAL 拼接从同一条轨迹的多个顺序片段识别。单个旧式大写 V 本身有转折，仍属于 PiNK。Simai 原作者明确把 C1、C2 都归为中央，MiaCode 的普通和严格解析也都会将二者归一到 C。因此 `C2h[4:1]` 和 `C1h[4:1]` 都按 DX 中央 TOUCH HOLD 处理。[simai 传感器定义](https://w.atwiki.jp/simai/pages/509.html)普通 BREAK TAP 与 BREAK 星头不能单独作为 FESTiVAL 的依据。

## 本地语料抽查

仅摘录本次核对所用的最小符号。原文件在官谱新档库 `谱面` 下，均取 MASTER（`inote_5`）；SHA-256 对应检查时的完整 maidata.txt。

| 曲目 ID 及标题 | 显式版本 | MASTER 中的例子 | SHA-256 |
| --- | --- | --- | --- |
| 10125 千本桜 [DX] | maimai DX | E4 TOUCH | `559527f452b6dffc68aab9ebebd6354c7b7b5ea2a88f28850cbae8afee175bb8` |
| 10070 ジングルベル [DX] | maimai DX FESTiVAL | D1 TOUCH；连续轨迹 4-6-2 | `183f59d93c717bc9684f2965bd38e943abba36232069b748c2ab8ac50d05c70c` |
| 11821 Ref:rain (for 7th Heaven) [DX] | maimai DX PRiSM PLUS | E3h TOUCH HOLD | `413ea9f4de2efb6f0f522a344ef30f69f31f1c2a6b856f398b452a7d9d1c4e6b` |

SEGA 的 GreeN 十周年回顾直接确认是在 GreeN PLUS 加入新形态，并记下谱面制作者对 CYCLES 的说明；PiNK 回顾直接记录了扇形 SLIDE。各字母、星头类型和子版本的细分对应以 simai 作者的原始文档为依据。FESTiVAL 的三类扩展由作者在发行当时记录。PRiSM PLUS 的非中央长按由作者后补说明，表中官谱的 E3h 为本地独立例证。单首曲的目录版本只能证明该版已有这种配置。

早期规则尚无足够资料精确划定所有弧线长度及端点组合。宴谱特效、MiaCode 自定义标记、速度指令和 mine 目前没有用于提升版本。后续新增规则应先核对最早可证实的子版本，用原始 token 经解析器生成的标记验收，并保存来源。
