#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_model.hpp"

namespace mjbase::spi {

/// @brief 作业级两阶段提交：global pass 循环内逐 class 序列化到 pending，
///        全部成功后统一 commit（resource->set）。
///
/// 与 GlobalToClassPassAdaptor 的原子性契约对齐：第 N 类序列化抛出时前
/// N-1 类尚未写回，整个作业保持原样，随后的导出/落盘不会持久化半成品。
/// 直写型 global pass（逐类 resource->set）在任意类失败时会把前面的
/// 修改留在容器里，应改用本工具。
class ClassResourcePendingCommit
{
public:
    /// @brief 暂存一个类的新字节（不写回）。同一资源重复 stage 以最后一次为准。
    void stage(mjbase::model::ClassResource* resource, std::vector<ca::u8> bytes)
    {
        for (auto& [res, pending] : pending_) {
            if (res == resource) {
                pending = std::move(bytes);
                return;
            }
        }
        pending_.emplace_back(resource, std::move(bytes));
    }

    /// @brief 阶段二：全部成功后统一写回。
    void commit()
    {
        for (auto& [resource, bytes] : pending_) {
            resource->set(std::move(bytes));
        }
        pending_.clear();
    }

    [[nodiscard]] bool        empty() const noexcept { return pending_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return pending_.size(); }

private:
    std::vector<std::pair<mjbase::model::ClassResource*, std::vector<ca::u8>>> pending_;
};

}   // namespace mjbase::spi
