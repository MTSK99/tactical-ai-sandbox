// ===========================================================================
//  tests/test_cover_effectiveness.cpp
//
//  掩护度检测（coverEffectivenessTest）的回归测试。
//
#include <cmath>
#include <utility>
//  【背景：这个测试项曾经是"距离代理"】
//  原实现只算「候选点到最近掩体中心的距离」：
//
//      for (cover : context.coverPositions) {
//          nearest = min(nearest, length(cover - candidate));
//      }
//      return nearest;                       // 越近越好
//
//  它测的是"我离某个掩体多近"，而不是"这个掩体对我的交火有没有用"。
//  最要命的是：**掩体落在 AI 的侧后方时，距离同样近、得分同样高**，
//  但它对交火毫无帮助。于是这个权重 0.20（占四项决策依据的四分之一）
//  的测试项，有相当一部分概率在给出无效建议——
//  AI 会为一个根本挡不到子弹的掩体专门跑过去。
//
//  新判据量的是「掩体到**我→目标连线**的距离」：
//  掩体是否处在交火路径上，成为唯一的判据。
//
//  【本文件的测试策略】
//  每个测试都用**同样的距离**、**不同的掩体朝向**构造场景。
//  这是唯一能区分两个实现的方式——
//  纯距离代理对"掩体在前方"和"掩体在侧后方"给出完全相同的分数，
//  只有以交火连线为参照的判据才能拉开差距。
// ===========================================================================
#include <gtest/gtest.h>

#include "ai/EQS.h"

using namespace TacticalAI;
using namespace TacticalAI::AI;

namespace {

/// 构造上下文：我在 self，目标在 target，掩体由调用方指定。
EQSContext makeContext(glm::vec2 self, glm::vec2 target,
                       std::vector<glm::vec2> covers) {
    EQSContext ctx;
    ctx.selfPosition  = self;
    ctx.hasTarget     = true;
    ctx.targetPosition = target;
    ctx.coverPositions = std::move(covers);
    return ctx;
}

/// 取测试项的原始测量值（未经曲线变换）。
float rawMeasure(const EQSTest& test, glm::vec2 candidate, const EQSContext& ctx) {
    return test.measure(candidate, ctx);
}

// —— 几何约定 ——
//
// 敌人(AI)      目标(玩家)
//    A -------------------> B
//
//    ↑
//    | 候选点就在这里附近
//
// 三种掩体位置：
//   1. OnLine   掩体在 AB 连线上        → 应得最高分
//   2. Behind   掩体在 A 的正后方       → 离连线最远，应得最低分
//   3. Side     掩体在 A 的正侧方       → 介于两者之间

constexpr glm::vec2 kSelf  {0.0f, 0.0f};
constexpr glm::vec2 kTarget{400.0f, 0.0f};   // 目标在正东

/// 候选点：AI 站位（在 A 附近）
constexpr glm::vec2 kCandidate{0.0f, 0.0f};

/// 掩体正好处在 A→B 连线上
constexpr glm::vec2 kCoverOnLine{200.0f, 0.0f};

/// 掩体在 AI 的正后方（远离目标方向）
constexpr glm::vec2 kCoverBehind{0.0f, -200.0f};

/// 掩体在 AI 的正侧方（垂直于交火方向）
constexpr glm::vec2 kCoverSide{0.0f, 200.0f};

} // namespace

// ---------------------------------------------------------------------------
//  核心回归：掩体是否在交火路径上，必须影响得分
// ---------------------------------------------------------------------------

TEST(CoverEffectivenessTest, ScoresHigherWhenCoverIsOnFiringLine) {
    // 掩体正好在「我→目标」的连线上 → 距离 0 → 掩护度满分
    const EQSTest test = EnvironmentQuerySystem::coverEffectivenessTest(0.2f);
    const EQSContext ctx = makeContext(kSelf, kTarget, {kCoverOnLine});

    EXPECT_NEAR(rawMeasure(test, kCandidate, ctx), 0.0f, 1e-3f)
        << "掩体在交火连线上时，原始距离应为 0";

    EXPECT_NEAR(test.applyCurve(rawMeasure(test, kCandidate, ctx)), 1.0f, 1e-3f)
        << "掩体在交火连线上时，掩护度应得满分";
}

TEST(CoverEffectivenessTest, ScoresLowerWhenCoverIsBehindSelf) {
    // 掩体在 AI 正后方：到「我→目标」连线的距离 = 200（垂直方向）
    const EQSTest test = EnvironmentQuerySystem::coverEffectivenessTest(0.2f);
    const EQSContext ctx = makeContext(kSelf, kTarget, {kCoverBehind});

    const float raw = rawMeasure(test, kCandidate, ctx);

    EXPECT_NEAR(raw, 200.0f, 1e-3f)
        << "掩体在正后方时，到交火连线的距离应为 200";

    EXPECT_LT(test.applyCurve(raw), 0.05f)
        << "掩体在正后方（对交火毫无帮助）时，掩护度应接近 0 分";
}

// ---------------------------------------------------------------------------
//  旧实现无法通过的测试：同样距离，不同朝向
// ---------------------------------------------------------------------------

TEST(CoverEffectivenessTest, DistinguishesCoverPositionNotJustDistance) {
    // 【这条是整个文件的核心断言】
    //
    // 三个掩体到候选点的距离完全相同（都是 200）：
    //   OnLine (200,0)  →  200
    //   Behind (0,-200) →  200
    //   Side   (0,200)  →  200
    //
    // 旧的距离代理实现对三者返回**完全相同的值**（200），
    // 因此它无法区分"掩体在交火路径上"与"掩体在背后"。
    // 新实现必须给出不同的值。
    const EQSTest test = EnvironmentQuerySystem::coverEffectivenessTest(0.2f);

    const float onLine = rawMeasure(test, kCandidate,
                                    makeContext(kSelf, kTarget, {kCoverOnLine}));
    const float behind = rawMeasure(test, kCandidate,
                                    makeContext(kSelf, kTarget, {kCoverBehind}));
    const float side   = rawMeasure(test, kCandidate,
                                    makeContext(kSelf, kTarget, {kCoverSide}));

    // 旧实现下三者都等于 200（到掩体中心的距离），这条会失败
    EXPECT_LT(onLine, behind)
        << "掩体在交火线上(" << onLine << ") 应优于掩体在正后方(" << behind
        << ")。两者到候选点距离相同(都是200)，旧的距离代理无法区分。";

    EXPECT_LT(onLine, side)
        << "掩体在交火线上(" << onLine << ") 应优于掩体在侧方(" << side << ")";
}

TEST(CoverEffectivenessTest, RewardsCoverBetweenSelfAndTarget) {
    // 掩体在 AI 与目标之间 → 比在侧方更接近交火路径
    const EQSTest test = EnvironmentQuerySystem::coverEffectivenessTest(0.2f);

    const glm::vec2 coverMid{200.0f, 0.0f};    // 正在交火路径上
    const glm::vec2 coverOffset{200.0f, 150.0f}; // 偏离交火路径

    const float mid    = rawMeasure(test, kCandidate,
                                    makeContext(kSelf, kTarget, {coverMid}));
    const float offset = rawMeasure(test, kCandidate,
                                    makeContext(kSelf, kTarget, {coverOffset}));

    EXPECT_LT(mid, offset)
        << "偏离交火路径的掩体(" << offset << ") 应比路径上的(" << mid << ") 差";
}

// ---------------------------------------------------------------------------
//  多掩体时取最优
// ---------------------------------------------------------------------------

TEST(CoverEffectivenessTest, PicksBestCoverAmongMultiple) {
    // 场景：两个掩体，一个在背后，一个在交火路径上。
    // EQS 应选后者 —— 这是"AI 会不会跑向无用掩体"的关键判据。
    const EQSTest test = EnvironmentQuerySystem::coverEffectivenessTest(0.2f);
    const EQSContext ctx = makeContext(
        kSelf, kTarget, {kCoverBehind, kCoverOnLine, kCoverSide});

    const float raw = rawMeasure(test, kCandidate, ctx);

    EXPECT_NEAR(raw, 0.0f, 1e-3f)
        << "存在多个掩体时，应取对交火最有用的那个（在交火路径上）";
}

// ---------------------------------------------------------------------------
//  边界情形
// ---------------------------------------------------------------------------

TEST(CoverEffectivenessTest, ReturnsWorstScoreWithoutTarget) {
    // 没有目标时无从判断交火路径，应给最差分而不是"随便找个掩体"。
    // 否则 AI 会在无目标时为了掩体乱跑。
    const EQSTest test = EnvironmentQuerySystem::coverEffectivenessTest(0.2f);

    EQSContext ctx;
    ctx.hasTarget = false;
    ctx.selfPosition = kSelf;
    ctx.coverPositions = {kCoverOnLine};

    const float raw = rawMeasure(test, kCandidate, ctx);

    EXPECT_GE(raw, test.rangeMax)
        << "无目标时原始值应不小于 rangeMax（即该项得 0 分）";
    EXPECT_FLOAT_EQ(test.applyCurve(raw), 0.0f)
        << "无目标时掩护度应为 0 分";
}

TEST(CoverEffectivenessTest, ReturnsWorstScoreWithNoCover) {
    const EQSTest test = EnvironmentQuerySystem::coverEffectivenessTest(0.2f);
    const EQSContext ctx = makeContext(kSelf, kTarget, {});

    EXPECT_FLOAT_EQ(test.applyCurve(rawMeasure(test, kCandidate, ctx)), 0.0f)
        << "场景里没有掩体时，掩护度应为 0 分";
}

TEST(CoverEffectivenessTest, HandlesCandidateAtTargetPosition) {
    // 退化情形：AI 与目标重合，线段长度为 0。
    // 此时"点到线段"退化为"点到点"，不应产生 NaN 或除零。
    const EQSTest test = EnvironmentQuerySystem::coverEffectivenessTest(0.2f);
    const EQSContext ctx = makeContext(kSelf, kSelf, {glm::vec2{100.0f, 0.0f}});

    const float raw = rawMeasure(test, kSelf, ctx);

    EXPECT_FALSE(std::isnan(raw)) << "退化线段不应产生 NaN";
    EXPECT_NEAR(raw, 100.0f, 1e-3f)
        << "退化时应退化为点—点距离";
}

TEST(CoverEffectivenessTest, CoverOnRayExtensionIsNotTreatedAsOnLine) {
    // 投影参数必须钳制到 [0,1]。
    //
    // 反例：掩体在 A 的正后方 200，但AB 连线只朝 +x 方向。
    // 未钳制时投影 t = -1，算出的"距离"会接近 0 ——
    // 于是一个在正后方的掩体被误判成"正好在交火路径上"，
    // 正是本次修复要消除的那类错误。
    const EQSTest test = EnvironmentQuerySystem::coverEffectivenessTest(0.2f);

    const glm::vec2 coverFarBehind{100.0f, -400.0f};
    const float raw = rawMeasure(test, kCandidate,
                                 makeContext(kSelf, kTarget, {coverFarBehind}));

    EXPECT_GT(raw, 100.0f)
        << "掩体在交火线的延长线上（远在背后）时，距离不应被算成接近 0。"
        << "实际 " << raw << " —— 投影参数没有钳制到 [0,1]";
}

TEST(CoverEffectivenessTest, HasExpectedConfiguration) {
    // 契约固定：InverseLinear（越近越好）、默认 rangeMax=200。
    const EQSTest test = EnvironmentQuerySystem::coverEffectivenessTest(0.35f);

    EXPECT_EQ(test.name, "CoverEffectiveness");
    EXPECT_FLOAT_EQ(test.weight, 0.35f);
    EXPECT_EQ(test.curve, ScoreCurve::InverseLinear);
    EXPECT_FLOAT_EQ(test.rangeMin, 0.0f);
    EXPECT_FLOAT_EQ(test.rangeMax, 200.0f);
}

// ---------------------------------------------------------------------------
//  集成：在真实查询里它必须真的影响选点结果
// ---------------------------------------------------------------------------

TEST(CoverEffectivenessTest, InfluencesRealQuerySelection) {
    // 【为什么还需要这条】
    // 前面的测试都是直接调 measure，验证"分数算得对"。
    // 这条验证"分数确实改变了选点"——即 EQS 的加权汇总真的用上了它。
    //
    // 场景：两个候选点距离目标相同，其中一个旁边有个掩体在交火路径上。
    // 带掩护度的那份查询应当选中前者。
    EQSQuery query;
    query.name = "FindCover";
    query.minAcceptableScore = 0.0f;
    query.maxResults = 1;
    query.generator = [](const EQSContext&) {
        return std::vector<glm::vec2>{{100.0f, 0.0f}, {100.0f, 300.0f}};
    };

    // 只保留掩护度这一项，去掉其他因素的干扰
    query.tests.push_back(EnvironmentQuerySystem::coverEffectivenessTest(1.0f));

    EQSContext ctx;
    ctx.selfPosition  = kSelf;
    ctx.hasTarget     = true;
    ctx.targetPosition = kTarget;

    // 掩体放在 (100, 0) 附近 → 它在 kSelf→kTarget 的连线上，
    // 于是靠近它的候选点应该胜出
    ctx.coverPositions = {glm::vec2{100.0f, 0.0f}};

    EnvironmentQuerySystem exec;
    const EQSResult result = exec.run(query, ctx);

    ASSERT_TRUE(result.hasResult()) << "查询应至少返回��个候选";
    const auto best = result.bestPosition();
    ASSERT_TRUE(best.has_value());

    EXPECT_NEAR(best->x, 100.0f, 1e-3f);
    EXPECT_NEAR(best->y, 0.0f, 1e-3f)
        << "应选中靠近交火线上掩体的候选点 (100,0)，而不是远离它的 (100,300)";
}