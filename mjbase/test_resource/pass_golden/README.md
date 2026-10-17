# pass_golden —— pass 迁移 golden 向量基线

旧 mbase 栈冻结提取的 golden 向量与其语料（迁移计划 §0 规则 2 / §5 第 3 关：
golden 必须在切换前自旧栈提取入库）。对拍消费方与管道说明见
[mjbase/doc_ai/pass-golden-pipeline.md](../doc_ai/pass-golden-pipeline.md)。

## 布局

```
corpus/tree/<语料名>.class        旧 jbytecode tree writer 规范形（sorter/tightener 道）
corpus/jcf/<语料名>.class         旧 jclassfile writer 规范形（cleaner 道）
corpus/um/<语料名>.class          未使用成员家族专用 mini 语料（um 道；容器另带
                                  services/Manifest 非类 keep 根，compare 侧重建）
golden/<Pass名>/<语料名>.class    旧栈跑该 pass 的输出字节
manifests/<Pass名>.manifest.txt   声明差异清单（默认零差异；格式见文件头注释）
```

- 语料是「旧栈 writer 规范形」：源类经旧栈恒等写回一次后冻结，是旧 writer 的
  不动点（提取器带 canonical(canonical(x))==canonical(x) 自检）。
- golden 生成自冻结的旧栈（mbase + libjbytecode + libjclassfile），与语料一样
  是基线文件；`test/` 下不留任何中间产物。

## 再生成（仅旧栈侧入口）

```sh
MJBASE_PASS_GOLDEN_REGEN=1 ./build/windows/x64/release/mbase_unittest.exe \
  --gtest_filter='PassGoldenExtract.*'
```

入口在 `mbase/unittest/pass/golden/pass_golden_extract_test.cpp`（默认配置零动
作）。再生成必须且只能在冻结的旧栈上做——旧栈退役后 golden 不可再补攒；常规
再生成应字节幂等（改动即差异，需过声明流程）。

## 加新 pass / 改语料

见 `mjbase/doc_ai/pass-golden-pipeline.md`；差异声明流程：对拍失败 → 归因
（pass 语义 vs writer 级）→ 本目录 manifests/ 补条目与理由 → 对拍测试按声明
类目改跑替代断言。
