// ===========================================================================
//  src/ai/EQS.cpp
// ===========================================================================
#include "ai/EQS.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <random>

namespace TacticalAI::AI {

constexpr float kTwoPi = 6.28318530717958647692f;

float distanceBetween(glm::vec2 a, glm::vec2 b) noexcept {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

// ---------------------------------------------------------------------------
//  EQSTest::applyCurve
// ---------------------------------------------------------------------------
float EQSTest::applyCurve(float raw) const {
    // 【NaN / inf 防护】
    // 自定义 measure 很容易算出 NaN（两个零向量求夹角、0/0、sqrt(负数)…），
    // 而 std::clamp 对 NaN 会**原样返回 NaN**：于是 candidate.score 变成 NaN，
    // 之后 `score < minAcceptableScore` 恒为 false（NaN 参与的比较都是 false），
    // 这个候选不但不会被剔除，还会带着 NaN 进入 stable_sort ——
    // 比较器失去严格弱序，属于未定义行为。
    // 所以统一在打分的入口把非有限值压成 0（= 最差），让它被正常淘汰。
    if (!std::isfinite(raw)) {
        raw = 0.0f;
    }

    // 先归一化到 [0,1]。区间非法时直接返回 0，避免除零。
    const float span = rangeMax - rangeMin;
    if (!std::isfinite(span) || span <= 1e-6f) {
        return (curve == ScoreCurve::Constant) ? 1.0f : 0.0f;
    }

    float normalized = std::clamp((raw - rangeMin) / span, 0.0f, 1.0f);

    switch (curve) {
        case ScoreCurve::Linear:
            // 原始值越大越满意
            return normalized;

        case ScoreCurve::InverseLinear:
            // 原始值越小越满意（距离类测试的主力曲线）
            return 1.0f - normalized;

        case ScoreCurve::Quadratic: {
            // 平滑衰减：接近最优值（rangeMin）时上升很快，远离时趋于平坦。
            // 用于「越近越好但边际收益递减」的场景（如距离奖励）。
            const float inverse = 1.0f - normalized;
            return inverse * inverse;
        }

        case ScoreCurve::Constant:
            return 1.0f;
    }
    return 0.0f;
}

// ---------------------------------------------------------------------------
//  内置生成器
// ---------------------------------------------------------------------------
EQSGenerator EnvironmentQuerySystem::gridGenerator(glm::vec2 center, float radius,
                                                   float spacing, float jitter) {
    // 参数在构造时捕获并校验一次，避免每个候选点重复判断
    const float safeSpacing = std::max(spacing, 1.0f);

    // 【候选数量上限】
    // 网格点数是 (2·radius/spacing + 1)²，完全由调用方传入的参数决定：
    // spacing=1、radius=2000 会一次性申请约 1600 万个点（reserve 就是 128MB+），
    // 并且每个点都要跑一遍全部测试项与筛选器——足以让一帧直接卡死。
    // 这里把半径钳到"最多 65×65 ≈ 4.2k 个候选"：少几个采样点可以接受，
    // 一次查询把帧率打穿不行。
    constexpr int   kMaxSteps     = 65;
    constexpr float kMaxHalfSteps = static_cast<float>((kMaxSteps - 1) / 2);
    const float maxRadius  = safeSpacing * kMaxHalfSteps;
    const float safeRadius = std::max(std::min(radius, maxRadius), safeSpacing);

    return [center, safeRadius, safeSpacing, jitter](const EQSContext& context) {
        std::vector<glm::vec2> points;

        // 预估容量，避免反复扩容（网格点数可能上千）
        const int steps = static_cast<int>(safeRadius / safeSpacing) * 2 + 1;
        points.reserve(static_cast<size_t>(steps) * static_cast<size_t>(steps));

        // 随机抖动：让候选点不严格对齐网格，行为看起来更自然。
        //
        // 【注释更正】种子掺入了 gameTime，所以它**不是**"固定种子"：
        // 同一位置在不同时刻会得到不同的抖动点集。这是刻意的（AI 每次选点
        // 不至于永远一模一样），但同一步内可复现。
        // 取模 + clamp 是为了避免 double→uint32 在超大/负时间下的 UB。
        const double seedTime = std::max(std::fmod(context.gameTime, 3600.0), 0.0);
        std::mt19937 rng(0xC0FFEEu ^ static_cast<uint32_t>(seedTime * 1000.0));
        std::uniform_real_distribution<float> dist(-jitter, jitter);

        for (float x = -safeRadius; x <= safeRadius + 0.001f; x += safeSpacing) {
            for (float y = -safeRadius; y <= safeRadius + 0.001f; y += safeSpacing) {
                glm::vec2 point{center.x + x, center.y + y};
                if (jitter > 0.0f) {
                    point.x += dist(rng);
                    point.y += dist(rng);
                }
                points.push_back(point);
            }
        }
        return points;
    };
}

EQSGenerator EnvironmentQuerySystem::ringGenerator(glm::vec2 center, float innerRadius,
                                                   float outerRadius, int rings,
                                                   int pointsPerRing) {
    const int safeRings = std::max(rings, 1);

    // 同样的候选数量上限（见 gridGenerator）：环形点数 = 环数 × 每环点数，
    // 也要有上界，否则退回"一次查询卡死一帧"。
    constexpr int kMaxRingCandidates = 4096;
    const int maxPoints = std::max(kMaxRingCandidates / safeRings, 3);
    const int safePoints = std::clamp(pointsPerRing, 3, maxPoints);
    const float safeInner = std::max(innerRadius, 0.0f);
    const float safeOuter = std::max(outerRadius, safeInner + 1.0f);

    // 环形点可以与世界状态无关，但保留 center 参数以支持「围绕动态目标」的查询：
    // 调用方通常会重新生成 generator，或在此 lambda 里读取 context。
    return [center, safeInner, safeOuter, safeRings, safePoints](const EQSContext& context) {
        (void)context;
        std::vector<glm::vec2> points;
        points.reserve(static_cast<size_t>(safeRings) * static_cast<size_t>(safePoints));

        for (int ring = 0; ring < safeRings; ++ring) {
            // t 从 0 到 1，线性插值出该环的半径
            const float t = (safeRings == 1)
                                ? 0.0f
                                : static_cast<float>(ring) / static_cast<float>(safeRings - 1);
            const float radius = safeInner + (safeOuter - safeInner) * t;

            // 每环的起始角错开，避免所有环的点排成径向直线
            const float angleOffset = (static_cast<float>(ring) / static_cast<float>(safeRings))
                                      * (kTwoPi / static_cast<float>(safePoints));

            for (int i = 0; i < safePoints; ++i) {
                const float angle = angleOffset + kTwoPi * static_cast<float>(i)
                                                 / static_cast<float>(safePoints);
                points.push_back({center.x + std::cos(angle) * radius,
                                  center.y + std::sin(angle) * radius});
            }
        }
        return points;
    };
}

EQSGenerator EnvironmentQuerySystem::pointListGenerator(std::vector<glm::vec2> points) {
    // 按值捕获（移动），生成器自持一份，生命周期不依赖调用方
    return [points = std::move(points)](const EQSContext& context) {
        (void)context;
        return points;
    };
}

EQSGenerator EnvironmentQuerySystem::offsetGenerator(std::vector<glm::vec2> offsets) {
    return [offsets = std::move(offsets)](const EQSContext& context) {
        std::vector<glm::vec2> points;
        points.reserve(offsets.size());
        for (const glm::vec2& offset : offsets) {
            points.push_back(context.selfPosition + offset);
        }
        return points;
    };
}

// ---------------------------------------------------------------------------
//  内置测试项
// ---------------------------------------------------------------------------
EQSTest EnvironmentQuerySystem::distanceToTargetTest(float weight, float optimalDistance,
                                                     float maxDistance) {
    EQSTest test;
    test.name = "DistanceToTarget";
    test.weight = weight;
    test.rangeMin = optimalDistance;
    test.rangeMax = maxDistance;
    // 距离小于 optimalDistance 时也视为满分（clamp 已保证），
    // 所以用 InverseLinear 即可表达「越近越好」。
    test.curve = ScoreCurve::InverseLinear;
    test.measure = [](const glm::vec2& candidate, const EQSContext& context) -> float {
        if (!context.hasTarget) {
            // 没有目标时该项无意义，返回一个大值使其得分为 0，
            // 从而让其他测试项主导决策。
            return 1e6f;
        }
        return distanceBetween(candidate, context.targetPosition);
    };
    return test;
}

EQSTest EnvironmentQuerySystem::distanceFromTargetTest(float weight, float safeDistance,
                                                       float maxDistance) {
    EQSTest test;
    test.name = "DistanceFromTarget";
    test.weight = weight;
    test.rangeMin = safeDistance;
    test.rangeMax = maxDistance;
    // 越远越好
    test.curve = ScoreCurve::Linear;
    test.measure = [maxDistance](const glm::vec2& candidate, const EQSContext& context) -> float {
        if (!context.hasTarget) {
            // 【修复】这里原来返回 0.0f。但本测试的 rangeMin = safeDistance，
            // 曲线是 Linear（原始值越大越满意），于是 0 会被归一化成
            // clamp((0 - safe) / span) = 0 —— 也就是**最低分**，与注释
            // 「无目标时给满分」完全相反。
            //
            // 后果：没有目标时该项会拉低每一个候选点的加权总分，
            // 可能把全部候选压到 minAcceptableScore 之下，导致整次查询
            // 空手而归（AI 表现为「没有目标时反而不知所措」）。
            // 返回 rangeMax 才能归一化到 1.0（满分）。
            return maxDistance;
        }
        return distanceBetween(candidate, context.targetPosition);
    };
    return test;
}

EQSTest EnvironmentQuerySystem::distanceFromSelfTest(float weight, float maxDistance) {
    EQSTest test;
    test.name = "DistanceFromSelf";
    test.weight = weight;
    test.rangeMin = 0.0f;
    test.rangeMax = maxDistance;
    test.curve = ScoreCurve::InverseLinear;
    test.measure = [](const glm::vec2& candidate, const EQSContext& context) -> float {
        return distanceBetween(candidate, context.selfPosition);
    };
    return test;
}

EQSTest EnvironmentQuerySystem::threatAvoidanceTest(float weight, float safeRadius) {
    EQSTest test;
    test.name = "ThreatAvoidance";
    test.weight = weight;
    test.rangeMin = 0.0f;
    test.rangeMax = safeRadius;
    test.curve = ScoreCurve::Linear;
    test.measure = [](const glm::vec2& candidate, const EQSContext& context) -> float {
        // 分数取「到最近威胁源的距离」：越远越安全。
        // 威胁源为空时给一个超过 safeRadius 的值，使该项满分且不影响其他项。
        if (context.threatSources.empty()) {
            return 1e6f;
        }
        float nearest = 1e6f;
        for (const glm::vec2& threat : context.threatSources) {
            nearest = std::min(nearest, distanceBetween(candidate, threat));
        }
        return nearest;
    };
    return test;
}

EQSTest EnvironmentQuerySystem::coverEffectivenessTest(float weight, float rangeMax) {
    EQSTest test;
    test.name = "CoverEffectiveness";
    test.weight = weight;
    test.rangeMin = 0.0f;
    test.rangeMax = rangeMax;
    test.curve = ScoreCurve::InverseLinear;   // 离交火线越近越好
    test.measure = [rangeMax](const glm::vec2& candidate,
                              const EQSContext& context) -> float {
        // 无目标时无从判断交火路径，直接给最差分。
        // 这比"随便找个掩体"诚实——没有目标时本就不该为掩体乱跑。
        if (!context.hasTarget || context.coverPositions.empty()) {
            return rangeMax;
        }

        const glm::vec2 target = context.targetPosition;

        // 点到线段的最短距离。
        // 退化情形（自己就在目标位置）退化为点—点距离。
        const glm::vec2 ab = target - candidate;
        const float lengthSquared = ab.x * ab.x + ab.y * ab.y;

        float nearest = rangeMax;
        for (const glm::vec2& cover : context.coverPositions) {
            float distance = 0.0f;
            if (lengthSquared < 1e-6f) {
                distance = distanceBetween(cover, candidate);
            } else {
                const glm::vec2 ap = cover - candidate;
                // 投影参数钳制到 [0,1]：落在射线延长线上的掩体
                // 不该算"在交火路径上"，钳制后它会退化为到端点的距离。
                const float t = std::clamp(
                    (ap.x * ab.x + ap.y * ab.y) / lengthSquared, 0.0f, 1.0f);
                distance = distanceBetween(cover, candidate + ab * t);
            }
            nearest = std::min(nearest, distance);
        }
        return nearest;
    };
    return test;
}

EQSTest EnvironmentQuerySystem::customTest(
    std::string name,
    std::function<float(const glm::vec2&, const EQSContext&)> measure,
    ScoreCurve curve, float rangeMin, float rangeMax, float weight) {
    EQSTest test;
    test.name = std::move(name);
    test.measure = std::move(measure);
    test.curve = curve;
    test.rangeMin = rangeMin;
    test.rangeMax = rangeMax;
    test.weight = weight;
    return test;
}

// ---------------------------------------------------------------------------
//  执行
// ---------------------------------------------------------------------------
EQSResult EnvironmentQuerySystem::run(const EQSQuery& query, const EQSContext& context) {
    const auto startTime = std::chrono::steady_clock::now();

    EQSResult result;
    result.queryName = query.name;

    if (!query.generator) {
        // 无生成器：配置错误，返回空结果而不是崩溃
        lastResult_ = result;
        hasRunOnce_ = true;
        return result;
    }

    std::vector<glm::vec2> rawPoints = query.generator(context);
    result.evaluatedCount = static_cast<int>(rawPoints.size());

    // 权重总和用于归一化。全零权重视为等权，否则得分会全为 0。
    float totalWeight = 0.0f;
    for (const EQSTest& test : query.tests) {
        totalWeight += test.weight;
    }
    const bool useEqualWeights = (totalWeight <= 1e-6f);

    for (const glm::vec2& point : rawPoints) {
        EQSCandidate candidate;
        candidate.position = point;
        candidate.testScores.reserve(query.tests.size());

        // 查询级硬过滤：不满足公共约束的候选点直接丢弃，连分数都不算。
        if (query.candidateFilter && !query.candidateFilter(point, context)) {
            candidate.valid = false;
            candidate.rejectReason = "未通过查询级过滤";
            result.rejected.push_back(std::move(candidate));
            continue;
        }

        bool rejected = false;
        for (const EQSTest& test : query.tests) {
            // 过滤器优先：硬约束不满足则无需计算分数
            if (test.filter && !test.filter(point, context)) {
                candidate.valid = false;
                candidate.rejectReason = test.rejectReason;
                rejected = true;
                break;
            }

            const float raw = test.measure ? test.measure(point, context) : 0.0f;
            const float score = test.applyCurve(raw);
            candidate.testScores.push_back(score);
        }

        if (rejected) {
            result.rejected.push_back(std::move(candidate));
            continue;
        }

        // 加权平均得到总分
        if (query.tests.empty()) {
            candidate.score = 0.0f;
        } else if (useEqualWeights) {
            const float sum = std::accumulate(candidate.testScores.begin(),
                                              candidate.testScores.end(), 0.0f);
            candidate.score = sum / static_cast<float>(candidate.testScores.size());
        } else {
            float weighted = 0.0f;
            for (size_t i = 0; i < query.tests.size(); ++i) {
                weighted += candidate.testScores[i] * query.tests[i].weight;
            }
            candidate.score = weighted / totalWeight;
        }

        if (candidate.score < query.minAcceptableScore) {
            candidate.valid = false;
            candidate.rejectReason = "分数低于阈值";
            result.rejected.push_back(std::move(candidate));
            continue;
        }

        result.candidates.push_back(std::move(candidate));
    }

    // 按分数降序。同分时按位置稳定排序，保证结果可复现
    // （否则视觉上会看到 AI 在同分位置间来回抖动）。
    std::stable_sort(result.candidates.begin(), result.candidates.end(),
                     [](const EQSCandidate& a, const EQSCandidate& b) {
                         if (std::fabs(a.score - b.score) > 1e-6f) {
                             return a.score > b.score;
                         }
                         if (std::fabs(a.position.x - b.position.x) > 1e-6f) {
                             return a.position.x < b.position.x;
                         }
                         return a.position.y < b.position.y;
                     });

    if (query.maxResults > 0 &&
        result.candidates.size() > static_cast<size_t>(query.maxResults)) {
        // 被截断的候选转移到 rejected 里，调试时仍可见（区别于「被剔除」）
        for (size_t i = static_cast<size_t>(query.maxResults); i < result.candidates.size(); ++i) {
            EQSCandidate trimmed = result.candidates[i];
            trimmed.valid = false;
            trimmed.rejectReason = "排名超出 maxResults";
            result.rejected.push_back(std::move(trimmed));
        }
        result.candidates.resize(static_cast<size_t>(query.maxResults));
    }

    const auto endTime = std::chrono::steady_clock::now();
    result.elapsedMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

    ++totalQueries_;
    totalElapsedMs_ += result.elapsedMs;

    lastResult_ = result;
    hasRunOnce_ = true;
    return result;
}

} // namespace TacticalAI::AI
