target("test_helper")
    set_kind("static")
    set_group("libs/test")
    -- 通用机制委托 ca::test；libjdk_finder 为 luiox-repo 直连包：
    -- jdk_fixture.hpp 经本目录 include 的消费方依赖其传递可见性。
    add_packages("libca", "libjdk_finder", {public = true})
    add_files("test_helper.cpp")
    add_includedirs(".", {public = true})
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
