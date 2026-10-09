// ===========================================================================
//  tests/test_projectile.cpp
//
//  子弹实体的回归测试。
//
//  【背景：这条测试线为什么存在】
//  项目早期把攻击简化成"瞬时射线"——扣扳机的那一瞬间用线段求交决定命中，
//  然后补画一条线当作弹道。它带来两个无法通过调参解决的问题：
//
//    1. 攻击没有实体。子弹不占空间、不花时间，屏幕上那条线只是事后画的
//       装饰，与实际结算没有因果关系。
//    2. 攻击无法被躲避、也无法被掩体阻挡。命中在开火瞬间就定了，
//       之后目标的任何移动、任何地形都无法改变结果。
//
//  改成实体子弹后，上面两条性质变成了**可以在测试里断言的契约**：
//    - 开火后世界里必须多出一个 Projectile 实体（而不是立刻掉血）
//    - 伤害必须在"子弹到达"之后才发生（而不是扣扳机那一刻）
//    - 掩体必须真的挡住子弹
//    - 子弹必须能被打空（飞出射程后消失）
//
//  这一组测试就是在钉死这些契约，防止哪天有人为了"手感更爽"又把
//  命中改回瞬时结算——那会让上面两条问题一起复活。
// ===========================================================================
#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

#include "game/Player.h"
#include "game/Projectile.h"
#include "game/World.h"

using namespace TacticalAI;

namespace {

constexpr double kFixedDelta = 1.0 / 60.0;

} // namespace

// ---------------------------------------------------------------------------
//  配置一致性：AI 的开火距离不能超过自己的子弹射程
// ---------------------------------------------------------------------------

TEST(ProjectileTest, EnemyWeaponReachCoversItsFiringRange) {
    // AI 判定"能开火"的依据是 EnemyConfig::fireRange，而子弹能飞多远由
    // ProjectileConfig::maxRange 决定。两者一旦不一致，就会出现"AI 认真开枪、
    // 子弹在半路消失"——玩家看到的只是"敌人打空枪"，很难归因。
    //
    // 这条测试把两个数字锁在一起：以后谁单独调其中一个（比如只把射程调远
    // 方便 AI 还手），都会立刻在这里失败，而不是变成一个隐蔽的手感问题。
    const Game::EnemyConfig enemy;
    const Game::ProjectileConfig bullet = Game::defaultEnemyProjectile();

    EXPECT_GE(bullet.maxRange, enemy.fireRange)
        << "敌人开火距离 " << enemy.fireRange << " 超过子弹射程 "
        << bullet.maxRange << "：超出部分全是打空枪";
}

namespace {

/// 构造一个干净的世界：无示例障碍、无敌人，玩家放在指定位置。
///
/// 【为什么要关掉示例障碍物】
/// 默认场景里有一组手工设计的掩体，位置是"刻意不整齐"的战术地形。
/// 这些掩体会随机挡住测试用的弹道，让"子弹该不该命中"变得不可预测。
/// 关掉之后地形只剩网格边界，弹道完全可控，测试才有确定性。
std::unique_ptr<Game::World> makeCleanWorld(glm::vec2 playerStart = {400.0f, 600.0f}) {
    Game::WorldConfig config;
    config.generateDemoObstacles = false;
    config.initialEnemyCount = 0;
    config.playerStart = playerStart;

    auto world = std::make_unique<Game::World>(config);
    world->initialize();
    return world;
}

/// 把一个敌人变成"靶子"：既不移动，也不还击。
///
/// 【为什么需要】
/// 敌人是活的：它的行为树会巡逻、会选位、会朝玩家开火。如果不处理，
/// "子弹飞过去应该打中它"就变成了"子弹飞过去的时候它还在不在那、
/// 以及它是不是同时也打中了玩家"——测试会变得依赖一堆无关条件。
///
/// 具体做法：
///   - 期望速度与加速度清零 ⇒ 速度永远是零（完全静止）
///   - 开火射程设为 0 ⇒ 任何距离都判定为"太远"，永不开火
///
/// 这样测试里的敌人就退化成一块静止的靶板，弹道完全可控。
void makeTrainingDummy(Game::Enemy& enemy) {
    enemy.config().moveSpeed = 0.0f;
    enemy.config().acceleration = 0.0f;
    enemy.config().fireRange = 0.0f;
}

/// 跑若干逻辑步。
void step(Game::World& world, double& gameTime, int steps) {
    for (int i = 0; i < steps; ++i) {
        world.update(kFixedDelta, gameTime);
        gameTime += kFixedDelta;
    }
}

/// 一直跑到空中没有子弹为止。
///
/// 【为什么不能"跑固定步数然后断言弹着点"】
/// 弹着残影只活 0.11 秒（约 7 步）。子弹飞到 400 单位外需要约 26 步，
/// 因此如果一路跑满 45 步再检查，残影早已过期清理掉了——测的就不是
/// "有没有留下弹着点"，而是"残影活不活得够久"，完全跑偏。
/// 必须先跑到子弹落定，再立刻检查。
///
/// @return 子弹落定时经过的步数
int stepUntilProjectilesGone(Game::World& world, double& gameTime, int maxSteps = 300) {
    int used = 0;
    while (world.projectileCount() > 0 && used < maxSteps) {
        world.update(kFixedDelta, gameTime);
        gameTime += kFixedDelta;
        ++used;
    }
    return used;
}

/// 让玩家瞄准目标并开火。先走一步使朝向生效，再开火。
///
/// 【为什么不能在同一调用里做完】
/// 玩家的朝向是在实体更新阶段从 lookAtPoint 算出来的，而开火发生在
/// 更新阶段**之前**。因此"设置瞄准 → 开火"必须跨一步，否则子弹会
/// 沿上一帧的朝向飞出去。这也是真实操作里的时序。
void aimAndFire(Game::World& world, double& gameTime, glm::vec2 target) {
    auto* player = world.player();
    player->setLookAtPoint(target);
    step(world, gameTime, 1);   // 让朝向生效
    player->requestFire();
    step(world, gameTime, 1);   // 消费开火请求，子弹进入世界
}

} // namespace

// ---------------------------------------------------------------------------
//  开火产生实体
// ---------------------------------------------------------------------------
TEST(ProjectileTest, PlayerFireSpawnsProjectileEntity) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    auto* player = world->player();
    ASSERT_NE(player, nullptr);

    aimAndFire(*world, gameTime, player->position() + glm::vec2{300.0f, 0.0f});

    // 硬契约：开火产生的是一个**实体**，而不是一次立即结算的伤害。
    ASSERT_EQ(world->projectileCount(), 1u) << "开火没有产生子弹实体";

    const std::vector<Game::Projectile*> bullets = world->projectiles();
    ASSERT_EQ(bullets.size(), 1u);
    EXPECT_EQ(bullets[0]->type(), Game::EntityType::Projectile);
    EXPECT_EQ(bullets[0]->ownerId(), player->id());
    EXPECT_EQ(bullets[0]->faction(), Game::Faction::Player);
    EXPECT_FALSE(bullets[0]->terminated());

    EXPECT_EQ(world->totalShotsFired(), 1u);
}

TEST(ProjectileTest, ProjectileTravelsOverTimeInsteadOfArrivingInstantly) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;
    auto* player = world->player();

    aimAndFire(*world, gameTime, player->position() + glm::vec2{300.0f, 0.0f});

    ASSERT_EQ(world->projectiles().size(), 1u);
    Game::Projectile* bullet = world->projectiles()[0];

    const float speed = bullet->config().speed;
    const float expectedStep = speed * static_cast<float>(kFixedDelta);
    const glm::vec2 start = bullet->position();

    step(*world, gameTime, 1);

    ASSERT_FALSE(bullet->terminated()) << "子弹一步就消失了";
    const glm::vec2 moved = bullet->position() - start;
    const float distance = std::sqrt(moved.x * moved.x + moved.y * moved.y);

    // 每步位移应当等于 速度 × 步长（浮点误差内）。
    // 这同时证明了"子弹在飞"以及"速度参数真的起作用了"。
    EXPECT_NEAR(distance, expectedStep, expectedStep * 0.02f)
        << "子弹没有按预期速度推进，实际每步 " << distance
        << " 单位，期望 " << expectedStep;
}

// ---------------------------------------------------------------------------
//  伤害发生在"到达"而不是"开火"
// ---------------------------------------------------------------------------
TEST(ProjectileTest, DamageDoesNotApplyOnTheFiringStep) {
    // 这是整组测试里最重要的一条：它区分了"攻击是实体"与"攻击是瞬时射线"。
    // 如果是瞬时射线，开火那一步敌人就已经掉血了。
    auto world = makeCleanWorld(glm::vec2{400.0f, 600.0f});
    double gameTime = 0.0;

    auto* enemy = world->spawnEnemy(glm::vec2{800.0f, 600.0f});
    ASSERT_NE(enemy, nullptr);
    makeTrainingDummy(*enemy);

    const float healthBefore = enemy->health();

    aimAndFire(*world, gameTime, enemy->position());

    EXPECT_FLOAT_EQ(enemy->health(), healthBefore)
        << "开火当步就掉血了——说明命中仍然是瞬时结算的，子弹只是个装饰";
}

TEST(ProjectileTest, ProjectileDamagesTargetWhenItArrives) {
    auto world = makeCleanWorld(glm::vec2{400.0f, 600.0f});
    double gameTime = 0.0;

    auto* enemy = world->spawnEnemy(glm::vec2{800.0f, 600.0f});
    ASSERT_NE(enemy, nullptr);
    makeTrainingDummy(*enemy);

    const float healthBefore = enemy->health();

    aimAndFire(*world, gameTime, enemy->position());

    // 距离 400，速度 880 → 约 0.42 秒 ≈ 26 步。给足余量跑 45 步。
    step(*world, gameTime, 45);

    // 【修复】伤害值原先硬编码为 18.0f，与 defaultPlayerProjectile() 的
    // 配置隐式耦合：调伤害参数时必须记得同步改测试，否则测试会莫名失败
    // （更糟的是：失败信息会指向"子弹没命中"，而实际只是数值变了）。
    const float expectedDamage = Game::defaultPlayerProjectile().damage;
    EXPECT_FLOAT_EQ(enemy->health(), healthBefore - expectedDamage)
        << "子弹应该飞过 400 单位后命中并造成 " << expectedDamage << " 点伤害";

    EXPECT_EQ(world->totalProjectileHits(), 1u) << "命中计数没有增加";
    ASSERT_GT(world->totalDamageDealt(), 0.0f);
}

TEST(ProjectileTest, ProjectileDoesNotHarmItsOwnShooter) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;
    auto* player = world->player();

    const float healthBefore = player->health();
    aimAndFire(*world, gameTime, player->position() + glm::vec2{300.0f, 0.0f});
    step(*world, gameTime, 60);

    EXPECT_FLOAT_EQ(player->health(), healthBefore) << "玩家被自己的子弹打到了";
}

// ---------------------------------------------------------------------------
//  掩体挡子弹
// ---------------------------------------------------------------------------
TEST(ProjectileTest, WallStopsProjectileAndProtectsTarget) {
    auto world = makeCleanWorld(glm::vec2{400.0f, 600.0f});
    double gameTime = 0.0;

    // 在玩家与敌人之间竖一堵墙。
    (void)world->spawnObstacle("TestWall", glm::vec2{600.0f, 600.0f},
                               glm::vec2{40.0f, 200.0f});

    auto* enemy = world->spawnEnemy(glm::vec2{800.0f, 600.0f});
    ASSERT_NE(enemy, nullptr);
    makeTrainingDummy(*enemy);

    const float healthBefore = enemy->health();

    aimAndFire(*world, gameTime, enemy->position());
    const int stepsUsed = stepUntilProjectilesGone(*world, gameTime);

    EXPECT_FLOAT_EQ(enemy->health(), healthBefore)
        << "墙没有挡住子弹——掩体形同虚设";

    // 子弹应当在墙的位置附近就终止了，而不是飞到敌人身上。
    // 距离敌人（400 单位）需要约 26 步；撞墙只需约 11 步。
    EXPECT_LT(stepsUsed, 20) << "子弹飞得太远才停下，看起来像是飞过墙了";

    ASSERT_FALSE(world->impactMarks().empty())
        << "子弹撞墙也应该留下弹着点，否则玩家看不到自己的枪打在哪";
}

TEST(ProjectileTest, HitsTargetHuggingWallButNotThroughIt) {
    // 【现象】"靠在墙边就没人打得到"。
    //
    // 根因：子弹撞墙用的是**导航网格的阻挡格**，而阻挡格是被膨胀过的近似
    // ——障碍只要压住某格的一部分，整格就标成阻挡（NavGrid::fillRect 用
    // 角点 floor 填格）。测例这堵墙的西侧面在 x=700，而 x=700 落在格 21
    // （[672,704]）里，于是格 21 整格变成阻挡：**阻挡范围比墙面多出 28 单位**。
    // 贴着墙面站（x=688，距离 12 单位）的目标因此处于"膨胀区"内，
    // 子弹在离墙一格远的地方就被吃掉（实测掉血 0）。
    //
    // 现在撞墙改为精确几何判定（点是否落在障碍矩形内），与所见一致。
    const auto damageToTargetAt = [](glm::vec2 targetPos) {
        auto world = makeCleanWorld(glm::vec2{400.0f, 620.0f});
        double gameTime = 0.0;

        // 墙：中心 (720,620)、40x200 → 西侧面 x=700，东侧面 x=740
        (void)world->spawnObstacle("Wall", glm::vec2{720.0f, 620.0f},
                                   glm::vec2{40.0f, 200.0f});

        auto* target = world->spawnEnemy(targetPos);
        if (target == nullptr) {
            return -1.0f;
        }
        makeTrainingDummy(*target);

        const float before = target->health();
        aimAndFire(*world, gameTime, targetPos);
        stepUntilProjectilesGone(*world, gameTime);
        return before - target->health();
    };

    // 贴着墙面（看得见的一侧）：必须打得中
    EXPECT_GT(damageToTargetAt(glm::vec2{688.0f, 620.0f}), 0.0f)
        << "贴着墙面的目标打不中 —— 子弹在离墙一格远的地方就被判成撞墙";

    // 墙的另一侧：墙必须真的挡住子弹
    EXPECT_FLOAT_EQ(damageToTargetAt(glm::vec2{752.0f, 620.0f}), 0.0f)
        << "墙没有挡住子弹（掩体失效）";
}

TEST(ProjectileTest, BlockedProjectileLeavesNonHitImpactMark) {
    auto world = makeCleanWorld(glm::vec2{400.0f, 600.0f});
    double gameTime = 0.0;

    (void)world->spawnObstacle("TestWall", glm::vec2{600.0f, 600.0f},
                               glm::vec2{40.0f, 200.0f});

    aimAndFire(*world, gameTime, glm::vec2{900.0f, 600.0f});
    stepUntilProjectilesGone(*world, gameTime);

    ASSERT_FALSE(world->impactMarks().empty());
    const Game::ImpactMark& mark = world->impactMarks()[0];

    EXPECT_FALSE(mark.hit) << "撞墙不是命中，不该标记为 hit";
    EXPECT_TRUE(mark.isPlayerShot);
    EXPECT_GT(mark.length(), 0.0f) << "残影长度为零，看不出弹道方向";
}

// ---------------------------------------------------------------------------
//  射程
// ---------------------------------------------------------------------------
TEST(ProjectileTest, ProjectileExpiresAfterMaxRange) {
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    // 没有任何目标：子弹应当飞完射程后自行消失。
    aimAndFire(*world, gameTime, glm::vec2{1000.0f, 600.0f});
    ASSERT_EQ(world->projectileCount(), 1u);

    const Game::Projectile* bullet = world->projectiles()[0];
    const float maxRange = bullet->maxRange();

    // 射程 520 / 速度 880 ≈ 0.59s ≈ 36 步
    stepUntilProjectilesGone(*world, gameTime);

    EXPECT_EQ(world->projectileCount(), 0u)
        << "打空的子弹没有消失，会无限累积在场景里";

    ASSERT_FALSE(world->impactMarks().empty());
    const Game::ImpactMark& mark = world->impactMarks()[0];
    EXPECT_FALSE(mark.hit) << "没有目标却标记为命中";

    // 残影长度应当接近射程：说明子弹真的飞完了全程，
    // 而不是在某个中间位置被误判为撞墙。
    EXPECT_NEAR(mark.length(), maxRange, maxRange * 0.1f)
        << "子弹没有飞满射程就消失了，实际只飞了 " << mark.length();
}

// ---------------------------------------------------------------------------
//  阵营规则
// ---------------------------------------------------------------------------
TEST(ProjectileTest, EnemyProjectileDoesNotDamageOtherEnemies) {
    // 刻意不做友军伤害：敌人之间互相挡枪会让调试变成噩梦
    // （一个敌人被队友打死，看起来就像 AI 逻辑出错），而收益为零。
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    auto* shooter = world->spawnEnemy(glm::vec2{400.0f, 400.0f});
    auto* bystander = world->spawnEnemy(glm::vec2{700.0f, 400.0f});
    ASSERT_NE(shooter, nullptr);
    ASSERT_NE(bystander, nullptr);
    makeTrainingDummy(*shooter);
    makeTrainingDummy(*bystander);

    const float healthBefore = bystander->health();

    // 直接用世界接口发射一颗敌方子弹，指向另一名敌人
    Game::ProjectileConfig config = Game::defaultEnemyProjectile();
    ASSERT_NE(world->spawnProjectile(shooter->id(), Game::Faction::Enemy,
                                     shooter->position(),
                                     glm::vec2{1.0f, 0.0f}, config),
              nullptr);

    step(*world, gameTime, 45);

    EXPECT_FLOAT_EQ(bystander->health(), healthBefore)
        << "敌人子弹打到了另一个敌人——出现了不该有的友军伤害";
}

TEST(ProjectileTest, PlayerProjectileDamagesEnemiesOnly) {
    auto world = makeCleanWorld(glm::vec2{400.0f, 600.0f});
    double gameTime = 0.0;

    auto* hunter = world->spawnEnemy(glm::vec2{800.0f, 600.0f});
    auto* spared = world->spawnEnemy(glm::vec2{420.0f, 900.0f});
    ASSERT_NE(hunter, nullptr);
    ASSERT_NE(spared, nullptr);
    makeTrainingDummy(*hunter);
    makeTrainingDummy(*spared);

    const float sparedBefore = spared->health();

    aimAndFire(*world, gameTime, hunter->position());
    step(*world, gameTime, 45);

    EXPECT_FLOAT_EQ(spared->health(), sparedBefore)
        << "射程外的敌人不该被打到";
    EXPECT_LT(hunter->health(), hunter->maxHealth())
        << "正前方的敌人应当被打中";
}

// ---------------------------------------------------------------------------
//  清理不变式
// ---------------------------------------------------------------------------
TEST(ProjectileTest, ZeroRangeRequestIsRejected) {
    // 零方向或零射程的子弹如果被放进来，会永远停在空中参与碰撞检测。
    auto world = makeCleanWorld();
    auto* player = world->player();

    EXPECT_EQ(world->spawnProjectile(player->id(), Game::Faction::Player,
                                     player->position(), glm::vec2{0.0f, 0.0f},
                                     Game::defaultPlayerProjectile()),
              nullptr)
        << "零方向的子弹应当被拒绝";
    EXPECT_EQ(world->projectileCount(), 0u);
}

TEST(ProjectileTest, PlayerShotCountIsRecorded) {
    // 回归：早期实现里 World 与 Player::onUpdate 两处都在消费同一个开火请求，
    // 先到的那一处把它吃掉，导致 HUD 上"射击"计数**恒为 0**。
    auto world = makeCleanWorld();
    double gameTime = 0.0;
    auto* player = world->player();

    ASSERT_EQ(player->shotsFired(), 0);

    aimAndFire(*world, gameTime, player->position() + glm::vec2{300.0f, 0.0f});

    EXPECT_EQ(player->shotsFired(), 1) << "玩家开火后射击计数没有增加";
    EXPECT_EQ(world->totalShotsFired(), 1u);
}

TEST(ProjectileTest, EveryShotProducesExactlyOneProjectile) {
    // 关键不变式：开火次数与子弹数量一一对应。
    // 这能抓住"统计说开枪了、却没有子弹飞出来"这类割裂 bug
    // （早期敌人开火被掩体挡住时直接 continue，既无子弹也无事件）。
    auto world = makeCleanWorld();
    double gameTime = 0.0;
    auto* player = world->player();

    // 冷却 0.15s = 9 个逻辑步，因此开火间隔取 10 步（0.167s）。
    // 循环里把"等冷却"放在最前面，是为了让最后开的那一枪后面
    // 不再跟多余的步数——否则第一颗子弹会先飞完射程而消失，
    // "空中的子弹数 == 开火数"就断言不出来了。
    constexpr int kCoolDownSteps = 9;
    constexpr int kShots = 3;

    for (int i = 0; i < kShots; ++i) {
        if (i > 0) {
            step(*world, gameTime, kCoolDownSteps);
        }
        player->setLookAtPoint(player->position() + glm::vec2{400.0f, 0.0f});
        step(*world, gameTime, 1);   // 让朝向生效
        player->requestFire();
        step(*world, gameTime, 1);   // 子弹入世

        EXPECT_EQ(world->totalShotsFired(), static_cast<size_t>(i + 1))
            << "第 " << (i + 1) << " 次开火没有被记录";
    }

    // 三颗子弹都朝同一方向飞，此时都还在空中（最老的一颗只飞了 322 单位，
    // 远小于 520 的射程），因此数量必须严格相等。
    EXPECT_EQ(world->projectileCount(), static_cast<size_t>(kShots))
        << "开火次数与空中的子弹数不一致";
    EXPECT_EQ(player->shotsFired(), kShots);
}

TEST(ProjectileTest, ProjectileCountDropsBackToZeroAfterFirefight) {
    // 长时间交火后空中的子弹数必须有界。如果清理逻辑失效，
    // 实体列表会随步数线性增长，最终把帧率拖垮。
    auto world = makeCleanWorld();
    double gameTime = 0.0;

    // 放一排静止的敌人当靶子，制造持续交火
    for (int i = 0; i < 6; ++i) {
        auto* enemy = world->spawnEnemy(glm::vec2{400.0f + 180.0f * i, 200.0f});
        if (enemy != nullptr) {
            makeTrainingDummy(*enemy);
        }
    }

    size_t peak = 0;
    for (int i = 0; i < 600; ++i) {   // 10 秒
        auto* player = world->player();
        if (player != nullptr && !player->isDead()) {
            player->setLookAtPoint(player->position() + glm::vec2{400.0f, 0.0f});
            player->requestFire();
        }
        world->update(kFixedDelta, gameTime);
        gameTime += kFixedDelta;
        peak = std::max(peak, world->projectileCount());
    }

    // 稳态上界：射程 520 / 速度 880 ≈ 0.59 秒在空，冷却 0.15 秒
    // ⇒ 同时最多约 4 颗。给足余量（32），只要能证明不是线性增长即可。
    EXPECT_LT(peak, 32u) << "空中的子弹数异常增长，可能没有正确回收";
    EXPECT_GT(peak, 0u) << "整个测试期间都没出现过子弹，测试没有意义";
}

