/// @file        outline_plan.hpp
/// @brief       OutlinePlan：直线区域 outline 处方（asm/sem 设施层第一批编辑面）。
///
/// 设计依据 luiox/mjbase #115 §2.3/§3.2/§3.3（D0 设计稿）A2 里程碑：把
/// RegionView 交付的 SESE 直线候选区域整体搬入新合成的
/// `ACC_PRIVATE|ACC_STATIC|ACC_SYNTHETIC` 方法，原方法调用点以 invokestatic
/// 替换。对齐 MemberRemapPlan 先例：plan 只存数据、可校验、可搬运，不遍历
/// classfile、不识别、不修改——识别与判据检查在 planOutline（消费
/// FrameView/SlotProvenanceView/RegionView 完成），落地在 apply（走 libjbc
/// 会话，错误 Result 值语义）。
///
/// v1 判据口径（#115 §1.1 + §1.2 + A2 收紧，A3 放开项见注）：
///   1. 直线区域（RegionCandidate.straightLine）且末条指令为指向出口锚的
///      goto——出口栈形取该 goto 的到达帧（区域路径独占、未经汇合点合并），
///      落出出口（末指令非 goto 的 fallthrough 出口）v1 不开档。
///   2. 栈切面干净：出口栈形 = 入口栈形（逐槽类型 + provenance 一致的公共
///      前缀）+ 栈顶恰 1 个新值（定义产自区域内）作为返回值。切面槽
///      provenance 唯一（SlotProvenanceView.stackSlotClean，§3.4 DUP 边界
///      半径判据）。Uninitialized/Null 值不跨切面（§3.2；Null 返回改变
///      续延汇合类型，v1 拒绝；Null 入参安全，描述符升格 Object）。
///      A3 放开（§3.4 落临时档的机械化）：出口栈顶 = 区域内末条 DUP/DUP2
///      对入口栈顶原值的复制时，原值升格 Stack 入参 + 调用点 DUP 复位
///      （retIsEntryCopy）——返回值插回原位（原值留栈下、副本作实参、
///      返回值落顶），N 体零改写；dup_x1 族/SWAP 跨切点仍拒（出口形状
///      结构性不可表达，#115 §3.4 实测档）。
///   3. 写回集为空：区域内被写 ∩ 出口后活跃 = 空（#115 §3.2）。A3 随
///      LivenessView（S2）放开 A2 的「区域不写局部槽」保守收紧：被写槽
///      出口后仍被读（liveAfter）才拒绝；「读了但恒被重写」的槽（读的
///      到达定义全在区域内）不进参数集（def-use 恒等搬运剔除）。
///   4. 异常上下文同质且 A2 收紧为 handler 集为空：try 覆盖内的同质区域
///      v1 不开档（调用点须落在 try 区间内，涉及 insertAtLabel 标签重锚
///      与异常表边界语义，后续批处理）。N 无异常表，父表零改动。
///   5. jsr/ret：FrameView.hasSubroutine 拒绝。
///   6. 构造器 UninitThis 存活期（FrameView.hasUninitThis）拒绝。
///
/// apply 序（#115 §3.3）：建 N（体 = 区域指令值拷贝、局部操作数按重编号
/// 表改写、出口 goto 剥离、追加返回指令）→ 调用点插局部入参装载 +
/// invokestatic + 转发 goto（复用被剥离 goto 的目标标签）→ eraseBatch 删
/// 区域 → 父/子两侧 lower + Strict 帧重算 + maxes 采用 + SMT 再生（调用点
/// 成新分支目标，链式 offset 重算不覆盖，须整表再生；LNT/LVT 行按归属
/// 拆分——完全落在区域内的行迁 N 并重编号，越切点行 fail-closed 已在
/// planOutline 拒绝）。
///
/// 红线（#115 §2.1）：plan 值语义、自持有（文本经自有 Utf8StringArena
/// 驻留）；InsnId/LabelId 句柄为引用口径——构建与 apply 之间会话不得发生
/// 结构变更（同 RegionView 快照纪律），apply 前做句柄探测校验。
#pragma once

#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"
#include "libca/str/utf8_string_arena.hpp"

#include "jbc/analysis/domain.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/sem/region_view.hpp"

namespace mjbase::asm_sem {

/// @brief outline 失败语义：识别侧判据拒绝与 apply 侧落地失败同表分域。
enum class OutlineError : ca::u8 {
    Ok = 0,

    // ---- 识别侧（planOutline 判据拒绝，会话零改动）----

    BadRegionHandles,        ///< 区域/父方法句柄无效或不属于本方法体
    ParentNoCode,            ///< 父方法无方法体（abstract/native）
    InterfaceOwner,          ///< 接口方法 v1 不做（InterfaceMethodref 面）
    RegionEmpty,             ///< 区域成员为空
    NotStraightLine,         ///< 判据 1：直线旗标为假
    TryCovered,              ///< 判据 4 A2 收紧：handler 集非空（try 覆盖）
    Subroutine,              ///< 判据 5：方法体含 jsr/ret
    UninitThisLive,          ///< 判据 6：UninitThis 在区域内存活
    LastInsnNotGoto,         ///< v1：末条指令须为 goto（出口栈形取其到达帧）
    EntryLabelAmbiguous,     ///< 区域头锚定多于一个标签（fail-closed）
    FrameFailed,             ///< Strict 帧不可算（携带 summary 的判据前置）
    ProvenanceTruncated,     ///< def-use 到达集溢出：切面结论不可信（保守拒）
    ExitShapeImpure,         ///< 判据 2：出口 ≠ 公共前缀 + 恰 1 个新栈顶值
    NoNewTopValue,           ///< 出口无新增栈顶值（M == k）：v1 不开档
    BoundarySlotDirty,       ///< 切面槽 provenance 不唯一（§3.4 DUP 半径）
    RetNotProduced,          ///< 返回值非区域内产出（边界 DUP 复制值形态）
    UninitCrossing,          ///< Uninitialized 值跨切面（栈切片/局部入参）
    RetNullUnsupported,      ///< 返回值类型 Null（续延汇合类型漂移，v1 拒）
    RetTypeUnknown,          ///< 返回值类型 Top/Bottom（帧不自洽，fail-closed）
    LocalTypeMissing,        ///< 区域读到的局部槽在入口帧不可辨型（fail-closed）
    RegionWritesLocal,       ///< 判据 3 A2 收紧：区域内写局部槽（含 iinc）
    InternalBranch,          ///< 区域内出现非链 glue 的分支/终止（fail-closed）
    LvtRowCrossesCut,        ///< LVT/LVTT 行越切点或槽不可映射（fail-closed；
                             ///  宽松开关下改记裁剪不拒，见 OutlineRequest）
    UnsupportedAttr,         ///< 父方法携带代码锚定属性（TypeAnnotations 族）
    MalformedPlan,           ///< 处方内部不一致（validate 数据侧拒绝）

    // ---- 落地侧（apply；失败可能留下半截变换，见类注）----

    EditFailed,              ///< CodeDom 编辑失败（插入/删除）
    LowerFailed,             ///< lowerMethod 失败
    MaxesFailed,             ///< 变换后 maxes 采用失败（超 u16 域）
    SmtFailed,               ///< SMT 再生失败
};

const char* to_string(OutlineError e) noexcept;

/// @brief copy-set 一行：切面值 → 合成方法参数槽（#115 §3.2）。
struct SlotCopy
{
    enum class Kind : ca::u8 {
        Stack,   ///< 入口栈切片槽（fromTop 按值计，0 = 栈顶；调用点免装载）
        Local,   ///< 区域读到的局部槽（调用点按 descriptor 装载）
    };

    Kind                kind = Kind::Stack;
    ca::u32             fromTop = 0;    ///< Kind::Stack：入口栈值序（0 = 栈顶）
    ca::u32             oldLocal = 0;   ///< Kind::Local：原局部槽号
    ca::u32             newSlot = 0;    ///< 合成方法内槽号（cat2 占 2 槽）
    ca::str::Utf8StringRef desc;        ///< 参数描述符（计划池驻留）
};

/// @brief 区域内局部读的重编号行（合成方法槽位从 0 重排：参数序 + 局部）。
struct LocalRemap
{
    ca::u32 oldSlot = 0;
    ca::u32 newSlot = 0;
};

/// @brief 完全落在区域内的 LVT/LVTT 行迁移安排（迁 N、槽重编号、
///        startPc 重锚定到 N 体指令）。
struct LvtRowMove
{
    ca::u32 startPc = 0;   ///< 原方法布局 pc（apply 期父表行删除匹配键）
    ca::u32 length  = 0;
    ca::u32 nameCp  = 0;   ///< 原 CP Utf8 索引原样复用（同 session 共享池）
    ca::u32 descCp  = 0;
    bool typeTable = false; ///< 源表：false = LVT，true = LVTT（迁 N 同名表）
    ca::u32 oldSlot = 0;
    ca::u32 newSlot = 0;
    ca::u32 bodyIndex = 0; ///< N 体指令序（dom.order 下标）——startPc 重锚定点
};

/// @brief 锚点落在区域内的 LNT 行迁移安排（迁 N，行号原样）。
struct LntRowMove
{
    ca::u32 startPc = 0;   ///< 原方法布局 pc（apply 期父表行删除匹配键）
    ca::u32 line    = 0;
    ca::u32 bodyIndex = 0; ///< N 体指令序（startPc 重锚定点）
};

/// @brief 越切点 LVT/LVTT 行的裁剪记录（宽松开关档：父表删除、不迁 N）。
struct LvtRowDrop
{
    ca::u32 startPc = 0;
    ca::u32 length  = 0;
    ca::u32 nameCp  = 0;
    ca::u32 descCp  = 0;
    bool typeTable = false;
    ca::u32 slot   = 0;
};

/// @brief outline 落地产物。
struct OutlineOutcome
{
    jbc::ir::MethodId outlined{};   ///< 合成方法身份
    jbc::ir::InsnId   callInsn{};   ///< 父方法调用点（invokestatic）
};

/// @brief 直线区域 outline 处方（纯数据 + 校验/落地分离）。
///
/// 文本（新方法名/描述符/参数描述符）驻留计划自有池，视图随计划存活。
/// apply 之前会话不得发生结构变更（句柄引用口径）；apply 前做句柄探测
/// 校验，失效即 Err(BadRegionHandles)。apply 中途失败不回滚（#115 §2.3
/// applier 错误 Result 值语义；识别侧判据已前置，落地失败属编码域异常，
/// 如分支偏移溢出）。
class OutlinePlan
{
public:
    /// @brief 计划内部一致性校验（数据侧）：描述符齐备、槽位排布自洽、
    ///        体局部操作数全部有重编号映射。判据级语义检查在 planOutline
    ///        识别侧完成，本函数不触碰会话。
    ca::core::Result<void, OutlineError> validate() const;

    /// @brief 落地：建合成方法 + 调用点替换 + 两侧 lower/SMT 再生。
    ///
    /// @param resolver Strict 引用合并策略（变换后两侧帧重算/SMT 再生用）。
    /// 会话版本 < 50（无 SMT 定义）跳过再生；≥ 50 时父方法 SMT 整表再生
    /// （调用点成为新分支目标，链式重算不覆盖）。LNT/LVT 迁移行挂入 N 的
    /// Code 嵌套属性（CP 索引同 session 直接复用）；宽松开关裁剪的越切点
    /// 行（lvtDrops）自父表删除。调用点发射口径：goto 入边形态下参数装载
    /// 块随入边走（标签锚定首条装载，A2 缺陷修复）；retIsEntryCopy 时先
    /// DUP 复制入口栈顶（副本作实参、原值留栈下、返回值落顶插回原位）。
    ca::core::Result<OutlineOutcome, OutlineError>
    apply(jbc::ir::ClassSession& session, jbc::analysis::ITypeResolver& resolver) const;

    // ---- 处方数据（planOutline 填装）----

    jbc::ir::MethodId parent{};             ///< 父方法身份
    ca::str::Utf8StringRef newName;         ///< 合成方法名（outline$<名>$<序>）
    ca::str::Utf8StringRef newDesc;         ///< 合成描述符
    ca::u32 newAccess = 0x0002u | 0x0008u | 0x1000u;  ///< private|static|synthetic

    std::vector<SlotCopy> params;           ///< copy-set：栈切片 + 局部入参
    bool      hasRet = false;               ///< v1 恒 true（恰 1 个新栈顶值）
    SlotCopy  ret{};                        ///< 返回值（出口栈顶 1 槽）
    /// §3.4 落临时档（A3）：返回值 = 区域内末条 DUP/DUP2 对入口栈顶原值的
    /// 复制。原值升格 Stack 入参（公共前缀收缩 1 值），调用点先 DUP 复制
    /// 入口栈顶（副本作实参、原值留栈下），返回值落顶插回原位；N 体零改写。
    bool      retIsEntryCopy = false;

    std::vector<LocalRemap>   locals;       ///< 区域局部读/写重编号表（含非入参的
                                            ///  区域内自足槽：先写后读、恒被重写）
    std::vector<LvtRowMove>   lvtMoves;     ///< LVT/LVTT 行迁移（含 LVTT 同构行）
    std::vector<LntRowMove>   lntMoves;     ///< LNT 行迁移
    std::vector<LvtRowDrop>   lvtDrops;     ///< 宽松开关下裁剪的越切点行（父表删）

    /// 区域指令体值快照（程序序；出口 goto 与链 glue 已剥离；局部操作数
    /// 已按 locals 重映射）。apply 逐条 append 后追加返回指令。
    std::vector<jbc::ir::Insn> body;

    // ---- 边界锚（InsnId/LabelId 引用口径，见类注）----

    jbc::ir::InsnId  entryInsn{};           ///< 区域头指令（调用点替换位）
    jbc::ir::InsnId  exitGoto{};            ///< 区域末条 goto（apply 剥离）
    jbc::ir::LabelId exitLabel{};           ///< exitGoto 目标标签（转发 goto 复用）
    jbc::ir::LabelId entryLabel{};          ///< 区域头入边标签（无则无效句柄）
    std::vector<jbc::ir::InsnId> members;   ///< 区域成员（程序序，含边界）

    /// 计划自有驻留池（文本 intern 落点）。
    ca::str::Utf8StringArena arena;
};

/// @brief outline 识别请求：消费面 = RegionView 候选 + FrameView/
///        SlotProvenanceView（#115 A2 消费面口径）。
struct OutlineRequest
{
    jbc::ir::ClassSession&            session;    ///< 父类会话（只读消费）
    jbc::ir::MethodId                 parent;     ///< 父方法（须可升格）
    jbc::analysis::MethodSig          sig;        ///< 父方法签名（帧播种）
    jbc::analysis::ITypeResolver&     resolver;   ///< Strict 引用合并策略
    const RegionView&                 view;       ///< 候选来源视图（块→指令解析）
    const RegionCandidate&            region;     ///< RegionView 候选区域
    ca::u32                           sequence;   ///< 命名序号（确定性方案）
    /// LVT/LVTT 越切点行处置开关（#115 §6 决策点 3：validate 默认拒绝，
    /// applier 显式宽松）。true 时越切点行裁剪（父表删除、记 lvtDrops），
    /// 不再 Err(LvtRowCrossesCut)。LNT 行锚点恒单点，不受本开关影响。
    bool                              allowLvtCrossCut = false;
};

/// @brief 识别：对候选区域跑 v1 全判据，通过则交付处方（会话零改动）。
///
/// 判据清单见文件头注。识别失败返回 Err(OutlineError)，父方法与区域
/// 保持原状。
ca::core::Result<OutlinePlan, OutlineError> planOutline(const OutlineRequest& req);

}   // namespace mjbase::asm_sem
