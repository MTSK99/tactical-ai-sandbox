// ===========================================================================
//  tests/test_pathfinding.cpp
//
//  寻路测试。除了「能找到路」这种基本功能，重点验证：
//    - 起点/终点在障碍物内时的优雅降级（部分路径）
//    - 对角移动不穿墙角
//    - 路径平滑的正确性
//    - 无解时不会死循环
// ===========================================================================
#include <gtest/gtest.h>

#include <cmath>

#include "ai/Pathfinding.h"

using namespace TacticalAI::AI;

namespace {

/// 构造一个 32x32 格、每格 10 单位的空地网格。
/// 格边长 10 让测试的距离计算比较好心算。
NavGrid makeEmptyGrid(int columns = 32, int rows = 32, float cellSize = 10.0f) {
    NavGrid grid;
    grid.initialize(glm::vec2(0.0f, 0.0f), cellSize, columns, rows);
    return grid;
}

/// 在世界坐标里放置一堵墙（实心矩形）。
void placeWall(NavGrid& grid, glm::vec2 min, glm::vec2 max) {
    grid.fillRect(min, max, true);
}

} // namespace

// ===========================================================================
//  NavGrid
// ===========================================================================
TEST(NavGridTest, InitializesFullyWalkable) {
    const NavGrid grid = makeEmptyGrid(10, 8);

    EXPECT_TRUE(grid.isInitialized());
    EXPECT_EQ(grid.columns(), 10);
    EXPECT_EQ(grid.rows(), 8);
    EXPECT_FLOAT_EQ(grid.cellSize(), 10.0f);

    // 默认全部可通行
    EXPECT_EQ(grid.walkableCellCount(), 80u);

    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 10; ++x) {
            EXPECT_TRUE(grid.isWalkable({x, y})) << "格 (" << x << "," << y << ") 应可通行";
        }
    }
}

TEST(NavGridTest, WorldToGridConversion) {
    const NavGrid grid = makeEmptyGrid(32, 32, 10.0f);

    // 格 (0,0) 覆盖世界 [0,10) x [0,10)
    EXPECT_EQ(grid.worldToGrid({0.0f, 0.0f}), (GridCoord{0, 0}));
    EXPECT_EQ(grid.worldToGrid({5.0f, 5.0f}), (GridCoord{0, 0}));
    EXPECT_EQ(grid.worldToGrid({10.0f, 10.0f}), (GridCoord{1, 1}));
    EXPECT_EQ(grid.worldToGrid({95.0f, 15.0f}), (GridCoord{9, 1}));
}

TEST(NavGridTest, WorldToGridClampsOutOfBounds) {
    const NavGrid grid = makeEmptyGrid(10, 10, 10.0f);

    // 网格外的坐标应被钳制到边界格。
    // 用夹紧而非报错，让寻路能「优雅降级」而不是直接失败。
    EXPECT_EQ(grid.worldToGrid({-500.0f, -500.0f}), (GridCoord{0, 0}));
    EXPECT_EQ(grid.worldToGrid({5000.0f, 5000.0f}), (GridCoord{9, 9}));
}

TEST(NavGridTest, GridToWorldReturnsCellCenter) {
    const NavGrid grid = makeEmptyGrid(10, 10, 10.0f);

    // 返回格中心而非角点：作为移动目标时不那么容易擦到相邻障碍
    const glm::vec2 center = grid.gridToWorld({0, 0});
    EXPECT_FLOAT_EQ(center.x, 5.0f);
    EXPECT_FLOAT_EQ(center.y, 5.0f);

    const glm::vec2 center2 = grid.gridToWorld({3, 4});
    EXPECT_FLOAT_EQ(center2.x, 35.0f);
    EXPECT_FLOAT_EQ(center2.y, 45.0f);
}

TEST(NavGridTest, OutOfBoundsIsBlocked) {
    const NavGrid grid = makeEmptyGrid(10, 10);

    // 越界视为阻挡：防止寻路意外走出网格
    EXPECT_FALSE(grid.isWalkable({-1, 0}));
    EXPECT_FALSE(grid.isWalkable({10, 0}));
    EXPECT_FALSE(grid.isWalkable({0, -1}));
    EXPECT_FALSE(grid.isWalkable({0, 10}));
    EXPECT_TRUE(grid.isWalkable({9, 9}));
}

TEST(NavGridTest, FillRectBlocksCells) {
    NavGrid grid = makeEmptyGrid(32, 32, 10.0f);

    // 阻挡世界坐标 [100,200] x [100,200]
    placeWall(grid, {100.0f, 100.0f}, {200.0f, 200.0f});

    EXPECT_FALSE(grid.isWalkable(grid.worldToGrid({150.0f, 150.0f})));
    EXPECT_TRUE(grid.isWalkable(grid.worldToGrid({50.0f, 50.0f})));
    EXPECT_LT(grid.walkableCellCount(), 32u * 32u);
}

TEST(NavGridTest, SetCostAndHazard) {
    NavGrid grid = makeEmptyGrid(10, 10);

    const GridCoord coord{3, 3};
    EXPECT_FLOAT_EQ(grid.costAt(coord), 1.0f);   // 默认基准代价

    grid.setCost(coord, 2.5f);
    EXPECT_FLOAT_EQ(grid.costAt(coord), 2.5f);

    EXPECT_FALSE(grid.isHazard(coord));
    grid.setHazard(coord, true);
    EXPECT_TRUE(grid.isHazard(coord));
    // 危险格仍可通行（只是代价更高），这与阻挡不同
    EXPECT_TRUE(grid.isWalkable(coord));
}

TEST(NavGridTest, CostHasLowerBound) {
    NavGrid grid = makeEmptyGrid(10, 10);
    const GridCoord coord{1, 1};

    // 代价不能为 0——否则 A* 的 g 值停止增长，可能破坏启发函数一致性
    grid.setCost(coord, 0.0f);
    EXPECT_GT(grid.costAt(coord), 0.0f);

    grid.setCost(coord, -5.0f);
    EXPECT_GT(grid.costAt(coord), 0.0f);
}

TEST(NavGridTest, ClearanceFieldComputedCorrectly) {
    NavGrid grid = makeEmptyGrid(20, 20, 10.0f);

    // 在中心放一个阻挡格
    grid.setBlocked({10, 10}, true);
    grid.computeClearanceField();

    // 阻挡格自身间距为 0
    EXPECT_FLOAT_EQ(grid.clearanceAt({10, 10}), 0.0f);
    // 相邻格为 1
    EXPECT_FLOAT_EQ(grid.clearanceAt({11, 10}), 1.0f);
    EXPECT_FLOAT_EQ(grid.clearanceAt({10, 11}), 1.0f);
    // 再远一格为 2
    EXPECT_FLOAT_EQ(grid.clearanceAt({12, 10}), 2.0f);
}

// ===========================================================================
//  基础寻路
// ===========================================================================
TEST(PathfinderTest, StraightPathOnEmptyGrid) {
    const NavGrid grid = makeEmptyGrid();
    Pathfinder pathfinder;

    const PathResult result = pathfinder.findPath(grid, {50.0f, 50.0f}, {250.0f, 50.0f});

    ASSERT_TRUE(result.success);
    EXPECT_FALSE(result.partial);
    ASSERT_GE(result.waypoints.size(), 2u);

    // 起点和终点应精确匹配请求（不被吸附到格中心）
    EXPECT_FLOAT_EQ(result.waypoints.front().x, 50.0f);
    EXPECT_FLOAT_EQ(result.waypoints.front().y, 50.0f);
    EXPECT_FLOAT_EQ(result.waypoints.back().x, 250.0f);
    EXPECT_FLOAT_EQ(result.waypoints.back().y, 50.0f);
}

TEST(PathfinderTest, SmoothingCollapsesStraightLine) {
    const NavGrid grid = makeEmptyGrid();
    Pathfinder pathfinder;

    const PathResult result = pathfinder.findPath(grid, {50.0f, 50.0f}, {250.0f, 50.0f});

    ASSERT_TRUE(result.success);
    // 完全空旷的直线路径，平滑后应只剩起点+终点。
    // 如果平滑失效，这里会有十几个格点。
    EXPECT_EQ(result.waypoints.size(), 2u) << "直线路径应被拉直为两个端点";
}

TEST(PathfinderTest, PathAvoidsWall) {
    NavGrid grid = makeEmptyGrid(32, 32, 10.0f);

    // 一堵竖墙，中间留一个缺口
    placeWall(grid, {150.0f, 0.0f}, {160.0f, 150.0f});
    placeWall(grid, {150.0f, 200.0f}, {160.0f, 320.0f});

    Pathfinder pathfinder;
    const PathResult result = pathfinder.findPath(grid, {50.0f, 150.0f}, {280.0f, 150.0f});

    ASSERT_TRUE(result.success);

    // 路径上的每个点都必须可通行
    for (const glm::vec2& waypoint : result.waypoints) {
        const GridCoord coord = grid.worldToGrid(waypoint);
        EXPECT_TRUE(grid.isWalkable(coord))
            << "路径点 (" << waypoint.x << "," << waypoint.y << ") 落在阻挡格上";
    }

    // 起终点之间的直线被墙挡住，因此路径必须比直线长。
    //
    // 【修复】原先写的是 straightLine * 0.95f，即允许路径比直线还短 5%，
    // 与注释「必须比直线长」自相矛盾，也检验不出"真的绕行了"。
    const float straightLine = 230.0f;
    EXPECT_GT(result.geometricLength(), straightLine);
}

TEST(PathfinderTest, StartTooCloseToGoal) {
    const NavGrid grid = makeEmptyGrid();
    Pathfinder pathfinder;

    // 起点终点几乎重合：不应崩溃，也不应产生荒谬的长路径
    const PathResult result = pathfinder.findPath(grid, {50.0f, 50.0f}, {51.0f, 50.0f});

    // 允许成功（路径极短）或失败（起点即终点），但不能有大量节点
    EXPECT_LT(result.waypoints.size(), 5u);
}

// ===========================================================================
//  降级行为（最容易被忽略、也最容易出问题的部分）
// ===========================================================================
TEST(PathfinderTest, StartInsideWallRecoversToNearbyCell) {
    NavGrid grid = makeEmptyGrid(32, 32, 10.0f);
    placeWall(grid, {100.0f, 100.0f}, {200.0f, 200.0f});

    Pathfinder pathfinder;
    // 起点在墙内部——模拟「单位被卡在障碍里」
    const PathResult result = pathfinder.findPath(grid, {150.0f, 150.0f}, {300.0f, 300.0f});

    // 应能把单位救出来（找到附近的可行格作为起点）而不是直接失败。
    // 这个行为对「AI 卡住后能否自愈」至关重要。
    EXPECT_TRUE(result.success) << "起点在墙内时应向外搜索可行格而非直接失败";
}

TEST(PathfinderTest, GoalInsideWallProducesPartialPath) {
    NavGrid grid = makeEmptyGrid(32, 32, 10.0f);
    placeWall(grid, {200.0f, 0.0f}, {320.0f, 320.0f});

    Pathfinder pathfinder;
    // 目标点在墙内：玩家点了敌人背后的不可达位置
    const PathResult result = pathfinder.findPath(grid, {50.0f, 50.0f}, {250.0f, 250.0f});

    ASSERT_TRUE(result.success) << "允许部分路径时不应失败";
    EXPECT_TRUE(result.partial) << "目标不可达时应标记为部分路径";

    // 路径终点应在墙外（贴近墙的可行格）
    const GridCoord lastCoord = grid.worldToGrid(result.waypoints.back());
    EXPECT_TRUE(grid.isWalkable(lastCoord))
        << "部分路径的终点必须落在可通行区域内";
}

TEST(PathfinderTest, UnreachableGoalReturnsFailureOrPartial) {
    NavGrid grid = makeEmptyGrid(20, 20, 10.0f);

    // 四周围墙，内部完全封闭
    placeWall(grid, {50.0f, 50.0f}, {150.0f, 150.0f});

    Pathfinder pathfinder;
    const PathResult result = pathfinder.findPath(grid, {10.0f, 10.0f}, {100.0f, 100.0f});

    // 要么成功（部分路径走到墙外最近处），要么失败。
    // 关键是：必须有确定的结果，不能挂死或返回垃圾数据。
    //
    // 【修复】原断言 `success || !success` 是恒真的重言式，测试名承诺的
    // 「失败或部分路径」契约完全没有验证。改为真正表达契约的断言。
    EXPECT_TRUE(!result.success || result.partial)
        << "不可达目标要么失败，要么必须标记为部分路径";

    if (result.success) {
        for (const glm::vec2& waypoint : result.waypoints) {
            const GridCoord coord = grid.worldToGrid(waypoint);
            EXPECT_TRUE(grid.isWalkable(coord))
                << "任何返回的路径点都必须是可通行的";
        }
    }
}

TEST(PathfinderTest, UninitializedGridReturnsEmptyResult) {
    NavGrid grid;   // 未初始化
    Pathfinder pathfinder;

    const PathResult result = pathfinder.findPath(grid, {0.0f, 0.0f}, {100.0f, 100.0f});

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.waypoints.empty());
}

// ===========================================================================
//  对角移动与墙角
// ===========================================================================
TEST(PathfinderTest, DiagonalMovesAreShorterThanOrthogonal) {
    NavGrid grid = makeEmptyGrid(32, 32, 10.0f);

    // 关键：关闭平滑。空网格上平滑会把两种路径都拉成同一条直线，
    // 那样就测不出「对角线 vs 曼哈顿」的本质差别了。
    // 这里要比较的是「原始格路径」的几何长度。
    PathfindingConfig diagonalConfig;
    diagonalConfig.allowDiagonal = true;
    diagonalConfig.smoothPath = false;
    Pathfinder diagonalPathfinder(diagonalConfig);

    PathfindingConfig orthogonalConfig;
    orthogonalConfig.allowDiagonal = false;
    orthogonalConfig.smoothPath = false;
    Pathfinder orthogonalPathfinder(orthogonalConfig);

    const PathResult diagonal = diagonalPathfinder.findPath(grid, {50.0f, 50.0f}, {250.0f, 250.0f});
    const PathResult orthogonal = orthogonalPathfinder.findPath(grid, {50.0f, 50.0f}, {250.0f, 250.0f});

    ASSERT_TRUE(diagonal.success);
    ASSERT_TRUE(orthogonal.success);

    // 对角路径应明显短于曼哈顿路径（理想比例约 1/sqrt(2) ≈ 0.707）
    EXPECT_LT(diagonal.geometricLength(), orthogonal.geometricLength());
}

TEST(PathfinderTest, DoesNotCutThroughWallCorners) {
    NavGrid grid = makeEmptyGrid(32, 32, 10.0f);

    // 构造一个对角墙角：这样从左上到右下若允许斜穿会「穿过」墙的尖角
    placeWall(grid, {100.0f, 90.0f}, {110.0f, 200.0f});   // 竖墙
    placeWall(grid, {90.0f, 100.0f}, {200.0f, 110.0f});   // 横墙

    Pathfinder pathfinder;
    const PathResult result = pathfinder.findPath(grid, {70.0f, 70.0f}, {230.0f, 230.0f});

    ASSERT_TRUE(result.success);

    // 所有路径点必须在可通行格上
    for (const glm::vec2& waypoint : result.waypoints) {
        const GridCoord coord = grid.worldToGrid(waypoint);
        EXPECT_TRUE(grid.isWalkable(coord));
    }

    // 平滑后的路径还要保证相邻点之间有视线（否则平滑会「穿过」墙）
    for (size_t i = 0; i + 1 < result.waypoints.size(); ++i) {
        EXPECT_TRUE(Pathfinder::hasLineOfSight(grid, result.waypoints[i],
                                               result.waypoints[i + 1]))
            << "平滑后的路径段 " << i << " 穿过了障碍";
    }
}

// ===========================================================================
//  视线检测
// ===========================================================================
TEST(PathfinderTest, HasLineOfSightOnEmptyGrid) {
    const NavGrid grid = makeEmptyGrid();

    EXPECT_TRUE(Pathfinder::hasLineOfSight(grid, {10.0f, 10.0f}, {200.0f, 200.0f}));
    EXPECT_TRUE(Pathfinder::hasLineOfSight(grid, {50.0f, 50.0f}, {50.0f, 50.0f}));
}

TEST(PathfinderTest, HasLineOfSightBlockedByWall) {
    NavGrid grid = makeEmptyGrid(32, 32, 10.0f);
    placeWall(grid, {150.0f, 0.0f}, {160.0f, 320.0f});   // 完全隔断的竖墙

    EXPECT_FALSE(Pathfinder::hasLineOfSight(grid, {50.0f, 150.0f}, {250.0f, 150.0f}));

    // 但同一侧的点之间应有视线
    EXPECT_TRUE(Pathfinder::hasLineOfSight(grid, {20.0f, 150.0f}, {140.0f, 150.0f}));
}

TEST(PathfinderTest, HasLineOfSightRespectsBoundary) {
    const NavGrid grid = makeEmptyGrid(10, 10, 10.0f);

    // 越界被视为阻挡，因此穿出网格的视线应失败
    EXPECT_FALSE(Pathfinder::hasLineOfSight(grid, {50.0f, 50.0f}, {500.0f, 50.0f}));
}

// ===========================================================================
//  路径平滑
// ===========================================================================
TEST(PathfinderTest, SmoothRemovesRedundantWaypoints) {
    const NavGrid grid = makeEmptyGrid();

    // 一条直线上的多个点 —— 平滑后应只剩两个端点
    const std::vector<glm::vec2> points{
        {10.0f, 50.0f}, {30.0f, 50.0f}, {50.0f, 50.0f}, {70.0f, 50.0f}, {90.0f, 50.0f}
    };

    const std::vector<glm::vec2> smoothed = Pathfinder::smooth(grid, points);

    ASSERT_GE(smoothed.size(), 2u);
    EXPECT_EQ(smoothed.size(), 2u);
    EXPECT_FLOAT_EQ(smoothed.front().x, 10.0f);
    EXPECT_FLOAT_EQ(smoothed.back().x, 90.0f);
}

TEST(PathfinderTest, SmoothKeepsWaypointsNeededToAvoidWalls) {
    NavGrid grid = makeEmptyGrid(32, 32, 10.0f);
    placeWall(grid, {150.0f, 0.0f}, {160.0f, 150.0f});
    placeWall(grid, {150.0f, 200.0f}, {160.0f, 320.0f});

    // 一条 L 形路径，必须绕过墙的缺口
    const std::vector<glm::vec2> points{
        {50.0f, 150.0f}, {100.0f, 150.0f}, {155.0f, 175.0f}, {200.0f, 150.0f}, {280.0f, 150.0f}
    };

    const std::vector<glm::vec2> smoothed = Pathfinder::smooth(grid, points);

    // 不能过度平滑到穿过墙：结果至少需要 3 个点才能绕过缺口
    ASSERT_GE(smoothed.size(), 3u) << "平滑不应把需要绕路的路径压成直线";

    for (size_t i = 0; i + 1 < smoothed.size(); ++i) {
        EXPECT_TRUE(Pathfinder::hasLineOfSight(grid, smoothed[i], smoothed[i + 1]))
            << "平滑后的每一段都必须有视线";
    }
}

TEST(PathfinderTest, SmoothHandlesShortPaths) {
    const NavGrid grid = makeEmptyGrid();

    EXPECT_TRUE(Pathfinder::smooth(grid, {}).empty());

    const std::vector<glm::vec2> single{{10.0f, 10.0f}};
    EXPECT_EQ(Pathfinder::smooth(grid, single).size(), 1u);

    const std::vector<glm::vec2> pair{{10.0f, 10.0f}, {20.0f, 20.0f}};
    EXPECT_EQ(Pathfinder::smooth(grid, pair).size(), 2u);
}

// ===========================================================================
//  pointAhead（路径跟随）
// ===========================================================================
TEST(PathfinderTest, PointAheadReturnsPointAlongPath) {
    const std::vector<glm::vec2> path{{0.0f, 0.0f}, {100.0f, 0.0f}, {100.0f, 100.0f}};
    size_t cursor = 0;

    const auto ahead = Pathfinder::pointAhead(path, cursor, {0.0f, 0.0f}, 50.0f);

    ASSERT_TRUE(ahead.has_value());
    EXPECT_NEAR(ahead->x, 50.0f, 1.0f);
    EXPECT_NEAR(ahead->y, 0.0f, 1.0f);
}

TEST(PathfinderTest, PointAheadAdvancesCursorPastReachedWaypoints) {
    const std::vector<glm::vec2> path{{0.0f, 0.0f}, {10.0f, 0.0f}, {100.0f, 0.0f}};
    size_t cursor = 0;

    // 当前位置已经很接近前两个路点，游标应跳过它们
    const auto ahead = Pathfinder::pointAhead(path, cursor, {9.0f, 0.0f}, 30.0f);

    ASSERT_TRUE(ahead.has_value());
    EXPECT_GE(cursor, 1u) << "游标应跳过已到达的路点";
}

TEST(PathfinderTest, PointAheadReturnsNulloptWhenPathExhausted) {
    const std::vector<glm::vec2> path{{0.0f, 0.0f}, {10.0f, 0.0f}};
    size_t cursor = 0;

    // 到达终点后应返回 nullopt，让调用方知道「走完了」
    const auto ahead = Pathfinder::pointAhead(path, cursor, {10.0f, 0.0f}, 30.0f);
    EXPECT_FALSE(ahead.has_value());
}

TEST(PathfinderTest, PointAheadHandlesEmptyPath) {
    const std::vector<glm::vec2> path;
    size_t cursor = 0;

    EXPECT_FALSE(Pathfinder::pointAhead(path, cursor, {0.0f, 0.0f}, 30.0f).has_value());
}

TEST(PathfinderTest, PointAheadClampsToPathEnd) {
    const std::vector<glm::vec2> path{{0.0f, 0.0f}, {50.0f, 0.0f}};
    size_t cursor = 0;

    // ahead 超出剩余路径长度 -> 返回终点
    const auto ahead = Pathfinder::pointAhead(path, cursor, {0.0f, 0.0f}, 1000.0f);

    ASSERT_TRUE(ahead.has_value());
    EXPECT_FLOAT_EQ(ahead->x, 50.0f);
}

// ===========================================================================
//  地形代价
// ===========================================================================
TEST(PathfinderTest, HighCostCellsAreAvoided) {
    NavGrid grid = makeEmptyGrid(32, 32, 10.0f);

    // 在地图中间放一片高代价区（模拟泥地/危险区）
    for (int y = 0; y < 32; ++y) {
        for (int x = 12; x <= 18; ++x) {
            grid.setCost({x, y}, 12.0f);   // 代价极高
        }
    }

    Pathfinder pathfinder;
    const PathResult result = pathfinder.findPath(grid, {50.0f, 150.0f}, {270.0f, 150.0f});

    ASSERT_TRUE(result.success);

    // 直线会穿过高代价区（可行但有惩罚）。
    // A* 应选择绕行——因为 12 倍代价远超过绕路的额外距离。
    int highCostWaypoints = 0;
    for (const glm::vec2& waypoint : result.waypoints) {
        if (grid.costAt(grid.worldToGrid(waypoint)) > 5.0f) {
            ++highCostWaypoints;
        }
    }

    // 允许端点附近经过，但不应整条路径都在高代价区里
    EXPECT_LT(highCostWaypoints, static_cast<int>(result.waypoints.size()))
        << "A* 应尽量避开高代价地形";
}

// ===========================================================================
//  统计
// ===========================================================================
TEST(PathfinderTest, StatsAccumulate) {
    const NavGrid grid = makeEmptyGrid();
    Pathfinder pathfinder;

    EXPECT_EQ(pathfinder.totalSearches(), 0u);

    pathfinder.findPath(grid, {50.0f, 50.0f}, {250.0f, 250.0f});
    pathfinder.findPath(grid, {60.0f, 60.0f}, {240.0f, 240.0f});

    EXPECT_EQ(pathfinder.totalSearches(), 2u);
    EXPECT_GT(pathfinder.totalExpandedNodes(), 0u);
    EXPECT_GT(pathfinder.averageExpandedNodes(), 0.0f);

    pathfinder.resetStats();
    EXPECT_EQ(pathfinder.totalSearches(), 0u);
    EXPECT_FLOAT_EQ(pathfinder.averageExpandedNodes(), 0.0f);
}

TEST(PathfinderTest, ExpandedNodesBoundedByGridSize) {
    const NavGrid grid = makeEmptyGrid(16, 16, 10.0f);
    Pathfinder pathfinder;

    const PathResult result = pathfinder.findPath(grid, {10.0f, 10.0f}, {150.0f, 150.0f});

    ASSERT_TRUE(result.success);
    // 搜索规模不应超过总格数——超过说明有重复扩展（堆实现有 bug）
    EXPECT_LE(result.expandedNodes, static_cast<int>(grid.columns()) * grid.rows());
}

// ===========================================================================
//  确定性
// ===========================================================================
TEST(PathfinderTest, SameInputProducesSamePath) {
    NavGrid grid = makeEmptyGrid(32, 32, 10.0f);
    placeWall(grid, {120.0f, 60.0f}, {130.0f, 250.0f});

    Pathfinder pathfinder;

    const PathResult first = pathfinder.findPath(grid, {50.0f, 50.0f}, {280.0f, 280.0f});
    const PathResult second = pathfinder.findPath(grid, {50.0f, 50.0f}, {280.0f, 280.0f});

    // 确定性是 AI 可调试的前提：同样的输入必须得到完全一样的输出，
    // 否则「上次跑的时候还行」这类问题无法复现。
    ASSERT_EQ(first.waypoints.size(), second.waypoints.size());
    for (size_t i = 0; i < first.waypoints.size(); ++i) {
        EXPECT_FLOAT_EQ(first.waypoints[i].x, second.waypoints[i].x);
        EXPECT_FLOAT_EQ(first.waypoints[i].y, second.waypoints[i].y);
    }
}
