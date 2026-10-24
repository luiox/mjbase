# pass_golden —— pass 对拍 golden 基线

## 布局

```
corpus/tree/<语料名>.class        tree writer 规范形（sorter/tightener 道）
corpus/jcf/<语料名>.class         jclassfile writer 规范形（cleaner 道）
corpus/um/<语料名>.class          未使用成员家族专用 mini 语料（um 道；容器另带
                                  services/Manifest 非类 keep 根，compare 侧重建）
golden/<Pass名>/<语料名>.class    跑该 pass 的输出字节
manifests/<Pass名>.manifest.txt   声明差异清单（默认零差异；格式见文件头注释）
```

语料是 writer 规范形：源类经恒等写回一次后冻结（提取器带
canonical(canonical(x))==canonical(x) 自检）。golden 与语料一样是基线文件，
`test/` 下不留任何中间产物。

## 加新 pass / 改语料

差异声明流程：对拍失败 → 归因（pass 语义 vs writer 级）→ 本目录
manifests/ 补条目与理由 → 对拍测试按声明类目改跑替代断言。
