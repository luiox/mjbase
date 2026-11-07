/// @file        indy_resolver.hpp
/// @brief       invokedynamic 还原 SPI：resolver 契约（setup/resolve 两段）+
///              按优先级排序的 resolver 管理器。
///
/// 机制借鉴 Recaf 的 InvokeDynamicResolver / InvokeDynamicResolverManager
/// （只读参考），接口形态按 mjbase 惯例重塑：错误走 `Result` 值语义不抛异常，
/// 文本走借用视图（`std::string_view`，寿命 = resolver 对象），不新增
/// `std::string` 存储——resolver 动态物化的文本自行驻留进自有存储
/// （`KeyPool` / `Utf8StringArena` 家族）后交付视图。
///
/// 职责分工（双层）：通用层（本文件 + `IndySiteInliner` pass）只装机制——
/// 站点事实采集、resolver 迭代次序、目标判据复核与字节改写；具体 resolver
/// 由插件/宿主层供给（混淆器家族知识不进通用层，grunt 的键链解密/heavy
/// 互证是未来插件 resolver 的判据形态参考，只读参考不迁移）。
///
/// 契约要点：
/// - `resolve` 无目标 = `Err(NoTarget)`（正常控制流）：站点字节原样保留；
/// - `LambdaMetafactory` / `StringConcatFactory` 家族的站点由消费方在采集
///   阶段直接排除（生成面语义，不进 resolver 面）；
/// - 本文件只声明契约，不解释字节码——站点事实由消费方（pass）按
///   `mjbase/asm/classpath/indy.hpp` 的解析面采集后喂入。
///
/// @warning 非线程安全。注册与查询应在同一线程的初始化/单次运行内完成。
#pragma once

#include <algorithm>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "mjbase/model/resource_container.hpp"

namespace mjbase::spi {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

/// resolver 面错误码：`NoTarget` 是正常控制流，其余才是失败。
enum class IndyResolverError : ca::u8
{
    NoTarget = 1,   ///< 本站不归任何已注册 resolver（站点字节原样保留）
    SetupFailed,    ///< setup 失败：该 resolver 本轮整体排除
    ResolveFailed,  ///< resolve 内部失败：该站点保守保留，继续试下一 resolver
    NullResolver,   ///< 注册了空指针
    DuplicateName,  ///< 注册名字已被占用
};

const char* to_string(IndyResolverError e) noexcept;

/// BSM 方法句柄的只读视图（JVMS 4.4.8 reference_kind + 成员引用三段）。
///
/// 文本借用站点采集方的会话 CP：视图只在单次 `resolve` 调用内有效。
struct IndyHandleView
{
    ca::u16          kind = 0;   ///< reference_kind（1..9；0 = 结构畸形占位）
    std::string_view owner;
    std::string_view name;
    std::string_view desc;
    bool             isInterface = false;   ///< 引用条目为 InterfaceMethodref
};

/// BSM 静态参数的只读值形态（JVMS 4.7.23 bootstrap_arguments 按序交付）。
///
/// 结构畸形（CP 条目缺失/tag 越界）交付 `Kind::Unreadable` 占位：位置对应
/// 关系不因读失败而错位，resolver 见到占位应自行放弃（fail-closed）。
struct IndyBsmArg
{
    enum class Kind : ca::u8
    {
        Int,          ///< CONSTANT_Integer（num 有效）
        Long,         ///< CONSTANT_Long（num 有效）
        Float,        ///< CONSTANT_Float（num = 原始位型，i32 解释）
        Double,       ///< CONSTANT_Double（num = 原始位型，i64 解释）
        String,       ///< CONSTANT_String（str = 内容视图）
        Class,        ///< CONSTANT_Class（str = 内部名视图）
        MethodType,   ///< CONSTANT_MethodType（str = 描述符视图）
        MethodHandle, ///< CONSTANT_MethodHandle（handle 有效）
        Dynamic,      ///< CONSTANT_Dynamic（str/desc = NameAndType 两段；值不展开）
        Unreadable,   ///< 结构畸形占位（数值字段无意义）
    };

    Kind             kind = Kind::Int;
    ca::i64          num   = 0;   ///< Int/Long 值；Float/Double 原始位型
    std::string_view str;         ///< String 内容 / Class 内部名 / MethodType 描述符 / Dynamic 名
    std::string_view desc;        ///< Dynamic 类型描述符（其余族恒空）
    IndyHandleView   handle;      ///< MethodHandle 族有效
};

/// 单个 indy 站点的只读事实包（resolve 形参）。
///
/// 全部文本借用采集方会话：视图只在单次 `resolve` 调用内有效，resolver
/// 需要留存时自行驻留进自有存储。`bsmArgs` 与站点一一对应、按序交付。
struct IndySiteFacts
{
    std::string_view ownerClass;   ///< 调用者类内部名（'/' 分隔）
    std::string_view methodName;   ///< 调用者方法名
    std::string_view methodDesc;   ///< 调用者方法描述符
    std::string_view siteName;     ///< indy 调用点名（CP NameAndType）
    std::string_view siteDesc;     ///< indy 调用点描述符（invokedType）
    IndyHandleView   bootstrap;    ///< BSM 方法句柄（结构畸形时 kind == 0）
    std::vector<IndyBsmArg> bsmArgs;   ///< BSM 静态参数（按序）
};

/// resolver 命中后交付的直接目标：直接成员调用三段 + 调用形态。
///
/// 文本寿命 = resolver 对象存活期（管理器持 resolver 到 run 结束，改写阶段
/// 读视图时 resolver 必然存活）；动态物化的文本驻留 resolver 自有存储。
struct ResolvedIndyTarget
{
    /// 调用形态：改写目标的 invoke 族轴（v1 三态）。
    enum class Form : ca::u8
    {
        Static,    ///< invokestatic（目标 desc 不含接收者）
        Virtual,   ///< invokevirtual（目标 desc 不含接收者）
        Interface, ///< invokeinterface（目标 desc 不含接收者）
    };

    Form             form = Form::Static;
    std::string_view owner;   ///< 目标声明类内部名
    std::string_view name;    ///< 目标成员名（`<init>` 不在 v1 面）
    std::string_view desc;    ///< 目标方法描述符（非 static 形不含接收者参数）
    /// 站点尾部元数据实参个数：这些实参只喂 BSM、目标不消费，改写时丢弃。
    /// 改写侧判据见 doc:indy-inline-spec。
    ca::u32 trailingMetadataArgs = 0;
};

/// @brief invokedynamic 还原 resolver SPI：setup/resolve 两段。
///
/// - `setup`：容器级准备（扫描 BSM 表、驻留解密键等），一次运行调用一次；
///   失败（`Err(SetupFailed)`）时该 resolver 本轮整体排除，不影响其他
///   resolver 与本次运行。
/// - `resolve`：逐站点调用。命中返回 `Ok(target)`；不归本 resolver 返回
///   `Err(NoTarget)`（消费方继续试下一优先级）；内部失败返回
///   `Err(ResolveFailed)`（该站点保守保留，继续试下一优先级）。任何路径下
///   resolver 都不产生副作用——改写判据与字节落笔归消费方（pass）。
///
/// 实现方约定：`resolve` 必须是纯查询（可重复调用同结果）；文本视图寿命
/// 见 @ref ResolvedIndyTarget "ResolvedIndyTarget"。
class IndyResolver
{
public:
    virtual ~IndyResolver() = default;

    /// resolver 身份：诊断输出 + 同优先级稳定排序键。注册存活期内稳定。
    virtual std::string_view name() const = 0;

    /// 优先级：数值大者先试。缺省 0。
    virtual ca::i32 priority() const { return 0; }

    /// @brief 容器级准备（缺省 no-op）。失败码 `SetupFailed`。
    virtual Result<void, IndyResolverError> setup(mjbase::model::ResourceContainer&)
    {
        return Ok();
    }

    /// @brief 尝试还原一个 indy 站点（契约见类注）。
    virtual Result<ResolvedIndyTarget, IndyResolverError>
        resolve(const IndySiteFacts& site) = 0;
};

/// @brief resolver 管理器：注册 + 优先级排序 + setup 扇出 + 首个命中解析。
///
/// 排序纪律对齐 Recaf 的 InvokeDynamicResolverManager：priority 降序，同
/// 优先级按 `name()` 升序——同名站点在多次运行间命中同一 resolver，顺序
/// 确定性不依赖注册次序。同名重复注册显式拒绝（撞名是事故不是更新）。
class IndyResolverManager
{
public:
    /// @brief 注册一个 resolver（持有所有权）。空指针 `Err(NullResolver)`，
    ///        名字重复 `Err(DuplicateName)`。
    Result<void, IndyResolverError> registerResolver(std::unique_ptr<IndyResolver> resolver);

    /// @brief 对全部 resolver 跑一遍 setup；返回失败被排除的个数。
    ///        失败 resolver 本轮（到下一次 setupAll 为止）不参与 resolve。
    ca::usize setupAll(mjbase::model::ResourceContainer& container);

    /// @brief 当前可参与 resolve 的 resolver 视图（优先序）：setup 前 = 全部
    ///        已注册，setup 后 = 通过筛选的子集（与 resolveFirst 同池）。
    const std::vector<IndyResolver*>& resolvers() const
    {
        return setupDone_ ? active_ : sorted_;
    }

    /// @brief 逐优先级试到一个命中。全部未命中返回 `Err(NoTarget)`。
    Result<ResolvedIndyTarget, IndyResolverError>
        resolveFirst(const IndySiteFacts& site) const;

private:
    /// 排序 + 排除重算（setupAll 与 registerResolver 共用）。
    void resort();

    std::vector<std::unique_ptr<IndyResolver>> owned_;
    std::vector<IndyResolver*>                 sorted_;   ///< 优先序（含未 setup 的全集）
    std::vector<IndyResolver*>                 active_;   ///< setup 通过子集
    bool                                       setupDone_ = false;
};

// ── 实现（header-only：契约件无独立编译单元）──

inline const char* to_string(IndyResolverError e) noexcept
{
    switch (e) {
    case IndyResolverError::NoTarget:      return "no target";
    case IndyResolverError::SetupFailed:   return "setup failed";
    case IndyResolverError::ResolveFailed: return "resolve failed";
    case IndyResolverError::NullResolver:  return "null resolver";
    case IndyResolverError::DuplicateName: return "duplicate resolver name";
    }
    return "unknown";
}

inline Result<void, IndyResolverError>
    IndyResolverManager::registerResolver(std::unique_ptr<IndyResolver> resolver)
{
    if (resolver == nullptr) return Err(IndyResolverError::NullResolver);
    const std::string_view name = resolver->name();
    for (const IndyResolver* existing : sorted_) {
        if (existing->name() == name) return Err(IndyResolverError::DuplicateName);
    }
    sorted_.push_back(resolver.get());
    owned_.push_back(std::move(resolver));
    resort();
    setupDone_ = false;   // 新成员未 setup，重置排除面
    return Ok();
}

inline void IndyResolverManager::resort()
{
    std::stable_sort(sorted_.begin(), sorted_.end(),
                     [](const IndyResolver* a, const IndyResolver* b) {
                         if (a->priority() != b->priority()) {
                             return a->priority() > b->priority();
                         }
                         return a->name() < b->name();
                     });
}

inline ca::usize IndyResolverManager::setupAll(mjbase::model::ResourceContainer& container)
{
    active_.clear();
    ca::usize failed = 0;
    for (IndyResolver* resolver : sorted_) {
        if (resolver->setup(container).is_err()) {
            ++failed;
            continue;
        }
        active_.push_back(resolver);
    }
    setupDone_ = true;
    return failed;
}

inline Result<ResolvedIndyTarget, IndyResolverError>
    IndyResolverManager::resolveFirst(const IndySiteFacts& site) const
{
    // setup 未跑（或注册后重置）：视全体为活跃——契约件不做隐藏状态门槛，
    // 消费方（pass）保证 setupAll 先行；测试直接用 resolveFirst 时按全集迭代。
    for (IndyResolver* resolver : resolvers()) {
        auto r = resolver->resolve(site);
        if (r.is_ok()) return r;
        if (r.unwrap_err() == IndyResolverError::NoTarget) continue;
        // ResolveFailed：该 resolver 对该站点失败，保守换下一个。
        continue;
    }
    return Err(IndyResolverError::NoTarget);
}

}   // namespace mjbase::spi
