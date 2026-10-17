# mjbase

`mjbase` is a cross-class JVM bytecode analysis and transformation library built
on [libjbc](https://github.com/luiox/libjbc). It is the successor line of the
legacy `mbase` stack: keys (cross-class identity), class/method/field hierarchy,
call graphs and reference indexing, instruction engineering (the `asm` domain),
mapping and remapping, an AIR/SIR tree IR, and the pass pipeline that drives
them.

The split with libjbc is deliberate: **libjbc owns one class / one method**
(classfile DOM, CodeDom, CodeEditor, frame and dominator engines) while
**mjbase owns cross-class concerns**. mjbase never re-implements what libjbc
already provides, and never pushes global state back into libjbc.

## Build

Requires xmake 3.x and a C++17 compiler (MSVC on Windows, gcc/clang on Linux).

```sh
xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git
xmake f -p windows          # use -p linux on Linux
xmake build mjbase
```

Dependencies come from [luiox-repo](https://github.com/luiox/luiox-repo) and
xrepo: libca, libjbc, libjzip, libjstr, libgarena, libjclassfile, libjstd,
libjhasm, libjblw, libjbytecode (link-level transitive dependency of the JASM
text layer only), libarchive, zlib.

### Tests

```sh
xmake f --with_tests=y
xmake build mjbase_unittest
xmake run mjbase_unittest --gtest_brief=1
```

Some cases need a JDK and `GTEST_SKIP` when one is missing. Artifacts land in
the `test/` directory of the project root.

## Repository layout

```
mjbase/src/mjbase/     library sources, one directory per module
mjbase/test/           unit tests (gtest)
mjbase/test_resource/  static test inputs
mjbase/doc_ai/         design notes and the migration module map
mjbase/dev_plan.md     pending work only
test_helper/           test scaffolding (gtest integration, JDK fixtures)
```

Module status and migration provenance live in
[`mjbase/doc_ai/module-map.md`](mjbase/doc_ai/module-map.md).

## Conventions

- **No RTTI**: `dynamic_cast` / `typeid` are disabled; use the typed cast
  family
- **Types**: `ca::u32` / `ca::usize` / `Result<T, E>`; errors are `Result`
  values, never exceptions
- **Strings**: borrowed views are `std::string_view` over pool-resident text
  (no extra `std::string` storage)
- **Headers**: `"file.hpp"` within a module, `"mjbase/module/file.hpp"` across
  modules; `../` is forbidden
- **Comments**: Doxygen `///`; `/** */` is forbidden

See [mjbase/AGENTS.md](mjbase/AGENTS.md) for the full development conventions.

## License

[MIT](LICENSE)
