#pragma once

#include "jbc/core/class_file.hpp"

#include "mjbase/spi/pass.hpp"

namespace mjbase::spi {

class CfGlobalContext;
class CfClassContext;

/// 基于 libjbc core 的 class 文件级别 pass 基类。
///
/// 与指令级 pass（PassModel 家族，载体 jbc::ir::ClassSession/CodeUnit）并行，
/// 处理更原始的 class 文件层：载体是 jbc::core::ClassFile（低层 DOM，属性
/// lazy 解码、copy-through 写回），适合骨架级/属性级变换与只读预检。
/// 旧 CfClassPass 的载体（jclassfile ClassFile）换成 libjbc core 的同名面；
/// 休眠的 CfGlobalPass/CfMethodPass 旧生命周期模型不迁。
class CfPass
{
public:
    virtual ~CfPass() = default;

    virtual void doInitialization(CfGlobalContext& context) {}
    virtual void doFinalization(CfGlobalContext& context) {}
};

/// class 文件级 pass：在一个 jbc::core::ClassFile 上运行。
class CfClassPass : public CfPass
{
public:
    virtual ~CfClassPass()                                                           = default;
    virtual void run(jbc::core::ClassFile& classFile, CfClassContext& context) = 0;
};

/// classfile 级作业上下文（初始化/收尾与类间共享状态挂这里）。
class CfGlobalContext : public IPassContext
{
public:
    virtual ~CfGlobalContext() = default;
};

/// classfile 级单类上下文。
class CfClassContext : public IPassContext
{
public:
    virtual ~CfClassContext() = default;
};

}   // namespace mjbase::spi
