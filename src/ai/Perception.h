// ===========================================================================
//  src/ai/Perception.h
//
//  感知系统。把「世界真实状态」转换成「AI 认为的状态」。
//
//  这是战术 AI 可信度的关键：AI 不能直接读取敌人的精确坐标，
//  否则玩家会感觉「这敌人开挂」。感知系统引入三个真实感要素：
//    1. 视野锥（FOV）——背后看不见
//    2. 视线遮挡（LoS）——墙后看不见
//    3. 感知延迟与记忆——看见后要过 reactionTime 才反应，
//       失去目标后仍会朝最后已知位置搜索一段时间
//
//  感知结果写入黑板，供行为树的条件节点查询。
// ===========================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ai/Blackboard.h"
#include "glm/vec2.hpp"

namespace TacticalAI::AI {

/// 实体 ID 的统一类型别名。
///
/// 黑板用 std::any 做类型擦除，键值的类型必须「写入方与读取方完全一致」，
/// 否则 has<T>() 会静默返回 false（type_index 不匹配，不报错、不抛异常）。
/// 把 ID 的底层类型收敛成一个别名，可以避免 int / uint32_t 这类
/// 隐蔽的不一致再次发生——写 `blackboard.set(kNearestEnemy, id)` 与读
/// `blackboard.has<EntityId>(kNearestEnemy)` 用的是同一个类型。
using EntityId = uint32_t;

/// 感知目标的快照。由世界层提供（敌方的可见属性）。
struct PerceptTarget {
    EntityId    id       = 0;
    glm::vec2   position{0.0f, 0.0f};
    float       radius   = 8.0f;      ///< 用于视线检测的碰撞半径
    float       threat   = 1.0f;      ///< 威胁权重 [0,1]，影响威胁评估
    bool        alive    = true;
};

/// 感知配置。集中所有「AI 有多强」的数值，便于统一调参。
struct PerceptionConfig {
    /// 视野距离（世界单位）。
    float viewDistance     = 480.0f;

    /// 视野半角（弧度）。总视野 = 2 * halfFovAngle。
    /// 90 度总视野（约 0.785 rad 半角）接近人类有效观察范围。
    float halfFovAngle     = 0.7853981f;

    /// 目标在背面时的最小察觉距离。即「贴脸也能发现背后的人」。
    float proximityRadius  = 60.0f;

    /// 感知反应延迟（秒）。看见目标后需持续可见这么久才被「确认」。
    /// 模拟人类反应时间，也避免 AI 在一帧内对闪现的目标做出反应。
    double reactionTime    = 0.25;

    /// 失去视线后，记忆保持时长（秒）。期间 AI 会朝最后已知位置搜索。
    double memoryDuration  = 6.0;

    /// 是否启用视线遮挡检测。关闭后 AI 可穿墙感知（用于调试对比）。
    bool  enableLineOfSight = true;

    /// 视线检测采样的步长（世界单位）。越小越精确，越慢。
    float lineOfSightStep  = 8.0f;
};

/// 视线遮挡查询接口。由游戏层实现（查询地形/掩体）。
///
/// 用回调而非直接依赖 World，是为了让 AI 层不反向依赖游戏层——
/// 这也让单元测试可以注入一个假的地形，无需构造整个 World。
using LineOfSightQuery = std::function<bool(glm::vec2 from, glm::vec2 to)>;

/// 单个目标的感知记录。区分「刚看到」与「已确认」是反应延迟的实现基础。
struct PerceptionRecord {
    PerceptTarget target;
    double  firstSeenTime  = 0.0;   ///< 首次进入视野的时间
    double  lastSeenTime   = 0.0;   ///< 最近一次可见的时间
    bool    confirmed      = false; ///< 是否已通过反应延迟，可被 AI 使用
    bool    visibleNow     = false; ///< 本步是否可见
    float   distance       = 0.0f;  ///< 最近一次可见时的距离
    glm::vec2 lastKnownPosition{0.0f, 0.0f};

    /// 可见累计时长。
    [[nodiscard]] double visibleDuration(double now) const noexcept {
        return visibleNow ? (now - firstSeenTime) : (lastSeenTime - firstSeenTime);
    }
};

/// 感知系统。
///
/// 每个 AI agent 持有自己的实例——共用会导致所有敌人共享「谁看见了谁」，
/// 也就失去了个体视野的意义。
class PerceptionSystem {
public:
    /// @param config 感知参数
    /// @param losQuery 视线遮挡查询；传空则视为无遮挡
    explicit PerceptionSystem(PerceptionConfig config = {},
                              LineOfSightQuery losQuery = nullptr);

    /// 替换视线遮挡查询。
    ///
    /// 【为什么要有这个 setter】
    /// 调用方（Enemy::setLineOfSightQuery）原先为了换查询函数而整体重建
    /// PerceptionSystem，那会连带丢掉 records_（最后已知位置、已确认目标、
    /// 反应计时）——表现为「换一次查询，AI 就失忆一次」。用 setter 换函数
    /// 即可保持运行时状态不变。
    void setLineOfSightQuery(LineOfSightQuery losQuery) { losQuery_ = std::move(losQuery); }

    /// 更新感知。每逻辑步调用一次。
    ///
    /// @param selfPosition 感知者位置
    /// @param selfFacing   感知者朝向（单位向量，无需归一化）
    /// @param targets     当前世界中的候选目标
    /// @param gameTime    当前游戏时间（秒）
    /// @return 本步可见且已确认的目标数量
    size_t update(glm::vec2 selfPosition,
                  glm::vec2 selfFacing,
                  const std::vector<PerceptTarget>& targets,
                  double gameTime);

    /// 把感知结果写入黑板。
    /// 写入的键见 Blackboard::Keys，供行为树条件节点读取。
    void writeToBlackboard(Blackboard& blackboard, glm::vec2 selfPosition, double gameTime) const;

    // -- 查询 ----------------------------------------------------------------

    /// 最接近的已确认目标。无目标时返回 nullptr。
    [[nodiscard]] const PerceptionRecord* nearestConfirmedTarget() const;

    /// 最接近的、本步可见的目标。
    [[nodiscard]] const PerceptionRecord* nearestVisibleTarget() const;

    /// 指定 ID 的记录。
    [[nodiscard]] const PerceptionRecord* recordFor(uint32_t targetId) const;

    /// 所有感知记录。
    [[nodiscard]] const std::vector<PerceptionRecord>& records() const noexcept { return records_; }

    /// 是否处于「有敌人」状态（用于战斗/搜索状态机切换）。
    [[nodiscard]] bool hasConfirmedTarget() const noexcept { return nearestConfirmedTarget() != nullptr; }

    /// 距上次看到任何敌人过了多久（秒）。
    [[nodiscard]] double timeSinceLastSeen(double gameTime) const noexcept;

    /// 最后已知敌人位置。从未见过时返回 nullopt。
    [[nodiscard]] std::optional<glm::vec2> lastKnownEnemyPosition() const;

    /// 威胁等级 [0,1]。综合目标数量、距离、威胁权重。
    /// 行为树用它决定「撤退」分支的触发阈值。
    [[nodiscard]] float threatLevel() const noexcept { return threatLevel_; }

    // -- 几何工具（静态，便于单元测试与复用） --------------------------------

    /// 判断目标是否在视野锥内（不考虑遮挡）。
    [[nodiscard]] static bool isInFieldOfView(glm::vec2 selfPosition, glm::vec2 selfFacing,
                                              glm::vec2 targetPosition, const PerceptionConfig& config);

    /// 计算从 self 指向 target 的、以 selfFacing 为参照的偏转角（弧度，[-pi, pi]）。
    [[nodiscard]] static float angleTo(glm::vec2 selfFacing, glm::vec2 toTarget);

    // -- 调试 ----------------------------------------------------------------

    [[nodiscard]] const PerceptionConfig& config() const noexcept { return config_; }
    PerceptionConfig& config() noexcept { return config_; }

    /// 本步内部执行了几次视线检测（性能诊断用）。
    [[nodiscard]] uint64_t lineOfSightChecks() const noexcept { return lineOfSightChecks_; }

    /// 最近一次 update 的耗时描述。
    [[nodiscard]] std::string debugSummary(double gameTime) const;

private:
    /// 带遮挡检测的可见性判定。
    [[nodiscard]] bool canSee(glm::vec2 selfPosition, glm::vec2 selfFacing,
                              const PerceptTarget& target) const;

    PerceptionConfig config_;
    LineOfSightQuery losQuery_;

    std::vector<PerceptionRecord> records_;

    float    threatLevel_       = 0.0f;
    /// 视线检测次数属于「诊断统计量」而非逻辑状态：canSee() 是 const
    /// 查询，但仍需累加这个计数器供性能剖析使用。用 mutable 明确表达
    /// 「该字段不参与对象逻辑可变性」的意图。
    mutable uint64_t lineOfSightChecks_ = 0;
    double   lastSeenAnyTime_   = -1000.0;
    bool     hasEverSeenEnemy_  = false;
    glm::vec2 lastKnownAnyPosition_{0.0f, 0.0f};
};

} // namespace TacticalAI::AI
