.. _enum-name-restore-spec:

枚举名恢复映射生成件规格（EnumNameRestorer）
============================================

状态：已落地（v1 判据口径见「边界申报」节）。本件为 Recaf 侦察第三吸收项
（``EnumNameRestorationTransformer`` 的 mjbase 化），2026-11-07。

1. 动机与单职责
---------------

javac 生成枚举时，``<clinit>`` 必须以字符串 ldc 传入枚举常量真名
（``Enum.valueOf``/``Enum.name`` 的运行期语义依赖它），混淆器可以改名枚举
常量字段、支撑数组字段与支撑合成方法，却改不掉这些 ldc——它们构成可验证的
真名来源。本件扫容器内枚举类，按双形状判据把「被混淆名 → 泄漏真名」整理成
家族映射（``mjbase::mapping::IntermediateMapping``），挂在容器
AnalysisManager 上交付。

**单职责红线**：只产映射，不改名。字节码零改动（pass 恒返回
``PreservedAnalyses::all()``）；改名执行交映射消费面（renamer/remapper 家族）
组合，本件不含任何写回路径。

2. 判据（fail-closed）
----------------------

逐类独立判定，任一门失败该类零产出（不影响其他类）；畸形类资源跳过
（renamer 面 class_scan fail-open 同口径）。

2.1 形态锚
^^^^^^^^^^

类 access 带 ACC_ENUM（0x4000），且 ``values`` 方法签名 ``()[L<本类>;``、
``valueOf`` 方法签名 ``(Ljava/lang/String;)L<本类>;`` 同时在。缺一即跳过。

2.2 形状 A：构造点泄漏
^^^^^^^^^^^^^^^^^^^^^^

clinit 内重复出现的一组构造窗口（javap/jasm 亲验 javac 17 产物，
``--release 8`` 产物同形）：

.. code-block:: text

    NEW <本类>
    DUP
    LDC "<常量真名>"
    ICONST_<ordinal>            （0..5；更宽走 BIPUSH/SIPUSH/LDC Int）
    INVOKESPECIAL <本类>.<init> ((Ljava/lang/String;I…)V
    PUTSTATIC <本类>.<常量字段> L<本类>;

匹配规则：从 ldc 起四窗口（ldc String + 常量 ordinal + NEW/DUP 前锚 +
``<init>`` 描述符以 ``(Ljava/lang/String;I`` 开头），随后直线段到达
PUTSTATIC——直线段仅允许 nop 与 astore/aload 同槽中转对（Recaf 提到的旧
编译器/混淆器中转形态），步限 8；目标字段须为本类声明且 access 带 ACC_ENUM
位。窗口断裂的构造点丢弃（不判类失败，由互证兜底）。

2.3 形状 B：$values 数组
^^^^^^^^^^^^^^^^^^^^^^^^

clinit 内恰好一处 ``INVOKESTATIC <本类>.<数组方法> ()[L<本类>;``，随后直线
段（同上规则）到达 ``PUTSTATIC <本类>.<数组字段> [L<本类>;``；数组方法体内
``GETSTATIC <本类>.<常量字段> L<本类>;`` 的出现序即数组序：

.. code-block:: text

    ICONST_3
    ANEWARRAY <本类>
    DUP; ICONST_0; GETSTATIC <本类>.<常量0> L<本类>; AASTORE
    DUP; ICONST_1; GETSTATIC <本类>.<常量1> L<本类>; AASTORE
    DUP; ICONST_2; GETSTATIC <本类>.<常量2> L<本类>; AASTORE
    ARETURN

调用位名字为 ``values`` 的位置混入公有桥，或出现第二处匹配，均整类病态拒。形状 B
缺失（Java 8 时代 clinit 内联数组形状）整类拒。

2.4 双形状互证
^^^^^^^^^^^^^^

形状 A 收集的 ``(ordinal, 泄漏名, 字段名)`` 按 ordinal 升序排列，ordinal 必
须恰为 0..n-1 无重无缺；排列后的字段序与形状 B 的 getstatic 字段序逐元素相
等。序长不等或任一错位即整类拒绝——宁可漏恢复不可错恢复。这是本件相对
Recaf 参考实现的收紧点（Recaf 按单形状采信）。

2.5 名字口径（映射面最小化）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

泄漏真名必须是合法 Java 标识符（``IdentifierUtil::isValidJavaIdentifier``）；
被混淆名必须是**非法标识符**才产映射。合法名一律不动——含已与真名同名的
字段，也含 ``a``/``b``/``c`` 这类合法短名混淆（v1 有意收紧，见「边界申报」）。
数组字段恢复目标为 javac 规范名 ``$VALUES``，数组方法恢复目标为 ``$values``，
同样只在被混淆名为非法标识符且不等于规范名时产出。

3. 两形状 jasm 前后对照
-----------------------

混淆前（javac 产物语义等价 jasm，双常量示例）：

.. code-block:: text

    .class public super enum Color {
        .field public static final enum RED LColor;
        .field public static final enum GREEN LColor;
        .field private static final synthetic $VALUES [LColor;
        .method private static synthetic $values ()[LColor; {
            code: {
                iconst_2
                anewarray Color
                dup; iconst_0; getstatic Color.RED LColor;; aastore
                dup; iconst_1; getstatic Color.GREEN LColor;; aastore
                areturn
            }
        }
        .method static <clinit> ()V {
            code: {
                new Color; dup; ldc "RED";   iconst_0; invokespecial Color.<init>(Ljava/lang/String;I)V; putstatic Color.RED LColor;
                new Color; dup; ldc "GREEN"; iconst_1; invokespecial Color.<init>(Ljava/lang/String;I)V; putstatic Color.GREEN LColor;
                invokestatic Color.$values ()[LColor;; putstatic Color.$VALUES [LColor;
                return
            }
        }
    }

混淆后（混淆器改了字段/合成方法名，ldc 真名与指令序不动）：

.. code-block:: text

    .class public super enum Color {
        .field public static final enum a-b LColor;
        .field public static final enum c-d LColor;
        .field private static final synthetic A-B [LColor;
        .method private static synthetic sv-z ()[LColor; {
            code: {
                iconst_2
                anewarray Color
                dup; iconst_0; getstatic Color.a-b LColor;; aastore
                dup; iconst_1; getstatic Color.c-d LColor;; aastore
                areturn
            }
        }
        .method static <clinit> ()V {
            code: {
                new Color; dup; ldc "RED";   iconst_0; invokespecial Color.<init>(Ljava/lang/String;I)V; putstatic Color.a-b LColor;
                new Color; dup; ldc "GREEN"; iconst_1; invokespecial Color.<init>(Ljava/lang/String;I)V; putstatic Color.c-d LColor;
                invokestatic Color.sv-z ()[LColor;; putstatic Color.A-B [LColor;
                return
            }
        }
    }

4. 映射产物示例
---------------

上例混淆类经本件产出三条字段映射与一条方法映射（``IntermediateMapping``
条目，owner/desc/oldName/newName）：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 类别
     - owner
     - desc
     - oldName
     - newName
   * - Field
     - ``Color``
     - ``LColor;``
     - ``a-b``
     - ``RED``
   * - Field
     - ``Color``
     - ``LColor;``
     - ``c-d``
     - ``GREEN``
   * - Field
     - ``Color``
     - ``[LColor;``
     - ``A-B``
     - ``$VALUES``
   * - Method
     - ``Color``
     - ``()[LColor;``
     - ``sv-z``
     - ``$values``

5. 消费方式
-----------

映射挂在容器 AM 上：``GlobalAnalysisManager::getResult<EnumNameMappingAnalysis>
(container)``（惰性计算并缓存；pass 壳 ``EnumNameRestorer`` 注册进默认表后在
管线内触发，测试与外部消费方也可直接对容器 AM 调 ``getResult``）。

消费链：本映射是标准 ``IntermediateMapping``，可直接交
``mjbase::asm_remapper::MappingRemapper``（类名/成员名/描述符门面）与
``MemberRemapPlan``/``applyMemberRemapPlan``（改名执行面），或经
``MappingFormatRegistry`` 导出 Tiny-V2 等文本格式外流。编排建议次序：
先跑本件收映射 → 应用恢复 → 再进通用改名/清理链。jvm 测试
（``enum_name_restorer_jvm_test``）第 6 步即消费闭环实证：映射经 remapper
应用回混淆态后字段名复原。

6. 边界申报（v1）
-----------------

1. **仅认 $values 合成方法形状**：Java 8 时代 javac 在 clinit 内联数组分配
   （无 ``$values`` 方法）的形状不在本批，整类拒。
2. **合法短名混淆不恢复**：``a``/``b``/``c`` 这类合法标识符形态的混淆名不产
   映射（「合法名不动」口径，映射面最小化、fail-closed）；后续批如需放宽，
   扩展点在 2.5 节口径门（双形状互证证据已足以支撑全部 ≠ 真名口径）。
3. 带参枚举参数先于 ordinal 的窗口形状（``ldc <名>; ldc <参数>; …``）构造点
   窗口不成立，该点丢弃后互证不齐整类拒。
4. ordinal 断档 / 数组序错位 / 数组形状缺失 / 窗口断裂全部整类拒，无部分产出。

7. 测试面
---------

``test/unittest/pass/enum_name_restorer_test.cpp``：手拼夹具全矩阵（全形状
恢复、正名/合法短名两态不动、六路拒绝面、幂等二跑、注册挂名、字节零改动）。
``test/unittest/pass/enum_name_restorer_jvm_test.cpp``：javac 真产物硬验收
（正名零映射 → remapper 混淆态反演断言 → 映射经 remapper 应用复原字段名）。
