/// @file        indy.hpp
/// @brief       invokedynamic / MethodHandle / VarHandle 载体解析（BSM 分族）。
///
/// 输入面与 call_graph 同源（类字节或 ClassSession），输出是指令级语义事实，
/// 不是调用边：
///
/// - invokedynamic：CP InvokeDynamic.a 指到类级 BootstrapMethods 条目，按
///   BSM 方法句柄的 owner/name 分族。已知族按 JVMS/JDK 契约展开静态参数：
///   LambdaMetafactory 取 implMethod（reference_kind + 成员）、SAM 类型、
///   实例化类型、函数接口与捕获参数；ObjectMethods 取全部 MethodHandle
///   参数（record getter）；StringConcatFactory / SwitchBootstraps 只有家族
///   事实、没有成员目标；ConstantBootstraps.invoke 取 MethodHandle 参数。
/// - VarHandle：签名多态访问（invokevirtual java/lang/invoke/VarHandle.<mode>）
///   按访问模式表展开为读/写事实。
///
/// 未知 BSM 家族只记 Unknown，不按名字猜目标（fail-closed）。展开目标只来自
/// BSM 静态参数里的 MethodHandle 常量，类/字符串常量不反推成员。
///
/// 文本寿命：resolveIndyInsn 与 classifyVarHandleAccess 的结果借用传入会话
/// （CP 视图）；IndyIndex 在构建时把全部文本重驻留进自有 KeyPool，索引存活期
/// 内稳定，会话可随后释放。
#pragma once

#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/result.hpp"

#include "jbc/core/bytes.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/keys.hpp"

namespace mjbase::asm_classpath {

/// @brief BSM 家族：解析策略的分派键。
///
/// 家族只由 BSM 方法句柄的 owner/name 决定；同名成员出现在其他 owner 下
/// （或已知 owner 下的未知成员）一律 Unknown。
enum class BootstrapFamily : ca::u8 {
    Unknown = 0,
    LambdaMetafactory,    ///< java/lang/invoke/LambdaMetafactory.metafactory|altMetafactory
    StringConcatFactory,  ///< java/lang/invoke/StringConcatFactory.makeConcat*
    ObjectMethods,        ///< java/lang/runtime/ObjectMethods.bootstrap（record 三方法）
    ConstantBootstraps,   ///< java/lang/invoke/ConstantBootstraps 的已知 BSM 成员
    SwitchBootstraps,     ///< java/lang/runtime/SwitchBootstraps.typeSwitch|enumSwitch
};

/// @brief 家族名（诊断/测试用）。
const char* bootstrapFamilyName(BootstrapFamily family) noexcept;

/// @brief MethodHandle reference_kind 的分派类别（JVMS 4.4.8）。
enum class HandleDispatch : ca::u8 {
    Field = 0,   ///< 1..4：getField/getStatic/putField/putStatic，无调用语义
    Virtual,     ///< 5
    Static,      ///< 6
    Special,     ///< 7
    Constructor, ///< 8：<init>
    Interface,   ///< 9
    Unknown,
};

/// @brief reference_kind → 分派类别；域外值返回 Unknown。
HandleDispatch dispatchOfHandle(ca::u16 referenceKind) noexcept;

/// @brief 一个 MethodHandle 载体：kind + 目标成员。
struct HandleTarget
{
    ca::u16          kind = 0;   ///< reference_kind（1..9）
    std::string_view owner;
    std::string_view name;
    std::string_view desc;
    bool             isInterface = false;   ///< 引用条目为 InterfaceMethodref

    HandleDispatch dispatch() const noexcept { return dispatchOfHandle(kind); }
    bool isFieldKind() const noexcept { return kind >= 1 && kind <= 4; }
    bool isMethodKind() const noexcept { return kind >= 5 && kind <= 9; }
};

/// @brief 调用点坐标（与 ReferenceSite 同口径：CodeDom order() 程序序下标）。
struct IndySite
{
    std::string_view sourceClass;
    std::string_view sourceMethod;
    std::string_view sourceDesc;
    ca::i32          insnIndex = -1;
};

/// @brief 一条 invokedynamic 的解析结果。
///
/// family == Unknown 时后续字段可能仍有 name/desc（CP 可读），但 targets 恒空。
struct IndyResolution
{
    IndySite        site;
    BootstrapFamily family = BootstrapFamily::Unknown;
    std::string_view name;   ///< 调用点名（lambda 为 SAM 方法名）
    std::string_view desc;   ///< invokedType：捕获参数 → 目标类型
    /// lambda：invokedType 返回类型内部名；非 lambda 或返回非类类型时为空。
    std::string_view functionalInterface;
    std::string_view samMethodType;           ///< lambda 静态参数 args[0]
    std::string_view instantiatedMethodType;  ///< lambda 静态参数 args[2]
    /// altMetafactory FLAG_BRIDGES 的桥方法类型（按静态参数顺序）。
    std::vector<std::string_view> bridgeMethodTypes;
    HandleTarget                  bootstrap;   ///< BSM 自身句柄
    /// 展开目标：lambda implMethod / record getter / ConstantBootstraps.invoke。
    std::vector<HandleTarget> targets;
    /// invokedType 参数段（捕获参数形态，按序）。
    std::vector<std::string_view> capturedTypes;

    bool known() const noexcept { return family != BootstrapFamily::Unknown; }
    bool resolved() const noexcept { return !targets.empty(); }
};

/// @brief VarHandle 签名多态访问事实。
struct VarHandleAccess
{
    IndySite        site;
    std::string_view mode;   ///< 访问模式名（get/set/compareAndSet…）
    bool             read  = false;
    bool             write = false;
};

/// @brief 解析一条 invokedynamic 指令。
///
/// @param session 提供类级 BootstrapMethods 与 CP 文本视图。
/// @param insn    待解析指令。
/// @param out     解析结果；site 字段由调用方填。
/// @return 指令不是 INVOKEDYNAMIC 时返回 false（out 清空）；是 indy 时恒返回
///         true——CP/BSM 结构畸形按 family = Unknown 交付，不抛错。
bool resolveIndyInsn(jbc::ir::ClassSession& session, const jbc::ir::Insn& insn,
                     IndyResolution& out);

/// @brief 识别 VarHandle 签名多态访问。
///
/// 命中条件：invokevirtual 且 CP 目标为 java/lang/invoke/VarHandle 的已知
/// 访问模式成员。返回 false 时 out 清空（非 access mode 的 VarHandle 普通
/// 方法如 accessModeType 不记访问事实）。
bool classifyVarHandleAccess(const jbc::ir::ClassSession& session,
                             const jbc::ir::Insn& insn, VarHandleAccess& out);

/// @brief build 失败记录：哪个方法升格失败。文本借用 builder 的会话。
struct IndyBuildError
{
    jbc::ir::PromoteError error = jbc::ir::PromoteError::Ok;
    std::string_view      className;
    std::string_view      methodName;
    std::string_view      methodDesc;
};

class IndyIndexBuilder;

/// @brief 整程序 indy / VarHandle 事实索引。
///
/// 值语义产物，move-only。全部文本在构建时重驻留进自有 KeyPool：查询结果
/// 不依赖输入会话寿命。调用点定位用 IndySite 坐标而非 InsnId，因此索引可
/// 脱离会话独立持有。
class IndyIndex
{
public:
    IndyIndex() = default;

    IndyIndex(const IndyIndex&)            = delete;
    IndyIndex& operator=(const IndyIndex&) = delete;
    IndyIndex(IndyIndex&&)                 = default;
    IndyIndex& operator=(IndyIndex&&)      = default;

    const KeyPool& pool() const noexcept { return pool_; }

    /// @brief 全部 indy 解析（按扫描序）。
    const std::vector<IndyResolution>& resolutions() const noexcept { return resolutions_; }
    /// @brief 全部 VarHandle 访问事实（按扫描序）。
    const std::vector<VarHandleAccess>& varHandleAccesses() const noexcept
    {
        return varHandleAccesses_;
    }

    std::vector<const IndyResolution*> resolutionsOf(MemberKey method) const;
    std::vector<const IndyResolution*> resolutionsOf(std::string_view owner,
                                                     std::string_view name,
                                                     std::string_view desc) const;
    std::vector<const VarHandleAccess*> varHandleAccessesOf(MemberKey method) const;
    std::vector<const VarHandleAccess*> varHandleAccessesOf(std::string_view owner,
                                                            std::string_view name,
                                                            std::string_view desc) const;

    /// @brief 存在未能识别的 BSM 家族（相关 indy 只有保守边）。
    bool hasUnknownFamilies() const noexcept { return unknownFamilies_; }

private:
    friend class IndyIndexBuilder;

    void append(IndyResolution resolution);
    void append(VarHandleAccess access);
    std::string_view keep(std::string_view s);
    HandleTarget     keep(const HandleTarget& target);

    mutable KeyPool pool_;
    std::vector<IndyResolution> resolutions_;
    std::vector<VarHandleAccess> varHandleAccesses_;
    std::unordered_map<MemberKey, std::vector<ca::u32>, MemberKeyHash> resolutionsByMethod_;
    std::unordered_map<MemberKey, std::vector<ca::u32>, MemberKeyHash>
        varHandleAccessesByMethod_;
    bool unknownFamilies_ = false;
};

/// @brief 索引构建器：喂类字节或已开会话 → IndyIndex。
///
/// 逐方法 collectCodeUnits 升格后遍历 CodeDom；升格失败 fail-fast。
/// build 可重复调用（每次全量重建，喂入集合不变）。
class IndyIndexBuilder
{
public:
    /// @brief 喂入一份 class 字节（会话复制源字节，调用后字节可释放）。
    ca::core::Result<void, jbc::ir::SessionError> addClassBytes(jbc::core::ByteView bytes);

    /// @brief 喂入已开的会话（builder 持保活副本；重复喂入同一会话幂等）。
    void addSession(std::shared_ptr<jbc::ir::ClassSession> session);

    ca::core::Result<IndyIndex, IndyBuildError> build();

private:
    std::vector<std::shared_ptr<jbc::ir::ClassSession>> sessions_;
};

}   // namespace mjbase::asm_classpath
