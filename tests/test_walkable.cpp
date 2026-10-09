// ===========================================================================
//  tests/test_walkable.cpp
//
//  World::isPositionWalkable 膨胀检查的回归测试。
//
//  【背景：这个 bug 为什么危险】
//  原实现是这样的：
//
//      for (dy, dx in 3x3) {
//          if (!inBounds)      return false;
//          if (!isWalkable)    continue;    // ← 不可通行：跳过
//      }                                    // ← 可通行：也落到这里
//      return true;
//
//  两个分支的**执行结果完全相同**——整个循环从未拒绝过任何候选点，
//  函数实际等价于"只看中心格"。而它的注释写着：
//
//      「用实体半径做膨胀检查……否则实体会卡在墙角」
//
//  这属于最坏的一类 bug：**注释承诺了、文档描述了、后来者读到会以为
//  它生效了**，而实际从未生效。危害不会立刻暴露，只会在某天有人
//  调大实体尺寸、或把生成点放在贴墙格心时，表现为"实体卡进墙里"。
//
//  修复方式不是删掉注释了事，而是补上真正的几何判定（圆 vs 格子 AABB）。
//  这组测试钉住的是**语义**：贴墙的位置在给定实体半径下必须被判不可站立。
// ===========================================================================
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <memory>

#include "game/World.h"

using namespace TacticalAI;

namespace {

/// 一个只有「一面竖直墙」的世界：地图正中一堵贯穿上下的墙。
///
/// 为什么用最简地形：膨胀检查的判定只依赖「某格是否阻挡」，
/// 复杂地形会让断言难以归因——失败了分不清是膨胀逻辑错还是地形本身的问题。
///
/// 【墙体实际覆盖哪几列 —— 这不是随手写的常量】
/// 墙心 x=800、厚 32，即 x∈[784, 816)。但 fillRect 是**按格子**填的：
/// 它取 worldToGrid(784)=第 24 列 与 worldToGrid(816)=第 25 列，
/// 于是整列 24 与 25 都被封住，对应世界 x∈[768, 832)。
/// 换句话说，"厚 32 的墙"在网格上占了两列——边界没对齐格心时必然如此。
/// 这一点是实测出来的（先按"只占一列"写断言，跑出来全错）。
/// 下面的 kWallMinX / kWallMaxX 都以此为准。
std::unique_ptr<Game::World> makeWallWorld() {
    Game::WorldConfig config;
    config.generateDemoObstacles = false;   // 关掉手写掩体，只留我们自己的墙
    config.initialEnemyCount = 0;
    config.cellSize = 32.0f;
    config.gridExtent = {1600.0f, 1200.0f};

    auto world = std::make_unique<Game::World>(config);
    world->initialize();

    (void)world->spawnObstacle("TestWall", {800.0f, 600.0f}, {32.0f, 1200.0f}, false);
    return world;
}

/// 阻挡区左边界（世界坐标 x）。小于它的是可通行开阔地。
constexpr float kWallMinX = 768.0f;
/// 阻挡区右边界（开区间）。大于它的是可通行开阔地。
constexpr float kWallMaxX = 832.0f;

/// 该点在墙**左侧**开阔地，距墙 @p distance 个单位。
glm::vec2 leftOfWall(float distance) {
    return {kWallMinX - distance, 600.0f};
}

/// 该点在墙**右侧**开阔地，距墙 @p distance 个单位。
glm::vec2 rightOfWall(float distance) {
    return {kWallMaxX + distance, 600.0f};
}

} // namespace

// ---------------------------------------------------------------------------
//  半径为 0：退化为「只看中心格」，这是绝大多数调用方的语义
// ---------------------------------------------------------------------------

TEST(WalkableTest, ZeroRadiusOnlyChecksCenterCell) {
    auto world = makeWallWorld();

    // 距墙 2 单位：中心格可通行（不在阻挡列里），但紧邻阻挡格
    const glm::vec2 nearWall = leftOfWall(2.0f);
    ASSERT_TRUE(world->navGrid().isWalkable(world->navGrid().worldToGrid(nearWall)))
        << "前提：该点所在格本身应可通行";

    // 半径 0 → 只看中心格 → 可站立
    EXPECT_TRUE(world->isPositionWalkable(nearWall, 0.0f));

    // 墙内 → 中心格阻挡 → 不可站立
    EXPECT_FALSE(world->isPositionWalkable({800.0f, 600.0f}, 0.0f));
}

// ---------------------------------------------------------------------------
//  核心回归：给定半径时，贴墙位置必须被拒绝
// ---------------------------------------------------------------------------

TEST(WalkableTest, InflationRejectsPositionTooCloseToWall) {
    auto world = makeWallWorld();

    // 距墙 2 单位，实体半径 11 → 圆必然与墙列相交 → 不可站立
    EXPECT_FALSE(world->isPositionWalkable(leftOfWall(2.0f), 11.0f))
        << "实体半径 11、距墙仅 2 时必须判不可站立（原实现会漏判）";

    // 距墙 30 单位，半径 11 → 圆与墙相距 19 → 可站立
    EXPECT_TRUE(world->isPositionWalkable(leftOfWall(30.0f), 11.0f))
        << "距墙 30 > 半径 11，应判可站立";
}

TEST(WalkableTest, InflationWorksSymmetricallyOnBothSides) {
    auto world = makeWallWorld();

    EXPECT_FALSE(world->isPositionWalkable(leftOfWall(2.0f), 11.0f));
    EXPECT_FALSE(world->isPositionWalkable(rightOfWall(2.0f), 11.0f))
        << "膨胀检查必须对墙的两侧一视同仁（原实现只看中心格，两侧都漏判）";

    EXPECT_TRUE(world->isPositionWalkable(leftOfWall(30.0f), 11.0f));
    EXPECT_TRUE(world->isPositionWalkable(rightOfWall(30.0f), 11.0f));
}

TEST(WalkableTest, LargerRadiusRejectsMorePositions) {
    auto world = makeWallWorld();

    // 距墙 20：半径 10 时可站立（20 > 10），半径 30 时不可（20 < 30）
    const glm::vec2 point = leftOfWall(20.0f);

    EXPECT_TRUE(world->isPositionWalkable(point, 10.0f));
    EXPECT_FALSE(world->isPositionWalkable(point, 30.0f))
        << "更大的半径必须拒绝更多位置——这正是膨胀检查的意义";
}

TEST(WalkableTest, HalfCellOffsetIsHandled) {
    // 【为什么单独测这个】
    // 3x3 逐格判断处理不了"点在格内但偏向一侧"的情况：
    // 圆心可能只探进邻格 1~2 个单位，而邻格虽小，整格却是阻挡的。
    // 真正的几何判定（圆 vs AABB）不关心格边界在哪，只算距离。
    //
    // 这里把圆心放在格心附近、距墙 6 单位处，半径 11 → 圆伸进墙 5 单位。
    auto world = makeWallWorld();

    EXPECT_FALSE(world->isPositionWalkable(leftOfWall(6.0f), 11.0f))
        << "圆心在格内但圆伸进邻格时必须判不可站立";
}

// ---------------------------------------------------------------------------
//  边界：地图外一律不可站立
// ---------------------------------------------------------------------------

TEST(WalkableTest, OutOfBoundsIsNeverWalkable) {
    auto world = makeWallWorld();

    EXPECT_FALSE(world->isPositionWalkable({-10.0f, 600.0f}, 0.0f))
        << "地图外应判不可站立";
    EXPECT_FALSE(world->isPositionWalkable({-10.0f, 600.0f}, 11.0f));
    EXPECT_FALSE(world->isPositionWalkable({5000.0f, 600.0f}, 11.0f));
}

TEST(WalkableTest, RadiusOverhangingMapEdgeIsRejected) {
    // 【回归：圆心在图内、但身体伸出图外】
    //
    // 只判"圆心是否越界"是不够的：地图之外同样没有可站立的空间，
    // 一个半径 11 的实体站在格子里、贴着 0 号格边缘时，身体的 7 个
    // 单位其实悬在地图外。
    //
    // 这个用例钉住的是一处**注释与实现相反**的缺陷：膨胀循环里原本写着
    // `if (!inBounds(probe) || isWalkable(probe)) continue;` —— 越界格被
    // "跳过"而不是"判阻挡"，注释却写着"越界视为阻挡"。于是这种位置
    // 会被判成可站立（圆心越界的那条用例覆盖不到它）。
    auto world = makeWallWorld();

    const glm::vec2 corner{4.0f, 4.0f};   // 贴着地图左下角
    ASSERT_TRUE(world->navGrid().contains(corner)) << "前提：圆心仍在地图内";

    EXPECT_FALSE(world->isPositionWalkable(corner, 11.0f))
        << "半径伸出地图边界时必须判不可站立（地图外没有可站立空间）";

    // 反过来：把点往内挪到大半个身位之后，就该是合法的
    EXPECT_TRUE(world->isPositionWalkable({14.0f, 14.0f}, 11.0f))
        << "完全落在地图内的位置不应被误判";

    // 对角同理（右下角）
    const glm::vec2 farCorner = world->navGrid().origin()
                              + glm::vec2{1600.0f - 4.0f, 1200.0f - 4.0f};
    EXPECT_FALSE(world->isPositionWalkable(farCorner, 11.0f));
}

// ---------------------------------------------------------------------------
//  snapToWalkable：把贴墙点推到合法位置
// ---------------------------------------------------------------------------

TEST(WalkableTest, SnapToWalkablePushesOffTheWall) {
    auto world = makeWallWorld();

    // 故意给一个会膨胀失败的位置（墙内）
    const glm::vec2 insideWall{800.0f, 600.0f};
    ASSERT_FALSE(world->isPositionWalkable(insideWall, 11.0f));

    const glm::vec2 snapped = world->snapToWalkable(insideWall, 11.0f);

    EXPECT_TRUE(world->isPositionWalkable(snapped, 11.0f))
        << "吸附后的点必须真的可站立";
    EXPECT_NE(snapped.x, insideWall.x)
        << "吸附后不应与原点重合（否则等于没吸附）";
}

TEST(WalkableTest, SnapToWalkableKeepsAlreadyValidPositions) {
    auto world = makeWallWorld();

    const glm::vec2 valid = leftOfWall(60.0f);
    ASSERT_TRUE(world->isPositionWalkable(valid, 11.0f));

    const glm::vec2 result = world->snapToWalkable(valid, 11.0f);

    EXPECT_FLOAT_EQ(result.x, valid.x);
    EXPECT_FLOAT_EQ(result.y, valid.y)
        << "本就合法的点不应被移动";
}

// ---------------------------------------------------------------------------
//  回归：移除障碍物必须同时解除它对导航网格的阻挡
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  回归：贴着地图边缘的敌人「自己卡住」
// ---------------------------------------------------------------------------

TEST(WalkableTest, CanStandRejectsOutOfMapAndWallHugging) {
    auto world = makeWallWorld();
    const AI::NavGrid& grid = world->navGrid();

    // 地图外：worldToGrid 会把越界坐标**钳制**到边界格，所以必须先用
    // contains 拦截——否则"地图外的点"会被误判成可通行（这正是敌人卡死的
    // 直接原因）。
    EXPECT_FALSE(grid.canStand({800.0f, 5000.0f}, 11.0f)) << "地图外必须判不可站";
    EXPECT_FALSE(grid.canStand({-50.0f, 600.0f}, 11.0f));

    // 贴墙：墙左侧 6 单位处，半径 11 的圆必然插进墙里
    EXPECT_FALSE(grid.canStand(leftOfWall(6.0f), 11.0f));
    EXPECT_TRUE(grid.canStand(leftOfWall(30.0f), 11.0f));
}

TEST(WalkableTest, EnemiesNeverFreezeInPlace) {
    // 【这条测试对应一个真实现象：画面右下角的敌人一动不动】
    //
    // 成因是三处缺陷叠加：
    //   1. 导航网格按**整除**生成：1200 / 32 = 37.5 → 37 行，只覆盖到
    //      y = 1184。地图最下面 16 个单位没有导航数据，而下边界墙位于
    //      y ≥ 1200，与网格毫无交集 → fillRect 的相交测试直接 return，
    //      下边界等于**没有墙**（上边界只是因为恰好"碰到" y = 0 才生效）。
    //   2. 巡逻点校验用的是 `isWalkable(worldToGrid(candidate))`：不看实体
    //      半径，而且 worldToGrid 会把地图外的点钳制到边界格 —— 于是
    //      y = 1272 这样明显在地图外的点被判成"可通行"并被选中。
    //   3. 换点的常规条件是"已到达目标"。目标在地图外、永远够不到，
    //      于是连换点都做不到 → 敌人朝地图外硬推，被边界约束顶在原地。
    //
    // 现在：网格向上取整覆盖满、候选点用 NavGrid::canStand（半径 + 越界）
    // 判定、并且加了"迟迟走不到就换点"的兜底。这条测试从行为上钉死它：
    // 任何敌人都不许长时间静止。
    Game::WorldConfig config;
    config.initialEnemyCount = 3;
    Game::World world(config);
    world.initialize();

    // 先把玩家打死：这样敌人不会停在"交战保持射程"的状态里，
    // 全部回到巡逻/搜索行为，静止就意味着真的卡住了。
    if (Game::Player* player = world.player()) {
        world.applyDamage(player->id(), 10000.0f, 0, player->position(), 0.0);
    }

    constexpr double kFixedDelta = 1.0 / 60.0;
    double t = 0.0;
    for (int i = 0; i < 60 * 5; ++i) {   // 先跑 5 秒进入稳定行为
        world.update(kFixedDelta, t);
        t += kFixedDelta;
    }

    std::vector<glm::vec2> before;
    for (const Game::Enemy* enemy : world.enemies()) {
        before.push_back(enemy->position());
    }
    ASSERT_FALSE(before.empty());

    // 采样期间同时检查不变式：移动目标必须是自己站得下的地方
    const int samples = 60 * 5;
    for (int i = 0; i < samples; ++i) {
        world.update(kFixedDelta, t);
        t += kFixedDelta;

        if (i % 30 == 0) {
            for (const Game::Enemy* enemy : world.enemies()) {
                EXPECT_TRUE(world.isPositionWalkable(enemy->moveTarget(), enemy->radius()))
                    << enemy->name() << " 的移动目标 ("
                    << enemy->moveTarget().x << ", " << enemy->moveTarget().y
                    << ") 自己根本站不下 —— 它会朝着一个够不到的点硬推";
            }
        }
    }

    const std::vector<Game::Enemy*> after = world.enemies();
    ASSERT_EQ(after.size(), before.size());
    for (size_t i = 0; i < after.size(); ++i) {
        const glm::vec2 delta = after[i]->position() - before[i];
        const float moved = std::sqrt(delta.x * delta.x + delta.y * delta.y);
        EXPECT_GT(moved, 50.0f)
            << after[i]->name() << " 在 5 秒里只移动了 " << moved
            << " 单位（停在 (" << before[i].x << "," << before[i].y << ") → ("
            << after[i]->position().x << "," << after[i]->position().y
            << ")）—— 巡逻时静止就是卡住了";
    }
}

TEST(WalkableTest, RemovingObstacleRestoresWalkability) {
    // 【为什么这条必须有】
    // spawnObstacle 会用 fillRect(..., true) 把障碍所在区域标成阻挡，
    // 但 removeEntity 里**没有任何地方把它改回可通行**：障碍消失了，
    // 导航网格上却留下一堵"看不见的墙"。
    // 后果是 AI 绕着空地走、isPositionWalkable 判空地不可站立、
    // 子弹被虚无地形挡住——全都极难排查，因为画面上那里什么都没有。
    //
    // 这类"派生状态忘记同步"在本项目已经出现过一次（coverPositions_），
    // 这条测试用来保证导航网格不会重蹈覆辙。
    auto world = makeWallWorld();

    const glm::vec2 wallCenter{800.0f, 600.0f};
    ASSERT_FALSE(world->isPositionWalkable(wallCenter, 11.0f))
        << "前提：墙内应判不可站立";

    Game::Obstacle* wall = nullptr;
    for (const auto& entity : world->entities()) {
        if (entity->type() == Game::EntityType::Obstacle) {
            wall = static_cast<Game::Obstacle*>(entity.get());
        }
    }
    ASSERT_NE(wall, nullptr);
    const uint32_t wallId = wall->id();

    ASSERT_TRUE(world->removeEntity(wallId));

    EXPECT_TRUE(world->isPositionWalkable(wallCenter, 11.0f))
        << "障碍物已移除，该位置却仍被判不可站立——导航网格留下了幽灵墙";
    EXPECT_TRUE(world->navGrid().isWalkable(world->navGrid().worldToGrid(wallCenter)))
        << "导航网格的阻挡标记没有被清掉";
}

// ---------------------------------------------------------------------------
//  回归：生成点不得嵌进墙里
// ---------------------------------------------------------------------------

TEST(WalkableTest, SpawnedEnemyDoesNotOverlapObstacles) {
    // 生成点必须离障碍物至少一个实体半径。
    // 这条直接锁住 findFreeSpawnPosition 的调用方传参：
    // 早先它调用 isPositionWalkable(candidate)（半径 0，等于没检查），
    // 于是敌人可能半嵌在掩体里——生成点数量少时不易复现。
    Game::WorldConfig config;
    config.generateDemoObstacles = true;   // 用真实的示例地形
    config.initialEnemyCount = 0;
    config.minSpawnDistance = 0.0f;        // 关掉间距要求，专注"是否嵌墙"

    Game::World world(config);
    world.initialize();

    // 半径必须取自配置，不能写字面量 11：写死之后它恰好等于默认配置，
    // 断言就与实现同源了——日后把 EnemyConfig::radius 调大，实现按新值
    // 膨胀、测试仍按 11 复核，用例会**变弱却依然通过**。
    const float enemyRadius = Game::EnemyConfig{}.radius;

    for (int i = 0; i < 12; ++i) {
        auto* enemy = world.spawnEnemy();
        ASSERT_NE(enemy, nullptr);
        EXPECT_TRUE(world.isPositionWalkable(enemy->position(), enemyRadius))
            << "敌人生成在 (" << enemy->position().x << ", " << enemy->position().y
            << ")，该点按自身半径判定不可站立——说明生成点没做膨胀检查";
    }
}
