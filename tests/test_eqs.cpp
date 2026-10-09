// ===========================================================================
//  tests/test_eqs.cpp
//
//  EQS 测试。重点验证：
//    - 分数曲线的数学正确性（这是所有评分的基础）
//    - 加权汇总与归一化
//    - 过滤器（硬约束）与分数阈值的区别
//    - 生成器的候选点数量与分布
//    - 确定性排序（防止 AI 在同分位置间抖动）
// ===========================================================================
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "ai/EQS.h"

using namespace TacticalAI::AI;

namespace {

/// 距离测试的辅助：构造一个只有单测试项的简单查询。
EQSQuery singleTestQuery(EQSTest test, EQSGenerator generator) {
    EQSQuery query;
    query.name = "SingleTest";
    query.generator = std::move(generator);
    query.tests.push_back(std::move(test));
    query.maxResults = 1;
    return query;
}

EQSContext makeContext(glm::vec2 self = {0.0f, 0.0f}, glm::vec2 target = {100.0f, 0.0f}) {
    EQSContext context;
    context.selfPosition = self;
    context.targetPosition = target;
    context.hasTarget = true;
    context.selfFacing = {1.0f, 0.0f};
    return context;
}

} // namespace

// ===========================================================================
//  ScoreCurve 数学
// ===========================================================================
TEST(ScoreCurveTest, LinearMapsRawToNormalized) {
    EQSTest test;
    test.curve = ScoreCurve::Linear;
    test.rangeMin = 0.0f;
    test.rangeMax = 100.0f;

    EXPECT_FLOAT_EQ(test.applyCurve(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(test.applyCurve(50.0f), 0.5f);
    EXPECT_FLOAT_EQ(test.applyCurve(100.0f), 1.0f);
}

TEST(ScoreCurveTest, LinearClampsOutOfRange) {
    EQSTest test;
    test.curve = ScoreCurve::Linear;
    test.rangeMin = 0.0f;
    test.rangeMax = 100.0f;

    // 超出区间时钳制，而不是外插产生 >1 或 <0 的分数
    EXPECT_FLOAT_EQ(test.applyCurve(-500.0f), 0.0f);
    EXPECT_FLOAT_EQ(test.applyCurve(5000.0f), 1.0f);
}

TEST(ScoreCurveTest, InverseLinearInvertsPreference) {
    EQSTest test;
    test.curve = ScoreCurve::InverseLinear;
    test.rangeMin = 0.0f;
    test.rangeMax = 100.0f;

    // 越小越满意——距离类测试的主力曲线
    EXPECT_FLOAT_EQ(test.applyCurve(0.0f), 1.0f);
    EXPECT_FLOAT_EQ(test.applyCurve(50.0f), 0.5f);
    EXPECT_FLOAT_EQ(test.applyCurve(100.0f), 0.0f);
}

TEST(ScoreCurveTest, QuadraticFavorsCloserValues) {
    EQSTest test;
    test.curve = ScoreCurve::Quadratic;
    test.rangeMin = 0.0f;
    test.rangeMax = 100.0f;

    // 平方衰减在接近最优值时更高、远离时更低（比线性更陡）
    const float nearScore = test.applyCurve(25.0f);    // normalized 0.25 -> 0.75^2 = 0.5625
    const float farScore = test.applyCurve(75.0f);     // normalized 0.75 -> 0.25^2 = 0.0625

    EXPECT_NEAR(nearScore, 0.5625f, 0.001f);
    EXPECT_NEAR(farScore, 0.0625f, 0.001f);

    // 相比线性，平方曲线对远处惩罚更重
    EXPECT_LT(farScore, 1.0f - 0.75f);
}

TEST(ScoreCurveTest, ConstantAlwaysReturnsOne) {
    EQSTest test;
    test.curve = ScoreCurve::Constant;
    test.rangeMin = 0.0f;
    test.rangeMax = 100.0f;

    EXPECT_FLOAT_EQ(test.applyCurve(0.0f), 1.0f);
    EXPECT_FLOAT_EQ(test.applyCurve(50.0f), 1.0f);
    EXPECT_FLOAT_EQ(test.applyCurve(100.0f), 1.0f);
}

TEST(ScoreCurveTest, DegenerateRangeIsHandledSafely) {
    EQSTest test;
    test.curve = ScoreCurve::Linear;
    test.rangeMin = 50.0f;
    test.rangeMax = 50.0f;   // 区间为零

    // 不能除零产生 NaN
    const float score = test.applyCurve(50.0f);
    EXPECT_FALSE(std::isnan(score));
    EXPECT_GE(score, 0.0f);
    EXPECT_LE(score, 1.0f);
}

// ===========================================================================
//  内置生成器
// ===========================================================================
TEST(EQSGeneratorTest, GridGeneratesExpectedPattern) {
    const auto generator = EnvironmentQuerySystem::gridGenerator(
        /*center=*/{0.0f, 0.0f}, /*radius=*/100.0f, /*spacing=*/50.0f);

    EQSContext context = makeContext();
    const std::vector<glm::vec2> points = generator(context);

    // radius 100 / spacing 50 -> 每轴 5 个点 (-100,-50,0,50,100)
    EXPECT_EQ(points.size(), 25u);

    // 所有点应在半径范围内
    for (const glm::vec2& point : points) {
        EXPECT_LE(std::fabs(point.x), 100.0f + 0.001f);
        EXPECT_LE(std::fabs(point.y), 100.0f + 0.001f);
    }
}

TEST(EQSGeneratorTest, GridWithJitterStaysNearGrid) {
    const float jitter = 5.0f;
    const auto generator = EnvironmentQuerySystem::gridGenerator(
        {0.0f, 0.0f}, 100.0f, 50.0f, jitter);

    EQSContext context = makeContext();
    const std::vector<glm::vec2> points = generator(context);

    EXPECT_EQ(points.size(), 25u);

    // 抖动不应让点跑出合理范围
    for (const glm::vec2& point : points) {
        EXPECT_LE(std::fabs(point.x), 100.0f + jitter + 0.001f);
        EXPECT_LE(std::fabs(point.y), 100.0f + jitter + 0.001f);
    }
}

TEST(EQSGeneratorTest, GridIsDeterministicForSameGameTime) {
    const auto generator = EnvironmentQuerySystem::gridGenerator(
        {0.0f, 0.0f}, 100.0f, 50.0f, /*jitter=*/8.0f);

    EQSContext context = makeContext();
    context.gameTime = 5.0;

    const auto first = generator(context);
    const auto second = generator(context);

    // 同一 gameTime 必须得到同一结果——否则 AI 的位置选择会在每步抖动。
    // 这是「用固定种子 RNG」的原因。
    ASSERT_EQ(first.size(), second.size());
    for (size_t i = 0; i < first.size(); ++i) {
        EXPECT_FLOAT_EQ(first[i].x, second[i].x);
        EXPECT_FLOAT_EQ(first[i].y, second[i].y);
    }
}

TEST(EQSGeneratorTest, RingGeneratesConcentricCircles) {
    const auto generator = EnvironmentQuerySystem::ringGenerator(
        /*center=*/{0.0f, 0.0f}, /*inner=*/50.0f, /*outer=*/150.0f,
        /*rings=*/3, /*pointsPerRing=*/8);

    EQSContext context = makeContext();
    const std::vector<glm::vec2> points = generator(context);

    EXPECT_EQ(points.size(), 24u);   // 3 环 x 8 点

    // 所有点到中心的距离应在 [50, 150] 内
    for (const glm::vec2& point : points) {
        const float distance = std::sqrt(point.x * point.x + point.y * point.y);
        EXPECT_GE(distance, 50.0f - 1.0f);
        EXPECT_LE(distance, 150.0f + 1.0f);
    }
}

TEST(EQSGeneratorTest, PointListReturnsExactlyThosePoints) {
    const std::vector<glm::vec2> source{{10.0f, 20.0f}, {30.0f, 40.0f}, {50.0f, 60.0f}};
    const auto generator = EnvironmentQuerySystem::pointListGenerator(source);

    EQSContext context = makeContext();
    const std::vector<glm::vec2> points = generator(context);

    ASSERT_EQ(points.size(), 3u);
    EXPECT_FLOAT_EQ(points[0].x, 10.0f);
    EXPECT_FLOAT_EQ(points[2].y, 60.0f);
}

TEST(EQSGeneratorTest, OffsetGeneratorIsRelativeToSelf) {
    const auto generator = EnvironmentQuerySystem::offsetGenerator(
        {{100.0f, 0.0f}, {0.0f, 100.0f}, {-50.0f, -50.0f}});

    EQSContext context = makeContext(/*self=*/{500.0f, 300.0f});
    const std::vector<glm::vec2> points = generator(context);

    ASSERT_EQ(points.size(), 3u);
    // 偏移量应叠加在 selfPosition 上
    EXPECT_FLOAT_EQ(points[0].x, 600.0f);
    EXPECT_FLOAT_EQ(points[0].y, 300.0f);
    EXPECT_FLOAT_EQ(points[2].x, 450.0f);
    EXPECT_FLOAT_EQ(points[2].y, 250.0f);
}

// ===========================================================================
//  内置测试项
// ===========================================================================
TEST(EQSTestTest, DistanceToTargetPrefersCloserPoints) {
    EQSTest test = EnvironmentQuerySystem::distanceToTargetTest(
        /*weight=*/1.0f, /*optimal=*/0.0f, /*max=*/200.0f);

    EQSContext context = makeContext({0.0f, 0.0f}, {100.0f, 0.0f});

    const float nearScore = test.applyCurve(test.measure({90.0f, 0.0f}, context));
    const float farScore = test.applyCurve(test.measure({250.0f, 0.0f}, context));

    EXPECT_GT(nearScore, farScore);
    EXPECT_FLOAT_EQ(nearScore, 0.95f);   // 距离 10，区间 [0,200] -> 1 - 0.05
}

TEST(EQSTestTest, DistanceToTargetWithoutTargetScoresZero) {
    EQSTest test = EnvironmentQuerySystem::distanceToTargetTest(1.0f, 0.0f, 200.0f);

    EQSContext context = makeContext();
    context.hasTarget = false;   // 没有目标

    const float raw = test.measure({10.0f, 10.0f}, context);
    // 返回极大值 -> 曲线给出 0 分，让其他测试项主导决策
    EXPECT_FLOAT_EQ(test.applyCurve(raw), 0.0f);
}

TEST(EQSTestTest, DistanceFromTargetPrefersFartherPoints) {
    EQSTest test = EnvironmentQuerySystem::distanceFromTargetTest(
        /*weight=*/1.0f, /*safe=*/0.0f, /*max=*/500.0f);

    EQSContext context = makeContext({0.0f, 0.0f}, {100.0f, 0.0f});

    const float nearScore = test.applyCurve(test.measure({110.0f, 0.0f}, context));
    const float farScore = test.applyCurve(test.measure({400.0f, 0.0f}, context));

    EXPECT_LT(nearScore, farScore);
}

TEST(EQSTestTest, ThreatAvoidancePrefersDistantPoints) {
    EQSTest test = EnvironmentQuerySystem::threatAvoidanceTest(
        /*weight=*/1.0f, /*safeRadius=*/200.0f);

    EQSContext context = makeContext();
    context.threatSources = {{0.0f, 0.0f}, {100.0f, 0.0f}};

    // 离两个威胁源都远的点应得高分
    const float safeScore = test.applyCurve(test.measure({50.0f, 300.0f}, context));
    // 贴着威胁源的点应得低分
    const float dangerScore = test.applyCurve(test.measure({10.0f, 0.0f}, context));

    EXPECT_GT(safeScore, dangerScore);
}

TEST(EQSTestTest, ThreatAvoidanceWithNoThreatsIsSafe) {
    EQSTest test = EnvironmentQuerySystem::threatAvoidanceTest(1.0f, 200.0f);

    EQSContext context = makeContext();
    context.threatSources.clear();

    // 没有威胁时该项应满分，不影响其他项的权重分配
    const float raw = test.measure({100.0f, 100.0f}, context);
    EXPECT_FLOAT_EQ(test.applyCurve(raw), 1.0f);
}

// ===========================================================================
//  查询执行
// ===========================================================================
TEST(EnvironmentQuerySystemTest, SelectsBestCandidate) {
    // 在 [0, 0] 到 [200, 0] 之间布点，目标在 [200, 0]
    // 期望选中离目标最近的点
    const auto generator = EnvironmentQuerySystem::gridGenerator(
        {100.0f, 0.0f}, 100.0f, 50.0f);

    EQSTest test = EnvironmentQuerySystem::distanceToTargetTest(1.0f, 0.0f, 300.0f);

    EQSQuery query = singleTestQuery(std::move(test), generator);

    EQSContext context = makeContext({100.0f, 0.0f}, {200.0f, 0.0f});

    EnvironmentQuerySystem eqs;
    const EQSResult result = eqs.run(query, context);

    ASSERT_TRUE(result.hasResult());
    const EQSCandidate* best = result.best();
    ASSERT_NE(best, nullptr);

    // 最优位置应接近目标点
    EXPECT_NEAR(best->position.x, 200.0f, 0.001f);
    EXPECT_NEAR(best->position.y, 0.0f, 0.001f);
    EXPECT_GT(best->score, 0.9f);
}

TEST(EnvironmentQuerySystemTest, ResultsAreSortedByScoreDescending) {
    const auto generator = EnvironmentQuerySystem::gridGenerator({0.0f, 0.0f}, 150.0f, 50.0f);
    EQSTest test = EnvironmentQuerySystem::distanceToTargetTest(1.0f, 0.0f, 400.0f);

    EQSQuery query = singleTestQuery(std::move(test), generator);
    query.maxResults = 100;   // 保留多个结果以便验证排序

    EQSContext context = makeContext();

    EnvironmentQuerySystem eqs;
    const EQSResult result = eqs.run(query, context);

    ASSERT_GE(result.candidates.size(), 2u);

    for (size_t i = 0; i + 1 < result.candidates.size(); ++i) {
        EXPECT_GE(result.candidates[i].score, result.candidates[i + 1].score)
            << "候选点未按分数降序排列（索引 " << i << "）";
    }
}

TEST(EnvironmentQuerySystemTest, MaxResultsLimitsOutput) {
    const auto generator = EnvironmentQuerySystem::gridGenerator({0.0f, 0.0f}, 200.0f, 50.0f);
    EQSTest test = EnvironmentQuerySystem::distanceFromSelfTest(1.0f, 500.0f);

    EQSQuery query = singleTestQuery(std::move(test), generator);
    query.maxResults = 3;

    EQSContext context = makeContext();

    EnvironmentQuerySystem eqs;
    const EQSResult result = eqs.run(query, context);

    EXPECT_EQ(result.candidates.size(), 3u);
    // 被截断的候选应转移到 rejected（调试时仍可见），并有明确原因
    EXPECT_FALSE(result.rejected.empty());
}

TEST(EnvironmentQuerySystemTest, EqualWeightsAveragesScores) {
    // 两个测试项，权重相同 -> 总分应为两者平均
    auto generator = EnvironmentQuerySystem::pointListGenerator({{100.0f, 0.0f}});

    EQSQuery query;
    query.name = "EqualWeights";
    query.generator = generator;
    query.minAcceptableScore = 0.0f;

    // 测试 1：距离自身，候选在 100 处，区间 [0,200] -> InverseLinear -> 0.5
    query.tests.push_back(EnvironmentQuerySystem::distanceFromSelfTest(1.0f, 200.0f));

    // 测试 2：距离目标（目标在原点），候选在 (100,0)，区间 [0,200] -> 0.5
    query.tests.push_back(EnvironmentQuerySystem::distanceToTargetTest(1.0f, 0.0f, 200.0f));

    EQSContext context = makeContext({0.0f, 0.0f}, {0.0f, 0.0f});

    EnvironmentQuerySystem eqs;
    const EQSResult result = eqs.run(query, context);

    ASSERT_TRUE(result.hasResult());
    // 【修复】同文件其他用例都先判空再解引用，这里漏了；一旦 hasResult 与
    // best 不一致就会段错误而不是给出可读的失败信息。
    ASSERT_NE(result.best(), nullptr);
    EXPECT_NEAR(result.best()->score, 0.5f, 0.01f);
}

TEST(EnvironmentQuerySystemTest, WeightedAverageRespectsWeights) {
    auto generator = EnvironmentQuerySystem::pointListGenerator({{100.0f, 0.0f}});

    EQSQuery query;
    query.name = "Weighted";
    query.generator = generator;
    query.minAcceptableScore = 0.0f;

    // 测试 1 得 0.0 分（距离 100，区间 [0,100] -> InverseLinear 给 0），权重 1
    query.tests.push_back(EnvironmentQuerySystem::distanceFromSelfTest(1.0f, 100.0f));

    // 测试 2 得 1.0 分（距离自身 100，Linear 区间 [0,100] 给 1），权重 3
    query.tests.push_back(EnvironmentQuerySystem::customTest(
        "SelfDistanceLinear",
        [](const glm::vec2& candidate, const EQSContext& context) {
            const glm::vec2 delta = candidate - context.selfPosition;
            return std::sqrt(delta.x * delta.x + delta.y * delta.y);
        },
        ScoreCurve::Linear, 0.0f, 100.0f, 3.0f));

    EQSContext context = makeContext();

    EnvironmentQuerySystem eqs;
    const EQSResult result = eqs.run(query, context);

    ASSERT_TRUE(result.hasResult());
    // 加权平均 = (0.0 * 1 + 1.0 * 3) / 4 = 0.75
    EXPECT_NEAR(result.best()->score, 0.75f, 0.01f);
}

TEST(EnvironmentQuerySystemTest, FilterRejectsCandidates) {
    auto generator = EnvironmentQuerySystem::pointListGenerator(
        {{10.0f, 10.0f}, {50.0f, 50.0f}, {90.0f, 90.0f}});

    EQSTest test = EnvironmentQuerySystem::distanceFromSelfTest(1.0f, 500.0f);
    // 只接受 x > 40 的候选
    test.filter = [](const glm::vec2& candidate, const EQSContext&) {
        return candidate.x > 40.0f;
    };
    test.rejectReason = "x 太小";

    EQSQuery query = singleTestQuery(std::move(test), generator);
    query.maxResults = 10;

    EQSContext context = makeContext();

    EnvironmentQuerySystem eqs;
    const EQSResult result = eqs.run(query, context);

    // 只剩两个通过过滤（50,50 与 90,90）
    EXPECT_EQ(result.candidates.size(), 2u);
    EXPECT_EQ(result.rejected.size(), 1u);
    EXPECT_EQ(result.rejected[0].rejectReason, "x 太小");

    for (const EQSCandidate& candidate : result.candidates) {
        EXPECT_GT(candidate.position.x, 40.0f);
    }
}

TEST(EnvironmentQuerySystemTest, MinAcceptableScoreRejectsLowScores) {
    auto generator = EnvironmentQuerySystem::pointListGenerator(
        {{10.0f, 0.0f}, {500.0f, 0.0f}});

    // 距离越近分数越高；设一个高阈值把远点剔除
    EQSTest test = EnvironmentQuerySystem::distanceFromSelfTest(1.0f, 600.0f);

    EQSQuery query = singleTestQuery(std::move(test), generator);
    query.minAcceptableScore = 0.5f;
    query.maxResults = 10;

    EQSContext context = makeContext();

    EnvironmentQuerySystem eqs;
    const EQSResult result = eqs.run(query, context);

    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_FLOAT_EQ(result.candidates[0].position.x, 10.0f);
    EXPECT_EQ(result.rejected.size(), 1u);
}

TEST(EnvironmentQuerySystemTest, EmptyGeneratorReturnsNoResults) {
    EQSQuery query;
    query.name = "NoGenerator";
    // generator 未设置

    EQSContext context = makeContext();

    EnvironmentQuerySystem eqs;
    const EQSResult result = eqs.run(query, context);

    EXPECT_FALSE(result.hasResult());
    EXPECT_EQ(result.candidates.size(), 0u);
    EXPECT_EQ(result.evaluatedCount, 0);
}

TEST(EnvironmentQuerySystemTest, EmptyTestsFailSafe) {
    auto generator = EnvironmentQuerySystem::pointListGenerator({{10.0f, 10.0f}});

    EQSQuery query;
    query.name = "NoTests";
    query.generator = generator;
    // 没有测试项

    EQSContext context = makeContext();

    EnvironmentQuerySystem eqs;
    // 不应崩溃。没有测试项时所有候选得 0 分，会被阈值过滤掉。
    const EQSResult result = eqs.run(query, context);
    EXPECT_EQ(result.candidates.size(), 0u);
}

// ===========================================================================
//  确定性
// ===========================================================================
TEST(EnvironmentQuerySystemTest, TieBreaksDeterministically) {
    // 两个与自身等距的点：分数相同，结果必须稳定
    auto generator = EnvironmentQuerySystem::pointListGenerator({{100.0f, 0.0f}, {-100.0f, 0.0f}});

    EQSTest test = EnvironmentQuerySystem::distanceFromSelfTest(1.0f, 500.0f);
    EQSQuery query = singleTestQuery(std::move(test), generator);
    query.maxResults = 10;

    EQSContext context = makeContext();

    EnvironmentQuerySystem eqs;
    const EQSResult first = eqs.run(query, context);
    const EQSResult second = eqs.run(query, context);

    ASSERT_EQ(first.candidates.size(), second.candidates.size());
    for (size_t i = 0; i < first.candidates.size(); ++i) {
        // 同分时按位置稳定排序——否则 AI 会在同分位置间来回抖动
        EXPECT_FLOAT_EQ(first.candidates[i].position.x, second.candidates[i].position.x);
        EXPECT_FLOAT_EQ(first.candidates[i].position.y, second.candidates[i].position.y);
    }
}

// ===========================================================================
//  统计与状态
// ===========================================================================
TEST(EnvironmentQuerySystemTest, TracksLastResult) {
    auto generator = EnvironmentQuerySystem::pointListGenerator({{50.0f, 50.0f}});
    EQSTest test = EnvironmentQuerySystem::distanceFromSelfTest(1.0f, 500.0f);

    EnvironmentQuerySystem eqs;
    EXPECT_FALSE(eqs.hasLastResult());

    eqs.run(singleTestQuery(std::move(test), generator), makeContext());

    EXPECT_TRUE(eqs.hasLastResult());
    EXPECT_EQ(eqs.lastResult().queryName, "SingleTest");
    EXPECT_TRUE(eqs.lastResult().hasResult());
}

TEST(EnvironmentQuerySystemTest, AccumulatesStatistics) {
    auto generator = EnvironmentQuerySystem::pointListGenerator({{50.0f, 50.0f}});
    EQSTest test = EnvironmentQuerySystem::distanceFromSelfTest(1.0f, 500.0f);
    EQSQuery query = singleTestQuery(std::move(test), generator);

    EnvironmentQuerySystem eqs;
    EXPECT_EQ(eqs.totalQueries(), 0u);

    eqs.run(query, makeContext());
    eqs.run(query, makeContext());
    eqs.run(query, makeContext());

    EXPECT_EQ(eqs.totalQueries(), 3u);
    EXPECT_GE(eqs.totalElapsedMs(), 0.0);
}

TEST(EnvironmentQuerySystemTest, RecordsElapsedTime) {
    const auto generator = EnvironmentQuerySystem::gridGenerator({0.0f, 0.0f}, 500.0f, 20.0f);
    EQSTest test = EnvironmentQuerySystem::distanceFromSelfTest(1.0f, 500.0f);

    EnvironmentQuerySystem eqs;
    const EQSResult result = eqs.run(singleTestQuery(std::move(test), generator), makeContext());

    // 至少评估了候选点，且记录了耗时
    EXPECT_GT(result.evaluatedCount, 0);
    EXPECT_GE(result.elapsedMs, 0.0);
}

TEST(EnvironmentQuerySystemTest, EvaluatedCountReflectsGeneratorOutput) {
    const auto generator = EnvironmentQuerySystem::pointListGenerator(
        {{0.0f, 0.0f}, {10.0f, 0.0f}, {20.0f, 0.0f}, {30.0f, 0.0f}});
    EQSTest test = EnvironmentQuerySystem::distanceFromSelfTest(1.0f, 500.0f);

    EnvironmentQuerySystem eqs;
    const EQSResult result = eqs.run(singleTestQuery(std::move(test), generator), makeContext());

    EXPECT_EQ(result.evaluatedCount, 4);
    EXPECT_EQ(result.candidates.size() + result.rejected.size(), 4u);
}

// ===========================================================================
//  距离工具
// ===========================================================================
TEST(EQSUtilTest, DistanceBetweenComputesEuclideanDistance) {
    EXPECT_FLOAT_EQ(distanceBetween({0.0f, 0.0f}, {3.0f, 4.0f}), 5.0f);
    EXPECT_FLOAT_EQ(distanceBetween({0.0f, 0.0f}, {0.0f, 0.0f}), 0.0f);
    EXPECT_FLOAT_EQ(distanceBetween({-5.0f, -5.0f}, {-5.0f, -5.0f}), 0.0f);
    // 对称性
    EXPECT_FLOAT_EQ(distanceBetween({1.0f, 2.0f}, {7.0f, 9.0f}),
                    distanceBetween({7.0f, 9.0f}, {1.0f, 2.0f}));
}

// ===========================================================================
//  综合场景：真实的「找掩体」查询
// ===========================================================================
TEST(EnvironmentQuerySystemTest, CoverSeekQuerySelectsTacticallySensiblePosition) {
    // 构造一个真实场景：
    //   - AI 在 (0,0)，敌人在 (400,0)
    //   - 候选点分布在 AI 周围
    //   - 威胁源在敌人位置附近（代表敌方火力）
    //  意图：选出的位置应「不太远（能打）、不太近（不送死）、远离威胁」
    const auto generator = EnvironmentQuerySystem::gridGenerator({150.0f, 0.0f}, 180.0f, 45.0f);

    EQSQuery query;
    query.name = "FindCover";
    query.generator = generator;
    query.maxResults = 1;
    query.minAcceptableScore = 0.01f;

    // 保持中等距离
    query.tests.push_back(EnvironmentQuerySystem::distanceToTargetTest(0.4f, 150.0f, 500.0f));
    // 避开威胁
    query.tests.push_back(EnvironmentQuerySystem::threatAvoidanceTest(0.4f, 300.0f));
    // 别跑太远
    query.tests.push_back(EnvironmentQuerySystem::distanceFromSelfTest(0.2f, 400.0f));

    EQSContext context = makeContext({0.0f, 0.0f}, {400.0f, 0.0f});
    context.threatSources = {{400.0f, 0.0f}};

    EnvironmentQuerySystem eqs;
    const EQSResult result = eqs.run(query, context);

    ASSERT_TRUE(result.hasResult());
    const EQSCandidate* best = result.best();
    ASSERT_NE(best, nullptr);

    // 检查「战术合理性」：不应贴到敌人脸上（那是自杀）
    const float distanceToEnemy = distanceBetween(best->position, {400.0f, 0.0f});
    EXPECT_GT(distanceToEnemy, 100.0f)
        << "选出的位置离敌人太近，AI 会送死";

    // 也不应跑到地图角落（那样打不到）
    const float distanceToSelf = distanceBetween(best->position, {0.0f, 0.0f});
    EXPECT_LT(distanceToSelf, 400.0f)
        << "选出的位置离 AI 太远，不切实际";

    // 每个测试项都应被记录（调试面板需要逐项显示）
    EXPECT_EQ(best->testScores.size(), 3u);
}
