/// @file        keep_roots.hpp
/// @brief       keep 配置 → GlobalReachabilityOptions 三槽（可达性根）的翻译面。
///
/// 审计 P0-1 的落地件：把「模式匹配（class 模式/成员模式/public-protected 档）」
/// 翻译成可达性闭包的三类根，供任何消费 GlobalReachabilityOptions 的 pass
/// 统一生效——不再要求每个删除 pass 自己理解 keep 规则。
///
/// ## 定位：纯增量
///
/// 本面只往三槽 **追加** 根，不清空既有根、不改传播规则、不动可达性内核
/// （`GlobalReachability::buildInternal` 与其选项语义均不动）。既有根来源
/// （资源 keep：module 描述符 / Manifest 入口键 / services 文件名与内容；
/// 调用方自供的入口）保持原序在前，本面产物追加在后。
///
/// ## 生命周期契约（关键，读三遍）
///
/// 三槽持 `string_view`（`rootClasses` / `rootMethods` / `rootFields`），本面
/// 写入的视图**一律指向 ReferenceIndex 池内的声明文本**——与声明事实同源，
/// 索引存活期有效。绝不指向局部 `std::string`：悬垂视图会让根静默变空串，
/// 症状是「keep 类被误删且无诊断」（曾致 UnusedClassRemover 的 keep 类丢失，
/// 修法见提交 f8e204acc）。调用方须保证 `index` 存活到 `reachability.build`
/// 之后。
///
/// ## 档位语义（PUBLIC / PUBLIC_PROTECTED 档位）
///
/// 档位是规则之外的整体开关——不看名字，只看访问标志：
///
/// | 档位 | 类 | 成员 |
/// |---|---|---|
/// | `None` | 不叠加 | 不叠加 |
/// | `Public` | public 类入根 | **public 类**的 public 方法/字段入根 |
/// | `PublicProtected` | public 类入根 | **public 类**的 public + protected 方法/字段入根 |
///
/// 类的访问标志里 `ACC_PROTECTED` 非法（JVMS 表 4.1-B），故两档的类侧一致；
/// 包私有与私有成员始终不受档位影响，仍走规则命中与可达性闭包的既有判定。
/// 成员根蕴含类根（build 内 `enqueueMethod` 连带 `enqueueClass`），故档位下
/// 类根与成员根重复无害。
///
/// ### 为什么成员档位根要求宿主类也是 public
///
/// 包私有类整体对外不可链接，其 public 成员同理——若把「包私有类的 public
/// 成员」也入根，成员根会经 `enqueueClass(owner)` 把宿主类拉回闭包，档位就从
/// 「保留对外 API 面」退化成「保留一切带 public 成员的类」，trim 直接失效。
/// 该约束只作用于档位；**显式 member 规则不受此限**（用户点名的私有成员必须
/// 保得住，否则规则面形同虚设）。
///
/// ## 与规则的关系
///
/// 同「只加不减」不变量：规则命中与档位命中都是纯加法，只扩大保留集，不
/// 关闭任何既有保留依据。规则为空、档位为 `None` 时本面是恒等变换（三槽
/// 不变）——消费方无需分支判断。
///
/// ## 去重
///
/// 不在本面去重：可达性 build 的 `markClass`/`markMethod`/`markField` 本就是
/// 集合语义（插入幂等），重复根只多一次哈希查找、不改变闭包结果。重复根
/// 来自「档位命中 ∩ 规则命中」与「声明遍历中的重复声明」，二者都合法。
#pragma once

#include "libca/core/datatype.hpp"

#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/keep_rule.hpp"
#include "mjbase/reference/reference_index.hpp"

namespace mjbase::reference {

/// @brief keep 根规格：规则集（可空）+ 档位。
///
/// 两个来源独立可选：`rules` 为空表示只走档位，`tier` 为 `None` 表示只走
/// 规则；两者都空即恒等变换。
struct KeepRootSpec
{
    const KeepRuleSet* rules = nullptr;   ///< 借用视图：调用方保证其存活不短于本次调用
    KeepTier           tier  = KeepTier::None;
};

/// @brief 把 keep 规格追加进可达性根三槽。
///
/// - class 规则：与索引内**声明类名**匹配（glob 只用于匹配，入根的是声明名
///   本身——模式文本不能当根，它不是真实类名）；
/// - member 规则：与声明成员三段匹配 → 方法根 / 字段根；
/// - 档位：按访问标志整体入根（见文件头档位语义）。
///
/// @param spec    keep 规格（借用）
/// @param index   声明事实来源；三槽写入的视图指向其池内文本
/// @param options 目标选项；既有根保留，本面产物追加
void applyKeepRoots(const KeepRootSpec& spec, const ReferenceIndex& index,
                    GlobalReachabilityOptions& options);

}   // namespace mjbase::reference
