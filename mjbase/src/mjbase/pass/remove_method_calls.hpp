#pragma once

#include <string>
#include <vector>

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 成员点名规格：owner（类内部名）/ name / desc 三元组。
///
/// desc 为空串表示该名字下任意描述符都命中（重载整体点名）；owner/name
/// 恒为精确匹配。规格对容器内成员与外部（JDK/库）成员同样生效——匹配是
/// 调用点 CP 操作数上的字面比较，不要求目标在容器内声明。
struct MethodCallSpec
{
    std::string owner;   ///< 类内部名（'/' 分隔），精确匹配
    std::string name;    ///< 方法名，精确匹配
    std::string desc;    ///< 方法描述符；空 = 任意描述符
};

/// @brief MethodCallRemover 的配置面（程序化注入点）。
///
/// 两个删除面相互独立，均默认关闭：
///
/// - **点名删调用点**（includeCalls / excludeCalls）：include 命中的成员，
///   其在全部保留代码里的调用点被移除；exclude 从 include 集里整成员除名
///   （exclude 优先）。对应商业化 trim 工具 removeMethodCalls 脚本语句的
///   include/exclude 双名单模型（spec 驱动登记-批量应用）；脚本语言的
///   模式通配与 containedIn 调用方维度不携带——程序化层保持成员粒度。
/// - **无用结果调用删除**（enableUnusedResultWhitelist）：调用结果被立即
///   丢弃（pop/pop2 或存入死局部）且被调方命中白名单时删除调用。白名单 =
///   useDefaultWhitelist 时的内建保守集 + discardableCalls 用户追加。
///
/// 默认构造的配置两个面全关 → 整 pass 完全 no-op（opt-in 纪律）。
struct MethodCallRemovalConfig
{
    /// 点名要删除其调用点的成员。
    std::vector<MethodCallSpec> includeCalls;
    /// 从 include 集里除名的成员（exclude 优先于 include）。
    std::vector<MethodCallSpec> excludeCalls;
    /// 启用无用结果调用删除面。
    bool enableUnusedResultWhitelist = false;
    /// 白名单面启用时是否携带内建保守集（String 只读读 / Math·StrictMath
    /// 纯函数 / 零参无副作用 cat-2 JDK 调用）；false 时只用 discardableCalls。
    bool useDefaultWhitelist = true;
    /// 用户追加的可丢弃结果调用（结果被立即丢弃时删除）；owner/name 精确，
    /// desc 空 = 任意描述符。
    std::vector<MethodCallSpec> discardableCalls;
};

/// @brief 按配置点名删除方法调用点（容器级直写）。
///
/// 语义等价商业化 trim 工具的「调用点收尾」件：用户名单点名的成员，其调用
/// 点从全部保留代码里移除——后续「敢删非私有成员」的删除批依赖本件把悬垂
/// 调用先收干净。两个删除面的共同栈等价规则（宁可留调用，不可产生非法栈）：
///
/// - **可删形态**：调用点消费面为 `pop`（cat1 返回）/ `pop2`（cat2 返回）/
///   void 返回（无消费面），或「存入死局部」（该局部全方法再无同族 load，
///   存则随调用一并删）。删除 = 摘掉调用与消费面，在原位补 `pop`/`pop2`
///   序列吞掉实参槽与接收者（cat1 参数 pop1、long/double 参数与 cat2 返回
///   pop2）——吞返回值语义：删除后栈里不再有返回值。
/// - **放弃路径（调用点原样保留）**：调用点前一条是 dup/dup2 族或 swap
///   （`new; dup; invokespecial <init>` 形态删调用会让幸存引用永远停留
///   uninitialized）；返回类别与消费面不匹配；返回值被其他指令消费；调用
///   点或消费面被分支/异常表锚定（批删拒收）；invokedynamic 调用点永不删
///   （其目标是 bootstrap 而非成员）。
/// - **句柄钉死放弃（整成员）**：任一 class 的常量池存在引用该成员的
///   MethodHandle 条目（ldc / invokedynamic·condy 的 bootstrap 面最终都落
///   在 CP MethodHandle 上）时，该成员的调用点整体不删——调用点收尾为
///   「成员随后被删」服务，句柄仍钉着成员时删调用与删除意图矛盾，按保守
///   整体跳过。
///
/// 无用结果白名单的内建保守集（useDefaultWhitelist，与旧栈硬编码白名单
/// 逐条对齐）：`String.length()/isEmpty()`（仅恒定字符串接收者，三指令窗
/// 整体删）、`Math`/`StrictMath` 纯函数名单（全部实参均为相邻原始类型常量
/// 时整窗删）、零参无副作用 cat-2 JDK 调用 `System.nanoTime()J` /
/// `System.currentTimeMillis()J` / `Math.random()D`（结果紧跟 pop2 时删）。
///
/// 载体与提交：容器级直写（会话内 promote → CodeEditor 编辑 → lower →
/// 分支体 Strict 帧再生），两阶段提交（全部类序列化成功才动容器）；任一
/// class 不可解析整 pass 放弃（返回 all()，不做部分删除——句柄钉死证明
/// 需要全容器可读）。发生删除返回 none()：全局分析（引用索引/调用图/
/// 可达性）的缓存边随失效整体重建，后续消费者不会吃到悬垂边。第二次运行
/// 是严格 no-op（固定点）。
///
/// @par 标签
/// - `scope:global`
/// - `action:eliminate`
/// - `stability:opt-in`
class MethodCallRemover : public mjbase::spi::GlobalPass
{
public:
    /// @param config 删除配置；默认构造 = 完全 no-op。
    explicit MethodCallRemover(MethodCallRemovalConfig config = {});

    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;

private:
    MethodCallRemovalConfig config_;
};

}   // namespace mjbase::pass
