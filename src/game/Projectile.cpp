// ===========================================================================
//  src/game/Projectile.cpp
// ===========================================================================
#include "game/Projectile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace TacticalAI::Game {
namespace {

inline glm::vec2 normalizeOr(glm::vec2 v, glm::vec2 fallback) noexcept {
    const float length = std::sqrt(v.x * v.x + v.y * v.y);
    if (length < 1e-6f) {
        return fallback;
    }
    return {v.x / length, v.y / length};
}

} // namespace

const char* toString(Faction faction) noexcept {
    switch (faction) {
        case Faction::Player: return "Player";
        case Faction::Enemy:  return "Enemy";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// 默认弹道参数
// ---------------------------------------------------------------------------
ProjectileConfig defaultPlayerProjectile() {
    ProjectileConfig config;
    config.speed       = 880.0f;   // 玩家子弹略快，打移动目标不需要太多提前量
    config.radius      = 3.0f;
    config.maxRange    = 520.0f;   // 与旧的 kPlayerRange 保持一致
    config.damage      = 18.0f;    // 敌人 70 血 → 4 发击杀
    config.muzzleOffset = 16.0f;
    config.trailSeconds = 0.035f;
    return config;
}

ProjectileConfig defaultEnemyProjectile() {
    ProjectileConfig config;
    config.speed       = 760.0f;   // 比玩家慢，给玩家留出"看见子弹→侧移"的窗口
    config.radius      = 3.0f;
    // 【调整】与 EnemyConfig::fireRange 对齐（400）。
    //
    // 原来是 320。实测（玩家绕圈走位、无敌、60 秒）发现敌人有 2.4~6.8 秒
    // 的连续时段"够不着"：玩家 160~280 单位/秒，敌人只有 130，射程又比玩家
    // 短 200，于是大部分时间都在追。射程与追击速度一起调整后才有还手的机会。
    // 仍然远短于玩家的 520 —— 玩家依旧可以站在远处压制。
    config.maxRange    = 400.0f;
    config.damage      = 5.0f;     // 会被 EnemyConfig::damage 覆盖，此处仅为兜底
    config.muzzleOffset = 14.0f;
    config.trailSeconds = 0.040f;  // 慢弹拖尾稍长，看起来更像"飞过来的东西"
    return config;
}

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
Projectile::Projectile(uint32_t ownerId, Faction faction, glm::vec2 origin, glm::vec2 direction,
                       ProjectileConfig config, std::string name)
    : Entity(EntityType::Projectile, std::move(name), origin),
      ownerId_(ownerId),
      faction_(faction),
      config_(config),
      direction_(normalizeOr(direction, {1.0f, 0.0f})),
      launchPoint_(origin) {
    // 子弹的血量与射程无关：它靠"终止标记"消失，而不是靠掉血。
    // 给一个不会被误判为死亡的值，避免 cleanupDeadEntities 把它当尸体回收。
    setMaxHealth(1.0f);
    setHealth(1.0f);
    setRadius(std::max(config_.radius, 0.5f));
    setFacing(direction_);

    // 直接给出速度，方便任何按 velocity 绘制/推理的通用代码也能正常工作。
    setVelocity(direction_ * config_.speed);

    // 标签只用来表达归属，不参与 AI 的目标筛选
    // （AI 的感知目标列表按 EntityType 过滤，不会把子弹当成敌人）。
    addTag(faction_ == Faction::Player ? EntityTag::Friendly : EntityTag::Hostile);
}

// ---------------------------------------------------------------------------
// 飞行上报
// ---------------------------------------------------------------------------
bool Projectile::advance(float distance) noexcept {
    if (terminated_) {
        return true;
    }
    traveled_ += std::max(distance, 0.0f);
    return traveled_ >= config_.maxRange;
}

void Projectile::markHitTerrain() noexcept {
    if (terminated_) {
        return;
    }
    terminated_ = true;
    hitEntity_ = false;
    impactPoint_ = position_;
}

void Projectile::markHitEntity(uint32_t targetId, float damage) noexcept {
    if (terminated_) {
        return;
    }
    terminated_ = true;
    hitEntity_ = true;
    hitTargetId_ = targetId;
    dealtDamage_ = damage;
    impactPoint_ = position_;
}

void Projectile::markExpired() noexcept {
    if (terminated_) {
        return;
    }
    terminated_ = true;
    hitEntity_ = false;
    impactPoint_ = position_;
}

void Projectile::onUpdate(double fixedDelta) {
    // 子弹的位移由 World::updateProjectiles 的子步进逻辑驱动。
    //
    // 这里必须留空，而不是调用基类的一步积分：基类会把整步位移一次性加上，
    // 于是子弹可能"跨过"半径只有 11 的目标而检测不到（tunneling）。
    // 保留这个空实现是为了让"子弹不走通用积分"这件事在代码里可见——
    // 如果哪天有人删掉它，子弹就会悄悄退回成穿透模式。
    (void)fixedDelta;
}

std::string Projectile::debugString() const {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "#%u Projectile owner=%u faction=%s pos=(%.0f,%.0f) dir=(%.2f,%.2f) "
                  "traveled=%.0f/%.0f%s%s",
                  id_, ownerId_, toString(faction_),
                  static_cast<double>(position().x), static_cast<double>(position().y),
                  static_cast<double>(direction_.x), static_cast<double>(direction_.y),
                  static_cast<double>(traveled_), static_cast<double>(config_.maxRange),
                  terminated_ ? " [DONE]" : "",
                  hitEntity_ ? " [HIT]" : "");
    return std::string(buffer);
}

} // namespace TacticalAI::Game
