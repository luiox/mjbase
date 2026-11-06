.. _upstream-annotation-recursion:

上游需求条目：libjbc attr_codec 注解族无界递归（栈溢出 / 进程击穿）
===================================================================

- **目标仓**\ ：luiox/libjbc（mjbase 锁定 0.4.22 = a408506。\ **2026-11-04 复核**\ ： 上游 main 头 d287c5a 的 ``src/jbc/core/attr_codec.cpp`` 仍无任何深度守卫——``readElemValue`` 前向声明 落在 :552，互递归体原样）
- **严重级**\ ：高（敌意/畸形类可击穿宿主进程；catch(...)/SEH 均不可恢复）
- **状态**\ ：待上游立项；mjbase 侧已落过渡守卫（见文末），上游修复合入后退役

现象
----

AAC 保护壳产物 ``me/konsolas/aac/oo.class``\ （最小单类 jar，样本在 E:/corpus-ws 工作区，不进仓）使 analysis-grunt 的 DeadMember 残留段错误：非 ASAN 进程 AV ``0xC0000005`` 落在 ``ntdll!RtlpAllocateHeap``\ （崩在「下一次分配」= 栈深异常后的 次生症状）；MSVC ASAN 下同路径报 ``stack-overflow``\ ，第一现场锁定在 libjbc ``decodeAttr`` 注解读取内。

根因
----

``src/jbc/core/attr_codec.cpp`` 的 ``readAnnotation`` ↔ ``readElemValue`` 互递归 （约 :552-:602）对元素值嵌套深度无上限。注解元素值 tag ``'@'``\ （嵌套注解）与 ``'['``\ （数组）逐层进入新栈帧对，深度只受属性载荷字节数约束——而属性载荷本身 可合法地有 21 KB（JVMS 对注解嵌套无上界）。

族内对照（防栈炸弹先例均在，唯 attr_codec 缺席）：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 解码面
     - 守卫
   * - ``jbc/type/signature.cpp``
     - ``kMaxNesting = 255``\ ，\ ``parseType(depth)`` 显式深度参数
   * - libjclassfile ``annotation_reader.hpp``
     - ``DepthGuard`` RAII + ``depth_`` 计数
   * - **libjbc ``attr_codec.cpp`` 注解族**
     - **无** ← 本条

结构证据（oo.class 实测，2026-11-04 全量复核）
----------------------------------------------

- **除注解炸点外全类结构合法**\ ：CP 395 项、fields 1 项（无属性）、methods 3 项（Code 属性体 8+code_len+异常表+嵌套属性恰好对齐 attr len）、类属性表 3 项（Signature / SourceFile / RuntimeVisibleAnnotations）全部干净。
- 实际炸点：类内 ``RuntimeVisibleAnnotations``\ ，载荷 off=48910 len=21006 （恰好吃满到文件尾 69916，无截断）：

  - ``num_annotations = 1``\ ，注解体单 pair，元素值 tag ``'@'`` 连嵌 **3000 层** （每层恰 7 字节：u2 element_name + 0x40 + u2 type_index + u2 num_pairs； 6 字节头 + 3000 × 7 = 21006 与载荷严丝合缝），最深层为 ``num_pairs = 0`` 的空注解收尾（无叶标量）；
  - ``readElemValue``/``readAnnotation`` 随之互递归 3000 层 × 每层一对栈帧 + ``ElemValue``/``Annotation`` 节点分配，1 MB 线程栈打穿。

- 对照面：\ ``readAttrSlots`` 对越界属性 length 的语义是\ **整表** ``Err(BadAttributeLength)``\ （attr_codec.cpp ``remaining() < length`` 分支）， 属骨架读取期边界预检——本样本不触该面；炸点全在 lazy 属性解码。

影响面（同族五处，全部经 readElemValue）
----------------------------------------

1. ``decodeAnnotations``——Runtime{Visible,Invisible}Annotations；
2. ``decodeParameterAnnotationsBody``——Runtime{Visible,Invisible}ParameterAnnotations；
3. ``decodeAnnotationDefaultBody``——AnnotationDefault；
4. ``decodeTypeAnnotations``\ （经 ``readTypeAnnotation``\ ）——Runtime{Visible, Invisible}TypeAnnotations；
5. 上述即 ``ClassFile::decodeAttr`` 全部注解族入口——任何消费方（mjbase member_remapper、analysis-grunt dead_member 注解普查等）对敌意类调 decodeAttr 即触发。

readClass/makeSession 本身不受影响（属性内容 lazy，骨架读取已过 ``readAttrSlots`` 边界校验）——炸点全在「触碰才解码」的 lazy 面，故扫描型 消费方（scanClass 只读骨架）不炸，解码型消费方炸。

建议修法
--------

镜像族内既有口径（二选一，倾向 a）：

a. **深度参数**\ （同 signature.cpp）：\ ``readAnnotation``/``readElemValue`` 加 ``ca::u32 depth``\ ，超 ``kMaxNesting = 255``\ （与 signature.cpp 同一常量，族内 单一口径）返回 ``nullptr``/``Err``\ 。错误通道建议 ``ReadError::UnsupportedFeature`` （不新增枚举值；「结构已知但语义不可接受」与既有语义兼容），Diagnostics 降级记录走 ``keepBodyTrailing`` 同款警告位即可。 b. **RAII DepthGuard**\ （同 libjclassfile）：Ard 上挂守卫对象。侵入面更小， 但 Ard 是值传递的紧凑读取器，需改为引用或挂栈帧计数，改动面不比 a 小。

修复注意：

- ``writeVType``/注解写回侧无递归（表驱动），无需对称改动；
- 回归测试载体建议字节夹具（\ ``@`` 连嵌 255/256 两档边界 + 合法浅嵌对照）， 形态同 mjbase ``mjbase/test/unittest/util/annotation_depth_test.cpp``\ （可 直接搬）；
- 附带检查：\ ``decodeBootstrapMethodsBody`` 等其余属性解码族均为表驱动循环， 无同类递归——本缺陷为点状，不成族。

mjbase 侧过渡守卫（已落地，上游修复合入后退役）
-------------------------------------------------

- ``mjbase/src/mjbase/util/annotation_depth.hpp``\ ：同一语法的\ **迭代**\ 走查 （显式栈），只在确证嵌套超限时拒绝；歧义/截断/未知 target_type 一律 fail-open（那些形态 libjbc Result 错误通道是优雅的）。
- 接线：\ ``member_remapper.cpp`` 的 ``decodedAttrOf``\ （全注解族解码收口）与 Code 嵌套属性解码位——预检拒绝按「解不开原样保真」跳过。
- 回归测试：\ ``mjbase/test/unittest/util/annotation_depth_test.cpp``\ （字节 夹具：五族深嵌拒绝、浅嵌放行 + 真解码、255/256 边界、截断 fail-open 与 libjbc Err 相干、敌意类 lazy 装载不炸）。
- 未接线：mjbase 其余散点 decodeAttr 调用位（meta_info_remover 的注解族 槽位等）未逐点接线守卫——上游修复是正解，守卫接线随退役条目一起清理。

复现件索引
----------

- 样本：\ ``E:/corpus-ws/aac-oo-repro.jar``\ （不进仓）；类文件结构证据见上文， 可由 ``mjbase/test/unittest/util/annotation_depth_test.cpp`` 的夹具形态 完全重建（无需样本）。
- **取证复跑（2026-11-04）**\ ：libjbc a408506 /tmp 克隆（xmake.lua 全局 追加 ``/fsanitize=address``\ ）+ libca 0.0.13 同法插桩 + 最小 harness（readClass → 找 RuntimeVisibleAnnotations 槽 → decodeAttr），MSVC 2022：

  - 非 ASAN 直跑：\ ``decodeAttr`` 内 **``0xC00000FD``\ （STATUS_STACK_OVERFLOW）**\ ， ``readClass`` 骨架装载无恙（lazy 面不炸的直证）；
  - ASAN 直跑：\ ``ERROR: AddressSanitizer: stack-overflow``\ （首发现场与递归炸点 一致；MSVC ASAN 对栈深极值帧栈展开为空属正常）；
  - 取证环境一次性未留仓（克隆即弃），复现步骤即本节文字。

