// ===========================================================================
//  src/ai/Blackboard.h
//
//  黑板（Blackboard）。行为树节点之间共享状态的唯一通道。
//
//  为什么需要它：
//   行为树节点如果互相直接引用，树会变成网状依赖，无法复用也无法调试。
//   黑板把「数据」与「逻辑」分离——节点只声明它读写哪些键，
//   树结构因此可以自由重组，且调试面板能一次性看到 AI 的全部认知状态。
//
//  设计取舍：
//   - 用字符串键 + 类型擦除的 variant。牺牲一点性能换取可序列化与可枚举，
//     这对「AI 调试可视化」这个核心目标是值得的（性能敏感路径可加缓存键句柄）。
//   - 带版本号与脏标记，使调试 UI 只重绘变化的条目。
// ===========================================================================
#pragma once

#include <any>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "glm/vec2.hpp"

namespace TacticalAI::AI {

/// 键的句柄。预先解析字符串键可获得 O(1) 访问并避免重复哈希。
/// 长时间运行的 AI 应缓存句柄；调试代码直接传字符串即可。
class BlackboardKey {
public:
    BlackboardKey() = default;
    explicit BlackboardKey(std::string name) : name_(std::move(name)) {}

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] bool isValid() const noexcept { return !name_.empty(); }
    [[nodiscard]] uint64_t hash() const noexcept;

    friend bool operator==(const BlackboardKey& lhs, const BlackboardKey& rhs) noexcept {
        return lhs.name_ == rhs.name_;
    }

private:
    std::string name_;
};

/// 黑板条目。除值本身外还记录写入者与时间戳，
/// 这对调试「为什么 AI 做了这个决策」至关重要。
struct BlackboardEntry {
    std::any      value;
    std::type_index typeIndex{typeid(void)};
    uint64_t      version    = 0;    ///< 每次写入自增，UI 据此判断是否需要重绘
    double        lastWriteTime = 0.0; ///< 游戏时间
    std::string   lastWriter;        ///< 写入者名字（节点名 / 系统名）
    int           writeCount = 0;    ///< 累计写入次数，可发现每帧狂写的节点
};

/// 黑板。非线程安全——AI 层设计为单线程确定性仿真，
/// 若需并行评估请为每个 agent 独立实例化。
class Blackboard {
public:
    Blackboard() = default;

    // -- 写入 ----------------------------------------------------------------

    /// 传给 set() 的「不更新写入时间」哨兵值。
    static constexpr double kNoWriteTime = -1.0;

    /// 写入任意类型的值。返回该条目的新版本号。
    ///
    /// @param gameTime 游戏时间；传 kNoWriteTime（默认）表示不改动
    ///                 lastWriteTime，其余情况会写入该值。
    template <typename T>
    uint64_t set(std::string_view key, T&& value, std::string_view writer = "unknown",
                 double gameTime = kNoWriteTime) {
        BlackboardEntry& entry = entries_[std::string(key)];
        entry.value      = std::forward<T>(value);
        entry.typeIndex  = std::type_index(typeid(std::decay_t<T>));
        entry.lastWriter = std::string(writer);
        entry.writeCount++;
        entry.version++;

        // 【修复】lastWriteTime 此前是「声明了但永远为 0」的死字段：
        // set() 根本没有时间参数，任何依赖条目新鲜度的判断都会全部误判。
        if (gameTime != kNoWriteTime) {
            entry.lastWriteTime = gameTime;
        }

        if (entry.value.has_value()) {
            hasDirtyEntries_ = true;
        }
        ++totalWrites_;
        return entry.version;
    }

    /// 仅当键不存在时写入。用于「初始化默认值」而不覆盖已有状态。
    template <typename T>
    bool setIfAbsent(std::string_view key, T&& value, std::string_view writer = "unknown") {
        if (contains(key)) {
            return false;
        }
        set(key, std::forward<T>(value), writer);
        return true;
    }

    // -- 读取 ----------------------------------------------------------------

    /// 读取值。类型不符或键不存在时返回 nullopt（不抛异常，AI 逻辑不应被异常打断）。
    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view key) const {
        const auto it = entries_.find(std::string(key));
        if (it == entries_.end() || !it->second.value.has_value()) {
            return std::nullopt;
        }
        if (it->second.typeIndex != std::type_index(typeid(T))) {
            return std::nullopt;
        }
        try {
            return std::any_cast<T>(it->second.value);
        } catch (const std::bad_any_cast&) {
            return std::nullopt;
        }
    }

    /// 零拷贝读取：返回指向黑板内部值的指针，类型不符或键不存在时返回 nullptr。
    ///
    /// 用于 std::vector 这类大对象（如寻路结果 world.path）——
    /// get<T>() 按值返回，每次读取都会整份拷贝。
    template <typename T>
    [[nodiscard]] const T* getPtr(std::string_view key) const {
        const auto it = entries_.find(std::string(key));
        if (it == entries_.end() || !it->second.value.has_value()) {
            return nullptr;
        }
        if (it->second.typeIndex != std::type_index(typeid(T))) {
            return nullptr;
        }
        return std::any_cast<T>(&it->second.value);
    }

    /// 带默认值的读取——行为树条件节点的主力 API，避免到处写 .value_or()。
    template <typename T>
    [[nodiscard]] T getOr(std::string_view key, T fallback) const {
        return get<T>(key).value_or(std::move(fallback));
    }

    /// 只查询是否可读为指定类型，不拷贝值。
    template <typename T>
    [[nodiscard]] bool has(std::string_view key) const {
        const auto it = entries_.find(std::string(key));
        return it != entries_.end()
            && it->second.value.has_value()
            && it->second.typeIndex == std::type_index(typeid(T));
    }

    [[nodiscard]] bool contains(std::string_view key) const;
    [[nodiscard]] bool erase(std::string_view key);
    void clear();

    // -- 元信息 --------------------------------------------------------------

    /// 条目的版本号，0 表示不存在。用于 UI 脏检查。
    [[nodiscard]] uint64_t version(std::string_view key) const;
    [[nodiscard]] const BlackboardEntry* entry(std::string_view key) const;
    [[nodiscard]] size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] uint64_t totalWrites() const noexcept { return totalWrites_; }

    /// 当前是否存在未同步给 UI 的写入。
    [[nodiscard]] bool hasDirtyEntries() const noexcept { return hasDirtyEntries_; }
    void clearDirtyFlag() noexcept { hasDirtyEntries_ = false; }

    /// 所有键，按字典序排列（稳定输出便于比对两次运行的差异）。
    [[nodiscard]] std::vector<std::string> keys() const;

    /// 生成可读的状态转储，直接喂给调试面板。
    [[nodiscard]] std::string dump() const;

    /// 把值渲染成短文本。仅支持已注册的常见类型，其余显示类型名。
    [[nodiscard]] static std::string valueToString(const BlackboardEntry& entry);

    // -- 便捷接口：战术沙盒中最常用的键 ---------------------------------------
    // 集中定义避免字符串拼写错误（这类 bug 编译期无法发现）
    struct Keys {
        // 感知（Perception 模块写入）
        static constexpr const char* kVisibleEnemies      = "perception.visibleEnemies";
        static constexpr const char* kNearestEnemy        = "perception.nearestEnemy";
        static constexpr const char* kNearestEnemyDist    = "perception.nearestEnemyDistance";
        static constexpr const char* kLastKnownPosition   = "perception.lastKnownEnemyPosition";
        static constexpr const char* kThreatLevel         = "perception.threatLevel";
        static constexpr const char* kTimeSinceSeenEnemy  = "perception.timeSinceSeenEnemy";

        // 自状态（World 每步写入）
        static constexpr const char* kSelfPosition        = "self.position";
        static constexpr const char* kSelfFacing          = "self.facing";
        static constexpr const char* kSelfHealth          = "self.health";
        static constexpr const char* kSelfHealthRatio     = "self.healthRatio";
        static constexpr const char* kSelfAmmo            = "self.ammo";
        static constexpr const char* kSelfState           = "self.state";

        // 决策输出（行为树写入，移动系统读取）
        static constexpr const char* kMoveTarget          = "decision.moveTarget";
        static constexpr const char* kAimTarget           = "decision.aimTarget";
        static constexpr const char* kCurrentAction       = "decision.currentAction";
        static constexpr const char* kShouldFire          = "decision.shouldFire";

        // 寻路（Pathfinding 模块写入）
        static constexpr const char* kPath                = "path.points";
        static constexpr const char* kPathValid           = "path.isValid";
        static constexpr const char* kPathAge             = "path.age";
        static constexpr const char* kRepathRequested     = "path.repathRequested";

        // 小队情报（World 每步写入）
        //
        // 「某个队友最后看到目标在哪」。这不是实时坐标 —— 它带时间戳、
        // 会过期，且过期后会被清掉。共享它的意义是让没看见目标的敌人
        // 也有个方向去搜，而不是各自原地瞎转。
        static constexpr const char* kSquadIntelPosition  = "world.squadIntelPosition";
        static constexpr const char* kSquadIntelTime      = "world.squadIntelTime";
    };

private:
    std::unordered_map<std::string, BlackboardEntry> entries_;
    uint64_t totalWrites_   = 0;
    bool     hasDirtyEntries_ = false;
};

} // namespace TacticalAI::AI
