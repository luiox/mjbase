-- mjbase：基于 libjbc 的跨类程序分析与变换库。
-- 边界（AGENTS.md / doc:module-map）：libjbc 单类/单方法；mjbase 持
-- 跨类身份（keys）、层次/调用图与指令工程（asm 域）。
--
-- 源文件与测试文件按目录显式登记，不整库 glob：每加一层都要能独立构建 +
-- 独立测试。

target("mjbase")
    set_kind("static")

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

    -- zip 读写（libjzip）、fs/json/crypto（mapping 与 project 直用）。
    -- Java 整数语义（libjstd，expr_value）与日志/i18n 门面（libca_log，
    -- log_init/triple）。libjstd/libca_i18n 为 headeronly，无链接产物。
    -- libjclassfile 供敌意输入前置清洗级使用；deps_gate 禁止集不含它，
    -- mjbase → libjclassfile 是合法边。
    -- public：测试面直接 include jbc/... 与 libca/... 头，包 include 需传递。
    -- libjstr 直连而非靠依赖传递带入：pass 有两个消费方调用
    -- jstr::utf8_to_mutf8。
    -- GNU ld 单遍扫描「消费库在前、被依赖库在后」：libjhasm 0.3.x 的
    -- jvm_compiler.o 回引 libjbc 的 attach/internDynamic 族，libjhasm 置前
    -- 其回引才能落到其后扫描的 libjbc（MSVC 多遍扫描不敏感，linux 红根因）。
    add_packages("libjhasm", "libjbc", "libjzip", "libca", "libjstd",
                 "libjclassfile", "libjstr", {public = true})
    -- 分析数据库引擎：实现细节不对外暴露（analysis_db.hpp 不含 sqlite3.h）。
    add_packages("sqlite3")
    -- 先叶子与容器层（keys/util/expr/jar/mapping/model/project）：标识符、
    -- 内部名与路径的字符串口径在这一层定下，上层沿用。
    add_files("src/mjbase/keys.cpp", "src/mjbase/util.cpp")
    add_files("src/mjbase/expr/**.cpp")
    add_files("src/mjbase/jar/**.cpp")
    add_files("src/mjbase/mapping/**.cpp")
    add_files("src/mjbase/model/**.cpp")
    add_files("src/mjbase/project/**.cpp")
    -- asm 指令工程域（子目录之间零内依赖）+ hierarchy（只依赖 keys）。
    -- asm/classpath 只收 call_graph：resolved_call_graph 语义上属 reference，
    -- 与 analysis/reference 一同登记，否则 asm → reference 的越层边会把环带进来。
    add_files("src/mjbase/asm/analyzer/**.cpp")
    add_files("src/mjbase/asm/cfg/**.cpp")
    add_files("src/mjbase/asm/insn/**.cpp")
    add_files("src/mjbase/asm/sem/**.cpp")
    add_files("src/mjbase/asm/tmatcher/**.cpp")
    add_files("src/mjbase/asm/stmt/**.cpp")
    add_files("src/mjbase/asm/classpath/call_graph.cpp")
    add_files("src/mjbase/asm/classpath/indy.cpp")
    add_files("src/mjbase/hierarchy/**.cpp")
    -- analysis / reference / spi / pass / plugin / remapper。这层有两个实锤环
    -- （asm⇄analysis⇄reference、pass⇄spi），只能放进同一个 target 构建；
    -- 拆 target 前要先破环。
    add_files("src/mjbase/analysis/**.cpp")
    add_files("src/mjbase/reference/**.cpp")
    add_files("src/mjbase/spi/**.cpp")
    add_files("src/mjbase/pass/**.cpp")
    add_files("src/mjbase/plugin/**.cpp")
    add_files("src/mjbase/asm/remapper/**.cpp")
    -- mjt 与 mjt-py 的直接消费面。log 是 headeronly（log_init.hpp），无源文件。
    -- workspace/cli 不在本仓：workspace 引擎在下游 mjt，cli 的解析轮由
    -- libca ca::opt 承担、OutputSink 在 mjt。
    add_files("src/mjbase/renamer/**.cpp")
    add_files("src/mjbase/jasm/**.cpp")
    -- 导出捕获含 mjbase/ 前缀（src/(mjbase/**)）：安装根下保留 mjbase/ 子树，
    -- 消费方 #include "mjbase/keys.hpp" 等伞头/模块头统一解析。
    add_headerfiles("src/(mjbase/**.hpp)")
    add_includedirs("src", {public = true})
    set_languages("cxx17")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
        add_links("bcrypt")
    end

-- gtest 面随 with_tests 门控：默认配置不定义本 target、不拉 gtest。
if has_config("with_tests") then
target("mjbase_unittest")
    set_kind("binary")
    set_default(false)

    add_deps("mjbase", "test_helper")
    add_packages("libca")
    -- 引用计划回归测试需要裸 sqlite3 面（EXPLAIN QUERY PLAN / CHECK 行形状
    -- 直插断言）；SQL 仍只出现在 analysis_db.cpp，公开头不暴露 sqlite3。
    add_packages("sqlite3")
    -- 显式闭包（consumer→dep 序）：xmake 3.x 不自动链静态依赖的传递闭包。
    -- libjstd / libca_i18n 为 headeronly，无链接产物，不进闭包。
    mjbase_binary_link_closure("mjbase", "libjhasm", "libjbc", "test_helper",
                               "libjdk_finder", "libjzip",
                               "libjclassfile", "libjstr", "sqlite3")
    add_packages("gtest")
    add_files("test/unittest/main.cpp")
    -- 测试与 src 同口径：逐目录登记，不用整库 glob。
    add_files("test/unittest/expr/*_test.cpp")
    add_files("test/unittest/jar/*_test.cpp")
    add_files("test/unittest/mapping/*_test.cpp")
    add_files("test/unittest/model/*_test.cpp")
    add_files("test/unittest/project/*_test.cpp")
    add_files("test/unittest/asm/analyzer/*_test.cpp")
    add_files("test/unittest/asm/cfg/*_test.cpp")
    add_files("test/unittest/asm/classpath/*_test.cpp")
    add_files("test/unittest/asm/insn/*_test.cpp")
    add_files("test/unittest/asm/sem/*_test.cpp")
    add_files("test/unittest/asm/tmatcher/*_test.cpp")
    add_files("test/unittest/asm/stmt/*_test.cpp")
    add_files("test/unittest/hierarchy/*_test.cpp")
    add_files("test/unittest/analysis/*_test.cpp")
    add_files("test/unittest/reference/*_test.cpp")
    add_files("test/unittest/spi/*_test.cpp")
    add_files("test/unittest/pass/*_test.cpp")
    add_files("test/unittest/plugin/*_test.cpp")
    -- pass/golden 与旧栈 baseline 做字节/指纹对拍；子目录要单独一条 glob，
    -- 上面的 pass/*_test.cpp 不递归。
    add_files("test/unittest/pass/golden/*_test.cpp")
    add_files("test/unittest/asm/remapper/*_test.cpp")
    add_files("test/unittest/renamer/*_test.cpp")
    add_files("test/unittest/jasm/*_test.cpp")
    add_files("test/unittest/log/*_test.cpp")
    add_files("test/unittest/keys/*_test.cpp")
    add_files("test/unittest/util/*_test.cpp")
    add_includedirs("src")
    -- 跨模块单测共用的头放在 test/unittest/shared/，消费形如
    -- #include "shared/xxx.hpp"。
    add_includedirs("test/unittest")
    set_rundir("$(projectdir)")

    if is_plat("linux") then
        add_syslinks("pthread")
    end
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
end
