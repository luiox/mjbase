#include "mjbase/spi/pass_name_registry.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "mjbase/pass/access_modifier_tightener.hpp"
#include "mjbase/pass/char_arr_to_str_simplifier.hpp"
#include "mjbase/pass/class_member_sorter.hpp"
#include "mjbase/pass/class_renamer.hpp"
#include "mjbase/pass/constant_propagator.hpp"
#include "mjbase/pass/cons_jump_optimizer.hpp"
#include "mjbase/pass/constant_switch_folder.hpp"
#include "mjbase/pass/const_char_array_string_folder.hpp"
#include "mjbase/pass/enum_cast_check_remover.hpp"
#include "mjbase/pass/constant_folder.hpp"
#include "mjbase/pass/devirtualize_single_target.hpp"
#include "mjbase/pass/exception_trampoline_inliner.hpp"
#include "mjbase/pass/expression_simplifier.hpp"
#include "mjbase/pass/fix_pattern_simplifier.hpp"
#include "mjbase/pass/flattening_reverser.hpp"
#include "mjbase/pass/goto_rearranger.hpp"
#include "mjbase/pass/identity_wrapper_inliner.hpp"
#include "mjbase/pass/illegal_class_file_cleaner.hpp"
#include "mjbase/pass/intrinsic_folding.hpp"
#include "mjbase/pass/local_constant_propagation.hpp"
#include "mjbase/pass/junk_code_inserter.hpp"
#include "mjbase/pass/meta_info_remover.hpp"
#include "mjbase/pass/nop_remover.hpp"
#include "mjbase/pass/pop_unused_local_variables.hpp"
#include "mjbase/pass/redundant_jump_remover.hpp"
#include "mjbase/pass/redundant_load_val_eliminator.hpp"
#include "mjbase/pass/remove_method_calls.hpp"
#include "mjbase/pass/stack_operation.hpp"
#include "mjbase/pass/try_catch_cleanup.hpp"
#include "mjbase/pass/strength_reducer.hpp"
#include "mjbase/pass/tool_class_combiner.hpp"
#include "mjbase/pass/type_cast_simplifier.hpp"
#include "mjbase/pass/unreachable_insn_remover.hpp"
#include "mjbase/pass/useless_pop_cleaner.hpp"
#include "mjbase/pass/dead_local_store_remover.hpp"
#include "mjbase/pass/dead_side_effect_free_call_remover.hpp"
#include "mjbase/pass/string_concat_indirector.hpp"
#include "mjbase/pass/uncontended_monitor_eliminator.hpp"
#include "mjbase/pass/redundant_store_eliminater.hpp"
#include "mjbase/pass/store_load_relay_eliminator.hpp"
#include "mjbase/pass/ref_copy_propagator.hpp"
#include "mjbase/pass/global_str_pool_optimizer.hpp"
#include "mjbase/pass/insn_rearranger.hpp"
#include "mjbase/pass/stack_trace_recover.hpp"
#include "mjbase/pass/write_only_array_cleaner.hpp"
#include "mjbase/pass/redundant_invoke_remover.hpp"
#include "mjbase/pass/argument_array_scalarizer.hpp"
#include "mjbase/pass/static_field_default_inliner.hpp"
#include "mjbase/pass/static_array_field_scalarizer.hpp"
#include "mjbase/pass/unused_check_cleaner.hpp"
#include "mjbase/pass/unused_member_remover.hpp"

namespace mjbase::spi {

using ca::core::Err;
using ca::core::Ok;

const char* to_string(PassScope scope) noexcept
{
    switch (scope) {
    case PassScope::Method: return "Method";
    case PassScope::Class: return "Class";
    case PassScope::Global: return "Global";
    case PassScope::Cf: return "Cf";
    }
    return "?";
}

const char* to_string(RegistryError e) noexcept
{
    switch (e) {
    case RegistryError::Ok: return "Ok";
    case RegistryError::UnknownPass: return "UnknownPass";
    case RegistryError::DuplicateName: return "DuplicateName";
    case RegistryError::ScopeMismatch: return "ScopeMismatch";
    }
    return "?";
}

// ── 注册 ──

Result<void, RegistryError> PassNameRegistry::registerMethodPass(std::string_view name,
                                                                 MethodFactory    factory,
                                                                 std::string_view description)
{
    if (entries_.find(name) != entries_.end()) return Err(RegistryError::DuplicateName);
    Entry& e     = entries_[name];
    e.scope      = PassScope::Method;
    e.description  = description;
    e.method     = std::move(factory);
    e.clazz      = nullptr;
    e.global     = nullptr;
    e.cf         = nullptr;
    return Ok();
}

Result<void, RegistryError> PassNameRegistry::registerClassPass(std::string_view name,
                                                                ClassFactory     factory,
                                                                 std::string_view description)
{
    if (entries_.find(name) != entries_.end()) return Err(RegistryError::DuplicateName);
    Entry& e     = entries_[name];
    e.scope      = PassScope::Class;
    e.description  = description;
    e.method     = nullptr;
    e.clazz      = std::move(factory);
    e.global     = nullptr;
    e.cf         = nullptr;
    return Ok();
}

Result<void, RegistryError> PassNameRegistry::registerGlobalPass(std::string_view name,
                                                                 GlobalFactory    factory,
                                                                 std::string_view description)
{
    if (entries_.find(name) != entries_.end()) return Err(RegistryError::DuplicateName);
    Entry& e     = entries_[name];
    e.scope      = PassScope::Global;
    e.description  = description;
    e.method     = nullptr;
    e.clazz      = nullptr;
    e.global     = std::move(factory);
    e.cf         = nullptr;
    return Ok();
}

Result<void, RegistryError> PassNameRegistry::registerCfClassPass(std::string_view name,
                                                                  CfClassFactory   factory,
                                                                 std::string_view description)
{
    if (entries_.find(name) != entries_.end()) return Err(RegistryError::DuplicateName);
    Entry& e     = entries_[name];
    e.scope      = PassScope::Cf;
    e.description  = description;
    e.method     = nullptr;
    e.clazz      = nullptr;
    e.global     = nullptr;
    e.cf         = std::move(factory);
    return Ok();
}

// ── 查询 ──

const PassNameRegistry::Entry* PassNameRegistry::findEntry(std::string_view name) const
{
    const auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second;
}

bool PassNameRegistry::has(std::string_view name) const { return findEntry(name) != nullptr; }

Result<PassNameEntry, RegistryError> PassNameRegistry::find(std::string_view name) const
{
    const Entry* e = findEntry(name);
    if (e == nullptr) return Err(RegistryError::UnknownPass);
    return Ok(PassNameEntry{name, e->scope, e->description});
}

std::vector<PassNameEntry> PassNameRegistry::list() const
{
    std::vector<PassNameEntry> out;
    out.reserve(entries_.size());
    for (const auto& [name, entry] : entries_) {
        out.push_back(PassNameEntry{name, entry.scope, entry.description});
    }
    // 存储是 unordered_map，list 输出面保持按名稳定序（CLI list 纪律）。
    std::sort(out.begin(), out.end(),
              [](const PassNameEntry& lhs, const PassNameEntry& rhs) { return lhs.name < rhs.name; });
    return out;
}

// ── 构造 ──

Result<std::unique_ptr<MethodPass>, RegistryError>
    PassNameRegistry::createMethodPass(std::string_view name) const
{
    const Entry* e = findEntry(name);
    if (e == nullptr) return Err(RegistryError::UnknownPass);
    if (e->scope != PassScope::Method || !e->method) return Err(RegistryError::ScopeMismatch);
    return Ok(e->method());
}

Result<std::unique_ptr<ClassPass>, RegistryError>
    PassNameRegistry::createClassPass(std::string_view name) const
{
    const Entry* e = findEntry(name);
    if (e == nullptr) return Err(RegistryError::UnknownPass);
    if (e->scope != PassScope::Class || !e->clazz) return Err(RegistryError::ScopeMismatch);
    return Ok(e->clazz());
}

Result<std::unique_ptr<GlobalPass>, RegistryError>
    PassNameRegistry::createGlobalPass(std::string_view name) const
{
    const Entry* e = findEntry(name);
    if (e == nullptr) return Err(RegistryError::UnknownPass);
    if (e->scope != PassScope::Global || !e->global) return Err(RegistryError::ScopeMismatch);
    return Ok(e->global());
}

Result<std::unique_ptr<CfClassPass>, RegistryError>
    PassNameRegistry::createCfClassPass(std::string_view name) const
{
    const Entry* e = findEntry(name);
    if (e == nullptr) return Err(RegistryError::UnknownPass);
    if (e->scope != PassScope::Cf || !e->cf) return Err(RegistryError::ScopeMismatch);
    return Ok(e->cf());
}

// ── 全局默认表 ──
//
// 依赖方向注记：本文件在 spi 域，默认表的种子却是 pass 域的具体 pass——spi 头
// 文件面保持不依赖 pass（pass 依赖 spi 的正向），只在 .cpp 反向包含做种子注册。
// 名字用真实类名，registerBuiltinPasses 的命名纪律。

namespace {

/// 造默认表：新表种子注册不可能重名，Err 分支即注册面契约破坏，fail loud
/// （static 初始化内异常以 terminate 收场，符合不静默吞错纪律）。
PassNameRegistry makeDefaultRegistry()
{
    PassNameRegistry reg;
    const auto       seed = [&reg](Result<void, RegistryError> r) {
        if (r.is_err()) throw std::logic_error("defaultPassRegistry: seed registration failed");
    };
    seed(reg.registerClassPass(
        "ClassMemberSorter", [] { return std::make_unique<mjbase::pass::ClassMemberSorter>(); },
        "Sort class members into a deterministic order."));
    seed(reg.registerGlobalPass(
        "IllegalClassFileCleaner", [] { return std::make_unique<mjbase::pass::IllegalClassFileCleaner>(); },
        "Drop class entries that cannot be parsed as class files."));
    seed(reg.registerGlobalPass(
        "AccessModifierTightener", [] { return std::make_unique<mjbase::pass::AccessModifierTightener>(); },
        "Tighten access modifiers to the minimum visibility that keeps references valid."));
    // A 批（方法体改写族，纯指令变换）：默认链收尾清理件 + 冗余装载消除 +
    // 字符串族化简 + 冗余空检查清除。
    seed(reg.registerMethodPass(
        "NopRemover", [] { return std::make_unique<mjbase::pass::NopRemover>(); },
        "Remove nop instructions from method bodies."));
    seed(reg.registerMethodPass(
        "RedundantLoadValEliminator",
        [] { return std::make_unique<mjbase::pass::RedundantLoadValEliminator>(); },
        "Remove redundant load-then-store pairs on locals."));
    seed(reg.registerMethodPass(
        "CharArrToStrSimplifier", [] { return std::make_unique<mjbase::pass::CharArrToStrSimplifier>(); },
        "Rewrite char-array String construction into string constants where safe."));
    seed(reg.registerMethodPass(
        "UnusedCheckCleaner", [] { return std::make_unique<mjbase::pass::UnusedCheckCleaner>(); },
        "Remove redundant null/bounds checks whose results are unused."));
    // B 批（加控制流，A1 帧链首批消费者）：确定性垃圾分支块插入。
    seed(reg.registerMethodPass(
        "JunkCodeInserter", [] { return std::make_unique<mjbase::pass::JunkCodeInserter>(); },
        "Insert deterministic dead branch blocks (control-flow noise)."));
    // opt pass 波次（常量折叠族）：操作数栈常量链折叠，i/l/f/d 四型算术 +
    // 移位/按位 + 除零/Min/-1 红线保守停止。标签：scope:method /
    // action:fold / stability:opt-in（默认链不收，CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "ConstantFolder", [] { return std::make_unique<mjbase::pass::ConstantFolder>(); },
        "Fold constant operand-stack chains (arithmetic, shifts, bitwise; conservative stop rules)."));
    // opt pass 波次（强度削减族）：常量 2^k 乘/除改写为移位与移位-加法
    // 修正序列（int/long；float/double 形态不同未做）。标签：
    // scope:method / action:simplify / stability:opt-in。
    seed(reg.registerMethodPass(
        "StrengthReducer", [] { return std::make_unique<mjbase::pass::StrengthReducer>(); },
        "Rewrite multiplication/division by powers of two into shifts (int/long)."));
    // opt pass 波次（栈操作家族首件，W0 样板批）：冗余栈操作窗口折叠 25 条
    // 规则迭代到不动点；ldc 载荷不可判别的宽匹配语义与 Err 保守停收敛声明
    // 见 pass 头注。标签：scope:method / action:simplify / stability:default。
    seed(reg.registerMethodPass(
        "StackOperationSimplifier",
        [] { return std::make_unique<mjbase::pass::StackOperationSimplifier>(); },
        "Fold redundant stack-operation windows to a fixpoint (dup/pop family)."));
    // opt pass 波次（C 方法级指令变换组第一件）：常量 char 数组构造的
    // `new String(char[])` 整窗折叠为字符串 ldc。标签：scope:method /
    // action:fold / stability:opt-in（默认链不收，CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "ConstCharArrayStringFolder",
        [] { return std::make_unique<mjbase::pass::ConstCharArrayStringFolder>(); },
        "Fold whole `new String(constant char[])` construction windows into string ldc."));
    // opt pass 扩段：C 组方法级指令变换组第二件——移除枚举 values() 中
    // 可证明冗余的 INVOKEVIRTUAL/CHECKCAST 相邻对（签名：scope:method /
    // action:eliminate / stability:opt-in，默认不进链，CLI -p 显式点名）。
    seed(reg.registerMethodPass(
        "EnumCastCheckRemover",
        [] { return std::make_unique<mjbase::pass::EnumCastCheckRemover>(); },
        "Remove provably redundant enum values() INVOKEVIRTUAL/CHECKCAST adjacent pairs."));
    // opt pass 扩段（C 方法级指令变换组）：固定模式简化——位运算族 + 死代码族 +
    // 整数比较常量偏移折叠 + 同 slot store/load 中转消除。标签：scope:method /
    // action:simplify / stability:opt-in（默认链不收，CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "FixPatternSimplifier",
        [] { return std::make_unique<mjbase::pass::FixPatternSimplifier>(); },
        "Rewrite registered fixed obfuscator bytecode patterns (bitwise idioms, "
        "compare folding, store/load bounce)."));
    // opt pass 扩段（C 方法级指令变换组）：代数表达式 peephole 化简，恒等元 +
    // 交换/结合 + mul-by--1/shift 等旧规则集（丢弃一条代数不成立的 legacy 规则）。
    // 标签：scope:method / action:simplify / stability:opt-in。
    seed(reg.registerMethodPass(
        "ExpressionSimplifier",
        [] { return std::make_unique<mjbase::pass::ExpressionSimplifier>(); },
        "Fold algebraic expression windows (identity, swap/assoc, negation, bitwise idioms)."));
    // opt pass 扩段（C 方法级指令变换组）：原始类型 cast 常量折叠 + 已知 helper
    // （Float/Double 位图、Integer/Long 解析）折叠。标签：scope:method /
    // action:fold / stability:opt-in。
    seed(reg.registerMethodPass(
        "TypeCastSimplifier",
        [] { return std::make_unique<mjbase::pass::TypeCastSimplifier>(); },
        "Fold primitive cast chains and known JDK constant helpers."));
    // opt pass 波次（异常流混淆的跳板消解件）：异常流混淆器的
    // 「aconst_null; athrow → 紧邻 catch-any handler → pop」异常跳板整组消解
    // （删三条指令 + 摘表项，分支重定向 handler 体；帧引擎证
    // 入口栈深为 0）。标签：scope:method / action:rewrite /
    // stability:opt-in（默认链不收，CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "ExceptionTrampolineInliner",
        [] { return std::make_unique<mjbase::pass::ExceptionTrampolineInliner>(); },
        "Dissolve aconst_null/athrow catch-all trampolines into fallthrough."));
    // opt pass 波次（扁平化状态机静态解析件）：控制流反扁平化——常量状态传播
    // + switch 分发静态解析。通用引擎（唯一 switch 审计/状态槽读写审计/
    // 帧深等价门）+ 可注入形态表（变体扩展面，构造期可替换）。标签：
    // scope:method / action:rewrite / stability:opt-in（默认链不收，
    // CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "FlatteningReverser",
        [] { return std::make_unique<mjbase::pass::FlatteningReverser>(); },
        "Statically resolve flattened switch dispatch: constant state propagation, goto retarget."));
    // opt pass 波次（结构级死代码清理首件）：帧可达性证明删除不可达普通
    // 指令（混淆处理尾部死块/死分发块/分支解析残留）。锚异常表标签的
    // 不可达指令保守保留（交异常面清理批次）。标签：scope:method /
    // action:eliminate / stability:opt-in（默认链不收，CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "UnreachableInsnRemover",
        [] { return std::make_unique<mjbase::pass::UnreachableInsnRemover>(); },
        "Remove instructions proven unreachable by frame analysis (conservative on exception edges)."));
    // opt pass 波次（局部存储清理家族首件）：方法内无任何读取的槽位，
    // 其 store 降级 pop/pop2（宽度由 store 族操作码定死，不依赖帧溯源；
    // 不删生产者）。读点审计含 iinc/ret 与两槽装载的高半槽。标签：
    // scope:method / action:eliminate / stability:opt-in（默认链不收，
    // CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "PopUnUsedLocalVariables",
        [] { return std::make_unique<mjbase::pass::PopUnUsedLocalVariables>(); },
        "Downgrade stores to never-read local slots into pop/pop2 (width from store opcode)."));
    // opt pass 波次（局部存储清理家族第二件）：局部装载 + 紧随 POP/POP2
    // 的相邻对消除（死装载丢弃形态——状态槽 store 降级成 pop 后暴露的
    // 装载;pop 残段、装载填充噪声）。配对删除恒栈中性，不依赖局部槽内容。
    // 帧载体形态（非相邻生产者、POP2 双 cat1 半删）挂账帧面批次。标签：
    // scope:method / action:eliminate / stability:opt-in（默认链不收，
    // CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "UselessPopCleaner",
        [] { return std::make_unique<mjbase::pass::UselessPopCleaner>(); },
        "Remove dead pop chains: local-load+pop/pop2 adjacent pairs, and pop-discarded pure-const producers via def-use."));
    // opt pass 波次（帧溯源面主件）：def-use 到达定义判定死槽，store 整删/降级。
    seed(reg.registerMethodPass(
        "DeadLocalStoreRemover",
        [] { return std::make_unique<mjbase::pass::DeadLocalStoreRemover>(); },
        "Remove/downgrade stores to slots whose values never reach a meaningful sink."));
    // opt pass 波次（帧溯源面批次）：白名单无副作用调用的纯链弹弃整删。
    seed(reg.registerMethodPass(
        "DeadSideEffectFreeCallRemover",
        [] { return std::make_unique<mjbase::pass::DeadSideEffectFreeCallRemover>(); },
        "Remove whitelisted side-effect-free calls whose results feed a pop-discarded pure chain."));
    // P1 批：StringBuilder 拼接链 indy 化简（javac 9+ 规范形）。
    seed(reg.registerMethodPass(
        "StringConcatIndirector",
        [] { return std::make_unique<mjbase::pass::StringConcatIndirector>(); },
        "Rewrite straight-line StringBuilder append chains into invokedynamic makeConcatWithConstants."));
    // P1 批：方法内受限监视器对摘除（junk synchronized 无害化）。
    seed(reg.registerMethodPass(
        "UncontendedMonitorEliminater",
        [] { return std::make_unique<mjbase::pass::UncontendedMonitorEliminater>(); },
        "Remove confined monitor enter/exit pairs whose object never escapes the method."));
    // opt pass 波次（帧溯源面批次）：store 定义零使用边的死写降级。
    seed(reg.registerMethodPass(
        "RedundantStoreEliminater",
        [] { return std::make_unique<mjbase::pass::RedundantStoreEliminater>(); },
        "Downgrade stores whose definitions have no use edges to pop/pop2."));
    // opt pass 波次（#18 LDVSE 复刻）：store 定义使用边恰为紧邻 reload 的
    // 中转对整删（def-use 单使用证明；旧栈标签窗近似载体的偏离申报见头注）。
    seed(reg.registerMethodPass(
        "StoreLoadRelayEliminator",
        [] { return std::make_unique<mjbase::pass::StoreLoadRelayEliminator>(); },
        "Erase adjacent store/load relay pairs whose store definition has "
        "exactly one use: the adjacent reload."));
    // opt pass 波次（#19）：单来源不变引用的 aload 原位改写为来源指令。
    seed(reg.registerMethodPass(
        "RefCopyPropagator",
        [] { return std::make_unique<mjbase::pass::RefCopyPropagator>(); },
        "Rewrite aload reads with a single immutable-ref origin to the "
        "originating instruction."));
    // opt pass 波次（#19）：容器内 String ldc 去重迁进合成字符串池，ldc 改索引+取值调用。
    seed(reg.registerGlobalPass(
        "GlobalStrPoolOptimizer",
        [] { return std::make_unique<mjbase::pass::GlobalStrPoolOptimizer>(); },
        "Relocate container String ldc constants into a synthesized pool "
        "class and rewrite loads to indexed access."));
    // opt pass 波次（#20）：连续算术赋值块按槽首现序分组重排。
    seed(reg.registerMethodPass(
        "ArithmeticInsnRearranger",
        [] { return std::make_unique<mjbase::pass::ArithmeticInsnRearranger>(); },
        "Regroup consecutive commutative arithmetic assignment blocks by slot."));
    // opt pass 波次（#20）：交换律链常量后移，常量相邻供折叠命中。
    seed(reg.registerMethodPass(
        "CommutativeOperatorReorder",
        [] { return std::make_unique<mjbase::pass::CommutativeOperatorReorder>(); },
        "Move constants to the tail of commutative arithmetic chains."));
    // 装箱还原收尾（#37）：只写数组分配连存储窗整体删除。
    seed(reg.registerMethodPass(
        "WriteOnlyArrayCleaner",
        [] { return std::make_unique<mjbase::pass::WriteOnlyArrayCleaner>(); },
        "Remove array allocations consumed only by element stores."));
    // opt pass 波次（#20）：调试信息恢复——source 属性 + handler 打印调用 + 行号。
    seed(reg.registerGlobalPass(
        "StackTraceRecover",
        [] { return std::make_unique<mjbase::pass::StackTraceRecover>(); },
        "Restore SourceFile/SourceDebug, insert printStackTrace calls at exception "
        "handlers, and synthesize line number entries."));
    // opt pass 波次（帧溯源面批次收口）：容器内纯方法的弹弃调用点整组摘除。
    seed(reg.registerGlobalPass(
        "RedundantInvokeRemover",
        [] { return std::make_unique<mjbase::pass::RedundantInvokeRemover>(); },
        "Remove discard-called pure methods defined in the container via MethodCallRemover."));
    // opt pass 波次（#37 切片 1）：参数装箱还原——Object[] 退化签名标量化。
    seed(reg.registerGlobalPass(
        "ArgumentArrayScalarizer",
        [] { return std::make_unique<mjbase::pass::ArgumentArrayScalarizer>(); },
        "Scalarize boxed Object[] parameters back to real parameter lists."));
    // opt pass 波次（常量跳转折叠件，ConsJump 三件套首件）：常量装载 +
    // 常量条件跳转的相邻窗口折叠——谓词恒真改 GOTO（复用原目标标签）、
    // 恒假整窗删除（单槽 int 谓词、ACONST_NULL 空谓词、双槽 IF_ICMPxx；
    // 类型失配与 ACMP 不入矩阵）。窗口内部边界锚标签拒收（栈消费保护），
    // 头标签放行（整窗重放净栈效应为零）。标签：scope:method /
    // action:rewrite / stability:opt-in（默认链不收，CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "ConsJumpOptimizer",
        [] { return std::make_unique<mjbase::pass::ConsJumpOptimizer>(); },
        "Fold constant-condition jumps: always-true to goto, always-false deleted."));
    // opt pass 波次（常量跳转折叠件，ConsJump 三件套第二件）：int 常量
    // selector + LOOKUPSWITCH/TABLESWITCH 相邻窗折叠——按 selector 值解析
    // 命中分支（lookup 精确匹配 / table 区间匹配，未命中走 default），常量
    // 头原位改 GOTO 复用命中目标标签，switch 删除。switch 前置边界锚标签
    // 拒收（栈消费保护），头标签放行。标签：scope:method / action:rewrite /
    // stability:opt-in（默认链不收，CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "ConstantSwitchFolder",
        [] { return std::make_unique<mjbase::pass::ConstantSwitchFolder>(); },
        "Fold constant-selector switches into a goto to the resolved case target."));
    // opt pass 波次（常量传播与折叠族，#15）：直线段常量槽位追踪——「常量
    // 装载 + store」后同类装载原位物化为常量生产者，IINC 32 位回绕递推；
    // 引用槽只认 ACONST_NULL；标签锚定/控制流边界整体清空（宁可漏折不可
    // 折错）。槽族严格匹配为声明收紧（旧栈快捷形可产出 Verifier 非法替换）。
    // 标签：scope:method / action:rewrite / stability:opt-in（默认链不收，
    // CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "LocalConstantPropagation",
        [] { return std::make_unique<mjbase::pass::LocalConstantPropagation>(); },
        "Propagate constant stores to later same-kind loads within straight-line segments."));
    // opt pass 波次（常量传播与折叠族，#15）：def-use 到达定义面——装载点
    // 到达定义唯一且其 store 弹值定义为常量生产者时原位物化为常量。跨块
    // 传播（分支/汇合不阻断），路径精确；store 前装载不误折（旧栈整方法
    // 单 store 面的正确性修正）；截断集合整件跳过。标签：scope:method /
    // action:rewrite / stability:opt-in（默认链不收，CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "ConstantPropagator",
        [] { return std::make_unique<mjbase::pass::ConstantPropagator>(); },
        "Propagate constant stores across blocks via def-use reaching definitions."));
    // opt pass 波次（常量传播与折叠族收官件，#15）：intrinsic 常量折叠——
    // 装箱/拆箱对整删、种子 Random 五窗 LCG 折叠（jstd::Random 权威面）、
    // 白名单纯函数（Math.abs/max/min、String.length/isEmpty/substring、
    // Integer/Long 位运算与格式化、parseInt/parseLong）实参全常量时折叠
    // 为常量结果。窗内边界锚标签拒收；非 ASCII 常量串 length/substring
    // 按码元语义修正（旧栈按字节错折）。标签：scope:method /
    // action:rewrite / stability:opt-in（默认链不收，CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "IntrinsicFolding",
        [] { return std::make_unique<mjbase::pass::IntrinsicFolding>(); },
        "Fold intrinsic calls on constant arguments: box/unbox pairs, seeded "
        "Random, pure JDK functions."));
    // opt pass 波次（死代码与控制流清理族，#18）：异常边界清理——透明
    // 重抛条目删除（athrow / 同槽 astore-aload / 恒等静态包装三形状 +
    // 共享/可达/重叠守卫）、恒不抛段收口（域分析可达帧 + 保守可抛集合）、
    // NPE 形与自捕抛改写（栈深门槛 + 锚入标签守卫，旧栈缺失面的补强）。
    // 悬空标签与空区间条目修复性删除；乱序条目保留（合法类文件允许）。
    // 标签：scope:method / action:rewrite / stability:opt-in（默认链不收，
    // CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "TryCatchCleanup",
        [] { return std::make_unique<mjbase::pass::TryCatchCleanup>(); },
        "Clean up redundant try-catch entries: transparent rethrow, "
        "impossible catch, NPE shape, self-caught throw."));
    // opt pass 波次（标签布局族首件）：目标落在顺序后继前置边界的跳转
    // 消解——GOTO 整条删除（fallthrough 等价），条件跳原位换 POP/POP;POP
    // （分支与汇合重合，条件值被丢弃，弹宽对齐谓词元数）。方法末 GOTO
    // 删除无承接者保守跳过，方法末条件跳替换照常。标签：scope:method /
    // action:rewrite / stability:opt-in（默认链不收，CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "RedundantJumpRemover",
        [] { return std::make_unique<mjbase::pass::RedundantJumpRemover>(); },
        "Remove jumps whose target is the immediately following instruction."));
    // opt pass 波次（标签布局族第二件，消费 libjbc 0.4.19 moveRange）：GOTO
    // 目标尾段整体搬移使 GOTO 退化为紧邻冗余跳转再按 fallthrough 等价整删
    // ——无条件跳转次数 -1、方法尾布局改写（尾段提前、中间段沉底）。保守
    // 门：前向非邻接、无 JSR/RET、异常表不触段/不触 GOTO 边界、目标单前驱、
    // 段内无段外回边。标签：scope:method / action:rewrite / stability:opt-in
    // （默认链不收，CLI -p 显式挂点）。
    seed(reg.registerMethodPass(
        "GotoRearranger",
        [] { return std::make_unique<mjbase::pass::GotoRearranger>(); },
        "Move the goto target tail segment so the goto becomes an eliminable fallthrough."));
    // 批 A（容器级直写组首件）：全容器确定性类重命名。
    seed(reg.registerGlobalPass(
        "ClassRenamer", [] { return std::make_unique<mjbase::pass::ClassRenamer>(); },
        "Rename classes across the whole container deterministically."));
    // 批 A（未使用成员清理家族，容器级四件）：空类删除 + 可达闭包类删除 +
    // 可达类内不可达 private 字段/方法删除。
    seed(reg.registerGlobalPass(
        "EmptyClassRemover", [] { return std::make_unique<mjbase::pass::EmptyClassRemover>(); },
        "Remove empty classes from the container."));
    seed(reg.registerGlobalPass(
        "UnusedClassRemover", [] { return std::make_unique<mjbase::pass::UnusedClassRemover>(); },
        "Remove classes unreachable from container entry points."));
    seed(reg.registerGlobalPass(
        "UnusedFieldRemover", [] { return std::make_unique<mjbase::pass::UnusedFieldRemover>(); },
        "Remove private fields that are never read or written."));
    seed(reg.registerGlobalPass(
        "GlobalUnusedMethodRemover",
        [] { return std::make_unique<mjbase::pass::GlobalUnusedMethodRemover>(); },
        "Remove private methods that are never invoked."));
    // opt pass 波次（调用点收尾件，删除族前置）：按名单点名删除成员调用点 +
    // 无用结果白名单面。默认无配置 = 完全 no-op（opt-in）；配置为程序化注入
    // （MethodCallRemovalConfig），CLI/配置解析供给面属 cli 批。标签：
    // scope:global / action:eliminate / stability:opt-in。
    seed(reg.registerGlobalPass(
        "MethodCallRemover", [] { return std::make_unique<mjbase::pass::MethodCallRemover>(); },
        "Remove call sites of spec-named members and discardable-result whitelist calls."));
    // 批 B（类级骨架组，meta 元数据清理家族 13 件）：catch-all debug/framework
    // 元数据清除 + 单面件（final / synthetic+bridge / kotlin metadata /
    // source 信息 / 签名 / 非法注解 / 非法签名 / 非法 type 注解 / 非法 source
    // 信息 / 未知属性 / 非法 access 位 / 黑名单注解）。
    seed(reg.registerClassPass(
        "MetaInfoRemover", [] { return std::make_unique<mjbase::pass::MetaInfoRemover>(); },
        "Remove debug/framework metadata (source, signatures, annotations, unknown attributes)."));
    seed(reg.registerClassPass(
        "FinalRemover", [] { return std::make_unique<mjbase::pass::FinalRemover>(); },
        "Remove final modifiers from classes, fields and methods."));
    seed(reg.registerClassPass(
        "SyntheticBridgeRemover", [] { return std::make_unique<mjbase::pass::SyntheticBridgeRemover>(); },
        "Remove synthetic and bridge markers."));
    seed(reg.registerClassPass(
        "KotlinMetadataRemover", [] { return std::make_unique<mjbase::pass::KotlinMetadataRemover>(); },
        "Remove Kotlin @Metadata annotations."));
    seed(reg.registerClassPass(
        "SourceInfoRemover", [] { return std::make_unique<mjbase::pass::SourceInfoRemover>(); },
        "Remove SourceFile attributes and line number tables."));
    seed(reg.registerClassPass(
        "SignatureRemover", [] { return std::make_unique<mjbase::pass::SignatureRemover>(); },
        "Remove generic Signature attributes."));
    seed(reg.registerClassPass(
        "IllegalAnnotationRemover",
        [] { return std::make_unique<mjbase::pass::IllegalAnnotationRemover>(); },
        "Remove annotations with malformed structure or illegal targets."));
    seed(reg.registerClassPass(
        "IllegalSignatureRemover", [] { return std::make_unique<mjbase::pass::IllegalSignatureRemover>(); },
        "Remove malformed Signature attributes."));
    seed(reg.registerClassPass(
        "IllegalTypeAnnotationRemover",
        [] { return std::make_unique<mjbase::pass::IllegalTypeAnnotationRemover>(); },
        "Remove malformed type annotations."));
    seed(reg.registerClassPass(
        "IllegalSourceInfoRemover",
        [] { return std::make_unique<mjbase::pass::IllegalSourceInfoRemover>(); },
        "Remove malformed SourceFile/LineNumberTable attributes."));
    seed(reg.registerClassPass(
        "UnknownAttributeCleaner", [] { return std::make_unique<mjbase::pass::UnknownAttributeCleaner>(); },
        "Remove unrecognized class-file attributes."));
    seed(reg.registerClassPass(
        "IllegalAccessRemover", [] { return std::make_unique<mjbase::pass::IllegalAccessRemover>(); },
        "Remove illegal access flag combinations."));
    seed(reg.registerClassPass(
        "SpecificAnnotationRemover",
        [] { return std::make_unique<mjbase::pass::SpecificAnnotationRemover>(); },
        "Remove annotations matching the configured blacklist."));
    // opt pass 扩段（类级首件，mbase 治理版移植）：<clinit> 单次写入的常量
    // 内联进字段 ConstantValue + 删除等价初始化指令对；多次 PUTSTATIC 预扫描
    // 计数保守跳过。标签：scope:class / action:propagate / stability:opt-in
    // （默认链不收，CLI -p 显式挂点）。
    seed(reg.registerClassPass(
        "StaticFieldDefaultInliner",
        [] { return std::make_unique<mjbase::pass::StaticFieldDefaultInliner>(); },
        "Inline single constant <clinit> putstatic into the field's ConstantValue attribute."));
    // opt pass 扩段（#39 第一切片）：private static 原始类型数组字段拆成
    // 按访问下标的标量字段——分配与下标计算指令消失，读窗口三条缩一条；
    // 与 StaticFieldDefaultInliner 串联可消化 clinit 常量表。标签：
    // scope:class / action:rewrite / stability:opt-in（默认链不收）。
    seed(reg.registerClassPass(
        "StaticArrayFieldScalarizer",
        [] { return std::make_unique<mjbase::pass::StaticArrayFieldScalarizer>(); },
        "Scalarize private static primitive-array fields with constant-index-only access."));
    // 批 A（容器级直写组收口件）：static 工具类保守合并 + 全量 owner 重映射。
    seed(reg.registerGlobalPass(
        "ToolClassCombiner", [] { return std::make_unique<mjbase::pass::ToolClassCombiner>(); },
        "Merge static utility classes and remap their owners."));
    // opt pass 波次（去虚拟化，P1）：CHA 证明调用点唯一活目标时把
    // invokevirtual/invokeinterface 改写为 invokespecial 直接分派。标签：
    // scope:global / action:rewrite / stability:opt-in（默认链不收，CLI -p
    // 显式点名；JVMS 合法性依据与放弃清单见 pass 头注）。
    seed(reg.registerGlobalPass(
        "DevirtualizeSingleTarget",
        [] { return std::make_unique<mjbase::pass::DevirtualizeSingleTarget>(); },
        "Rewrite virtual/interface call sites with one provable target into invokespecial."));
    // opt pass 波次（恒等包装不动点内联）：invokestatic 壳调用点
    // 直达最终目标（路径压缩），壳体保留、删除延后 GlobalUnusedMethodRemover。
    // 标签：scope:global / action:rewrite / stability:opt-in（默认链不收）。
    seed(reg.registerGlobalPass(
        "IdentityWrapperInliner",
        [] { return std::make_unique<mjbase::pass::IdentityWrapperInliner>(); },
        "Rewrite invokestatic call sites of pure forwarder shells to their final targets."));
    return reg;
}

}   // namespace

PassNameRegistry& defaultPassRegistry()
{
    static PassNameRegistry registry = makeDefaultRegistry();
    return registry;
}

}   // namespace mjbase::spi
