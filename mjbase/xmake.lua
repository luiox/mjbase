-- mjbase：基于 libjbc 的跨类程序分析与变换库（mbase 的重写线，逐模块迁移）。
-- 边界（AGENTS.md / doc_ai/module-map.md）：libjbc 单类/单方法；mjbase 持
-- 跨类身份（keys）、层次/调用图与指令工程（asm 域）。旧 mbase 冻结对拍。
--
-- 迁移期约定：源文件与测试文件走 glob 收编（多 agent 并行落文件），
-- 模块稳定后按仓惯例转显式清单。

target("mjbase")
    set_kind("static")

    -- jar/model/mapping/project 批次新增：zip 读写（libjzip）、资源解包后端
    -- （libarchive，与 mbase 同源）、fs/json/crypto（mapping 与 project 直迁）。
    -- B3-A 批次新增：Java 整数语义（libjstd，expr_value）与日志/i18n 门面
    -- （libca_log，log_init/triple）。libjstd/libca_i18n 为 headeronly，无链接产物。
    -- F 线清洗批新增：libjclassfile（敌意输入前置清洗级，§3.0 裁决；deps_gate
    -- R1 禁止集不含它，mjbase → libjclassfile 为合法边）。
    -- public：测试面直接 include jbc/... 与 libca/... 头，包 include 需传递。
    add_packages("libjbc", "libjzip", "libca", "libjhasm", "libjstd",
                 "libjclassfile", {public = true})
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
    add_files("src/mjbase/consumer/**.cpp")
    -- cli 的 ArgParser 文案单源表（i18n_setup 静态注册，public include 供下游覆盖）。
    -- 批 1 不含 cli 源码，但 i18n.hpp 依赖其生成头，故规则始终保留。
    add_files("src/mjbase/cli/translations/*.lang", {rules = "libca.i18n.embed-lang"})
    add_headerfiles("src/mjbase/(**.hpp)")
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
    mjbase_binary_link_closure("mjbase", "libjbc", "test_helper", "libjdk_finder",
                               "libjzip", "libjhasm", "libjblw", "libjbytecode_util",
                               "libjbytecode_commons", "libjbytecode_analysis",
                               "libjbytecode_tree", "libjbytecode_core",
                               "libjclassfile", "libgarena", "libjstr")
    -- 注：libjstd / libca_i18n 为 headeronly，无链接产物，不进闭包。
    -- 注：libjhasm→libjblw→libjbytecode 是链接层传递依赖（jasm 文本面），
    -- mjbase 源码零 jbytecode include；旧栈退链归 P6 收口。
    add_packages("gtest")
    add_files("test/unittest/main.cpp")
    -- 迁移期按切片收编：与上面 src 的同批次模块一一对应（忌整库 glob）。
    add_files("test/unittest/expr/*_test.cpp")
    add_files("test/unittest/jar/*_test.cpp")
    add_files("test/unittest/mapping/*_test.cpp")
    add_files("test/unittest/model/*_test.cpp")
    add_files("test/unittest/project/*_test.cpp")
    add_files("test/unittest/consumer/*_test.cpp")
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
