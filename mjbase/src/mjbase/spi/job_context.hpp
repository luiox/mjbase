#pragma once

#include "libca/str/utf8_string_arena.hpp"

#include "mjbase/keys.hpp"

namespace mjbase::spi {

/// @brief 作业级根上下文：唯一持有 Utf8StringArena 与跨类身份 KeyPool，
///        活到整个 jar 作业结束。
///
/// 「单 arena 贯穿作业」：各级 AnalysisManager 构造时借用 arena()（经
/// am.arena() 暴露给 pass）；pass intern 的字符串（分析缓存里的类/成员名
/// 视图）全部指向它。跨类身份（ClassKey/MemberKey）统一驻留 identityPool()
/// ——方法级 pass 的作用域单元（MethodScope）持有的 MemberKey 即产自此池。
///
/// @warning 非线程安全。
class PassJobContext
{
public:
    /// 作业级 arena（分析/中间字符串）。
    ca::str::Utf8StringArena&       arena() { return arena_; }
    const ca::str::Utf8StringArena& arena() const { return arena_; }

    /// 跨类身份驻留池（ClassKey/MemberKey 的键来源）。
    mjbase::KeyPool&       identityPool() { return pool_; }
    const mjbase::KeyPool& identityPool() const { return pool_; }

private:
    // -- arena 死亡点声明 --
    // 本成员持有作业私有的 Utf8StringArena（全作业唯一）：经 arena() 借给
    // 各级 AnalysisManager，pass intern 的字符串视图全部指向它。
    // 视图寿命 = 本对象寿命；死亡点：随持有者析构——本类总是作为作业栈帧上
    // 的局部对象存在（管线入口 runClassPipeline / runContainerPipeline 的
    // 调用方各持一个），析构即整个 jar 作业结束，所有视图随之失效。
    // 消费者不得把视图带过作业结束点；需跨期持有则 intern 进自己的 arena 或深拷贝。
    ca::str::Utf8StringArena arena_;
    // KeyPool 自含驻留 arena：键 id 池存活期有效，纪律同上（死亡点 = 作业结束）。
    mjbase::KeyPool pool_;
};

}   // namespace mjbase::spi
