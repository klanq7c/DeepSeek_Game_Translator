# Failure Transparency

## 启动器文件 API 的长路径边界（2026-11-07）

- **外部问题：** 普通 Win32 文件 API 的路径长度限制会拒绝深目录下的组件、临时文件和中间目录；路径缓冲足够大并不代表读写成功。
- **上游限制：** 不能要求游戏、用户安装目录或系统注册表为启动器改名/改设置。`fsutil.c` / `SafeFs.cs` 在文件 API 边界把规范化的绝对长路径转换为 Windows 扩展形式；短路径继续使用原参数。属性检查、创建目录、读写、复制、移动、删除与枚举使用相同规则；组件字节比较同步转换。
- **是否掩盖：** 不截断路径，也不把写入失败算成成功。保留原有 1040 字符路径缓冲上限、设备命名空间及逐级重解析点拒绝。长路径中的尾点/尾空格、保留设备名和备用数据流拒绝转换；独立 `.` / `..` 先按普通 Win32 规则规范化。UNC 只做语法转换，不据此宣称所有网络共享或第三方工具都兼容。
- **诊断：** 无法转换时保留对应 Win32 错误码；操作边界沿用既有失败记录。自有组件同步会记录具体组件失败并返回 5，部署/还原维持原失败结果，不因为加入扩展前缀跳过所有权检查。

依据：[Microsoft 路径长度限制](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation)、[文件路径与命名空间](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file)。

## RPG Maker 命令对象与扫描边界（2026-11-07）

- **外部问题：** 合法 JSON 的 `code` / `parameters` 属性可以重排，插件也可能添加嵌套元数据；损坏文件则可能有未闭合参数或过深嵌套。
- **上游限制：** 启动器只读游戏资源，不能要求游戏重写属性顺序。C/C# 在当前对象闭合后配对字段，避免借用相邻或嵌套对象的命令码。
- **是否掩盖：** 文件对象嵌套上限为 128 层；参数沿用 JSON 值扫描器的 32 层上限。超限或参数未闭合时停止当前文件扫描，已采集文本保留；未扫描内容仍依赖运行时钩子，不能当成已完成翻译。游戏文件与共享缓存不被这个扫描器改写。
- **诊断：** 两版在扫描器所有权边界以 `RPGM JSON 预热未完成` 记录原因与文件路径，不输出文本内容。未完整闭合的命令不参与预热。

## 损坏缓存文本的加载边界（2026-11-07）

- **外部问题：** TSV 记录含非法或不完整 Base64、内嵌 NUL、无效 UTF-8；宽松解码会使键截断碰撞、坏值覆盖原译文，或使 HTTP 输出包含无效 UTF-8。C# 孤立代理也不能无损写入 UTF-8。
- **上游限制：** 文件可能来自旧版本、外部编辑或损坏；当前进程不能推断原始文本。两版只接受完整可往返的编码，C# 入库同时拒绝 NUL 与孤立代理，C 入库拒绝无效 UTF-8。
- **是否掩盖：** 跳过记录会使该条不可用，但不将损坏字节替换或截断后冒充译文，不覆盖之前的有效值，也不重写原文件。C 按实际读取字节确定换行，原始 NUL 或超长行只丢弃自身，不吞掉下一行。空白 Base64 和正常多行、制表、格式标记及 Unicode 仍可加载。
- **诊断：** 启动加载边界以 `load-record-invalid` 向 stderr 记录次数和文件路径，前三次及二次幂计数输出，不记录原文或译文。API 批量入库沿用 rejected 计数；没有把拒绝条目算作成功或持久化。

## 源码发布的已知布尔探针识别（2026-11-06）

- **边界：** 文本凭据正则无法仅凭 `password` 字段名区分秘密与 UI 密码掩码状态；原规则还遗漏了单字符赋值。
- **上游限制：** C/C# 探针已有相同诊断协议和消费者；源码审计不应为通过扫描改写该协议或源文件，也不能排除整份源文件。
- **可见限制：** 共用扫描适配器只在固定相对路径和完整已评审表达式匹配时，将内存视图中的字段名替换为非凭据标识。值、条件和相邻文本均保留，其他规则继续扫描；探针值、条件、锚定形式或路径改变即失去识别资格。普通数字密码和单字符 Key/secret 继续拒绝。这是已知非秘密表达式的识别，不是通用秘密检测的完备性保证。
- **诊断：** 识别输出 `recognized UI probe` 和相对路径；拒绝继续只报告路径和命中规则，不记录候选秘密正文。读取或适配器异常不能变为通过。准备脚本先执行反例守卫，再生成 staging，独立审计使用同一适配器。

## Unity 构建引用发布（2026-11-01）

- **外部边界：** Mono 的 `--no-incremental` 会清理输出依赖；外部文件写入、锁、复制、目录移动、指针替换可失败，另一个构建进程可能同时请求发布。`FOR /F` 不传递子进程退出码，Windows PowerShell 5.1 将普通 `$null` 字符串参数绑定为空路径。
- **上游限制：** 构建器可以清理自己拥有的输出目录，不能要求编译器保留借来的引用；构建流程必须在外部持有验证过的引用。发布器不能修复损坏的磁盘内容，也不能忽略文件锁或沿重解析点写入其他目录。使用真实 null 修复参数绑定，不以失败重试代替修复。
- **可见限制：** 仅输出缺失时允许使用已验证的 current；显式引用错误、已存在但损坏的 current/快照和路径类型错误直接失败。新版本在独占临时目录复制、校验并重新核对源后发布，指针最后原子更新。指针更新失败可能留下完整的未选中版本，但旧 current 仍有效；失败临时目录/文件保留诊断，不自动回收。锁拒绝并发发布，不锁整个编译流程；已发布引用不可变且不被此脚本删除。此机制不保证两个完整构建可同时改写相同输出目录。
- **诊断：** helper 的异常直接到 stderr，成功 stdout 只有内容键；批处理还要求子进程退出成功标记及严格键格式，否则返回非零，不执行 Mono 编译。指针发布异常明确包含目标、临时路径与原错误。持锁作用域以 `finally` 释放。引用错误不会被报告为成功构建，更不改变运行时译文或用户缓存。

## Ren'Py 中文分段补字（2026-10-31）

- **外部边界：** 游戏可能用 FontGroup 的范围/remap、字体别名、粗体替换或字体变换显示图标和文字；统一覆盖会破坏这些语义。部分引擎可能缺少可调用的 TextSegment/FontGroup 分段 API。
- **上游限制：** 只有引擎排版时知道当前样式、内联字体及变换结果；共享缓存不能推断字形来源。不能通过修改译文、游戏字体文件、存档或全局替换表解决。
- **可见限制：** 只为普通中文片段和 FontGroup 未明确指定字体的中文目标补字；非中文、明确范围和原替换表继续交给引擎。组合附标/变体选择符跟随已补基字，Windows Python 2 代理项保持完整。此策略不探测每个字体的字形覆盖；显式指定的字体缺字或部署字体本身缺字仍可能显示空缺，不能据此宣称全部字体兼容。无法安装分段 API 时保留原引擎，不回退到全局字体覆盖；可能失去中文补字，但不会假报译文/缓存成功。
- **诊断：** 安装边界以 `_ds_report_exception(..., 'install-font-segmentation')` 限频记录具体异常。真实分段、游戏字体变换及加载错误直接传播到引擎 traceback；不捕获整段绘制后伪造成功。线程局部上下文在原生成器推进的 `finally` 中释放，暂停、关闭和异常均不会遗留活动上下文。原组的独立调用保留原缺字错误。

## 审计明细写入与候选追踪（2026-10-28）

- **外部边界：** JSON 行编码、磁盘写入、Flush 或文件移动可以失败；其他进程也可能在扫描期间创建同名目标。坏缓存可能具有合法源键但无效的 base64/UTF-8 候选值。
- **上游限制：** 只读 QA 不能修复外部磁盘/目标竞争，也不能把损坏候选改造成成功译文。旧摘要的示例数量上限不适合作为完整失败清单。
- **可见限制：** 显式指定明细路径时，逐行写入同目录独占临时文件，全部完成后才以不覆盖方式移动到目标；任何编码/写入/发布错误均令命令失败，不返回成功汇总。拒绝游戏目录、已有目标以及游戏/输出路径上的重解析点，父目录须已存在。未指定明细路径时维持只读行为。有效旧缓存命中不因后来的拒绝行被清空；已解码源键的损坏值进入该键的诊断计数，无法恢复源键的坏行只计入原有全局统计。
- **诊断：** 原异常继续向调用者报告；只清理本次独占创建的临时文件，清理失败发出带路径和原因的警告。JSONL 的 `translation_source`、`marker_review`、`issues` 及候选计数保留复核原因；未知运行时来源不伪装成字段位置。结构匹配仅为静态原文对照，不能隐藏语义、字体或实机问题。

## Ren'Py 只读审计的原文对照（2026-10-25）

- **外部边界：** 静态字符串可能是尚未求值的 `.format` 模板、插值模板或双开括号字面量，单独交给文本标签检查器会误判。反过来，通用标记正则会漏掉方括号变量、空/数字字段和过长标签。
- **上游限制：** 审计读取预热转储与持久缓存，无法获知游戏随后采用哪种求值阶段，不能在共享缓存里替游戏规范或执行模板。
- **可见限制：** 只在 Ren'Py QA 分支比较完整且有序的结构；双开括号内的可见文字可翻译。未闭合片段比较种类后继续检查后续变量，避免坏前缀吞掉其余检查。没有合法类型后缀的 `%(说明)` 不作为旧式格式项；无法推断游戏是否启用了旧式替换，不能证明原始语法正确。有意调换变量顺序也会列为人工复核线索，不自动修复或拒绝缓存。
- **诊断：** 差异仍通过既有 `structural_marker_mismatch_suspects`、`marker_examples` 及中文审计输出记录。Ren'Py 的 `scope` 明示不执行模板、不推断旧式替换设置；原有的覆盖范围和未实测字形/布局说明继续保留，不产生“模板有效”或“实际渲染成功”的结论。

## 经验优化复查：映射键与字面标签（2026-10-25）

- **Godot 外部边界及上游限制：** 游戏可在 ready 前输出很长日志，或在 ready 与 ACK 间跨越文件读取块，不能要求游戏限制启动日志。预检独立读句柄持续前进，使用 64 字节行状态和 16 KiB 读缓冲，每轮最多读取 512 KiB，再检查退出和既有期限。超过行状态容量或含 NUL 的行不能是完整协议行，丢弃该行并在换行后恢复，不把截断片段认作证明；长日志不再挤掉后续完整凭据。协议仍缺失、读失败或进程自行退出时沿用既有诊断与失败回退，不将失败缓存为成功。内存及每轮处理有界，不屏蔽原有停止/退出错误。
- **Ren'Py 外部边界及上游限制：** Python 旧式格式化允许平衡括号及空映射键，提供方可以改写这些键；共享服务器无法获知当前游戏的替换设置。Ren'Py 层按完整映射键还原，不将合法语法判错。非法输出仍保留原文，可能保持英文；由原有 `renpy-percent-mismatch` / `restore-renpy-tokens` 限频记录，而非记为成功译文。
- **Unity 外部边界及上游限制：** TMP noparse、未识别样式和 Legacy UGUI 可以显示看似标签的字面字符。服务器及共享标签词法器不知道组件是否解释这些字符，故仅在 IL2CPP 字体入口保守扫描。可能额外检查无效样式中的字形，但不修复或隐藏游戏原标签错误；缺字、加字失败及覆盖查询仍由现有 `tmp-font-population-*`、`tmp-font-coverage-unavailable` 和字体应用边界诊断。失败不会记为成功字形覆盖或改写缓存。

## Godot autoload 专用输入确认（2026-10-25）

- **外部边界：** 子进程输出 ready 后可能自行退出；父进程若先调用 `TerminateProcess`，专用停止码也不能证明子进程本来会保持存活。只读到 ready 不再被当作完整凭据。
- **上游限制：** 启动器不能推断外部进程未来的退出意图；改为私有参数分支读取专用 stdin 并输出 ACK。完整一行在创建进程前写入匿名管道并关闭写端，避免等待用户输入或在退出期间写入。普通游戏启动不读取该输入，脚本仍由父进程收尾，避免主动 `SceneTree.quit` 与游戏资源加载竞争。
- **可见限制：** 缺失 ACK、输入接口不可用、管道错误或自行退出均不确认 autoload，也不缓存成功；继续现有兼容路径并保留未确认状态。握手只证明脚本执行和读取探针输入，不能证明游戏后续不会崩溃；现有退出码及停止所有权检查继续执行。
- **诊断：** 脚本用 `push_error` 报告 stdin 接口缺失或握手不匹配；启动器记录 input pipe/write、捕获读取、退出码及停止错误。捕获文件按原有机制自动清理，失败进入既有 autoload 预检失败日志。缓存签名升级为 `autoload-ready-v3-stdio-ack`，来源指纹升级为 `DST_GODOT_SOURCE_V5`，旧成功结论和旧运行时包均不跳过新协议。

## Ren'Py 完整标签与旧式映射字段（2026-10-25）

- **外部边界：** 提供方可以新增长伪标签、第 64 个之后的标签、未闭合片段，或新增/丢失旧式 `%(name)s`，引发标签错误和映射求值异常。
- **上游限制：** 共享响应适用于多种引擎，无法从统一服务器推断游戏的自定义标签和 `old_substitutions` 设置；原字段、标签和错拼标识必须由 Ren'Py 适配层保留。
- **可见限制：** 完整检查失败保留原文，不记为成功译文；外部一直给出相同非法结果时可能保持英文，不能隐藏为覆盖完成。原文自身的游戏脚本错误仍保留，翻译器不自动改故事变量。
- **诊断：** `renpy-token-injection`、`renpy-token-mismatch`、`renpy-token-unclosed`、`renpy-percent-mismatch` 和 `restore-renpy-tokens` 经现有限频器记录类型/数量或异常，不输出请求正文。原待处理/终态冷却流程继续处理被拒绝结果。

## Godot 压缩脚本预热边界（2026-10-25）

- **外部边界：** tokenizer v100 GDSC 的非零解压大小表示 Zstd 载荷，直接扫描可打印字节会得到虚假的文本候选；损坏载荷也不能作为明文处理。
- **上游限制：** 当前启动器未提供该解压格式；文本过滤器无法恢复被压缩的常量。新增解压/重建能力需独立验证格式、体积上限和原包保护，不能用原始字节回退冒充支持。
- **可见限制：** 跳过该容器的静态预热，保留现有运行时可见文本发现；不可见文本可能晚到。既不改包也不把片段排队为成功提取，普通场景与旧格式不受该判断影响。
- **诊断：** C/C# 每个 TextList 发现操作各记录一次 `Godot warmup: compressed GDSC skipped`，明确解压不可用及实时发现边界；限频状态不进入协议或缓存。

## Ren'Py 只读发现与语言字体生命周期（2026-10-24）

下列字体清单和语言回调描述记录 10 月 24 日实现；10 月 31 日已移除这两条字体路径，当前边界见上方“Ren'Py 中文分段补字”。只读清单/预热边界仍有效。

- **同日复查的边界：** 屏幕/输入采用 Python 字面量规则。动态拼接/格式化没有静态最终值，不进入猜测预热；命名 Unicode、NUL/孤立代理及版本相关 raw Unicode 无法在未装载对应 Python 的启动器中可靠解码。扫描器只跳过该项并记录“Python 字面量包含不可提交的转义”，由原有运行时发现补充，不改写共享键或报成功。
- **已有翻译的只读边界：** 原生字符串表结构不可用时，`native-string-inventory` 限频记录异常并保留原文给实时发现。不能调用会在引擎更新模式登记未知项的 `translate_string` 作为上游替代；正常查询只读当前语言的字典及上下文候选，不增加字符串或 unknown 记录。缺少该元数据可能造成多余预取，不能据此认定游戏缺少原生译文。

- **外部边界：** 源码字符串未闭合或扫描缓冲分配失败；导出版本没有兼容的原编译翻译块；字体清单枚举失败、超过 1024 个字体或没有语言变更回调。
- **上游限制：** 启动器不能修复游戏源码或要求游戏重新导出，运行时不能重建原翻译标识或改写故事变量。字体样式重建由引擎语言生命周期管理，字体路径可能位于归档中。
- **可见限制：** 静态预热失败保留原运行时发现；编译清单失败保留菜单与实时发现；字体清单失败仍使用已有 GUI/根目录字体，回调不可用时保留初始字体覆盖。均不把未发现或缺字文本写成成功译文；这些限制可能留下稍后才发现的文本或缺字。
- **诊断：** C/C# 源码扫描记录分配或未闭合边界；运行时用 `_ds_report_exception` / `_ds_report_diagnostic` 限频记录 `collect-compiled-dialogue-inventory`、`collect-compiled-say`、`compiled-prefetch-capacity`、`compiled-prefetch-scan-capacity`、`font-resource-inventory`、`font-resource-capacity` 和 `register-language-font-coverage`。原有请求和样式错误保留各自边界。

## Unity TMP 加字前的只读检查（2026-10-24）

- **同日复查的覆盖记录边界：** 游戏可清空原字体图集，当前覆盖检查必须先于已见字符集合。查询 API 缺失或异常时无法要求游戏恢复元数据；`tmp-font-population-coverage` 记录查询错误，`tmp-font-coverage-unavailable` 限频记录沿用旧成功加字记录的限制。该兼容路径不能保证识别不可查询图集的后续清空，保留原有显示回退；已知缺字会移除旧记录，加字失败仍不成为成功覆盖或译文。

- **外部边界：** Static 图集无法补字；Dynamic 字体可能缺少源 Font 和源路径；裁剪或旧版 TMP 的覆盖/模式属性可能缺失或 getter 抛错。
- **上游限制：** 已导出字体和 TMP 接口归游戏所有，翻译器不能补回未打包的源数据。共享译文无法修复原生字形问题；仅在字体显示层判断是否调用 FontEngine。
- **可见限制：** 已有本地图集覆盖直接使用，Static 缺字或确定缺源仍返回失败并保留既有显示回退，不假报成功。DynamicOS 和路径来源继续允许；元数据无法读取时沿用原加字入口，只有实际布尔成功才确认覆盖，不永久缓存失败状态。
- **诊断：** 缺源和元数据异常经两插件既有的 `ReportCaughtException` 限频记录 `tmp-font-population-source` / `tmp-font-population-metadata`，带 Mono/IL2CPP 与加字操作上下文。未知原生结果记录 `tmp-font-population-result`；已有缺字/覆盖层诊断仍保留。预热常量不能成为未验证的成功记录，等价重载不用于重试正常失败。

## 半汉化文本的中文保护（2026-10-23）

- 外部边界：模型可能改写混合句里的原有中文，或丢弃、重复、重排已有中文的保护标记。提示词不能保证外部模型一定遵守保护要求，因此在 C/C# API 响应入缓存之前验证。
- 处理：请求仅把原有连续汉字段替换为本次原文中不存在的稳定 ASCII 标记；结果必须按顺序完整保留每个标记且恰好一次，再逐字还原原有中文。纯中文和短缩写继续透传；标签、变量、控制码和 URL 不作为补译信号。新增逻辑不重写既有缓存。
- 失败含义：保护校验失败返回未完成，游戏继续显示原文或已有安全命中，不写成功翻译或持久化记录；既有内容失败的有界拆分策略仍可处理同批健康条目。此边界可以留下未翻译英文，不能伪装为汉化完成。
- 诊断：C API 使用限频 `batch-shape`，详情含 `mixed-cjk token mismatch`；C# API 使用限频 `mixed-cjk`，批量记录槽位。引擎原有控制符、富文本、网络和渲染诊断继续保留。
- 同日复查：中文恢复后的缓存/显示清洗也必须携带原文，保留原文自有的提示词形状前缀；纯格式整理不能登记为已完成翻译。Ren'Py 显式 `engine: "renpy"` 请求按真实标签和插值识别正文，避免字面双括号被通用规则当变量；未知提示沿用原语义。实际提供商失败仍保留既有限频诊断与未完成状态，实时回队不会丢失引擎格式提示。

## RPG Maker 独立渲染与无格式缓存（2026-10-21）

- 2026-10-22 复查：控制安全检查扩展至完整带参插件指令，防止未知指令被无格式缓存吞掉。上游提供方无法保证第三方插件语法，拒绝只发生在 RPG Maker 显示缓存；仍显示原文或安全片段，由原有 `*-control-guard` 限频记录拒绝，不改写共享记忆。MZ 测量与插件行距修复直接遵循渲染状态，不增加静默回退。
- 外部边界：第三方插件绕过普通文字入口，或者提供方仅有移除颜色等控制符的可见文字译文。把该结果直接用于对话重绘会丢掉原始样式；简化的中文绘制器也无法理解全部插件指令。
- 上游限制：独立插件的循环、游戏控制符及消息状态由游戏管理。只在 RPG Maker 字符处理与页面规划边界匹配安全片段，所有实际样式继续由引擎解析，不改写共享缓存或游戏源码。
- 不掩盖失败：移除控制符的整页结果拒绝用于富文本显示；安全片段不足时保留原文并继续既有异步查询。消息仍按页处理，独立插件可显示已完成的片段。不存在字符渲染 API 或引擎绘制抛错时，自动换行调用方保留原有插件回退，不把失败登记为成功译文。
- 诊断：拒绝无格式覆盖记录 `visible-format-cache-rejected` / `message-format-cache-rejected`；字符规划、安装和换行绘制错误分别记录 `translate-character-page`、`install-character-renderer-hook`、`draw-cjk-auto-wrap`，沿用限频诊断。缓存、网络与未命中状态继续使用原有故障边界。

## RPG Maker 自动图片文字识别（2026-10-18）

- **后续译文与协议复查：** 布局放不下的新译文不能由共享缓存修复，因为同一译文在其他区域或引擎可能可用；本层保留此前已成功显示的值，并用 `image-text-fit` / `image-text-measure` 限频说明。只有提交成功后才记录显示值，暂存绘制失败不产生成功状态。识别进程的合法 JSON 也可能具有错误形状；对象、请求号、错误字符串和结果对象在读取前校验，失败由 `image-ocr-worker` 记录并保留原图。每背景 512 个候选的上限避免在游戏帧中进行无界两两比较，超限通过 `image-text-plan` 报告；该降级会留下未翻译图片，不伪装成功。
- **外部边界：** 游戏把文字烘焙进图片，可能使用装饰字体、透明底或复杂背景；系统 OCR 语言缺失、进程启动、PNG 解码、字体加载及画布绘制也可能失败。
- **上游限制：** 图片与插件布局属于已发布游戏，无法由普通文字 hook 恢复。识别放在 RPG Maker 层，保留引擎解密和色相处理，不更改共享缓存或恢复插件禁用的窗口绘制。PowerShell 5 不能依赖系统代码页读取无 BOM 中文脚本，父进程显式按 UTF-8 加载。
- **不掩盖失败：** 系统能力缺失或识别超时使当前会话保留原图；候选词须来自当前游戏术语或通过双背景一致性校验，矛盾词、不可验证背景及放不下的译文不覆盖。字体未就绪和缓存未完成时保留原文；绘制先在独立画布完成，不提交半成品。以上分支不计为成功译文；已显示的成功译文不会被后续未命中擦除。识别仍可能误判，不承诺任意艺术字体或复杂图像可用。
- **诊断：** 模块、工作进程、系统识别、图像快照、尺寸、背景、字体、场景节点、绘制和容量分别用 `image-ocr-*` / `image-text-*` 经 `reportRpgmError` 限频记录；工作进程逐请求输出错误号和操作。启动器部署失败返回文件和 Win32 错误，不能报告部署成功。进程有就绪/请求超时和空闲退出，事件按进程归属校验，旧进程错误不能使新任务失效。

## RPG Maker 图片文字资源适配（2026-10-18）

- **外部边界：** MOG 等菜单把文字烘焙进 PNG，并禁用普通文字绘制；文字 hook 无法拦截图片中的标签。NW.js 的文件读取还可能遇到权限、损坏、升级后的资源或错误适配清单。
- **上游限制：** 已发布的游戏素材与菜单布局属于游戏作者。恢复其禁用的方法会叠字或改变排版；因此只在 `ImageManager.loadBitmap` 边界替换经核实的资源参数，继续由引擎管理加载、色相、平滑、缓存和刷新，不改消息或窗口生命周期。
- **不掩盖失败：** 可选清单中的原图与译图都必须通过 SHA-256、路径及相同 PNG 尺寸检查；失败的条目保持原图。原图文件及缓存对象不被覆盖，未创建成功翻译或共享记忆记录；缺失清单明确表示未启用图片适配。指纹只在启动时核对，不支持运行中改写资源。游戏开启图片解密时不把普通译图送入解密器，保留原来的解密路径；图片加密协议属于上游引擎，本层不伪造加密资源，`image-overrides-encrypted-runtime` 限频记录该不兼容情况。
- **诊断：** 初始化、清单、schema 和逐项资源验证分别记录 `image-overrides-runtime`、`image-overrides-manifest`、`image-overrides-schema`、`image-overrides-entry-N`，使用既有 `reportRpgmError` 限频并保留错误上下文。回归保护连续刷新时译图不消失、几何不变、禁用的文字方法不恢复，异常资源可回退而不影响普通文字翻译。

## 全库复查修复边界（2026-10-16）

- Unity Mono 本地缓存：文件锁、权限错误、JSON 损坏或超大缓存隔离失败会阻止覆盖旧文件。文件系统状态不能由翻译提供方修复；加载尝试结束与成功导入分别记录。失败保留脏状态，已有持久化去抖周期重新读取并合并，退出时仍未成功检查则保留原文件。`boundary=local-cache-load` / `oversized-local-cache-isolation` 限频记录异常，最终跳过落盘有明确日志，不把未保存内容报告为已持久化。
- C# API 请求构造：用户可直接编辑端点配置，非法端口等 URI 错误必须在发送边界再次校验，不能依赖配置窗口。拒绝时返回未解决结果，不生成缓存成功项、不拆分重试；`request-uri` 限频记录错误类型且不输出可能包含凭据的完整地址。请求头和响应体的超时均由配置取消令牌负责。
- Godot 静态资源：外部模型或旧缓存可能遗漏、注入或改写 BBCode。标签解释属于 Godot，不能在共享缓存中重写。资源生成器保留原字符串、不计入成功补丁，并按资源汇总 `incompatible tags or format tokens` 拒绝数量；两版实现使用相同判断和诊断。
- IL2CPP UGUI 备用覆盖层：游戏控制原 TMP 的颜色与透明度，覆盖层只借用其显示位置并关闭源组件，不再用透明色覆盖游戏状态。若裁剪后的 IL2CPP 元数据使源 `enabled` 不可写，文本或服务器层无法补齐此属性；撤销覆盖层并返回 `hide-source`，避免叠字。颜色读取失败返回 `color`。属性异常记录保留，扫描汇总 `colorOrHideFailed` 按状态变化输出，错误不影响共享译文缓存。

## 启动器外部服务关闭确认（2026-10-07）

- 外部边界：接管的本地服务可能接受 `/shutdown` 后仍持续响应 `/health`。HTTP 成功只表示请求被接受，不能表示进程已退出。
- 所有权：启动器没有接管进程的句柄，不能强制结束它。C/C# 均保留运行状态、停止按钮和重试能力；已有的缓存清除流程继续复查实际状态。
- 不掩盖失败：1500 ms 关闭等待计入 HTTP 耗时；期限后服务仍响应，保留已有的 `External C server still owns ... after the shutdown deadline` 诊断并返回，不再清空状态或报告已停止。正常关闭和明确拒绝关闭的行为保持原契约。
- 回归：`run_server_lifecycle_tests.ps1` 源链接 C 生产模块并调用实际 C# 启动器程序集，通过本地假服务验证接受但不退出、拒绝关闭、立即关闭、延迟关闭、慢健康响应，共 10 项；19999 端口已被占用时明确跳过。完整构建和真实服务器冒烟另行验证正常启动及自有进程退出。

This document records the recovery boundaries that intentionally keep a game
running after an external failure. A recovery must preserve the original text,
must not create a successful cache entry, and must retain enough context to
diagnose the real fault.

| Boundary | Exact failure prevented | Why it cannot be fixed upstream | Can it hide the fault? | Diagnostic record |
| --- | --- | --- | --- | --- |
| Launcher server startup | A child process exits before `/health`, or never becomes ready | Process creation, port ownership, and child initialization are outside the UI flow | No; launch is stopped | Launcher log records child exit code or readiness timeout |
| Launcher server shutdown | `/shutdown` transport/status failure or an owned child that ignores the shutdown deadline | The launcher cannot force the server's internal shutdown path to complete | No; HTTP stage/status is logged before owned-process termination | Launcher log records WinHTTP error/status and forced termination |
| Launcher game restore ownership check | A restore request encounters a Unity plugin, managed payload directory, or XUnity config that differs from the launcher-owned payload/snapshot, including files added after deployment | Older deployments and user-managed BepInEx trees do not provide a complete ownership manifest, so deleting a whole marked directory could remove a mod or user configuration | Marked payload trees are removed file-by-file only when installed bytes still equal the current payload; modified/added files and their parent directories remain, and restore is reported as incomplete rather than successful | Launcher log records every preserved path/reason and the restore summary reports preserved and failed counts |
| Launcher/install/release filesystem admission | A selected game, payload directory, or build output contains a junction or symbolic-link reparse point that redirects writes, replacements, recursive extraction, or restore deletion outside its lexical root | The external filesystem owner controls reparse targets and they cannot be made safe by normal path-prefix checks | The operation fails before following an existing reparse component; the target and any outside file remain unchanged. This can reject intentionally linked game/build layouts, but it cannot report deployment or restore success | Launcher deploy/restore logs retain the failed operation/path and Win32 access error; PowerShell installer/release scripts throw the exact reparse component before mutation |
| Mono runtime payload administrative extraction | Windows Installer applies a legacy path limit while expanding the official Mono MSI's deeply nested portable-framework files, causing error 1304/exit 1603 when the downloaded program lives under a long path | The MSI's internal directory tree and Windows Installer path handling are external; extracting only the required assemblies directly from the compressed MSI is not supported by the built-in PowerShell archive path | No; the pinned MSI is expanded under a short, uniquely owned system-temp child after reparse checks, only the fixed corlib allowlist is copied into the program directory, and any non-zero installer exit still aborts the payload installation | `mono-admin-extract.log` is retained beside the verified download on failure and the thrown error records the MSI exit code and log path; the log is removed after success |
| Launcher derived path/command admission | A near-limit game path would truncate a generated `.dst-*`, `.disabled`, `.bak`, atomic-temporary filename, server command, or Godot launch command and make the launcher target a different file or start a process with incomplete arguments | The selected game/root path is external and Win32/CRT formatting cannot safely recover omitted suffixes or arguments after truncation | The affected deploy, warmup write, self-update write, restore, server start, preflight, or translated launch fails before mutation/process creation; an existing translation file is preserved when its backup cannot be created | `path_append_suffix` and `wide_format_checked` set `ERROR_INSUFFICIENT_BUFFER`; the owning launcher operation records the engine/recovery/command role and reports failure instead of success |
| Launcher cache clear | The local server or an adopted external server still owns the persistent cache or retains translations in memory after a shutdown request | Only the server process owns its in-memory map, pending API work, and open persistence handle; deleting the file while it remains live could allow old data to continue or be written back | No; deletion is cancelled and reported as failed while the original cache file remains | Launcher log records shutdown polling timeout, retained-server state, file path, and any Win32 deletion error |
| Manual `start_server.bat` | Missing binary or server process exit | The batch file does not own server recovery policy | No; it no longer restarts | `logs/server_stderr.log` records timestamp and exit code |
| C remote API client | WinHTTP prepare/send/receive/read failure, non-2xx status, missing content, or invalid batch shape | Provider transport and response format are external | No; partial/invalid results are discarded and remain misses | `api_diag` writes rate-limited stage, WinHTTP code, status, and shape without text/key data |
| C/C# split-retry terminal abort | A failed batch is caused by an unreachable endpoint, handle/encoding failure, authentication rejection, rate limiting, or a 5xx, so binary splitting would replay the same error on every half (a fully failed 48-item batch would otherwise cost about 79 requests, each waiting the full timeout) | Provider availability, credentials, and quota are external; only the request-side failure stage can tell a content/shape failure from a terminal one | It can leave a whole batch unresolved after one request, instead of probing smaller batches that might have succeeded during a transient outage; those texts are forgotten by the async queue on completion and re-queue on the next miss, and no abort produces a cache entry. Only content/shape failures (`batch-shape`, empty/missing content, oversized body, request timeout, HTTP 400/408/413/422) still split | C: `api_diag` classifies each stage into the thread-local `g_api_last_fail`; `api_split_abort_diag` records rate-limited remaining counts. C#: `ApiClient.FailureInfo` carries the class through `SendChatAsync`; `SplitAbortDiag` records the same |
| C# server connection lifecycle | A local client stays idle without sending a complete request (net472 `NetworkStream.ReadAsync` ignores its cancellation token, so the recv timeout never fired), the peer resets mid-response, or a route handler throws on an unexpected request shape | The client owns its socket lifetime; .NET Framework's stream implementation cannot be changed from this process | Recv timeout is enforced with `Task.WhenAny`; on timeout the server answers `400 incomplete_request` exactly like C's `serve_one`, then `Shutdown(Send)` before `Close` so the response leaves with the FIN instead of being discarded by an abortive close, and the pooled buffer is not reused after the aborted read. The pre-close drain builds its `NetworkStream` *before* `Shutdown(Send)` because net472 `Socket.Shutdown` flips `Connected` to false and the stream constructor would otherwise throw on every connection (the drain silently never ran until the 2026-09-08 audit). A route-layer exception answers `500 internal_error` instead of silently dropping the connection; transport failures close the socket. Neither path can create a translation or cache entry | `HttpDiag` writes rate-limited stage (`recv`, `send`, `drain`, `timeout-shutdown`, `route-transport`, `route-internal:<type>`, `accept`, `listener-stop`), exception type, the underlying `SocketError` when present, and (first three occurrences) the message to stderr without request text |
| C# server cache load | The persisted TSV cannot be opened or read at startup (permissions, exclusive lock, path failure) | The file system and any other process holding the file are external | It can start with a partial or empty in-memory cache; entries read before the failure remain, misses stay unresolved, and the file is never rewritten | `[cache] load failed after N entries` records exception type and path on stderr |
| C# server cache append recovery | Opening, writing or flushing the TSV fails because the directory is missing, the file is locked, permissions deny access, or the disk fails | The cache cannot repair external file ownership, permissions or storage; it retains valid translations in memory so current rendering can continue | Failure remains visible as `memory_only` and an error/partial import status. Unpersisted keys remain dirty, so a later same-value import retries after the external fault is repaired; only successful writes and the final batch flush confirm new persistence. Write/flush failure retains every changed key as dirty, including A/B/A overwrites. Already persisted duplicates count as success without another append | `[cache] persist-open`, `persist-write`, `persist-flush`, and `persist-close` stderr diagnostics record the operation, exception type and cache path; `tests/server_contract -PersistenceOnly` verifies both servers through failure, repair, duplicate import and restart, and `tests/server_contract/cache_persistence` injects write/flush failures against the source-linked CacheStore |
| C HTTP connection admission | Local clients open more idle connections than the one-connection/one-thread server can safely own at once | A local game, browser, or broken hook owns its socket lifetime; the server cannot force it to send a complete request | Excess connections are closed before a worker is created, so an overloaded caller sees a transport failure rather than a false translation or cache hit | Server stderr records the first rejection and power-of-two occurrences with the active hard limit; `/health` exposes active, limit, and cumulative rejection counts |
| C worker-thread creation | Windows refuses a connection, live-translation, or asynchronous-prefetch worker because process resources are exhausted | Thread creation and the process resource limit are owned by Windows; the request cannot safely execute on the accept loop or an unrelated engine thread | A connection is closed before parsing, while translation work remains unresolved until at least one lane worker exists; a partially created translation pool remains usable and later calls retry only its missing capacity | Server stderr records rate-limited lane, desired/available worker counts and Win32 error; `/health` exposes connection-thread and translation-pool creation failure totals |
| C HTTP browser-origin boundary | A remote website targets the loopback server to read translation memory, spend the configured provider account, mutate cache state, or stop the service | RPG Maker file-origin hooks require CORS, while native Ren'Py/Unity/Godot clients do not send an Origin header; removing CORS globally would break the browser-engine runtime | File/null and explicit loopback origins retain translation routes, but every browser-origin administrative request and every non-local browser origin receives 403 before routing; rejected requests cannot become translations or cache entries | Server stderr records rate-limited `untrusted-or-malformed` versus `administrative-route` rejection counts without logging origin or game text |
| C cache oversized persisted row | A corrupted or adversarial TSV record has no newline and grows startup memory until allocation failure | The persisted file is an external/user-owned boundary and a malformed record cannot be decoded into a trustworthy translation | The oversized record is discarded after its terminating newline while later records continue loading; this can hide only that invalid row, never create a hit | `cache_diag` records `load-line-too-large`, occurrence count, and cache path with a fixed 8 MiB encoded-row limit |
| C server connection drain | `/shutdown` closes the listener while accepted workers are still receiving or sending, allowing `WSACleanup` and process exit to race their socket operations | Accepted sockets are owned by independent workers and may be waiting on an external local client until the mandatory receive timeout | Main waits on the worker lifecycle event; only a timeout/failure after the socket deadline permits process exit with workers still counted | Server stderr records the wait result and exact remaining active count |
| C asynchronous warmup admission | A malformed game or local caller queues a small number of extremely large unique strings, defeating the item-count limit and exhausting memory before provider workers drain | The external hook owns warmup text size and arrival rate; the server must remain nonblocking and cannot force the caller to split valid scripts | New jobs are rejected as unresolved misses once queued/in-flight job ownership reaches 32 MiB; existing work completes normally and no rejection becomes a cache hit | Server stderr records the first three and power-of-two byte-budget rejections; `/health` exposes current bytes and the hard limit |
| C synchronous translation admission | Concurrent local clients submit enough large foreground batches that their duplicated live-worker text exceeds a safe process budget | The local hooks own foreground request size and concurrency, while provider calls may retain jobs until an external timeout | New live work is left unresolved once queued/in-flight text ownership reaches 32 MiB; admitted requests finish normally and rejected text is not cached | Server stderr records the first three and power-of-two live budget rejections; `/health` exposes current live bytes and the hard limit |
| C remote API transport admission | An API configuration points at a non-loopback plaintext HTTP endpoint, which would expose its bearer key and game text to the network, or a non-loopback provider is configured without any API key and could never authenticate | TLS and account authentication are owned by the external provider and WinHTTP cannot make an unauthenticated HTTP origin confidential | The provider is disabled until its URL uses HTTPS; loopback HTTP remains available for local OpenAI-compatible providers, and an empty API key is accepted only on those loopback endpoints. Rejected work remains an unresolved miss and is never cached | Server stderr records that a non-loopback plaintext or malformed endpoint was disabled, or that an empty key is allowed only for loopback/local endpoints, without printing the endpoint, bearer key, or game text |
| Remote provider response byte limit | An external model endpoint returns a body larger than 2 MiB, including a chunked or incorrectly sized UTF-8 response | The provider controls its response bytes; the local server cannot constrain them upstream | Both servers stop reading after the limit is exceeded, leave the text unresolved, and never cache the oversized body; this can hide the provider's intended translation, but not report it as success | Native `api_diag` and C# `ApiClient.Diag` record `body-too-large` with per-stage rate limiting and no game text or key |
| C# provider response body deadline | A model endpoint sends successful headers then stalls before completing its body; on .NET Framework the response stream can ignore cancellation during `ReadAsync` | The remote provider owns the incomplete body, and request-header cancellation alone cannot close its already-open stream | Closing the response at the configured deadline can discard a late answer, but it returns an unresolved miss and never caches the original as a translation | `ApiClient.Diag("read-body", "timeout")` records the stage with rate limiting; `test_provider_body_timeout.py` checks that the next request can use the released channel |
| Godot scene deferred setter recovery | A game script queues another `set_deferred` from a method already deferred in scene `_ready`, overwriting a cached translation after the first ready-event flush | The game owns its message queue and text setters; the launcher and shared cache cannot reorder them | The next process-frame cache-only pass can replace only known originals with known translations before draw, without inventing hits or concealing a cache miss | Both runtimes rate-limit `scene-frame-connect` and `scene-cache-capacity` diagnostics; the real-engine scene regression checks the final draw in Godot 3 and 4 |
| DeepSeek model retirement compatibility | The official DeepSeek endpoint rejects the retired exact model name `deepseek-chat` after 2026-07-24, and V4 enables thinking by default even for short translation work | The provider owns model availability and defaults; rewriting a user's config could silently alter a custom OpenAI-compatible endpoint | It can select `deepseek-flash` only in memory for the exact official endpoint and exact retired alias, but never rewrites `api.ini`; explicit custom endpoints/models remain untouched. Thinking is disabled only for official DeepSeek `deepseek-flash` (V4.1 Flash) and V4-series translation requests | Server stderr records the one-time official-endpoint migration without the API key; `api_diag` retains HTTP status/stage for any later provider rejection |
| C batch-to-single translation | Provider rejects or malforms a batch while individual requests may still work | Provider batch behavior cannot be repaired by the local cache | No; the batch failure is logged and each individual result is independently validated | `api_diag` logs the batch failure; individual failures retain their own stage logs |
| Ren'Py launcher hook bytecode invalidation | A packaged Ren'Py runtime keeps loading an older `iron_deepseek.rpyc` after the launcher writes a newer hook source | Release-mode bytecode selection and source recompilation are owned by the packaged Ren'Py runtime, and some builds do not refresh an externally replaced `.rpy` | No; deployment stops if the launcher-owned stale bytecode cannot be removed, so the game is not reported as successfully prepared with an old hook | Launcher log records the exact `iron_deepseek.rpyc` path and Windows inspection, attribute, or deletion error |
| Ren'Py shared-store name isolation | A game assigns common store variables such as `time`, replacing standard-library modules imported by the hook and preventing cache misses from entering the worker queue | Ren'Py intentionally executes `init python` globals in the game store, whose variable names are owned by each game | No; hook modules use `_ds_*` aliases and real worker failures remain unresolved misses | `_ds_report_exception` retains its own `_ds_sys` and `_ds_traceback` aliases and reports the operation, occurrence count, and traceback through `renpy.log`/stderr |
| Ren'Py interaction refresh dispatch | A cache hit arrives on a hook worker thread while the current screen still owns a cached source-text layout | Only Ren'Py's main thread owns `Text.kill_layout`, redraw, and interaction rebuilding; the local server and worker cannot safely mutate displayables | On older Ren'Py versions without `invoke_in_main_thread`, the direct compatibility fallback may leave source text visible until the next interaction, but it does not accept or persist a false translation | `_ds_report_exception` distinguishes main-thread dispatch, Text tracking, layout invalidation, and hook-install failures; `_ds_report_diagnostic` records use of the older direct-refresh fallback |
| Ren'Py local HTTP | Local cache/batch request fails | The hook cannot control local server lifetime | No; original text remains unresolved and queued | `_ds_report_exception` logs operation, count, exception type, and first traceback through `renpy.log`/stderr |
| Ren'Py visible-menu priority | A compiled/RPA-backed `renpy.ast.Menu` reaches its first interaction before launcher warmup can see its labels, while unrelated UI misses already occupy the normal live batch | Only the running Ren'Py VM knows the complete menu group and its execution lifecycle; parsing every RPA/RPYC/pickle version in the launcher would duplicate engine internals | No; the wrapper preserves `Menu.execute`, unresolved labels remain source text, and a failed fast-worker start returns ownership to the normal pending queue without creating a cache hit | `_ds_report_exception` records menu-hook installation, menu priming, fast-worker startup, and HTTP failures; `_ds_report_diagnostic` records pending-capacity drops and retry-limit abandonment |
| Ren'Py compiled-menu startup prefetch | A packaged game keeps source scripts inside RPA archives, so launcher `.rpy` warmup cannot discover choices before their first interaction | Archive/pickle formats vary by Ren'Py version, while the running engine already exposes its decoded AST through `Script.namemap` and owns the start lifecycle | It may skip ahead-of-time menu prefetch on versions without `start_callbacks`, `game.script`, or `namemap`, but the existing live `Menu.execute` priority path remains active and no miss is cached as success | `_ds_report_exception` distinguishes inventory lookup, individual menu-node inspection, start-callback registration, worker startup, and `/prefetch` batch failure; `_ds_report_diagnostic` records the bounded 4096-label capacity |
| Ren'Py renderer compatibility | Optional Ren'Py style/tag/font API differs by version or game override | The game owns those optional renderer APIs | It may preserve original display instead of translation, but the fault is not silent | `_ds_report_exception` logs the precise compatibility operation |
| RPG Maker dynamic bootstrap/plugin compatibility | MZ `main.js` creates core `rmmz_*`, `plugins.js`, and plugin scripts after the translator tag has already executed, or an optional plugin/window/font method throws | The external engine/bootstrap and third-party plugins own when those classes and replacement methods become available; the launcher cannot install against APIs that do not exist yet | Script-load listeners may reinstall only the closest idempotent RPG Maker hooks and renderer fallback may call the original method, but neither path reports translation success or writes a cache entry | `reportRpgmError` distinguishes dynamic script observation/listener installation, bootstrap listener execution, plugin load observation, and optional renderer methods; it logs context, occurrence count, and first stack through `console.error`, while a missing diagnostic console rethrows |
| RPG Maker local HTTP | XHR creation, transport, status, or response parsing fails | The hook cannot control local server availability | No; original text remains and the key enters network backoff | `reportRpgmError` distinguishes request, HTTP, and parse contexts |
| RPG Maker plugin-owned window refresh | A plugin stores a visible title/HUD/command window on a scene field but outside the normal PIXI `children` walk when a local-cache lookup resolves | Only the live scene owns those plugin window references; the server and launcher warmup cannot infer their renderer lifecycle | The hook inspects only scene-owned fields whose names contain `window`, still requires the existing visible/open/marked target guard, and caps the combined walk at 2048 objects. A local miss also schedules the same nonblocking redraw so the normal foreground request can start; persistence scenes remain excluded | `reportRpgmError` distinguishes scene-field enumeration, scene-owned window reads, and renderer refresh failures; lookup transport/parse failures retain their existing contexts and never create a cache success |
| RPG Maker cold-dialogue first draw | A dialogue's asynchronous localhost cache read is still in flight when `Window_Message.startMessage` would otherwise construct the first visible source page, or a remote result resolves after that page has started | Only the live MV/MZ message lifecycle owns page construction and the active typewriter state; a synchronous cache read would block the render thread, while the server cannot safely restart an engine window | It can briefly defer construction only for the bounded local-cache XHR. A local miss, HTTP/parse failure, or transport timeout clears that gate and preserves source text; remote provider work never delays the page. A later safe hit can restart only the active message window, but a third-party replacement that removes its lifecycle method leaves the current source page unchanged | `reportRpgmError` distinguishes asynchronous cache start/HTTP/parse/timeout failures and records `refresh-active-message` failures with rate-limited stack context; neither path writes a miss or failure as a cache success |
| RPG Maker visible long-page batch | A live message-page batch has an XHR/status/JSON failure, a result drops RPG Maker control tokens, or only some visible lines are ready | Only the running message page can select the safe visible per-line keys and decide whether the complete current page is display-ready; the local server cannot infer plugin decoration or live page ownership | No; failed or control-unsafe results remain source text, are not cached, and enter per-key network backoff. Safe per-line hits may populate the renderer cache, but the visible page stays entirely source text until every line is ready, preventing a mixed-language page | `reportRpgmError` distinguishes batch start, HTTP, and parse failures; `reportRpgmLayoutFallback` records bounded control-guard rejections in `window.__deepSeekRpgmDiagnostics` |
| RPG Maker mixed-language display acceptance | A provider or existing cache entry returns CJK text with a copied run of three or more source English words, producing a visibly half-translated line | Only the RPG Maker renderer knows which Latin spans are legitimate live names, controls, key labels, or isolated game terms; globally rewriting shared translation memory would corrupt other engine consumers | It can preserve the complete source line when a genuinely translated result intentionally retains a long source-language phrase, but it cannot accept that rejection as a translation success or rewrite the shared cache | `reportRpgmLayoutFallback` records the bounded `mixed-language-guard` context in `window.__deepSeekRpgmDiagnostics` and rate-limited `console.info`; no source or translated text is included in the diagnostic |
| RPG Maker `drawTextEx` CJK layout | MV/MZ and plugin renderers process no-space translated CJK glyphs without wrapping them to the live contents width, so long text crosses the window boundary | Only the live renderer knows the window geometry; inserting hard line breaks in shared translations would corrupt reuse across windows and engines | No; the hook measures the next glyph unit and invokes the engine's original `processNewLine`, while all glyph and control-code rendering remains in the original pipeline | `reportRpgmLayoutFallback` stores a bounded record in `window.__deepSeekRpgmDiagnostics` and emits rate-limited `console.info` entries with width and translated character count |
| Unity Mono reflection/rendering | Optional Unity/TMP/UGUI/Fungus member lookup or renderer operation throws | Unity versions and game assemblies expose different optional members | It can preserve source rendering, but the exception remains visible | `ReportCaughtException` logs caller/context, count, and first full exception through BepInEx/Trace |
| Unity Mono stripped bootstrap | A game removes `System.IO.File.WriteAllText(string,string)` from `mscorlib.dll`, causing `BepInEx.Preloader` to throw `MissingMethodException`, and also removes `Application.logMessageReceived`/legacy log callbacks so BepInEx Unity-log mirroring null-dereferences | The stripped framework and UnityEngine facade are produced by the external game build; the launcher cannot add methods to those signed game assemblies or make BepInEx bind a missing callback | The complete corlib override can expose behavior absent from the stripped profile, and `UnityLogListening=false` removes Unity messages from BepInEx's secondary log; activation therefore requires a missing `WriteAllText` metadata marker, uses only the official downloaded Mono payload, and leaves the original `Player.log` intact | Launcher logs record the inspected game `mscorlib.dll`, official payload validation/deploy result, every config edit/conflict, and restore outcome; `doorstop_config.ini` and `BepInEx.cfg` use one-time backups plus exact owned snapshots |
| Unity Mono stripped lifecycle/font APIs | A player keeps Unity types but removes `SceneManager.sceneLoaded`, `Canvas.willRenderCanvases`, or `Application.onBeforeRender` add/remove accessors, removes `Application.unityVersion`, `Transform.root`, `WaitForSeconds(float)`, `Font.CreateDynamicFontFromOSFont(string,int)`, `Font(string)`, or only the managed `Font.Internal_CreateDynamicFont(Font,string[],int)` declaration while the native internal call remains | These APIs and constructor bodies belong to the external stripped UnityEngine assemblies; shared server/cache code has no renderer lifecycle, player-version, coroutine-constructor, or font-factory authority, and replacing or redistributing a Unity assembly would cross the game/vendor ownership boundary | Missing optional events reduce one scan/pump signal but the driver and Harmony frame hooks remain; missing `WaitForSeconds(float)` uses a per-frame `Time.time` iterator with the same scaled-time intent; the version fallback affects only renderer-local font selection; direct `Transform.root` dependence was removed. Only a clearly stripped BepInEx 5 Mono deployment receives the first-party preloader patcher, which adds the missing metadata declaration to the in-memory TextRenderingModule and never copies a Unity DLL. The plugin allocates a constructor-free managed `Font` shell, invokes the exact retained native dynamic-font factory once, and requires live glyph coverage. Any failure keeps the TMP source renderer visible and cannot accept or persist a translation | The patcher emits `[DeepSeek Font Patcher]` records for no-op/missing-type/restored-declaration outcomes; launcher logs record stripped detection, patcher ownership conflicts, deploy, and restore; `RegisterOptionalStaticEvent`, `GetUnityVersionSafe`, the scaled-frame iterator, and `ReportCaughtException` retain lifecycle/version/font boundary context; `[FONT-LOAD]` and `[OVERLAY-GEOMETRY]` record font liveness/glyph and source-visible overlay decisions |
| Unity Mono TMP overlay renderer ownership | A stripped player cannot construct a TMP CJK font, the game later refreshes an overlaid TMP component and reveals English beneath Chinese, or an inactive save-slot template is translated before the game enables and replaces its label | Only the live Unity renderer owns component activation, reuse, and color/text refresh timing; neither the local server nor shared translation memory can control it | The original TMP `Behaviour` state is captured and disabled only at the instant the overlay has verified geometry and a usable CJK font—not while preparing an inactive template—and active validation reasserts ownership. If the game changes the source, validation removes the overlay and restores that captured live state; if validation fails, the source remains visible. The fallback therefore cannot turn a reused timestamp/control into blank or successful translated text | `[OVERLAY-GEOMETRY]`, `[FONT-LOAD]`, and `ReportCaughtException` record geometry/font/renderer failures; live diagnostics count overlay creation and geometry misses while source state is held in weak renderer-local state |
| Unity Mono TMP setter re-entry ownership | A game writes a second value such as a save timestamp into a TMP component while the translator scanner can still observe the previous overlaid label, allowing stale overlay validation to disable the freshly reused renderer | The `text` property, `SetText(...)` overloads, TMP callbacks, and scanner are separate live renderer boundaries owned by the external game/Unity runtime; the server and cache cannot make their callbacks atomic | Every string `SetText` overload and the property setter mark the component as being inside a game-side write; paired postfixes reconcile committed text, while finalizers reset only translator-owned depth if the external setter throws and never swallow that exception | `[OVERLAY-OWNERSHIP]` is emitted once when an existing overlay crosses this boundary; `pendingExternalWriteDepth` preserves nested-write context, and finalizer failures retain operation context through `ReportCaughtException` |
| Unity Mono TMP live color/layout ownership | A stripped TMP player renders translated CJK through a UGUI fallback while the game recolors the hidden source or stores its visible color in `faceColor`/`_FaceColor`; copying the first TMP color makes dark UI text white-on-white. Some games finish configuring or parenting a newly created TMP component only after its text setter returns: the overlay can remain a child of the formerly parentless source, inherit a tall narrow rectangle, and render a valid translation as tiny vertical text | RGB, material face color, hierarchy attachment, final rectangle, wrapping, alignment, and auto-sizing bounds are live prefab/renderer state owned by the external game; changing shared translation text or cache entries cannot repair that state | The overlay owns and restores only source alpha/enabled, preserves game-owned live RGB, combines retained vertex and face colors, follows the source to its current parent, and refreshes current layout properties during active validation. A clearly collapsed or tall-narrow source may borrow only its usable immediate-parent bounds under bounded size/ratio checks; this can alter fallback typography or over-expand an unusual legitimate narrow label, but it cannot hide a failed overlay as a successful translation because geometry must still verify before the source is hidden | `[OVERLAY-COLOR]` records live TMP color ownership; `[OVERLAY-LAYOUT]` records one-time hierarchy repair and parent-bounds fallback with measured dimensions; live-style reflection failures use `ReportCaughtException` with `phase=refresh-live-style`, while `[OVERLAY-GEOMETRY]` records an overlay that still cannot produce visible glyph geometry |
| Unity Mono stripped object/TMP discovery | A player retains active `TextMeshProUGUI` objects but strips lifecycle events and makes both `Resources.FindObjectsOfTypeAll(Type)` and `Object.FindObjectsOfType(Type,bool)` unavailable or return no objects, so visible text never reaches the translator | The live Unity/TMP registries and render queues are external renderer state; the launcher and shared server cannot enumerate scene-native objects or infer their component ownership | The plugin first tries the normal global APIs, then reads only the retained bounded `TMP_UpdateManager` internal update queue and observes retained TMP `Rebuild`/`DoRebuilds` calls. Missing or malformed registries can reduce discovery coverage, but source text stays visible and no miss, queued response, or original echo becomes a successful cache entry | `[TMP-REGISTRY-PROBE]` and rate-limited `[TMP-REGISTRY-DIAG]` report hook count, registered/active/nonempty counts, and font readiness; global API and registry reflection failures retain renderer/type context through `ReportCaughtException`; runtime diagnostics expose `TmpRegistryHookCalls` and `TmpRegistryRegistered` |
| Unity Mono stripped renderer members | A player removes `AssetBundle`, dynamic-type `GetComponentsInChildren`, `CanvasRenderer` alpha accessors, IMGUI `GUIStyle` measurement/font members, `Material.SetTexture(string,Texture)`, or `Texture2D.GetPixel` while retaining adjacent types or overloads | The external game's stripped UnityEngine assemblies own those members; shared translation/cache code cannot restore renderer ABI | It can skip packaged TMP bundles or optional layout/alpha diagnostics when no equivalent exists; where an equivalent survives it uses the generic hierarchy call, property-ID texture setter, or bilinear pixel sampler. These renderer-only fallbacks never accept a translation, convert an original echo into success, or write cache entries | Each selected/unavailable boundary emits a one-time `[FONT-PACK]`, `[LIFECYCLE]`, or `[RENDER]` record; reflective invocation failures retain method/renderer context through `ReportCaughtException` |
| Unity Mono IMGUI short-label layout | `GUI.Label(Rect, string, GUIStyle)` receives a fixed source-sized rectangle, while a short CJK translation measured with the active font needs more horizontal space and is clipped | Only the IMGUI call site owns the per-frame rectangle and style; changing shared translations or cached text cannot alter draw geometry | No; the rectangle is widened only in the source-aware translation branch for a single-line CJK label, and the original left/center/right anchor is preserved | `[IMGUI-LAYOUT]` records rate-limited original/required widths, visible/source lengths, and alignment without logging content; measurement exceptions use `ReportCaughtException`, and `ImGuiShortLabelWidth` remains in live diagnostics |
| Unity Mono short UGUI label layout | A layout-owned, single-line CJK translation is written while `ContentSizeFitter`/layout groups still expose the previous source label's width, wrapping the last glyph below a one-line `VerticalWrapMode.Truncate` rect | The game owns the prefab geometry and Unity's live layout cache; the shared server cannot know when or where the text will render | No; only source-aware short translations temporarily switch the live Text from `Wrap` to `Overflow`, weak renderer state restores the original mode before a later source/long-text write, and cache text is unchanged | `[UGUI-LAYOUT]` records rate-limited activation with component path, rect dimensions, source length, and translated visible length; renderer exceptions use `ReportCaughtException`, and `UguiShortLabelOverflow` remains in live diagnostics |
| Unity Mono TMP short-label group layout | A vertical group of equal-size TMP labels mixes left and centered source alignments, so equally short CJK translations no longer share a visible left edge | The alignment and anchored positions are serialized in each game's live prefab and are not represented in shared translations or cache data | It can normalize a centered label only when the same direct parent has at least two nearby, equal-size left-aligned peers; weak state restores the original alignment and position before later source text, so standalone centered buttons and tabs remain unchanged | `[TMP-LAYOUT]` records the bounded component path, peer count, and selected group x-position; reflection/layout failures use `ReportCaughtException` with prepare/restore operation context |
| Unity Mono TMP font reset | A game reassigns a source-only TMP font after translated CJK text is already present, while a fallback-aware TMP query reports coverage that the host atlas/material cannot render | The live game owns component font assignments and material timing; the shared server/cache cannot observe renderer atlas ownership | It can replace the component font and therefore its visual style, but only when the current renderer text contains CJK; it never rewrites or accepts a translation/cache entry | `[CJK-RENDER-DIAG]` records a bounded component path, font, material, atlas, layout, and visibility state; reflection/mesh failures use `ReportCaughtException` |
| Unity IL2CPP TMP fallback | IL2CPP reflection/member access or optional font operation throws | Generated interop types vary by Unity/IL2CPP build | It can skip one font repair, not accept a bad translation | `ReportCaughtException` logs the operation and rate-limited exception detail |
| Unity IL2CPP first-process TMP setter patch | BepInEx generates IL2CPP interop assemblies in the current game process and the TMP fallback immediately installs Harmony setter patches; on affected Unity 6000 builds the first patched invocation terminates CoreCLR with `0x80131506` at `DynamicClass.Invoker_VoidThis` | The generated wrappers, CoreCLR dynamic invoker, and first-process JIT state belong to BepInEx/Il2CppInterop; neither the translation server nor the renderer plugin can catch a fatal CLR termination | Only the setter Harmony patch is deferred for the process that wrote `BepInEx/interop/assembly-hash.txt` after process start. The existing two-second renderer scan keeps CJK fallback active, translations remain source-visible on failure, and the next game start installs the setter patch normally | `[TMP-SETTER-PATCH]` records the marker-relative generation time, exact CoreCLR boundary, active scan fallback, and next-start retry; marker inspection failures use `ReportCaughtException` with `detect-fresh-il2cpp-interop` |
| Unity IL2CPP TMP AssetBundle load | Unity may retain a native AssetBundle even when IL2CPP returns a null managed wrapper; replaying identical bytes through file, temp, memory, and stream loaders can then raise a native access violation | The shared server and translation cache cannot repair Unity's per-process native AssetBundle ownership | Only the bundle matching `Application.unityVersion` is attempted, once per existing physical path; a null result falls through to system-font creation and may leave source glyphs unchanged, but never becomes a translation/cache success | The fallback plugin logs the asset name and states that the path will not be retried in that process; thrown loader failures use `ReportCaughtException` with `load-font-bundle` and the path |
| Unity IL2CPP TMP native wrapper | `AssetBundle.LoadAllAssets(Type)` may return a real `TMP_FontAsset` native pointer through a managed `UnityEngine.Object` wrapper; passing that base wrapper into TMP lists throws `ArgumentException`, while trusting it without conversion can destabilize native rendering | The generated IL2CPP interop assembly, not the shared server or bundle bytes, owns the managed wrapper type returned for a native object | It can reject an asset whose native pointer or generated `TMP_FontAsset(IntPtr)` wrapper is unavailable; the renderer then retains source text or uses the renderer-local UGUI fallback, and no translation/cache result is changed | Successful conversion logs `Rewrapped native TMP font asset`; failures use `ReportCaughtException` with `rewrap-native-tmp-font`, wrapper type, and occurrence count |
| Unity IL2CPP TMP-to-UGUI display fallback | A game cannot deserialize the matching TMP bundle, strips dynamic TMP/font factories, or exposes translated TMP UI through base `Transform` wrappers that cannot render CJK | Only the live IL2CPP renderer owns generated types, native font calls, component geometry, and scene lifetime; changing shared cached text cannot repair them | It can use a legacy UGUI font/style instead of the game's TMP styling; the original TMP component is disabled only after the overlay accepts text and is restored when the overlay is removed, so a failed overlay remains visible as source rather than a successful translation | `UGUI font glyph probe`, `Loaded UGUI Chinese font`, and aggregate `TMP UGUI fallback scan` counts record font coverage, overlays, geometry misses, creation failures, and text-write failures; caught calls use `ReportCaughtException` with the renderer phase |
| Unity IL2CPP legacy UGUI dynamic glyph atlas | A translated value introduces CJK characters that were not present when Unity built the current dynamic OS-font atlas, so the translation is present but renders as white missing-glyph boxes | Glyph rasterization belongs to the live Unity UGUI font, size, style, and scene lifecycle; neither the shared server nor a TMP fallback table can populate that atlas | The renderer requests each changed CJK value at the component's current size/style and dirties the text. If the external font API fails, that component may retain missing glyphs, but the translation/cache result is neither changed nor reported as newly successful | `LastUguiTextById` bounds successful requests to changed live text; `ReportCaughtException` records `warm-ugui-glyphs`, component type, exception, and occurrence count |
| Unity IL2CPP interactive TMP cold layout | XUnity supplies a cached CJK choice while its source TMP component is still being constructed, so the live `fontSize` is a transient sub-10 measurement even though `fontSizeMax` retains the prefab's intended auto-size bound | Text assignment and later prefab/layout configuration are separate external renderer lifecycle steps; neither the endpoint nor shared cache can make the early TMP measurement final | The interaction-safe overlay uses the source auto-size upper bound only when auto-sizing is enabled or the current size is clearly transient, and retains a 10px interactive floor; this may enlarge a deliberately tiny interactive label, but it never changes translation/cache data or hides a failed overlay as success | `[INTERACTIVE-LAYOUT]` logs the first cold-size repair in the process; reflection/property failures remain visible through the surrounding TMP fallback diagnostics |
| XUnity queued-result endpoint | The local cache-first server returns `queued`/`miss` before remote work completes, while XUnity counts each unresolved extraction as an endpoint error and disables the endpoint after a short consecutive run; transport errors, malformed responses, and provider timeouts remain possible | The server must return queued work immediately and XUnity owns its coroutine/error threshold, so neither shared cache semantics nor remote API latency can be changed to look synchronous | The endpoint keeps the XUnity coroutine alive and polls only the pending subset through `/cache/lookup` at a bounded cadence for at most the configured wall-clock budget; `pass` and resolved items are excluded, while timeout, malformed, unknown-source, or non-pass source-echo results still call `context.Fail` | XUnity logs the final explicit endpoint failure after the bounded wait; HTTP transport exceptions remain attached by `HttpEndpoint`, while `QueueWaitSeconds` and `QueuePollIntervalSeconds` in the launcher-owned config record the active boundary |
| XUnity progressively appended mixed-language text | A game appends a new English passage to a TMP component whose earlier prefix XUnity already replaced with Chinese; the shared server would correctly classify the unchanged mixed component as CJK `pass`, and XUnity's previous 400-character input limit could reject the growing component before the endpoint | Only the XUnity endpoint sees both the progressively mutated component and the server request boundary; weakening the shared CJK heuristic would retranslate normal Chinese text for every engine | Existing basic-CJK runs are replaced with stable ASCII variables only when a meaningful Latin passage remains, the IL2CPP deployment limit is XUnity's supported maximum of 2500 characters, and every variable must survive exactly once before the Chinese segments are restored. A missing or duplicated variable fails closed, leaves source visible, and never creates a successful XUnity mapping | `ProtectMixedCjkForRequest` and `TryRestoreMixedCjk` report token loss through `context.Fail`; the real endpoint DLL probe covers long mixed input, restoration, and token-loss rejection, while launcher tests pin the deployed limit and its first-start normalization |
| XUnity owned-config migration after first launch | XUnity adds provider defaults and endpoint-created settings to `AutoTranslatorConfig.ini`, so the live file no longer byte-matches the launcher's ownership snapshot even when the launcher-managed long-text value is unchanged | The additions are written by the external XUnity runtime after deployment; replacing the whole file would discard provider/user settings, while treating every addition as a user edit prevents required launcher migrations | Only `MaxCharactersPerTranslation` is migrated, and only when its live value exactly matches the prior ownership snapshot. All added sections remain byte-for-byte intact; a differing managed value is treated as user-owned and preserved | Launcher logs distinguish successful owned-setting migration, preserved user edits, read/write failure, ownership update failure, and rollback failure; `tests/launcher_probe.c` replays an old 400-character snapshot plus XUnity-added defaults |
| Godot 3/4 runtime sidecar | `HTTPRequest.request` error, non-200 response, invalid JSON/schema, or unknown callback ID | Export/runtime and local server are external to the generated sidecar | No; source text remains and failed batches enter network backoff | `_dst_report_error` emits rate-limited `push_warning` with request ID/result/status/error |
| Godot export runtime-sidecar launch | A Godot export rejects or cannot parse the generated `--script` entry point, exits nonzero during hidden preflight, or does not finish preflight before the bounded deadline | Command-line script policy and GDScript parsing are owned by the external export template and its embedded engine version; the launcher cannot safely assume that a script accepted by another Godot version will run here | It can fall back to an existing static patch or the original export, so live translation coverage may be absent for that run, but the fallback never records source text, queued work, or an original echo as a successful translation | `godot_runtime_sidecar_preflight` records process creation failure, timeout, or exact exit code before the launcher names the selected static/original fallback; generated-script regression tests reject the Godot 4 `SceneTree is Node` parse incompatibility |
| Godot PCK format 3 scan/patch | The pack-tail directory offset, entry count, path, or resource metadata is malformed or outside the copied pack | PCK structure is owned by the exported game and cannot be repaired in the shared server or another engine adapter | No; the original pack is never rewritten and an invalid copied patch is rejected | Godot patch/warmup logs identify unreadable PCK sources, missing overrides, and patch installation failures; `tests/godot_patch_probe.c` and `tests/warmup_probe.c` cover format 3 bounds |
| Godot 4.6 restricted export autoload | The release template disables `--script`/`--main-pack`, rejects `override.cfg`, cannot create the owned matching executable, or does not execute the embedded autoload | These switches and project-override build flags are compiled into the external Godot export template | No; hidden preflight requires a complete autoload proof line and a probe stopped by its launcher with the matching final exit code. Independent exits and failed stop/query operations fall back to the script/static/original path without accepting a translation | `godot_runtime_autoload_preflight` logs create, timeout, stop/query, and independent exit-code failures; patch creation logs override merge and launcher ownership failures |
| Server cache persistence write | `fopen`/`fprintf`/`fflush` on the TSV append path fails (permissions, disk full, file locking) | The OS owns the file system; the server cannot make a full or locked disk writable | It can leave a batch memory-only until a later batch retries the append; in-memory hits still work, but a restart loses the unflushed entries | `cache_diag` writes rate-limited reason/errno/path to stderr (first occurrences then powers of two), and load aborts with a `ferror` diagnostic instead of spinning |
| XUnity pass-through identity terminal | The server answers `source:"pass"` for text that must not be translated; treating it as a failure failed whole batches, while completing it normally would let XUnity persist `source == result` into `_AutoGeneratedTranslations.txt` | The server's cache-first contract intentionally returns `pass` immediately, and only XUnity's private job owns the `SaveResultGlobally` persistence decision | The endpoint marks only pass jobs session-only before completing the original text. Reflection is restricted to the bundled XUnity compatibility boundary; if its private layout changes, the endpoint fails closed instead of completing or caching an identity | `TryKeepIdentityResultsSessionOnly` reports the exact unsupported XUnity field/closure layout through `context.Fail`; mixed-batch pending-subset behavior is executed by `test_unity_endpoint_state.ps1` |
| Unity Mono batch source classification | A server `miss`/`queued`/`pass` source echo was counted as a rejected translation, abandoning the text for the whole session after three occurrences | The server must respond immediately while API work is pending; the plugin cannot make a miss synchronous | It can delay the next attempt by a transient cooldown, but the text is requested again and is never cached as translated | Per-item `sources[i]` classification is logged with `LogVerbose`; content rejections retain the existing `[RETRY]` diagnostics |
| Unity Mono TMP setter finalizer | The original TMP `set_text` throws between the prefix/postfix pair, leaving `pendingExternalWriteDepth` stuck and disabling overlay validation for that component | Harmony postfix hooks do not run when the original method throws; only a finalizer observes the exceptional path | It resets only the depth counter and rethrows the game's own exception; validation resumes on the next cycle | `ReportCaughtException` records the depth reset with rate limiting |
| Unity Mono local cache persistence | Interrupting `File.WriteAllText` mid-write corrupted `unity_translation_cache.json`, and write failures were invisible outside debug mode | Process termination and file-system errors are external to the plugin | It can lose one persist cycle; the previous snapshot survives because replacement is atomic (temp file then replace/move) | `ReportCaughtException` with `boundary=local-cache-persist` records write failures rate-limited |
| Unity Mono oversized local cache isolation | Legacy plugin versions could place a full server dump in the per-game JSON cache, making parse and every rewrite unbounded; the file may also contain user-curated rows | The current renderer cannot cheaply determine row ownership without first paying the unbounded parse/rewrite cost | Before persistence is allowed, the exact file is moved under the same file lock to a unique timestamped `.bak-oversized-*` sibling. It is not imported or reported as translated, and no previous backup is overwritten | A normal warning records the retained backup name and size/entry reason; `boundary=oversized-local-cache-isolation` records move failures without replacing the original |
| Ren'Py token guard rejection | A provider result drops or reorders Ren'Py `[...]`/`{...}` spans, or injects spans absent from the source; accepting it would break substitution or render wrong markup | Only the hook sees both source and result; the provider cannot be made span-safe | It keeps the affected line in source text as a terminal negative, then performs a bounded 30-second local-cache recheck so a corrected entry can be observed; it never displays or caches a span-unsafe result | `_ds_report_diagnostic` records rate-limited `renpy-token-mismatch` / `renpy-token-injection` and `terminal-negative-recheck` contexts |
| Godot `--main-pack` compatibility probe | Exported templates can reject launcher CLI flags, while unrelated headless autoload/audio/DRM failures can produce the same non-zero process exit | Only the exported executable can parse its own supported command-line surface; exit status alone cannot identify the failing subsystem | The launcher disables the static patch path only when captured output explicitly names `--main-pack` with an option-rejection diagnostic; other failures remain inconclusive and the translated launch is still attempted | `godot_main_pack_supported` records capture creation/start/wait failures, explicit rejection, timeout, or the unrelated non-zero exit code |
| Launcher Godot main-pack capability probe | A restricted export template rejects `--main-pack`, and the final static fallback would abort the game at launch | Command-line policy is compiled into the external Godot export template | It can launch without the patch pack, so that run has no static translations, but the game starts instead of aborting | Launcher log records the probe outcome and the plain-launch fallback |
| Launcher embedded script resource | `FindResourceW`/`LoadResource` fails or an RCDATA script (301–304) is missing/corrupted in a partial launcher build, so `deploy_renpy`, `deploy_rpgm`, or the Godot sidecar builder has no hook text | The launcher binary's resource section is produced by the build (`build_native.bat` RCDATA lines) and cannot be reconstructed at run time | No; deploy/patch stops and returns failure for that engine, nothing is written, and the game is not reported as prepared | `embedded_script_text` records the resource ID and Win32 error once per ID; the owning deploy path adds the user-facing consequence and target path. C# port: `EmbeddedScripts.Bytes`/`Text` log the logical name and refuse NUL-containing payloads |
| Unity Mono DLL ownership across translator updates | A newer translator payload no longer matches an older `UnityTranslator.dll` that this launcher deployed; comparing only with the current template would retain the old DLL as if it were foreign during IL2CPP switching or restore | The old installed bytes are external game-directory state and cannot be reconstructed from the new payload alone | A matching `.dst-owned` byte snapshot permits disabling/removing only the exact deployed DLL; a user-modified or unknown DLL is preserved, so an old install without a snapshot may still require manual inspection | `deploy_unity` writes the snapshot or fails with a logged Windows error; `deploy_unity_il2cpp` and restore log when a DLL cannot be disabled or its ownership cannot be confirmed; `tests/launcher_parity` covers old-owned and user-modified copies |
| Unity IL2CPP `.disabled` collision | A game already contains `UnityTranslator.dll.disabled` or `Il2Cppmscorlib.dll.disabled`; replacing it would destroy an existing backup. A historical `UnityTranslator.pdb.disabled` has no reliable ownership record | The conflicting target is external game-directory state; the launcher cannot know who created it or reconstruct overwritten bytes | DLL/interop conflicts stop deployment and one-click launch; PDB files remain untouched during deployment and historical disabled PDB files remain on restore, which reports an incomplete restore | `disable_existing_file` returns `ERROR_ALREADY_EXISTS` (183), the owning deploy path logs the exact file/error, and `restore_unity_il2cpp` logs preservation; `tests/launcher_parity` checks byte preservation and launch cancellation on both launchers |
| Unity Mono backup changed after its first snapshot | A user edits `UnityTranslator.dll` or `Newtonsoft.Json.dll` again while an older `.dst-backup` exists; reusing that backup would overwrite the new edit with no recoverable copy. A launcher payload upgrade can also change the DLL bytes | Only the live game directory contains the second edit; the first backup cannot represent it. The running launcher can distinguish its prior deployed bytes only if an ownership snapshot was recorded | A differing live file and backup stops deployment before that file is overwritten. Matching `.dst-owned` snapshots allow launcher-owned old DLLs to upgrade and restore; pre-snapshot installations with ambiguous old dependencies may require manual inspection | The Unity deploy path logs the affected DLL and Windows conflict code 183; snapshot write/restore failures log the path/error; `tests/launcher_parity` checks repeated edits, legitimate upgrades, matching backups, and one-click launch cancellation |
| C# launcher port coverage boundary | A caller reaches a C# launcher path that has not been ported. As of 2026-09-09 the port is complete — deploy/restore, warmup scanners for all five engines, embedded payload self-update, the Godot patch pack, the `api.ini` data layer and its dialog, the Godot launch preflight, the one-click launch flow, and the Win32 window itself are all ported — so this boundary now only guards future modules and engines | The C# port is a module-by-module migration; faking the C result or silently skipping would make parity tests green while the module is absent | No; unported entry points must return `Deploy.NotPorted` (`-1`) / exit 3 with a `[not ported]` log line, so a caller cannot mistake it for the C `0/1` result | The log line names the missing module; `tests/launcher_parity` only adds scenarios for ported modules, and the synthetic launcher root there omits `UnityMonoRuntime6X86` so the "missing payload + install command" branch is pinned rather than hidden |
| Launcher window render probe (`--ui-probe-and-exit` / `--ui-probe`) | The parity test needs the exact pixels both launchers paint, but a live window depends on the wall clock (`GetTickCount` animation), the host's DPI, the user's saved game directory, the real cache size, log timestamps, and whether a server happens to be listening on 19999 — none of which are comparable between two runs | Those inputs belong to the OS and the user's machine; rendering through a screenshot of the real window would compare the environment instead of the code | No; probe mode is opt-in (`g_ui_probe` / `MainWindow.ProbeMode`), never set by the GUI flow, and it only *removes* nondeterminism: DPI is forced to 96, `anim_tick()`/`Theme.AnimTick()` return the frozen `g_anim_tick_override`/`Theme.AnimTickOverride` instead of `GetTickCount`, the server state comes from an argument (`ServerProcess.SetStartedForProbe`), and `WM_CREATE` skips payload sync, the last-directory restore and the timers. The paint, layout and owner-draw code under test is the shipped code, unbranched — a probe-only drawing path would prove nothing | A render difference *is* the diagnostic: the `ui/*` scenarios compare the layout report line by line and the bitmap by SHA-256, printing the first differing pixel's coordinates and both colours on mismatch |
| C# launcher optional Win32 APIs (`Theme.EnableDpiAwareness`, `Theme.SetDpiForWindow`, `MainWindow.ApplyWindowChrome`) | `SetProcessDpiAwarenessContext` (Win8.1-), `GetDpiForWindow` (Win10 1607-), `DwmSetWindowAttribute` dark-mode/caption attributes, and `SetWindowTheme` are missing on older Windows, raising `EntryPointNotFoundException`/`DllNotFoundException` at the P/Invoke boundary instead of C's `GetProcAddress` returning NULL | These entry points ship with the OS; the launcher cannot add them | No; each catch covers exactly one import and falls back the same way the C launcher does (Per-Monitor V2 → V1 → `SetProcessDPIAware` → system virtualisation; `GetDpiForWindow` → `GetDeviceCaps(LOGPIXELSY)` → 96; title bar keeps the default light chrome). Only cosmetics or DPI crispness degrade — no translation, deploy or cache path is involved | The DPI fallbacks are silent in both languages by design (C's `GetProcAddress` branch is silent too, and the parity gate would break if one side logged); the chrome failures write the missing DLL/entry point to `OutputDebugStringW` |
| Launcher runtime identity strings | The rail footer tag and hero subtitle deliberately differ (`C native runtime` / `C# managed runtime`), so a user can tell which binary is running; comparing them in the pixel diff would fail every render, and harmonising them would hide which launcher produced a bug report | The difference is the point — it is the only user-visible way to identify the running implementation | No; `--ui-probe` swaps in the neutral strings *only* for the bitmap, and the real strings are asserted separately by `--ui-identity*`, so neutralisation cannot quietly spread to the shipped window | `ui/identity` fails if either binary reports the other's tag or subtitle, or if a probe render leaks a real runtime string |
| C# launcher server lifecycle (`ServerProcess`) | `Process.Start` throws `Win32Exception`, the child exits before `/health`, readiness times out, `/shutdown` transport/status fails, or `Kill` races an already-exited child | Same external boundaries as the C `server_proc.c`: process creation, port ownership, child initialization | No; start returns false after logging the exit code/timeout and terminating an owned child; a failed `/shutdown` to an *adopted* external server keeps the "running" state instead of reporting "stopped". The only swallowed exception is `InvalidOperationException` from `Process.Kill` on a child that exited between `HasExited` and `Kill`, which is the success case | Log lines mirror `server_proc.c` wording (`C 服务端在健康检查前退出，退出码：N`, `... 内未通过 /health 健康检查`, `发送服务端关闭请求失败。错误：<WebExceptionStatus>`) and the `StateChanged` event carries the label/button the UI must show |
| Launcher warmup dump mode (`--warmup-and-exit` / `--warmup`) | The parity test needs the exact `/prefetch` and `/cache/import` bodies both launchers would send, but a real POST requires a running server and would mutate the translation memory; the XUnity cached-rewrite branch would also modify fixture files | Scanner output is only observable at the HTTP boundary; a separate "collect only" API would duplicate the batching code and drift from the shipped path | No; dump mode is opt-in (`g_warmup_dump_stdout` / `Warmup.DumpSink`), never set by the GUI flow, and inside it `/health` is reported ready, `localhost_get_cached_translate` returns a forced miss, and every `local_http_post` prints `post=<path> <body>` instead of connecting, so no translation-file rewrite or cache write can happen | Each `warm_*` scenario snapshots the fixture tree before and after both dumps and fails if anything changed; the dumped bodies are compared byte-for-byte |
| Launcher warmup C# `LocalHttp` submission/readiness | `HttpWebRequest` throws `WebException` (refused/timeout/protocol) or the local server answers non-2xx to `/prefetch` or `/cache/import` | The local server is a separate process; the launcher cannot repair its availability from the scanner | No; `Post` returns false and `WaitReady` returns false after the deadline. `WebException` responses are read for the status code and closed; connection-level `WebException` without a response returns false silently, which is the same as C's `WinHttpSendRequest` failure path; other exception types propagate | The first non-2xx per session logs `预热批次被本地服务端拒绝（HTTP N），该批次未入队。` and later rejections stay quiet, exactly like C's `LocalHttp.reject_logged` guard; readiness timeout logs `预热跳过：本地服务端未及时就绪。`. XUnity immediate-miss submission failure additionally records the lookup boundary diagnostic below |
| XUnity batched cache lookup (C `xunity_lookup_*`, C# `LocalHttp.Lookup`) | Local HTTP refusal, non-200, timeout, truncated response, invalid UTF-8/JSON/shape/NUL, or a valid multi-key response larger than 2 MiB | Availability and cache values belong to the separate server; the scanner cannot repair them or reduce an existing translation's size | No; only a complete validated hit replaces a line. Faults retain original lines and queue candidates for background work. Only confirmed oversize responses split; network/JSON faults do not retry. A single-key response permits up to 64 MiB to cover the existing 8-MiB encoded cache-row limit with worst-case JSON escaping | First failure per session logs `Unity warmup: cache lookup failed` with stage, HTTP status and platform error; first oversize split logs `cache lookup response exceeded batch limit`. `tests/launcher_parity/run_warmup_http_tests.ps1` checks writeback/backup, faults, large values and exact C/C# requests |
| Launcher Godot pack scan (C and C# `godot_warmup`) | A `.pck` or embedded `pck` section has a malformed directory (zero/oversized path length, entry table past the pack end, unknown format, > 200000 files, `file_base` beyond the pack), an entry whose data offset/size leaves the pack, or the pack file cannot be opened/read (`CreateFileW`/`FileStream` failure, short read) | Pack layout is owned by the exported game and cannot be repaired by the launcher; the scan is a read-only best-effort text discovery | No; a directory-parse failure makes `scan_godot_pck_file`/`ScanGodotPckFile` return false and the whole file falls back to raw byte-run scanning (same as a foreign or corrupt pack), single bad entries are skipped, and nothing is ever queued as a translation — texts only enter `/prefetch` | Same in both languages by design: the fallback is silent (no log line), which is the pre-existing C behavior pinned by `warm_godot_pck_v2_v3_fallback`; a mismatch between the two scanners is the diagnostic and fails `tests/launcher_parity` |
| Launcher embedded payload self-update (C `self_update.c` and C# `SelfUpdate`) | A required RCDATA/EmbeddedResource payload (101–106) is absent from a partial build, or a target under the launcher root cannot be replaced: temp write fails, `MoveFileSafe` refuses (target is a directory/reparse point, in use by a running server or game), or the streaming compare cannot open the existing file | The launcher binary's resource section is produced by `build_native.bat`/the csproj `EmbeddedResource` items and cannot be reconstructed at run time; a locked target is owned by another process | No; a missing required payload counts as `missing_important` and a failed write counts as `failed`, both make `sync_embedded_payloads`/`SyncEmbeddedPayloads` return false (`--sync-payloads-and-exit`/`--sync-payloads` exit 5), the `.dstmp` temporary is deleted and the previous target file stays intact, other payloads are still synced. Optional Unity assemblies (201–205) are allowed to be absent without a log line, the pre-existing C rule (they require a local BepInEx reference build) | `Built-in component update failed: <label>`, `Warning: launcher was built without N required embedded component(s).`, `Warning: N built-in component(s) could not be updated. ...` — identical wording in both launchers, mirrored to stdout in the diagnostic mode and compared by the four `sync-payloads` checks in `tests/launcher_parity` |
| Launcher Godot patch pack build (C `godot_patch.c` and C# `GodotPatch`) | The source `.pck`/embedded section cannot be copied or its offsets normalised, the copy's directory ends before all declared entries, the local server is unreachable or answers a batch with the wrong shape, a resource fails to parse (invalid UTF-8 payload, unknown GDScript variant type, out-of-range OptimizedTranslation record), a translation would break bracket/`%s`/`%d`/`%1.2f` formatting, no system CJK font exists, or the finished pack cannot replace a pack the running game still holds open | Pack layout, resource encoding, and the installed font set belong to the exported game and the OS; the translation itself comes from a separate process | No; every stage is fail-closed on a copy under `dst_godot_patch.building` while the original `.pck`/EXE stays read-only. A rejected translation leaves the original bytes in place, a resource with no accepted string is not rewritten, a pack with zero patched resources is deleted instead of installed (`no patch output for the packaged game`), and a format 3 pack whose runtime embed fails is discarded — a partially patched pack is never installed. A busy active pack degrades to `dst_godot_patch.next.pck` for the next launch, which is reported, not silent | `Godot: local server was not reachable…`, `Godot: PCK directory ended before all declared entries were parsed.`, `Godot: no system CJK font found …`, `Godot: existing override.cfg is unreadable…`, `Godot: failed to embed the runtime translator and rebuild the format 3 PCK directory.`, `Godot: failed to install refreshed patch pack (replace error N, stage error M).`, and the closing `Godot: patched N resources with M translated strings.` — identical wording in both launchers, compared with the built pack bytes and the `/batch` bodies by the `godot-patch/*` scenarios in `tests/launcher_parity` |
| Launcher Godot PCK entry end-of-buffer | `patch_pack_file` in `godot_patch.c` allocated exactly `entry.size` bytes, but the text collectors scan that buffer with C-string semantics (`json_parse_string` stops at a quote or NUL), so an entry ending inside an unterminated string literal could read past the allocation | Entry payloads are external; only the reader can bound its own scan | No; the buffer is now `size + 1` with an explicit NUL, which is what the collectors already assumed. Detected 2026-09-09 while porting the module, since the C# side reads through bounds-checked `ByteStr.At` and would have diverged on such an entry | Both sides now stop at the same byte; the `godot-patch/gp_pck_v1_resources` fixture covers JSON/scene/markdown entries end-to-end |
| Launcher RPG Maker JSON scan end-of-buffer | `parse_rpgm_json_file` in `warmup.c` advanced one byte past the NUL terminator when a `data\*.json` file ended in trailing whitespace (`json_ws` returned the NUL, then `p++` ran unconditionally), an out-of-bounds read that MSVC/MinGW builds usually tolerated but is undefined behavior | File contents are external; only the scanner loop can bound its own pointer | No; the loop now breaks at the NUL. Detected 2026-09-09 when the C# port (bounds-checked `At`) produced the same texts but flagged the C loop shape | `warm_rpgm_flat_and_limits` fixture (`Armors.json` ending in CRLF/space/tab after `]`) keeps both scanners in step |
| Launcher one-click flow dry run (`--launch-flow-and-exit` / `--launch-flow`) | The parity test needs the exact process the launcher would start for each engine, but really starting a game (or the detached patch worker) would leave live processes behind on the test machine | The decision is only observable at the process-creation boundary; a separate "planner" API would duplicate the branch logic and drift from the shipped path | No; the dry run is opt-in (`g_launch_dry_run` / `LaunchFlow.SpawnSink`), never set by the GUI flow, and it replaces only the three points that start something (game `ShellExecuteW`, Godot `CreateProcessW`, patch worker) with a printed plan line. Everything that decides *which* line to print still runs for real, including the Godot headless preflight probes, which really start the game executable — a probe answered with a canned result would test nothing | Each `launch-flow` scenario compares stdout, the resulting tree, and the command lines the probed executable recorded, so both the decision and the probe arguments have to match |
| Launcher `path_join` self-aliasing | `path_join(out, cap, out, b)` cleared `out` before formatting, so `unity_is_il2cpp` compared `is_dir("il2cpp_data")` (relative to the CWD) and the `*_Data\il2cpp_data` IL2CPP marker never fired; games without a root `GameAssembly.dll` were classified as Mono | C `snprintf` with overlapping buffers is undefined behavior; only the call site can avoid it | No; `path_join` now appends in place when `a == out` (fail-closed on overflow with `ERROR_INSUFFICIENT_BUFFER`) and the call site uses a separate buffer. Detected 2026-09-08 by `tests/launcher_parity` when the C# port reported `Unity (IL2CPP)` for a fixture the C build called `Unity` | The parity test's `unity_il2cpp_data_dir` fixture pins the corrected behavior on both launchers |

Cache tail recovery (2026-09-17): an interrupted append or an externally
provided file without a final newline can leave the next record outside a TSV
boundary. Only the cache IO owner can inspect this disk state; the in-memory
map cannot repair it. C and C# check the final byte when opening the writer,
append only a missing separator, and preserve all earlier bytes. The anomaly
is visible as `[cache] persist-tail`; open/read/seek/write failures remain
unconfirmed and retain dirty entries. Partial-byte failure and restart guards
protect this recovery, complete final records and adjacent engine markup.

XUnity allocation failures (2026-09-17): resource pressure can make the native
`ByteBuf` append silently fail; the scanner cannot repair OS allocation. The
XUnity boundary checks actual encoded lengths when constructing requests,
receiving responses, decoding strings, assembling output and importing values.
Any mismatch rejects that buffer before file writeback or import, preserving
the original file. C# catches `OutOfMemoryException` at its corresponding HTTP,
decode, row-output and import-encoding boundaries. Failures are not reported as
successful translations and log the rate-limited `cache lookup failed` with
`allocate` and error 8. Six native allocation-injection scenarios verify the
original file/backup, diagnostics and absence of damaged cache imports; the
normal C/C# parity and adjacent RPG Maker cases remain enabled.

Provider catalog discovery (2026-09-18): a provider may omit GET `/models`, reject
the key, redirect, time out, or return malformed/incomplete/oversized JSON. The
launcher cannot repair provider availability or account permissions upstream.
At the catalog boundary, discovery fails visibly and leaves manual model entry
available; it never substitutes a static preset list as a successful online
result, changes the chosen model, or saves configuration. The window status and
launcher log record stage plus HTTP/transport error codes, while debug output
records completion even if the window has closed. URLs, credentials and raw
response bodies are omitted. HTTPS is required except explicit loopback hosts;
redirects are disabled so credentials cannot follow a new destination. Response
size, item count, UTF-8 ID length, JSON depth and request/read time are bounded.
The Anthropic host uses its documented headers and `after_id` cursor pagination.
DashScope's documented same-host `/api/v1/models` uses `output.models[].model`,
`page_no` and `output.total`; its host may require an account workspace, which
the launcher cannot infer. No key is sent to a guessed replacement host. The
SiliconFlow adapter requests `sub_type=chat`. Bounds apply across pagination:
64 pages, 4096 unique IDs, 16 MiB overall and 60 seconds between bounded network
operations. Missing/repeating cursors, no-progress pages, changed totals and any
later HTTP/JSON failure discard all partial results and emit the same visible
stage/HTTP diagnostics. Manual entry remains available without hiding the fault.
Presets no longer contain model names. A selected ID missing from a complete
catalog may be a valid alias or private model: only the provider can determine
that, so the UI retains it with an explicit membership warning and fetch time,
allowing a deliberate manual save. This warning is shown in the dialog; the
completion log records catalog count, not the potentially private model ID.
Changing endpoint/key invalidates in-flight results; workers own copied inputs
and never access HWNDs. UI completion polling observes a worker handle/task, not
an assumed fixed network delay. Fake-provider guards cover these fallbacks and
normal discovery without touching user files.

Catalog adapter protocol references (checked 2026-09-18):
[Claude Models](https://platform.claude.com/docs/en/api/models/list),
[DashScope Models](https://help.aliyun.com/zh/model-studio/list-models),
[SiliconFlow Models](https://api-docs.siliconflow.cn/docs/api/models-get).

The short sleeps in launcher health polling and server foreground-priority
scheduling are deadline/scheduling boundaries, not attempts to repair a state
race. They must not be expanded into blind retry loops.

## Godot PCK v4 and first launch (2026-09-22)

- Boundary: Godot 4.7 exports can use PCK v4 and release templates can ignore
  `--script`. A sidecar-only first launch then starts an untranslated game.
- Ownership: only the Godot launcher can read the pack and embed a runtime;
  the shared translation cache/server cannot repair export loading behavior.
- Recovery: ordinary v4 packs share the v3 directory layout. Both launchers now
  read it, preserve the original pack, and embed the runtime in a separate copy.
  Before the first launch a runtime-only pack is prepared without HTTP or static
  resource translation; ordinary warmup and background refresh still run later.
  This does not claim a successful translation or put originals in the cache.
- Diagnostics: source/copy/embed/install failures retain existing stage logs;
  encrypted/sparse/unknown pack flags and encrypted/removal/delta entries are
  explicitly rejected for patching. Warmup skips flagged v4 entries with a log.
  The initial path logs `preparing initial runtime pack without translation requests`.
  Its success is checked by autoload preflight before use.
- Protocol evidence: [Godot's pack reader](https://github.com/godotengine/godot/blob/master/core/io/file_access_pack.cpp)
  and [pack flags](https://github.com/godotengine/godot/blob/master/core/io/file_access_pack.h),
  inspected 2026-09-22. Guards cover ordinary/embedded v4, flagged packs,
  runtime injection and first-launch behavior alongside v1-v3 and other engines.

## Model catalog and INI boundaries (2026-09-22)

- Windows Profile APIs cannot represent every model ID in an ANSI INI. Only an
  explicit save that needs Unicode prepares a UTF-16 temporary copy, writes all
  three API values, flushes the file and atomically replaces the target. Existing
  unrelated content is retained; rejected lengths/read-only files keep original
  bytes. Save failures log the stage and Windows error; opening/refreshing a
  catalog never converts configuration. Both server readers encode model IDs
  as UTF-8 and reject IDs exceeding their 255-byte wire buffer.
- A provider can repeat pagination fields or close the response before its
  advertised Content-Length. The catalog reader owns these checks; a provider
  cannot be repaired by selecting a guessed default. Duplicate consumed fields,
  invalid JSON numbers, and truncated responses discard the catalog and expose
  failure status, retaining the user's selected value. Legitimate close-delimited
  and chunked responses remain accepted. No response failure becomes a successful
  partial catalog. Fake-provider tests cover both implementations; the general
  server JSON parser retains its existing duplicate-key/depth behavior.

## Godot source updates and runtime proof (2026-09-23)

- Boundary: an exported game's PCK or EXE can be replaced by an updater while
  preserving its filename, size or timestamp. The launcher cannot repair that
  upstream update, so its owned runtime pack records a byte fingerprint of the
  source PCK, both bundled Godot runtime scripts and the translator version;
  its owned EXE copy is compared with the current game EXE.
  Mismatches rebuild the runtime or refresh only the owned EXE. A source that
  changes during the build discards the generated copy. The original PCK/EXE
  and user translation memory are never rewritten.
- Boundary: some export templates ignore `--script` and exit normally. Exit 0
  alone cannot prove a translation hook loaded. Newly generated Godot scripts
  answer the private preflight argument with exit 73; only that result counts
  as runtime proof. Old unstamped v1/v2 patch packs keep the earlier exit-0
  compatibility path. The preflight signature includes the proof protocol and
  pack identity, so an earlier cached success cannot certify a rebuilt pack.
- Recovery and visibility: when runtime proof fails the existing static or
  normal launch fallback remains available, but the final launcher status says
  the live module was not confirmed. The exact probe exit code, timeout or
  process error is logged. An unrecognized future PCK layout is diagnosed and
  left unmodified, never treated as a successful translated pack.
- Guards: launcher parity covers C/C# byte-identical source markers, same-size
  same-time PCK changes, payload/version stamp validation and migration, owned
  EXE refresh, ignored preflight exit 0, legacy v1 behavior, marker cleanup on
  restore, and adjacent engine flows.

## Ren'Py and RPG Maker font ownership (2026-09-26)

- Boundary: a game or mod can already contain `ds_font.ttf/.ttc/.otf`, can edit
  a font after deployment, or Windows can replace/remove the source font.
  The launcher cannot infer ownership from the filename or repair a changed
  external font upstream. It now creates a small `.dst-owned` marker only when
  it copies a new system font, and never overwrites an existing font.
- Recovery: restore removes the copied font only when the marker has the
  expected source identifier and the file still equals that system source.
  Missing/invalid markers, changed fonts and changed system sources are
  preserved. This can leave an old launcher-created font from a version that
  did not write markers; preserving an ambiguous file is intentional and does
  not mark a translation or cache hit as successful.
- Visibility: deployment logs copy or marker-write failures; restore logs each
  preserved font and includes it in the `preserved` count. The C/C# launcher
  parity scenarios check user TTF/OTF preservation, edited owned fonts,
  launcher-owned font removal, and adjacent engine flows.

## Bundled CJK fallback when Windows has no Chinese font (2026-09-26)

- Incompatibility: Windows installations with no usable CJK font file leave
  Chinese translations as missing glyphs in Ren'Py, RPG Maker, Godot, or Unity.
- Upstream boundary: an exported game cannot add a missing OS font, so the
  launcher embeds an unmodified Noto Sans CJK SC OTF and its SIL OFL text.
- Visibility: the fallback can make a game display text that previously failed,
  but cannot hide a missing/corrupt embedded font, a user file conflict, or a
  renderer that rejects external OTF. These produce diagnostics and no
  successful font-deployment result.
- Diagnostics: `CJK font bundle resource ...`, `CJK font bundle asset ...`,
  `CJK font copy failed ...`, and per-engine failure logs record the path and
  Windows error. Restore requires matching `.dst-owned` markers and byte-for-byte
  font/license matches; modified files are reported and preserved.
- Godot patch packaging creates `dst_godot_patch.OFL.txt` only when the bundled
  font is selected. A conflicting license cancels the patch; restore verifies
  exact license bytes and its ownership marker before removal.

## RPG Maker plugin parameter warmup (2026-09-27)

- Incompatibility: RPG Maker MV/MZ plugins can draw save/help/menu labels from
  `js/plugins.js` parameters, outside the standard `data/*.json` scanner. A
  malformed or oversized third-party manifest cannot be repaired by the
  launcher without rewriting the game's own plugin configuration.
- Fallback: the launcher only reads generated manifests up to 8 MiB and
  selects display-like values from enabled plugins before the general resource
  scan. Unsupported manifests are skipped and normal data scanning continues.
  This can leave plugin labels untranslated on first display; it never records
  a cache hit or successful translation for a skipped value.
- Diagnostics: the warmup log distinguishes oversized, unreadable, unexpected
  and incomplete `plugins.js` files. The existing 40,000-entry cap warning
  remains visible when later resources may have been skipped.
- Guards: C/C# launcher parity checks active and disabled plugins, UI and
  technical parameters, scan ordering, flat MZ layouts, malformed-manifest
  fallback, and isolation from Ren'Py scanning.

## Launcher Explorer file drop registration (2026-09-29)

- Incompatibility: a Windows child control may reject `SetWindowSubclass`, so
  Explorer can deliver `WM_DROPFILES` to a control without a forwarding handler.
- Upstream boundary: the failure occurs at the Win32 control boundary; the
  launcher cannot force an OS control to accept subclassing.
- Fallback: the main window remains a drop target, and directory selection and
  the browse button still work. A child area whose subclass failed may not
  accept a drop; this cannot create a false translation or cache entry.
- Diagnostics: both launchers append `拖放初始化失败` with the control ID and
  Win32 error code. The `ui/drop-*` parity scenarios check the main window,
  path field, start button, log control, automatic start decision and C/C# parity.

## Godot localhost HTTP response and pause boundaries (2026-09-30)

- External failure: the local service can stop after HTTP headers or a partial
  body, return a failed transfer with an HTTP status, or send a `translations`
  field/value of the wrong JSON type. Godot's default HTTPRequest timeout is
  zero (unbounded), so four stalled requests previously occupied the entire pool.
  Nonstring values were coerced into display translations; Godot 4 also called
  `has` on a string/null map and raised a script error.
- Ownership: the game-side adapter cannot finish a stalled external process or
  repair its response upstream. Each owned HTTPRequest now has a 12-second
  deadline. Both callbacks require `RESULT_SUCCESS`, HTTP 200 and a translations
  dictionary; each accepted entry must be a nonblank, nonidentity string.
  Original engine tags and shared translation memory are not normalized here.
- Recovery visibility: a failed batch releases its busy/pending ownership and
  uses the existing eight-second miss backoff. A malformed entry alone remains
  unresolved while valid adjacent entries can still be accepted. This may leave
  source text visible until a later successful response, but cannot hide the
  original failure or turn it into a successful cache record.
- Diagnostics: existing rate-limited `batch-http` includes request ID, engine
  result and status; `batch-schema` records the missing dictionary; `batch-value`
  records request ID, source length and actual Variant type. Existing JSON parse
  diagnostics remain active. Real-engine tests inspect both state recovery and
  diagnostic counts instead of treating absence of a crash as success.
- Pause ownership: an external Godot 3 game may pause its SceneTree while its
  menu stays visible. Only the translator's owned HTTP nodes and scan timer now
  use `PAUSE_MODE_PROCESS`; Godot 4 already uses `PROCESS_MODE_ALWAYS`. This
  keeps translation/timeout processing alive without changing game pause state.
  Request failures continue through the same diagnosed boundary.
- Evidence: `test_godot_http_contract.py` reproduces wrong-value caching, Godot 4
  map errors, four-slot stalls and Godot 3 paused stalls before the fixes. With
  configured real Godot 3.6/4.3 engines it checks autoload and sidecar modes,
  normal strings, blank/identity rejection, markup, recovery and paused menus.
  Timeout semantics follow the official
  [Godot HTTPRequest documentation](https://docs.godotengine.org/en/4.3/classes/class_httprequest.html).

## Managed build fingerprint publication (2026-09-30)

- Incompatibility: Windows rejects truncation of a file with a live memory-mapped
  reader (`ERROR_USER_MAPPED_FILE`). A final build encountered this on a JSON
  payload stamp even though the fingerprint contents were unchanged.
- Ownership: a build script cannot close another process's mapped view. The real
  stamp writer now skips byte-identical records and writes changed records to a
  unique sibling before atomically replacing the destination. Existing readers
  keep their previous complete view; new readers see the complete new record.
- Visibility: this avoids an unnecessary destructive write, not a failed
  fingerprint check. A reader that denies replacement, inaccessible directory,
  unsupported filesystem replacement, or failed write still stops the build.
  No delay, retry, or fabricated success is introduced; the old stamp remains
  intact and the temporary sibling is removed.
- Diagnostics: the narrow write boundary records the payload name, stamp path
  and original exception with `Managed fingerprint write failed`, then rethrows.
  The actual external reader from the observed build was not identified; the
  regression fixture owns a known mapped reader and reproduces the same Windows
  error before the fix, then checks replacement-denying ownership separately.

## RPG Maker visible UI completion and reconnect (2026-09-30)

- Incompatibility: the local server intentionally answers `queued` immediately;
  static menus have no completion event, and a disconnected/restarted server can
  leave the original request unqueued or lose its in-memory queue. Completed
  dialogue still awaiting input is visible even though `_textState` is null.
- Ownership: the server cannot refresh live MV/MZ windows or infer whether the
  player has left a page. The RPG Maker hook owns scene/window visibility, page
  signatures and engine input pause; no shared translation-memory changes apply.
- Visibility: ordinary UI checks only local cache after `queued`, retries failed
  requests at the existing cooldown, and restores enqueue permission after a
  cache transport/parse failure. Misses do not repeatedly redraw. Scene changes
  and loss of all visible refresh targets cancel polling; at most 128 keys poll
  for 60 seconds each.
  Retry callbacks retain the original deadline, so a late failure cannot restart
  expired polling. A completed message keeps its existing 15-second page deadline
  and can repaint while preserving the engine's input pause. Unresolved text
  stays source text and is never accepted as a successful translation.
- Diagnostics: HTTP and parsing failures remain recorded at the request boundary;
  `visible-ui-cache-poll-capacity`, `visible-ui-cache-poll-timeout`, timer
  scheduling and scene/window property failures are rate limited through
  `reportRpgmError`. A plugin property failure skips only its target; the poll
  worker is not wrapped in a broad exception handler. Message-page
  timeouts retain `active-message-cache-poll-timeout`. The public-method fixture
  in `test_rpgm_runtime_refresh.js` loads the complete shipped hook and verifies
  async cache/queue/reconnect, scene/page changes, window reuse, cancellation,
  cooldown and late callbacks; it simulates engine classes and does not establish
  compatibility with every commercial game's plugins.

## Ren'Py legacy main-thread refresh and late responses (2026-09-30)

- Incompatibility: Ren'Py 7.2 lacks `invoke_in_main_thread`; calling layout,
  redraw and restart APIs from an HTTP worker can race the engine. A delayed
  queued reply can also arrive after a cache poll has already resolved the text.
- Ownership: the hook cannot upgrade the game's engine or serialize external
  HTTP completion order. The existing engine periodic/interaction callback lists
  own legacy main-thread refresh, and `_ds_lock` owns memo publication.
- Visibility: a worker only marks refresh pending; legacy lifecycle callbacks
  drain it on the main thread. Callback lists preserve game handlers. If none
  can be registered, source text remains and the incompatibility is reported;
  there is no worker-thread UI fallback. A queued identity value cannot downgrade
  a positive memo entry; genuine new translations can still replace it.
- Diagnostics: registration exceptions name their callback list; unavailable
  lists and legacy deferral use rate-limited diagnostics. Existing per-displayable
  layout/redraw and interaction errors keep their original diagnostic boundaries.
  Four threaded guards and real Ren'Py 7.2/8.3 isolated Text-token fixtures verify
  ownership, coalescing, preserved handlers, tags and late-result behavior.

## Ren'Py 完成键与恢复对象的刷新依赖（2026-10-28）

- **外部边界：** 引擎正常复制、历史快照和状态恢复可能复用已经分词的 Text，不再次调用构造或 `update`。第三方也可能直接复制对象，或重写 `update` 自行分词。只在构造时登记会漏掉恢复对象；只记录某一次原始字符串会漏掉插值和自定义标签生成的正文。真实插件 Text 守卫复现了绕过基础 `update` 时迟到译文不显示的问题。
- **所有权与上游限制：** hook 不改变引擎的复制、存档和交互语义。实际 `update` 的线程局部栈记录 `_ds_fetch` 查询，成功更新后才替换弱键依赖；引擎 `_copy` 直接继承已知依赖。不在 Text 或存档数据上增加字段。无法继承依赖的干净对象首次进入 `per_interact` 时标脏并请求重绘，由正常分词重建自身依赖。
- **可见性：** 重建只影响该对象。绕过基础分词的对象无法从引擎外恢复其真实查询关系，因此独立弱集合中的未捕获干净 Text 保留原有保守刷新；待分词的脏对象下一次绘制会读最新缓存，无需额外刷新。已捕获对象仍精确匹配。保守路径可能多做布局，且有显式诊断，不伪装翻译成功；缓存 miss、透传和令牌还原失败保持原有处理。引擎分词异常原样传播，捕获栈在 `finally` 释放，失败的半次更新不覆盖上次依赖。完成键只在锁内合并/取走，worker 不操作显示对象。
- **诊断：** 未登记干净对象进入交互时记录限流 `text-dependency-rebuild`，干净插件对象保守刷新记录 `text-dependency-uncaptured`；依赖登记、未捕获登记、复制及重建失败分别记录 `track-text-dependencies`、`track-uncaptured-text`、`copy-text-dependencies`、`rebuild-text-dependencies`。原有主线程派发、旧版回调注册、布局/重绘和重启异常记录继续保留。
- **范围：** 依赖表仍会被扫描；匹配键的隐藏/历史对象也可失效，有匹配时仍重启一次交互。没有全场景可见性索引，也没有据此推导游戏帧率。

## Unity renderer attributes, stripped atlas metadata and XUnity JSON (2026-09-30)

- Incompatibility: TMP punctuation fallback previously also changed punctuation
  inside link/sprite identifiers. Some stripped Unity assemblies omit the
  `isReadable` property, so directly referencing it can fail compilation or JIT.
  The XUnity field scanner also accepted truncated JSON and blank translations.
- Ownership: tag attributes identify engine resources, and only the Unity
  renderer can distinguish them from visible text. The hook cannot restore APIs
  removed from the game's assemblies. Endpoint parsing uses the source-linked
  `DstCore.Json` implementation; shared translation memory is not rewritten.
- Visibility: all three Unity renderers preserve complete quoted tag spans and
  normalize only display text. Unavailable atlas metadata skips CPU diagnostics
  with an explicit `unavailable` result, preserving font/translation work.
  Invalid JSON, blank results or source echoes after display normalization fail
  the XUnity task; they cannot become successful translation/cache entries.
- Diagnostics: atlas property absence, invalid return type and getter exceptions
  record operation and exception through `ReportCaughtException`. XUnity initial,
  lookup and extraction parse failures use `context.Fail` with the original parse
  exception. Blank/identity batch failures identify the ownership boundary/index.
  The Unity contract tests exercise the built endpoint with actual SDK interfaces
  and source-linked renderer methods; full player/native glyph execution remains
  a separate verification level.

## Godot 4.6 加密目录与自动加载预检（2026-10-01）

- **外部边界：** PCK v3 的目录可单独加密；把密文当作路径长度会使运行时补丁构建失败。兼容层依据 Godot 4.6.1 `FileAccessEncrypted` 的 AES-256-CFB128 包装，在匹配的导出程序可写初始化数据区寻找编译期 32 字节数组（8 字节对齐、扫描最多 32 MiB/100 万候选）。整个目录的 MD5 和全部条目边界必须通过校验。密钥仅在内存中使用，不写入文件、配置或日志。
- **上游限制：** 已导出的游戏无法要求作者重新导出明文资源。仅在启动器自有 PCK 副本追加明文目录并清除目录加密位；原始 EXE/PCK 不变，文件级加密资源及其元数据原样保留，补丁层跳过这些资源。其他加密格式、异常标志、坏校验或无法定位数组均拒绝补丁。
- **预检边界：** Godot .NET 游戏可在主场景启动时后台加载资源；autoload 的 `_ready` 中立即 `SceneTree.quit(73)` 会与这些工作竞争，实际样本触发 `0xC0000374`。自动加载预检改为专用用户参数、完整 stdout 证明行和仍存活的探测进程；启动器在确认脚本实际进入 `_ready` 后结束自己创建的独立探测。普通游戏启动不带该参数；侧车 `--script` 的退出码 73 协议保持不变。
- **结束归属（2026-10-22 复查）：** Windows 可能接受对正在退出进程的 `TerminateProcess` 请求，但最终保留其自行退出码；也可能先拒绝请求、稍后才将进程句柄置为有信号。启动器用探针专用结束码并读取最终退出码，只有自己发起且最终匹配的结束才确认加载。拒绝停止后等待实际退出事件，沿用一秒收尾期限；未结束仍记录原始停止错误，绝不重试或延时放行。游戏退出由外部进程管理，无法从标签或脚本上游消除；日志区分真实退出、查询失败和停止失败。预检签名升级使旧结论失效，不更改译文或用户配置。
- **不掩盖失败：** 证明仅表示补丁脚本已加载，不宣称远程翻译或整个游戏运行成功。缺少证明、脚本错误里引用证明字符串、已退出/崩溃的进程、创建/读取/结束探测失败均不确认自动加载，也不缓存成功。加密资源跳过不成为成功译文或缓存项。
- **诊断与验证：** 加密目录和预检结果进入 C/C# 启动器日志，操作失败保留对应原因。`test_godot_encrypted_pack.py` 验证正常/损坏目录、不同密钥、边界、原文件保护和 C/C# 补丁字节一致；launcher parity 验证真实进程证明、误导性错误文本及提前崩溃。实际游戏另外验证 `/batch` 请求和控件文字应用。

## 选择动画与缓存译文绘制（2026-10-01）

- **外部边界：** 游戏动画可在翻译扫描后重写英文。Godot 3/4 控件的 `draw` 在本次画布命令生成后发出，延迟连接在同一帧队列收尾时只恢复该控件的已有译文，重新生成画布命令；不遍历场景，也不请求 HTTP。原有扫描仍负责未命中发现。
- **上游限制：** 作者脚本、插件、控制符和渲染队列属于游戏。仅在各自渲染边界修复：Godot 保留 BBCode；Unity Mono 的 TMP/UGUI setter 先查本地命中再对未命中片段去抖；RPG Maker 对原始反斜杠和已转换 ESC 控制符均按片段命中，保留当帧颜色、字号及变量。Ren’Py 已在 Text 分词前替换；IL2CPP 的 XUnity 端点和 TMP 层保留当帧标签与链接属性，不增加公共缓存改写。
- **不掩盖失败：** 未命中新选项保留新原文，不能冻结到旧译文，也不能将原文缓存为成功。Godot 若游戏 `_draw` 不断写回原文，可能形成重绘循环；每控件每处理帧最多恢复两次，超过后保留游戏文本。弱引用和原有控件/项目容量上限限制存活记录。
- **诊断：** Godot 信号连接失败记录 `scene-draw-connect`；循环保护实际阻止缓存命中时记录 `scene-draw-repeat`，含节点、属性或项目索引，按原有限频器输出。原有队列、HTTP、字体与容量错误继续按各自边界记录；循环保护可能留下未恢复原文，明确报告该限制，不伪造翻译成功。
- **回归：** `test_godot_animated_text.py` 在真实 Godot 3/4、autoload/sidecar 中观察最终 CanvasItem 命令与帧提交；RPG Maker 动画控制符和完整缓存相邻行为、实际 Ren’Py 7/8 Text 连续替换、Unity Mono 源链接 setter 与 IL2CPP 实际 SDK 端点均覆盖 60 次动画变化及未命中新内容。Unity 测试的字体/原生对象边界由夹具模拟，不等同于商业游戏完整游玩。

## Godot 设置选择器拆词与部分命中（2026-10-01）

- **外部边界：** 已导出的游戏可以把同一个选择器值拆到两个 URL 点击区域，例如 `< Gen` 与 `eral >`。按 BBCode 文本槽分别翻译会请求半词；整块正文必须全部命中的旧渲染方式又使一个未命中项阻止其他已命中项显示。数字分辨率 `0x0` 也会被误拆成待译的 `x0 >`，带斜杠的按键说明则被旧路径过滤拒绝。
- **上游限制：** 作者的文本拆分及 URL 标识决定点击动作，翻译器不能重写原始游戏脚本。Godot 3/4 的渲染与发现路径共用逻辑文本计划：同一行完整 `< ... >` 选择器及单词内部样式合并查询；译文按可见长度比例分配回原槽，标签、箭头和 URL 标识保留。数字值不翻译，实际路径继续过滤，操作说明允许斜杠。相邻独立链接、图片及布局标签仍隔开语义。
- **不掩盖失败：** 只显示已完成的缓存命中，缺少译文的余项保留当前原文并继续原有异步队列；原文不能成为成功译文。旧半词缓存记录不删除、不改写，只是不再用于完整选择器查询。没有完整箭头边界、图片/布局阻隔或路径判定时保持既有原文；不宣称其已翻译。该修复不改变共享服务器或其他引擎的文本拆分。
- **诊断与回归：** 请求、HTTP、未命中退避、容量与绘制循环继续由原有 `batch-request`、`batch-http`、schema 和 `scene-draw-repeat` 边界限频记录；计划函数不联网、不写缓存、不隐藏异常。`test_godot_settings_text.py` 在 Godot 3/4、autoload/sidecar 各观察 60 个实际提交帧，覆盖已命中与未命中混排、晚到译文、旧半词缓存、动态颜色、独立链接、多词选择器、数字值、斜杠说明和资源路径。HardStuck 独立副本另外通过真实元数据点击切换五个分类并返回重进，缓存仅只读使用；未命中的项目在该缓存隔离试验中仍保持英文，远程译文质量不由此证明。

## Godot 运行时译文新增标签（2026-10-22）

- 同日复查：内置标签名按引擎的大小写与完整名称识别，避免 `[B]`、`[b option]` 等字面提示被误拒绝；真实 Godot 3/4 解析结果与响应回调共同验证此边界。
- **外部边界：** 可见文本查询的外部结果可能带入新的 BBCode，迟到的颜色或链接标签会覆盖游戏当前格式，甚至替换已有安全译文。
- **上游限制：** 共享翻译记忆供多个引擎使用，不能为 Godot 改写其中的文本；显示标签及自定义效果由 Godot 适配层判断。HTTP 回调在写入本地显示缓存前校验结果，拒绝引擎标签、带参数或成对的自定义效果；字面按键括号继续允许。
- **可见限制：** 不安全结果不成为成功命中，保留已有安全译文；没有安全命中时保留原文并采用原有未命中退避。外部一直返回同一不安全结果时可能继续显示原文，不能宣称汉化完成。无参数、无闭合且不属于已知标签的自定义效果无法与字面括号可靠区分，仍按字面文本处理。
- **诊断：** `batch-markup` 通过原有 `_dst_report_error` 限频记录请求编号、查询和响应字符数；不输出用户正文。`test_godot_settings_text.py` 调用正式响应回调并观察真实引擎 60 帧，覆盖新命中拒绝、旧命中保护、动态颜色、链接、字面括号与有效后续结果恢复。

## 启动器减少动画设置（2026-10-12）

- 外部边界：Windows `SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION)` 读取失败。
- 上游限制：偏好由操作系统管理，启动器不能修复该系统调用；失败时以静态装饰显示。
- 可见限制：星轨与呼吸灯不再运动，悬停仍直接反馈；不改变翻译、缓存或按钮操作结果。
- 诊断：C `refresh_ui_motion` / C# `Theme.RefreshMotion` 用 `OutputDebugStringW`
  记录操作和 Win32 错误码，同一故障段只记录一次，读取恢复后重新允许报告。

## 启动器后台操作与关闭所有权（2026-10-27）

- **外部边界：** Win32 `CreateThread` 可能返回失败；托管线程创建/启动可能抛出 `OutOfMemoryException`、`ThreadStateException` 或 `SecurityException`。线程仍在服务就绪/停服、磁盘或游戏预热边界内时，窗口也可能收到关闭请求。
- **上游限制：** UI 无法保证外部服务立即退出或 I/O 立即返回；线程资源失败不能在服务层修复。同步回退会再次冻结窗口，直接销毁 HWND 会让在途 UI 更新失去所有者。
- **可见限制：** 线程创建失败时不执行后台操作，并释放预留供用户重试；已开始的操作持有窗口至真实线程完成，期间继续泵送消息。关闭请求会抑制最终弹窗，不能将失败改成成功。服务仍响应时取消缓存删除；成功删除但重启失败仍明确报告。执行器不捕获或吞掉整条业务流程的未知异常。
- **诊断：** 创建失败记录操作边界、Win32 错误码或异常类型与消息，并在状态栏报告。等待自有线程句柄失败只记录一次 Win32 码，保留窗口和忙状态，不虚构完成；原服务/部署/持久化诊断继续输出。UI 记录延后关闭；工作线程不释放 UI 所有权或投递携带裸 HWND 的完成回调。
