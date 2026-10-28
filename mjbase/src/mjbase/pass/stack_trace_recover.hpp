#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 堆栈跟踪恢复：恢复 SourceFile/SourceDebug 类属性，在每个
///        try-catch handler 入口插入 `dup; invokevirtual printStackTrace()V`，
///        并为每个标签锚点补 LineNumberTable 行（旧栈 mbase pass/opt/
///        stack_trace_recover 重推导）。
///
/// 适配形态：混淆器剥掉调试信息后异常栈不可读；本件按确定性规则重建
/// 「源文件名 = 类内名点分 + .java」「handler 入口打印调用栈」「每个标签
/// 一个递增行号」的最小调试面，使异常回溯重新可定位。
///
/// 判据与流程（逐类）：
/// 1. SourceFile/SourceDebug 覆盖式写入（旧栈 setter 语义，已有槽改写、
///    缺失槽新建）；SourceDebug 载荷为 u2 Utf8 索引（RawAttr）。
/// 2. handler 聚合：同一 handler label 被多个 try 块共享时——单一 catch
///    类型取该类型为 invokevirtual owner；catch-all（catchCp = 0）或多
///    类型共享取 java/lang/Throwable。printStackTrace()V 声明在
///    Throwable 上，合法 catch 类型必为其子类，以自身/Throwable 为
///    owner 恒可验证；硬编码具体子类会让 catch-all/宽类型 handler 产
///    生 VerifyError（入栈类型不可赋值）。
/// 3. 行号：每个标签锚点一行（startPc = 锚点指令旧 pc，line 类级跨方
///    法递增、>65535 回绕 1）；行写入 backing Code 属性的 LNT（旧 pc
///    坐标），布局变化时由 lowerMethod 按 pcmap 精确重定向，布局未变
///    时坐标恒等直接生效。
///
/// 相对旧实现偏离（逐条申报）：
/// 1. 载体换 GlobalPass 逐类会话（旧栈 AsmClassPass）；不可解析类整件
///    放弃（fail-closed，对齐全局件家族惯例——旧栈类域 pass 不受无关
///    类影响，此处一票否决更保守）。
/// 2. LNT 行生成换「标签锚点 → LNT 行」直写（旧栈插 LineNumberNode 列
///    表节点，由 ASM 写回时落表）。
/// 3. dup 插入的 maxStack 保守 +1（旧栈 ASM 写回自动重算 maxes）。
/// 4. 未锚定 handler label 跳过（旧栈 label 映射 miss 同形）。
///
/// 非幂等（对齐旧栈）：每轮追加一组打印调用与一批新行号，opt-in 单轮
/// 使用。sourceFile 恒被改写 → 恒返回 none()。
///
/// 安全门：任一类不可解析 / 属性解码失败 / 编辑或 lower 失败 → 整件放
/// 弃（两阶段提交：全部类序列化成功才动容器）。
///
/// @par 标签
/// - `scope:global`
/// - `action:recover`
/// - `stability:opt-in`
class StackTraceRecover : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace mjbase::pass
