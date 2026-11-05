#include "mjbase/project/reachability_closure.hpp"

#include <algorithm>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "libca/crypto/sha256.hpp"

namespace mjbase::project {

/// @brief 闭包指纹算法版本字节（closure_batches.fp_version；算法变更即 bump，
///        旧行自证版本位——仿 method_versions 的 fp_version 模式）。
inline constexpr ca::u32 kReachabilityClosureFpVersion = 1;

namespace {

//-- JVM access 位（JVMS 表 4.1/4.5/4.6；hierarchy/summary 同口径）---------------

constexpr ca::u32 kAccPublic    = 0x0001;
constexpr ca::u32 kAccPrivate   = 0x0002;
constexpr ca::u32 kAccProtected = 0x0004;
constexpr ca::u32 kAccStatic    = 0x0008;
constexpr ca::u32 kAccAbstract  = 0x0400;

bool isVirtualAccess(ca::u32 access) { return (access & (kAccStatic | kAccPrivate)) == 0; }
bool isConcreteAccess(ca::u32 access) { return (access & kAccAbstract) == 0; }

AnalysisDbError invalidError(std::string message)
{
    AnalysisDbError error;
    error.kind    = AnalysisDbErrorKind::Invalid;
    error.message = ca::str::Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(message.data()), message.size());
    return error;
}

//-- 内存图：类图 + 方法域（id 全部来自 AnalysisDb 身份层，零文本节点）------------

struct MethodNode
{
    ca::i64 id      = 0;
    ca::i64 classId = -1;
    ca::u64 sig     = 0;   ///< (name, desc) 全局 intern 键（族合并键）
    ca::u32 access  = 0;
    // 谓词面（main / <clinit> 判定）的名与描述符 intern 反查表下标；文本由图
    // 持有（种子发现与 clinit 登记是唯一文本消费点，查询面不经过这里）。
    ca::u32 nameSlot = 0;
    ca::u32 descSlot = 0;
};

/// @brief (name, desc) 对 intern 成槽位下标；槽位表持有文本（唯一文本所有权）。
ca::u32 internSlot(std::unordered_map<std::string, ca::u32>& intern,
                   std::vector<std::string>& slots, const ca::str::Utf8StringRef& text)
{
    std::string key(static_cast<std::string_view>(text));
    auto it = intern.find(key);
    if (it != intern.end()) return it->second;
    const auto slot = static_cast<ca::u32>(slots.size());
    slots.push_back(std::move(key));
    intern.emplace(slots.back(), slot);
    return slot;
}

struct ClosureGraph
{
    std::vector<MethodNode>                           methods;
    std::unordered_map<ca::i64, ca::usize>            indexById;
    std::unordered_map<std::string, ca::i64>          classIdByName;
    std::unordered_map<ca::i64, ca::i64>              superOfClass;
    std::unordered_map<ca::i64, std::vector<ca::i64>> ifacesOfClass;
    std::unordered_map<ca::i64, ca::u32>              accessOfClass;
    std::unordered_map<ca::u64, ca::usize>            declByOwnerSig;
    /// (owner 类 → 该类声明的 (签名, 节点下标) 清单)：sibling default 冲突
    /// 并集的装载面（同签名 default 在共同实现者的直接接口组内全并）。
    std::unordered_map<ca::i64, std::vector<std::pair<ca::u64, ca::usize>>> declsOfOwner;
    std::unordered_map<ca::u64, std::vector<ca::i64>> familyByRoot;
    std::unordered_map<ca::i64, ca::i64>              clinitOfClass;
    std::unordered_map<ca::i64, std::vector<ca::i64>> adjMethod;
    std::unordered_map<ca::i64, std::vector<ca::i64>> classTriggers;
    std::vector<std::string>                          nameBySlot;
    std::vector<std::string>                          descBySlot;

    const std::string& nameOf(ca::usize i) const { return nameBySlot[methods[i].nameSlot]; }
    const std::string& descOf(ca::usize i) const { return descBySlot[methods[i].descSlot]; }
    ca::i64 classOfMember(ca::i64 memberId) const
    {
        auto it = indexById.find(memberId);
        return it == indexById.end() ? -1 : methods[it->second].classId;
    }

    /// @brief (owner 类, 签名) 复合成一个 map 键：OR 位打包，owner 与签名各占
    ///        32 位（装载期已校验两侧都不越 2^32，位域不重叠，零碰撞）。
    static ca::u64 ownerSig(ca::i64 cls, ca::u64 sig)
    {
        return (static_cast<ca::u64>(cls) << 32) | (sig & 0xffffffffu);
    }

    std::vector<ca::usize> uf;
    ca::usize              ufFind(ca::usize x)
    {
        while (uf[x] != x) {
            uf[x] = uf[uf[x]];
            x     = uf[x];
        }
        return x;
    }
    void ufUnion(ca::usize a, ca::usize b)
    {
        a = ufFind(a);
        b = ufFind(b);
        if (a != b) uf[b] = a;
    }

    void buildFamilies()
    {
        std::unordered_map<ca::usize, std::vector<ca::i64>> byRoot;
        for (ca::usize i = 0; i < methods.size(); ++i) byRoot[ufFind(i)].push_back(i);
        for (auto& [root, members] : byRoot) {
            std::vector<ca::i64> concrete;
            for (ca::usize i : members) {
                const MethodNode& m = methods[i];
                // 族内过滤：非 abstract、非 static、非 private、非 <init>/<clinit>
                //（构造器与初始化器不经虚分派命中；<clinit> 只走触发边，设计稿 §3）。
                const std::string& name = nameBySlot[m.nameSlot];
                if (name == "<init>" || name == "<clinit>") continue;
                if (isVirtualAccess(m.access) && isConcreteAccess(m.access))
                    concrete.push_back(m.id);
            }
            std::sort(concrete.begin(), concrete.end());
            concrete.erase(std::unique(concrete.begin(), concrete.end()), concrete.end());
            familyByRoot.emplace(static_cast<ca::u64>(root), std::move(concrete));
        }
    }

    static constexpr ca::usize kNoIndex = static_cast<ca::usize>(-1);

    ca::usize findDeclUpChain(ca::i64 cls, ca::u64 sig)
    {
        ca::i64 cur = cls;
        while (cur >= 0) {
            auto it = declByOwnerSig.find(ownerSig(cur, sig));
            if (it != declByOwnerSig.end()) return it->second;
            auto sup = superOfClass.find(cur);
            if (sup == superOfClass.end()) break;
            cur = sup->second;
        }
        return kNoIndex;
    }

    void collectIfaceDecls(ca::i64 iface, ca::u64 sig, std::vector<ca::usize>& out,
                           std::unordered_set<ca::i64>& visited)
    {
        if (!visited.insert(iface).second) return;
        auto it = declByOwnerSig.find(ownerSig(iface, sig));
        if (it != declByOwnerSig.end()) out.push_back(it->second);
        auto sup = ifacesOfClass.find(iface);
        if (sup != ifacesOfClass.end())
            for (ca::i64 parent : sup->second) collectIfaceDecls(parent, sig, out, visited);
    }

    /// @brief 虚/接口目标的保守目标集：链落解声明者的族；链不中走接口闭包并集。
    const std::vector<ca::i64>& targetsOf(ca::usize methodIndex)
    {
        const MethodNode& m   = methods[methodIndex];
        ca::usize         decl = findDeclUpChain(m.classId, m.sig);
        if (decl != kNoIndex) return familyByRoot[static_cast<ca::u64>(ufFind(decl))];
        merged_.clear();
        std::vector<ca::usize>      ifaceDecls;
        std::unordered_set<ca::i64> visited;
        if (m.classId >= 0) collectIfaceDecls(m.classId, m.sig, ifaceDecls, visited);
        for (ca::usize d : ifaceDecls) {
            const auto& fam = familyByRoot[static_cast<ca::u64>(ufFind(d))];
            merged_.insert(merged_.end(), fam.begin(), fam.end());
        }
        std::sort(merged_.begin(), merged_.end());
        merged_.erase(std::unique(merged_.begin(), merged_.end()), merged_.end());
        return merged_;
    }

    /// @brief 类的 `<clinit>` 触发链（本类 + super 链；环防御截断）。
    void clinitChain(ca::i64 cls, std::vector<ca::i64>& out)
    {
        ca::i64                     cur = cls;
        std::unordered_set<ca::i64> visited;
        while (cur >= 0 && visited.insert(cur).second) {
            auto it = clinitOfClass.find(cur);
            if (it != clinitOfClass.end()) out.push_back(it->second);
            auto sup = superOfClass.find(cur);
            if (sup == superOfClass.end()) break;
            cur = sup->second;
        }
    }

private:
    std::vector<ca::i64> merged_;
};

void sortDedupe(std::vector<ca::i64>& list)
{
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
}

void sortDedupeAdj(std::unordered_map<ca::i64, std::vector<ca::i64>>& adj)
{
    for (auto& [key, list] : adj) sortDedupe(list);
}

//-- 图装载：两遍（一遍建类名/id 映射，二遍填层次与方法域）------------------------

ca::Result<void, AnalysisDbError> loadGraph(AnalysisDb& db, ClosureGraph& g)
{
    auto classes = db.listClasses();
    if (classes.is_err()) return ca::Err(std::move(classes).unwrap_err());
    auto classRows = std::move(classes).unwrap();

    std::unordered_map<std::string, ca::u64> sigIntern;
    std::unordered_map<std::string, ca::u32> nameIntern;
    std::unordered_map<std::string, ca::u32> descIntern;
    auto internSig = [&](const ca::str::Utf8StringRef& name,
                         const ca::str::Utf8StringRef& desc) {
        std::string key(static_cast<std::string_view>(name));
        key.push_back('\0');
        key.append(static_cast<std::string_view>(desc));
        auto    it = sigIntern.find(key);
        if (it != sigIntern.end()) return it->second;
        ca::u64 id = static_cast<ca::u64>(sigIntern.size()) + 1;
        sigIntern.emplace(std::move(key), id);
        return id;
    };

    // 一遍：类名映射
    for (const DbClassInfo& cls : classRows) {
        if (cls.classId >= (ca::i64(1) << 32)) {
            return ca::Err(invalidError("class id exceeds closure graph packing range: " +
                                        std::to_string(cls.classId)));
        }
        g.classIdByName.emplace(cls.internalName.to_std_string(), cls.classId);
    }

    // 二遍：层次 + 方法域（类序 = listClasses 的 internal_name BINARY 确定序）
    for (const DbClassInfo& cls : classRows) {
        g.accessOfClass[cls.classId] = cls.access;
        g.superOfClass[cls.classId]  = -1;
        if (!cls.superName.is_empty()) {
            auto it = g.classIdByName.find(cls.superName.to_std_string());
            if (it != g.classIdByName.end()) g.superOfClass[cls.classId] = it->second;
        }
        for (const ca::str::Utf8String& iface : cls.interfaces) {
            auto it = g.classIdByName.find(iface.to_std_string());
            if (it != g.classIdByName.end()) g.ifacesOfClass[cls.classId].push_back(it->second);
        }
        auto members = db.listMembers(cls.internalName.ref());
        if (members.is_err()) return ca::Err(std::move(members).unwrap_err());
        for (const DbMemberInfo& m : std::move(members).unwrap()) {
            if (m.kind != DbMemberKind::Method) continue;
            if (sigIntern.size() >= (ca::u64(1) << 32)) {
                return ca::Err(invalidError("signature intern space exceeds closure graph "
                                            "packing range"));
            }
            MethodNode node;
            node.id       = m.memberId;
            node.classId  = cls.classId;
            node.sig      = internSig(m.name.ref(), m.descriptor.ref());
            node.nameSlot = internSlot(nameIntern, g.nameBySlot, m.name.ref());
            node.descSlot = internSlot(descIntern, g.descBySlot, m.descriptor.ref());
            node.access   = m.access;
            const ca::usize index = g.methods.size();
            g.indexById.emplace(m.memberId, index);
            g.declByOwnerSig[ClosureGraph::ownerSig(cls.classId, node.sig)] = index;
            g.declsOfOwner[cls.classId].emplace_back(node.sig, index);
            g.methods.push_back(node);
            if (g.nameOf(index) == "<clinit>")
                g.clinitOfClass[cls.classId] = m.memberId;
        }
    }
    for (auto& [classId, list] : g.ifacesOfClass) sortDedupe(list);
    return ca::Ok();
}

//-- 种子集 ----------------------------------------------------------------------

/// @brief 种子档位位词表（closure_batches.seed_mask）：options 的直接投影，
///        不依赖图装载——复用判定在装载前就要用（同图不同档位不可互复用）。
ca::u32 seedMaskOf(const ReachabilityClosureOptions& options)
{
    ca::u32 mask = 0;
    if (options.entryPoints) mask |= static_cast<ca::u32>(ClosureSeedMask::EntryPoints);
    if (options.keepRoots) mask |= static_cast<ca::u32>(ClosureSeedMask::KeepRoots);
    if (!options.explicitSeeds.empty())
        mask |= static_cast<ca::u32>(ClosureSeedMask::Explicit);
    return mask;
}

ca::Result<std::vector<ca::i64>, AnalysisDbError> discoverSeeds(
    const ClosureGraph& g, const ReachabilityClosureOptions& options)
{
    std::vector<ca::i64> seeds;
    auto                 push = [&](ca::i64 id) { seeds.push_back(id); };

    if (options.entryPoints) {
        for (ca::usize i = 0; i < g.methods.size(); ++i) {
            const MethodNode& m = g.methods[i];
            // isJavaMainMethod 谓词投影：public static void main(String[])，
            // 附加具体方法约束（抽象 main 不可执行）。
            if ((m.access & kAccStatic) != 0 && (m.access & kAccPublic) != 0 &&
                isConcreteAccess(m.access) && g.nameOf(i) == "main" &&
                g.descOf(i) == "([Ljava/lang/String;)V")
                push(m.id);
        }
    }
    if (options.keepRoots) {
        for (ca::usize i = 0; i < g.methods.size(); ++i) {
            const MethodNode& m = g.methods[i];
            if (!isConcreteAccess(m.access)) continue;
            if ((m.access & (kAccPublic | kAccProtected)) == 0) continue;
            auto clsAcc = g.accessOfClass.find(m.classId);
            if (clsAcc == g.accessOfClass.end()) continue;
            if ((clsAcc->second & kAccPublic) == 0) continue;   // public 类门槛（档位语义）
            push(m.id);
        }
    }
    if (!options.explicitSeeds.empty()) {
        for (ca::i64 id : options.explicitSeeds) {
            if (g.indexById.find(id) == g.indexById.end()) {
                return ca::Err(invalidError("explicit seed member is not a registered method: " +
                                            std::to_string(id)));
            }
            push(id);
        }
    }
    sortDedupe(seeds);
    return ca::Ok(std::move(seeds));
}

}   // namespace

ca::Result<std::vector<ca::u8>, AnalysisDbError> reachabilityClosureFingerprint(
    const AnalysisDb& db)
{
    auto payload = db.closureFingerprintPayload(kReachabilityClosureFpVersion);
    if (payload.is_err()) return ca::Err(std::move(payload).unwrap_err());
    auto bytes = std::move(payload).unwrap();
    unsigned char digest[32];
    ca::crypto::SHA256 hasher;
    hasher.add(bytes.data(), bytes.size());
    hasher.get_hash(digest);
    return ca::Ok(std::vector<ca::u8>(digest, digest + sizeof(digest)));
}

ca::Result<ReachabilityClosureStats, AnalysisDbError> computeAndStoreReachabilityClosure(
    AnalysisDb& db, ca::i64 producerId, const ReachabilityClosureOptions& options)
{
    ReachabilityClosureStats stats;
    if (!db.isOpen()) {
        return ca::Err(invalidError("database is not open"));
    }
    if (db.isReadOnly()) {
        AnalysisDbError e;
        e.kind    = AnalysisDbErrorKind::ReadOnly;
        e.message = ca::str::Utf8String::from_cstr(
            "cannot compute a reachability closure on a read-only database");
        return ca::Err(std::move(e));
    }

    //-- 种子档位（options 直接投影；全空快败）--------------------------------
    const ca::u32 optionsMask = seedMaskOf(options);
    if (optionsMask == 0) {
        return ca::Err(invalidError("reachability closure has no seeds"));
    }
    stats.seedMask = optionsMask;

    //-- 指纹复用判定（指纹 + 种子档位双匹配）----------------------------------
    auto fingerprint = reachabilityClosureFingerprint(db);
    if (fingerprint.is_err()) return ca::Err(std::move(fingerprint).unwrap_err());
    auto fpValue = std::move(fingerprint).unwrap();
    auto fresh   = db.freshClosureBatch(fpValue, optionsMask);
    if (fresh.is_err()) return ca::Err(std::move(fresh).unwrap_err());
    auto freshBatch = std::move(fresh).unwrap();
    if (freshBatch.has_value()) {
        stats.batchId     = freshBatch->batchId;
        stats.memberCount = freshBatch->memberCount;
        stats.reused      = true;
        return ca::Ok(std::move(stats));
    }

    //-- 图装载 ----------------------------------------------------------------
    ClosureGraph g;
    if (auto loaded = loadGraph(db, g); loaded.is_err())
        return ca::Err(std::move(loaded).unwrap_err());

    g.uf.resize(g.methods.size());
    for (ca::usize i = 0; i < g.uf.size(); ++i) g.uf[i] = i;
    for (ca::usize i = 0; i < g.methods.size(); ++i) {
        const MethodNode& m = g.methods[i];
        if (!isVirtualAccess(m.access)) continue;
        auto sup = g.superOfClass.find(m.classId);
        if (sup != g.superOfClass.end() && sup->second >= 0) {
            auto it = g.declByOwnerSig.find(ClosureGraph::ownerSig(sup->second, m.sig));
            if (it != g.declByOwnerSig.end()) g.ufUnion(i, it->second);
        }
        auto ifaces = g.ifacesOfClass.find(m.classId);
        if (ifaces != g.ifacesOfClass.end())
            for (ca::i64 iface : ifaces->second) {
                auto it = g.declByOwnerSig.find(ClosureGraph::ownerSig(iface, m.sig));
                if (it != g.declByOwnerSig.end()) g.ufUnion(i, it->second);
            }
    }
    // default 方法冲突（sibling 接口同签名声明）：共同实现者的直接接口组内
    // 全部并集，不做 most-specific 裁决（JVM IncompatibleClassChangeError
    // 场景宁过近似不丢真实执行边，设计稿 §3）。并集对合并顺序不敏感，最终
    // 分割确定（族成员表再排序去重），无确定性暴露。
    for (const auto& [classId, ifaces] : g.ifacesOfClass) {
        std::unordered_map<ca::u64, ca::usize> firstDeclBySig;
        for (ca::i64 iface : ifaces) {
            auto owner = g.declsOfOwner.find(iface);
            if (owner == g.declsOfOwner.end()) continue;
            for (const auto& [sig, declIndex] : owner->second) {
                auto slot = firstDeclBySig.find(sig);
                if (slot == firstDeclBySig.end()) firstDeclBySig.emplace(sig, declIndex);
                else g.ufUnion(slot->second, declIndex);
            }
        }
    }
    g.buildFamilies();

    //-- 种子 ------------------------------------------------------------------
    auto seeds = discoverSeeds(g, options);
    if (seeds.is_err()) return ca::Err(std::move(seeds).unwrap_err());
    auto seedList = std::move(seeds).unwrap();
    if (seedList.empty()) {
        return ca::Err(invalidError("reachability closure has no seeds"));
    }
    stats.seedCount = seedList.size();

    //-- 邻接快照 --------------------------------------------------------------
    auto edges = db.closureGraphEdges();
    if (edges.is_err()) return ca::Err(std::move(edges).unwrap_err());
    auto edgeRows = std::move(edges).unwrap();
    for (const auto& [src, dst] : edgeRows.methodEdges) g.adjMethod[src].push_back(dst);
    for (const auto& [src, cls] : edgeRows.classTriggers) g.classTriggers[src].push_back(cls);
    for (const auto& [src, cls] : edgeRows.textBranchClassTriggers) {
        g.classTriggers[src].push_back(cls);
        stats.textBranchClasses++;
    }
    sortDedupeAdj(g.adjMethod);
    sortDedupeAdj(g.classTriggers);

    //-- worklist 闭包（FIFO；种子升序、邻接升序、族展开升序 → 确定性）----------
    std::unordered_set<ca::i64> visited;
    std::unordered_set<ca::i64> triggeredClasses;
    std::deque<ca::i64>         queue;
    auto enqueue = [&](ca::i64 id) {
        if (id != 0 && visited.insert(id).second) queue.push_back(id);
    };
    auto triggerClass = [&](ca::i64 cls) {
        if (cls < 0 || !triggeredClasses.insert(cls).second) return;
        std::vector<ca::i64> chain;
        g.clinitChain(cls, chain);
        for (ca::i64 clinit : chain) {
            stats.clinitMarks++;
            enqueue(clinit);
        }
    };

    for (ca::i64 seed : seedList) {
        enqueue(seed);
        auto it = g.indexById.find(seed);
        if (it != g.indexById.end()) triggerClass(g.methods[it->second].classId);
    }

    while (!queue.empty()) {
        ca::i64 cur = queue.front();
        queue.pop_front();

        auto mit = g.adjMethod.find(cur);
        if (mit != g.adjMethod.end()) {
            for (ca::i64 target : mit->second) {
                enqueue(target);
                triggerClass(g.classOfMember(target));
                // refs 无 opcode：方法边一律保守族展开（静态/私有族=自身，语义不变）
                auto tit = g.indexById.find(target);
                if (tit != g.indexById.end()) {
                    const auto& targets = g.targetsOf(tit->second);
                    stats.familyExpansions++;
                    for (ca::i64 t : targets) enqueue(t);
                }
            }
        }
        auto cit = g.classTriggers.find(cur);
        if (cit != g.classTriggers.end())
            for (ca::i64 cls : cit->second) triggerClass(cls);
    }

    std::vector<ca::i64> members(visited.begin(), visited.end());
    sortDedupe(members);
    stats.memberCount = static_cast<ca::i64>(members.size());

    //-- 落库（类域作用域由 storeClosureBatch 从成员集派生）---------------------
    DbClosureBatchRecord record;
    record.producerId    = producerId;
    record.fingerprint   = fpValue;
    record.fpVersion     = kReachabilityClosureFpVersion;
    record.seedMask      = optionsMask;
    record.seedCount     = static_cast<ca::i64>(stats.seedCount);
    record.coverageMask  = static_cast<ca::u32>(ClosureCoverageMask::RefsDirectOnly);
    record.indyEdgeCount = 0;
    record.memberIds     = std::move(members);
    auto stored = db.storeClosureBatch(record);
    if (stored.is_err()) return ca::Err(std::move(stored).unwrap_err());
    stats.batchId = std::move(stored).unwrap().batchId;
    return ca::Ok(std::move(stats));
}

}   // namespace mjbase::project
