-- 集中定义本仓库的构建开关。
--
-- 命令行用下划线形式：xmake f --with_tests=y 等。
-- 根 xmake.lua 在其他 includes 之前 `includes("xmake/options.lua")`，
-- 之后用 has_config("with_xxx") 判断分支。

-- ---------------------------------------------------------------------------
-- 测试开关
-- ---------------------------------------------------------------------------

-- 启用单元测试 target（mjbase_unittest）。gtest 仅在开启时才被 add_requires。
option("with_tests")
    set_default(false)
    set_showmenu(true)
    set_description("Enable the mjbase_unittest target (pulls gtest via xrepo).")


-- AddressSanitizer。MSVC 下 /fsanitize=address 会改变 STL 的 ABI
-- （annotate_vector 等注解），因此**必须在全局作用域开启**，让所有 target
-- ——包括被依赖的静态库——用同一套插桩，否则链接期报 LNK2038 不匹配。
--
-- 注意两点：
--   1. Windows 的 ASan 只查越界/use-after-free 等，**不做泄漏检测**；
--      泄漏请用 CRT 调试堆（xmake f -m debug）。
--   2. gtest 走 xrepo 预编译、不会跟着插桩，故 ASan 构建不要链 gtest。
option("with_asan")
    set_default(false)
    set_showmenu(true)
    set_description("Build with AddressSanitizer (global; do not combine with gtest targets).")
