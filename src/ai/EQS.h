// ===========================================================================
//  src/ai/EQS.h
//
//  EQS (Environment Query System) —— 环境查询系统。
//
//  解决的问题：行为树告诉 AI「去掩体」，但「哪个掩体最好」是另一个问题。
//  EQS 把这类问题形式化为：
//      生成候选点 (Generator) -> 逐项打分 (Test) -> 加权汇总 -> 取最优
//
//  相比硬编码 if-else，EQS 的优势：
//    - 每个考量因素（距离、掩护度、可见性）是独立可插拔的测试项
//    - 权重可调，行为调参从「改代码」变成「改数字」
//    - 完整保留候选点与评分，调试时能画出「AI 眼中的地图」
//
//  典型战术查询：
//    「找一个能打到我目标、但不容易被打到的位置」=
//      GridGenerator(半径 300)
//      + Test 距离目标（越近越好，权重 0.4）
//      + Test 掩护度（越高越好，权重 0.35）
//      + Test 与自身距离（越近越好，权重 0.25）
// ===========================================================================
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "glm/vec2.hpp"

namespace TacticalAI::AI {

/// 候选点。
struct EQSCandidate {
    glm::vec2 position{0.0f, 0.0f};
    float     score       = 0.0f;   ///< 归一化加权总分 [0,1]
    bool      valid       = true;   ///< 被过滤器剔除时置 false
    std::string rejectReason;       ///< 剔除原因，调试时极有价值
    std::vector<float> testScores;  ///< 各测试项的原始分数，调试时逐项显示
};

/// 查询上下文。包含评估所需的全部环境信息。
struct EQSContext {
    glm::vec2 selfPosition{0.0f, 0.0f};
    glm::vec2 selfFacing{1.0f, 0.0f};

    /// 当前主目标（敌人）。无目标时 hasTarget = false。
    bool      hasTarget         = false;
    glm::vec2 targetPosition{0.0f, 0.0f};

    /// 威胁源列表（可能在视野外的危险区域，如雷区、火力覆盖区）。
    std::vector<glm::vec2> threatSources;

    /// 掩体位置列表。与 threatSources 语义相反：
    /// 威胁要远离，掩体要靠近。两者必须分开存放，
    /// 混在一个列表里会让「规避威胁」和「寻找掩护」互相抵消。
    std::vector<glm::vec2> coverPositions;

    double gameTime = 0.0;
};

/// 分数曲线。把「原始测量值」映射到 [0,1] 的满意度。
///
/// 这是 EQS 的精髓：距离 200 是好是坏，取决于意图。
/// 「接近」用 LinearDecay，「保持距离」用 LinearRise。
enum class ScoreCurve {
    Linear,        ///< 线性：raw 从 min 到 max 时，分数从 0 到 1
    InverseLinear, ///< 反向线性：raw 从 min 到 max 时，分数从 1 到 0
    Quadratic,     ///< 平方衰减：接近目标值时分数快速上升（鼓励精确）
    Constant       ///< 恒定为 1（仅用于取原始值做后续计算）
};

/// 一个评估项。
struct EQSTest {
    std::string name;

    /// 从候选点和上下文计算原始值（如距离、掩护度）。
    std::function<float(const glm::vec2&, const EQSContext&)> measure;

    /// 原始值 -> 满意度曲线
    ScoreCurve curve = ScoreCurve::InverseLinear;

    /// 曲线作用区间
    float rangeMin = 0.0f;
    float rangeMax = 1000.0f;

    /// 本项的权重。最终得分是加权平均，权重会自动归一化。
    float weight = 1.0f;

    /// 过滤器：返回 false 则该候选点直接被剔除（不参与打分）。
    /// 用于硬约束，例如「不能站进墙里」「必须在视野内」。
    std::function<bool(const glm::vec2&, const EQSContext&)> filter;

    /// 剔除原因描述（filter 返回 false 时记录）。
    std::string rejectReason = "被过滤";

    /// 对原始值施加曲线变换。
    [[nodiscard]] float applyCurve(float raw) const;
};

/// 候选点生成器。
/// 返回一组候选点，通常是网格、环形或从掩体列表来的离散点。
using EQSGenerator = std::function<std::vector<glm::vec2>(const EQSContext&)>;

/// 环境查询。
struct EQSQuery {
    std::string name;
    EQSGenerator generator;
    std::vector<EQSTest> tests;

    /// 查询级的硬过滤器：返回 false 的候选点直接丢弃，不参与评分。
    /// 与 EQSTest::filter 的区别是「语义位置」：
    ///   candidateFilter 表达整个查询的公共约束（如「必须在地图内」），
    ///   test.filter       表达某一项测试自己的约束。
    /// 便于阅读，也避免在每个 test 里重复写同样的条件。
    std::function<bool(const glm::vec2&, const EQSContext&)> candidateFilter;

    /// 得分低于此值的候选点视为不可用。
    float minAcceptableScore = 0.05f;

    /// 返回前 N 个最优候选（调试时想看多个备选）。
    int maxResults = 1;
};

/// 查询结果。
struct EQSResult {
    std::string queryName;
    std::vector<EQSCandidate> candidates;  ///< 按分数降序，仅保留有效项
    std::vector<EQSCandidate> rejected;    ///< 被剔除的候选，供调试可视化
    int   evaluatedCount = 0;
    double elapsedMs     = 0.0;            ///< 查询耗时，性能诊断用

    [[nodiscard]] bool hasResult() const noexcept { return !candidates.empty(); }
    [[nodiscard]] const EQSCandidate* best() const noexcept {
        return candidates.empty() ? nullptr : &candidates.front();
    }
    [[nodiscard]] std::optional<glm::vec2> bestPosition() const {
        if (const EQSCandidate* candidate = best()) {
            return candidate->position;
        }
        return std::nullopt;
    }
};

/// EQS 执行器。
///
/// 单次查询是无状态的，但执行器保留最近一次结果供调试绘制，
/// 因此每个 agent 或每个查询类型持有一个实例即可。
class EnvironmentQuerySystem {
public:
    EnvironmentQuerySystem() = default;

    /// 执行查询。
    [[nodiscard]] EQSResult run(const EQSQuery& query, const EQSContext& context);

    /// 最近一次查询结果（调试绘制用）。切换查询时会被覆盖，
    /// 若需要同时显示多个查询，请为每个查询各持一个实例。
    [[nodiscard]] const EQSResult& lastResult() const noexcept { return lastResult_; }
    [[nodiscard]] bool hasLastResult() const noexcept { return hasRunOnce_; }

    /// 统计信息：累计执行的查询数与耗时。
    [[nodiscard]] uint64_t totalQueries() const noexcept { return totalQueries_; }
    [[nodiscard]] double totalElapsedMs() const noexcept { return totalElapsedMs_; }

    /// 生成候选点的耗时是否超过预算（用于决定是否需要分帧执行）。
    [[nodiscard]] static constexpr double recommendedBudgetMs() noexcept { return 2.0; }

    // ------------------------------------------------------------------------
    //  内置生成器（静态工厂，直接赋给 EQSQuery::generator）
    // ------------------------------------------------------------------------

    /// 以某点为中心的方形网格。
    /// @param spacing 格点间距，太小会产生大量候选（影响性能）
    [[nodiscard]] static EQSGenerator gridGenerator(glm::vec2 center, float radius,
                                                    float spacing, float jitter = 0.0f);

    /// 环形生成器：在半径 [inner, outer] 之间的同心圆上布点。
    /// 比网格更适合「围绕目标机动」这类查询。
    [[nodiscard]] static EQSGenerator ringGenerator(glm::vec2 center, float innerRadius,
                                                    float outerRadius, int rings,
                                                    int pointsPerRing);

    /// 从固定点列表生成（例如关卡中手工标注的所有掩体位置）。
    [[nodiscard]] static EQSGenerator pointListGenerator(std::vector<glm::vec2> points);

    /// 相对生成器：以 self 位置为原点，按偏移量列表生成。
    /// 用于「我的左前方 100 处」这类相对查询。
    [[nodiscard]] static EQSGenerator offsetGenerator(std::vector<glm::vec2> offsets);

    // ------------------------------------------------------------------------
    //  内置测试项（静态工厂）
    // ------------------------------------------------------------------------

    /// 距离目标越近分数越高（用于「接近敌人」）。
    [[nodiscard]] static EQSTest distanceToTargetTest(float weight, float optimalDistance,
                                                      float maxDistance);

    /// 距离目标越远分数越高（用于「脱离接触」）。
    [[nodiscard]] static EQSTest distanceFromTargetTest(float weight, float safeDistance,
                                                        float maxDistance);

    /// 距离自己越近分数越高（用于「选择最容易到达的位置」）。
    [[nodiscard]] static EQSTest distanceFromSelfTest(float weight, float maxDistance);

    /// 威胁规避：远离所有威胁源。
    [[nodiscard]] static EQSTest threatAvoidanceTest(float weight, float safeRadius);

    /// 掩护有效性：候选点是否真的处在「掩体—目标」的交火路径上。
    ///
    /// 分数取「最近掩体到**我→目标连线**的距离」，越近越好。
    ///
    /// 【为什么参照物是连线而不是候选点自己】
    /// 最直觉的写法是量"候选点到最近掩体的距离"，但那测的是
    /// "我离某个掩体多近"，不是"这个掩体对我这场交火有没有用"。
    /// 掩体落在AI 的侧后方时两者距离同样近、得分同样高，
    /// 而它对交火毫无帮助——AI 会为一个无用的掩体跑过去。
    ///
    /// 换成到交火连线的距离后，掩体是否位于交火路径上就成了唯一的判据。
    ///
    /// 【为什么不用"掩体与射线相交"这种更严格的判据】
    /// 因为它与本测试的典型用法自相矛盾。调用方通常还会加一条
    /// "候选点必须能通视目标"的硬约束（否则 AI 会躲进掩体后开不了枪）。
    /// 而一旦要求通视，「我→目标」的连线上按定义就没有阻挡格，
    /// 于是"掩体与该连线相交"永远为假——实测覆盖率为 0。
    ///
    /// 真实战术里两者的统一靠"探身射击"（peek）：身体在掩体后，
    /// 枪口从边缘探出。那需要"掩体附近存在能通视的点"，
    /// 而不是"掩体正好压在连线上"。本测试项负责"掩体离交火线近"，
    /// 由调用方的通视约束负责"现在能还手"，两者分工明确。
    ///
    /// @param weight  权重
    /// @param rangeMax 距离超过此值即视为无掩护（取一个明显超出战术距离的值）
    [[nodiscard]] static EQSTest coverEffectivenessTest(float weight,
                                                        float rangeMax = 200.0f);

    /// 自定义测试项构造（测量函数由调用方提供，通常是游戏层的掩护度查询）。
    [[nodiscard]] static EQSTest customTest(std::string name,
                                            std::function<float(const glm::vec2&, const EQSContext&)> measure,
                                            ScoreCurve curve, float rangeMin, float rangeMax,
                                            float weight);

private:
    EQSResult lastResult_;
    bool      hasRunOnce_     = false;
    uint64_t  totalQueries_   = 0;
    double    totalElapsedMs_ = 0.0;
};

/// 两个二维点之间的距离。
[[nodiscard]] float distanceBetween(glm::vec2 a, glm::vec2 b) noexcept;

} // namespace TacticalAI::AI
