# pass 迁移 golden 对拍管道

轨道 B（对标设施）第一件落地：以三个试点 pass（ClassMemberSorter /
IllegalClassFileCleaner / AccessModifierTightener）验证的「旧栈输出 = golden、
mjbase 输出 = 对拍对象」字节级验收机器，供后续 pass A/B 批（29 个）复用。
语义对齐迁移计划 §0 规则 2（双栈对拍、golden 切换前提取）与规则 3（输出有变
必须显式声明）；pass 契约语境见 [pass-contract.md](pass-contract.md)。

A 批四个方法体 pass（NopRemover / RedundantLoadValEliminator /
CharArrToStrSimplifier / UnusedCheckCleaner）已按既有管道入对拍；B 批
JunkCodeInserter 无旧栈对标，走自基线道（见下文专节）。

## 管道结构（两侧各一入口，golden 只从旧栈流出）

```
旧栈侧（私有侧统一提取单测）
  MJBASE_PASS_GOLDEN_REGEN=1 驱动；默认零动作（回归面无感）
  1. 语料规范化：源类 ──旧栈恒等写回──> 冻结语料（两条规范形道）
  2. golden 提取：语料 ──旧栈 pass 原样运行──> golden 字节落盘
  产物：mjbase/test_resource/pass_golden/{corpus,golden}/（基线，入库）
  自检：canonical(canonical(x))==canonical(x)（规范形必须是旧 writer 不动点）

mjbase 侧（mjbase/test/unittest/pass/golden/pass_golden_compare_test.cpp）
  同语料 ──defaultPassRegistry 按名构造 pass──> 写回 ──逐字节 vs golden──>
  一致＝绿；确有正当差异＝manifests/<Pass>.manifest.txt 声明类目内改跑替代断言
```

## 语料策略：双规范形道（字节级对拍成立的前提）

语料必须先是旧 writer 的不动点，no-op pass 的 golden 才等于语料，mjbase 的
copy-through 写回（未触碰区保真）才天然可与旧栈逐字节对齐。旧栈两个写回层的
CP 编排不同，故按 pass 旧实现自己的 writer 分道：

- **tree 道**（corpus/tree/）：jbytecode tree roundtrip，服务旧实现经旧
  ClassWriter 写回的 pass（sorter、tightener）。tree 模型不持有未知属性，tree
  道语料天然无未知属性——这是旧栈行为的一部分，不是缺陷。
- **jcf 道**（corpus/jcf/）：jclassfile roundtrip，服务旧实现经低层 writer 写回
  的 pass（cleaner）。DefaultAttribute 保真未知属性——Raw 兜底对拍在此道验证。
- **um 道**（corpus/um/）：未使用成员清理家族专用 mini 语料（5 类 + 全语料一
  容器），容器随附 services 文件与 Manifest 两件非类 keep 根（不落盘，compare
  侧按同源内容重建）——家族的根策略（services 文件名/内容、Manifest 入口键）
  与删除集语义需要这层容器形状才能钉死。
- **cc 道**（corpus/cc/）：容器级合并 pass 专用 mini 语料（9 类 + 全语料一
  容器）：候选矩阵（字段+方法 / 分支体 / 撞名分桶 / 宽常量）、拒绝矩阵
  （`$` 名 / 接口 / 实例字段 / 非空 clinit）、跨类消费面（引用供体成员的
  普通类）。合并 pass 的 golden 宿主不占语料条目——宿主字节经旧 writer 与
  新 writer 各自重编码，对拍按成员集合配对后走骨架矩阵 + 逐方法指令指纹，
  声明保留供体的成员在配对面扣除（构造器/clinit 两栈都不搬，不在扣除面）。

新 pass 入管道时先回答：旧实现经哪个 writer 写回，就用哪条道；根策略/删除集
属语义面的容器级 pass 再补专用 mini 道。

## 语料构成（各道规范形）

- 手造最小类（提取器内自包含裸字节组装）：sorter 已序 + tightener 收紧矩阵
  一体类、owner/caller 收紧矩阵、未知属性类（Raw 兜底）、垃圾异常表类、非法
  描述符成员类；
- A 批手造方法体类（提取器内裸字节组装，tree 道）：NOP 序列/分支目标
  （hb_nop_seq）、常量加载-弹出对（hb_pop_pairs，int/string/long/嵌套/LDC2_W
  非 cat2 形态）、char[] 拼串构造（hb_char_arr，ASCII/增补平面字符/计数
  不符/元素干扰形态）、冗余 requireNonNull 三连（hb_unused_check，实例/
  static 豁免/异 owner 形态）；
- cc 道手造类（提取器内裸字节组装，tree 道）：合并候选矩阵与拒绝矩阵
  九件（hb_cc_*，见上）；
- 仓内既有测试类：asm-test origin 预编译样本（注解 ×2、缺省包类、jdk8 泛型
  结构类、jdk5 枚举类）+ 旧栈 test_resource 大样本。

语料规模纪律：覆盖 pass 声明的语义矩阵（触发/不触发各占相当比例）+ 元数据
怪癖面（注解/泛型签名/未知属性），不追求量。

## 差异声明流程（manifest，每 pass 一份）

1. 对拍失败 → 归因：pass 语义差异（新实现错，修实现）还是 writer 级/旧栈怪癖
   （语义等价或新栈更完整，走声明）。
2. 声明落在 `mjbase/test_resource/pass_golden/manifests/<Pass名>.manifest.txt`：
   条目 = 语料名 + 类目码，文件头注释写类目定义与理由；对拍测试按类目改跑
   替代断言（如成员序、access 矩阵、保帧方向），未命中条目一律逐字节。
3. 默认零差异：manifest 无条目即全部逐字节；声明过期（实际未再发生差异）由
   测试反向报错，防止清单腐化。

已钉死的类目（试点实证）：

| 类目 | pass | 机理 |
|---|---|---|
| CLASSFILE_REENCODE | sorter；A 批 RLVE/CharArr/UnusedCheck；批 A UnusedFieldRemover；批 B meta 家族（槽摘除/槽内过滤触发的 34 个语料类目） | 旧栈全量重编码 writer：重排时 CP 按新 encounter 序重编号，且改写了 CP 引用指令的 pass 会连锁触发未引用条目剪除；mjbase copy-through 保原池。成员序/指令语义等价，writer 级差异。类目内替代断言 = 指令指纹对拍（逐方法 op 序列 + Int/String 常量与成员引用语义不变），骨架类目（sorter）另做成员序对拍，成员删除类目（UnusedFieldRemover）另做成员 (name,desc,access) 矩阵对拍，批 B meta 家族另做 meta 语义矩阵（三层属性槽面 + access 矩阵 + Code 嵌套属性名序）。注意「触发」不蕴含「字节必异」：纯标志位触发的类目（FinalRemover/SyntheticBridgeRemover 部分语料/IllegalAccessRemover）旧栈重序列化产物与 mjbase 原位改写可逐字节相同——批 B 对拍协议对 golden==output 直接放行，不做「声明则必须字节差异」的反向检查 |
| TREE_SKIP_FRAMES_REENCODE | tightener | 旧栈 SKIP_FRAMES 准入怪癖：被收紧类重写时丢 StackMapTable（V1_8 丢帧即 verifier 拒收产物）；mjbase 保帧，语义更完整，access 矩阵一致 |

cleaner 当前零声明：jcf 道读取期校验族已吸收结构性垃圾（越界/空区间异常表
条目进不了语料），规则 1（非法描述符成员）两栈同规则字节一致，未知属性两侧
保真。旧栈其余清洗规则（LVT/LNT 修剪、BSM 压缩等）未随迁（pass 头注明确不
迁、留待 cf 域专批），规范形使其无触发面。

## 试点对拍结果（基线）

| pass | 语料 | 逐字节一致 | 声明差异 |
|---|---|---|---|
| ClassMemberSorter | 13 | 5 | 8（CLASSFILE_REENCODE，成员序对拍过；含 A 批 4 个新语料的方法体重排连锁） |
| AccessModifierTightener | 13 | 12 | 1（TREE_SKIP_FRAMES_REENCODE，access 对拍过 + 保帧方向钉死） |
| IllegalClassFileCleaner | 9 | 9 | 0 |
| NopRemover | 4 | 4 | 0（NOP 删除不触碰 CP，全语料字节一致） |
| RedundantLoadValEliminator | 4 | 3 | 1（CLASSFILE_REENCODE：LDC 字符串+POP 折叠后旧 writer 剪除失引常量；指纹对拍过） |
| CharArrToStrSimplifier | 4 | 3 | 1（CLASSFILE_REENCODE：char[]→LDC 后原 Utf8/类型条目失引被剪；指纹对拍过） |
| UnusedCheckCleaner | 4 | 3 | 1（CLASSFILE_REENCODE：三连删除后方法引用条目失引被剪；指纹对拍过） |
| EmptyClassRemover | 5（um 道） | 2 | 0（删除集 3 以 golden 缺席钉死；幸存类无改写面，逐字节） |
| UnusedClassRemover | 5（um 道） | 4 | 0（删除集 1 以 golden 缺席钉死；services/Manifest 根保留面同容器钉死） |
| UnusedFieldRemover | 5（um 道） | 4 | 1（CLASSFILE_REENCODE：删字段后旧 writer 全池重排重编号、失引 Utf8 落池尾不剪；成员矩阵 + 指纹对拍过） |
| GlobalUnusedMethodRemover | 5（um 道） | 5 | 0（um 语料上旧 writer 池序未扰动，逐字节成立；其余语料形状若重编码差异再现按声明流程补条目） |
| MetaInfoRemover | 8 | 0 | 8（SourceFile/SourceDebugExtension/注解两族/MethodParameters 槽摘除触发旧 writer 重编码；meta 语义矩阵 + 指纹对拍过） |
| FinalRemover | 8 | 8 | 0（3 语料触发但纯标志位清位，原位改写与旧栈重序列化逐字节一致；manifest 记录触发面） |
| SyntheticBridgeRemover | 8 | 5 | 3（quirks/jdk5/cf1——旧 writer 重编码时剪失引 Utf8；语义矩阵 + 指纹对拍过） |
| KotlinMetadataRemover | 8 | 7 | 1（quirks：类级 visible 槽内条目删除） |
| SourceInfoRemover | 8 | 0 | 8（SourceFile/SourceDebugExtension 槽摘除触发重编码） |
| SignatureRemover | 8 | 5 | 3（quirks/jdk5/jdk8：三层 Signature 槽摘除） |
| IllegalAnnotationRemover | 8 | 7 | 1（quirks：畸形条目槽内过滤） |
| IllegalSignatureRemover | 8 | 7 | 1（quirks：非法签名槽摘除，判定经 libjbc signature 解析器分道） |
| IllegalTypeAnnotationRemover | 8 | 7 | 1（quirks：含 Code 嵌套槽内过滤） |
| IllegalSourceInfoRemover | 8 | 7 | 1（quirks：非法 SourceFile 摘除、合法 SourceDebugExtension 保留） |
| UnknownAttributeCleaner | 8 | 8 | 0（tree 规范形语料无未知属性，全语料零触发字节恒等；触发面由单测钉死） |
| IllegalAccessRemover | 8 | 8 | 0（quirks 触发：标志位与 MethodParameters（旧栈遗留 u1 count 布局）原位改写与 golden 逐字节一致；manifest 记录触发面） |
| SpecificAnnotationRemover | 8 | 7 | 1（quirks：黑名单/畸形条目槽内过滤） |

批 B 对拍语料固定 8 条（与提取侧同源：6 条既有 tree 道语料 + hb_meta_minimal/
hb_meta_quirks 厨房水槽双件），对拍主体见 pass_golden_compare_test.cpp 的
compareMetaFamilyPass。
| ToolClassCombiner | 9（cc 道） | 4 | 声明类目收口：CC_NOTMODELED_KEPT 1（宽常量候选新栈保守保留，供体与语料逐位一致）+ CLASSFILE_REENCODE 1（消费面 owner 重指向，指纹对拍过）；拒绝矩阵 4 以 golden 缺席/在场钉死；宿主 2 件按成员集合配对（扣除声明保留成员）后骨架矩阵 + 逐方法指纹，帧方向钉死（golden 无帧 / 新栈分支体再生 SMT），硬验收面 JVM 真跑（输出等价 + 反射核对字段 owner + 供体零残留） |

tightener 语料数随 A 批新语料同步扩面（新语料无 access 可收紧点，新增 4 条
全部逐字节一致）。

## B 批自基线道（无旧栈对标的 pass）

控制流改动型 pass（首者 JunkCodeInserter）在旧栈无语义对标（或旧实现语义
不可比），「旧栈输出 = golden」的事实源不存在。此类 pass 走自基线道：

- 提取侧同一 env 门控（MJBASE_PASS_GOLDEN_REGEN=1）把 mjbase 管线输出落
  `golden_self/<Pass名>/<语料>.class`（基线，入库）；
- 常规运行逐字节复验基线 = **确定性 + 幂等锁**：同一输入两次运行字节一致、
  与入库基线一致，破坏即触发声明流程；
- 自基线不享有「旧栈事实源」地位，只锁不漂移；待旧栈出现可对标的实现或
  该 pass 语义冻结经主线裁决，再切真 golden 道。

## 加新 pass 的步骤

1. 有旧栈对标：旧栈侧提取器为新 pass 加提取段（读语料 → 旧栈 pass 运行 →
   golden 落盘 `golden/<Pass名>/`）；pass 的旧实现经哪个 writer 写回，语料
   用哪条道。无旧栈对标（B 批形态）：走自基线道（见上节），golden_self
   落盘 + 确定性锁。
2. mjbase 侧：pass 落地 + defaultPassRegistry 种子注册后，对拍测试加一个
   TEST：registry 按名构造 → 同语料运行 → 默认逐字节比较。
3. 跑对拍：全绿则零声明收口；有差异走上面声明流程。
4. 更新本页结果表与 README 布局说明。

## 纪律

- golden 与语料是基线文件入库；`test/` 下不留中间产物；提取器与对拍测试的
  常规运行不写任何仓库文件（提取器 env 门控）。
- golden 再生成只能在冻结旧栈上做（提取有时限，迁移计划 §5 第 3 关）；再生成
  字节幂等，幂等破坏即触发声明流程。
- 旧栈 src 与 libs/ 一行不改：旧栈只消费。
