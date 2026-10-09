// ===========================================================================
//  src/game/Projectile.h
//
//  会飞的子弹。「攻击应该是一个实体，而不是一条线」。
//
//  【为什么要从"瞬时射线"改成"实体子弹"】
//  早期实现把开火简化成一次瞬时射线：对着方向做一次线段求交，命中就
//  立刻扣血，然后在屏幕上补画一条线当作"弹道"。这样做有两个根本问题：
//
//    1. 攻击**没有实体**。它不占空间、不花时间、无法被观察。玩家看到的
//       那条线只是事后画的装饰，和实际结算没有任何因果关系——
//       线画得再漂亮，你也没法"看见子弹飞过来"。
//    2. 攻击**无法被躲避**。命中在开火那一瞬间就决定了，目标之后的任何
//       移动都改变不了结果。战术上这意味着「走位」这个维度完全不存在。
//
//  改成实体子弹后，命中判定发生在**子弹飞到目标身上那一刻**，于是：
//    - 子弹有飞行时间 → 移动中的目标可以躲开（走位第一次变得有意义）
//    - 子弹会被掩体挡住 → 掩体第一次真的能"挡子弹"，而不只是挡视线
//    - 子弹是场景里可见的东西 → 玩家能看见"一串子弹朝我飞过来"
//
//  【和曳光线的关系】
//  旧的"弹道曳光线"被降级为**弹着残影**（见 World::ImpactMark）：
//  子弹命中/落地时留下一条极短命的淡出轨迹，用来回答"这一枪是从哪来的"。
//  主体视觉变成了真正在飞的子弹本体。
// ===========================================================================
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "game/Entity.h"

namespace TacticalAI::Game {

/// 子弹的归属阵营。决定它能伤害谁——只打敌对阵营，不做友军伤害。
///
/// 刻意不写"友军伤害"：沙盒里敌人之间互相挡枪会让调试变得一团糟
/// （一个敌人被队友打死，看起来就像"AI 逻辑错了"），而收益为零。
enum class Faction : uint8_t {
    Player = 0,
    Enemy  = 1,
};

[[nodiscard]] const char* toString(Faction faction) noexcept;

/// 子弹参数。玩家与敌人各用一份，便于分别调平衡。
struct ProjectileConfig {
    /// 飞行速度（世界单位 / 秒）。
    ///
    /// 【为什么是这个量级】
    /// 800 左右意味着：跨越 320 单位的敌人射程需要约 0.4 秒。这个延迟
    /// 刚好让"看到子弹再侧移"成为可行操作（人眼反应约 0.2 秒），
    /// 同时又不至于慢到看起来像投掷物。低于 400 会明显变"炮弹"，
    /// 高于 1600 则等同于瞬时命中，等于退回到旧的射线模型。
    float speed = 820.0f;

    /// 碰撞半径。取值偏小（3）是为了让子弹看起来是"点"，而不是球。
    float radius = 3.0f;

    /// 最大飞行距离。超过即自动消失（子弹的"射程"）。
    float maxRange = 520.0f;

    /// 命中伤害。
    float damage = 18.0f;

    /// 出膛偏移：从枪口再往前推这么远生成，避免子弹一出生就撞上开枪者自己。
    float muzzleOffset = 16.0f;

    /// 拖尾对应的时长（秒）。绘制时用 speed * 此值得到拖尾长度。
    /// 放在配置里而非类常量，是为了让"曳光看起来多长"能和子弹速度一起调。
    float trailSeconds = 0.035f;
};

/// 玩家子弹的默认参数（射程远、伤害高、单发慢）。
[[nodiscard]] ProjectileConfig defaultPlayerProjectile();

/// 敌人子弹的默认参数（射程短、伤害低、连发快）。
///
/// 【为什么敌人射程要做短】
/// 敌人的数量优势 + 持续的射击频率，如果射程还和玩家一样远，
/// 玩家在开阔地会被来自多个方向的火力同时覆盖。320 大致等于
/// EnemyConfig::fireRange，两者对齐后"看得见的射程"与"实际能打到"一致。
[[nodiscard]] ProjectileConfig defaultEnemyProjectile();

/// 一颗子弹。
///
/// 【为什么它不复用 Entity::update 的积分】
/// Entity::update 的做法是「位置 += 速度 * dt」一步到位。对子弹来说这是错的：
/// 一步 60Hz 下子弹走 820/60 ≈ 13.7 个单位，而目标半径只有 11 左右——
/// 高速子弹会**直接穿过去**（tunneling），表现为"明明打中了却没伤害"。
///
/// 因此子弹的推进由 World::updateProjectiles 负责，用**子步进**把一步运动
/// 切成若干小段逐段做碰撞检测。本类只保存状态与判定结果，不自己做积分。
class Projectile final : public Entity {
public:
    /// @param ownerId   开枪者的实体 ID（用于统计与"自己打不中自己"）
    /// @param faction   归属阵营（决定能伤害谁）
    /// @param origin    出膛位置（调用方应已加上 muzzleOffset）
    /// @param direction 飞行方向（无需归一化，构造时会归一化）
    Projectile(uint32_t ownerId, Faction faction, glm::vec2 origin, glm::vec2 direction,
               ProjectileConfig config = {}, std::string name = "Bullet");

    Projectile(const Projectile&)            = delete;
    Projectile& operator=(const Projectile&) = delete;

    // -- 查询 ----------------------------------------------------------------

    [[nodiscard]] uint32_t ownerId() const noexcept { return ownerId_; }
    [[nodiscard]] Faction  faction() const noexcept { return faction_; }
    [[nodiscard]] float    damage() const noexcept { return config_.damage; }
    [[nodiscard]] float    maxRange() const noexcept { return config_.maxRange; }
    [[nodiscard]] const ProjectileConfig& config() const noexcept { return config_; }

    /// 已经飞过的距离（世界单位）。
    [[nodiscard]] float traveled() const noexcept { return traveled_; }
    /// 剩余可飞距离。
    [[nodiscard]] float remainingRange() const noexcept {
        return std::max(0.0f, config_.maxRange - traveled_);
    }
    [[nodiscard]] bool outOfRange() const noexcept { return traveled_ >= config_.maxRange; }

    /// 单位飞行方向。
    [[nodiscard]] glm::vec2 direction() const noexcept { return direction_; }

    /// 拖尾长度（世界单位）。绘制时从 position - direction * trailLength 连到
    /// 当前位置，就得到一条真实的运动轨迹——它由子弹实际飞过的距离决定，
    /// 而不是凭空画出来的线。
    [[nodiscard]] float trailLength() const noexcept {
        return config_.speed * config_.trailSeconds;
    }

    /// 出膛位置（已含 muzzleOffset）。弹着残影需要它来回答"这一枪从哪来"。
    [[nodiscard]] glm::vec2 launchPoint() const noexcept { return launchPoint_; }

    // -- 本帧飞行上报（由 World::updateProjectiles 调用） ---------------------

    /// 推进一段距离。返回本次推进后是否已超出射程。
    bool advance(float distance) noexcept;

    /// 标记：撞到地形（墙 / 掩体）而终止。
    void markHitTerrain() noexcept;
    /// 标记：打中了实体而终止。
    void markHitEntity(uint32_t targetId, float damage) noexcept;
    /// 标记：飞完射程自然耗尽。
    void markExpired() noexcept;

    /// 本帧是否已终止（命中 / 撞墙 / 射程耗尽任一）。
    [[nodiscard]] bool terminated() const noexcept { return terminated_; }
    /// 终止原因：是否打中了实体（而不是撞墙 / 射程耗尽）。
    [[nodiscard]] bool hitEntity() const noexcept { return hitEntity_; }
    /// 命中的目标 ID（未命中实体时为 0）。
    [[nodiscard]] uint32_t hitTargetId() const noexcept { return hitTargetId_; }
    /// 本次命中造成的实际伤害。
    [[nodiscard]] float dealtDamage() const noexcept { return dealtDamage_; }

    /// 终止发生的世界坐标（即弹着点）。
    [[nodiscard]] glm::vec2 impactPoint() const noexcept { return impactPoint_; }

    /// 是否属于玩家阵营（绘制与统计用）。
    [[nodiscard]] bool isPlayerShot() const noexcept { return faction_ == Faction::Player; }

    [[nodiscard]] std::string debugString() const override;

protected:
    /// 故意留空：子弹的位移由 World 子步进驱动，不走基类的一步积分。
    void onUpdate(double fixedDelta) override;

private:
    uint32_t        ownerId_ = 0;
    Faction         faction_ = Faction::Enemy;
    ProjectileConfig config_{};
    glm::vec2       direction_{1.0f, 0.0f};
    glm::vec2       launchPoint_{0.0f, 0.0f};

    float  traveled_ = 0.0f;
    bool   terminated_ = false;
    bool   hitEntity_ = false;
    uint32_t hitTargetId_ = 0;
    float  dealtDamage_ = 0.0f;
    glm::vec2 impactPoint_{0.0f, 0.0f};
};

} // namespace TacticalAI::Game
