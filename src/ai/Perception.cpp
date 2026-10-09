// ===========================================================================
//  src/ai/Perception.cpp
// ===========================================================================
#include "ai/Perception.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>

namespace TacticalAI::AI {
namespace {

constexpr float kPi = 3.14159265358979323846f;

/// 把角度规约到 [-pi, pi]。
inline float wrapAngle(float angle) noexcept {
    while (angle >  kPi) { angle -= 2.0f * kPi; }
    while (angle < -kPi) { angle += 2.0f * kPi; }
    return angle;
}

inline float length(glm::vec2 v) noexcept {
    return std::sqrt(v.x * v.x + v.y * v.y);
}

/// 安全归一化：零向量返回 (1,0)，避免产生 NaN 污染后续计算。
inline glm::vec2 normalize(glm::vec2 v) noexcept {
    const float len = length(v);
    if (len < 1e-6f) {
        return {1.0f, 0.0f};
    }
    return {v.x / len, v.y / len};
}

} // namespace

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
PerceptionSystem::PerceptionSystem(PerceptionConfig config, LineOfSightQuery losQuery)
    : config_(config), losQuery_(std::move(losQuery)) {
    // 视野半角钳制到 (0, pi]，防止配置成 0 或负数导致永远看不见
    config_.halfFovAngle = std::clamp(config_.halfFovAngle, 0.01f, kPi);
    config_.viewDistance = std::max(config_.viewDistance, 1.0f);
    config_.lineOfSightStep = std::max(config_.lineOfSightStep, 0.5f);
}

// ---------------------------------------------------------------------------
// 几何工具
// ---------------------------------------------------------------------------
float PerceptionSystem::angleTo(glm::vec2 selfFacing, glm::vec2 toTarget) {
    const glm::vec2 facing = normalize(selfFacing);
    const glm::vec2 target = normalize(toTarget);

    // 用 atan2(叉积, 点积) 求带符号夹角。
    // 相比 acos(点积) 的优势：不会在夹角接近 0 或 pi 时因浮点误差丢精度。
    const float cross = facing.x * target.y - facing.y * target.x;
    const float dot   = facing.x * target.x + facing.y * target.y;

    return std::atan2(cross, dot);
}

bool PerceptionSystem::isInFieldOfView(glm::vec2 selfPosition, glm::vec2 selfFacing,
                                       glm::vec2 targetPosition,
                                       const PerceptionConfig& config) {
    const glm::vec2 toTarget = targetPosition - selfPosition;
    const float distance = length(toTarget);

    // 距离超出视野范围：直接不可见
    if (distance > config.viewDistance) {
        return false;
    }

    // 贴身距离内，视野方向不再有意义（模拟「听到脚步 / 余光察觉」）
    if (distance <= config.proximityRadius) {
        return true;
    }

    // 极近距离时 toTarget 近似零向量，normalize 会失稳，已在上面处理
    const float angle = angleTo(selfFacing, toTarget);
    return std::fabs(angle) <= config.halfFovAngle;
}

// ---------------------------------------------------------------------------
// 可见性判定
// ---------------------------------------------------------------------------
bool PerceptionSystem::canSee(glm::vec2 selfPosition, glm::vec2 selfFacing,
                              const PerceptTarget& target) const {
    if (!target.alive) {
        return false;
    }

    // 第一关：视野锥
    if (!isInFieldOfView(selfPosition, selfFacing, target.position, config_)) {
        return false;
    }

    // 第二关：视线遮挡
    if (!config_.enableLineOfSight || !losQuery_) {
        return true;
    }

    ++lineOfSightChecks_;
    return losQuery_(selfPosition, target.position);
}

// ---------------------------------------------------------------------------
// 主更新
// ---------------------------------------------------------------------------
size_t PerceptionSystem::update(glm::vec2 selfPosition,
                                glm::vec2 selfFacing,
                                const std::vector<PerceptTarget>& targets,
                                double gameTime) {
    lineOfSightChecks_ = 0;

    // 标记本轮所有已有记录为「不可见」，随后重建——
    // 这样消失的目标不会被误认为仍在视野中。
    for (PerceptionRecord& record : records_) {
        record.visibleNow = false;
    }

    // 目标 ID 集合，用于稍后清理已消失的目标
    std::unordered_set<uint32_t> seenIds;
    seenIds.reserve(targets.size());

    size_t confirmedVisibleCount = 0;
    float  maxThreat = 0.0f;

    for (const PerceptTarget& target : targets) {
        if (!target.alive || target.id == 0) {
            continue;
        }

        seenIds.insert(target.id);

        // 找到（或创建）该目标的记录
        auto it = std::find_if(records_.begin(), records_.end(),
                               [id = target.id](const PerceptionRecord& r) {
                                   return r.target.id == id;
                               });

        const bool visible = canSee(selfPosition, selfFacing, target);

        // 【修复】记录只在真正看见时才建立/更新快照。
        //
        // 原实现在可见性判定**之前**就无条件 `record.target = target`，
        // 于是墙后、视野外的目标其真实坐标也被同步进记录，而 records()
        // 是公开接口 —— 任何读取 record.target.position 的调用方
        // （调试绘制、EQS、游戏层）都能拿到 AI 本不该知道的信息，
        // 与「AI 不能开挂，只能依据 lastKnownPosition 记忆行动」的设计冲突。
        //
        // 另外，过去对 targets 里的每个目标都建记录（哪怕从未看见），
        // 等于默认全知；现在改为「没看见过就不建立记忆」。
        if (!visible && it == records_.end()) {
            continue;
        }

        if (it == records_.end()) {
            PerceptionRecord fresh;
            fresh.target = target;
            fresh.firstSeenTime = gameTime;
            fresh.lastSeenTime  = gameTime;
            fresh.lastKnownPosition = target.position;
            records_.push_back(fresh);
            it = std::prev(records_.end());
        }

        PerceptionRecord& record = *it;
        if (visible) {
            record.target = target;   // 同步最新位置/状态
        }

        record.visibleNow = visible;

        if (visible) {
            record.lastSeenTime = gameTime;
            record.lastKnownPosition = target.position;
            record.distance = length(target.position - selfPosition);

            // 反应延迟：持续可见足够久才算「确认」。
            // 若中途丢失视线，计时器会因 visibleNow=false 而在下轮重置。
            const double visibleFor = gameTime - record.firstSeenTime;
            if (!record.confirmed && visibleFor >= config_.reactionTime) {
                record.confirmed = true;
            }

            // 对已在记忆中但视线中断的目标，恢复可见时立即重新确认
            if (record.confirmed) {
                ++confirmedVisibleCount;
            }
        } else {
            // 丢失视线：重置确认计时，但保留 confirmed 状态，
            // 使其在 memoryDuration 内仍可被 AI 利用（即「记忆」）。
            record.firstSeenTime = gameTime;

            const double sinceSeen = gameTime - record.lastSeenTime;
            if (sinceSeen > config_.memoryDuration) {
                record.confirmed = false;
            }
        }

        if (record.confirmed) {
            // 威胁评估：距离越近、威胁权重越高，威胁越大。
            // 用倒数衰减，保证近距离目标的影响显著高于远处目标。
            const float distanceFactor = 1.0f / (1.0f + record.distance / 200.0f);
            maxThreat = std::max(maxThreat, target.threat * distanceFactor);
        }
    }

    // 清理已从世界消失的目标（死亡后尸体被移除等）
    records_.erase(
        std::remove_if(records_.begin(), records_.end(),
                       [&seenIds](const PerceptionRecord& r) {
                           return seenIds.find(r.target.id) == seenIds.end();
                       }),
        records_.end());

    // 更新「最后一次看见敌人」的时间戳
    if (confirmedVisibleCount > 0) {
        lastSeenAnyTime_  = gameTime;
        hasEverSeenEnemy_ = true;

        if (const PerceptionRecord* nearest = nearestConfirmedTarget()) {
            lastKnownAnyPosition_ = nearest->lastKnownPosition;
        }
    }

    // 威胁等级随时间衰减：刚刚遭遇时最高，长期无接触则降为 0。
    // 这个衰减曲线（而非阶跃）让「撤退」决策看起来更自然。
    const double sinceSeen = gameTime - lastSeenAnyTime_;
    const float decay = (sinceSeen < 0.0)
                            ? 1.0f
                            : static_cast<float>(std::max(0.0, 1.0 - sinceSeen / config_.memoryDuration));
    threatLevel_ = std::clamp(maxThreat * decay, 0.0f, 1.0f);

    return confirmedVisibleCount;
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------
const PerceptionRecord* PerceptionSystem::nearestConfirmedTarget() const {
    const PerceptionRecord* best = nullptr;
    float bestDistance = 0.0f;

    for (const PerceptionRecord& record : records_) {
        if (!record.confirmed) {
            continue;
        }
        if (!best || record.distance < bestDistance) {
            best = &record;
            bestDistance = record.distance;
        }
    }
    return best;
}

const PerceptionRecord* PerceptionSystem::nearestVisibleTarget() const {
    const PerceptionRecord* best = nullptr;
    float bestDistance = 0.0f;

    for (const PerceptionRecord& record : records_) {
        // 用最近一次可见距离作为排序依据，避免本步不可见的目标被直接排除
        if (!record.visibleNow) {
            continue;
        }
        if (!best || record.distance < bestDistance) {
            best = &record;
            bestDistance = record.distance;
        }
    }
    return best;
}

const PerceptionRecord* PerceptionSystem::recordFor(uint32_t targetId) const {
    for (const PerceptionRecord& record : records_) {
        if (record.target.id == targetId) {
            return &record;
        }
    }
    return nullptr;
}

double PerceptionSystem::timeSinceLastSeen(double gameTime) const noexcept {
    if (!hasEverSeenEnemy_) {
        // 从未见过：返回一个大值，让「警戒」条件判定成立
        return 1e9;
    }
    return gameTime - lastSeenAnyTime_;
}

std::optional<glm::vec2> PerceptionSystem::lastKnownEnemyPosition() const {
    if (!hasEverSeenEnemy_) {
        return std::nullopt;
    }

    // 优先返回当前仍有记忆的目标位置
    if (const PerceptionRecord* nearest = nearestConfirmedTarget()) {
        return nearest->lastKnownPosition;
    }
    return lastKnownAnyPosition_;
}

// ---------------------------------------------------------------------------
// 黑板写入
// ---------------------------------------------------------------------------
void PerceptionSystem::writeToBlackboard(Blackboard& blackboard, glm::vec2 selfPosition,
                                         double gameTime) const {
    const PerceptionRecord* target = nearestConfirmedTarget();

    // 【修复】下面所有写入都带上 gameTime —— 否则 BlackboardEntry::
    // lastWriteTime 永远是 0，调试 UI 上「这条数据是什么时候写的」一栏
    // 没有任何意义（这正是之前 set() 缺少时间参数造成的）。
    if (target) {
        blackboard.set(Blackboard::Keys::kNearestEnemy, target->target.id, "Perception", gameTime);
        blackboard.set(Blackboard::Keys::kNearestEnemyDist, target->distance, "Perception", gameTime);
        blackboard.set(Blackboard::Keys::kLastKnownPosition, target->lastKnownPosition,
                       "Perception", gameTime);
    } else {
        // 没有确认目标时清空，防止行为树读到过期数据做出错误决策
        (void)blackboard.erase(Blackboard::Keys::kNearestEnemy);
        (void)blackboard.erase(Blackboard::Keys::kNearestEnemyDist);
    }

    if (const auto lastKnown = lastKnownEnemyPosition()) {
        blackboard.set(Blackboard::Keys::kLastKnownPosition, *lastKnown, "Perception", gameTime);
    }

    // 可见敌人 ID 列表（EQS 与目标选择会用到）
    //
    // 【修复】这里原先存 std::vector<int>，而同一函数上方写 kNearestEnemy
    // 用的是 EntityId（uint32_t）。黑板是 std::any 类型擦除，读写类型必须
    // 完全一致，否则 get<std::vector<EntityId>> 会静默返回 nullopt、
    // 不报错也不抛异常 —— 正是 Perception.h 顶部注释警告的陷阱。
    std::vector<EntityId> visibleIds;
    visibleIds.reserve(records_.size());
    for (const PerceptionRecord& record : records_) {
        if (record.confirmed) {
            visibleIds.push_back(record.target.id);
        }
    }
    blackboard.set(Blackboard::Keys::kVisibleEnemies, visibleIds, "Perception", gameTime);

    blackboard.set(Blackboard::Keys::kThreatLevel, threatLevel_, "Perception", gameTime);
    blackboard.set(Blackboard::Keys::kTimeSinceSeenEnemy,
                   static_cast<float>(timeSinceLastSeen(gameTime)), "Perception", gameTime);
    blackboard.set(Blackboard::Keys::kSelfPosition, selfPosition, "Perception", gameTime);
}

// ---------------------------------------------------------------------------
// 调试
// ---------------------------------------------------------------------------
std::string PerceptionSystem::debugSummary(double gameTime) const {
    char buffer[512];
    const PerceptionRecord* nearest = nearestConfirmedTarget();

    std::snprintf(buffer, sizeof(buffer),
                  "records=%zu confirmed=%s threat=%.2f losChecks=%llu losSinceSeen=%.1fs",
                  records_.size(),
                  nearest ? "yes" : "no",
                  static_cast<double>(threatLevel_),
                  static_cast<unsigned long long>(lineOfSightChecks_),
                  timeSinceLastSeen(gameTime));
    return std::string(buffer);
}

} // namespace TacticalAI::AI
