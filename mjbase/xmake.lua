-- mjbase：基于 libjbc 的跨类程序分析与变换库（mbase 的重写线，逐模块迁移）。
-- 边界（AGENTS.md / doc_ai/module-map.md）：libjbc 单类/单方法；mjbase 持
-- 跨类身份（keys）、层次/调用图与指令工程（asm 域）。旧 mbase 冻结对拍。
--
-- 迁移期约定：源文件与测试文件走 glob 收编（多 agent 并行落文件），
-- 模块稳定后按仓惯例转显式清单。

-- i18n 生成头回源：libca.i18n.embed-lang 将 .lang 文案编译为
-- i18n_mjbase_translations.generated.hpp（落于构建树 .gens），公开头
-- i18n.hpp 同目录直引。包导出只捕源树（add_headerfiles("src/(mjbase/**)")），
-- 故构建后把生成件按内容判等同步回 src/mjbase/，供安装面捕获；内容
-- 未变不落盘，避免 mtime 抖动触发消费方重编级联。
rule("mjbase.i18n_src_sync")
    after_build(function (target)
        import("core.project.config")
        local name = "i18n_mjbase_translations.generated.hpp"
        local gens = os.files(path.join(config.builddir(), "**", name))
        if not gens or #gens == 0 then
            raise("mjbase.i18n_src_sync: 构建树内未找到 " .. name)
        end
        local dst = path.join(os.scriptdir(), "src", "mjbase", name)
        local data = io.readfile(gens[1])
        if data ~= (os.isfile(dst) and io.readfile(dst) or nil) then
            io.writefile(dst, data)
            print("  > mjbase.i18n_src_sync: 同步 %s 至 src/mjbase/", name)
        end
    end)

target("mjbase")
    set_kind("static")
    add_rules("mjbase.i18n_src_sync")

    -- plugin 构建戳：git 提交哈希编译期注入 build_stamp.h（仅
    -- src/mjbase/plugin/build_stamp.cpp 消费），plugin 同源门据此在装配期
    -- 比对宿主与 pack 的 mjbase 构建来源。内容按提交变化，只重编消费它的
    -- TU；无 git 环境（发布包）落 "unknown"，同源两侧同为 unknown 仍等值。
    on_load(function (target)
        -- rev-parse 在有 .git 但无任何提交的仓（init 后未 commit）也会失败，
        -- os.iorunv 失败即 raise；沙箱没有 pcall/xpcall，只有 try{} DSL。
        local commit = "unknown"
        local gitdir = path.join(os.projectdir(), ".git")
        if os.isdir(gitdir) or os.isfile(gitdir) then
            try
            {
                function ()
                    local out = os.iorunv("git", {"rev-parse", "HEAD"})
                    if out then
                        commit = (out:gsub("%s+", ""))
                    end
                end
            }
        end
        local dir     = path.join(target:autogendir(), "plugin")
        local stamp   = path.join(dir, "build_stamp.h")
        local content = '#pragma once\n#define MJBASE_BUILD_COMMIT "' .. commit .. '"\n'
        if not os.isdir(dir) then
            os.mkdir(dir)
        end
        if not os.isfile(stamp) or io.readfile(stamp) ~= content then
            io.writefile(stamp, content)
        end
        target:add("includedirs", dir)
    end)

    -- jar/model/mapping/project 批次新增：zip 读写（libjzip）、资源解包后端
    -- （libarchive，与 mbase 同源）、fs/json/crypto（mapping 与 project 直迁）。
    -- B3-A 批次新增：Java 整数语义（libjstd，expr_value）与日志/i18n 门面
    -- （libca_log，log_init/triple）。libjstd/libca_i18n 为 headeronly，无链接产物。
    -- F 线清洗批新增：libjclassfile（敌意输入前置清洗级，§3.0 裁决；deps_gate
    -- R1 禁止集不含它，mjbase → libjclassfile 为合法边）。
    -- public：测试面直接 include jbc/... 与 libca/... 头，包 include 需传递。
    -- libjbytecode 已随 relaxed_verifier 换道 libjbc FrameEngine 摘除
    -- （B8 8.7）：0.0.x 时代经 libjhasm 传递，后改本仓直连，现无此边。
    -- libjstr 直连（jstr::utf8_to_mutf8，pass 两位消费方）——原经
    -- libjbytecode 传递带入，摘除后补正为显式声明。
    -- GNU ld 单遍扫描「消费库在前、被依赖库在后」：libjhasm 0.3.x 的
    -- jvm_compiler.o 回引 libjbc 的 attach/internDynamic 族，libjhasm 置前
    -- 其回引才能落到其后扫描的 libjbc（MSVC 多遍扫描不敏感，linux 红根因）。
    add_packages("libjhasm", "libjbc", "libjzip", "libca", "libjstd",
                 "libjclassfile", "libjstr", {public = true})
    add_packages("libarchive")
    -- 迁移切片：按批次收编模块（消费方驱动，逐批验证；忌整库一次搬运）。
    -- 见 doc_ai/module-map.md 的依赖图——批 1 只收零环的叶子与容器层，
    -- 字符串口径（util/mapping/jar 的标识符与路径面）在这一批定死。
    -- 后续批次（asm/hierarchy → reference/spi/pass/workspace → air/sir/jasm/cli）
    -- 按 mjt 与 mjt-py 的实际消费面追加，每批独立构建 + 独立测试。
    add_files("src/mjbase/keys.cpp", "src/mjbase/util.cpp")
    add_files("src/mjbase/expr/**.cpp")
    add_files("src/mjbase/jar/**.cpp")
    add_files("src/mjbase/mapping/**.cpp")
    add_files("src/mjbase/model/**.cpp")
    add_files("src/mjbase/project/**.cpp")
    -- 批 2：asm 指令工程域（零内依赖的六个子目录）+ hierarchy（只依赖 keys）。
    -- asm/classpath 只收 call_graph：resolved_call_graph 语义上属 reference
    -- （module-map 第五批），留在批 3 与 analysis/reference 一同落仓，
    -- 否则 asm → reference 的越层边会把环带进来。
    add_files("src/mjbase/asm/analyzer/**.cpp")
    add_files("src/mjbase/asm/cfg/**.cpp")
    add_files("src/mjbase/asm/insn/**.cpp")
    add_files("src/mjbase/asm/tmatcher/**.cpp")
    add_files("src/mjbase/asm/stmt/**.cpp")
    add_files("src/mjbase/asm/classpath/call_graph.cpp")
    add_files("src/mjbase/asm/classpath/indy.cpp")
    add_files("src/mjbase/hierarchy/**.cpp")
    -- 批 3：analysis / reference / spi / pass，连同上面推迟的 resolved_call_graph
    -- 与 remapper。本批含两个实锤环（asm⇄analysis⇄reference 余下部分、
    -- pass⇄spi），单 target 内可构建；拆 target 前必须先破环，见
    -- doc_ai/migration-boundary.md 的批次表。
    add_files("src/mjbase/analysis/**.cpp")
    add_files("src/mjbase/reference/**.cpp")
    add_files("src/mjbase/spi/**.cpp")
    add_files("src/mjbase/pass/**.cpp")
    add_files("src/mjbase/plugin/**.cpp")
    add_files("src/mjbase/asm/remapper/**.cpp")
    -- 批 4：mjt 与 mjt-py 的直接消费面——renamer、jasm、log、cli。
    -- （workspace 已整体退役移交下游，jasm 仍留本批供 cli/验证面消费；
    -- cli 只收源码（文案表 .lang 的规则在上方始终保留）。）
    add_files("src/mjbase/renamer/**.cpp")
    add_files("src/mjbase/jasm/**.cpp")
    add_files("src/mjbase/cli/*.cpp")
    -- cli 的 ArgParser 文案单源表（i18n_setup 静态注册，public include 供下游覆盖）。
    -- 批 1 不含 cli 源码，但 i18n.hpp 依赖其生成头，故规则始终保留。
    add_files("src/mjbase/cli/translations/*.lang", {rules = "libca.i18n.embed-lang"})
    -- 导出捕获含 mjbase/ 前缀（src/(mjbase/**)）：安装根下保留 mjbase/ 子树，
    -- 消费方 #include "mjbase/keys.hpp" 等伞头/模块头统一解析（同 libjbytecode
    -- src/(jbytecode/*.hpp) 口径）。
    add_headerfiles("src/(mjbase/**.hpp)")
    add_includedirs("src", {public = true})
    set_languages("cxx17")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
        add_links("bcrypt")
        -- xmllite：libarchive 静态库内含 xar 格式读取（依赖 CreateXmlReader），
        -- 链接 mjbase 的可执行需解析该符号。public 传递给下游（同 mbase）。
        add_syslinks("xmllite", {public = true})
    end

-- gtest 面随 with_tests 门控：默认配置不定义本 target、不拉 gtest。
if has_config("with_tests") then
target("mjbase_unittest")
    set_kind("binary")
    set_default(false)

    add_deps("mjbase", "test_helper")
    add_packages("libca")
    -- 显式闭包（consumer→dep 序）：xmake 3.x 不自动链静态依赖的传递闭包。
    -- libjhasm 0.3.0 后端换 libjbc：libjblw 出链；libjbytecode 五段闭包与
    -- 其独占传递的 libgarena 已随 relaxed_verifier 换道 libjbc 摘除
    -- （B8 8.7，garena 无人 include、无符号消费）。
    mjbase_binary_link_closure("mjbase", "libjhasm", "libjbc", "test_helper",
                               "libjdk_finder", "libjzip",
                               "libjclassfile", "libjstr")
    -- 注：libjstd / libca_i18n 为 headeronly，无链接产物，不进闭包。
    add_packages("gtest")
    add_files("test/unittest/main.cpp")
    -- 迁移期按切片收编：与上面 src 的同批次模块一一对应（忌整库 glob）。
    add_files("test/unittest/expr/*_test.cpp")
    add_files("test/unittest/jar/*_test.cpp")
    add_files("test/unittest/mapping/*_test.cpp")
    add_files("test/unittest/model/*_test.cpp")
    add_files("test/unittest/project/*_test.cpp")
    -- 批 2：与上面 src 的 asm 子目录、hierarchy 一一对应。
    add_files("test/unittest/asm/analyzer/*_test.cpp")
    add_files("test/unittest/asm/cfg/*_test.cpp")
    add_files("test/unittest/asm/classpath/*_test.cpp")
    add_files("test/unittest/asm/insn/*_test.cpp")
    add_files("test/unittest/asm/tmatcher/*_test.cpp")
    add_files("test/unittest/asm/stmt/*_test.cpp")
    add_files("test/unittest/hierarchy/*_test.cpp")
    -- 批 3：与上面 src 的 analysis/reference/spi/pass 及 remapper 一一对应。
    add_files("test/unittest/analysis/*_test.cpp")
    add_files("test/unittest/reference/*_test.cpp")
    add_files("test/unittest/spi/*_test.cpp")
    add_files("test/unittest/pass/*_test.cpp")
    -- plugin：pass-pack 插件接口（doc_ai/plugin-api.md）。
    add_files("test/unittest/plugin/*_test.cpp")
    -- golden 对拍子目录（旧栈 baseline 字节/指纹对拍机器，pass-golden-pipeline.md）。
    add_files("test/unittest/pass/golden/*_test.cpp")
    add_files("test/unittest/asm/remapper/*_test.cpp")
    -- 批 4：与上面 src 的 renamer/jasm/log/cli 一一对应（workspace 测试随模块退役）。
    add_files("test/unittest/renamer/*_test.cpp")
    add_files("test/unittest/jasm/*_test.cpp")
    add_files("test/unittest/log/*_test.cpp")
    add_files("test/unittest/cli/*_test.cpp")
    -- 第 5 批（孤儿测试救回）：keys/i18n/util 伞头与叶子件，
    -- 与 src 的 keys.hpp/i18n.hpp/util 一一对应（B3-A 波次测试面，副本删除前收容）。
    add_files("test/unittest/keys/*_test.cpp")
    add_files("test/unittest/i18n/*_test.cpp")
    add_files("test/unittest/util/*_test.cpp")
    add_includedirs("src")
    -- 共享测试夹具目录（test/unittest/shared/）：跨模块单测共用的头文件面，
    -- 消费形如 #include "shared/xxx.hpp"（structural_pass_util 升格共享 util）。
    add_includedirs("test/unittest")
    set_rundir("$(projectdir)")

    if is_plat("linux") then
        add_syslinks("pthread")
    end
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
end
