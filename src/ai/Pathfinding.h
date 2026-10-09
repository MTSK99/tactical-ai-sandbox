// ===========================================================================
//  src/ai/Pathfinding.h
//
//  寻路系统。A* 网格搜索 + 路径平滑 + 导航网格抽象。
//
//  设计要点：
//   - 网格是「可通行性 + 代价」的二维位图，由游戏层从地形生成。
//   - A* 用二叉堆开放列表 + 关闭列表位图。相比朴素线性扫描，
//     在 128x128 网格上快一个数量级，这对「每个敌人每 0.5 秒重新寻路」很关键。
//   - 结果经过视线拉直（string pulling），避免 AI 走出明显的锯齿折线。
//   - 提供路径缓存与重寻路节流，因为寻路是本项目最贵的每帧操作之一。
// ===========================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "glm/vec2.hpp"

namespace TacticalAI::AI {

/// 网格坐标（整数格）。
struct GridCoord {
    int x = 0;
    int y = 0;

    friend bool operator==(const GridCoord& a, const GridCoord& b) noexcept {
        return a.x == b.x && a.y == b.y;
    }
    friend bool operator!=(const GridCoord& a, const GridCoord& b) noexcept {
        return !(a == b);
    }
};

/// 导航网格。把世界划分为等距方格，每格记录可通行性与额外代价。
class NavGrid {
public:
    /// 格子类型。用 uint8_t 而非 enum class 存储，便于直接做位运算与批量填充。
    enum CellFlags : uint8_t {
        kWalkable   = 0,        ///< 可通行
        kBlocked    = 1u << 0,  ///< 不可通行（墙/障碍）
        kHazard     = 1u << 1,  ///< 危险但可通行（穿过会受伤）
        kCover      = 1u << 2,  ///< 有掩护（EQS 的掩护度测试会用到）
    };

    NavGrid() = default;

    /// 初始化网格。
    /// @param origin   网格左下角的世界坐标
    /// @param cellSize 单元格边长（世界单位）
    /// @param columns  列数（x 方向）
    /// @param rows     行数（y 方向）
    void initialize(glm::vec2 origin, float cellSize, int columns, int rows);

    void clear();

    [[nodiscard]] bool  isInitialized() const noexcept { return columns_ > 0 && rows_ > 0; }
    [[nodiscard]] int   columns() const noexcept { return columns_; }
    [[nodiscard]] int   rows() const noexcept { return rows_; }
    [[nodiscard]] float cellSize() const noexcept { return cellSize_; }
    [[nodiscard]] glm::vec2 origin() const noexcept { return origin_; }

    /// 网格覆盖的世界范围尺寸。
    [[nodiscard]] glm::vec2 worldSize() const noexcept {
        return {static_cast<float>(columns_) * cellSize_,
                static_cast<float>(rows_) * cellSize_};
    }

    /// 世界坐标是否在网格覆盖范围内。
    [[nodiscard]] bool contains(glm::vec2 world) const noexcept;

    /// 半径感知的「能不能站在这里」：以 @p world 为圆心、@p radius 为半径的
    /// 圆是否完全落在可通行区域内（不与任何阻挡格相交，也不越出网格）。
    ///
    /// 【为什么必须由 NavGrid 提供这个判定】
    /// 各处的"这点能不能站"曾经写成 `isWalkable(worldToGrid(p))`，它有两个
    /// 致命缺陷：
    ///   1. 只看格心、不考虑实体半径 —— 贴着墙一格内也算"能站"，
    ///      实体走过去会与墙重叠，被碰撞解算推来推去；
    ///   2. `worldToGrid` 会把越界坐标**钳制**到边界格，于是**地图外的点
    ///      会被误判成可通行**。AI 的目标点一旦落到地图外，寻路给出的
    ///      终点是够不到的边界格心，而移动动作会一直朝那个方向推进，
    ///      被边界约束顶住 → 表现为"敌人贴着地图边缘永远不动"。
    ///
    /// 把判定收敛到这里，寻路、生成点、AI 目标选择、EQS 候选筛选就都用
    /// 同一把尺子。网格未初始化时返回 true（不限制，兼容测试场景）。
    [[nodiscard]] bool canStand(glm::vec2 world, float radius) const noexcept;

    // -- 坐标转换 ------------------------------------------------------------

    [[nodiscard]] GridCoord worldToGrid(glm::vec2 world) const noexcept;
    /// 返回格子中心点的世界坐标（比返回格子角点更适合作为移动目标）。
    [[nodiscard]] glm::vec2 gridToWorld(GridCoord coord) const noexcept;

    // -- 格子属性 ------------------------------------------------------------

    [[nodiscard]] bool inBounds(GridCoord coord) const noexcept;
    [[nodiscard]] uint8_t flagsAt(GridCoord coord) const noexcept;
    void setFlags(GridCoord coord, uint8_t flags);

    [[nodiscard]] bool isWalkable(GridCoord coord) const noexcept;
    void setBlocked(GridCoord coord, bool blocked);

    [[nodiscard]] bool isHazard(GridCoord coord) const noexcept;
    void setHazard(GridCoord coord, bool hazard);

    /// 格子额外代价。1.0 为基准；危险格、泥地等可以提高。
    /// 用代价而非硬性禁止，让 AI 表现出「能走但尽量避免」的智能。
    [[nodiscard]] float costAt(GridCoord coord) const noexcept;
    void setCost(GridCoord coord, float cost);

    /// 到最近障碍物的距离近似值（0 表示贴着墙）。
    /// 由 computeClearanceField() 预计算，供「别贴墙走」类 EQS 测试使用。
    [[nodiscard]] float clearanceAt(GridCoord coord) const noexcept;

    // -- 批量操作 ------------------------------------------------------------

    /// 用世界坐标的矩形设置障碍（矩形外的格子不受影响）。
    void fillRect(glm::vec2 min, glm::vec2 max, bool blocked);

    /// 预计算「间距场」：每个格子到最近阻挡格的距离（以格为单位，上限 5）。
    /// 这是一次性开销，之后 clearanceAt 是 O(1)。
    void computeClearanceField();

    // -- 调试 ----------------------------------------------------------------

    [[nodiscard]] size_t walkableCellCount() const noexcept;
    [[nodiscard]] std::string debugSummary() const;

private:
    [[nodiscard]] size_t indexOf(GridCoord coord) const noexcept;

    glm::vec2 origin_{0.0f, 0.0f};
    float     cellSize_ = 32.0f;
    int       columns_  = 0;
    int       rows_     = 0;

    // 用扁平数组而非 vector<vector<>>：缓存友好，且避免每行一次堆分配
    std::vector<uint8_t> flags_;
    std::vector<float>   costs_;
    std::vector<uint8_t> clearance_;
};

/// 寻路请求。
struct PathRequest {
    glm::vec2 start{0.0f, 0.0f};
    glm::vec2 goal{0.0f, 0.0f};

    /// 目标是否允许「尽量接近」而非必须到达。
    /// 玩家点了敌人背后的位置时，若目标点不可站立，允许停在最近可达处。
    bool allowPartialPath = true;

    /// 是否允许对角移动。允许则路径更短，但需要额外的穿墙检查。
    bool allowDiagonal = true;

    /// 最大搜索节点数，防止在不可达目标上耗尽预算。
    int maxExpandedNodes = 20000;
};

/// 寻路结果。
struct PathResult {
    std::vector<glm::vec2> waypoints;   ///< 世界坐标路径点（含起点与终点）
    bool   success        = false;
    bool   partial        = false;      ///< 未能到达目标，返回了最近的接近路径
    int    expandedNodes  = 0;          ///< 搜索了多大规模，性能诊断用
    float  totalCost      = 0.0f;       ///< 路径代价（含地形额外代价）
    double elapsedMs      = 0.0;

    [[nodiscard]] bool empty() const noexcept { return waypoints.empty(); }
    [[nodiscard]] size_t length() const noexcept { return waypoints.size(); }

    /// 路径总长度（世界单位），用于估算到达时间。
    [[nodiscard]] float geometricLength() const noexcept;
};

/// 寻路配置。
struct PathfindingConfig {
    /// 允许对角移动。
    bool allowDiagonal = true;

    /// 对角移动的代价倍率。sqrt(2) 是几何正确的值；
    /// 取略小于 sqrt(2) 的值会让路径更倾向走斜线（看起来更自然）。
    float diagonalCost = 1.41421356f;

    /// 启发函数权重。1.0 为标准 A*（最优但慢），
    /// >1 为加权 A*（更快但可能非最优）。战术游戏中 1.2 左右很常见。
    float heuristicWeight = 1.15f;

    /// 是否对结果做视线拉直（string pulling）。
    bool  smoothPath = true;

    /// 平滑时检查视线所用的采样步长（世界单位）。
    float smoothingStep = 4.0f;
};

/// A* 寻路器。
///
/// 有状态（持有关闭列表与开放列表的复用缓冲），因此不要跨线程共享；
/// 多线程寻路时每个线程各持一个实例。
class Pathfinder {
public:
    Pathfinder() = default;

    explicit Pathfinder(PathfindingConfig config) : config_(config) {}

    // -- 主接口 --------------------------------------------------------------

    /// 在网格上寻路。
    [[nodiscard]] PathResult findPath(const NavGrid& grid, const PathRequest& request);

    /// 使用内部配置的便捷重载。
    [[nodiscard]] PathResult findPath(const NavGrid& grid, glm::vec2 start, glm::vec2 goal);

    // -- 配置 ----------------------------------------------------------------

    [[nodiscard]] PathfindingConfig& config() noexcept { return config_; }
    [[nodiscard]] const PathfindingConfig& config() const noexcept { return config_; }

    // -- 统计 ----------------------------------------------------------------

    [[nodiscard]] uint64_t totalSearches() const noexcept { return totalSearches_; }
    [[nodiscard]] uint64_t totalExpandedNodes() const noexcept { return totalExpandedNodes_; }
    [[nodiscard]] double   totalElapsedMs() const noexcept { return totalElapsedMs_; }
    [[nodiscard]] float    averageExpandedNodes() const noexcept;
    void resetStats() noexcept;

    // -- 静态工具 ------------------------------------------------------------

    /// 视线检测：a 到 b 之间是否没有阻挡格（Bresenham 直线）。
    /// 寻路平滑与感知遮挡检测共用这个函数。
    [[nodiscard]] static bool hasLineOfSight(const NavGrid& grid, glm::vec2 a, glm::vec2 b,
                                             float stepSize = 4.0f);

    /// 对折线做视线拉直：删除所有「可以直达」的中间点。
    /// 输入输出都是世界坐标。
    [[nodiscard]] static std::vector<glm::vec2> smooth(const NavGrid& grid,
                                                       const std::vector<glm::vec2>& points,
                                                       float stepSize = 4.0f);

    /// 沿路径行走时，取「前方 ahead 距离」处的点。
    /// 移动系统用它做前瞻跟随，避免走一步停一步的机械感。
    [[nodiscard]] static std::optional<glm::vec2> pointAhead(const std::vector<glm::vec2>& path,
                                                             size_t& cursor, glm::vec2 current,
                                                             float ahead);

private:
    /// 真正的搜索实现。两个公开重载都转交给它，并在此处让
    /// PathRequest 的 allowPartialPath / allowDiagonal / maxExpandedNodes 生效。
    [[nodiscard]] PathResult findPathImpl(const NavGrid& grid, glm::vec2 start, glm::vec2 goal,
                                          bool allowPartialPath, bool allowDiagonal,
                                          int maxExpandedNodes);

    PathfindingConfig config_;

    // 复用缓冲：A* 每帧可能被调用几十次，反复分配 vector 会成为瓶颈。
    // 保留上次的容量即可消除绝大部分分配。
    //
    // 注意：这些缓冲在每次搜索前都会被 ensureBuffers 完整重置。
    // 曾尝试用「代数戳」只标记而不清零来省时间，但实测中一旦漏清
    // cameFrom_/fScore_ 就会破坏搜索的确定性（同输入两次路径不同），
    // 因此改为无条件清零，用微小的时间代价换取绝对正确的语义。
    std::vector<float>    gScore_;
    std::vector<float>    fScore_;
    std::vector<int32_t>  cameFrom_;
    std::vector<uint8_t>  closed_;
    std::vector<int32_t>  openHeap_;      ///< 二叉堆，存格子线性索引
    std::vector<size_t>   heapPositions_; ///< 每个格子在堆中的位置（-1 表示不在堆中）

    uint64_t totalSearches_     = 0;
    uint64_t totalExpandedNodes_ = 0;
    double   totalElapsedMs_    = 0.0;

    /// 确保内部缓冲尺寸与网格匹配。
    void ensureBuffers(size_t cellCount);

    /// 堆操作。
    void heapPush(int32_t node);
    [[nodiscard]] int32_t heapPop();
    void heapUpdate(int32_t node);
    void heapSiftUp(size_t index);
    void heapSiftDown(size_t index);
    [[nodiscard]] float heapKey(int32_t node) const noexcept;
};

} // namespace TacticalAI::AI
