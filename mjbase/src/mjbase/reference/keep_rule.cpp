#include "mjbase/reference/keep_rule.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mjbase::reference {

const char* to_string(KeepRuleErrorCode code) noexcept
{
    switch (code) {
    case KeepRuleErrorCode::EmptyPattern: return "EmptyPattern";
    case KeepRuleErrorCode::UnknownKeyword: return "UnknownKeyword";
    case KeepRuleErrorCode::MissingArgument: return "MissingArgument";
    case KeepRuleErrorCode::MalformedMember: return "MalformedMember";
    case KeepRuleErrorCode::UnknownPreset: return "UnknownPreset";
    case KeepRuleErrorCode::UnknownTier: return "UnknownTier";
    }
    return "?";
}

const char* to_string(KeepPreset preset) noexcept
{
    switch (preset) {
    case KeepPreset::Serialization: return "serialization";
    case KeepPreset::Lambda: return "lambda";
    case KeepPreset::Rmi: return "rmi";
    case KeepPreset::Ejb: return "ejb";
    case KeepPreset::Jmx: return "jmx";
    case KeepPreset::Enum: return "enum";
    case KeepPreset::Swing: return "swing";
    case KeepPreset::Jaxb: return "jaxb";
    }
    return "?";
}

Result<KeepPreset, KeepRuleErrorCode> keepPresetFrom(std::string_view text)
{
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    // 穷举而非另建映射表：表只此一处，分家就会出现「能解析但不能展开」的洞。
    for (const KeepPreset preset : {KeepPreset::Serialization, KeepPreset::Lambda, KeepPreset::Rmi,
                                    KeepPreset::Ejb, KeepPreset::Jmx, KeepPreset::Enum,
                                    KeepPreset::Swing, KeepPreset::Jaxb}) {
        if (text == to_string(preset)) return Ok(preset);
    }
    return Err(KeepRuleErrorCode::UnknownPreset);
}

const char* to_string(KeepTier tier) noexcept
{
    switch (tier) {
    case KeepTier::None: return "none";
    case KeepTier::Public: return "public";
    case KeepTier::PublicProtected: return "public-protected";
    }
    return "?";
}

Result<KeepTier, KeepRuleErrorCode> keepTierFrom(std::string_view text)
{
    if (text.empty() || text == "none") return Ok(KeepTier::None);
    if (text == "public") return Ok(KeepTier::Public);
    if (text == "public-protected" || text == "public_protected") {
        return Ok(KeepTier::PublicProtected);
    }
    return Err(KeepRuleErrorCode::UnknownTier);
}

namespace {

/// glob 回溯匹配（语义见 keep_rule.hpp：单星不跨 '/'，双星跨，? 单字符）。
///
/// @param pattern 模式视图
/// @param p       当前模式位置
/// @param text    文本视图
/// @param t       当前文本位置
bool globMatchFrom(std::string_view pattern, std::size_t p, std::string_view text, std::size_t t)
{
    while (true) {
        if (p == pattern.size()) return t == text.size();
        const char c = pattern[p];
        if (c == '*') {
            const bool       doubleStar = (p + 1 < pattern.size()) && (pattern[p + 1] == '*');
            const std::size_t next      = doubleStar ? p + 2 : p + 1;
            // 双星可吞任意长（含 '/' 与空）；单星遇到 '/' 即止（不得跨包）。
            for (std::size_t k = t; k <= text.size(); ++k) {
                if (!doubleStar && k > t && text[k - 1] == '/') break;
                if (globMatchFrom(pattern, next, text, k)) return true;
            }
            return false;
        }
        if (t == text.size()) return false;
        if (c == '?') {
            if (text[t] == '/') return false;   // '?' 不跨 '/'
        }
        else if (c != text[t]) {
            return false;
        }
        ++p;
        ++t;
    }
}

/// 去首尾空白（空格/制表符；行级与段级 trim 用）。
std::string_view trimSpace(std::string_view s)
{
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t");
    return s.substr(first, last - first + 1);
}

/// 模式归一化：'.' → '/'（类名/描述符的源码形态直写）。
std::string normalizePattern(std::string_view raw)
{
    std::string out(raw);
    for (char& ch : out) {
        if (ch == '.') ch = '/';
    }
    return out;
}

/// 造一条 member 规则（name/desc 恒非空；调用方已校验）。
KeepRule makeMemberRule(std::string_view owner, std::string_view name, std::string_view desc,
                        std::string_view source)
{
    KeepRule rule;
    rule.kind    = KeepRuleKind::Member;
    rule.owner   = normalizePattern(owner);
    rule.name    = normalizePattern(name);
    rule.desc    = normalizePattern(desc);
    rule.source  = std::string(source);
    return rule;
}

/// 造一条 class 规则（预设里的类名模式；只有 owner 段）。
KeepRule makeClassRule(std::string_view owner, std::string_view source)
{
    KeepRule rule;
    rule.kind   = KeepRuleKind::Class;
    rule.owner  = normalizePattern(owner);
    rule.source = std::string(source);
    return rule;
}

/// member 行三段装配（rest 已 trim；形态非法返回 false）。
///
/// 描述符段从 '(' 起、到行尾整体作为 glob（JVMS `(args)ret`：返回类型在闭括号
/// 之后，不做「闭括号后只允许空白」的校验——那会误杀一切带返回类型的描述符）。
/// 仅要求 '(' 后有闭括号存在。
bool parseMemberRule(std::string_view rest, std::string_view sourceLine, KeepRule& out)
{
    const auto hash = rest.find('#');
    if (hash == std::string_view::npos) return false;

    const std::string_view ownerPart = trimSpace(rest.substr(0, hash));
    std::string_view       tail      = rest.substr(hash + 1);

    std::string_view namePart = tail;
    std::string_view descPart = "**";   // 省略括号 = 任意描述符
    const auto       paren    = tail.find('(');
    if (paren != std::string_view::npos) {
        if (tail.find(')', paren) == std::string_view::npos) return false;   // 无闭括号
        namePart = tail.substr(0, paren);
        descPart = tail.substr(paren);   // 含括号与返回类型的整体描述符 glob
    }
    namePart = trimSpace(namePart);
    descPart = trimSpace(descPart);

    if (ownerPart.empty() || namePart.empty()) return false;

    out = makeMemberRule(ownerPart, namePart, descPart, sourceLine);
    return true;
}

/// 预设展开：把 `rest`（`+name` 或裸 name）对应的模式规则追加到 out。
/// 名字解析单入口走 keepPresetFrom（穷举 KeepPreset 枚举），此处只按枚举
/// 分派展开规则——不另建文本映射，避免「能解析但不能展开」的洞。
/// 未知/空名字返回 false（调用方转 Err）。
bool appendPresetRules(std::string_view rest, std::vector<KeepRule>& out)
{
    if (!rest.empty() && rest.front() == '+') rest.remove_prefix(1);
    rest = trimSpace(rest);
    if (rest.empty()) return false;

    const auto preset = keepPresetFrom(rest);
    if (preset.is_err()) return false;

    switch (preset.unwrap()) {
    case KeepPreset::Serialization: {
        // 序列化回调族：精确描述符（框架按签名反射查找，差一字节即失配）。
        // serialVersionUID / serialPersistentFields 是字段：描述符任意（long
        // 与 ObjectStreamField[] 是常规形态但不强制——用户可自定义 SUID 类型否？
        // 不可，但跨编译器产出的描述符差异由 glob 兜住更稳）。
        static constexpr struct
        {
            std::string_view name;
            std::string_view desc;
        } kSerializationMembers[] = {
            {"readObject", "(Ljava/io/ObjectInputStream;)V"},
            {"writeObject", "(Ljava/io/ObjectOutputStream;)V"},
            {"readObjectNoData", "()V"},
            {"readExternal", "(Ljava/io/ObjectInput;)V"},
            {"writeExternal", "(Ljava/io/ObjectOutput;)V"},
            {"writeReplace", "()Ljava/lang/Object;"},
            {"readResolve", "()Ljava/lang/Object;"},
            {"serialVersionUID", "**"},
            {"serialPersistentFields", "[Ljava/io/ObjectStreamField;"},
        };
        for (const auto& m : kSerializationMembers) {
            out.push_back(makeMemberRule("**", m.name, m.desc, "preset +serialization"));
        }
        // 无参构造器：反序列化框架（ObjectInputStream 反射实例化）的入口面。
        out.push_back(makeMemberRule("**", "<init>", "()V", "preset +serialization"));
        return true;
    }
    case KeepPreset::Lambda:
        out.push_back(makeMemberRule("**", "lambda$*", "**", "preset +lambda"));
        out.push_back(makeMemberRule("**", "$deserializeLambda$*", "**", "preset +lambda"));
        return true;
    case KeepPreset::Rmi: {
        // RMI 骨架/存根类：rmic 与动态代理按 `<类名>_Stub` 命名约定反射加载，
        // 名字就是契约（无注解、无接口可依）。层级谓词（implements Remote /
        // Skeleton / Tie）留待层级事实接入后补——见 keep_rule.hpp 挂账。
        static constexpr std::string_view kRmiClasses[] = {
            "**_Stub",
            "**_Skel",
            "**_Tie",
        };
        for (const std::string_view pattern : kRmiClasses) {
            out.push_back(makeClassRule(pattern, "preset +rmi"));
        }
        // RemoteStub 子类构造器的容器注入入口（RemoteRef 由运行时传入）。
        out.push_back(
            makeMemberRule("**", "<init>", "(Ljava/rmi/server/RemoteRef;)V", "preset +rmi"));
        return true;
    }
    case KeepPreset::Ejb: {
        // EJB bean 生命周期回调：容器按名反射调用（无接口、无注解可依）。
        // `ejbCreate*` / `ejbFind*` / `ejbSelect*` / `ejbHome*` 是实体/会话 bean
        // 的 home 方法族，同样按名解析，故用后缀通配而非枚举。
        static constexpr std::string_view kEjbCallbacks[] = {
            "ejbCreate*", "ejbPostCreate*", "ejbFind*",      "ejbSelect*",
            "ejbHome*",   "ejbActivate",    "ejbPassivate",  "ejbRemove",
            "ejbLoad",    "ejbStore",       "setSessionContext",     "unsetSessionContext",
            "setEntityContext", "unsetEntityContext", "setMessageDrivenContext",
        };
        for (const std::string_view name : kEjbCallbacks) {
            out.push_back(makeMemberRule("**", name, "**", "preset +ejb"));
        }
        return true;
    }
    case KeepPreset::Jmx: {
        // JMX 标准 MBean 的命名约定：`<Something>MBean` / `<Something>MXBean`
        // 接口由 MBeanServer 按名发现（接口与实现的名字必须严格对应）。
        static constexpr std::string_view kJmxClasses[] = {
            "**MBean",
            "**MXBean",
        };
        for (const std::string_view pattern : kJmxClasses) {
            out.push_back(makeClassRule(pattern, "preset +jmx"));
        }
        return true;
    }
    case KeepPreset::Enum:
        // 枚举的编译器合成面：`values()` 与 `valueOf(String)` 的返回类型是枚举
        // 自身（owner 相关），故描述符用前缀 glob 钉住参数侧、放开返回侧；
        // `$VALUES` 是支撑 `values()` 的合成数组字段。
        out.push_back(makeMemberRule("**", "values", "()**", "preset +enum"));
        out.push_back(makeMemberRule("**", "valueOf", "(Ljava/lang/String;)**", "preset +enum"));
        out.push_back(makeMemberRule("**", "$VALUES", "**", "preset +enum"));
        return true;
    case KeepPreset::Swing:
        // 可插拔 L&F：UI 委托由 UIManager 反射按名实例化（`createUI` 静态工厂、
        // `getUI` 访问器），名字与签名都是契约。
        out.push_back(makeMemberRule("**", "createUI", "(Ljavax/swing/JComponent;)**",
                                     "preset +swing"));
        out.push_back(
            makeMemberRule("**", "getUI", "()Ljavax/swing/plaf/ComponentUI;", "preset +swing"));
        return true;
    case KeepPreset::Jaxb:
        // JAXB 代码生成物：ObjectFactory 由 JAXBContext 反射实例化。注解族
        // （@XmlType / @XmlRootElement 的 getter/setter 面）需注解读取，挂账。
        out.push_back(makeClassRule("**ObjectFactory", "preset +jaxb"));
        return true;
    }
    return false;
}

}   // namespace

bool globMatch(std::string_view pattern, std::string_view text)
{
    return globMatchFrom(pattern, 0, text, 0);
}

Result<KeepRuleSet, KeepRuleParseError> KeepRuleSet::parse(std::string_view text)
{
    KeepRuleSet set;
    ca::u32     lineNo = 0;
    std::size_t pos    = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        ++lineNo;
        pos = (end == text.size()) ? text.size() : end + 1;

        line = trimSpace(line);
        if (line.empty() || line.front() == '#') continue;   // 空行/注释行

        const auto       space   = line.find_first_of(" \t");
        const std::string_view keyword = (space == std::string_view::npos) ? line
                                                                           : line.substr(0, space);
        const std::string_view rest =
            (space == std::string_view::npos) ? std::string_view{} : trimSpace(line.substr(space));

        if (keyword == "class") {
            if (rest.empty()) {
                return Err(KeepRuleParseError{lineNo, KeepRuleErrorCode::MissingArgument});
            }
            KeepRule rule;
            rule.kind   = KeepRuleKind::Class;
            rule.owner  = normalizePattern(rest);
            rule.source = std::string(line);
            if (rule.owner.empty()) {
                return Err(KeepRuleParseError{lineNo, KeepRuleErrorCode::EmptyPattern});
            }
            set.rules_.push_back(std::move(rule));
            continue;
        }
        if (keyword == "member") {
            if (rest.empty()) {
                return Err(KeepRuleParseError{lineNo, KeepRuleErrorCode::MissingArgument});
            }
            KeepRule rule;
            if (!parseMemberRule(rest, line, rule)) {
                return Err(KeepRuleParseError{lineNo, KeepRuleErrorCode::MalformedMember});
            }
            set.rules_.push_back(std::move(rule));
            continue;
        }
        if (keyword == "preset") {
            if (rest.empty()) {
                return Err(KeepRuleParseError{lineNo, KeepRuleErrorCode::MissingArgument});
            }
            if (!appendPresetRules(rest, set.rules_)) {
                return Err(KeepRuleParseError{lineNo, KeepRuleErrorCode::UnknownPreset});
            }
            continue;
        }
        return Err(KeepRuleParseError{lineNo, KeepRuleErrorCode::UnknownKeyword});
    }
    return Ok(set);
}

Result<void, KeepRuleParseError> KeepRuleSet::addPreset(KeepPreset preset)
{
    const auto before = rules_.size();
    // 走名字而非另铺一条 switch：预设名的唯一真相源是 to_string，分家就会
    // 出现「枚举能加但 DSL 同名行报未知预设」的不一致。
    if (!appendPresetRules(to_string(preset), rules_)) {
        return Err(KeepRuleParseError{0, KeepRuleErrorCode::UnknownPreset});
    }
    // 已知预设的展开表恒非空；空展开即内部契约破坏，fail loud。
    if (rules_.size() == before) {
        return Err(KeepRuleParseError{0, KeepRuleErrorCode::UnknownPreset});
    }
    return Ok();
}

bool KeepRuleSet::matchesClass(std::string_view internalName) const
{
    for (const KeepRule& rule : rules_) {
        if (rule.kind != KeepRuleKind::Class) continue;
        if (globMatch(rule.owner, internalName)) return true;
    }
    return false;
}

bool KeepRuleSet::matchesMember(std::string_view owner, std::string_view name,
                                std::string_view desc) const
{
    for (const KeepRule& rule : rules_) {
        if (rule.kind != KeepRuleKind::Member) continue;
        if (!globMatch(rule.owner, owner)) continue;
        if (!globMatch(rule.name, name)) continue;
        if (!globMatch(rule.desc, desc)) continue;
        return true;
    }
    return false;
}

}   // namespace mjbase::reference
