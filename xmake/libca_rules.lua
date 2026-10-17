-- libca 构建规则内嵌（来源：luiox/libca @ 1ab29dea，即 libca 包 0.0.1）。
-- libca.i18n.embed-lang 与 libca.resources.embed 两条 rule 原属 libca
-- submodule 的 i18n/resources 模块 xmake.lua；包安装只产头文件与库，
-- 不带规则，故在此原样内嵌供 mbase/mjt 的 add_files({rules=...}) 使用。
-- 同步要求：libca 升级时若规则变更，需同步更新本文件。

rule("libca.i18n.embed-lang")
    set_extensions(".lang")
    add_orders("libca.i18n.embed-lang", "c++.build.modules.builder")
    on_load(function (target)
        -- 生成头为 UTF-8（含 CJK 字面量），MSVC 必须显式按 UTF-8 编译。
        if is_plat("windows") then
            target:add("cxflags", "/utf-8", {tools = "cl"})
        end
    end)
    on_preparecmd_file(function (target, batchcmds, sourcefile, opt)
        local langdir = path.directory(sourcefile)
        local langfiles = os.files(path.join(langdir, "*.lang"))
        assert(langfiles and #langfiles > 0, "no .lang files beside " .. sourcefile)
        table.sort(langfiles)

        -- 解析全部 .lang：langs[文件] = {{key,value},...}
        local langs = {}
        local seen_zh = false
        for _, file in ipairs(langfiles) do
            local name = path.basename(file)
            assert(name:match("^[a-z]+_[A-Z][A-Za-z]*$"),
                   "locale file name must be xx_YY.lang, got: " .. name)
            if name == "zh_CN" then seen_zh = true end
            local entries = {}
            local used = {}
            for line in io.readfile(file):gmatch("[^\r\n]+") do
                line = line:match("^%s*(.-)%s*$")
                if #line > 0 and line:sub(1, 1) ~= "#" then
                    local eq = line:find("=", 1, true)
                    assert(eq, "malformed line (expected key=value): " .. line)
                    local key = line:sub(1, eq - 1)
                    local val = line:sub(eq + 1)
                    assert(key:match("^[a-z0-9_.]+$") and not key:match("%.%.") and
                               key:sub(1, 1) ~= "." and key:sub(-1) ~= ".",
                           "invalid i18n key: " .. key)
                    assert(used[key] == nil, "duplicate i18n key in " .. name .. ".lang: " .. key)
                    used[key] = true
                    table.insert(entries, {key = key, val = val})
                end
            end
            table.sort(entries, function (a, b) return a.key < b.key end)
            langs[name] = entries
        end
        assert(seen_zh, "zh_CN.lang is required (fallback language): " .. langdir)

        -- 完整性：其它语言 key 必须是 zh_CN 的子集。
        local zhkeys = {}
        for _, e in ipairs(langs["zh_CN"]) do zhkeys[e.key] = true end
        for name, entries in pairs(langs) do
            if name ~= "zh_CN" then
                for _, e in ipairs(entries) do
                    assert(zhkeys[e.key],
                           "key missing in zh_CN.lang (source of truth): " .. e.key ..
                           " (from " .. name .. ".lang)")
                end
            end
        end

        -- .lang 值内联转义：\n → 换行，\\ → 反斜杠，其余 \x → x。
        -- （多行文案靠 \n 承载，文件本身保持单行。）
        local function unescape(s)
            return (s:gsub("\\(.)", function (c)
                if c == "n" then return "\n" end
                return c
            end))
        end

        -- C++ 字符串字面量转义：\、" 与换行。
        local function cppstr(s)
            return '"' .. s:gsub('[\\"]', "\\%1"):gsub("\n", "\\n") .. '"'
        end

        -- 符号后缀：target 名中的非标识符字符归一为 _。
        local suffix = target:name():gsub("[^%w]", "_")

        local generated = {
            "// Generated from translations/*.lang by rule libca.i18n.embed-lang. Do not edit.",
            "#pragma once",
            "",
            "#include <cstddef>",
            "",
            '#include "libca/i18n/i18n.hpp"',
            "",
        }
        for _, file in ipairs(langfiles) do
            local name = path.basename(file)
            table.insert(generated,
                         "constexpr ca::i18n::Entry kI18nEntries_" .. suffix .. "_" .. name .. "[] = {")
            for _, e in ipairs(langs[name]) do
                table.insert(generated,
                             "    {" .. cppstr(e.key) .. ", " .. cppstr(unescape(e.val)) .. "},")
            end
            table.insert(generated, "};")
            table.insert(generated, "")
        end
        table.insert(generated,
                     "constexpr ca::i18n::LangTable kI18nLangTables_" .. suffix .. "[] = {")
        for _, file in ipairs(langfiles) do
            local name = path.basename(file)
            table.insert(generated,
                         "    {" .. cppstr(name) .. ", kI18nEntries_" .. suffix .. "_" .. name ..
                         ", sizeof(kI18nEntries_" .. suffix .. "_" .. name .. ") / sizeof(kI18nEntries_" ..
                         suffix .. "_" .. name .. "[0])},")
        end
        table.insert(generated, "};")
        table.insert(generated, "")
        table.insert(generated,
                     "constexpr ca::i18n::TableSet kI18nTableSet_" .. suffix ..
                     " = {kI18nLangTables_" .. suffix ..
                     ", sizeof(kI18nLangTables_" .. suffix .. ") / sizeof(kI18nLangTables_" ..
                     suffix .. "[0])};")
        table.insert(generated, "")

        local headerdir = path.join(target:autogendir(), "rules", "libca_i18n", "embed-lang")
        if not os.isdir(headerdir) then
            os.mkdir(headerdir)
        end
        target:add("includedirs", headerdir, {public = true})

        local headerfile =
            path.join(headerdir, "i18n_" .. suffix .. "_translations.generated.hpp")
        local content = table.concat(generated, "\n")
        local existing = os.isfile(headerfile) and io.readfile(headerfile) or nil
        if existing ~= content then
            io.writefile(headerfile, content)
        end
        batchcmds:add_depfiles(sourcefile)
        for _, file in ipairs(langfiles) do
            batchcmds:add_depfiles(file)
        end
        batchcmds:set_depmtime(os.mtime(headerfile))
        batchcmds:set_depcache(target:dependfile(headerfile))
    end)

-- header-only：注册表用 function-local static，静态库依赖无下游链接负担。

rule("libca.resources.embed")
    set_extensions(".resources")
    add_orders("libca.resources.embed", "c++.build.modules.builder")
    on_preparecmd_file(function (target, batchcmds, sourcefile, opt)
        local suffix = target:name():gsub("[^%w]", "_")

        -- 解析 资源清单:name=dir(相对清单目录)，边解析边校验保证确定性 与
        local manifestdir = path.directory(sourcefile)
        local byname = {}
        local mounts = {}
        for line in io.readfile(sourcefile):gmatch("[^\r\n]+") do
            line = line:match("^%s*(.-)%s*$")
            if #line > 0 and line:sub(1, 1) ~= "#" then
                local eq = line:find("=", 1, true)
                assert(eq, "resources manifest expects 'name=dir', got: " .. line)
                local name = line:sub(1, eq - 1):match("^%s*(.-)%s*$")
                local dir = line:sub(eq + 1):match("^%s*(.-)%s*$"):gsub("\\", "/")
                assert(name:match("^[a-z][a-z0-9_]*$"),
                       "resources mount name must match ^[a-z][a-z0-9_]*$: " .. tostring(name))
                assert(byname[name] == nil, "duplicate resources mount name: " .. name)
                if not path.is_absolute(dir) then
                    dir = path.join(manifestdir, dir)
                end
                assert(os.isdir(dir),
                       "resources mount dir does not exist (" .. name .. "): " .. dir)
                byname[name] = true
                table.insert(mounts, {name = name, dir = dir})
            end
        end
        assert(#mounts > 0, "resources manifest declares no mounts: " .. sourcefile)

        -- 单后端构建：target 侧可选挂载过滤（target 经 set_values 声明
        -- "resources.embed.only"，形如 "stackc,templates"）。未设置时行为不变
        -- （全量挂载照常嵌入）；被过滤挂载的文件不进生成头也不进 depfiles。
        local only_values = target:values("resources.embed.only")
        if only_values ~= nil and #only_values > 0 then
            -- xmake 各版本 values() 形态不一（string 或 table），归一为字符串。
            local only = type(only_values) == "table" and table.concat(only_values, ",")
                             or tostring(only_values)
            local wanted = {}
            for name in only:gmatch("[^,]+") do
                wanted[name] = true
            end
            local filtered = {}
            for _, mount in ipairs(mounts) do
                if wanted[mount.name] then
                    table.insert(filtered, mount)
                end
            end
            assert(#filtered > 0,
                   "resources.embed.only selects no declared mount: " ..
                   only .. " (" .. sourcefile .. ")")
            mounts = filtered
        end

        table.sort(mounts, function (a, b) return a.name < b.name end)

        -- 收集 空间全部挂载点的文件条目:第一遍先收集再剔除 目录
        -- 允许跨挂载同路径合法(bundle 隔离)；文件内容在第二遍读取。 与
        local MAX_FILE_BYTES = 1024 * 1024
        local depfiles = {sourcefile}
        local bundles = {}   -- {name=..., entries={{path,file,size},...}}
        for _, mount in ipairs(mounts) do
            local entries = {}
            for _, file in ipairs(os.filedirs(path.join(mount.dir, "**"))) do
                -- "**" 同时含子目录本身;子目录不是文件则跳过,自然剔除空目录
                if not os.isdir(file) then
                    local rel = path.relative(file, mount.dir):gsub("\\", "/")
                    table.insert(entries, {path = "/" .. rel, file = file})
                end
            end
            -- 按字节序排序,生成期定序,查询时二分(string_view 比较同语义)。
            table.sort(entries, function (a, b) return a.path < b.path end)
            for _, e in ipairs(entries) do
                local data = assert(io.readfile(e.file, {encoding = "binary"}),
                                    "cannot read resource file: " .. e.file)
                assert(#data <= MAX_FILE_BYTES,
                       "resource file exceeds 1 MiB constexpr limit (" .. #data ..
                       " bytes): " .. e.file ..
                       "\n(large files need a future obj-based embedding scheme)")
                e.size = #data
                table.insert(depfiles, e.file)
            end
            table.insert(depfiles, mount.dir)
            table.insert(bundles, {name = mount.name, entries = entries})
        end

        -- 生成 C++ 字符串字面量转义(\ 和 ")。
        local function cppstr(s)
            return '"' .. s:gsub('[\\"]', "\\%1") .. '"'
        end

        -- 输出 生成头:每挂载一个 数据区 + 条目表 + Bundle + Registrar 常量
        local out = {
            "// Generated by rule libca.resources.embed. Do not edit.",
            "#pragma once",
            "",
            "#include <libca/resources/resources.hpp>",
            "",
            "namespace ca::resources::gen {",
            "",
        }
        for _, bundle in ipairs(bundles) do
            local total = 0
            for _, e in ipairs(bundle.entries) do
                total = total + e.size
            end
            out[#out + 1] = "// 挂载 mount '" .. bundle.name .. "': " ..
                            #bundle.entries .. " file(s), " .. total .. " byte(s) 总量"
            if #bundle.entries > 0 then
                for idx, e in ipairs(bundle.entries) do
                    if e.size > 0 then
                        local sym = "kResData_" .. suffix .. "_" .. bundle.name .. "_" .. idx
                        local data = assert(io.readfile(e.file, {encoding = "binary"}),
                                            "cannot read resource file: " .. e.file)
                        out[#out + 1] = "inline constexpr unsigned char " .. sym .. "[] = {"
                        local row = {}
                        for i = 1, e.size do
                            row[#row + 1] = string.format("0x%02x", data:byte(i))
                            if #row == 32 then
                                out[#out + 1] = "    " .. table.concat(row, ", ") .. ","
                                row = {}
                            end
                        end
                        if #row > 0 then
                            out[#out + 1] = "    " .. table.concat(row, ", ") .. ","
                        end
                        out[#out + 1] = "};"
                    end
                end
                out[#out + 1] =
                    "inline constexpr ca::resources::RawEntry kResEntries_" .. suffix ..
                    "_" .. bundle.name .. "[] = {"
                for idx, e in ipairs(bundle.entries) do
                    if e.size > 0 then
                        local sym = "kResData_" .. suffix .. "_" .. bundle.name .. "_" .. idx
                        out[#out + 1] = "    {" .. cppstr(e.path) .. ", " .. sym ..
                                        ", sizeof(" .. sym .. ")},"
                    else
                        out[#out + 1] = "    {" .. cppstr(e.path) .. ", nullptr, 0},"
                    end
                end
                out[#out + 1] = "};"
                local esym = "kResEntries_" .. suffix .. "_" .. bundle.name
                out[#out + 1] =
                    "[[maybe_unused]] inline constexpr ca::resources::Bundle kResBundle_" ..
                    suffix .. "_" .. bundle.name .. "{" .. esym ..
                    ", sizeof(" .. esym .. ") / sizeof(" .. esym .. "[0])};"
            else
                out[#out + 1] =
                    "[[maybe_unused]] inline constexpr ca::resources::Bundle kResBundle_" ..
                    suffix .. "_" .. bundle.name .. "{nullptr, 0};"
            end
            out[#out + 1] =
                "[[maybe_unused]] inline const ca::resources::Registrar kResRegister_" ..
                suffix .. "_" .. bundle.name .. "{\"" .. bundle.name ..
                "\", kResBundle_" .. suffix .. "_" .. bundle.name .. "};"
            out[#out + 1] = ""
        end
        out[#out + 1] = "}   // namespace ca::resources::gen"
        out[#out + 1] = ""

        -- 输出 头文件(public includedir 同 libi18n 模式)+ 依赖清单 依赖
        -- 生成头是 UTF-8 路径字面量,MSVC 需显式按 UTF-8 编译。
        if is_plat("windows") then
            target:add("cxflags", "/utf-8", {tools = "cl"})
        end
        local headerdir = path.join(target:autogendir(), "rules", "libca_resources", "embed")
        if not os.isdir(headerdir) then
            os.mkdir(headerdir)
        end
        target:add("includedirs", headerdir, {public = true})

        local headerfile = path.join(headerdir, "resources_" .. suffix .. ".generated.hpp")
        local content = table.concat(out, "\n")
        local existing = os.isfile(headerfile) and io.readfile(headerfile) or nil
        if existing ~= content then
            io.writefile(headerfile, content)
        end
        batchcmds:add_depfiles(depfiles)
        batchcmds:set_depmtime(os.mtime(headerfile))
        batchcmds:set_depcache(target:dependfile(headerfile))
    end)

-- header-only：状态为 function-local static，静态初始化完成后各接口可并发调用。
