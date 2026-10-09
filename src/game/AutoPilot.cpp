// ===========================================================================
//  src/game/AutoPilot.cpp
// ===========================================================================
#include "game/AutoPilot.h"

#include <algorithm>
#include <cmath>

#include "game/Entity.h"
#include "game/Player.h"
#include "game/Projectile.h"
#include "game/World.h"

namespace TacticalAI::Game {
namespace {

float lengthOf(glm::vec2 v) noexcept {
    return std::sqrt(v.x * v.x + v.y * v.y);
}

glm::vec2 normalizedOr(glm::vec2 v, glm::vec2 fallback) noexcept {
    const float len = lengthOf(v);
    if (len < 1e-4f) {
        return fallback;
    }
    return v / len;
}

/// 最近的**活着**的敌人。
/// 注意 world.enemies() 里**含尚未回收的尸体**（尸体滞留 3 秒），
/// 直接拿第一个/最近的那个会把枪口对着尸体 —— 这里必须自己过滤。
Enemy* nearestAliveEnemy(World& world, glm::vec2 from) {
    Enemy* best = nullptr;
    float  bestDistance = 0.0f;

    for (Enemy* enemy : world.enemies()) {
        if (enemy == nullptr || enemy->isDead()) {
            continue;
        }
        const float distance = lengthOf(enemy->position() - from);
        if (best == nullptr || distance < bestDistance) {
            best = enemy;
            bestDistance = distance;
        }
    }
    return best;
}

} // namespace

void AutoPilot::update(World& world, Player& player, double gameTime, double fixedDelta) {
    (void)fixedDelta;

    const glm::vec2 self = player.position();
    if (!initialized_) {
        waypoint_    = self;
        initialized_ = true;
    }

    Enemy* target = nearestAliveEnemy(world, self);

    // -----------------------------------------------------------------------
    // 1) 没有敌人：在地图内巡游（并且**不**开火 —— 对着空地扫射很出戏）
    // -----------------------------------------------------------------------
    if (target == nullptr) {
        static const glm::vec2 kWaypoints[] = {
            {400.0f, 300.0f}, {1200.0f, 300.0f}, {1200.0f, 900.0f}, {400.0f, 900.0f}};

        if (lengthOf(waypoint_ - self) < 60.0f) {
            waypointIndex_ = (waypointIndex_ + 1) % 4;
            waypoint_      = kWaypoints[waypointIndex_];
        }

        player.setAiming(false);
        player.setSprinting(false);
        player.setLookAtPoint(waypoint_);
        player.setMoveIntent(normalizedOr(waypoint_ - self, glm::vec2{1.0f, 0.0f}));
        return;
    }

    const glm::vec2 toTarget  = target->position() - self;
    const float     distance  = lengthOf(toTarget);
    const glm::vec2 direction = normalizedOr(toTarget, glm::vec2{1.0f, 0.0f});

    // -----------------------------------------------------------------------
    // 2) 瞄准：始终指向目标。
    //    子弹有飞行时间，但玩家在演示里通常在中距离交战，提前量收益很小，
    //    而"瞄准点抖动"会明显破坏观感 —— 所以这里刻意不预判。
    // -----------------------------------------------------------------------
    player.setLookAtPoint(target->position());
    player.setAiming(distance > kPreferredRange);   // 远距离放慢脚步，像在认真瞄

    // -----------------------------------------------------------------------
    // 3) 走位：保持交战距离 + 横向游走（"边打边绕"）
    // -----------------------------------------------------------------------
    if (gameTime - lastStrafeSwitch_ >= kStrafeSwitchInterval) {
        lastStrafeSwitch_ = gameTime;
        // 逆时针切向；每次换向时交替左右，形成"绕圈"而不是"一直往一边滑"
        const glm::vec2 tangent{-direction.y, direction.x};
        strafeDirection_ = ((waypointIndex_++) % 2 == 0) ? tangent : -tangent;
    }

    glm::vec2 intent{0.0f, 0.0f};
    if (distance < kKeepAwayRange) {
        intent = -direction;                        // 太近：后撤
    } else if (distance > kPreferredRange) {
        intent = direction;                         // 太远：压上
    } else {
        intent = strafeDirection_;                  // 距离合适：横向游走
    }
    intent += strafeDirection_ * 0.35f;             // 叠一点切向，避免直线对撞的呆感

    player.setMoveIntent(normalizedOr(intent, strafeDirection_));
    player.setSprinting(distance > kPreferredRange * 1.6f);

    // -----------------------------------------------------------------------
    // 4) 开火：在射程内、且枪口已经大致对准
    //
    //    requestFire 只是"请求"，真正能不能开枪由 Player 的冷却与 World 的
    //    结算决定（每步都给请求 ≈ 自动步枪）。这条链路与真人按键完全一致。
    // -----------------------------------------------------------------------
    constexpr float kFireRange           = 480.0f;
    constexpr float kFireFacingTolerance = 0.22f;   // 约 12.6°

    if (distance < kFireRange) {
        const glm::vec2 facing = player.facing();
        const float cross = facing.x * direction.y - facing.y * direction.x;
        if (std::fabs(cross) < kFireFacingTolerance) {
            player.requestFire();
        }
    }

    // -----------------------------------------------------------------------
    // 5) 躲弹：有子弹正朝自己飞来就翻滚
    //
    //    "翻滚躲开子弹"是这个沙盒最值得展示的一幕（HUD 上的"闪避"计数会跳），
    //    而它只有在**子弹真的飞来时**才会发生 —— 所以演示驾驶员必须看得见
    //    子弹（world.projectiles()），否则只能靠定时翻滚，看起来像抽搐。
    // -----------------------------------------------------------------------
    if (!player.isRolling() && player.rollCooldownRemaining(gameTime) <= 0.0) {
        for (const Projectile* bullet : world.projectiles()) {
            if (bullet == nullptr || bullet->faction() != Faction::Enemy) {
                continue;
            }

            const glm::vec2 toBullet = bullet->position() - self;
            const float bulletDistance = lengthOf(toBullet);
            if (bulletDistance > kDodgeRadius || bulletDistance < 1e-3f) {
                continue;
            }

            // 子弹方向与"从子弹指向我"的方向一致 ⇒ 它正朝我飞来
            const glm::vec2 bulletDirection = bullet->direction();
            const float alignment =
                (bulletDirection.x * toBullet.x + bulletDirection.y * toBullet.y) / bulletDistance;
            if (alignment > 0.9f) {
                (void)player.tryStartRoll(gameTime);
                break;
            }
        }
    }
}

} // namespace TacticalAI::Game
