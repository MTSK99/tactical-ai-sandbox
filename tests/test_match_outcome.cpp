// ===========================================================================
//  tests/test_match_outcome.cpp
//
//  对局胜负判定 + 重开闭环的回归测试。
//
//  【背景：这条测试线为什么存在】
//  项目曾经**没有任何胜负概念**。打完一局的结果是：
//
//    - 敌人全清完了 → 什么都不发生
//    - 玩家被打死 → 什么都不发生，而且世界从此停摆
//
//  第二条尤其严重。根因在 World::collectPerceptionTargets()：
//  它只在 `target.alive` 为真时才把玩家放进感知列表，于是玩家一死，
//  敌人就集体失去目标、行为树回落到巡逻分支，世界上再也不会有子弹。
//  实测证据：30 秒与 60 秒两次无头运行的统计**完全相同**
//  （shots=20 / dmg=100 / kills=0 / enemies 4/4）——
//  模拟彻底停摆，连"观察失败现场"都做不到。
//
//  这一组测试把「结束」变成一个可断言的状态，并盯住两个曾经出错的判定：
//   1. --enemies 0（从未生成过敌人）不能被判成胜利
//   2. reset() 必须清掉"曾经生成过敌人"，否则重开瞬间就显示胜利
// ===========================================================================
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "game/Player.h"
#include "game/World.h"

using namespace TacticalAI;

namespace {

constexpr double kFixedDelta = 1.0 / 60.0;

std::unique_ptr<Game::World> makeWorld(int enemyCount,
                                       bool demoObstacles = false) {
    Game::WorldConfig config;
    config.generateDemoObstacles = demoObstacles;
    config.initialEnemyCount = enemyCount;
    config.playerStart = {400.0f, 600.0f};

    auto world = std::make_unique<Game::World>(config);
    world->initialize();
    return world;
}

/// 推进若干逻辑步。outcome() 是实时计算的，所以世界必须真的跑起来。
void step(Game::World& world, int frames) {
    double t = 0.0;
    for (int i = 0; i < frames; ++i) {
        world.update(kFixedDelta, t);
        t += kFixedDelta;
    }
}

/// 同上，但把时钟交给调用方保管，跨多次调用保持单调递增。
///
/// 【为什么需要它】上面的 step() 每次都从 0 重新开始，于是 gameTime 会在
/// 两次调用之间"回拨"。任何依赖时间差的逻辑（冷却、搜索计时、本局用时）
/// 遇到负增量都会算出荒唐的结果——用它测这些行为是测不准的。
/// 需要跨调用累计时间时请用这个版本。
void stepAt(Game::World& world, double& gameTime, int frames) {
    for (int i = 0; i < frames; ++i) {
        world.update(kFixedDelta, gameTime);
        gameTime += kFixedDelta;
    }
}

/// 直接把玩家打死（绕过射击，便于确定性地构造失败局面）。
void killPlayer(Game::World& world) {
    auto* player = world.player();
    ASSERT_NE(player, nullptr);
    // 伤害要够大且调用方不会传 0/负数（applyDamage 有 amount <= 0 保护）
    world.applyDamage(player->id(), 10000.0f, 0, player->position(), 0.0);
}

} // namespace

// ---------------------------------------------------------------------------
//  Running：一切正常时不应误判
// ---------------------------------------------------------------------------

TEST(MatchOutcomeTest, RunningWhileEnemiesAreAlive) {
    auto world = makeWorld(3);

    EXPECT_EQ(world->outcome(), Game::MatchOutcome::Running);
    EXPECT_FALSE(world->isMatchOver());

    // 跑一段真实模拟，AI 会动但不会死，结论必须一直是 Running
    step(*world, 120);
    EXPECT_EQ(world->outcome(), Game::MatchOutcome::Running);
}

TEST(MatchOutcomeTest, RunningWithNoEnemiesSpawned) {
    // 【关键回归：--enemies 0 不能开局即"胜利"】
    //
    // 一局里从来没有过敌人，与"打赢了"是两件事。若只判
    // aliveEnemyCount() == 0，开局第一帧就会显示胜利。
    auto world = makeWorld(0);

    EXPECT_EQ(world->outcome(), Game::MatchOutcome::Running)
        << "从未生成过敌人时不应判为胜利";
}

// ---------------------------------------------------------------------------
//  Defeat
// ---------------------------------------------------------------------------

TEST(MatchOutcomeTest, DefeatWhenPlayerDies) {
    auto world = makeWorld(2);
    ASSERT_EQ(world->outcome(), Game::MatchOutcome::Running);

    killPlayer(*world);

    EXPECT_EQ(world->outcome(), Game::MatchOutcome::Defeat);
    EXPECT_TRUE(world->isMatchOver());
}

TEST(MatchOutcomeTest, DefeatTakesPriorityOverVictory) {
    // 最后一击可能同时打死玩家和最后一个敌人。
    // 两者都成立时必须显示"失败"——"我方团灭"比"敌人清空"更需要被告知。
    auto world = makeWorld(1);
    ASSERT_EQ(world->aliveEnemyCount(), 1u);

    // 先打死玩家，再把唯一的敌人也清掉
    killPlayer(*world);
    for (auto* enemy : world->enemies()) {
        world->applyDamage(enemy->id(), 10000.0f, 0, enemy->position(), 0.0);
    }

    EXPECT_EQ(world->aliveEnemyCount(), 0u);
    EXPECT_EQ(world->outcome(), Game::MatchOutcome::Defeat)
        << "玩家与最后一个敌人同时死亡时，失败应优先于胜利";
}

TEST(MatchOutcomeTest, WorldKeepsSimulatingAfterDefeat) {
    // 【关键回归：失败后世界不能停摆】
    //
    // 这是"打完没结果"的深层原因。旧版玩家一死，敌人就集体转入巡逻、
    // 再也不开火，统计数字从此不再变化——看起来像崩溃，实则是静默失效。
    // 修复后世界必须继续推进：实体仍在更新，子弹仍在飞。
    //
    // 断言方式刻意选"逻辑仍在跑"而不是"敌人还在开枪"：
    // 后者依赖 AI 行为与视线，脆弱得多。
    auto world = makeWorld(3);

    killPlayer(*world);
    ASSERT_EQ(world->outcome(), Game::MatchOutcome::Defeat);

    // 【修复：原来的断言是弱代理】
    // 它只看 elapsedGameTime() > 0，而那个字段是外部时钟的拷贝
    // （World::update 里无条件赋值），step() 自己把 t 从 0 递增——
    // 所以它只证明了"我调用了 update 60 次"，与仿真是否推进毫无关系：
    // 即使 AI 全停、子弹不再生成，它照样大于 0。
    // 真正该看的是"世界里的东西还在动"。
    std::vector<glm::vec2> positionBefore;
    positionBefore.reserve(world->enemies().size());
    for (const Game::Enemy* enemy : world->enemies()) {
        positionBefore.push_back(enemy->position());
    }
    ASSERT_FALSE(positionBefore.empty());

    step(*world, 300);   // 5 秒

    const std::vector<Game::Enemy*> positionAfter = world->enemies();
    ASSERT_EQ(positionAfter.size(), positionBefore.size());

    float maxMoved = 0.0f;
    for (size_t i = 0; i < positionBefore.size(); ++i) {
        const glm::vec2 delta = positionAfter[i]->position() - positionBefore[i];
        const float moved = std::sqrt(delta.x * delta.x + delta.y * delta.y);
        maxMoved = std::max(maxMoved, moved);
    }

    EXPECT_GT(maxMoved, 1.0f)
        << "失败后世界停摆：5 秒里没有任何敌人移动过（最远位移 "
        << maxMoved << "）——玩家死后 AI 应当转入巡逻继续活动";

    // 玩家应当处于死亡状态（而非被重置）
    EXPECT_TRUE(world->player()->isDead());
}

TEST(MatchOutcomeTest, PlayerCanClearTheLevelByShooting) {
    // 【端到端：这一局到底能不能玩】
    //
    // 瞄准 → 开火 → 子弹生成 → 飞行 → 命中 → 扣血 → 敌人死亡 → 判定胜利，
    // 这条链上任何一环断掉（朝向没生效、子弹没生成、命中判定失效、
    // 伤害没结算、胜负不判定）都会让这条测试失败。它是"可玩"最小闭环的
    // 自动化版本——比手工试玩更可靠，因为可以反复跑。
    auto world = makeWorld(0, /*demoObstacles=*/false);
    double t = 0.0;

    Game::Player* player = world->player();
    ASSERT_NE(player, nullptr);

    // 在玩家正前方 260 单位放一个"靶子"：
    // 移动速度与射速都设为极慢/极慢，让结果完全由玩家的射击决定。
    // （否则敌人会侧移躲避，命中与否取决于随机时机，测试会变得脆弱。
    //   那是另一条测试要管的事——这里要验证的是战斗闭环本身。）
    Game::Enemy* enemy = world->spawnEnemy(player->position() + glm::vec2{260.0f, 0.0f});
    ASSERT_NE(enemy, nullptr);
    enemy->config().moveSpeed = 0.0f;
    enemy->config().sprintSpeed = 0.0f;
    enemy->config().fireCooldown = 1000.0f;   // 不还手

    const int enemyId = static_cast<int>(enemy->id());

    bool killed = false;
    for (int i = 0; i < 600 && !killed; ++i) {
        if (!enemy->isDead()) {
            player->setLookAtPoint(enemy->position());
            player->requestFire();
        }
        stepAt(*world, t, 1);
        killed = (world->aliveEnemyCount() == 0);
    }

    ASSERT_TRUE(killed)
        << "10 秒内一发都没打死敌人（敌人 hp 还剩 " << enemy->health()
        << "）——开火/子弹/命中/伤害这条链有问题";
    EXPECT_GT(world->enemiesKilled(), 0);
    EXPECT_GT(world->totalShotsFired(), 0u);
    EXPECT_GT(world->totalProjectileHits(), 0u);
    (void)enemyId;

    stepAt(*world, t, 120);   // 等残余弹幕落地
    EXPECT_EQ(world->outcome(), Game::MatchOutcome::Victory)
        << "敌人已全灭，却没有判定胜利";
}

TEST(MatchOutcomeTest, VictoryWaitsForEnemyProjectilesToLand) {
    // 【回归：胜利不能在"敌方弹幕还在飞"的时候就宣布】
    //
    // 子弹是独立实体，不随主人消失。击杀最后一个敌人的那一瞬间，它可能
    // 刚好射出一发还在空中的子弹；若此时立刻判胜利，下一帧这发子弹把玩家
    // 打死，结算就会从"胜利"翻成"失败"——玩家看到的是"我赢了，然后突然输了"。
    auto world = makeWorld(1);
    double t = 0.0;
    stepAt(*world, t, 1);

    const std::vector<Game::Enemy*> enemies = world->enemies();
    ASSERT_EQ(enemies.size(), 1u);
    Game::Enemy* enemy = enemies.front();

    // 造一发"慢慢飞"的敌方子弹：射程 400 / 速度 400 = 1 秒后才自然终止
    Game::ProjectileConfig config = Game::defaultEnemyProjectile();
    config.speed    = 400.0f;
    config.maxRange = 400.0f;

    Game::Projectile* shot = world->spawnProjectile(
        enemy->id(), Game::Faction::Enemy, enemy->position(),
        glm::vec2{1.0f, 0.0f}, config);
    ASSERT_NE(shot, nullptr);

    // 击杀全部敌人：此时战场上只剩这发在飞的敌方子弹
    for (Game::Enemy* e : world->enemies()) {
        world->applyDamage(e->id(), 10000.0f, 0, e->position(), t);
    }
    ASSERT_EQ(world->aliveEnemyCount(), 0u);
    ASSERT_TRUE(world->hasLiveEnemyProjectile()) << "前提：敌方子弹必须仍在飞行";

    EXPECT_EQ(world->outcome(), Game::MatchOutcome::Running)
        << "敌方弹幕还在空中就宣布了胜利——一旦它打中玩家，结算会翻转";

    stepAt(*world, t, 150);   // 2.5 秒，足够子弹飞完射程
    EXPECT_FALSE(world->hasLiveEnemyProjectile());
    EXPECT_EQ(world->outcome(), Game::MatchOutcome::Victory)
        << "弹幕落地后应当判定胜利";
}

// ---------------------------------------------------------------------------
//  Victory
// ---------------------------------------------------------------------------

TEST(MatchOutcomeTest, VictoryWhenAllEnemiesCleared) {
    auto world = makeWorld(3);
    ASSERT_EQ(world->outcome(), Game::MatchOutcome::Running);

    for (auto* enemy : world->enemies()) {
        world->applyDamage(enemy->id(), 10000.0f, 0, enemy->position(), 0.0);
    }

    EXPECT_EQ(world->outcome(), Game::MatchOutcome::Victory);
    EXPECT_TRUE(world->isMatchOver());
}

TEST(MatchOutcomeTest, VictoryRequiresPlayerAlive) {
    auto world = makeWorld(2);

    killPlayer(*world);
    for (auto* enemy : world->enemies()) {
        world->applyDamage(enemy->id(), 10000.0f, 0, enemy->position(), 0.0);
    }

    EXPECT_EQ(world->outcome(), Game::MatchOutcome::Defeat)
        << "玩家已死时不该显示胜利";
}

// ---------------------------------------------------------------------------
//  沙盒语义：胜利不是关上的门
// ---------------------------------------------------------------------------

TEST(MatchOutcomeTest, SpawningEnemyAfterVictoryRestoresRunning) {
    // 沙盒里按 N 可以随时增兵，因此"胜利 → 增兵 → 又是 Running"
    // 是刻意允许的：它让胜利成为一个可以随时看一眼的状态。
    auto world = makeWorld(1);

    for (auto* enemy : world->enemies()) {
        world->applyDamage(enemy->id(), 10000.0f, 0, enemy->position(), 0.0);
    }
    ASSERT_EQ(world->outcome(), Game::MatchOutcome::Victory);

    ASSERT_NE(world->spawnEnemy(), nullptr);

    EXPECT_EQ(world->outcome(), Game::MatchOutcome::Running)
        << "增兵后应回到进行中——沙盒不因胜利而锁死";
}

// ---------------------------------------------------------------------------
//  reset：重开必须真正回到干净状态
// ---------------------------------------------------------------------------

TEST(MatchOutcomeTest, ResetClearsDefeat) {
    auto world = makeWorld(2);

    killPlayer(*world);
    ASSERT_EQ(world->outcome(), Game::MatchOutcome::Defeat);

    world->reset();

    EXPECT_EQ(world->outcome(), Game::MatchOutcome::Running)
        << "重开后不应仍是失败";
    ASSERT_NE(world->player(), nullptr);
    EXPECT_FALSE(world->player()->isDead())
        << "重开后玩家应满血复活";
    EXPECT_EQ(world->aliveEnemyCount(), world->enemyCount())
        << "重开后敌人应全部存活";
}

TEST(MatchOutcomeTest, ResetClearsVictoryFlagBeforeFirstSpawn) {
    // 【关键回归：spawnedAnyEnemy_ 必须在 reset 里清掉】
    //
    // 若忘了清，重开流程会这样出错：reset() 把实体清空 → aliveEnemyCount
    // 变成 0 → 但 spawnedAnyEnemy_ 仍是 true（上一局生成过）→
    // initialize() 还没生成第一个敌人，outcome() 就判成 Victory。
    // 表现为"刚按回车就显示胜利"，且与真实战况无关。
    //
    // 这个断言刻意选在 reset() 之后、任何新敌人生成之前的那一瞬。
    auto world = makeWorld(2);

    for (auto* enemy : world->enemies()) {
        world->applyDamage(enemy->id(), 10000.0f, 0, enemy->position(), 0.0);
    }
    ASSERT_EQ(world->outcome(), Game::MatchOutcome::Victory);

    world->reset();

    EXPECT_EQ(world->outcome(), Game::MatchOutcome::Running)
        << "reset 后、生成首敌前不应残留胜利状态";
}

TEST(MatchOutcomeTest, ResetRestoresStatistics) {
    auto world = makeWorld(2);
    double t = 0.0;

    // --- 前置：先制造"非零"的统计，否则断言 0 == 0 是同义反复 ---
    //
    // 原版这里只 killPlayer + step(30)：而 applyDamage 并不累加射击数/
    // 伤害统计，0.5 秒里（敌人冷却 0.95s）也几乎不可能开火——于是 reset
    // 前后这些量本来就都是 0，"reset 是否清零"根本没被验证。
    Game::Player* player = world->player();
    ASSERT_NE(player, nullptr);
    player->setLookAtPoint(player->position() + glm::vec2{300.0f, 0.0f});
    stepAt(*world, t, 1);        // 让朝向生效
    player->requestFire();
    stepAt(*world, t, 1);        // 子弹入世

    ASSERT_GT(world->totalShotsFired(), 0u)
        << "前置条件不成立：一枪都没开出来，无法验证 reset 的清零";

    // 跑够一段时间，让"本局用时"积累到明显大于 0
    stepAt(*world, t, 300);      // 5 秒
    killPlayer(*world);
    stepAt(*world, t, 1);

    const double elapsedBeforeReset = world->matchElapsedTime();
    ASSERT_GT(elapsedBeforeReset, 4.0) << "前置：本局用时应当已经积累了几秒";

    world->reset();

    EXPECT_EQ(world->totalShotsFired(), 0u);
    EXPECT_EQ(world->totalProjectileHits(), 0u);
    EXPECT_EQ(world->totalProjectilesDodged(), 0u);
    EXPECT_EQ(world->enemiesKilled(), 0);
    EXPECT_FLOAT_EQ(world->totalDamageDealt(), 0.0f);

    // --- 重开后的"本局用时"必须重新开始 ---
    //
    // 【这里曾经是假通过】原断言写的是 elapsedGameTime() == 0，而它只在
    // reset 之后**不再推进**时成立：elapsedGameTime_ 是外部时钟的拷贝，
    // update() 每一步都会把它写回去，重开跑一步就恢复成上一局的绝对时间。
    // 所以必须"reset 后再跑一段"才有检验力。
    stepAt(*world, t, 60);       // 1 秒

    EXPECT_LT(world->matchElapsedTime(), 2.0)
        << "重开后本局用时仍从 " << world->matchElapsedTime()
        << " 秒开始（重开前是 " << elapsedBeforeReset << " 秒）——计时没有归零";
}

// ---------------------------------------------------------------------------
//  toString：结算画面直接渲染它，中文必须对
// ---------------------------------------------------------------------------

TEST(MatchOutcomeTest, SettledTimeFreezesWhenMatchEnds) {
    // 【需求】结算横幅上的"用时/坚持"必须停在分出胜负的那一刻。
    //
    // 背景：分出胜负之后世界**仍在推进**（敌人退巡逻、子弹落地、尸体消失），
    // 所以实时用时（matchElapsedTime）还会一直涨。横幅如果用实时值，
    // 数字就像个忘了停的秒表——玩家盯着一场已经结束的对局看，会以为还没打完。
    auto world = makeWorld(1, /*demoObstacles=*/false);
    double t = 0.0;
    stepAt(*world, t, 120);   // 先打 2 秒

    for (Game::Enemy* enemy : world->enemies()) {
        world->applyDamage(enemy->id(), 10000.0f, 0, enemy->position(), t);
    }
    stepAt(*world, t, 1);
    ASSERT_EQ(world->outcome(), Game::MatchOutcome::Victory);

    const double settledAtVictory = world->settledElapsedTime();
    const double liveAtVictory    = world->matchElapsedTime();
    EXPECT_GT(settledAtVictory, 1.0) << "胜利时刻的本局用时应当已经积累了几秒";

    // 胜利后再跑 5 秒：实时用时继续涨（世界确实在推进），结算用时必须冻结
    stepAt(*world, t, 300);
    EXPECT_GT(world->matchElapsedTime(), liveAtVictory + 4.0)
        << "前提：胜负已分后世界仍应继续推进";
    EXPECT_DOUBLE_EQ(world->settledElapsedTime(), settledAtVictory)
        << "胜负已定，结算横幅的用时却还在变（" << settledAtVictory
        << " → " << world->settledElapsedTime() << "）";

    // 增兵续战：结果回到 Running，结算用时恢复为实时值（继续计时）
    ASSERT_NE(world->spawnEnemy(), nullptr);
    ASSERT_EQ(world->outcome(), Game::MatchOutcome::Running);
    EXPECT_DOUBLE_EQ(world->settledElapsedTime(), world->matchElapsedTime())
        << "续战后应当继续计时，而不是卡在上一局的结算数字上";
}

TEST(MatchOutcomeTest, ToStringRendersChineseLabels) {
    EXPECT_STREQ(toString(Game::MatchOutcome::Running), "进行中");
    EXPECT_STREQ(toString(Game::MatchOutcome::Victory), "胜利");
    EXPECT_STREQ(toString(Game::MatchOutcome::Defeat), "失败");
}
