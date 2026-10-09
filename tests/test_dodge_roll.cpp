// ===========================================================================
//  tests/test_dodge_roll.cpp
//
//  翻滚（Dodge Roll）的回归测试。
//
//  【这条测试线要守住什么】
//  翻滚是一个"看起来简单、其实每个参数都能悄悄失效"的功能：
//
//    - 距离：位移曲线如果不做归一化，150 会随曲线形状漂移；
//            如果被 clampVelocity 削一刀（翻滚速度比冲刺上限还快），
//            更是只在按住 Shift 时才露馅。这两种都不会报错，只会"不太对"。
//    - 无敌：它是一条完全不可见的规则。写错了不会崩、不会报错，
//            表现出来只是"翻滚好像没什么用"。必须用伤害入口去断言。
//    - 冷却与"翻滚中不可再触发"：都属于"该拒绝的时候拒绝了没有"，
//            没有断言就只能靠手速去试。
//    - 翻滚中不能射击：这条最容易在重构时丢掉，因为它跨了两个类
//            （Player 的状态 + World 的开火结算）。
//
//  所以这里逐条钉死：距离、时长、方向两套规则、冷却、拒绝条件、
//  无敌窗口的起止、以及"子弹被吸收但不掉血"这个具体的交战结果。
// ===========================================================================
#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include "game/Player.h"
#include "game/Projectile.h"
#include "game/World.h"

using namespace TacticalAI;

namespace {

constexpr double kFixedDelta = 1.0 / 60.0;

/// 构造一个干净的世界：无示例障碍、无敌人。
///
/// 翻滚是"位移 + 时间窗口"的功能，测试里必须让地形完全可控。
/// 默认场景那组手工掩体会随机把翻滚路径截断，让"滚了多远"变成
/// 一个取决于地形的量——那就没法断言 150 了。
std::unique_ptr<Game::World> makeCleanWorld(glm::vec2 playerStart = {400.0f, 600.0f}) {
    Game::WorldConfig config;
    config.generateDemoObstacles = false;
    config.initialEnemyCount = 0;
    config.playerStart = playerStart;

    auto world = std::make_unique<Game::World>(config);
    world->initialize();
    return world;
}

/// 跑若干逻辑步，同时推进游戏时间。
void step(Game::World& world, double& gameTime, int steps) {
    for (int i = 0; i < steps; ++i) {
        world.update(kFixedDelta, gameTime);
        gameTime += kFixedDelta;
    }
}

float distanceBetween(glm::vec2 a, glm::vec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

/// 一次翻滚需要多少步（按 60Hz 算，0.35s 约 21 步）。
int stepsForRoll(const Game::Player& player) {
    return static_cast<int>(std::ceil(player.config().rollDuration / kFixedDelta));
}

/// 一直跑到翻滚结束，正好停在结束的那一步上。
///
/// 【为什么不能"跑固定步数再量位移"】
/// 翻滚一结束，玩家立刻回到常规移动。测试里 moveIntent 还设着，
/// 于是他接着朝同一个方向走——多跑 3 步就多走约 2 个单位，
/// 于是"翻滚位移 = 150"会被量成 152。
/// 那个 2 看起来像翻滚超距，其实只是把"滚完之后的正常走路"也算进了位移。
/// 这类"测量窗口没对齐"是位移类测试最常见的假失败来源。
int stepUntilRollEnds(Game::World& world, double& gameTime, int maxSteps = 300) {
    int used = 0;
    while (used < maxSteps) {
        Game::Player* player = world.player();
        if (player == nullptr || !player->isRolling()) {
            break;
        }
        world.update(kFixedDelta, gameTime);
        gameTime += kFixedDelta;
        ++used;
    }
    return used;
}

} // namespace

// ---------------------------------------------------------------------------
//  位移与时长：两个可以直接量出来的契约
// ---------------------------------------------------------------------------

TEST(DodgeRollTest, RollMovesPlayerExactlyRollDistance) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    // 有移动输入 → 朝移动方向滚
    player->setMoveIntent({1.0f, 0.0f});
    player->setLookAtPoint(player->position() + glm::vec2{1.0f, 0.0f});

    const glm::vec2 start = player->position();
    ASSERT_TRUE(player->tryStartRoll(gameTime));

    // 量到位移的那一刻必须就是翻滚结束的那一步，不能多跑
    stepUntilRollEnds(*world, gameTime);
    ASSERT_FALSE(player->isRolling());

    const float moved = distanceBetween(player->position(), start);

    // 容差 1.0 个单位：曲线沿解析原函数积分，理论上是精确的，
    // 残留偏差只来自 float 舍入。
    // 真正的价值在于：如果哪天有人把 clampVelocity 重新套回翻滚
    // （翻滚起手速度约 857，远高于冲刺上限 560），或者把曲线换成
    // 未归一化的写法，位移会一下子偏出好几十个单位，这条断言会立刻炸掉。
    EXPECT_NEAR(moved, player->config().rollDistance, 1.0f);
}

TEST(DodgeRollTest, RollFallsShortOfRollDistanceWhenBlockedByWall) {
    // 贴着地图左边界起手，朝墙滚。
    // 期望：人停在边界上，位移明显小于 150，且不会跑到地图外。
    auto world = makeCleanWorld({60.0f, 600.0f});
    double gameTime = 0.0;

    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    player->setMoveIntent({-1.0f, 0.0f});
    player->setLookAtPoint(player->position() + glm::vec2{-1.0f, 0.0f});

    const glm::vec2 start = player->position();
    ASSERT_TRUE(player->tryStartRoll(gameTime));

    stepUntilRollEnds(*world, gameTime);

    EXPECT_GE(player->position().x, player->radius() - 0.5f)
        << "翻滚把玩家推到了地图边界之外";
    EXPECT_LT(distanceBetween(player->position(), start), player->config().rollDistance)
        << "撞墙的翻滚竟然还是走满了全程";
}

TEST(DodgeRollTest, RollLastsAboutRollDuration) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    player->setMoveIntent({1.0f, 0.0f});
    player->setLookAtPoint(player->position() + glm::vec2{1.0f, 0.0f});
    ASSERT_TRUE(player->tryStartRoll(gameTime));

    int stepsRolling = 0;
    while (player->isRolling() && stepsRolling < 200) {
        step(*world, gameTime, 1);
        ++stepsRolling;
    }

    // 允许一个步长的误差：判定发生在步与步之间，"刚好 0.35s" 落在哪一步
    // 取决于浮点累加的结果，不应该把测试写死成某一个确切的步数。
    EXPECT_NEAR(stepsRolling * kFixedDelta, player->config().rollDuration, kFixedDelta + 1e-9);
    EXPECT_FALSE(player->isRolling());
}

// ---------------------------------------------------------------------------
//  方向：有移动输入用移动方向，没有就用鼠标方向
// ---------------------------------------------------------------------------

TEST(DodgeRollTest, RollFollowsMoveInputDirection) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    // 移动方向朝下，鼠标指向正上方——两者故意相反。
    // 这样"到底用了哪一个"不会有歧义。
    player->setMoveIntent({0.0f, 1.0f});
    player->setLookAtPoint(player->position() + glm::vec2{0.0f, -100.0f});

    const glm::vec2 start = player->position();
    ASSERT_TRUE(player->tryStartRoll(gameTime));

    EXPECT_NEAR(player->rollDirection().y, 1.0f, 1e-4f) << "翻滚方向没有采用移动输入";
    EXPECT_NEAR(player->rollDirection().x, 0.0f, 1e-4f);

    stepUntilRollEnds(*world, gameTime);

    EXPECT_GT(player->position().y - start.y, 100.0f) << "玩家没有被滚到移动方向上";
    EXPECT_NEAR(player->position().x, start.x, 0.5f);
}

TEST(DodgeRollTest, RollFallsBackToMouseDirectionWhenNoMoveInput) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    // 没有移动输入，鼠标指向左方
    player->setMoveIntent({0.0f, 0.0f});
    player->setLookAtPoint(player->position() + glm::vec2{-100.0f, 0.0f});

    const glm::vec2 start = player->position();
    ASSERT_TRUE(player->tryStartRoll(gameTime));

    EXPECT_NEAR(player->rollDirection().x, -1.0f, 1e-4f) << "无移动输入时没有采用鼠标方向";

    stepUntilRollEnds(*world, gameTime);

    EXPECT_LT(player->position().x - start.x, -100.0f);
}

TEST(DodgeRollTest, RollDirectionIsAlwaysNormalized) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    // 鼠标正好停在角色身上：方向会退化。此时必须仍然给出一个单位向量，
    // 否则翻滚会原地不动，玩家只会觉得"空格失灵了"。
    player->setMoveIntent({0.0f, 0.0f});
    player->setLookAtPoint(player->position());

    ASSERT_TRUE(player->tryStartRoll(gameTime));

    const glm::vec2 dir = player->rollDirection();
    EXPECT_NEAR(dir.x * dir.x + dir.y * dir.y, 1.0f, 1e-3f)
        << "退化情况下翻滚方向不是单位向量，会导致位移为 0 或超出距离";
}

// ---------------------------------------------------------------------------
//  触发条件：翻滚中不可再触发、冷却内不可触发
// ---------------------------------------------------------------------------

TEST(DodgeRollTest, CannotRollWhileAlreadyRolling) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    player->setMoveIntent({0.0f, 0.0f});
    player->setLookAtPoint(player->position() + glm::vec2{1.0f, 0.0f});
    ASSERT_TRUE(player->tryStartRoll(gameTime));
    EXPECT_EQ(player->rollsStarted(), 1);

    // 翻滚进行中反复尝试触发，全部应该被拒绝
    for (int i = 0; i < stepsForRoll(*player) - 2; ++i) {
        step(*world, gameTime, 1);
        if (!player->isRolling()) {
            break;
        }
        EXPECT_FALSE(player->tryStartRoll(gameTime));
    }

    EXPECT_EQ(player->rollsStarted(), 1) << "翻滚过程中被重复触发了";
}

TEST(DodgeRollTest, CannotRollDuringCooldown) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    player->setMoveIntent({0.0f, 0.0f});
    player->setLookAtPoint(player->position() + glm::vec2{1.0f, 0.0f});
    const double rollStartTime = gameTime;
    ASSERT_TRUE(player->tryStartRoll(gameTime));

    // 跑到翻滚结束、但冷却还没走完的那一刻
    step(*world, gameTime, stepsForRoll(*player) + 2);
    ASSERT_FALSE(player->isRolling());
    ASSERT_LT(gameTime - rollStartTime, player->config().rollCooldown)
        << "测试前提不成立：此时冷却应该还没结束";

    EXPECT_FALSE(player->tryStartRoll(gameTime)) << "冷却期内竟然可以起手第二次翻滚";
    EXPECT_EQ(player->rollsStarted(), 1);
    EXPECT_GT(player->rollCooldownRemaining(gameTime), 0.0);
}

TEST(DodgeRollTest, CanRollAgainAfterCooldownElapses) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    player->setMoveIntent({0.0f, 0.0f});
    player->setLookAtPoint(player->position() + glm::vec2{1.0f, 0.0f});
    const double rollStartTime = gameTime;
    ASSERT_TRUE(player->tryStartRoll(gameTime));

    // 一直跑到冷却结束
    while (gameTime - rollStartTime < player->config().rollCooldown) {
        step(*world, gameTime, 1);
    }

    EXPECT_LE(player->rollCooldownRemaining(gameTime), 0.0);

    // 松掉移动输入，让第二次翻滚的冷却判定不受上一次影响
    player->setLookAtPoint(player->position() + glm::vec2{1.0f, 0.0f});
    EXPECT_TRUE(player->tryStartRoll(gameTime)) << "冷却结束后仍然无法再次翻滚";
    EXPECT_EQ(player->rollsStarted(), 2);
}

TEST(DodgeRollTest, RollCooldownRemainingIsReported) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    // 一次都没滚过时应当直接是"就绪"（初值取了极大负数）
    EXPECT_LE(player->rollCooldownRemaining(gameTime), 0.0);

    player->setMoveIntent({0.0f, 0.0f});
    player->setLookAtPoint(player->position() + glm::vec2{1.0f, 0.0f});
    ASSERT_TRUE(player->tryStartRoll(gameTime));

    EXPECT_NEAR(player->rollCooldownRemaining(gameTime),
                static_cast<double>(player->config().rollCooldown), 1e-6);
}

// ---------------------------------------------------------------------------
//  翻滚期间不能射击
// ---------------------------------------------------------------------------

TEST(DodgeRollTest, CannotFireWhileRolling) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    player->setMoveIntent({0.0f, 0.0f});
    player->setLookAtPoint(player->position() + glm::vec2{1.0f, 0.0f});
    ASSERT_TRUE(player->tryStartRoll(gameTime));

    // 翻滚第一步就请求开火：请求会被消费掉，但不该产生子弹
    player->requestFire();
    step(*world, gameTime, 1);
    EXPECT_EQ(world->projectileCount(), 0) << "翻滚期间打出了子弹";
    EXPECT_EQ(player->shotsFired(), 0);

    // 翻滚中段再试一次，同样不该成功（此时武器冷却早已走完，
    // 所以失败的原因只可能是"翻滚中禁止射击"）
    step(*world, gameTime, stepsForRoll(*player) / 2);
    ASSERT_TRUE(player->isRolling());
    ASSERT_LE(player->fireCooldownRemaining(gameTime), 0.0)
        << "测试前提不成立：武器冷却未过，无法区分是冷却还是翻滚拦住了这一枪";

    player->requestFire();
    step(*world, gameTime, 1);
    EXPECT_EQ(world->projectileCount(), 0) << "翻滚中段打出了子弹";
    EXPECT_EQ(player->shotsFired(), 0);
}

TEST(DodgeRollTest, CanFireAgainAfterRollEnds) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    player->setMoveIntent({0.0f, 0.0f});
    player->setLookAtPoint(player->position() + glm::vec2{1.0f, 0.0f});
    ASSERT_TRUE(player->tryStartRoll(gameTime));
    step(*world, gameTime, stepsForRoll(*player) + 2);
    ASSERT_FALSE(player->isRolling());

    player->requestFire();
    step(*world, gameTime, 1);

    EXPECT_EQ(player->shotsFired(), 1) << "翻滚结束后依然开不了枪";
    EXPECT_EQ(world->projectileCount(), 1);
}

// ---------------------------------------------------------------------------
//  无敌帧
// ---------------------------------------------------------------------------

TEST(DodgeRollTest, PlayerIsInvulnerableAtRollStart) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    EXPECT_FALSE(player->isInvulnerable()) << "没翻滚时不该是无敌的";

    player->setMoveIntent({0.0f, 0.0f});
    player->setLookAtPoint(player->position() + glm::vec2{1.0f, 0.0f});
    ASSERT_TRUE(player->tryStartRoll(gameTime));

    EXPECT_TRUE(player->isInvulnerable()) << "翻滚起手没有无敌帧";
}

TEST(DodgeRollTest, InvulnerabilityBlocksDamageDuringWindow) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    const float healthBefore = player->health();
    player->setMoveIntent({0.0f, 0.0f});
    player->setLookAtPoint(player->position() + glm::vec2{1.0f, 0.0f});
    ASSERT_TRUE(player->tryStartRoll(gameTime));

    // 在无敌窗口内连续吃伤害，应当一点血都不掉
    const double invuln = static_cast<double>(player->config().rollInvulnTime);
    while (gameTime < invuln - kFixedDelta) {
        step(*world, gameTime, 1);
        ASSERT_TRUE(player->isRolling());
        const float dealt = world->applyDamage(player->id(), 10.0f, 0, player->position(), gameTime);
        EXPECT_FLOAT_EQ(dealt, 0.0f) << "无敌帧内仍然结算了伤害";
    }

    EXPECT_FLOAT_EQ(player->health(), healthBefore) << "无敌帧内血量被扣掉了";
}

TEST(DodgeRollTest, PlayerBecomesVulnerableAfterInvulnerabilityWindow) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    const float healthBefore = player->health();
    player->setMoveIntent({0.0f, 0.0f});
    player->setLookAtPoint(player->position() + glm::vec2{1.0f, 0.0f});
    ASSERT_TRUE(player->tryStartRoll(gameTime));

    // 跑到无敌窗口之后、翻滚还没结束的区间
    const double invuln = static_cast<double>(player->config().rollInvulnTime);
    const double duration = static_cast<double>(player->config().rollDuration);
    // 【修复】只断言 invuln < duration 是不够的：下面的循环会多走一个
    // 固定步，真正需要的是 invuln + 一步 之后仍在翻滚中。若将来把
    // rollInvulnTime 调得离 rollDuration 很近（差 ≤ 1 步），测试会在
    // 断言 isRolling 处失败，且报错信息完全指不到真正的原因。
    ASSERT_LT(invuln + kFixedDelta, duration)
        << "测试前提不成立：无敌窗口 + 一个固定步之后仍应处于翻滚中";

    while (gameTime < invuln + kFixedDelta) {
        step(*world, gameTime, 1);
    }

    EXPECT_TRUE(player->isRolling());
    EXPECT_FALSE(player->isInvulnerable()) << "无敌帧没有在 rollInvulnTime 之后结束";

    const float dealt = world->applyDamage(player->id(), 10.0f, 0, player->position(), gameTime);
    EXPECT_GT(dealt, 0.0f) << "无敌窗口结束后仍然打不掉血";
    EXPECT_LT(player->health(), healthBefore);
}

TEST(DodgeRollTest, ProjectileIsAbsorbedWithoutDamageDuringInvulnerability) {
    // 这条测试覆盖的是"翻滚躲子弹"的真实交战路径，而不只是 applyDamage 这个函数。
    // 它同时验证三件事：
    //   1. 子弹确实被挡住，玩家不掉血
    //   2. 子弹被吸收（不会穿过去，等无敌结束再从背后命中）
    //   3. "闪避"计数真的在涨——否则无敌帧就是一条无法观测的规则
    auto world = makeCleanWorld({400.0f, 600.0f});
    double gameTime = 0.0;

    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    const float healthBefore = player->health();

    player->setMoveIntent({1.0f, 0.0f});   // 朝右滚
    player->setLookAtPoint(player->position() + glm::vec2{1.0f, 0.0f});
    ASSERT_TRUE(player->tryStartRoll(gameTime));

    // 从玩家正前方近距离朝身后打一发高速子弹。
    // 推进方向与翻滚方向相反 ⇒ 两者相向而行，几个逻辑步内必相遇，
    // 命中时刻稳稳落在 0.25s 的无敌窗口里。
    Game::ProjectileConfig config = Game::defaultEnemyProjectile();
    config.speed    = 1200.0f;
    config.damage   = 5.0f;
    config.maxRange = 300.0f;
    config.muzzleOffset = 16.0f;

    ASSERT_NE(world->spawnProjectile(0, Game::Faction::Enemy,
                                     glm::vec2{460.0f, 600.0f},
                                     glm::vec2{-1.0f, 0.0f},
                                     config),
              nullptr);

    // 跑到子弹落定
    int guard = 0;
    while (world->projectileCount() > 0 && guard < 60) {
        step(*world, gameTime, 1);
        ++guard;
    }

    EXPECT_EQ(world->projectileCount(), 0) << "子弹既没命中也没飞出射程，状态异常";
    EXPECT_FLOAT_EQ(player->health(), healthBefore) << "无敌帧内被子弹打掉了血";
    EXPECT_EQ(world->totalProjectilesDodged(), 1u) << "闪避计数没有记录这次格挡";
    EXPECT_EQ(world->totalProjectileHits(), 0u) << "不该被算成一次命中";
    EXPECT_FLOAT_EQ(world->totalDamageDealt(), 0.0f);
}
