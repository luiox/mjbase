add_rules("plugin.compile_commands.autoupdate", {outputdir = "."})

set_project("mjbase")
set_version("0.0.1")
set_xmakever("2.9.3")
set_allowedplats("windows", "linux", "macosx")
set_defaultplat("windows")
set_defaultarchs("x64")

set_languages("c99", "cxx17")
add_rules("mode.debug", "mode.release")
set_warnings("all")

-- 全线静态 CRT（/MT，debug 档自动映射 /MTd）：静态库消费方（MCP server、注入型
-- agent）与宿主进程旧 CRT 混链是 LNK2038/AV 根因。xrepo 包按项目 runtime 自动
-- 以 MT 口味重装。Linux/macOS 不受此设置影响。
if is_plat("windows") then
    set_runtimes("MT")
end

-- 日志门面编译期级别=Trace：CA_LOG_DEBUG/TRACE 一律编入，可见性由 Logger
-- 运行期级别与各调用点既有门控负责。
add_defines("CA_COMPILE_LOG_LEVEL=0")

-- binary 的静态库链接闭包（add_links + linux 闭包组）。
-- GNU ld 单遍从左到右扫描归档，闭包列表必须消费库在前、被依赖库在后；
-- MSVC link.exe 对库顺序不敏感。声明序即闭包序（xmake 3.0.9 实证）。
function mjbase_binary_link_closure(...)
    local libs = {...}
    local unpack = table.unpack or unpack
    add_links(unpack(libs))
    if is_plat("linux") then
        add_linkgroups(unpack(libs))
    end
end

-- 禁用 RTTI：dynamic_cast 替换为 tag_cast / Polymorphic isa 家族。
if is_plat("windows") then
    add_cxflags("/GR-", {tools = "cl"})
    add_cxflags("/EHa", {tools = "cl"})
    add_cxflags("/wd4541", {tools = "cl"})
    -- 关掉 xmake 默认追加的 /EHsc：与 /EHa 同传会触发 MSVC D9025。
    set_exceptions("none")
else
    add_cxxflags("-fno-rtti")
end

if is_plat("windows") then
    set_toolchains("msvc")
end

if is_host("windows") then
    set_plat("windows")
    set_toolchains("msvc")
end

-- 构建开关：必须最先 include，后续用 has_config("with_xxx") 判断。
-- 命令行用下划线形式：xmake f --with_tests=y 等。
includes("xmake/options.lua")

if has_config("with_asan") then
    set_optimize("none")
    set_symbols("debug")
    if is_plat("windows") then
        add_cxflags("/fsanitize=address", {tools = "cl"})
        add_defines("_DISABLE_VECTOR_ANNOTATION", "_DISABLE_STRING_ANNOTATION")
        add_ldflags("/INCREMENTAL:NO", {tools = "link"})
    else
        add_cxflags("-fsanitize=address", "-fno-omit-frame-pointer")
        add_ldflags("-fsanitize=address")
    end
end

-- gtest：仅在启用测试时通过 xrepo 拉取。
if has_config("with_tests") then
    add_requires("gtest")
end

-- libarchive：model 的资源解包后端（tar/xz/gz/zst）。
-- 本地覆盖包（xmake/repo/packages/l/libarchive/xmake.lua）规避官方包在
-- Linux 漏传 POSIX_REGEX_LIB=NONE 导致的编译失败。
add_repositories("mjbase-repo xmake/repo")
add_requires("libarchive 3.8.8", {configs = {openssl3 = false}})
add_requires("zlib")

-- 中心包仓直连包（无仓内副本、无 phony 垫片，消费点直接 add_packages）。
add_repositories("luiox-repo https://github.com/luiox/luiox-repo.git")
-- libca 版本/模块双单源：顶层与全部嵌套路径一律归并到同一 (版本, 模块集)。
-- 家族包内声明的 libca 依赖无版本约束且是小模块超集，放任自解析会多出实例；
-- 消费方目标同名包去重后落到无 json 的实例时，链接期 LNK2019 ca::json。
-- 升级 libca 只改下面两行。0.0.15 = zip UTF-8 路径 + 解析器 locale 加固
-- （家族包 0.0.2 即 libca 0.0.14 对齐版，与其基线对齐消 skew）。
-- env 模块为 libjdk_finder 0.1.0 所需（ca::env 读环境变量面）。
LUIOX_LIBCA_VERSION = "0.0.15"
LUIOX_LIBCA_MODULES = "core,crypto,str,fs,env,io,json,net,http,process,opt,thread,log,zip,resources,i18n,test"
-- libjbc 走项目内联包定义（git 直连 + commit 锁）：luiox-repo 的发布版落后于
-- mjbase 需要的面（Module 族属性 typed 建模等）。内联定义不吃 requireconfs，
-- 其嵌套 libca 由定义内的 add_deps 钉到上面的单源版本，故两处必须同一字面量。
includes("xmake/packages/libjbc.lua")
add_requires("libca " .. LUIOX_LIBCA_VERSION, {configs = {modules = LUIOX_LIBCA_MODULES}})
add_requires("libjbc 0.4.15")
add_requires("libjstr 0.0.2")
add_requires("libjbytecode 0.0.2")
add_requires("libjzip 0.0.2")
add_requires("libgarena 0.0.2")
add_requires("libjclassfile 0.0.2")
add_requires("libjstd 0.0.1")
add_requires("libjdk_finder 0.1.0")
add_requires("libjblw 0.0.1")
add_requires("libjhasm 0.0.1")
-- 嵌套 libca 全路径钉定，缺一条就多一个实例。
add_requireconfs("libjbc.libca",                         {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjstr.libca",                        {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjbytecode.libca",                   {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjbytecode.libjstr.libca",           {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjbytecode.libgarena.libca",         {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjzip.libca",                        {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libgarena.libca",                      {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjclassfile.libca",                  {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjclassfile.libjstr.libca",          {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjstd.libca",                        {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjdk_finder.libca",                  {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjblw.libca",                        {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjblw.libjbytecode.libca",           {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjblw.libjbytecode.libjstr.libca",   {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjblw.libjbytecode.libgarena.libca", {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjhasm.libca",                       {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjhasm.libjblw.libca",               {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjhasm.libjbytecode.libca",          {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjhasm.libjbytecode.libjstr.libca",  {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjhasm.libjbytecode.libgarena.libca",{version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjhasm.libjblw.libjbytecode.libca",  {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
-- i18n/resources 构建规则内嵌（.lang add_files 规则，包安装产物不带规则）。
includes("xmake/libca_rules.lua")

includes("test_helper")
includes("mjbase")
