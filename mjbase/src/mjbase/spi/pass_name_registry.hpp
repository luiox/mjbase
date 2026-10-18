#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "mjbase/spi/cf_pass.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::spi {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

/// pass 作用域标签：与 pass-contract §1 三级作用域 + §6 cf 域一一对应。
enum class PassScope : ca::u8
{
    Method,  ///< 方法级：MethodPass，载体 MethodScope
    Class,   ///< 类级：ClassPass，载体 jbc::ir::ClassSession
    Global,  ///< 容器级：GlobalPass，载体 ResourceContainer
    Cf,      ///< classfile 级：CfClassPass，载体 jbc::core::ClassFile
};

const char* to_string(PassScope scope) noexcept;

/// 注册面失败码。
enum class RegistryError : ca::u8
{
    Ok = 0,
    UnknownPass,    ///< 查询/构造的名字未注册
    DuplicateName,  ///< 注册名字已被占用（跨 scope 撞名同样拒绝）
    ScopeMismatch,  ///< create 入口与该名字注册的 scope 不符
};

const char* to_string(RegistryError e) noexcept;

/// 注册表条目视图（查询面交付物）：名字 + scope 标签 + 描述；工厂由注册表持有，
/// 经 create 族调用。name/description 为注册时借入的视图，寿命 = 视图实参实体
/// （见注册契约）。description 服务 workspace availablePasses / CLI --describe
/// 展示面，注册方给直述文本（mbase 存 i18n key 由 tr 渲染；mjbase 消费面
/// （MCP）语言固定英文，直述文本免 i18n 依赖，未 i18n::init 的宿主也不泄漏 key）。
struct PassNameEntry
{
    std::string_view name;
    PassScope        scope = PassScope::Method;
    std::string_view description = {};
};

/// @brief pass 名字注册面：name → factory + scope 标签。
///
/// 职责边界：本注册面只管 name→factory 映射与 scope 标签，不管装配序——
/// buildNestedPipeline 装配面按 pass-contract §9 留待第二个管线消费方出现时再
/// 定型；CLI -p 按名选择属 cli 批，届时经本面查名构造再按 scope 标签投入对应
/// 管线。三个 create 入口按 scope 分型（unique_ptr<对应基类>），与三级
/// PassManager / CfClassPassManager 的 addPass 直连。
///
/// 形态：全局默认表（defaultPassRegistry）+ 显式注册两种；测试建独立
/// PassNameRegistry 实例，不污染全局表。
///
/// @par 注册契约
/// - 键为借用视图：注册方保证名字实体在注册表存活期内有效（惯例：字符串字面
///   量 / 驻留池）；注册表不复制存储，不收 std::string。
/// - 重复注册显式 Err(DuplicateName)，不覆盖——对 mbase 旧栈「同名覆盖」的显式
///   偏离：同名几乎必然是两个 pass 撞名的事故，静默覆盖会把错埋到 -p 选择时才
///   炸；需要幂等种子时由调用方先 has() 判断（defaultPassRegistry 即如此）。
/// - 查未知名：find 返回 Err(UnknownPass)；create 族对未知名返回
///   Err(UnknownPass)，不交哨兵空指针——空 unique_ptr 在 addPass 处是另一条
///   契约（配置错误抛 invalid_argument），两不相混。
///
/// @warning 非线程安全：注册与查询应在初始化阶段完成（对齐 mbase 同款纪律）。
class PassNameRegistry
{
public:
    using MethodFactory  = std::function<std::unique_ptr<MethodPass>()>;
    using ClassFactory   = std::function<std::unique_ptr<ClassPass>()>;
    using GlobalFactory  = std::function<std::unique_ptr<GlobalPass>()>;
    using CfClassFactory = std::function<std::unique_ptr<CfClassPass>()>;

    /// 注册一个方法级 pass；重名 Err(DuplicateName)。
    /// description 为可选展示文本（借用视图，寿命同 name）。
    Result<void, RegistryError> registerMethodPass(std::string_view name, MethodFactory factory,
                                                   std::string_view description = {});
    /// 注册一个类级 pass；重名 Err(DuplicateName)。
    Result<void, RegistryError> registerClassPass(std::string_view name, ClassFactory factory,
                                                  std::string_view description = {});
    /// 注册一个容器级 pass；重名 Err(DuplicateName)。
    Result<void, RegistryError> registerGlobalPass(std::string_view name, GlobalFactory factory,
                                                   std::string_view description = {});
    /// 注册一个 classfile 级 pass；重名 Err(DuplicateName)。
    Result<void, RegistryError> registerCfClassPass(std::string_view name, CfClassFactory factory,
                                                    std::string_view description = {});

    /// 名字是否已注册。
    bool has(std::string_view name) const;

    /// @brief 查名：scope 标签面；未知名 Err(UnknownPass)。
    Result<PassNameEntry, RegistryError> find(std::string_view name) const;

    /// @brief 全部条目，按 name 字典序（CLI list 类稳定输出的旧栈纪律）。
    std::vector<PassNameEntry> list() const;

    /// @brief 按名构造方法级 pass；未知名 Err(UnknownPass)、scope 不符
    ///        Err(ScopeMismatch)。工厂产出的具体 pass 以基类指针交付。
    Result<std::unique_ptr<MethodPass>, RegistryError>
        createMethodPass(std::string_view name) const;
    /// @copydoc createMethodPass
    Result<std::unique_ptr<ClassPass>, RegistryError> createClassPass(std::string_view name) const;
    /// @copydoc createMethodPass
    Result<std::unique_ptr<GlobalPass>, RegistryError>
        createGlobalPass(std::string_view name) const;
    /// @copydoc createMethodPass
    Result<std::unique_ptr<CfClassPass>, RegistryError>
        createCfClassPass(std::string_view name) const;

private:
    /// 条目：scope 标签 + 描述 + 四槽工厂（恰一槽非空，与注册入口一一对应——
    /// pass 基类之间无公共可delete基类型，按槽分存是最直白的类型安全形态）。
    struct Entry
    {
        PassScope      scope = PassScope::Method;
        std::string_view description = {};
        MethodFactory  method;
        ClassFactory   clazz;
        GlobalFactory  global;
        CfClassFactory cf;
    };

    const Entry* findEntry(std::string_view name) const;

    std::unordered_map<std::string_view, Entry> entries_;
};

/// @brief 全局默认注册表（函数局部静态，首次访问时种子注册）。
///
/// 种子面：三个结构级 pass 试点（ClassMemberSorter / IllegalClassFileCleaner /
/// AccessModifierTightener）+ pass A 批方法体改写族（NopRemover /
/// RedundantLoadValEliminator / CharArrToStrSimplifier / UnusedCheckCleaner）+
/// B 批确定性垃圾插入（JunkCodeInserter），名字用真实类名——对齐 mbase 旧栈
/// 注册命名纪律。种子在首次访问内完成且自带 has 防护语义（新表重名不可能），
/// 调用方无需也不要对默认表重复注册；pass 后续批次落地时在此追加种子。
PassNameRegistry& defaultPassRegistry();

}   // namespace mjbase::spi
