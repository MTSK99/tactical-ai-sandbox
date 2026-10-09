// ===========================================================================
//  tests/test_enemy_search.cpp
//
//  Enemy 搜索分支（丢失目标 → 去最后已知位置 → 观察 → 放弃）的回归测试。
//
//  【背景：这个 bug 为什么会被漏掉】
//  搜索动作的代码原先长这样：
//
//      const bool arrived = followPath(context.fixedDelta, moveTarget_);
//      if (arrived) {
//          // 到了却没发现敌人：原地等一会儿再放弃
//          return NodeStatus::Running;
//      }
//      return NodeStatus::Running;
//
//  两个分支返回**同一个值**，于是注释里的"放弃"从未发生。
//  AI 到了最后已知位置后就永久站住，只有等 8 秒后
//  HasLastKnownPosition 条件失效才切到巡逻。表现是：
//  玩家绕柱子躲一下，敌人就杵在原地不动直到记忆过期。
//
//  这类 bug 特别难靠"看画面"发现——站着不动本身不刺眼，
//  很难与"AI 在谨慎观察"区分开。能钉住它的只有对**行为**的断言：
//  "放弃之后必须重新产生位移"。
//
//  【为什么必须走 World::update 而不是直接调 Enemy::updateAI】
//  写第一版时直接调 `enemy->updateAI(...)`，结果位移恒为 0，查了很久。
//  原因是 updateAI 只**算出速度**，位置积分发生在 Entity::update() 里，
//  而那一步由 World::update() 调用。绕过 World 就等于只跑决策不跑物理，
//  敌人"想走"但"没走"——测出来的位移永远是 0。
//  这个陷阱本身值得记住：凡是断言"位移"的测试，都必须驱动完整的 World。
// ===========================================================================
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "game/Enemy.h"
#include "game/Entity.h"
#include "game/World.h"

using namespace TacticalAI;
using TacticalAI::Game::Entity;

namespace {

constexpr double kFixedDelta = 1.0 / 60.0;

/// 玩家出生点与敌人出生点。两者都在 y=600，敌人朝 +x，因此玩家在它的**正前方**。
///
/// 【为什么必须在正前方】
/// 感知默认视野是 ±45° 的锥。早期版本把玩家放在敌人身后 400 单位处，
/// 于是它从头到尾看不见玩家——测试里"进入搜索分支"的前提压根没成立，
/// 却表现为"AI 不动"，很容易误判成搜索逻辑有问题。
constexpr glm::vec2 kPlayerStart{900.0f, 600.0f};
constexpr glm::vec2 kEnemyStart {800.0f, 600.0f};

/// 「玩家已走远到看不见」的位置。距敌人约 780 单位，超出 viewDistance(500)。
constexpr glm::vec2 kPlayerGone{200.0f, 100.0f};

/// 造一个「无地形、只有一个玩家与一个敌人」的干净世界。
///
/// 为什么关掉示例障碍物：默认场景有一组手工掩体，会挡住 AI 的移动路径，
/// 让"它到底走没走到位"变得不确定。这个测试要观察的是决策，不是寻路。
std::unique_ptr<Game::World> makeTestWorld() {
    Game::WorldConfig config;
    config.generateDemoObstacles = false;
    config.initialEnemyCount = 0;
    config.playerStart = kPlayerStart;
    config.minSpawnDistance = 0.0f;   // 关掉生成间距，方便手工指定双方位置

    auto world = std::make_unique<Game::World>(config);
    world->initialize();
    return world;
}

/// 造一个感知参数「调教好」的敌人：看得远、反应快、记忆短、很快就放弃搜索。
///
/// 【memoryDuration 为什么必须调短 —— 这是写第一版时踩的坑】
/// 丢失视线后，感知**不会立刻**放弃目标：只要 memoryDuration 还没过，
/// `nearestConfirmedTarget()` 仍返回该目标，黑板里的 kNearestEnemy 就还在，
/// 行为树于是继续走**交战分支**，压根进不了搜索分支。
///
/// 默认记忆 6 秒 + 搜索 8 秒，意味着"先交战 6 秒，再搜索 8 秒"，
/// 测试里想观察的那段行为被推迟到十几秒之后，而且期间状态一直在Engaging，
/// 极易误判成"搜索逻辑坏了"。所以这里把记忆压到 0.4 秒，
/// 让"丢失 → 搜索"在几百毫秒内发生，测试才抓得住真正想抓的东西。
Game::Enemy* spawnTunedEnemy(Game::World& world) {
    auto* enemy = world.spawnEnemy(kEnemyStart);
    if (enemy == nullptr) {
        return nullptr;
    }

    auto& perception = enemy->perceptionConfig();
    perception.reactionTime   = 0.0;      // 看见即确认
    perception.memoryDuration = 0.4;      // 丢失后很快转入搜索分支
    perception.viewDistance   = 500.0f;   // 够看见 100 单位外的玩家

    // 搜索时长必须大于记忆时长，否则条件 HasLastKnownPosition
    // 会先失效，搜索分支一步都走不到。
    enemy->config().searchDuration = 30.0;
    enemy->config().searchHoldDuration = 0.5;
    return enemy;
}

/// 推进世界若干逻辑步（走完整条流水线，含物理积分）。
void step(Game::World& world, double& gameTime, int frames) {
    for (int i = 0; i < frames; ++i) {
        world.update(kFixedDelta, gameTime);
        gameTime += kFixedDelta;
    }
}

/// 两点间距离。
float distanceBetween(glm::vec2 a, glm::vec2 b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

} // namespace

// ---------------------------------------------------------------------------
//  前提：这套场景真的能建立"看见 → 丢失"的完整前提
// ---------------------------------------------------------------------------

TEST(EnemySearchTest, EnemyConfirmsTargetWhenPlayerIsInFront) {
    // 前置检查。不做这一步的话，后面两条测试失败时无从判断是
    // "搜索逻辑坏了"还是"前提根本没成立"——第一版就踩了这个坑。
    auto world = makeTestWorld();
    auto* enemy = spawnTunedEnemy(*world);
    ASSERT_NE(enemy, nullptr);

    double t = 0.0;
    step(*world, t, 30);

    EXPECT_TRUE(enemy->perception().hasConfirmedTarget())
        << "玩家在正前方 100 单位、视野内，应当被确认。"
        << "若这条失败，检查 spawnTunedEnemy 的位置与 viewDistance。";
}

// ---------------------------------------------------------------------------
//  核心回归：到达后必须「放弃」并切回巡逻
// ---------------------------------------------------------------------------

TEST(EnemySearchTest, GivesUpSearchAndResumesPatrol) {
    // 【这条是整个文件的核心断言】
    //
    // 修复前：arrive 之后两个分支都返回 Running，AI 永久站住。
    // 断言方式刻意是"是否重新产生位移"而不是状态名——
    // 因为搜索分支与巡逻分支都把状态设为 Patrolling，看状态名区分不出来，
    // 而"站着不动 vs 重新走起来"才是真正的差别。
    auto world = makeTestWorld();
    auto* enemy = spawnTunedEnemy(*world);
    ASSERT_NE(enemy, nullptr);

    double t = 0.0;

    // --- 阶段 1：看得见玩家，建立记忆 ---
    step(*world, t, 30);
    ASSERT_TRUE(enemy->perception().hasConfirmedTarget())
        << "前提：这一步必须先确认目标";

    // --- 阶段 2：玩家消失，等记忆过期后进入搜索分支 ---
    world->player()->setPosition(kPlayerGone);

    // 【为什么必须等记忆过期，不能只看"看不见了"】
    // 看不见之后，感知会**继续保留** confirmed 状态直到 memoryDuration 用完
    // ——那正是"记忆"的定义（Perception.cpp 的 else 分支明确保留 confirmed）。
    // 期间 kNearestEnemy 仍在黑板上，行为树会一直走交战分支。
    // 所以判据不是"目标不可见"，而是"确认目标已过期"。
    // 这里设了 memoryDuration=0.4，等 1 秒足够越过。
    step(*world, t, 60);

    EXPECT_FALSE(enemy->perception().hasConfirmedTarget())
        << "前提：记忆过期后不应再有确认目标。"
        << "若这条失败，多半是 memoryDuration 太大，测试还在交战分支里。";

    // --- 阶段 3：观察它最终是否会离开最后已知位置 ---
    //
    // 【这一段是本测试的全部难点，前后改了四版才做对】
    //
    // v1：测"消失后 8 秒的总位移" → 修复前后都绿。
    //     因为那 8 秒大部分时间它在**走向**最后已知位置，位移当然大，
    //     而"能走过去"修复前后都成立。差异只在**到位之后**。
    //
    // v2：改成"到位后开始测后续位移" → 修复前正确失败（位移 0），
    //     但修复后前提断言失败：修复后它已放弃并走开了。
    //
    // v3：把到位等待从 3 秒缩到 2 秒 → 仍然在赛跑，
    //     40 单位的阈值卡在"刚好到达"与"已经走开"之间，极其脆弱。
    //
    // v4（现在）：不再测量"某个时刻的瞬时位移"，改为测量
    //     **整段轨迹上离最后已知位置的最远距离**。
    //
    // 之所以这样最稳，是因为它直接对应 bug 的本质：
    // 修复前 AI 永久站住 → 最远距离 ≈ 10（就是到达半径本身）；
    // 修复后放弃并巡逻 → 最远距离 300+（跑出去老远）。
    // 两者差 30 倍，任何合理的阈值都能区分，不必跟时序赛跑。
    float maxDistanceFromLastKnown = 0.0f;

    // 采样 8 秒，每 0.25 秒记一次轨迹上的最远距离
    for (int i = 0; i < 32; ++i) {
        step(*world, t, 15);
        maxDistanceFromLastKnown =
            std::max(maxDistanceFromLastKnown,
                     distanceBetween(enemy->position(), kPlayerStart));
    }

    EXPECT_GT(maxDistanceFromLastKnown, 100.0f)
        << "搜索并放弃后应离开最后已知位置（轨迹最远 " << maxDistanceFromLastKnown
        << " 单位）。若它一直停在原地（最远仅约 10 单位），"
        << "正是 SearchLastKnown 里 if(arrived) 与末尾返回同值的那个 bug："
        << "两个分支都返回 Running，'放弃'从未发生。";

    EXPECT_FALSE(enemy->isDead());
}

TEST(EnemySearchTest, ActuallyVisitsLastKnownPosition) {
    // 顺带钉住"搜索确实走到了最后已知位置"这件事。
    // 上一条只证明"之后动起来了"，这一条证明"先到位、再放弃"——
    // 否则一个"一开始就乱走"的实现也能通过上一条。
    auto world = makeTestWorld();
    auto* enemy = spawnTunedEnemy(*world);
    ASSERT_NE(enemy, nullptr);

    double t = 0.0;
    step(*world, t, 30);                       // 看见玩家
    ASSERT_TRUE(enemy->perception().hasConfirmedTarget());

    world->player()->setPosition(kPlayerGone);
    step(*world, t, 60);                       // 等记忆过期，真正进入搜索分支

    // 【判据改成"整段轨迹上的最近距离"，而不是某一瞬间的瞬时距离】
    //
    // 原写法是"跑满 3 秒后看瞬时距离 < 120"。但到达之后敌人会立刻进入
    // "观察 → 放弃 → 走向新巡逻点"的流程，于是**恰好在采样的那一步**
    // 它已经重新走远了 —— 实测 122 单位，卡在阈值上判失败。
    // 这条测试的本意是"它到过那儿"，那么最贴合的判据自然是最小距离：
    // 它不依赖采样时机，也不会因为"到达得快"而失败。
    float closestApproach = 1e9f;
    for (int i = 0; i < 60 * 3; ++i) {         // 3 秒，边走边记录
        step(*world, t, 1);
        closestApproach = std::min(closestApproach,
                                   distanceBetween(enemy->position(), kPlayerStart));
    }

    // 允许 60 单位余量：路径平滑与到达阈值（followPath 里 14~20 单位）
    // 都不要求它精确站在目标点上。
    EXPECT_LT(closestApproach, 60.0f)
        << "AI 从未走到玩家最后已知位置附近（整段轨迹最近只到 "
        << closestApproach << " 单位）";
}

// ---------------------------------------------------------------------------
//  反向断言：观察期内不该放弃
// ---------------------------------------------------------------------------

TEST(EnemySearchTest, KeepsSearchingDuringHoldWindow) {
    // 与上面的"最终会放弃"成对。
    //
    // 只修"最终放弃"最省事的做法是把两个分支都改成返回 Failure——
    // 那样上一条会通过，但 AI 一到位置就放弃，丢掉"到这儿看看"的行为。
    // 这条保证"不是立刻放弃"。
    auto world = makeTestWorld();
    auto* enemy = spawnTunedEnemy(*world);
    ASSERT_NE(enemy, nullptr);

    enemy->config().searchHoldDuration = 8.0;   // 故意给很长的观察期

    double t = 0.0;
    step(*world, t, 30);
    world->player()->setPosition(kPlayerGone);
    step(*world, t, 60);          // 等记忆过期，进入搜索分支

    // 先花足够时间走到最后已知位置，确认它确实到位了。
    // 【为什么不能一上来就量】
    // 观察期有 8 秒，而敌人 1 秒左右就能走到位。第一版在"消失后 2 秒"取样，
    // 其中大部分时间它还在路上，测出 128 单位——那是"没走到"，不是"放弃了"。
    // 这个误判很危险：它会让人以为修复没生效，从而去改本来正确的代码。
    step(*world, t, 60 * 2);

    const float afterArrival = distanceBetween(enemy->position(), kPlayerStart);
    ASSERT_LT(afterArrival, 120.0f)
        << "前提：2 秒后它应已到达最后已知位置附近（实测 " << afterArrival << "）";

    // 现在它已到位，且仍在 8 秒观察期内。再走 3 秒——远不到期，
    // 若这段时间它又跑远了，说明"一到就放弃"。
    step(*world, t, 60 * 3);

    const float toLastKnown = distanceBetween(enemy->position(), kPlayerStart);

    EXPECT_LT(toLastKnown, 120.0f)
        << "观察期(" << enemy->config().searchHoldDuration
        << "s)内不该放弃搜索，但到位 3 秒后它离最后已知位置 "
        << toLastKnown << " 单位远——说明一到就放弃了";
}

// ---------------------------------------------------------------------------
//  重获目标后应能重新搜索（防止修复引入"永久放弃"）
// ---------------------------------------------------------------------------

TEST(EnemySearchTest, ReacquiringTargetReenablesSearch) {
    // 【为什么必须测这条】
    // 修复引入了 searchAbandoned_ 标志。若它在"重新看见玩家"时没被清掉，
    // AI 就会**一辈子只放弃一次**：此后即便玩家就站在它旁边，
    // 它也只会巡逻、再也不搜索。
    //
    // 这是典型的"修一个 bug 引入另一个 bug"，而且更难发现——
    // 它只在"曾经丢失过目标"的那局里出现，普通冒烟测试看不到。
    auto world = makeTestWorld();
    auto* enemy = spawnTunedEnemy(*world);
    ASSERT_NE(enemy, nullptr);

    enemy->config().searchHoldDuration = 0.2;   // 快速走完一次"放弃"

    double t = 0.0;

    // 看见 → 走远 → 记忆过期 → 走完搜索与放弃
    step(*world, t, 30);
    ASSERT_TRUE(enemy->perception().hasConfirmedTarget());
    world->player()->setPosition(kPlayerGone);
    step(*world, t, 60 * 5);

    EXPECT_FALSE(enemy->perception().hasConfirmedTarget())
        << "前提：这5 秒里应已完成一次「搜索 → 观察 → 放弃」";

    // 玩家重新进入视野：应回到交战/转移状态。
    //
    // 【两个必须注意的前提，缺一个这条测试就没有意义】
    //
    // 1. 不能把玩家放回固定坐标 kPlayerStart：搜索与放弃花了 5 秒，
    //    期间敌人已离开出生点一段距离，玩家可能被放到 viewDistance 之外。
    //
    // 2. **必须放在敌人「面朝方向」上**，而不是简单的 +x 偏移。
    //    搜索结束时敌人的朝向是 (-0.99, 0.12)——朝 -x，也就是背对 +x。
    //    若把玩家放在敌人当前位置的 +x侧，它恰好落在背后 180°，
    //    而视野是 ±45° 的锥，于是永远看不见。
    //    第一版就栽在这里：现象是"重获目标后AI 仍在巡逻"，
    //    看起来像 searchAbandoned_ 没清干净，实际是压根没看见。
    //
    // 结论：这类测试不能假设 AI 的朝向，必须按它真实的朝向来摆放目标。
    glm::vec2 forward = enemy->facing();
    const float facingLength = distanceBetween({0.0f, 0.0f}, forward);
    if (facingLength <= 1e-3f) {
        forward = glm::vec2{1.0f, 0.0f};   // 退化兜底：朝向为零向量时用 +x
    } else {
        forward = forward / facingLength;
    }
    world->player()->setPosition(enemy->position() + forward * 120.0f);
    step(*world, t, 30);

    EXPECT_TRUE(enemy->perception().hasConfirmedTarget())
        << "前提：玩家走回视野后应重新被确认";

    const bool engaged = enemy->state() == Game::AIState::Engaging ||
                         enemy->state() == Game::AIState::Repositioning;
    EXPECT_TRUE(engaged)
        << "重新看见玩家后应回到交战/转移状态，实际是「"
        << Game::toString(enemy->state()) << "」。若长期停在巡逻，"
        << "多半是 searchAbandoned_ 在重获目标时没被清掉。";
}

// ---------------------------------------------------------------------------
//  小队情报共享：队友之间更新「最后已知位置」
// ---------------------------------------------------------------------------

TEST(EnemySearchTest, TeammatesShareLastKnownPosition) {
    // 【需求】看见目标的敌人要把"目标最后在哪"告诉队友，让没看见的队友
    // 也有方向可搜 —— 而不是各自原地巡逻碰运气。
    //
    // 语义边界（这条测试同时守住它）：共享的是**情报**，不是视野。
    // 收到情报的敌人依然要靠自己的感知才能进入交战分支。
    auto world = makeTestWorld();   // 无地形；玩家固定在 kPlayerStart
    double t = 0.0;

    // A（观察者）：站在玩家正前方约 100 单位，必然看得见
    Game::Enemy* spotter = spawnTunedEnemy(*world);
    ASSERT_NE(spotter, nullptr);

    // B（队友）：远在地图另一角，距玩家约 860 单位 > viewDistance(480)，
    // 因此它自己**永远看不到**玩家，只能靠 A 的情报。
    Game::Enemy* teammate = world->spawnEnemy(glm::vec2{200.0f, 1100.0f});
    ASSERT_NE(teammate, nullptr);

    step(*world, t, 30);   // 0.5 秒：A 建立目击

    ASSERT_TRUE(spotter->perception().hasConfirmedTarget()) << "前提：A 应当看到玩家";
    ASSERT_FALSE(teammate->perception().hasConfirmedTarget())
        << "前提：B 看不到玩家（否则这条测试证明不了情报共享）";

    // 情报已经广播到 B
    ASSERT_TRUE(teammate->hasFreshSquadIntel(t))
        << "队友的目击没有传给 B —— 情报广播没有生效";
    EXPECT_NEAR(distanceBetween(teammate->squadIntelPosition(), kPlayerStart), 0.0f, 1.0f)
        << "共享的位置不是玩家所在地";

    // B 真的用上了它：路径终点指向被报告的位置。
    // （决策阶段在寻路之后，路径会滞后一步，所以多跑几步再断言。）
    step(*world, t, 60);
    const std::vector<glm::vec2>& waypoints = teammate->currentPath().waypoints;
    ASSERT_FALSE(waypoints.empty()) << "B 应当正在朝情报位置移动";
    EXPECT_LT(distanceBetween(waypoints.back(), kPlayerStart), 60.0f)
        << "B 的移动目标不是队友报告的位置（实际终点 ("
        << waypoints.back().x << ", " << waypoints.back().y << ")）";

    // 情报必须会过期：玩家离开所有人的视野，等过有效期。
    //
    // 时长从配置里取（而不是写死 14 秒）：有效期是可调的，写死会让
    // "把情报有效期调长"这样一个正常改动把测试打挂，而且报错信息
    // 完全指不到真正的原因。
    world->player()->setPosition(glm::vec2{1500.0f, 100.0f});
    const int waitSeconds = static_cast<int>(teammate->config().squadIntelMemory) + 2;
    step(*world, t, 60 * waitSeconds);

    EXPECT_FALSE(teammate->hasFreshSquadIntel(t))
        << "过期情报仍被当成有效线索 —— 全队会永远朝一个旧坐标搜索";
    EXPECT_FALSE(teammate->perception().hasConfirmedTarget());
}

// ---------------------------------------------------------------------------
//  掩体的边界：贴墙不该隐身，墙后必须看不见
// ---------------------------------------------------------------------------

TEST(EnemySearchTest, HuggingWallDoesNotHidePlayerButWallStillBlocksSight) {
    // 【现象】"靠在墙边能让人发现不到"。
    //
    // 根因：视线判定原先走的是**导航网格的阻挡格**，而阻挡格是被膨胀过的
    // 近似——障碍只要压住某格的一部分，整格就被标成阻挡
    // （NavGrid::fillRect 用矩形角点的 floor 填格）。
    //
    // 测例这堵墙的西侧面在 x=700，而 x=700 落在格 21（[672,704]）内，
    // 于是格 21 整格变成阻挡：阻挡范围比真实墙面多出 28 个单位。
    // 贴在墙面站（x=688，距墙面 12 单位）的玩家正好落在"膨胀区"里 ⇒
    // 视线采样的最后一个点就在阻挡格里 ⇒ **完全看不见**（实测确认）。
    //
    // 现在视线改为精确几何（线段是否穿过障碍矩形）：贴墙可见、墙后不可见。
    const glm::vec2 kWallCenter{720.0f, 600.0f};
    const glm::vec2 kWallSize{40.0f, 200.0f};   // 西侧面 x=700，东侧面 x=740

    // 每次都用全新世界，避免上一轮的"记忆"干扰下一轮的结论
    const auto seesPlayerAt = [&](glm::vec2 playerPos) {
        auto world = makeTestWorld();
        double t = 0.0;

        (void)world->spawnObstacle("Wall", kWallCenter, kWallSize);

        // 观察者在墙西侧 300 单位，朝 +x
        auto* watcher = world->spawnEnemy(glm::vec2{400.0f, 600.0f});
        if (watcher == nullptr) {
            return false;
        }
        watcher->config().moveSpeed    = 0.0f;   // 别动，免得走出视野
        watcher->config().acceleration = 0.0f;

        world->player()->setPosition(playerPos);
        step(*world, t, 40);                     // 超过 reactionTime(0.25s)

        return watcher->perception().hasConfirmedTarget();
    };

    EXPECT_TRUE(seesPlayerAt(glm::vec2{688.0f, 600.0f}))
        << "玩家贴在墙面上（距墙面 12 单位）却被判成看不见 —— "
        << "掩体的阻挡范围比它看起来的样子胖了一圈";

    EXPECT_FALSE(seesPlayerAt(glm::vec2{760.0f, 600.0f}))
        << "玩家躲在墙后却仍被看见 —— 掩体失效";
}

// ---------------------------------------------------------------------------
//  丢失视线之后：不许对着"记忆位置"开枪 / 不许被记忆钉在原地
// ---------------------------------------------------------------------------

TEST(EnemySearchTest, DoesNotFireAtRememberedPosition) {
    // 【现象】敌人对着一个**空位置**连续开枪。
    //
    // 开火用的目标位置来自 kLastKnownPosition —— 敌人**记忆**中的位置。
    // 玩家离开视野（绕到背后 / 躲进掩体 / 跑远）后，这个位置停在原地，
    // 而感知的"确认目标"在 memoryDuration（默认 6 秒）内仍然成立，
    // 于是敌人朝着空位开火，一直打到记忆过期。LoS 检查也救不了它：
    // 那个空点自己有视线。
    //
    // 实测（修复前）：玩家瞬移走后，敌人对着 762 单位外的空点打出 7 发，
    // 持续整整 6 秒。
    auto world = makeTestWorld();
    auto* enemy = world->spawnEnemy(kEnemyStart);   // 默认配置：记忆 6 秒
    ASSERT_NE(enemy, nullptr);

    double t = 0.0;
    step(*world, t, 60 * 2);                        // 2 秒：确认目标并开火
    ASSERT_GT(enemy->shotsFired(), 0) << "前提：看得见时应当开过枪";

    const int shotsBeforeLeaving = enemy->shotsFired();

    // 玩家离开视野：背后约 780 单位，同时在视野锥（±45°）与视野距离(500)之外
    world->player()->setPosition(kPlayerGone);
    step(*world, t, 60 * 7);

    EXPECT_EQ(enemy->shotsFired(), shotsBeforeLeaving)
        << "玩家离开视野后敌人仍在开火（记忆里的位置 = 空位置）";
    EXPECT_FALSE(enemy->perception().hasConfirmedTarget())
        << "前提：7 秒后记忆应已过期，否则这条测的是别的东西";
}

TEST(EnemySearchTest, LeavesLastKnownPositionAfterScanning) {
    // 【现象】敌人跑到 last location 点位后**长时间发呆**。
    //
    // 根因：updatePerception 里用 hasConfirmedTarget() 判断"重新确认目标"，
    // 而它在记忆期内**一直为真**。于是搜索分支"到达 → 观察 → 放弃"的结果
    // 下一步就被清掉（searchAbandoned_ = false）、到达时刻被重置，
    // 敌人原地无限重播这段动作 —— 实测站了 3.5 秒以上，直到 6 秒记忆过期。
    auto world = makeTestWorld();
    auto* enemy = world->spawnEnemy(kEnemyStart);
    ASSERT_NE(enemy, nullptr);
    enemy->config().searchHoldDuration = 0.3;       // 让观察窗口短一些，缩短用例

    double t = 0.0;
    step(*world, t, 60);                            // 1 秒：看见并确认玩家
    ASSERT_TRUE(enemy->perception().hasConfirmedTarget());

    world->player()->setPosition(kPlayerGone);

    // 等它走到最后已知位置附近
    const glm::vec2 lead = kPlayerStart;            // 玩家最后被看见的位置
    bool arrived = false;
    for (int i = 0; i < 60 * 8 && !arrived; ++i) {
        step(*world, t, 1);
        arrived = distanceBetween(enemy->position(), lead) < 24.0f;
    }
    ASSERT_TRUE(arrived) << "前提：应当先走到最后已知位置";

    // 到点之后 3 秒内必须"动起来"，而不是原地重播观察动作。
    //
    // 【为什么用"累计位移"而不是"离记忆点多远"】
    // 这个场景里敌人出生点距最后已知位置只有 100 单位，而它的巡逻环以
    // 出生点为圆心（半径 70~260）—— 搜索结束后它本来就在**附近**游荡，
    // "离得够远"并不成立（实测最远只到 84）。真正区分得开的是：
    // 修复前它被"观察 → 放弃 → 被重置"锁死在原地（累计位移≈0），
    // 修复后它会真的重新走起来（实测累计 300+ 单位）。
    float totalTravel = 0.0f;
    glm::vec2 previous = enemy->position();
    for (int i = 0; i < 60 * 3; ++i) {
        step(*world, t, 1);
        const glm::vec2 current = enemy->position();
        totalTravel += distanceBetween(current, previous);
        previous = current;
    }

    EXPECT_GT(totalTravel, 120.0f)
        << "到点后 3 秒内只移动了 " << totalTravel
        << " 单位 —— 观察动作在无限重播，敌人被钉在最后已知位置（站桩发呆）";
}

TEST(EnemySearchTest, DropsEngageBranchSoonAfterLosingSight) {
    // 【现象】玩家一消失，敌人就**站在原地干等**。
    //
    // 交战分支的条件原先只看"有确认目标"，而确认在 memoryDuration 内一直成立，
    // 于是交战分支（优先级高于搜索）把敌人钉在原地 6 秒，什么都不做。
    // 现在加了宽限期（engageLostGrace，默认 0.5 秒）：超过就交给搜索分支，
    // 敌人会真的走过去找。
    auto world = makeTestWorld();
    auto* enemy = world->spawnEnemy(kEnemyStart);
    ASSERT_NE(enemy, nullptr);

    double t = 0.0;
    step(*world, t, 60 * 2);                        // 2 秒：确认并进入交战
    ASSERT_TRUE(enemy->state() == Game::AIState::Engaging ||
                enemy->state() == Game::AIState::Repositioning)
        << "前提：看得见时应当处于交战/转移状态，当前是 "
        << Game::toString(enemy->state());

    world->player()->setPosition(kPlayerGone);
    step(*world, t, 60 * 2);                        // 2 秒 > 宽限期 0.5 秒

    EXPECT_TRUE(enemy->state() == Game::AIState::Patrolling ||
                enemy->state() == Game::AIState::Idle)
        << "失去视线 2 秒后仍停在交战状态（state=" << Game::toString(enemy->state())
        << "）—— 敌人会对着记忆位置干等";
}

// ---------------------------------------------------------------------------
//  搜索到位后的"观察"：环顾而不是站桩
// ---------------------------------------------------------------------------

TEST(EnemySearchTest, SearchHoldLooksAroundInsteadOfFreezing) {
    // 【需求】"敌人老是跑到 last location 点位发呆"。
    //
    // 观察窗口本来就是"到了看看人在不在"，但**必须看得出它在看**：
    // 位置基本不动，朝向要摆动。原实现是位置、朝向全都不动的两秒静默，
    // 画面上就是一个人杵在原地。
    auto world = makeTestWorld();
    auto* enemy = spawnTunedEnemy(*world);
    ASSERT_NE(enemy, nullptr);
    enemy->config().searchHoldDuration = 1.5;   // 留出足够的采样窗口

    double t = 0.0;
    step(*world, t, 30);                        // 看见玩家
    ASSERT_TRUE(enemy->perception().hasConfirmedTarget());

    world->player()->setPosition(kPlayerGone);
    step(*world, t, 60);                        // 记忆过期 → 进入搜索分支

    // 走到最后已知位置（到达阈值 14，这里取 16 作为"基本到位"）
    bool arrived = false;
    for (int i = 0; i < 60 * 8 && !arrived; ++i) {
        step(*world, t, 1);
        arrived = distanceBetween(enemy->position(), kPlayerStart) < 16.0f;
    }
    ASSERT_TRUE(arrived) << "前提：应当先走到最后已知位置";

    // 采样 0.6 秒：位置几乎不动，朝向应累计转过明显角度
    const glm::vec2 posBefore = enemy->position();
    glm::vec2 prevFacing = enemy->facing();
    float totalRotation = 0.0f;
    for (int i = 0; i < 36; ++i) {
        step(*world, t, 1);

        const glm::vec2 facing = enemy->facing();
        const float dot = std::clamp(prevFacing.x * facing.x + prevFacing.y * facing.y,
                                     -1.0f, 1.0f);
        totalRotation += std::acos(dot);
        prevFacing = facing;
    }
    const float moved = distanceBetween(enemy->position(), posBefore);

    EXPECT_GT(totalRotation, 0.15f)
        << "观察窗口里朝向几乎没变（累计 " << totalRotation
        << " 弧度）—— 这就是「到点站桩发呆」";
    EXPECT_LT(moved, 16.0f)
        << "观察窗口里不应该乱跑（位移 " << moved << " 单位）";
}

// ---------------------------------------------------------------------------
//  追击与开火：玩家走直线逃跑应该被追上并持续挨打
// ---------------------------------------------------------------------------

TEST(EnemySearchTest, EnemyChasesWalkingPlayerAndKeepsFiring) {
    // 【需求】"射击间隔感觉很怪，有时射不出来"。
    //
    // 验收场景取最常见的实战情形：玩家在开阔地以**走速**直线逃离
    // （不冲刺、不进掩体）。此时敌人应当：
    //   1) 用上冲刺追击（走速 145 追不上 160 的玩家）；
    //   2) 进入射程后按冷却稳定输出（0.95 秒一发）。
    //
    // 实测数据（本用例的由来）：修复前敌人最高速度只有 130（冲刺配置
    // 根本没被用上，被 applyMovement 的速度上限夹回去了），20 秒里一枪
    // 未开；修复后首次开火 0.28 秒、间隔稳定 0.95 秒、最高速度 240。
    Game::WorldConfig config;
    config.generateDemoObstacles = false;
    config.initialEnemyCount = 0;
    config.playerStart = {300.0f, 200.0f};
    Game::World world(config);
    world.initialize();
    world.player()->setMaxHealth(1.0e6f);
    world.player()->setHealth(1.0e6f);

    // 敌人在玩家西侧，玩家在其视野正前方
    Game::Enemy* enemy = world.spawnEnemy(glm::vec2{100.0f, 200.0f});
    ASSERT_NE(enemy, nullptr);

    double t = 0.0;
    int lastShots = 0;
    double lastShotTime = -1.0;
    double maxGap = 0.0;
    float maxSpeed = 0.0f;
    int shots = 0;

    for (int i = 0; i < 60 * 6; ++i) {   // 6 秒（玩家仍在图内）
        world.player()->setPosition(glm::vec2{300.0f + 160.0f * static_cast<float>(t), 200.0f});

        world.update(1.0 / 60.0, t);
        t += 1.0 / 60.0;

        const glm::vec2 velocity = enemy->velocity();
        maxSpeed = std::max(maxSpeed, std::sqrt(velocity.x * velocity.x +
                                                velocity.y * velocity.y));

        if (enemy->shotsFired() != lastShots) {
            ++shots;
            if (lastShotTime >= 0.0) {
                maxGap = std::max(maxGap, t - lastShotTime);
            }
            lastShotTime = t;
            lastShots = enemy->shotsFired();
        }
    }

    EXPECT_GE(shots, 4) << "6 秒里只开了 " << shots << " 枪 —— 追击/开火链路有问题";
    EXPECT_LE(maxGap, 2.5) << "最长间隔 " << maxGap << " 秒（冷却只有 "
                           << enemy->config().fireCooldown << " 秒）";
    EXPECT_GT(maxSpeed, enemy->config().moveSpeed + 10.0f)
        << "追击时没有用上冲刺（最高速度 " << maxSpeed << "，走速 "
        << enemy->config().moveSpeed << "）—— 玩家走直线就能永远拉开距离";
}

// ---------------------------------------------------------------------------
//  撤退方向（此前是"撤退变冲锋"）
// ---------------------------------------------------------------------------

TEST(EnemySearchTest, LowHealthEnemyRetreatsAwayFromThreat) {
    // 断言撤退的**行为契约**：低血量时进入撤退状态，并且真的与威胁拉开距离。
    //
    // 【这条测试能守什么、不能守什么 —— 实测记录】
    // MoveToRetreat 曾经把"往哪走"解析成黑板里的 kLastKnownPosition（玩家
    // 位置），而不是 EQS 选出的撤退点。我按"敌人会掉头冲锋"写了三个版本的
    // 断言（moveTarget_ 到威胁的距离、速度方向、5 秒后的距离），**三种写法
    // 在错误实现下都会通过**：因为撤退点每 1.5 秒重选一次、寻路路径占主导，
    // 错误只暴露在"寻路失败 / 路径耗尽"的降级路径上（那时会改用
    // moveDirectlyToward(玩家位置)）。
    //
    // 所以这条测试守的是"撤退方向不能反"这个行为契约，而不是那个具体的键。
    // 键的语义已由 resolveTargetPosition() 的注释钉住（它明确写着"这是目标
    // 位置，不是移动目标"）。
    auto world = makeTestWorld();
    double t = 0.0;

    Game::Enemy* enemy = spawnTunedEnemy(*world);
    ASSERT_NE(enemy, nullptr);

    // 先让它看见玩家：这正好会把 kLastKnownPosition 写进黑板
    // （也就是当初被误读的那个键必须存在，测试才有意义）
    step(*world, t, 30);
    ASSERT_TRUE(enemy->perception().hasConfirmedTarget())
        << "前提：敌人应当已经确认了玩家";

    // 把血量压到撤退阈值以下
    enemy->setHealth(1.0f);
    ASSERT_LT(enemy->healthRatio(), enemy->config().retreatHealthRatio);

    step(*world, t, 5);   // 反应式选择器每步重估，几步内必然进入撤退分支

    ASSERT_EQ(enemy->state(), Game::AIState::Retreating)
        << "低血量时应当进入撤退状态，实际是「"
        << Game::toString(enemy->state()) << "」";

    const glm::vec2 playerPosition = world->player()->position();

    // 【关键：必须断言"实际位移"，而不是 moveTarget_】
    // moveTarget_ 是 FindRetreatPosition 写下的 EQS 撤退点，它一直是对的；
    // 出问题的是**移动动作往哪个方向走**（它曾经把撤退点解析成玩家位置）。
    // 第一版这里断言的是 moveTarget_ 到玩家的距离，结果在 bug 存在时依然
    // 通过 —— 一个不会失败的回归测试等于没有测试。
    const float before = distanceBetween(enemy->position(), playerPosition);

    // 跑满 5 秒：撤退点大约 360 单位外，1~2 秒就能走到；走到之后如果
    // "到达判定"用的是玩家位置（错误键），敌人会因为永远判不到达而
    // 转入"直接朝目标直线靠拢"的降级路径 —— 也就是掉头冲回玩家身边。
    // 所以必须跑到撤退点之后再看，短窗口会掩盖这个 bug。
    step(*world, t, 300);

    const float after = distanceBetween(enemy->position(), playerPosition);

    EXPECT_GT(after, before + 100.0f)
        << "撤退时与玩家的距离没有拉开（" << before << " → " << after
        << "）——移动目标被解析成了玩家所在位置，敌人是在冲锋而不是撤离";
}
