#include "mjbase/pass/tool_class_combiner.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/remapper/member_remapper.hpp"
#include "mjbase/asm/remapper/member_remap_plan.hpp"
#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/frame_rewire.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"

namespace mjbase::pass {

namespace {

constexpr ca::u16 kAccPublic     = 0x0001;
constexpr ca::u16 kAccStatic     = 0x0008;
constexpr ca::u16 kAccSuper      = 0x0020;
constexpr ca::u16 kAccInterface  = 0x0200;
constexpr ca::u16 kAccAnnotation = 0x2000;

/// 旧栈分桶容量：宿主方法数合计上限（canMoveToolClassInto 语义照迁）。
constexpr ca::usize kMaxHostMethodTotal = 100;

/// 宿主命名（旧栈 newMergedToolClass 语义照迁）；幂等重入防护用短标记。
constexpr const char* kMergedHostPrefix = "me/canrad/morpher/ToolClassMergedClass_";
constexpr const char* kMergedHostMarker = "ToolClassMergedClass_";

bool isConstructorName(std::string_view name) noexcept { return name == "<init>"; }
bool isClinitName(std::string_view name) noexcept { return name == "<clinit>"; }

/// 装载完成的类：会话（backing + IR 同生共死）+ 类名视图 + 写回目标
/// （class_renamer 同款形状，comb 的搬移面在此之上做成员手术）。
struct LoadedClass
{
    std::string                            name;      ///< 类内部名（会话解析面）
    std::string                            uri;       ///< 容器条目（供体删除面）
    mjbase::model::ClassResource*          resource = nullptr;
    std::shared_ptr<jbc::ir::ClassSession> session;
};

/// 合并宿主：从零建类起步的会话 + 宿主名（分桶面）。
struct MergedHost
{
    std::string                            name;
    std::shared_ptr<jbc::ir::ClassSession> session;
};

/// 两阶段提交的暂存写回项。
struct PendingWrite
{
    std::string          uri;
    std::vector<ca::u8>  bytes;
};

/// 旧栈 isEmptyOrSafeToolClinit 语义照迁：<clinit> 无体，或体内全部指令
/// 都是 RETURN（libjbc 域无伪指令，标签不占指令位）。
bool isEmptyOrSafeToolClinit(jbc::ir::ClassSession& session, ca::usize methodIdx)
{
    if (!jbc::ir::hasCode(session, methodIdx)) return true;
    auto promoted = jbc::ir::promoteMethod(session, methodIdx);
    if (promoted.is_err()) return false;
    const jbc::ir::CodeUnit& cu = *promoted.unwrap().get();
    for (const jbc::ir::InsnId id : cu.dom.order()) {
        const jbc::ir::Insn* in = cu.dom.peek(id);
        if (in == nullptr) return false;
        if (in->op == jbc::core::Op::RETURN) continue;
        return false;
    }
    return true;
}

/// 旧栈 isSimpleToolMethodBody 语义照迁 + libjbc NotModeled 域保守延伸：
/// 无任何方法调用（invoke 全家族，indy 含在内）、无 PUTSTATIC；ldc 宽常量
/// 形态（Long/Float/Double/MethodType/MethodHandle/Dynamic，即 adoptMethod
/// 的 fail-closed 清单）命中时同样落选——对应旧栈「不支持范围保持原资源」
/// 按新搬移契约口径的延伸。
bool isSimpleToolMethodBody(jbc::ir::ClassSession& session, ca::usize methodIdx)
{
    if (!jbc::ir::hasCode(session, methodIdx)) return true;
    auto promoted = jbc::ir::promoteMethod(session, methodIdx);
    if (promoted.is_err()) return false;
    const jbc::ir::CodeUnit&       cu   = *promoted.unwrap().get();
    jbc::ir::ClassSession::MemberSem member;
    jbc::ir::ClassSession::ConstSem  constant;
    for (const jbc::ir::InsnId id : cu.dom.order()) {
        const jbc::ir::Insn* in = cu.dom.peek(id);
        if (in == nullptr) return false;
        if (session.memberOf(*in, member)) {
            if (member.kind != jbc::ir::ClassSession::MemberSem::Kind::Field) return false;
            if (in->op == jbc::core::Op::PUTSTATIC) return false;
        }
        if (session.constOf(*in, constant)) {
            using CK = jbc::ir::ClassSession::ConstSem::Kind;
            switch (constant.kind) {
            case CK::Long:
            case CK::Float:
            case CK::Double:
            case CK::MethodType:
            case CK::MethodHandle:
            case CK::Dynamic: return false;
            default: break;
            }
        }
    }
    return true;
}

/// 旧栈 isToolClassCandidate 语义照迁：Object 父类、名不含 `$`、无接口/
/// 注解、只含 static 字段与方法及构造器、非空 `<clinit>` 必须安全、至少
/// 一件可搬成员。升格失败按保守落选处理（原资源保持）。
bool isToolClassCandidate(jbc::ir::ClassSession& session)
{
    const jbc::ir::ClassUnit& unit = session.unit();
    if ((unit.access & kAccInterface) != 0) return false;
    if ((unit.access & kAccAnnotation) != 0) return false;
    if (unit.symbols().str(unit.superClass) != "java/lang/Object") return false;
    if (unit.symbols().str(unit.thisClass).find('$') != std::string_view::npos) return false;

    for (const jbc::ir::FieldUnit& fu : unit.fields) {
        if ((fu.access & kAccStatic) == 0) return false;
    }
    bool hasMovableMember = !unit.fields.empty();
    for (ca::usize i = 0; i < unit.methods.size(); ++i) {
        const jbc::ir::MethodUnit& mu    = unit.methods[i];
        const std::string_view     mname = unit.symbols().str(mu.name);
        if (isConstructorName(mname)) continue;
        if (isClinitName(mname)) {
            if (!isEmptyOrSafeToolClinit(session, i)) return false;
            continue;
        }
        if ((mu.access & kAccStatic) == 0) return false;
        if (!isSimpleToolMethodBody(session, i)) return false;
        hasMovableMember = true;
    }
    return hasMovableMember;
}

/// 旧栈 canMoveToolClassInto 语义照迁：方法数合计上限、字段撞名、方法
/// 撞 name+desc 任一命中即不可并入。
bool canMoveToolClassInto(const jbc::ir::ClassSession& host, const jbc::ir::ClassSession& src)
{
    const jbc::ir::ClassUnit& hu = host.unit();
    const jbc::ir::ClassUnit& su = src.unit();
    if (hu.methods.size() + su.methods.size() > kMaxHostMethodTotal) return false;
    for (const jbc::ir::FieldUnit& fu : su.fields) {
        const std::string_view name = su.symbols().str(fu.name);
        for (const jbc::ir::FieldUnit& hf : hu.fields) {
            if (hu.symbols().str(hf.name) == name) return false;
        }
    }
    for (const jbc::ir::MethodUnit& mu : su.methods) {
        const std::string_view name = su.symbols().str(mu.name);
        if (isConstructorName(name) || isClinitName(name)) continue;
        const std::string_view desc = su.symbols().str(mu.desc);
        for (const jbc::ir::MethodUnit& hm : hu.methods) {
            if (hu.symbols().str(hm.name) != name) continue;
            if (hu.symbols().str(hm.desc) == desc) return false;
        }
    }
    return true;
}

/// 宿主 CP 查找或追加 Utf8（intern 纪律：命中既有池不膨胀）。
ca::u32 findOrAddUtf8(jbc::core::ClassFile& cf, std::string_view s)
{
    if (const ca::u32 found = cf.cp.findUtf8(s); found != 0) return found;
    return cf.cp.addUtf8(s);
}

/// 重建被搬移字段的 ConstantValue：旧栈整节点搬移保真该属性；adoptField
/// 只搬元数据（libjbc §34.3(4) 字段属性表不随迁），常量值在此按源语义
/// intern 进宿主 CP 后以新建属性槽重挂——static final 常量的行为面等价。
void rebuildConstantValue(jbc::ir::ClassSession& src, const jbc::ir::FieldUnit& srcField,
                          jbc::ir::ClassSession& dst, const jbc::ir::FieldUnit& dstField)
{
    jbc::core::ClassFile& srcCf = src.backing();
    jbc::core::ClassFile& dstCf = dst.backing();

    jbc::core::MemberInfo& sm = srcCf.fields[srcField.backingIndex];
    for (jbc::core::AttrSlot& slot : sm.attrs) {
        if (!srcCf.cp.utf8Equals(slot.nameCp, "ConstantValue")) continue;
        const auto decoded = srcCf.decodeAttr(slot);
        if (decoded.is_err() || decoded.unwrap()->kind() != jbc::core::AttrKind::Raw) {
            throw std::runtime_error("ToolClassCombiner: ConstantValue 属性不可解码");
        }
        const auto* raw = static_cast<const jbc::core::RawAttr*>(slot.body.get());
        const std::vector<ca::u8>& payload = raw->bytes();
        if (payload.size() != 2) {
            throw std::runtime_error("ToolClassCombiner: ConstantValue 载荷长度非法");
        }
        const ca::u32 srcIdx =
            (static_cast<ca::u32>(payload[0]) << 8) | static_cast<ca::u32>(payload[1]);
        // 值拷贝后再动宿主池（Entry& 不得跨追加持存）。
        const jbc::core::ConstantPool::Entry entry = srcCf.cp.at(srcIdx);
        ca::u32 newIdx = 0;
        switch (entry.tag) {
        case jbc::core::CpTag::Integer:
            newIdx = dstCf.cp.addInteger(entry.ival);
            break;
        case jbc::core::CpTag::Float:
            newIdx = dstCf.cp.addFloatBits(static_cast<ca::u32>(entry.ival));
            break;
        case jbc::core::CpTag::Long:
            newIdx = dstCf.cp.addLong(entry.lval);
            break;
        case jbc::core::CpTag::Double:
            newIdx = dstCf.cp.addDoubleBits(static_cast<ca::u64>(entry.lval));
            break;
        case jbc::core::CpTag::String:
            newIdx = dstCf.cp.addString(dstCf.cp.addUtf8(srcCf.cp.utf8Of(entry.a)));
            break;
        default:
            throw std::runtime_error("ToolClassCombiner: ConstantValue 指向非常量条目");
        }
        const std::vector<ca::u8> rebuilt{static_cast<ca::u8>(newIdx >> 8),
                                          static_cast<ca::u8>(newIdx)};
        jbc::core::MemberInfo& dm = dstCf.fields[dstField.backingIndex];
        dm.attrs.push_back(jbc::core::AttrSlot::makeNew(
            findOrAddUtf8(dstCf, "ConstantValue"),
            std::make_unique<jbc::core::RawAttr>(rebuilt)));
        return;
    }
}

/// 从零建宿主（旧栈 newMergedToolClass 语义照迁）：V1_8 public super +
/// 调用 super 构造器的 public `<init>`，走 A5 从零会话 + addMethod 管线。
MergedHost createMergedHost(ca::usize index)
{
    MergedHost host;
    host.name = std::string(kMergedHostPrefix) + std::to_string(index);

    jbc::ir::NewClassSeed seed;
    seed.version   = (52u << 16);   // V1_8
    seed.access    = kAccPublic | kAccSuper;
    seed.thisName  = host.name;
    seed.superName = "java/lang/Object";
    auto built     = jbc::ir::makeSession(seed);
    if (built.is_err()) {
        throw std::runtime_error("ToolClassCombiner: 宿主种子被拒: " + host.name);
    }
    host.session = std::move(built).unwrap();

    jbc::ir::MethodSeed init;
    init.name      = "<init>";
    init.desc      = "()V";
    init.access    = kAccPublic;
    init.withCode  = true;
    init.maxStack  = 1;
    init.maxLocals = 1;
    auto added     = host.session->addMethod(init);
    if (added.is_err()) {
        throw std::runtime_error("ToolClassCombiner: 宿主 <init> 挂入失败: " + host.name);
    }
    const jbc::ir::MethodId initId = added.unwrap();
    auto promoted = jbc::ir::promoteMethod(*host.session, initId);
    if (promoted.is_err()) {
        throw std::runtime_error("ToolClassCombiner: 宿主 <init> 升格失败: " + host.name);
    }
    {
        jbc::ir::CodeEditor ed(*promoted.unwrap().get());
        const ca::u32 superCtor =
            host.session->internMethodRef("java/lang/Object", "<init>", "()V");
        if (!ed.append(jbc::ir::CodeEditor::simple(jbc::core::Op::ALOAD_0)).is_ok()
            || !ed
                   .append(jbc::ir::CodeEditor::cpInsn(jbc::core::Op::INVOKESPECIAL, superCtor))
                   .is_ok()
            || !ed.append(jbc::ir::CodeEditor::simple(jbc::core::Op::RETURN)).is_ok()) {
            throw std::runtime_error("ToolClassCombiner: 宿主 <init> 建体失败: " + host.name);
        }
    }
    if (jbc::ir::lowerMethod(*host.session, initId).is_err()) {
        throw std::runtime_error("ToolClassCombiner: 宿主 <init> 写回失败: " + host.name);
    }
    return host;
}

/// 方法搬移（§34.3 挂账回收）：libjbc adoptMethod 直调——libjbc 修复重放
/// 核的分支体双缺陷（CP 三读门误伤分支族 + 前向分支占位标签被 append 校验
/// 拒收）后，原「NOP 遮罩 append + get() 变异面恢复」的 pass 侧规避层整体
/// 回收，搬移语义单一真源回到库契约（design.md libjbc §34.3）。NoCode
/// （abstract/native static）按 adoptMethod 契约只搬元数据；失败转抛
/// （调用方整批回滚，容器不动）。
jbc::ir::MethodId adoptToolMethodIntoHost(MergedHost& host, LoadedClass& src,
                                          jbc::ir::MethodId mid)
{
    auto moved = host.session->adoptMethod(*src.session, mid);
    if (moved.is_err()) {
        throw std::runtime_error(std::string("ToolClassCombiner: adoptMethod 失败 (")
                                 + src.name + "): "
                                 + jbc::ir::to_string(moved.unwrap_err()));
    }
    return moved.unwrap();
}

/// 搬移一件供体（旧栈 moveToolClassMembers 语义照迁）：全部字段 adoptField
/// （ConstantValue 重建）；非构造器非 `<clinit>` 方法 adoptMethod 直调
/// （规避层已回收，见 adoptToolMethodIntoHost）。
/// 任何搬移失败整批抛出——准入面已把 NotModeled 形态挡在候选之外，这里
/// 的失败只剩真错误，按容器不动保守收场。
void moveToolClassMembers(LoadedClass& src, MergedHost& host)
{
    jbc::ir::ClassUnit& su = src.session->unit();

    for (ca::usize i = 0; i < su.fields.size(); ++i) {
        const jbc::ir::FieldId fid = su.fieldId(i);
        auto moved = host.session->adoptField(*src.session, fid);
        if (moved.is_err()) {
            throw std::runtime_error(std::string("ToolClassCombiner: adoptField 失败 (")
                                     + src.name + "): "
                                     + jbc::ir::to_string(moved.unwrap_err()));
        }
        const jbc::ir::FieldUnit* dfu = host.session->unit().field(moved.unwrap());
        const jbc::ir::FieldUnit* sfu = su.field(fid);
        if (dfu == nullptr || sfu == nullptr) {
            throw std::runtime_error("ToolClassCombiner: 搬移后字段身份失配");
        }
        rebuildConstantValue(*src.session, *sfu, *host.session, *dfu);
    }

    for (ca::usize i = 0; i < su.methods.size(); ++i) {
        const jbc::ir::MethodId mid = su.methodId(i);
        const jbc::ir::MethodUnit* mu = su.method(mid);
        if (mu == nullptr) continue;
        const std::string_view mname = su.symbols().str(mu->name);
        if (isConstructorName(mname) || isClinitName(mname)) continue;
        adoptToolMethodIntoHost(host, src, mid);
    }
}

/// 宿主帧再生（libjbc §34.3(4)(b) 消费者义务）：分支体或带异常表的方法
/// 并入 V51+ 宿主后 Strict 重算 + maxes 采用 + SMT 再生挂回，走 spi 帧链
/// 同一入口。宿主方法均为搬移产物（无非 Code 嵌套属性随迁），无悬垂适配面。
void regenerateHostFrames(MergedHost& host, mjbase::KeyPool& pool,
                          mjbase::spi::StrictTypeResolverAdapter& resolver)
{
    jbc::ir::ClassSession& session = *host.session;
    jbc::ir::ClassUnit&    unit    = session.unit();
    const mjbase::ClassKey ownerKey =
        mjbase::classKeyOf(pool, unit.symbols().str(unit.thisClass));

    for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
        const jbc::ir::MethodUnit& mu = unit.methods[idx];
        if (isConstructorName(unit.symbols().str(mu.name))) continue;
        if (!jbc::ir::hasCode(session, idx)) continue;
        auto promoted = jbc::ir::promoteMethod(session, idx);
        if (promoted.is_err()) {
            throw std::runtime_error(std::string("ToolClassCombiner: 宿主方法升格失败 (")
                                     + std::string(unit.symbols().str(mu.name)) + "): "
                                     + jbc::ir::to_string(promoted.unwrap_err()));
        }
        auto cuRef = std::move(promoted).unwrap();

        bool needsFrames = !cuRef->exc.empty();
        if (!needsFrames) {
            const ca::u32 branchBit = 1u << static_cast<ca::u32>(jbc::ir::InsnFamily::Branch);
            for (const jbc::ir::InsnId id : cuRef->dom.order()) {
                const jbc::ir::Insn* in = cuRef->dom.peek(id);
                if (in != nullptr && (jbc::ir::insnFamilyBit(in->op) & branchBit) != 0) {
                    needsFrames = true;
                    break;
                }
            }
        }
        if (!needsFrames) continue;

        const std::string_view name = unit.symbols().str(mu.name);
        const std::string_view desc = unit.symbols().str(mu.desc);
        const mjbase::MemberKey key =
            mjbase::memberKeyOf(pool, ownerKey, name, desc);
        // 堆分配 scope：unique_ptr 局部持有、地址互异且贯穿本次调用
        //（pass-contract §1 自造 scope 的地址唯一自证）。
        auto scope = std::make_unique<mjbase::spi::MethodScope>(
            mjbase::spi::MethodScope{mjbase::spi::MethodTarget{key, mu.id, name, desc,
                                                               mu.access, session},
                                     *cuRef.get()});
        mjbase::spi::frame_rewire::rewireFramesAfterLower(session, *scope, resolver);
    }
}

}   // namespace

mjbase::spi::PreservedAnalyses ToolClassCombiner::run(mjbase::model::ResourceContainer&  container,
                                                      mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    // 阶段零：装载与准入（装载序来自容器迭代，跨平台漂移；候选处理序
    // 在下方统一按内部名排序固定——解析失败按旧栈保守跳过）。
    std::vector<LoadedClass> loaded;
    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        if (!resource
            || !ca::core::tag_cast::isa<mjbase::model::ClassResource>(resource.get())) {
            continue;
        }
        auto* cls = ca::core::tag_cast::cast<mjbase::model::ClassResource>(resource.get());
        if (cls->get().empty()) continue;
        auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(cls->get()));
        if (sessionResult.is_err()) continue;
        LoadedClass entry;
        entry.session = std::move(sessionResult).unwrap();
        entry.name.assign(entry.session->backing().thisName());
        if (entry.name.empty()) continue;
        entry.uri      = cls->getLocation().to_std_string();
        entry.resource = cls;
        loaded.push_back(std::move(entry));
    }
    if (loaded.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 分桶是贪心序依赖的（撞名/方法数上限决定供体落哪个宿主），容器是
    // unordered_map——迭代序随标准库实现漂移（MSVC/libstdc++ 分桶策略不同）。
    // 固定按类内部名排序，输出跨平台逐位稳定。
    std::sort(loaded.begin(), loaded.end(),
              [](const LoadedClass& a, const LoadedClass& b) {
                  return a.name != b.name ? a.name < b.name : a.uri < b.uri;
              });

    std::vector<LoadedClass*> candidates;
    for (LoadedClass& entry : loaded) {
        if (entry.name.find(kMergedHostMarker) != std::string::npos) continue;
        if (isToolClassCandidate(*entry.session)) candidates.push_back(&entry);
    }
    if (candidates.size() < 2) return mjbase::spi::PreservedAnalyses::all();

    // 阶段一：分桶与搬移（全部在会话层暂存，容器不动——作业级原子）。
    std::vector<MergedHost>                 hosts;
    mjbase::asm_remapper::MemberRemapPlan   plan;
    std::vector<LoadedClass*>               donors;
    for (LoadedClass* candidate : candidates) {
        MergedHost* target = nullptr;
        for (MergedHost& host : hosts) {
            if (canMoveToolClassInto(*host.session, *candidate->session)) {
                target = &host;
                break;
            }
        }
        if (target == nullptr) {
            hosts.push_back(createMergedHost(hosts.size()));
            target = &hosts.back();
        }
        plan.mapClass(candidate->name, target->name);
        moveToolClassMembers(*candidate, *target);
        donors.push_back(candidate);
    }

    // 阶段二：owner 重映射（供体→宿主计划施加于宿主与全部幸存类；
    // remapper 面 bytecodeOnly，class_renamer 同款）。
    for (MergedHost& host : hosts) {
        auto remapped = mjbase::asm_remapper::applyMemberRemapPlan(*host.session, plan);
        if (remapped.is_err()) {
            throw std::runtime_error(std::string("ToolClassCombiner: 宿主重映射失败 (")
                                     + host.name + "): "
                                     + mjbase::asm_remapper::to_string(remapped.unwrap_err()));
        }
    }
    std::vector<PendingWrite> pending;
    for (LoadedClass& entry : loaded) {
        const bool isDonor = std::find(donors.begin(), donors.end(), &entry) != donors.end();
        if (isDonor) continue;
        auto remapped = mjbase::asm_remapper::applyMemberRemapPlan(*entry.session, plan);
        if (remapped.is_err()) {
            throw std::runtime_error(std::string("ToolClassCombiner: 幸存类重映射失败 (")
                                     + entry.name + "): "
                                     + mjbase::asm_remapper::to_string(remapped.unwrap_err()));
        }
        if (!remapped.unwrap()) continue;
        auto written = jbc::core::writeClass(entry.session->backing());
        if (written.is_err()) {
            throw std::runtime_error(std::string("ToolClassCombiner: 幸存类写回失败 (")
                                     + entry.name + ")");
        }
        PendingWrite item;
        item.uri   = entry.uri;
        item.bytes = std::move(written).unwrap();
        pending.push_back(std::move(item));
    }

    // 阶段三：宿主帧再生（分支体的 JVM verifier 义务）+ 宿主序列化。
    mjbase::KeyPool                           pool;
    mjbase::hierarchy::ClassHierarchy         hierarchy{pool};
    mjbase::spi::CommonSuperClassResolver     superResolver{hierarchy};
    mjbase::spi::StrictTypeResolverAdapter    typeResolver{superResolver};
    for (MergedHost& host : hosts) {
        regenerateHostFrames(host, pool, typeResolver);
        auto written = jbc::core::writeClass(host.session->backing());
        if (written.is_err()) {
            throw std::runtime_error(std::string("ToolClassCombiner: 宿主写回失败 (")
                                     + host.name + ")");
        }
        PendingWrite item;
        item.uri   = host.name + ".class";
        item.bytes = std::move(written).unwrap();
        pending.push_back(std::move(item));
    }

    // 阶段四：提交。全部序列化成功后一次动容器：先删全部供体条目，再以
    // 宿主名 URI 插入宿主（覆盖语义对齐旧栈 #962）。异常路径到此为止都
    // 未触碰容器——保守回滚由两阶段结构保证。
    auto& resources = container.getResources();
    for (const LoadedClass* donor : donors) {
        resources.erase(donor->uri);
    }
    for (PendingWrite& item : pending) {
        container.replaceResource(item.uri,
                                  std::make_unique<mjbase::model::ClassResource>(
                                      item.uri, std::move(item.bytes)));
    }

    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
