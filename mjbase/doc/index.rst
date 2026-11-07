mjbase 设计规格
===============

跨模块设计、数据模型与格式契约的权威文本。微观契约占位符与参数级说明在代码
``///`` 注释里（Doxygen 层），本目录只管「为什么这样设计」与「契约是什么」。
分层规则见 ``mjbase/AGENTS.md`` 的「文档规范」一节。

代码注释以 ``doc:<label>`` 形式指向本目录的具体小节；``check_refs.py`` 校验这些
指向不会腐烂。下列文档的标题即为该文档的根 label。

.. toctree::
   :maxdepth: 2
   :caption: 设计与契约

   analysis-database
   refs-id-design
   semantic-fp-spec
   library-identification
   member-census-design
   analysis-project-schema-v2
   analysis-cache-and-container-stack
   reachability-closure-design
   method-inliner
   stack-op-expander
   pass-contract
   pass-catalog
   plugin-api
   upstream-libjbc-annotation-recursion
   module-map
