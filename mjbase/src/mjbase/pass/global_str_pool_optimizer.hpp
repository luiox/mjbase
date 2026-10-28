#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 全局字符串池合成器：容器内全部 String ldc 去重编号，改写为
///        「索引装载 + 池访问调用」，并合成 `<clinit>` 初始化的字符串池
///        helper 类（旧栈 mbase pass/opt/global_str_pool_optimizer 载体
///        重推导）。
///
/// 适配形态：把散落各类的字符串常量迁移进单个合成类的静态数组——分析
/// 面只见「整数索引 + 单一取值点」，字符串内容面收敛到一处。
///
/// 判据与流程：
/// 1. 池类已存在（URI 命中）→ 整件跳过（幂等）。
/// 2. 全量普查：逐类资源建会话，收集全部 String ldc 的常量内容去重编号
///    （map 承载使 clinit 写出序为字典序；索引按遭遇序编号，对齐旧栈）。
///    任一类资源不可解析 → 整件放弃（普查全量性前提，fail-closed）；
///    非 class 资源忽略。
/// 3. 改写：命中 ldc 原位改写为索引装载（materializeIntConst 波段物化），
///    紧随 INVOKESTATIC str/Pool.getStr(I)Ljava/lang/String;——对是净栈
///    中性替换（调用弹索引推引用），分支目标帧不漂移，无需帧再生；
///    改写方法 maxStack 保守 +1（精确重算随帧面批次）。
/// 4. 合成 helper：str/Pool（中性名，不携带任何工具身份）——public
///    static String[] strPool、<init>、<clinit>（new String[n] + 逐项
///    aastore）、private static getStr(I)；writeClass 序列化后入容器。
///
/// 相对旧实现偏离（逐条申报）：
/// 1. 池类名改 `str/Pool`——旧栈类名携带来源仓身份，公开面禁现。
/// 2. 旧栈 clinit 计数无条件 SIPUSH（n>32767 溢出）；新栈走
///    materializeIntConst 波段（BIPUSH/SIPUSH/LDC）。
/// 3. maxStack 由「拍平 max(旧,3)」改为「改写方法 +1」的保守上界（对是
///    中间深度 +1，任何点位不超过旧 max+1）。
/// 4. 会话经 addMethod/promote/append/lowerMethod 全合成模型构建 helper
///    （旧栈 visit 式 writer）。
///
/// 安全门：任一改写方法 lower 或序列化失败 → 整件放弃（容器两阶段提交：
/// 改写批次与 helper 入容器前全部序列化成功才动容器）。
///
/// 发生改写返回 none()，否则 all()。使字符串内容面失效。
///
/// @par 标签
/// - `scope:global`
/// - `action:rewrite`
/// - `stability:opt-in`
class GlobalStrPoolOptimizer : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace mjbase::pass
