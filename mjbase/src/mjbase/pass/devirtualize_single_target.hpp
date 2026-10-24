#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 去虚拟化：CHA 能证明调用点唯一活目标的 invokevirtual 改写为
///        invokespecial 直接分派（opt-in，不进默认链）。
///
/// 消费 `ResolvedCallGraphAnalysis`（CHA 档）+ `ReferenceIndexAnalysis` +
/// `HierarchyBundleAnalysis`：某虚调用点在覆盖等价族上恰有一条边、且该唯一
/// 目标是容器内声明的具体非桥方法时，把调用点改写为指向目标声明类的
/// `invokespecial`（Methodref 按 intern 纪律复用或追加，CP 只增）。
///
/// ## JVMS 合法性依据（SE17 章节；实现红线，逐条对照）
///
/// - **§6.5 invokespecial（选择规则）**：符号引用命名类记 R、当前类记 K。
///   三条件全真（(a) 落解方法非 `<init>`；(b) R 是类而非接口且 R 是 K 的
///   超类；(c) ACC_SUPER 置位）则 C = K 的直接父类，否则 C = R；选择自 C
///   沿父类链上扫首个同 name+desc 声明。本 pass 的目标恒非 `<init>`
///   （虚拟族排除 '<' 名），改写后 R = C_t ∈ ancestors(K)：
///   - C_t == K：条件 (b) 假 → C = K，首条选择规则命中 C_t 自身声明；
///   - C_t ⊊ K 的祖先：条件 (b) 真 + ACC_SUPER 置位 → C = super(K)，扫描
///     段 [super(K)..C_t] 内无同签名声明（C_t 是族内唯一声明者）→ 同选
///     C_t.m。与原 invokevirtual 的运行时分派（§6.5 invokevirtual：按
///     objectref 运行时类选择，§5.4.6）落同一方法。
/// - **§4.1 ACC_SUPER**：「In Java SE 8 and above, the Java Virtual Machine
///   considers the ACC_SUPER flag to be set in every class file, regardless
///   of the actual value of the flag and the version of the class file。」
///   调用者类版本 major < 52 时旧 JVM 会按实际位取语义，故本 pass 只改写
///   major ≥ 52 的调用者类（宿主无关的安全下界）。
/// - **§4.10.1.9.invokespecial（verifier）**：非 `<init>` 情形要求
///   `isAssignable(NamedClass, CurrentClass)`（命名类是当前类的超类/同类，
///   规则原文：「must name a method in the current class/interface or a
///   superclass/superinterface」）**且** 栈上接收者可赋给当前类（原文：
///   「targets a receiver object of the current class or deeper」；§4.9.2
///   结构约束的概括：「the class or interface named by invokespecial must
///   be "above" the caller … while the receiver object … must be "at" or
///   "below" the caller」）。因此接收者守卫收紧为**前一条指令恰是
///   `ALOAD_0`**（this 的校验类型 = 当前类；aload_1..3 可能持兄弟类型局部
///   变量，改写后过不了 verifier）。该接收者约束取代 protected 检查
///   （规则原文自述「supercedes … passesProtectedCheck」），protected 目标
///   无需额外门。
/// - **§5.4.6（执行期选择）+ nestmates（JEP 181，§5.4.4）**：落解方法为
///   ACC_PRIVATE 时「it is the selected method」——private 虚调用的运行时
///   目标就是落解目标；但 invokespecial 在「命名类是超类」形态下自
///   super(K) 上扫，中间类的同签名新声明会被误选（这正是 javac 对跨嵌套
///   类私有调用改用 invokevirtual 的原因）。故 ACC_PRIVATE 目标只在
///   C_t == K 时改写；跨 nest 私有形态（Java 11+ javac 产物）放弃。
///   访问控制（§5.4.4）对改写后的引用恒成立：public / protected（调用者
///   是目标声明类子类）/ package-private（C_t 在调用者父链上且原引用可
///   落解 ⇒ 同运行时包）/ private（同类）。
/// - **§2.9.3（签名多态）**：签名多态方法只声明于 java.lang.invoke.
///   MethodHandle / VarHandle——恒为容器外类型，CHA 落解降级编码目标、
///   「目标须容器内声明」门天然排除。`<init>`/`<clinit>`：虚拟族语义排除
///   '<' 名（§4.9.2 的 `<init>` 结构约束分支不可达）。
/// - **接口目标一律放弃**：invokeinterface/invokevirtual 指向接口声明时，
///   唯一族成员必为接口自身声明（类实现者会并入族使其非唯一）。§6.5
///   invokeinterface 的选择（§5.4.6）类链优先——接收者运行时类的父链
///   （含容器外 JDK 父类）上的同签名声明会压过接口 default，故「族唯一」
///   不构成改写到接口目标的证明（CHA 档文件头自述的 class-wins 欠近似
///   正是此事）。接口命名形态的 invokespecial 选择规则亦不同，不进入。
/// - **栈/帧不变**：invokevirtual 与 invokespecial 栈效应相同（弹接收者
///   与实参、压返回值）且同为 3 字节定长，帧与 StackMapTable 逐点不变，
///   lower 重定位为恒等映射。
/// - **CP 形态**：改写后操作数须为 CONSTANT_Methodref（§4.4.1；接口目标
///   既已放弃，实际只发生 Methodref→Methodref 复用/追加），经
///   `ClassSession::setMember` intern 落池。
///
/// ## 分析消费与封闭世界公理
///
/// 等价性论证依赖「运行时接收者类的父链在 C_t 之下只有容器内类」——即
/// 容器即全程序的封闭世界公理。这与既有全局删除面（ReferenceIndex /
/// GlobalReachability / UnusedClassRemover 依 CHA 边删成员）同一公理级别：
/// 容器外子类型（运行期挂接的未知子类）不受支持，opt-in 时由使用者担保。
///
/// **RTA 档不适用，本 pass 只消费 CHA（build()）**。RTA 的已实例化集合是
/// 观测驱动的欠近似（反射/native/classpath 外实例化按保守根处理，头注铁
/// 律「不作删除依据」），其方向是「宁多入根」；而改写是行为改写——证明
/// 强度要求与保守根相反，错误改写 = 外部子类覆写被静默绕过。RTA 档的
/// 边表与 RtaCallGraphAnalysis 包装不动、铁律原样保留。
///
/// ## 改写与放弃清单
///
/// 改写（全部门通过）：站点 opcode ∈ {INVOKEVIRTUAL, INVOKEINTERFACE}、
/// kind == Direct；该站点在 CHA 边表恰 1 条边；唯一目标容器内声明、非
/// static/abstract/bridge/synthetic、名非 '<' 开头；ACC_PRIVATE 时目标
/// 声明类 == 调用者；目标声明类非接口且在调用者 super 链上；调用者类
/// major ≥ 52；站点前一条指令为 ALOAD_0；站点前置边界无锚定标签；
/// 站点字节码事实（opcode + 编码目标）与分析一致。
///
/// 放弃（保守跳过该站点）：上述任一门不满足——多目标、零边/降级编码
/// 目标（含外部 owner）、接口目标、跨 nest 私有、复杂接收者形态
/// （非 ALOAD_0 前缀，dup 链等）、锚定窗（分支目标/异常表/调试边界落
/// 在调用点前缘）、分析-字节码坐标漂移。类级放弃（整类不写回，容器
/// 不动）：会话装载/方法升格/lower/序列化任一失败。幂等：改写后站点
/// 已是 invokespecial，二次运行严格 no-op。
///
/// 变换规则：无站点命中返回 all()；有改写返回 none()。
///
/// @par 标签
/// - `scope:global`
/// - `action:rewrite`
/// - `stability:opt-in`
class DevirtualizeSingleTarget : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&  container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace mjbase::pass
