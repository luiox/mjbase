#pragma once

#include <string>
#include <unordered_map>

#include "libca/core/datatype.hpp"

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 收集到的静态常量值（值语义，键侧字符串自持有——发布视图存活期
///        越过收集会话，会话内借用视图不得带出）。
struct StaticValue
{
    enum class Kind : ca::u8
    {
        Int,     ///< I/B/C/S/Z 字段族（栈面 int）；num 低 32 位有效
        Long,    ///< J 字段族；num 有效
        Float,   ///< F 字段族；bits 低 32 位为 IEEE-754 原始位图
        Double,  ///< D 字段族；bits 为 IEEE-754 原始位图
        Str,     ///< Ljava/lang/String; 字段族；str 为文本
    };

    Kind        kind = Kind::Int;
    ca::i64     num  = 0;   ///< Int / Long 数值
    ca::u64     bits = 0;   ///< Float / Double 原始位图（保 NaN payload 与 -0.0）
    std::string str;        ///< Str 文本
};

/// @brief 静态常量视图：收集件的本批产出物，物化件的消费面。
///
/// 键为 (owner, name, desc) 三段精确匹配——GETSTATIC 的 CP 操作数语义视图，
/// 与字段声明身份一致（经父类解析的继承读点键不命中即保守保留，见
/// static-value-inlining-spec §4）。键与值全部深持有（std::string），视图
/// 存活期由挂载它的 GlobalAnalysisManager 决定，与收集期会话解耦。
class StaticValueView
{
public:
    /// 写入一条（同键覆盖；收集件对同键只产一条，参数形式服务幂等二跑）。
    void put(std::string owner, std::string name, std::string desc, StaticValue value);

    /// 精确查找；未命中返回 nullptr。
    const StaticValue* find(std::string_view owner, std::string_view name,
                            std::string_view desc) const;

    bool      empty() const noexcept { return values_.empty(); }
    ca::usize size() const noexcept { return values_.size(); }

private:
    struct Key
    {
        std::string owner;
        std::string name;
        std::string desc;
        bool operator==(const Key& o) const noexcept
        {
            return owner == o.owner && name == o.name && desc == o.desc;
        }
    };
    struct KeyHash
    {
        ca::usize operator()(const Key& k) const noexcept
        {
            std::hash<std::string> h;
            const ca::usize seed = h(k.owner);
            return seed ^ (h(k.name) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2)) ^
                   (h(k.desc) + 0x9e3779b97f4a7c15ULL);
        }
    };

    std::unordered_map<Key, StaticValue, KeyHash> values_;
};

/// @brief 视图的 AM pinned 分析包装：跨 pass 传递载体（无计算语义）。
///
/// 收集件经 GlobalAnalysisManager::setPinned 发布，物化件经 getPinned 消费。
/// 走 pinned 面是刻意的：视图必须在收集件返回后的全部后续 pass（含中间
/// 变更件的 none() 失效）之下存活，普通结果缓存做不到；且「链上没有收集件
/// 就没有视图」保证物化件单独启用时严格 no-op（独立启停，见
/// static-value-inlining-spec §3）。
struct StaticValueViewAnalysis
{
    using Result = StaticValueView;
};

/// @brief 字段描述符族 → 期望值类别（收集件的写入面与物化件的类别桶
///        共用同一张族表，两件判据不漂移）：I/B/C/S/Z → Int（栈面 int），
///        J → Long，F → Float，D → Double，Ljava/lang/String; → Str；
///        支持集外返回 false。
inline bool staticValueFamilyKind(std::string_view desc, StaticValue::Kind& out)
{
    if (desc == "I" || desc == "B" || desc == "C" || desc == "S" || desc == "Z") {
        out = StaticValue::Kind::Int;
        return true;
    }
    if (desc == "J") {
        out = StaticValue::Kind::Long;
        return true;
    }
    if (desc == "F") {
        out = StaticValue::Kind::Float;
        return true;
    }
    if (desc == "D") {
        out = StaticValue::Kind::Double;
        return true;
    }
    if (desc == "Ljava/lang/String;") {
        out = StaticValue::Kind::Str;
        return true;
    }
    return false;
}

/// 值类别与字段描述符族是否匹配（物化件的 cat1/cat2 桶判据）。
inline bool staticValueFamilyMatches(std::string_view desc, StaticValue::Kind kind)
{
    StaticValue::Kind want = StaticValue::Kind::Int;
    return staticValueFamilyKind(desc, want) && want == kind;
}

/// @brief 静态常量收集器（容器级，只读）：逐类评估 `<clinit>` 对本类
///        static 字段的常量/纯算术写，连同既有 ConstantValue 属性面，产出
///        容器级 StaticValueView（pinned 发布，字节零改动）。
///
/// 动机来源：Recaf 侦察第一吸收项（StaticValueCollectionTransformer 的
/// mjbase 域收编）——真实混淆器普遍把常量散进伴生类 static 字段
/// （grunt 罐实证 2516 类 / 93.8 万 GETSTATIC 站点），本件与其配对物化件
/// StaticValueInlining 组合消费；与写侧 StaticFieldDefaultInliner 在同一
/// 管线可链（SFDI 先把 clinit 单写折叠进 ConstantValue，本件走属性面
/// 照收），互不依赖、互不修改。
///
/// ## 判据（fail-closed 逐桶，全部拒绝面字节不动）
///
/// 只发布同时满足下列全部条件的类与字段：
///
/// - **走廊纯度（类级）**：clinit 指令集 ⊆ {常量装载（int/long/float/
///   double/String），32 条纯算术二元 op，NOP，PUTSTATIC(owner==本类)，
///   RETURN}，异常表为空。走廊外的任何指令（invoke/分支/foreign 静态
///   访问/监视器/athrow/aconst_null 等）→ 整类不发布。这条门同时背书
///   物化件的安全性：clinit 无可观察初始化效应，删 GETSTATIC 读点不丢
///   行为（spec §5.1）。
/// - **父链纯度**：super 链逐级同判；链上类不在容器且非 ``java/`` 前缀
///   → 拒（父类初始化效应不可证）。``java/`` 截断（基座类 clinit 无
///   可观察效应，Recaf 同口径）；父链环 → 拒。
/// - **走廊预算**：clinit 指令数 > kMaxCorridorInsns（2 万，Recaf
///   max-steps 同量级）→ 整类拒（求值不收敛桶）。
/// - **单写者**：容器全域 PUTSTATIC 普查，(owner,name,desc) 写点总数
///   ≠ 1 或唯一写点不在 owner 的 `<clinit>` → 该字段拒（多次写冲突桶
///   + 非 clinit 写桶）。
/// - **字段面**：volatile 拒；描述符不入五族（见
///   staticValueFamilyKind）拒；字段不存在拒。
/// - **值面**：走廊求值经 scratch 会话跑既有 ConstantFolder（复用其
///   除零 / MIN÷-1 红线与 NaN 位图语义，本件不重写算术）后，PUTSTATIC
///   的顺序前驱必须恰为匹配族常量装载，否则该字段拒。
/// - **ConstantValue 面（链式收编）**：static + final + 非 volatile +
///   容器全域零写点的字段，其既有 ConstantValue 属性（五族）照收进
///   视图——SFDI 在前道工序产出、javac 常量字段原生形态两条来源统一。
///   final 是硬判据（JVM 只对 static final 生效该属性，非 final 挂该
///   属性的值不可信）。
///
/// 任一 class 资源不可解析 → 整件放弃（普查全量性前提）。视图经 pinned
/// 发布后返回 all()（只读件：无 IR 改动；视图生命周期由 pinned 面保证，
/// 不参与失效协议）。二次运行幂等（覆盖式重发布，结果恒同；无内容时
/// 撤钉，物化件侧严格 no-op）。
///
/// @par 标签
/// - `scope:global`
/// - `action:collect`
/// - `stability:opt-in`
class StaticValueCollection : public mjbase::spi::GlobalPass
{
public:
    /// clinit 走廊求值预算（指令数；超出整类拒）。Recaf
    /// StaticValueCollectionTransformer 的 max-steps 20000 同量级口径。
    static constexpr ca::usize kMaxCorridorInsns = 20000;

    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace mjbase::pass
