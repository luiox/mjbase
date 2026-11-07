#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf8_string_arena.hpp"

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @defgroup kotlin_metadata_collection Kotlin Metadata 收集件（收集 + 描述符
/// 映射工具链，Recaf KotlinMetadataCollectionTransformer 的能力面吸收项）。
///
/// 只读收集：逐类读类级 RuntimeVisibleAnnotations 中 `Lkotlin/Metadata;` 的
/// 注解体（与 KotlinMetadataRemover 读面同域、互不依赖——Remover 删该槽，
/// 本件读该槽），解码 d1 载荷（BitEncoding 7to8/UTF8 双形态 + protobuf 线
/// 格式子集）与 d2 串表（StringTableTypes 记录展开 + 预定义串 + 子串/替换/
/// 操作修饰），产出 Kotlin 类/函数/属性模型与「容器二进制名 ↔ Kotlin 真名」
/// 双向映射，挂容器 AnalysisManager 惰性缓存（ENR 收集件同款）。字节码零
/// 改动（恒 `PreservedAnalyses::all()`），还原件（按收集面匹配成员改名的
/// 组合件）不在本件。规格篇 doc:kotlin-metadata-collect-spec。
///
/// 描述符映射工具链吸收 Recaf `mapKtDescriptor` / `mapToJvm` /
/// `reverseMapDescriptor` 的价值面：Kotlin 模型 → L 形描述符（泛型擦除 +
/// Array 特例）、kotlin stdlib → JVM 类型替换表、收集真名 → 容器二进制名的
/// 反向重写。与 Recaf 原件的两处口径差异（吸收面增强）：扩展接收者进函数
/// 描述符（Recaf 丢弃 receiver_type，扩展函数描述符缺首参）；id 形类型引用
/// 经 TypeTable 展开（Recaf 只读 inline 形，共享类型场景失配）。
/// @{

/// @brief Metadata 的 k 字段取值（Kotlin 侧 kind 表）。
enum class KtClassKind : ca::u8
{
    Unknown          = 0,   ///< k 缺席/越界（fail-closed 拒面）
    Class            = 1,   ///< 类消息（d1 = ProtoBuf.Class）
    File             = 2,   ///< 文件门面（d1 = ProtoBuf.Package）
    SyntheticClass   = 3,   ///< 合成类/lambda（d1 = ProtoBuf.Function）
    MultiFileFacade  = 4,   ///< 多文件类门面（v1 不解出面）
    MultiFilePart    = 5,   ///< 多文件类部件（d1 = ProtoBuf.Package）
};

/// @brief Kotlin 类型可空性（d1 Type.nullable 面的收集口径）。
enum class KtNullability : ca::u8
{
    Unknown  = 0,   ///< nullable 字段缺席（平台类型）
    Nullable,       ///< nullable = true
    NonNull,        ///< nullable = false
};

/// @brief Kotlin 类型引用模型（d1 Type 消息）。
///
/// 名为内部名形（resolve 后 `.` 转 `$`，与 Recaf 口径一致）；泛型实参
/// 收集保留（诊断面），描述符映射面按擦除处理（仅 `kotlin/Array` 特例取
/// 首实参）。星号投影（STAR）与四类类型名全缺席的条目 `hasName = false`，
/// 映射面落 `Ljava/lang/Object;`。
struct KtType
{
    ca::str::Utf8StringRef name;                 ///< 类型内部名（hasName 时有效）
    bool                        hasName    = false;   ///< false = 星号投影/无名类型
    std::vector<KtType>         arguments;           ///< 泛型实参（保持 d1 序）
    KtNullability               nullability = KtNullability::Unknown;
};

/// @brief Kotlin 属性/参数模型（名 + 类型）。
struct KtVariable
{
    ca::str::Utf8StringRef name;             ///< 名（串表 resolve 后）
    KtType                 type;             ///< 类型
    bool                   hasType = false;  ///< false = d1 未给类型（映射面落 Object）
};

/// @brief Kotlin 函数模型（名 + 返回 + 参数 + 扩展接收者）。
///
/// 扩展接收者只在文件门面（k=2/5）的扩展函数上出现（类成员的分派接收者
/// 不写进 metadata）；描述符映射面把它作 JVM 首参（Recaf 原件丢弃此面，
/// 见组注释）。上下文接收者（v1.9+ field 10）v1 不收集（不进 JVM 参数面
/// 的稳定口径未定，申报为边界）。
struct KtFunction
{
    ca::str::Utf8StringRef  name;                          ///< 名
    KtType                  returnType;                    ///< 返回类型
    bool                    hasReturnType = false;         ///< false = d1 未给返回类型
    KtType                  receiverType;                  ///< 扩展接收者（hasReceiver 时有效）
    bool                    hasReceiver   = false;         ///< true = 扩展函数
    std::vector<KtVariable> parameters;                    ///< 值参数（保持 d1 序）
};

/// @brief Kotlin 构造器模型（参数表；v1 不解 id 形参数类型——构造器消息无
///        自有 TypeTable，申报为边界）。
struct KtConstructor
{
    std::vector<KtVariable> parameters;   ///< 值参数（保持 d1 序）
};

/// @brief Kotlin 类模型（k=1 Class / k=2,5 Package / k=3 Function 三类消息
///        的统一产物；文件门面与 lambda 的 name = 定义类二进制名）。
struct KtClass
{
    KtClassKind                 kind = KtClassKind::Unknown;
    ca::i32                     extraFlags = 0;              ///< xi 字段（缺席为 0）
    ca::str::Utf8StringRef      name;                        ///< Kotlin 真名（内部名形）
    bool                        hasName = false;             ///< false = 消息未给名
    ca::str::Utf8StringRef      companionObjectName;         ///< 伴生对象名（k=1 可选）
    bool                        hasCompanionObjectName = false;
    std::vector<KtType>         superTypes;                  ///< 父类型表（k=1）
    std::vector<KtConstructor>  constructors;                ///< 构造器表（k=1）
    std::vector<KtVariable>     properties;                  ///< 属性表（k=1,2,5）
    std::vector<KtFunction>     functions;                   ///< 函数表（k=1,2,5；k=3 恰一条）
};

/// @brief 收集失败原因（fail-closed 码表；失败类不进收集结果）。
enum class KotlinMetadataError : ca::u8
{
    Ok                 = 0,
    NoAnnotation,        ///< 类无 `Lkotlin/Metadata;` 类级 visible 注解（非失败语义，统一码表）
    BadAnnotationShape,  ///< 注解体形状不符（k/d1/d2 缺席或元素类型不符）
    BadData1Encoding,    ///< d1 解码失败（7to8/UTF8 形态或 CP 串解码出错）
    BadStringTable,      ///< StringTableTypes 记录展开越界（range 非正或超出 d2 界）
    BadProto,            ///< d1 protobuf 线格式截断/字段形状不符/索引越界
    UnsupportedKind,     ///< k=0/4（v1 不解出面，整类拒绝）
};

/// @brief 错误码的稳定文本（诊断输出用）。
const char* to_string(KotlinMetadataError e) noexcept;

/// @brief KtType 转 JVM 字段描述符（L 形；泛型擦除 + `kotlin/Array` 特例）。
///
/// 无名类型/空类型落 `Ljava/lang/Object;`；`kotlin/Array` 取首实参递归
/// 拼作 `[<首参描述符>`（首实参缺位退回 `Ljava/lang/Object;`）；其余为
/// `L<内部名>;`（后续 stdlib 替换交 @ref kotlinToJvmDescriptor "stdlib 替换面"）。
std::string ktTypeDescriptor(const KtType& type);

/// @brief KtFunction 转 JVM 方法描述符 `(参数)返回`。
///
/// 参数按声明序取 @ref ktTypeDescriptor 的结果；`hasReceiver` 时扩展接收者作
/// 首参；未给返回类型落 `Ljava/lang/Object;`。
std::string ktFunctionDescriptor(const KtFunction& fn);

/// @brief kotlin stdlib 转 JVM 等价类型描述符替换（Recaf mapToJvm 同表）。
///
/// 方法描述符逐参数/返回段替换，字段描述符整段替换；未命中表条的段原样
/// 保留。形状非法（非描述符文法）原样返回（fail-closed）。
std::string kotlinToJvmDescriptor(std::string_view descriptor);

/// @brief 收集结果（容器级分析产物）：二进制名转 Kotlin 模型索引 + 双向
///        类名映射。
///
/// 字符串面全部 intern 进本对象私有 arena（视图与对象等寿）；拷贝安全
/// （视图指向 chunk 堆块，移动不移内存）。建模同
/// `mjbase::mapping::IntermediateMapping` 的私有 arena 口径。
class KotlinMetadataCollection
{
public:
    /// 按容器二进制类名取模型；未收集返回 nullptr。
    const KtClass* findClass(std::string_view binaryName) const;

    /// 按 Kotlin 真名（内部名形，`.` 转 `$`）取模型；未收集返回 nullptr。
    const KtClass* findClassByKotlinName(std::string_view kotlinName) const;

    /// 容器二进制名转 Kotlin 真名；未收集返回空视图。
    ca::str::Utf8StringRef kotlinNameOf(std::string_view binaryName) const;

    /// Kotlin 真名（内部名形）转容器二进制名；未收集返回空视图。
    ca::str::Utf8StringRef binaryNameOf(std::string_view kotlinName) const;

    /// 收集条目（二进制名转模型，容器资源序；测试与诊断用）。
    const std::vector<std::pair<ca::str::Utf8StringRef, KtClass>>& entries() const { return entries_; }

    /// 收集条目数。
    ca::usize size() const noexcept { return entries_.size(); }

    /// 本对象私有 arena（收集期 intern 入口；随本对象存活）。
    ca::str::Utf8StringArena& arena() { return arena_; }

    /// 收集期写入面（仅收集器内部使用；同键二次写入拒绝，幂等二跑前提）。
    /// @return false = 该二进制名已有条目（重复收集）。
    bool add(ca::str::Utf8StringRef binaryName, KtClass&& model);

private:
    ca::str::Utf8StringArena                                arena_;
    std::vector<std::pair<ca::str::Utf8StringRef, KtClass>> entries_;
    std::unordered_map<ca::str::Utf8StringRef, ca::usize>   byBinary_;
    std::unordered_map<ca::str::Utf8StringRef, ca::usize>   byKotlin_;
};

/// @brief 反向映射描述符：串中命中收集 Kotlin 真名的类名段替换为容器
///        二进制名（Recaf reverseMapDescriptor 的吸收面）。
///
/// 输入为字段或方法描述符（文法内逐 `L...;` 段替换，数组前缀保持）；
/// 形状非法原样返回。未命中任何真名时返回内容相同的串。
std::string reverseMapDescriptor(const KotlinMetadataCollection& collection,
                                 std::string_view                descriptor);

/// @brief 单类 Metadata 注解体解析面（容器收集与单测共用）。
///
/// 读类级 RuntimeVisibleAnnotations 中 `Lkotlin/Metadata;` 条目（与
/// KotlinMetadataRemover 读面同域：不查 invisible 与 type annotation），
/// 解码产出模型；字符串视图 intern 进调用方 arena。无注解返回
/// `Err(NoAnnotation)`；解码/解析失败返回对应错误码（fail-closed，不产
/// 半截模型）。
///
/// @param cf 类 classfile DOM（懒 DOM：注解槽解码驻留缓存，参数取可变引用
///        与 `analysis::semanticFp` 同口径，算法本身只读）
/// @param arena 模型字符串的驻留池（与产出模型等寿）
ca::core::Result<KtClass, KotlinMetadataError>
parseKotlinMetadata(jbc::core::ClassFile& cf, ca::str::Utf8StringArena& arena);

/// @brief 容器级收集分析（AM 惰性缓存载体）：逐 ClassResource 解析
///        Metadata 注解，产出容器级收集结果。
///
/// 畸形类资源跳过（class_scan fail-open 同口径）；单类解析失败（fail-closed
/// 码表）该类零产出，不影响其他类。
struct KotlinMetadataCollectionAnalysis
{
    using Result = KotlinMetadataCollection;

    /// 逐类收集；结果缓存由 AnalysisManager 管理（ENR 同款惰性求值）。
    Result run(mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am);
};

/// @brief Kotlin Metadata 收集 pass（分析壳）：注册分析并触发计算，
///        字节码零改动，恒返回 `PreservedAnalyses::all()`。
///
/// `opt-in`（不进默认链；消费方为还原件与外部宿主，经
/// `getResult<KotlinMetadataCollectionAnalysis>` 取收集结果）。
///
/// @par 标签
/// - `scope:global`
/// - `action:collect`
/// - `stability:opt-in`
class KotlinMetadataCollector : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// @}

}   // namespace mjbase::pass
