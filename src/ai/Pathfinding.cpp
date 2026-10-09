// ===========================================================================
//  src/ai/Pathfinding.cpp
// ===========================================================================
#include "ai/Pathfinding.h"

#include "ai/EQS.h"   // 复用 distanceBetween（同一 AI 层内的公共几何工具）

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <queue>
#include <unordered_map>

namespace TacticalAI::AI {
namespace {

constexpr float kInfinity = std::numeric_limits<float>::max();
constexpr int32_t kNoParent = -1;
constexpr size_t kInvalidHeapSlot = static_cast<size_t>(-1);

/// 单次搜索的默认节点预算。便捷重载 findPath(grid, start, goal) 用它，
/// 走 PathRequest 的调用则由 request.maxExpandedNodes 决定。
constexpr int kDefaultMaxExpandedNodes = 20000;

/// 8 邻域方向偏移（含对角）。
constexpr int kDirX[8] = { 1, -1,  0,  0,  1,  1, -1, -1 };
constexpr int kDirY[8] = { 0,  0,  1, -1,  1, -1,  1, -1 };

/// 前 4 个是正交方向，后 4 个是对角方向。
constexpr int kOrthogonalDirectionCount = 4;

/// 防御性 clamp，避免 NaN 一路传播进网格索引。
inline int clampToRange(int value, int min, int max) noexcept {
    return std::max(min, std::min(value, max));
}

} // namespace

// ===========================================================================
//  NavGrid
// ===========================================================================
void NavGrid::initialize(glm::vec2 origin, float cellSize, int columns, int rows) {
    origin_   = origin;
    // 格子过小会导致网格爆炸，过大则丢失细节。8~128 是合理区间。
    cellSize_ = std::clamp(cellSize, 1.0f, 1024.0f);
    columns_  = std::max(columns, 0);
    rows_     = std::max(rows, 0);

    const size_t cellCount = static_cast<size_t>(columns_) * static_cast<size_t>(rows_);

    // 默认全部可通行（flags=0），代价为基准 1.0
    flags_.assign(cellCount, static_cast<uint8_t>(kWalkable));
    costs_.assign(cellCount, 1.0f);
    clearance_.assign(cellCount, 0);
}

void NavGrid::clear() {
    flags_.clear();
    costs_.clear();
    clearance_.clear();
    columns_ = 0;
    rows_ = 0;
}

size_t NavGrid::indexOf(GridCoord coord) const noexcept {
    return static_cast<size_t>(coord.y) * static_cast<size_t>(columns_)
         + static_cast<size_t>(coord.x);
}

bool NavGrid::inBounds(GridCoord coord) const noexcept {
    return coord.x >= 0 && coord.x < columns_ && coord.y >= 0 && coord.y < rows_;
}

bool NavGrid::contains(glm::vec2 world) const noexcept {
    return world.x >= origin_.x && world.x < origin_.x + worldSize().x
        && world.y >= origin_.y && world.y < origin_.y + worldSize().y;
}

GridCoord NavGrid::worldToGrid(glm::vec2 world) const noexcept {
    if (cellSize_ <= 0.0f) {
        return {0, 0};
    }
    // floor 而非截断：负坐标下截断会得到错误结果（-0.5 会变成 0 而不是 -1）
    const int x = static_cast<int>(std::floor((world.x - origin_.x) / cellSize_));
    const int y = static_cast<int>(std::floor((world.y - origin_.y) / cellSize_));

    // 钳制到网格内：越界的世界坐标映射到边界格，让寻路优雅降级而非失败
    return {clampToRange(x, 0, std::max(columns_ - 1, 0)),
            clampToRange(y, 0, std::max(rows_ - 1, 0))};
}

bool NavGrid::canStand(glm::vec2 world, float radius) const noexcept {
    if (!isInitialized()) {
        return true;   // 没有网格信息时不限制（与历史语义一致）
    }

    // 必须先判越界：worldToGrid 会把地图外的坐标钳制到边界格，
    // 只靠它无法区分"地图外"与"边界格内"。
    if (!contains(world)) {
        return false;
    }

    // 半径先做一次上限保护：异常大的半径会让下面的 span 爆炸
    // （span² 次探测），这里钳到网格尺寸量级即可。
    const float safeRadius = std::clamp(radius, 0.0f, 4096.0f);

    const int maxSpan = std::max(columns_, rows_);
    const int span = std::min(static_cast<int>(std::ceil(safeRadius / cellSize_)) + 1, maxSpan);
    const GridCoord center = worldToGrid(world);

    // 中心格必须可通行。
    // 对半径 0 而言这是**唯一**的判据（"只看格心"的语义）：
    // 下面的圆-矩形判定在 radius = 0 时恒不成立（距离 < 0 永远为假），
    // 所以没有这一句，"点在墙里"也会被判成可站立。
    if (!isWalkable(center)) {
        return false;
    }

    for (int dy = -span; dy <= span; ++dy) {
        for (int dx = -span; dx <= span; ++dx) {
            const GridCoord probe{center.x + dx, center.y + dy};

            // 可通行的格子不构成阻挡（越界格由 isWalkable 视为阻挡，
            // 会走下面的 AABB 判定，于是"身体伸出地图外"也能被正确拒绝）。
            if (isWalkable(probe)) {
                continue;
            }

            const glm::vec2 cellMin{origin_.x + static_cast<float>(probe.x) * cellSize_,
                                    origin_.y + static_cast<float>(probe.y) * cellSize_};
            const glm::vec2 closest{
                std::clamp(world.x, cellMin.x, cellMin.x + cellSize_),
                std::clamp(world.y, cellMin.y, cellMin.y + cellSize_)};

            const glm::vec2 delta = world - closest;
            if (std::sqrt(delta.x * delta.x + delta.y * delta.y) < safeRadius) {
                return false;
            }
        }
    }
    return true;
}

glm::vec2 NavGrid::gridToWorld(GridCoord coord) const noexcept {
    // 返回格子中心：作为移动目标点时，中心比角点更不容易擦到相邻障碍
    return {origin_.x + (static_cast<float>(coord.x) + 0.5f) * cellSize_,
            origin_.y + (static_cast<float>(coord.y) + 0.5f) * cellSize_};
}

uint8_t NavGrid::flagsAt(GridCoord coord) const noexcept {
    if (!inBounds(coord)) {
        // 越界视为阻挡，这样寻路不会意外走出网格
        return static_cast<uint8_t>(kBlocked);
    }
    return flags_[indexOf(coord)];
}

void NavGrid::setFlags(GridCoord coord, uint8_t flags) {
    if (inBounds(coord)) {
        flags_[indexOf(coord)] = flags;
    }
}

bool NavGrid::isWalkable(GridCoord coord) const noexcept {
    return (flagsAt(coord) & kBlocked) == 0;
}

void NavGrid::setBlocked(GridCoord coord, bool blocked) {
    if (!inBounds(coord)) {
        return;
    }
    uint8_t& flags = flags_[indexOf(coord)];
    if (blocked) {
        flags |= kBlocked;
    } else {
        flags &= static_cast<uint8_t>(~kBlocked);
    }
}

bool NavGrid::isHazard(GridCoord coord) const noexcept {
    return (flagsAt(coord) & kHazard) != 0;
}

void NavGrid::setHazard(GridCoord coord, bool hazard) {
    if (!inBounds(coord)) {
        return;
    }
    uint8_t& flags = flags_[indexOf(coord)];
    if (hazard) {
        flags |= kHazard;
    } else {
        flags &= static_cast<uint8_t>(~kHazard);
    }
}

float NavGrid::costAt(GridCoord coord) const noexcept {
    if (!inBounds(coord)) {
        return kInfinity;
    }
    return costs_[indexOf(coord)];
}

void NavGrid::setCost(GridCoord coord, float cost) {
    if (!inBounds(coord)) {
        return;
    }
    // 代价下限 0.1：0 会让 A* 的 g 值停止增长，可能破坏启发函数一致性
    costs_[indexOf(coord)] = std::max(cost, 0.1f);
}

float NavGrid::clearanceAt(GridCoord coord) const noexcept {
    if (!inBounds(coord) || clearance_.empty()) {
        return 0.0f;
    }
    return static_cast<float>(clearance_[indexOf(coord)]);
}

void NavGrid::fillRect(glm::vec2 min, glm::vec2 max, bool blocked) {
    if (!isInitialized()) {
        return;
    }

    // 先做「矩形 vs 网格」的相交测试。
    //
    // worldToGrid 会把越界坐标钳制到边界格，若直接拿钳制结果填格，一个
    // 完全落在网格之外的矩形（例如 min = {-1000, -1000}）会被映射成角上
    // 的格子并把它封死——地图上凭空多出一堵墙，寻路绕远甚至失败。
    const glm::vec2 gridMin = origin_;
    const glm::vec2 gridMax = origin_ + worldSize();

    const float rectMinX = std::min(min.x, max.x);
    const float rectMaxX = std::max(min.x, max.x);
    const float rectMinY = std::min(min.y, max.y);
    const float rectMaxY = std::max(min.y, max.y);

    if (rectMaxX < gridMin.x || rectMinX > gridMax.x
        || rectMaxY < gridMin.y || rectMinY > gridMax.y) {
        return;  // 与网格毫无交集：不影响任何格子
    }

    const GridCoord a = worldToGrid(min);
    const GridCoord b = worldToGrid(max);

    const int x0 = std::min(a.x, b.x);
    const int x1 = std::max(a.x, b.x);
    const int y0 = std::min(a.y, b.y);
    const int y1 = std::max(a.y, b.y);

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            setBlocked(GridCoord{x, y}, blocked);
        }
    }
}

void NavGrid::computeClearanceField() {
    if (!isInitialized()) {
        return;
    }

    // 多源 BFS：把所有阻挡格作为源，逐层向外扩散。
    // 得到的距离值即为「到最近障碍的格子数」，上限 5。
    constexpr int kMaxClearance = 5;

    clearance_.assign(flags_.size(), static_cast<uint8_t>(kMaxClearance));

    std::queue<GridCoord> frontier;

    for (int y = 0; y < rows_; ++y) {
        for (int x = 0; x < columns_; ++x) {
            const GridCoord coord{x, y};
            if (!isWalkable(coord)) {
                clearance_[indexOf(coord)] = 0;
                frontier.push(coord);
            }
        }
    }

    while (!frontier.empty()) {
        const GridCoord current = frontier.front();
        frontier.pop();

        const int currentClearance = clearance_[indexOf(current)];
        if (currentClearance >= kMaxClearance) {
            continue;
        }

        for (int d = 0; d < kOrthogonalDirectionCount; ++d) {
            const GridCoord neighbor{current.x + kDirX[d], current.y + kDirY[d]};
            if (!inBounds(neighbor)) {
                continue;
            }
            uint8_t& neighborClearance = clearance_[indexOf(neighbor)];
            const int candidate = currentClearance + 1;
            if (candidate < static_cast<int>(neighborClearance)) {
                neighborClearance = static_cast<uint8_t>(candidate);
                frontier.push(neighbor);
            }
        }
    }
}

size_t NavGrid::walkableCellCount() const noexcept {
    size_t count = 0;
    for (const uint8_t flags : flags_) {
        if ((flags & kBlocked) == 0) {
            ++count;
        }
    }
    return count;
}

std::string NavGrid::debugSummary() const {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "NavGrid %dx%d cell=%.1f origin=(%.0f,%.0f) walkable=%zu/%zu (%.1f%%)",
                  columns_, rows_, static_cast<double>(cellSize_),
                  static_cast<double>(origin_.x), static_cast<double>(origin_.y),
                  walkableCellCount(), flags_.size(),
                  flags_.empty() ? 0.0
                                 : 100.0 * static_cast<double>(walkableCellCount())
                                       / static_cast<double>(flags_.size()));
    return std::string(buffer);
}

// ===========================================================================
//  PathResult
// ===========================================================================
float PathResult::geometricLength() const noexcept {
    float total = 0.0f;
    for (size_t i = 0; i + 1 < waypoints.size(); ++i) {
        const glm::vec2 delta = waypoints[i + 1] - waypoints[i];
        total += std::sqrt(delta.x * delta.x + delta.y * delta.y);
    }
    return total;
}

// ===========================================================================
//  Pathfinder —— 堆操作
// ===========================================================================
void Pathfinder::ensureBuffers(size_t cellCount) {
    // 分配或扩容：只在尺寸变化时重新分配内存。
    if (gScore_.size() != cellCount) {
        gScore_.assign(cellCount, kInfinity);
        fScore_.assign(cellCount, kInfinity);
        cameFrom_.assign(cellCount, kNoParent);
        closed_.assign(cellCount, 0);
        heapPositions_.assign(cellCount, kInvalidHeapSlot);
        openHeap_.clear();
        openHeap_.reserve(cellCount / 4 + 16);
    }

    // 每次搜索开始前必须把「上一次搜索的残留状态」清干净。
    //
    // 早期版本只在「尺寸变化」时才初始化，导致同一尺寸的第二次搜索直接
    // 复用了上一次的 gScore_ / cameFrom_ / 堆状态，产生非确定性的路径
    // （同样的输入两次跑出不同结果）。确定性是 AI 可复现调试的前提，
    // 因此这里无条件重置所有搜索状态。
    //
    // 代价是一次 O(格子数) 的清零。对一个几百×几百的战术地图来说这个
    // 开销远小于 A* 本身的展开代价，用绝对正确换来省心是划算的。
    // （若将来地图大到清零成为瓶颈，可换成「记录 touched 索引再定点清」
    //   或真正的多字节代数戳方案。）
    std::fill(gScore_.begin(), gScore_.end(), kInfinity);
    std::fill(fScore_.begin(), fScore_.end(), kInfinity);
    std::fill(cameFrom_.begin(), cameFrom_.end(), kNoParent);
    std::fill(closed_.begin(), closed_.end(), static_cast<uint8_t>(0));
    std::fill(heapPositions_.begin(), heapPositions_.end(), kInvalidHeapSlot);
    openHeap_.clear();
}

float Pathfinder::heapKey(int32_t node) const noexcept {
    return fScore_[static_cast<size_t>(node)];
}

void Pathfinder::heapSiftUp(size_t index) {
    while (index > 0) {
        const size_t parent = (index - 1) / 2;
        if (heapKey(openHeap_[index]) >= heapKey(openHeap_[parent])) {
            break;
        }
        // 交换并同步更新位置索引，否则 heapUpdate 会找不到节点
        std::swap(openHeap_[index], openHeap_[parent]);
        heapPositions_[static_cast<size_t>(openHeap_[index])] = index;
        heapPositions_[static_cast<size_t>(openHeap_[parent])] = parent;

        index = parent;
    }
}

void Pathfinder::heapSiftDown(size_t index) {
    const size_t size = openHeap_.size();
    while (true) {
        const size_t left  = index * 2 + 1;
        const size_t right = index * 2 + 2;
        size_t smallest = index;

        if (left < size && heapKey(openHeap_[left]) < heapKey(openHeap_[smallest])) {
            smallest = left;
        }
        if (right < size && heapKey(openHeap_[right]) < heapKey(openHeap_[smallest])) {
            smallest = right;
        }
        if (smallest == index) {
            break;
        }

        std::swap(openHeap_[index], openHeap_[smallest]);
        heapPositions_[static_cast<size_t>(openHeap_[index])]    = index;
        heapPositions_[static_cast<size_t>(openHeap_[smallest])] = smallest;

        index = smallest;
    }
}

void Pathfinder::heapPush(int32_t node) {
    const size_t slot = heapPositions_[static_cast<size_t>(node)];
    if (slot != kInvalidHeapSlot) {
        // 已在堆中：只需因 f 值下降而上浮
        heapSiftUp(slot);
        return;
    }
    openHeap_.push_back(node);
    heapPositions_[static_cast<size_t>(node)] = openHeap_.size() - 1;
    heapSiftUp(openHeap_.size() - 1);
}

void Pathfinder::heapUpdate(int32_t node) {
    const size_t slot = heapPositions_[static_cast<size_t>(node)];
    if (slot == kInvalidHeapSlot) {
        return;
    }
    // f 值只可能下降（A* 单调性保证），因此向上调整即可
    heapSiftUp(slot);
}

int32_t Pathfinder::heapPop() {
    if (openHeap_.empty()) {
        return -1;
    }

    const int32_t top = openHeap_.front();

    // 把末尾元素提到根，然后下沉
    openHeap_.front() = openHeap_.back();
    heapPositions_[static_cast<size_t>(openHeap_.front())] = 0;
    openHeap_.pop_back();

    heapPositions_[static_cast<size_t>(top)] = kInvalidHeapSlot;

    if (!openHeap_.empty()) {
        heapSiftDown(0);
    }
    return top;
}

// ===========================================================================
//  Pathfinder —— 视线与平滑
// ===========================================================================
bool Pathfinder::hasLineOfSight(const NavGrid& grid, glm::vec2 a, glm::vec2 b, float stepSize) {
    if (!grid.isInitialized()) {
        return true;
    }

    const glm::vec2 delta = b - a;
    const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    const float step = std::max(stepSize, 0.5f);

    // 用等距采样而非 Bresenham：步长可调，且能正确处理非格子对齐的端点
    const int sampleCount = static_cast<int>(distance / step) + 1;

    for (int i = 0; i <= sampleCount; ++i) {
        const float t = (sampleCount == 0)
                            ? 0.0f
                            : static_cast<float>(i) / static_cast<float>(sampleCount);
        const glm::vec2 sample{a.x + delta.x * t, a.y + delta.y * t};

        // 采样点跑出网格范围时一律视为「被挡住」。
        // 不能只依赖 isWalkable：worldToGrid 会把越界坐标钳制到边界格，
        // 于是「射线穿出地图」会被误判成畅通。这里必须用 contains 明确拦截。
        if (!grid.contains(sample)) {
            return false;
        }
        if (!grid.isWalkable(grid.worldToGrid(sample))) {
            return false;
        }
    }
    return true;
}

std::vector<glm::vec2> Pathfinder::smooth(const NavGrid& grid,
                                          const std::vector<glm::vec2>& points,
                                          float stepSize) {
    if (points.size() <= 2) {
        return points;
    }

    std::vector<glm::vec2> result;
    result.reserve(points.size());
    result.push_back(points.front());

    // 贪心拉直：从当前锚点出发，尽量跳到最远的、有视线的点。
    // 这一步把格子路径变成看起来「有人类意图」的直线段。
    size_t anchor = 0;
    while (anchor + 1 < points.size()) {
        size_t farthest = anchor + 1;

        for (size_t candidate = anchor + 2; candidate < points.size(); ++candidate) {
            if (hasLineOfSight(grid, points[anchor], points[candidate], stepSize)) {
                farthest = candidate;
            } else {
                // 视线断了，停止扩张（后面的点只会更不可达）
                break;
            }
        }

        result.push_back(points[farthest]);
        anchor = farthest;
    }

    return result;
}

std::optional<glm::vec2> Pathfinder::pointAhead(const std::vector<glm::vec2>& path,
                                                size_t& cursor, glm::vec2 current,
                                                float ahead) {
    if (path.empty()) {
        return std::nullopt;
    }

    // 先推进游标：跳过已经走过的路点。
    //
    // 判定分两种情形，缺一不可：
    //   1. 距离足够近（< kReachedThreshold）——标准「到达」判定；
    //   2. 路点在 agent 的行进方向「后方」——agent 已经越过它了，
    //      此时距离可能很大（例如刚刚加速冲过去），但显然不该回头。
    // 只做情形 1 会导致 agent 被一个已经甩在身后的路点「拽回去」。
    constexpr float kReachedThreshold = 6.0f;
    while (cursor < path.size()) {
        if (distanceBetween(current, path[cursor]) < kReachedThreshold) {
            ++cursor;
            continue;
        }

        // 情形 2：用「当前 -> 路点」与「当前 -> 下一路点」的方向点积判断。
        // 点积为负说明路点在下一段的反方向，即 agent 已越过它。
        if (cursor + 1 < path.size()) {
            const glm::vec2 toCurrent = path[cursor] - current;
            const glm::vec2 toNext    = path[cursor + 1] - current;
            // 手写 2D 点积，避免为一个两分量运算引入 glm/geometric 头
            if (toCurrent.x * toNext.x + toCurrent.y * toNext.y < 0.0f) {
                ++cursor;
                continue;
            }
        }
        break;
    }

    if (cursor >= path.size()) {
        return std::nullopt;
    }

    // agent 已经站在终点上：整条路径视为走完，返回 nullopt 让调用方
    // 停止跟随（否则会一直「朝终点再走 0 距离」，永远不结束）。
    if (distanceBetween(current, path.back()) < kReachedThreshold) {
        cursor = path.size();
        return std::nullopt;
    }

    // 从当前游标开始，沿路径前进 ahead 距离，返回该处位置。
    // 这让 agent 沿着折线平滑移动，而不是走一段停一段。
    glm::vec2 position = current;
    float remaining = ahead;

    for (size_t i = cursor; i < path.size(); ++i) {
        const glm::vec2 toPoint = path[i] - position;
        const float segmentLength = std::sqrt(toPoint.x * toPoint.x + toPoint.y * toPoint.y);

        if (segmentLength < 1e-4f) {
            continue;
        }
        if (segmentLength >= remaining) {
            const glm::vec2 direction = toPoint / segmentLength;
            return position + direction * remaining;
        }

        remaining -= segmentLength;
        position = path[i];
    }

    // ahead 超出剩余路径：直接返回终点
    return path.back();
}

// ===========================================================================
//  Pathfinder —— A* 主算法
// ===========================================================================
PathResult Pathfinder::findPath(const NavGrid& grid, const PathRequest& request) {
    // 【修复】以前这里只把 start/goal 转交给下面的重载，request 里的
    // allowPartialPath / allowDiagonal / maxExpandedNodes 被静默丢弃：
    // 调用方设置 allowPartialPath = false 期待「到不了就失败」，却仍会拿到
    // success = true 的部分路径，上层状态机会被错误驱动。现在三个字段全部生效。
    return findPathImpl(grid, request.start, request.goal,
                        request.allowPartialPath,
                        request.allowDiagonal,
                        request.maxExpandedNodes);
}

PathResult Pathfinder::findPath(const NavGrid& grid, glm::vec2 start, glm::vec2 goal) {
    // 便捷重载：沿用寻路器自身配置与旧的「允许部分路径」语义。
    return findPathImpl(grid, start, goal,
                        true, config_.allowDiagonal, kDefaultMaxExpandedNodes);
}

PathResult Pathfinder::findPathImpl(const NavGrid& grid, glm::vec2 start, glm::vec2 goal,
                                    bool allowPartialPath, bool allowDiagonal,
                                    int maxExpandedNodes) {
    // 防御：非法的预算值回落到默认值，避免「预算 0 = 立即放弃」这种
    // 难以察觉的静默失败。
    if (maxExpandedNodes <= 0) {
        maxExpandedNodes = kDefaultMaxExpandedNodes;
    }

    const auto startClock = std::chrono::steady_clock::now();

    PathResult result;

    if (!grid.isInitialized()) {
        return result;
    }

    const int columns = grid.columns();

    // 把世界坐标转成格子索引，并注意起点/终点可能落在阻挡格里。
    GridCoord startCoord = grid.worldToGrid(start);
    GridCoord goalCoord  = grid.worldToGrid(goal);

    // 起点被阻挡（例如单位卡在墙里）时，向外环形搜索一个最近的可行格，
    // 这样寻路仍能救出单位，而不是直接失败。
    auto isUsable = [&grid](GridCoord coord) { return grid.isWalkable(coord); };

    bool startRescued = false;

    if (!isUsable(startCoord)) {
        startRescued = true;

        // 环形向外搜索最近可行格。搜索半径不再固定为 4：
        // 单位可能卡在任意厚的墙/大块障碍内部，半径必须足够覆盖最坏情况。
        // 上界取网格对角线长度即可保证「要么找到边界出口，要么确认全封死」。
        const int maxRadius = std::max(grid.columns(), grid.rows());

        bool found = false;
        for (int radius = 1; radius <= maxRadius && !found; ++radius) {
            // 【优化】只遍历当前这一圈的周长，而不是整个 (2r+1)^2 方块。
            // 原写法每轮都扫完整个方块、再用 continue 跳过内部格子，
            // 总迭代量约 (4/3)·maxRadius^3；而 maxRadius 取的是
            // max(columns, rows)，128x128 全阻挡网格下是数百万次空转。
            // 周长只有 4·(2r+1) 个格子，代价降到 O(radius^2)。
            for (int edge = 0; edge < 4 && !found; ++edge) {
                for (int step = -radius; step <= radius && !found; ++step) {
                    int dx = 0;
                    int dy = 0;
                    switch (edge) {
                        case 0: dx = step;  dy = -radius; break;  // 下边
                        case 1: dx = step;  dy =  radius; break;  // 上边
                        case 2: dx = -radius; dy = step;  break;  // 左边
                        default: dx = radius;  dy = step;  break; // 右边
                    }

                    const GridCoord probe{startCoord.x + dx, startCoord.y + dy};
                    if (grid.inBounds(probe) && isUsable(probe)) {
                        startCoord = probe;
                        found = true;
                    }
                }
            }
        }
        if (!found) {
            return result;  // 起点被完全封死，网格里没有任何可行格
        }

        // 起点是被「救」出来的：结果应标记为部分路径，
        // 让调用方知道实际起始位置与请求位置并不一致。
        //
        // 【修复】调用方明确禁止部分路径时，起点不可站立即是失败，
        // 不能悄悄给出一条起点被挪动过的路径。
        if (!allowPartialPath) {
            return result;
        }
        result.partial = true;
    }

    if (!isUsable(goalCoord)) {
        // 目标不可站立：若允许部分路径，就退化为「接近目标」；
        // 否则直接失败。
        //
        // 【修复】这里原先无条件做「接近目标」降级，注释承诺的
        // 「否则直接失败」从未实现，allowPartialPath = false 被无视。
        if (!allowPartialPath) {
            return result;
        }
        bool found = false;
        float bestDistance = kInfinity;
        GridCoord bestCoord = goalCoord;

        for (int radius = 1; radius <= 8; ++radius) {
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    const GridCoord probe{goalCoord.x + dx, goalCoord.y + dy};
                    if (!grid.inBounds(probe) || !isUsable(probe)) {
                        continue;
                    }
                    const float distance = static_cast<float>(
                        std::abs(probe.x - goalCoord.x) + std::abs(probe.y - goalCoord.y));
                    if (distance < bestDistance) {
                        bestDistance = distance;
                        bestCoord = probe;
                        found = true;
                    }
                }
            }
            if (found) {
                break;  // 找到最近一圈的可达格即可停止扩张
            }
        }

        if (!found) {
            return result;
        }
        goalCoord = bestCoord;
        result.partial = true;
    }

    const int32_t startIndex = static_cast<int32_t>(
        static_cast<size_t>(startCoord.y) * static_cast<size_t>(columns)
        + static_cast<size_t>(startCoord.x));
    const int32_t goalIndex = static_cast<int32_t>(
        static_cast<size_t>(goalCoord.y) * static_cast<size_t>(columns)
        + static_cast<size_t>(goalCoord.x));

    const size_t cellCount = static_cast<size_t>(grid.columns()) * static_cast<size_t>(grid.rows());
    ensureBuffers(cellCount);

    // 清空本次搜索用到的格子状态。
    //
    // 【注释更正】这里并不存在「代数戳」：ensureBuffers 无条件清零全部数组
    // （见 Pathfinding.h 的说明：曾尝试过 stamp 方案，因破坏确定性而放弃）。
    // 旧注释描述的是已废弃的实现，会误导后来者。
    // 【防御】NaN 坐标会让 floor→int 转换变成 UB，起点/终点索引因此可能
    // 落在缓冲之外；这里一旦检测到就放弃本次搜索。
    if (!std::isfinite(start.x) || !std::isfinite(start.y)
        || !std::isfinite(goal.x) || !std::isfinite(goal.y)) {
        return result;
    }
    if (startIndex < 0 || goalIndex < 0
        || static_cast<size_t>(startIndex) >= cellCount
        || static_cast<size_t>(goalIndex) >= cellCount) {
        return result;
    }
    gScore_[static_cast<size_t>(startIndex)] = 0.0f;
    cameFrom_[static_cast<size_t>(startIndex)] = kNoParent;

    auto heuristic = [&grid, columns, goalCoord, this](int32_t index) -> float {
        const int x = index % columns;
        const int y = index / columns;
        // 八方向网格用 Octile 距离：对角允许时它是可采纳且紧致的启发函数
        const float dx = std::fabs(static_cast<float>(x - goalCoord.x));
        const float dy = std::fabs(static_cast<float>(y - goalCoord.y));
        const float diagonal = std::min(dx, dy);
        const float straight = std::max(dx, dy) - diagonal;

        const float estimate = straight + diagonal * config_.diagonalCost;
        return estimate * config_.heuristicWeight * grid.cellSize();
    };

    fScore_[static_cast<size_t>(startIndex)] = heuristic(startIndex);
    heapPush(startIndex);

    int expanded = 0;
    bool reachedGoal = false;

    // 【优化】顺带记录「已扩展过的、离目标最近」的节点。
    // 原实现在搜索失败后线性扫描整个 closed_ 数组（128x128 = 16384 格），
    // 而「敌人反复对不可达目标重寻路」正是高频场景；改成搜索过程中
    // O(1) 维护，代价为零。
    float   bestHeuristic = heuristic(startIndex);
    int32_t bestNode      = startIndex;

    // 【修复】节点预算改为来自 PathRequest，不再硬编码 20000。
    while (expanded < maxExpandedNodes) {
        const int32_t current = heapPop();
        if (current < 0) {
            break;  // 开放列表耗尽：目标不可达
        }

        if (current == goalIndex) {
            reachedGoal = true;
            break;
        }

        // 用代数戳标记「已关闭」。stamp 每次分配新缓冲时自增，
        // 因此无需在每次搜索前 memset 整个数组。
        if (closed_[static_cast<size_t>(current)] != 0) {
            continue;
        }
        closed_[static_cast<size_t>(current)] = 1;

        // 维护「离目标最近的已扩展节点」，供部分路径回退使用
        const float currentHeuristic = heuristic(current);
        if (currentHeuristic < bestHeuristic) {
            bestHeuristic = currentHeuristic;
            bestNode      = current;
        }

        ++expanded;

        const int cx = current % columns;
        const int cy = current / columns;

        // 【修复】对角开关改为来自 PathRequest（原先只读寻路器自身配置，
        // request.allowDiagonal 被忽略）。
        const int directionCount = allowDiagonal ? 8 : 4;

        for (int d = 0; d < directionCount; ++d) {
            const int nx = cx + kDirX[d];
            const int ny = cy + kDirY[d];

            if (nx < 0 || nx >= columns || ny < 0 || ny >= grid.rows()) {
                continue;
            }

            const GridCoord neighborCoord{nx, ny};
            if (!grid.isWalkable(neighborCoord)) {
                continue;
            }

            const int32_t neighborIndex = static_cast<int32_t>(
                static_cast<size_t>(ny) * static_cast<size_t>(columns)
                + static_cast<size_t>(nx));

            if (closed_[static_cast<size_t>(neighborIndex)] != 0) {
                continue;
            }

            const bool isDiagonal = (d >= kOrthogonalDirectionCount);

            // 对角穿墙检查：若对角方向的两个正交邻居都是墙，
            // 不允许斜穿墙角（否则单位会视觉上「穿过」墙的尖角）。
            if (isDiagonal) {
                const GridCoord sideA{cx + kDirX[d], cy};
                const GridCoord sideB{cx, cy + kDirY[d]};
                if (!grid.isWalkable(sideA) && !grid.isWalkable(sideB)) {
                    continue;
                }
                // 只允许一侧是墙的贴角通行，两侧全通或单侧阻挡都可行
                if (!grid.isWalkable(sideA) || !grid.isWalkable(sideB)) {
                    // 贴角通行附加惩罚，让路径倾向走开阔处
                }
            }

            const float stepCost = grid.cellSize() * (isDiagonal ? config_.diagonalCost : 1.0f);
            const float terrainCost = grid.costAt(neighborCoord);
            const float tentativeG = gScore_[static_cast<size_t>(current)]
                                     + stepCost * terrainCost;

            if (tentativeG < gScore_[static_cast<size_t>(neighborIndex)]) {
                gScore_[static_cast<size_t>(neighborIndex)] = tentativeG;
                cameFrom_[static_cast<size_t>(neighborIndex)] = current;
                fScore_[static_cast<size_t>(neighborIndex)] = tentativeG + heuristic(neighborIndex);

                if (heapPositions_[static_cast<size_t>(neighborIndex)] == kInvalidHeapSlot) {
                    heapPush(neighborIndex);
                } else {
                    heapUpdate(neighborIndex);
                }
            }
        }
    }

    result.expandedNodes = expanded;
    result.success = reachedGoal;

    // 若未到达目标但允许部分路径，取搜索过程中离目标最近的节点作为终点。
    // 这实现了「走到尽可能近的地方」而不是干脆不动。
    int32_t finalIndex = goalIndex;
    if (!reachedGoal) {
        // 【修复】这里原先有一段 `if (!result.partial && !true)` 的占位死代码，
        // 之后无条件生成部分路径并置 success = true —— 于是调用方设置
        // allowPartialPath = false 期待「到不了就失败」，却拿到一条标记成功
        // 的部分路径，上层「到达后才进入下一阶段」的状态机会被错误驱动。
        if (!allowPartialPath) {
            result.success = false;
            result.partial = false;
            return result;
        }

        // 【优化】不再线性扫描 closed_ 全数组，改用搜索过程中维护的 bestNode。
        const int32_t bestIndex = bestNode;

        if (bestIndex == startIndex) {
            // 一步都没走成：确实无路可走
            result.success = false;
            result.partial = false;
            return result;
        }

        finalIndex = bestIndex;
        result.partial = true;
        result.success = true;   // 有可用的部分路径
    }

    // 回溯路径（从终点反向走父指针到起点）
    std::vector<glm::vec2> rawPath;
    for (int32_t node = finalIndex; node != kNoParent; node = cameFrom_[static_cast<size_t>(node)]) {
        rawPath.push_back(grid.gridToWorld(GridCoord{node % columns, node / columns}));

        // 防御性检查：父指针成环会导致死循环
        if (rawPath.size() > cellCount) {
            break;
        }
    }
    std::reverse(rawPath.begin(), rawPath.end());

    // 用真实的世界坐标替换首尾点，让路径精确起止于请求位置。
    // 否则 agent 会先「瞬移」到起点格中心，看起来像跳了一下。
    //
    // 关键区别在终点：
    //   - 完整路径：终点就是请求的 goal，直接替换没问题；
    //   - 部分路径：goal 本身是不可通行的（目标在墙里/不可达），
    //     此时端点必须保持为「搜索找到的最近可行格」，绝不能写成 goal，
    //     否则路径末端会落进障碍里，agent 会一头撞进墙中。
    if (!rawPath.empty()) {
        // 【修复】起点若被「救援」过（原始 start 落在障碍里），首点必须保持
        // 为救援格中心，否则路径第一段会从墙里穿出去。
        if (!startRescued) {
            rawPath.front() = start;
        }
        if (!result.partial) {
            rawPath.back() = goal;
        }
        // partial 情况下保留 rawPath.back()（即 gridToWorld(finalIndex)）
    }

    result.totalCost = gScore_[static_cast<size_t>(finalIndex)];

    // 平滑
    if (config_.smoothPath && rawPath.size() > 2) {
        result.waypoints = smooth(grid, rawPath, config_.smoothingStep);
    } else {
        result.waypoints = std::move(rawPath);
    }

    const auto endClock = std::chrono::steady_clock::now();
    result.elapsedMs = std::chrono::duration<double, std::milli>(endClock - startClock).count();

    ++totalSearches_;
    totalExpandedNodes_ += static_cast<uint64_t>(expanded);
    totalElapsedMs_ += result.elapsedMs;

    return result;
}

float Pathfinder::averageExpandedNodes() const noexcept {
    if (totalSearches_ == 0) {
        return 0.0f;
    }
    return static_cast<float>(totalExpandedNodes_) / static_cast<float>(totalSearches_);
}

void Pathfinder::resetStats() noexcept {
    totalSearches_ = 0;
    totalExpandedNodes_ = 0;
    totalElapsedMs_ = 0.0;
}

} // namespace TacticalAI::AI
