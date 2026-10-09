// ===========================================================================
//  tests/test_combat_cooldown.cpp
//
//  开火冷却与击杀节奏（TTK）的回归测试。
//
//  【这组测试来自一个真实的 Bug】
//  玩家反馈："敌方攻击没有冷却，能秒杀我"。
//
//  查下去发现的根因非常难看：EnemyConfig 里一直声明着 fireCooldown，
//  调参面板里甚至有一根"射击间隔"滑块在改它，但 **AimAndFire 从来没有读过它**。
//  那一行注释写着"带冷却"，代码里没有任何冷却。于是 AI 只要把枪口对准玩家，
//  就会每个逻辑步开一枪：
//
//      60 Hz × 4 点伤害 = 240 DPS  →  100 血的玩家在 0.42 秒内被打空
//
//  "秒杀"根本不是夸张，是字面意义上的 0.4 秒。
//
//  这类 bug 的危险之处在于它**看起来是对的**：参数在、UI 在、注释在，
//  只有运行时行为不对。因此测试必须断言"实际射速"，而不只是"参数存在"。
// ===========================================================================
#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

#include "game/Enemy.h"
#include "game/Player.h"
#include "game/World.h"

using namespace TacticalAI;

namespace {

constexpr double kFixedDelta = 1.0 / 60.0;

std::unique_ptr<Game::World> makeWorld(int enemies) {
    Game::WorldConfig config;
    config.initialEnemyCount = enemies;
    auto world = std::make_unique<Game::World>(config);
    world->initialize();
    return world;
}

void step(Game::World& world, double& gameTime, int steps) {
    for (int i = 0; i < steps; ++i) {
        world.update(kFixedDelta, gameTime);
        gameTime += kFixedDelta;
    }
}

/// 把玩家变成"打不死"，用于测量**射速**。
///
/// 【为什么必须这么做】
/// 玩家一死，collectPerceptionTargets 就不再把他放进感知列表，敌人立刻
/// 失去目标停火；而伤害统计又被玩家 100 点血量封顶。于是所有以"累计伤害 /
/// 累计射击数"为指标的断言都会饱和——最极端的情况下，"冷却完全没接线"
/// （240 DPS）也能通过一条写着"DPS 应小于 12"的断言，因为玩家 0.4 秒
/// 就被打死，总伤害永远不超过 100。
/// 让玩家打不死，指标才真正反映射速。
void makePlayerInvincible(Game::World& world) {
    Game::Player* player = world.player();
    if (player == nullptr) {
        return;
    }
    player->setMaxHealth(1.0e6f);
    player->setHealth(1.0e6f);
}

} // namespace

// ---------------------------------------------------------------------------
//  敌人冷却
// ---------------------------------------------------------------------------
TEST(CombatCooldownTest, EnemyNeverExceedsItsFireCooldown) {
    // 核心断言：任何时刻的累计射速都不能超过 1 / fireCooldown。
    //
    // 【为什么要用"不超过"而不是"等于"】
    // 敌人开火还需要"看见目标 + 转向对准"两件事，所以真实射速通常低于
    // 上限。上限是硬件约束，下限取决于 AI 状态——只有上限是可断言的契约。
    auto world = makeWorld(4);
    double gameTime = 0.0;

    // 玩家打不死：否则他在 5 秒左右阵亡，敌人随即停火，
    // 观测窗口里剩下一半时间什么都没有，"超限"也就无从谈起。
    makePlayerInvincible(*world);

    constexpr double kObserveSeconds = 30.0;
    const int totalSteps = static_cast<int>(kObserveSeconds / kFixedDelta);

    for (int i = 0; i < totalSteps; ++i) {
        world->update(kFixedDelta, gameTime);
        gameTime += kFixedDelta;
    }

    const std::vector<Game::Enemy*> enemies = world->enemies();
    ASSERT_FALSE(enemies.empty());

    size_t totalEnemyShots = 0;
    for (const Game::Enemy* enemy : enemies) {
        const double cooldown = enemy->config().fireCooldown;
        ASSERT_GT(cooldown, 0.0);

        // 允许一个"起步"的余量：第一枪不受冷却限制。
        // +1 而不是 +0.5，避免浮点边界把合法结果判成失败。
        const double maxAllowed = kObserveSeconds / cooldown + 1.0;

        EXPECT_LE(static_cast<double>(enemy->shotsFired()), maxAllowed)
            << enemy->name() << " 在 " << kObserveSeconds << " 秒内开了 "
            << enemy->shotsFired() << " 枪，超过冷却上限 " << maxAllowed
            << " 枪（冷却 " << cooldown << "s）——冷却又没接线";
        totalEnemyShots += static_cast<size_t>(enemy->shotsFired());
    }

    // 同时要确保敌人确实开了火，否则上面的断言是空跑
    EXPECT_GT(totalEnemyShots, 0u) << "30 秒里敌人一枪都没开，测试没有意义";
}

TEST(CombatCooldownTest, HugeCooldownAllowsAtMostOneShotPerEnemy) {
    // 把一个敌人的冷却设成 1000 秒——它这一辈子最多只能开一枪。
    //
    // 这是对冷却逻辑最锋利的探测：只要冷却根本没被读取（也就是那个
    // 原始 bug），射速就会退回 60 发/秒，这个测试必然爆红。
    auto world = makeWorld(3);
    double gameTime = 0.0;

    for (Game::Enemy* enemy : world->enemies()) {
        enemy->config().fireCooldown = 1000.0f;
    }

    const int totalSteps = static_cast<int>(40.0 / kFixedDelta);   // 40 秒
    for (int i = 0; i < totalSteps; ++i) {
        world->update(kFixedDelta, gameTime);
        gameTime += kFixedDelta;
    }

    size_t totalShots = 0;
    for (const Game::Enemy* enemy : world->enemies()) {
        EXPECT_LE(enemy->shotsFired(), 1)
            << enemy->name() << " 在 1000 秒冷却下开了 " << enemy->shotsFired()
            << " 枪——冷却完全没有生效";
        totalShots += static_cast<size_t>(enemy->shotsFired());
    }

    // 【补充】上界断言对一个"完全不开火"的 AI 也是满足的（0 ≤ 1）。
    // 这里补一个下界，让用例不至于空跑：只要敌人真的接触到了玩家，
    // 1000 秒冷却下它必然正好开过一枪。
    EXPECT_GT(totalShots, 0u)
        << "40 秒里没有任何敌人开过枪，这条用例没测到冷却逻辑";
}

TEST(CombatCooldownTest, ShorterCooldownProducesStrictlyMoreShots) {
    // 冷却参数必须真的**影响**射速，而不只是"不超限"。
    // 用同一场景跑两遍，一遍冷却 2 秒、一遍 0.2 秒，后者必然打得多。
    //
    // 【为什么需要这条】
    // "不超过上限"的断言可以被一个"几乎从不开火"的 AI 满足——
    // 那样测试是绿的，但参数全都没起作用。这条测试排除这种情况。
    const auto shootCount = [](float cooldown) {
        auto world = makeWorld(4);
        double gameTime = 0.0;

        // 【修复测试前提】原来玩家会被 4 个敌人打死，一旦死亡敌人就失去
        // 目标、射击停止 —— 于是两组都被"打死玩家所需的枪数"这一个
        // 上限截断，冷却快慢根本体现不出来（实测两组都是 21 枪）。
        // 之前测试之所以能通过，只是因为敌人很少真正进入交战（反应式
        // 选择器每帧重启交战分支，瞄准动作无法完成）。修好那个 bug 后
        // 敌人正常交战，这个隐藏的前提缺陷就暴露了。
        //
        // 让玩家不会被打死，指标才真正反映射速。
        // 注意：这里不能用 ASSERT_* —— 它要求所在函数返回 void，
        // 而本 lambda 返回 size_t。
        if (Game::Player* player = world->player()) {
            player->setMaxHealth(1.0e6f);
            player->setHealth(1.0e6f);
        }

        for (Game::Enemy* enemy : world->enemies()) {
            enemy->config().fireCooldown = cooldown;
        }

        const int totalSteps = static_cast<int>(25.0 / kFixedDelta);
        for (int i = 0; i < totalSteps; ++i) {
            world->update(kFixedDelta, gameTime);
            gameTime += kFixedDelta;
        }
        return world->totalShotsFired();
    };

    const size_t slowShots = shootCount(2.0f);
    const size_t fastShots = shootCount(0.2f);

    EXPECT_GT(fastShots, slowShots)
        << "把冷却从 2.0s 调到 0.2s 却没打出更多子弹，说明冷却参数没有被使用"
        << "（慢=" << slowShots << "，快=" << fastShots << "）";
}

// ---------------------------------------------------------------------------
//  玩家冷却
// ---------------------------------------------------------------------------
TEST(CombatCooldownTest, PlayerCannotFireTwiceWithinCooldown) {
    auto world = makeWorld(0);
    double gameTime = 0.0;
    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    player->setLookAtPoint(player->position() + glm::vec2{400.0f, 0.0f});
    step(*world, gameTime, 1);   // 让朝向生效

    player->requestFire();
    world->update(kFixedDelta, gameTime);
    gameTime += kFixedDelta;
    EXPECT_EQ(world->totalShotsFired(), 1u);

    // 冷却期内的第二次请求必须被吞掉
    player->requestFire();
    world->update(kFixedDelta, gameTime);
    gameTime += kFixedDelta;
    EXPECT_EQ(world->totalShotsFired(), 1u)
        << "冷却期内的开火请求没有被拦住，玩家可以连点刷弹幕";
}

TEST(CombatCooldownTest, PlayerCanFireAgainAfterCooldownElapses) {
    auto world = makeWorld(0);
    double gameTime = 0.0;
    auto* player = world->player();

    player->setLookAtPoint(player->position() + glm::vec2{400.0f, 0.0f});
    step(*world, gameTime, 1);

    player->requestFire();
    step(*world, gameTime, 1);
    ASSERT_EQ(world->totalShotsFired(), 1u);

    // 冷却 0.15s = 9 步，等 11 步确保稳过
    const double cooldown = static_cast<double>(player->fireCooldown());
    const int waitSteps = static_cast<int>(cooldown / kFixedDelta) + 2;
    step(*world, gameTime, waitSteps);

    EXPECT_LE(player->fireCooldownRemaining(gameTime), 0.0)
        << "等了超过冷却时间，武器却仍显示未就绪";

    player->requestFire();
    step(*world, gameTime, 1);

    EXPECT_EQ(world->totalShotsFired(), 2u) << "冷却结束后无法再次开火";
}

TEST(CombatCooldownTest, PlayerCooldownRemainingIsReported) {
    auto world = makeWorld(0);
    double gameTime = 0.0;
    auto* player = world->player();

    // 从未开火：应当显示"已就绪"
    EXPECT_LE(player->fireCooldownRemaining(gameTime), 0.0);

    player->setLookAtPoint(player->position() + glm::vec2{400.0f, 0.0f});
    step(*world, gameTime, 1);
    player->requestFire();
    step(*world, gameTime, 1);

    // 刚开完火：剩余冷却应当接近满值且为正
    const double remaining = player->fireCooldownRemaining(gameTime);
    EXPECT_GT(remaining, 0.0);
    EXPECT_LE(remaining, static_cast<double>(player->fireCooldown()));
}

// ---------------------------------------------------------------------------
//  击杀节奏（TTK）—— 直接对应"能秒杀我"
// ---------------------------------------------------------------------------
TEST(CombatCooldownTest, PlayerIsNotInstantlyKilledByDefaultEnemies) {
    // 这是对用户原话"敌方攻击没有冷却，能秒杀我"最直接的回归保护。
    //
    // 修复前的实际数据：3 个敌人，玩家站着不动，约 **0.4 秒** 被打成 0 血。
    // 本测试要求玩家至少撑过 3 秒——比修复前宽 7 倍以上，
    // 因此它既能挡住"冷却再次失效"，也不会因为小幅调参而误报。
    auto world = makeWorld(3);
    double gameTime = 0.0;

    constexpr double kMustSurviveSeconds = 3.0;
    const int stepsToCheck = static_cast<int>(kMustSurviveSeconds / kFixedDelta);

    const auto* player = world->player();
    ASSERT_NE(player, nullptr);
    const float startingHealth = player->health();

    for (int i = 0; i < stepsToCheck; ++i) {
        world->update(kFixedDelta, gameTime);
        gameTime += kFixedDelta;
    }

    const auto* after = world->player();
    ASSERT_NE(after, nullptr);
    EXPECT_FALSE(after->isDead())
        << "玩家在 " << kMustSurviveSeconds << " 秒内就被打死了"
        << "（起始血量 " << startingHealth << "，冷却修复前约 0.4 秒即死）";
}

TEST(CombatCooldownTest, PlayerEventuallyTakesDamageWhenStandingInTheOpen) {
    // 反向保护：修冷却不能修成"敌人打不到人"。
    //
    // 一个站着不动、不找掩体的玩家**最终**应该被打死——这正是 AI 有效的证据。
    // 如果这条测试变绿的方式是"玩家永远不掉血"，那说明子弹系统坏了
    // （例如子弹一生成就消失、或命中判定永远不触发）。
    auto world = makeWorld(3);
    double gameTime = 0.0;

    const int totalSteps = static_cast<int>(60.0 / kFixedDelta);   // 60 秒

    for (int i = 0; i < totalSteps; ++i) {
        world->update(kFixedDelta, gameTime);
        gameTime += kFixedDelta;
    }

    EXPECT_GT(world->totalDamageDealt(), 0.0f)
        << "60 秒里没有造成任何伤害——敌人根本没打中过玩家";
    EXPECT_GT(world->totalProjectileHits(), 0u)
        << "没有任何子弹命中过实体，命中判定可能坏了";
}

TEST(CombatCooldownTest, EnemyDamagePerSecondStaysWithinSanityBound) {
    // 把"秒杀"这件事量化成一条可断言的不等式。
    //
    // 修复前：4 点伤害 × 60 发/秒 = 240 DPS/敌人（玩家 0.42 秒即死）。
    // 修复后理论上限：伤害 / 冷却 = 5 / 0.95 ≈ 5.3 DPS/敌人。
    //
    // 这里放宽到 12 DPS/敌人 作为"理智上界"——它比修复前低 20 倍，
    // 足以挡住任何形式的"冷却失效"，同时给未来的调参留出空间
    // （比如把伤害调到 10 点仍然能通过）。
    auto world = makeWorld(3);
    double gameTime = 0.0;

    // 【修复：这条断言此前是恒真的】
    // totalDamageDealt_ 只在命中且实际掉血时累加，而玩家只有 100 血、
    // 死后伤害恒为 0 —— 于是 20 秒内的总伤害永远 ≤ 100，
    // perEnemyDps ≤ 100/20/3 ≈ 1.67，无论冷却有没有生效都小于 12。
    // 让玩家打不死，这个上界才真正描述"射速"。
    makePlayerInvincible(*world);

    constexpr double kObserveSeconds = 20.0;
    const int totalSteps = static_cast<int>(kObserveSeconds / kFixedDelta);

    for (int i = 0; i < totalSteps; ++i) {
        world->update(kFixedDelta, gameTime);
        gameTime += kFixedDelta;
    }

    const double totalDps = static_cast<double>(world->totalDamageDealt()) / kObserveSeconds;
    const double perEnemyDps = totalDps / 3.0;

    EXPECT_LT(perEnemyDps, 12.0)
        << "单个敌人的平均 DPS 达到 " << perEnemyDps
        << "，远超冷却允许的 5.3——冷却很可能没有生效";
}

TEST(CombatCooldownTest, FrozenAIDoesNotKeepFiring) {
    // 【回归：调试用的"冻结 AI"不能变成无限机枪】
    //
    // firedThisStep_ 的唯一清零点在 Enemy::updateAI 的开头。World 冻结 AI
    // 时不再调用 updateAI，如果不同时清掉这个标志，那么**冻结前一帧开过火
    // 的敌人会永久保持"本步要开火"**：每一步都被 processWeaponFire 发射一颗
    // 子弹，而且因为整棵行为树都被跳过，连开火冷却都不生效。
    // 玩家按 F2 想"停下来逐帧观察"，看到的却是一屏幕弹幕。
    auto world = makeWorld(4);
    makePlayerInvincible(*world);   // 保证敌人确实会开火
    double gameTime = 0.0;

    // 先等到至少有人开过一枪——否则"冻结后没增加"是废话
    bool firedBeforeFreeze = false;
    for (int i = 0; i < 60 * 30 && !firedBeforeFreeze; ++i) {
        world->update(kFixedDelta, gameTime);
        gameTime += kFixedDelta;
        firedBeforeFreeze = world->totalShotsFired() > 0;
    }
    ASSERT_TRUE(firedBeforeFreeze)
        << "前提不成立：30 秒内没有敌人开火，无法验证冻结行为";

    world->setAIEnabled(false);
    const size_t shotsAtFreeze = world->totalShotsFired();

    step(*world, gameTime, 120);   // 冻结状态下再跑 2 秒

    EXPECT_EQ(world->totalShotsFired(), shotsAtFreeze)
        << "AI 已冻结，射击数却从 " << shotsAtFreeze << " 涨到 "
        << world->totalShotsFired() << "——冰冻期间每步都在发射子弹";
}
