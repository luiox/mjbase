#!/usr/bin/env python3
"""代码引用面门禁：规范见 mjbase/AGENTS.md「文档规范」。

只认三类可校验事实，其余一律失败：

1. ``doc:<label>`` 必须能在 mjbase/doc/**.rst 里找到 ``.. _<label>:`` 声明；
   同理 ``:ref:``/``:doc:`` 目标在 RST 侧自检（label 唯一 + doc 文件在册）。
2. 代码面出现的 ``mjbase/doc/...rst`` 路径必须存在；``doc_ai/`` 是历史目录名，出现即失败。
3. 代码面不得引用设计内容形态的 ``.md``。白名单只有流程文档（AGENTS/dev_plan/
   README/CHANGELOG）；跨仓出处必须带仓名标记（libjbc / mbase / mjt / 私有仓），
   带标记即视为 prose，不做存在性校验。

用法：python mjbase/doc/check_refs.py [仓库根]
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# 代码面：源码、测试、构建脚本。人读文档（README/dev_plan）不在此列，
# 它们引用 .md 是允许的。
CODE_ROOTS = ("mjbase/src", "mjbase/test", "mjbase/xmake.lua", "xmake.lua", "xmake", "test_helper", "build_verify.sh")
CODE_SUFFIXES = {".hpp", ".cpp", ".h", ".c", ".lua", ".sh", ".py"}

# 流程文档：仓库级、稳定、允许被代码注释点名，但只校验存在性。
PROCESS_DOCS = {"AGENTS.md", "dev_plan.md", "README.md", "README.zh-CN.md", "CHANGELOG.md", "CONTRIBUTING.md", "SECURITY.md"}
# 跨仓出处的 prose 标记：命中则该行的 .md 提及不参与存在性校验。
UPSTREAM_MARKERS = ("libjbc", "mbase", "mjt", "luiox", "私有仓", "私仓")

DOC_DIRNAME = "mjbase/doc"
LABEL_DEF = re.compile(r"^\.\. _([\w.-]+):\s*$")
LABEL_REF = re.compile(r"doc:([a-z][a-z0-9]*(?:-[a-z0-9]+)*)")
RST_PATH = re.compile(r"(?:mjbase/)?doc/[\w./-]+\.rst")
LEGACY_PATH = re.compile(r"doc_ai")
MD_MENTION = re.compile(r"[\w./-]*[\w-]\.md\b")
REF_ROLE = re.compile(r":ref:`([^>`]+)`")
DOC_ROLE = re.compile(r":doc:`([^>`]+)`")
ANON_LINK = re.compile(r"`[^`]*\.(?:md|rst) <([^`>]+\.(?:md|rst))>`_{1,2}")


def code_files(root: Path):
    for spec in CODE_ROOTS:
        base = root / spec
        if base.is_file():
            yield base
        elif base.is_dir():
            for p in base.rglob("*"):
                if p.is_file() and (p.suffix in CODE_SUFFIXES or p.name in {"Dockerfile"}):
                    yield p


def iter_lines(path: Path):
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        text = path.read_text(encoding="utf-8", errors="replace")
    return enumerate(text.splitlines(), 1)


def collect_labels(doc_dir: Path):
    labels, files, dup = {}, set(), []
    for p in sorted(doc_dir.rglob("*.rst")):
        files.add(p.name)
        for lineno, line in iter_lines(p):
            m = LABEL_DEF.match(line)
            if m:
                name = m.group(1)
                if name in labels:
                    dup.append(f"{p.name}:{lineno}: label 重复定义 {name}（首见 {labels[name]}）")
                labels[name] = f"{p.name}:{lineno}"
    return labels, files, dup


def toctree_entries(index: Path):
    """index.rst 里 toctree  directive 的条目（去扩展名）。"""
    if not index.exists():
        return set()
    inside, entries = False, set()
    for line in index.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith(".. toctree::"):
            inside = True
            continue
        if inside:
            if stripped == "":
                continue
            if stripped.startswith(":"):
                continue
            if not line.startswith("   "):
                inside = False
                continue
            entries.add(stripped.split("#")[0].strip())
    return entries


def check(root: Path):
    problems = []
    doc_dir = root / DOC_DIRNAME
    if not doc_dir.is_dir():
        return [f"{DOC_DIRNAME}/ 不存在：目录已改名或迁移未落地"]
    labels, rst_files, dup = collect_labels(doc_dir)
    problems += dup
    in_tree = toctree_entries(doc_dir / "index.rst")

    # RST 侧：文档内链必须指向在册文件/已声明 label，匿名 md/rst 链接一律禁止。
    for p in sorted(doc_dir.rglob("*.rst")):
        if p.name == "index.rst":
            continue
        if p.stem not in in_tree:
            problems.append(f"{p.name}: 未挂进 index.rst 的 toctree（Sphinx -W 会报 orphan）")
    for p in sorted(doc_dir.rglob("*.rst")):
        for lineno, line in iter_lines(p):
            for m in LEGACY_PATH.finditer(line):
                problems.append(f"{p.name}:{lineno}: 历史目录名 doc_ai 仍在引用")
            for m in ANON_LINK.finditer(line):
                problems.append(f"{p.name}:{lineno}: 匿名文件链接 {m.group(1)}，改用 :ref:/:doc:")
            for m in MD_MENTION.finditer(line):
                if Path(m.group(0)).name in PROCESS_DOCS:
                    continue
                if any(marker in line for marker in UPSTREAM_MARKERS):
                    continue
                problems.append(
                    f"{p.name}:{lineno}: 文档点名 markdown 规格 {m.group(0)}——"
                    "本仓设计内容用 :ref:，跨仓出处加仓名标记"
                )
            for m in REF_ROLE.finditer(line):
                target = m.group(1).split("<")[-1].strip().rstrip(">, ")
                if target and target not in labels and not target.startswith("<"):
                    problems.append(f"{p.name}:{lineno}: :ref:`{target}` 无对应 label 声明")
            for m in DOC_ROLE.finditer(line):
                target = m.group(1).split("<")[-1].strip().rstrip(">, ")
                if target and Path(target).name not in rst_files and Path(target).stem not in {f[:-4] for f in rst_files}:
                    problems.append(f"{p.name}:{lineno}: :doc:`{target}` 指向不在册文件")

    # 代码面：只允许指向 RST 规格层的引用。
    for f in code_files(root):
        rel = f.relative_to(root).as_posix()
        for lineno, line in iter_lines(f):
            for m in LEGACY_PATH.finditer(line):
                problems.append(f"{rel}:{lineno}: 历史目录名 doc_ai 仍在引用")
            for m in LABEL_REF.finditer(line):
                if m.group(1) not in labels:
                    problems.append(f"{rel}:{lineno}: doc:{m.group(1)} 无对应 label 声明")
            for m in RST_PATH.finditer(line):
                rel_path = m.group(0)
                if not (root / "mjbase" / rel_path.replace("mjbase/", "", 1)).exists():
                    problems.append(f"{rel}:{lineno}: 文档路径不存在 {rel_path}")
            for m in MD_MENTION.finditer(line):
                name = m.group(0)
                if Path(name).name in PROCESS_DOCS:
                    continue
                if any(marker in line for marker in UPSTREAM_MARKERS):
                    continue
                problems.append(f"{rel}:{lineno}: 代码面引用 markdown 规格 {name}——设计内容转 {DOC_DIRNAME}/*.rst，或删指")

    return problems


def main(argv):
    root = Path(argv[1]).resolve() if len(argv) > 1 else Path(__file__).resolve().parents[2]
    problems = check(root)
    if problems:
        print(f"文档引用门禁失败：{len(problems)} 处")
        for p in problems:
            print("  " + p)
        return 1
    print("文档引用门禁通过")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
