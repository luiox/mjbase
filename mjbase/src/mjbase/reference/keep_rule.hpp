/// @file        keep_rule.hpp
/// @brief       keep 规则面：行式 DSL、类/成员 glob 匹配、保守 keep 预设模式库。
///
/// 审计 P0-1/P0-2（mbase/doc_ai/opt-pass-completeness-audit.md §6）的落地件：
/// 对标 商业化 trim 工具 的 trimExclude 脚本与 DEFAULT_TRIM_EXCLUDE 保守模式库，
/// 为全局收缩家族（unused_member_remover 四件）提供可配置的保留策略。
///
/// ## 设计定位
///
/// 本模块只做「判定」：给一个类名/成员，回答「是否被规则命中」。它不删字节、
/// 不碰容器、不依赖分析面——消费方（清理 pass）在既有硬底线（构造器/`<clinit>`/
/// 非 private/keep metadata）之上叠加规则命中，形成「硬底线 ∪ 规则」的保留集。
///
/// ## 键契约：规则只加不减
///
/// 规则集是纯加法输入：命中 → 保留；未命中 → 回到消费方既有策略。规则无法
/// 关闭硬底线（不存在「删除构造器」的规则形态——`member` 行恒为保留语义）。
/// 该不变量由消费方的使用方式结构保证（规则只喂 keep 判定），并由集成测试
/// 钉死。
///
/// ## DSL 语法（行式；空行与 `#` 注释行忽略；大小写敏感）
///
/// ```text
/// class <pattern>                      # 类保留：内部名 glob（'.' 归一为 '/'）
/// member <owner>#<name>(<desc>)        # 成员保留：三段 glob；省略括号 = 任意描述符
/// member <owner>#<name>                #
/// preset +<name>                       # 展开内置保守模式库（serialization/lambda）
/// ```
///
/// glob 语义：`*` 匹配不含 `/` 的任意片段（可为空）；`**` 匹配任意片段（可为空，
/// 含 `/`）；`?` 匹配不含 `/` 的单字符；其余字面。类名/宿主名的 `.` 在解析时
/// 归一为 `/`（Java 源码形态可直接书写）；`**` 才是「跨包」的通配——`com.foo.*`
/// 不匹配 `com/foo/a/b/C`。
///
/// 解析 fail-loud：任一非法行返回 `Err`（带 1-based 行号与原因码），不静默跳过
/// ——配置错误必须稳定失败，对齐仓内「配置错误稳定失败」纪律。
///
/// ## 预设模式库（商业化 trim 工具的默认排除表 的**命名模式**等价子集）
///
/// 每个预设是一条纯数据表（名字/描述符 glob），不含层级谓词（`extends`/
/// `implements`）与注解谓词（`@Foo`）——那两类需要 ReferenceIndex 的层级与
/// 注解事实，见下方「挂账」。能纯命名落地的按命名落地，落不了的宁可不做，
/// 也不造「看着像但语义不对」的近似：错的保留面比不保留更难排查。
///
/// | 预设 | 内容 |
/// |---|---|
/// | `+serialization` | 序列化回调族（精确描述符）+ `serialVersionUID` + 无参构造器 |
/// | `+lambda` | `lambda$*` / `$deserializeLambda$*` 合成方法 |
/// | `+rmi` | `*_Stub` / `*_Skel` / `*_Tie` 类名 + `<init>(RemoteRef)` |
/// | `+ejb` | EJB bean 生命周期回调族（`ejbCreate*` 等，按名） |
/// | `+jmx` | `*MBean` / `*MXBean` 类名 |
/// | `+enum` | `values()` / `valueOf(String)` / `$VALUES` 合成字段 |
/// | `+swing` | `createUI(JComponent)` / `getUI()`（可插拔 L&F 反射入口） |
/// | `+jaxb` | `ObjectFactory`（JAXB 生成物，反射实例化入口） |
///
/// ### 挂账（需要层级/注解事实，本模块不做）
///
/// - RMI：`interface *.* implements java.rmi.Remote` 的全部方法——需 `implements`
///   判定；当前只保 `_Stub`/`_Skel`/`_Tie` 这些**命名有特征**的骨架类。
/// - Servlet：`*.* implements javax.servlet.Servlet`——同上，纯命名无特征可用。
/// - EJB/JPA/JAX-WS：`@javax.ejb.*` / `@javax.persistence.*` / `@javax.jws.*`
///   注解族——需注解读取；`+ejb` 只覆盖命名有特征的生命周期回调。
/// - JAXB：`@XmlType` / `@XmlRootElement` 的 getter/setter 面——需注解读取；
///   `+jaxb` 只保 `ObjectFactory`。
/// - FXML：`@javafx.fxml.FXML`——需注解读取，纯命名无特征（`*Controller`
///   过宽，会把一切 MVC 控制器拖进保留集）。
///
/// 这些条目待 reference 面把层级与注解事实消费起来后按同一张表补齐——表本身
/// 是数据，补的是「谓词来源」，不是模式设计。
///
/// ## 线程安全
///
/// `parse`/`addPreset` 为突变入口（非线程安全）；`matchesClass`/`matchesMember`
/// 为 const 查询。惯例：构造完成后经 `shared_ptr<const KeepRuleSet>` 共享，
/// 只走查询面。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

namespace mjbase::reference {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

/// keep 规则种类。
enum class KeepRuleKind
{
    Class,   ///< 类级保留（内部名 glob）
    Member,  ///< 成员级保留（宿主/名称/描述符三段 glob）
};

/// 规则解析失败原因码。
enum class KeepRuleErrorCode : ca::u8
{
    EmptyPattern = 0,   ///< 模式为空（关键字后无内容或模式串为空）
    UnknownKeyword,     ///< 行首关键字不是 class/member/preset
    MissingArgument,    ///< 关键字存在但参数缺失/为空
    MalformedMember,    ///< member 行不是 `<owner>#<name>` 形态（缺 '#' 等）
    UnknownPreset,      ///< preset 名字不在预设库
    UnknownTier,        ///< keep 档位名不在档位表（见 keepTierFrom）
};

const char* to_string(KeepRuleErrorCode code) noexcept;

/// 解析失败详情：行号 + 原因码（`Err` 载荷；诊断面按行号定位）。
struct KeepRuleParseError
{
    ca::u32           line = 0;   ///< 1-based 行号（DSL 文本内）
    KeepRuleErrorCode code = KeepRuleErrorCode::EmptyPattern;
};

/// 保守 keep 预设（命名模式库）。
enum class KeepPreset : ca::u8
{
    Serialization = 0,   ///< 序列化回调族 + serialVersionUID + 无参构造器
    Lambda,              ///< lambda$* / $deserializeLambda$* 合成方法
    Rmi,                 ///< *_Stub / *_Skel / *_Tie + <init>(RemoteRef)
    Ejb,                 ///< EJB bean 生命周期回调族（按名）
    Jmx,                 ///< *MBean / *MXBean 类名
    Enum,                ///< values() / valueOf(String) / $VALUES
    Swing,               ///< createUI(JComponent) / getUI()
    Jaxb,                ///< ObjectFactory（JAXB 生成物）
};

const char* to_string(KeepPreset preset) noexcept;

/// @brief 预设名文本解析（`preset` 行与 `addPreset` 的文本入口共用）。
///
/// 接受 `serialization`/`lambda`/`rmi`/`ejb`/`jmx`/`enum`/`swing`/`jaxb`
/// （可选前导 `+`，大小写敏感）；空串与未知名字 `Err(UnknownPreset)`。
Result<KeepPreset, KeepRuleErrorCode> keepPresetFrom(std::string_view text);

/// @brief keep 档位：按访问标志整体保留对外 API 面（PUBLIC / PUBLIC_PROTECTED 档位）。
///
/// 档位是规则之外的整体开关——不看名字，只看成员的访问标志：
/// - `Public`：public 类 + public 成员入根（外部可链接面的最小集）；
/// - `PublicProtected`：public 类 + public/protected 成员入根（库型 JAR 的
///   默认保守档——子类仍可通过 protected 成员扩展）。
///
/// 包私有与私有成员不受档位影响：它们仍走规则命中与可达性闭包的既有判定。
/// 与规则的关系同「只加不减」：档位只扩大保留集，不关闭任何既有保留依据。
enum class KeepTier : ca::u8
{
    None = 0,          ///< 不叠加档位（只走规则命中）
    Public,            ///< public 类与成员整体入根
    PublicProtected,   ///< public 类 + public/protected 成员整体入根
};

const char* to_string(KeepTier tier) noexcept;

/// @brief 档位文本解析（CLI/`--keep-tier` 面）。
///
/// 接受 `none` / `public` / `public-protected`（下划线写法 `public_protected`
/// 同义，大小写敏感）；空串按 `None` 处理（"未指定"不是错误）。其余文本
/// `Err(UnknownTier)`——对齐仓内「配置错误稳定失败」纪律，不静默降级。
Result<KeepTier, KeepRuleErrorCode> keepTierFrom(std::string_view text);

/// 单条 keep 规则（glob 形态；name/desc 仅 member 规则使用）。
///
/// 三段模式均已在解析时归一化（`.` → `/`）。`name`/`desc` 空串表示「任意」的
/// 通配等价形态由解析层保证（desc 缺省展开为 `**`，name 不允许为空）。
struct KeepRule
{
    KeepRuleKind kind   = KeepRuleKind::Class;
    std::string  owner;   ///< 类内部名 glob（member 规则为宿主 glob）
    std::string  name;    ///< 成员名 glob（member；解析后恒非空）
    std::string  desc;    ///< 描述符 glob（member；缺省 `**`）
    std::string  source;  ///< 原始行文本（诊断回溯用，保留归一化前的输入）
};

/// glob 匹配：`*` 不跨 `/`，`**` 跨 `/`，`?` 单字符不跨 `/`，其余字面。
///
/// 模式与文本均按内部名形态（`/` 分隔）比较。回溯实现，模式规模受 DSL 行
/// 长度天然约束（用户配置行，非数据驱动），不设深度上限。
bool globMatch(std::string_view pattern, std::string_view text);

/// @brief keep 规则集：DSL 解析产物 + 匹配查询面。
///
/// 匹配语义为并集：任一路规则命中即保留。重复规则无害（不去重，保源序）。
class KeepRuleSet
{
public:
    KeepRuleSet() = default;

    /// @brief 解析行式 DSL 全文（fail-loud，见文件头语法）。
    /// @return Ok(规则集) 或 Err(行号+原因码)；不含任何部分结果。
    static Result<KeepRuleSet, KeepRuleParseError> parse(std::string_view text);

    /// @brief 追加一个预设的模式展开（程序化构造路径；未知预设 Err）。
    Result<void, KeepRuleParseError> addPreset(KeepPreset preset);

    /// 类内部名（`/` 分隔）是否被 class 规则命中。
    bool matchesClass(std::string_view internalName) const;

    /// 成员是否被 member 规则命中（owner/name/desc 均取内部形态）。
    bool matchesMember(std::string_view owner, std::string_view name,
                       std::string_view desc) const;

    bool     empty() const { return rules_.empty(); }
    ca::usize size() const { return rules_.size(); }

    /// 规则列表（源序；诊断与测试断言面）。
    const std::vector<KeepRule>& rules() const { return rules_; }

private:
    std::vector<KeepRule> rules_;
};

}   // namespace mjbase::reference
