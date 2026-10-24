#!/usr/bin/env bash
# mjbase 全量构建 + 单测（W0 样板批验证）
cd /d/WorkSpace/mjbase || exit 1
export XMAKE_TMPDIR=/d/WorkSpace/mjbase/build/_tmp
export TEMP="$XMAKE_TMPDIR"
export TMP="$XMAKE_TMPDIR"
mkdir -p "$XMAKE_TMPDIR"

xmake f --with_tests=y -m release -y || { echo CONFIG_FAIL; exit 1; }
xmake -y || { echo BUILD_FAIL; exit 1; }
# mjbase_unittest 是 set_default(false)：必须显式构建，否则跑到旧 exe。
xmake -y mjbase_unittest || { echo UT_BUILD_FAIL; exit 1; }
echo BUILD_OK

EXE=build/windows/x64/release/mjbase_unittest.exe
if [ ! -f "$EXE" ]; then
  EXE=$(ls build/*/x64/release/mjbase_unittest.exe 2>/dev/null | head -1)
fi
[ -f "$EXE" ] || { echo EXE_NOT_FOUND; exit 1; }

echo "=== 新增用例 ==="
"$EXE" --gtest_filter='StackOperationSimplifierTest.*' || { echo NEW_TESTS_FAIL; exit 1; }
echo "=== 全量回归 ==="
"$EXE" || { echo FULL_SUITE_FAIL; exit 1; }
echo ALL_GREEN
