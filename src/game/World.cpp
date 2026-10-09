// ===========================================================================
//  src/game/World.cpp
// ===========================================================================
#include "game/World.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <sstream>

namespace TacticalAI::Game {
namespace {

inline float vectorLength(glm::vec2 v) noexcept {
    return std::sqrt(v.x * v.x + v.y * v.y);
}

/// 点到轴对齐矩形的距离（在矩形内部返回 0）。
inline float distanceToRect(glm::vec2 point, glm::vec2 center, glm::vec2 size) noexcept {
    const float halfX = size.x * 0.5f;
    const float halfY = size.y * 0.5f;

    const float dx = std::max(std::fabs(point.x - center.x) - halfX, 0.0f);
    const float dy = std::max(std::fabs(point.y - center.y) - halfY, 0.0f);

    return std::sqrt(dx * dx + dy * dy);
}

/// 线段与轴对齐矩形是否相交（slab 法，闭区间）。
///
/// 【为什么视线与弹道需要它，而不是继续用导航网格的阻挡格】
/// 阻挡格是**膨胀过的近似**：`NavGrid::fillRect` 用矩形角点的 floor 填格，
/// 障碍只要**压住某格的一部分**，整格就被标成阻挡。于是每个掩体的实际
/// 阻挡范围会向外多出 0~32 单位（取决于墙面落在格子内的位置）。
/// 拿它判视线/子弹的后果是"贴墙即隐身、并且子弹在离墙一格远的地方就被吃掉"
/// —— 一堵看不见的护盾（实测：墙西侧面在 x=700、玩家贴在 x=688 时，
/// 敌人完全看不见他；朝他开枪也打不中）。
///
/// 视线与弹道要的是**几何真相**（线段有没有穿过那个矩形），
/// 而膨胀格是给**寻路/站立**用的表示（实体需要身体间隙），两者不该混用。
bool segmentIntersectsRect(glm::vec2 from, glm::vec2 to,
                           glm::vec2 min, glm::vec2 max) noexcept {
    const glm::vec2 delta = to - from;
    float tEnter = 0.0f;
    float tExit  = 1.0f;

    for (int axis = 0; axis < 2; ++axis) {
        const float origin = (axis == 0) ? from.x : from.y;
        const float dir    = (axis == 0) ? delta.x : delta.y;
        const float lo     = (axis == 0) ? min.x : min.y;
        const float hi     = (axis == 0) ? max.x : max.y;

        if (std::fabs(dir) < 1e-6f) {
            // 与该轴平行：只要起点在这条轴的区间之外，整条线段都在外侧
            if (origin < lo || origin > hi) {
                return false;
            }
            continue;
        }

        float t0 = (lo - origin) / dir;
        float t1 = (hi - origin) / dir;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tEnter = std::max(tEnter, t0);
        tExit  = std::min(tExit, t1);
        if (tEnter > tExit) {
            return false;
        }
    }

    return true;   // 存在 t ∈ [0,1] 落进矩形
}

} // namespace

// ===========================================================================
//  构造 / 析构
// ===========================================================================
World::World(WorldConfig config) : config_(config) {
    // 预分配缓存，避免运行时反复扩容
    perceptionTargets_.reserve(64);
    threatSources_.reserve(64);
    coverPositions_.reserve(64);
    damageEvents_.reserve(32);
    impactMarks_.reserve(64);

    // 弹道参数自检：射程必须为正，否则子弹一出生就被判定为"射程耗尽"，
    // 表现为"枪响了但什么都不飞出来"。这里直接把非法值修正掉，
    // 而不是等到运行时才让人去猜为什么没子弹。
    if (config_.playerProjectile.maxRange <= 0.0f) {
        config_.playerProjectile.maxRange = defaultPlayerProjectile().maxRange;
    }
    if (config_.enemyProjectile.maxRange <= 0.0f) {
        config_.enemyProjectile.maxRange = defaultEnemyProjectile().maxRange;
    }
}

World::~World() = default;

const char* toString(MatchOutcome outcome) noexcept {
    // 与 toString(AIState) 同理：这些字符串会渲染在结算面板上，用中文。
    // 新增字面量后需确认码点进了 GeneratedGlyphs.h（由 gen_glyphs.py 自动扫描）。
    switch (outcome) {
        case MatchOutcome::Running: return "进行中";
        case MatchOutcome::Victory: return "胜利";
        case MatchOutcome::Defeat:  return "失败";
    }
    return "未知";
}

// ===========================================================================
//  初始化
// ===========================================================================
void World::initialize() {
    if (initialized_) {
        return;
    }

    buildNavGrid();

    if (config_.generateDemoObstacles) {
        generateDemoObstacles();
    }

    // 重建导航网格的间距场（依赖障碍物已就位）
    navGrid_.computeClearanceField();

    // 玩家
    auto player = std::make_unique<Player>(config_.playerStart);
    entityIndex_[player->id()] = player.get();
    entities_.push_back(std::move(player));

    // 敌人
    const int enemyCount = std::max(config_.initialEnemyCount, 0);
    for (int i = 0; i < enemyCount; ++i) {
        (void)spawnEnemy();   // 生成失败（找不到空位）时静默跳过，不阻断初始化
    }

    initialized_ = true;

    std::printf("[World] 初始化完成: %zu 实体, %zu 敌人, %s\n",
                entities_.size(), aliveEnemyCount(), navGrid_.debugSummary().c_str());
}

void World::reset() {
    entities_.clear();
    entityIndex_.clear();
    damageEvents_.clear();
    impactMarks_.clear();
    pendingRemoval_.clear();

    // 【修复】掩体/威胁列表原先漏清。initialize() → generateDemoObstacles()
    // 会重新 push 一遍相同位置，于是每次 reset 后 coverPositions_ 都翻倍，
    // EQS 的掩体邻近度测试拿到重复数据；威胁源同理。
    coverPositions_.clear();
    threatSources_.clear();

    enemyCount_ = 0;
    totalShotsFired_ = 0;
    totalProjectileHits_ = 0;
    totalProjectilesDodged_ = 0;
    totalDamageDealt_ = 0.0f;
    enemiesKilled_ = 0;
    elapsedGameTime_ = 0.0;
    rebaseTimePending_ = true;   // 下一次 update 重新对齐"本局起点"
    lastOutcome_ = MatchOutcome::Running;
    outcomeSettledTime_ = 0.0;
    aiUpdateCount_ = 0;

    // 重开必须让 AI 活过来：否则"F2 冻结 → 重开"之后会得到一个完全不动的
    // 世界，而界面上没有任何线索指向"AI 还冻着"。（调试面板里的冻结开关
    // 是局内状态，重开即视为放弃这个调试上下文。）
    aiEnabled_ = true;
    totalAITimeMs_ = 0.0;
    spawnSeed_ = 0x5EEDu;
    initialized_ = false;
    // 重开必须把"曾经生成过敌人"清掉，否则新一局在生成第一个敌人之前
    // 会被判成 Victory（aliveEnemyCount 已经是 0）。
    spawnedAnyEnemy_ = false;

    initialize();
}

// ---------------------------------------------------------------------------
//  导航网格构建
// ---------------------------------------------------------------------------
void World::buildNavGrid() {
    const float cellSize = std::max(config_.cellSize, 4.0f);

    // 【必须向上取整，不能整除】
    //
    // 整除会让网格**覆盖不到**配置的边界：gridExtent.y = 1200、cellSize = 32
    // 时 1200/32 = 37.5 → 37 行 → 只覆盖到 y = 1184，地图最下面 16 个单位
    // 完全没有导航数据。后果是连锁的：
    //
    //   1. 下边界墙落在 y ∈ [1200, 1240]，与网格 [0, 1184) **毫无交集**，
    //      fillRect 的相交测试直接 return —— 下边界等于没有墙。
    //      （上边界墙因为"恰好碰到" y = 0 才侥幸生效，这就是为什么地图
    //      上边界能挡住而边界挡不住。）
    //   2. AI 的目标点一旦落到这段"无数据区"之外，worldToGrid 会把它钳制
    //      到边界格，于是地图外的点看起来"可通行"，敌人被派出去却永远
    //      走不到，被边界约束顶在原处 → 卡死（实测敌人贴着下边缘不动）。
    //
    // 向上取整后网格覆盖 [0, 1216)，边界墙与网格有交集、能真正封住外圈。
    const int columns = static_cast<int>(std::ceil(config_.gridExtent.x / cellSize));
    const int rows    = static_cast<int>(std::ceil(config_.gridExtent.y / cellSize));

    navGrid_.initialize(glm::vec2(0.0f, 0.0f), cellSize, columns, rows);
}

void World::generateDemoObstacles() {
    // 一组手工设计的障碍物，构成有意义的战术地形：
    //  - 中央的几个 L 形掩体，提供「可绕行、可利用」的遮挡
    //  - 左右两侧的长墙，制造通道（AI 会自然学会走通道）
    // 这些位置是刻意「不整齐」的，以便 EQS 有真实的选择空间。

    struct ObstacleSpec {
        glm::vec2 position;
        glm::vec2 size;
        bool      cover;
    };

    const std::vector<ObstacleSpec> specs = {
        // 中央主掩体群
        {{700.0f, 560.0f}, {40.0f, 180.0f}, true},
        {{860.0f, 640.0f}, {180.0f, 40.0f}, true},
        {{1040.0f, 520.0f}, {40.0f, 160.0f}, true},

        // 左侧通道
        {{420.0f, 320.0f}, {30.0f, 200.0f}, false},
        {{420.0f, 900.0f}, {30.0f, 180.0f}, false},

        // 右侧通道
        {{1320.0f, 300.0f}, {30.0f, 220.0f}, true},
        {{1320.0f, 880.0f}, {30.0f, 200.0f}, true},

        // 零散小掩体，供 AI 做微调位置
        {{600.0f, 880.0f}, {70.0f, 70.0f}, true},
        {{980.0f, 300.0f}, {70.0f, 70.0f}, true},
        {{1150.0f, 780.0f}, {60.0f, 60.0f}, true},
    };

    int index = 0;
    for (const ObstacleSpec& spec : specs) {
        (void)spawnObstacle("Obstacle" + std::to_string(index++), spec.position, spec.size,
                            spec.cover);
    }

    // 边界的四面墙：把 AI 限制在沙盒内。
    // 用厚墙而非把网格边界设为阻挡，是为了让「视觉边界」与「导航边界」一致。
    const glm::vec2 extent = config_.gridExtent;
    constexpr float kWallThickness = 40.0f;

    (void)spawnObstacle("Wall.Top",    {extent.x * 0.5f, -kWallThickness * 0.5f},
                        {extent.x, kWallThickness}, false);
    (void)spawnObstacle("Wall.Bottom", {extent.x * 0.5f, extent.y + kWallThickness * 0.5f},
                        {extent.x, kWallThickness}, false);
    (void)spawnObstacle("Wall.Left",   {-kWallThickness * 0.5f, extent.y * 0.5f},
                        {kWallThickness, extent.y}, false);
    (void)spawnObstacle("Wall.Right",  {extent.x + kWallThickness * 0.5f, extent.y * 0.5f},
                        {kWallThickness, extent.y}, false);

    // 把障碍物同步到导航网格。
    // 这一步必须在所有障碍物生成之后、计算间距场之前完成。
    for (const auto& entity : entities_) {
        if (entity->type() != EntityType::Obstacle) {
            continue;
        }
        const auto* obstacle = static_cast<const Obstacle*>(entity.get());
        const glm::vec2 half = obstacle->size() * 0.5f;
        navGrid_.fillRect(obstacle->position() - half, obstacle->position() + half, true);
    }
}

// ===========================================================================
//  实体访问
// ===========================================================================
Player* World::player() noexcept {
    for (const auto& entity : entities_) {
        if (entity->type() == EntityType::Player) {
            return static_cast<Player*>(entity.get());
        }
    }
    return nullptr;
}

const Player* World::player() const noexcept {
    for (const auto& entity : entities_) {
        if (entity->type() == EntityType::Player) {
            return static_cast<const Player*>(entity.get());
        }
    }
    return nullptr;
}

std::vector<Enemy*> World::enemies() const {
    std::vector<Enemy*> result;
    result.reserve(enemyCount_);
    for (const auto& entity : entities_) {
        if (entity->type() == EntityType::Enemy) {
            result.push_back(static_cast<Enemy*>(entity.get()));
        }
    }
    return result;
}

size_t World::aliveEnemyCount() const {
    size_t count = 0;
    for (const auto& entity : entities_) {
        if (entity->type() == EntityType::Enemy && !entity->isDead()) {
            ++count;
        }
    }
    return count;
}

bool World::hasLiveEnemyProjectile() const noexcept {
    for (const auto& entity : entities_) {
        if (entity->type() != EntityType::Projectile) {
            continue;
        }
        const auto* projectile = static_cast<const Projectile*>(entity.get());
        if (projectile->faction() == Faction::Enemy && !projectile->terminated()) {
            return true;
        }
    }
    return false;
}

MatchOutcome World::outcome() const {
    // 失败优先于胜利。
    //
    // 【为什么先判玩家死亡】
    // 最后一击通常是致命的：打死最后一个敌人的那颗子弹可能同时（或几乎同时）
    // 命中玩家。两个条件同时成立时，先判谁决定显示什么。
    // 选 Defeat 是因为"我方团灭"比"敌人清空"更需要被立刻告知 ——
    // 而反过来（敌人全灭但玩家还活着）本来就是玩家赢。
    if (const auto* p = player(); p != nullptr && p->isDead()) {
        return MatchOutcome::Defeat;
    }

    // 胜利：曾经有敌人，现在一个都不剩，并且**敌方在飞的子弹也都落地了**。
    //
    // spawnedAnyEnemy_ 而不是 enemyCount_ > 0：--enemies 0 从未生成过敌人，
    // 不该被判成胜利（否则开局第一帧就显示"胜利"）。
    //
    // 【为什么还要等子弹】
    // 子弹是独立实体，不随主人消失。最后一个敌人被击杀时，它可能刚射出一发
    // 还在空中的子弹；若此时立刻宣布胜利，下一帧这发子弹把玩家打死，
    // 结算就会从"胜利"翻成"失败"——玩家看到的是"我赢了，然后突然输了"。
    // 等敌方弹幕清空再判定，这个翻转就不可能发生。
    // （尸体不阻塞判定：它只是 3 秒的击杀反馈，不该让玩家多等。）
    if (spawnedAnyEnemy_ && aliveEnemyCount() == 0 && !hasLiveEnemyProjectile()) {
        return MatchOutcome::Victory;
    }

    return MatchOutcome::Running;
}

std::vector<Projectile*> World::projectiles() const {
    std::vector<Projectile*> result;
    for (const auto& entity : entities_) {
        if (entity->type() != EntityType::Projectile) {
            continue;
        }
        result.push_back(static_cast<Projectile*>(entity.get()));
    }
    return result;
}

size_t World::projectileCount() const noexcept {
    size_t count = 0;
    for (const auto& entity : entities_) {
        if (entity->type() != EntityType::Projectile) {
            continue;
        }
        // 【修复】Projectile 的 health 恒为 1，isDead() 对它永远返回 false，
        // 这个过滤条件是死代码（当下无害只是因为已终止的子弹在同一步的
        // 阶段 5 就被移除了）。真正表达"还在飞"的是 terminated()。
        const auto* projectile = static_cast<const Projectile*>(entity.get());
        if (!projectile->terminated()) {
            ++count;
        }
    }
    return count;
}

Entity* World::findEntity(uint32_t id) noexcept {
    const auto it = entityIndex_.find(id);
    return (it == entityIndex_.end()) ? nullptr : it->second;
}

const Entity* World::findEntity(uint32_t id) const noexcept {
    const auto it = entityIndex_.find(id);
    return (it == entityIndex_.end()) ? nullptr : it->second;
}

// ===========================================================================
//  生成
// ===========================================================================
glm::vec2 World::findFreeSpawnPosition(int maxAttempts, float clearanceRadius) const {
    // 简单的拒绝采样：随机撒点直到找到不与任何实体重叠的位置。
    // 对沙盒规模（几十个实体）完全够用，无需空间分割结构。
    std::mt19937 rng(spawnSeed_);
    std::uniform_real_distribution<float> distX(120.0f, config_.gridExtent.x - 120.0f);
    std::uniform_real_distribution<float> distY(120.0f, config_.gridExtent.y - 120.0f);

    const Player* playerEntity = player();
    const glm::vec2 playerPosition = playerEntity ? playerEntity->position()
                                                  : glm::vec2(0.0f, 0.0f);

    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        const glm::vec2 candidate{distX(rng), distY(rng)};

        // 必须可站立。传入实体半径做膨胀检查，否则实体生成在贴墙的格心时
        // 会有一半身体插进掩体，看起来像"卡在墙里"。
        if (!isPositionWalkable(candidate, clearanceRadius)) {
            continue;
        }

        // 与玩家保持距离：否则敌人会在玩家脸上凭空出现
        if (vectorLength(candidate - playerPosition) < config_.minSpawnDistance) {
            continue;
        }

        // 与其他实体保持距离
        bool tooClose = false;
        for (const auto& entity : entities_) {
            if (entity->type() == EntityType::Obstacle) {
                continue;   // 障碍物已通过 isPositionWalkable 检查过
            }
            if (vectorLength(candidate - entity->position()) < config_.minSpawnDistance * 0.6f) {
                tooClose = true;
                break;
            }
        }
        if (tooClose) {
            continue;
        }

        return candidate;
    }

    // 拒绝采样失败：退化为在网格中心附近直接放置。
    // 宁可位置不理想也不要生成失败——沙盒里「敌人少了一个」比「位置怪」更难排查。
    //
    // 但仍要走一次膨胀修正：网格中心可能正落在掩体上（示例关卡里就有
    // 中央掩体群），直接返回会让实体半嵌在墙里。snapToWalkable 是螺旋
    // 搜索，中心不可站立时会自动向外找最近的可站立点。
    return snapToWalkable(config_.gridExtent * 0.5f, clearanceRadius);
}

Enemy* World::spawnEnemy(std::optional<glm::vec2> position) {
    EnemyConfig enemyConfig;
    enemyConfig.useEQS = config_.enemiesUseEQS;

    // 配置先于位置确定：生成点的膨胀检查要用 enemyConfig.radius，
    // 而它是"敌人身体有多大"的唯一真相来源。写死一个 11.0 会在有人
    // 调大敌人尺寸时静默失效——实体卡进墙里，而生成逻辑看起来毫无问题。
    glm::vec2 spawnPosition = position ? *position
                                       : findFreeSpawnPosition(64, enemyConfig.radius);

    // 每次生成都推进种子，让连续生成的敌人位置不同
    spawnSeed_ = spawnSeed_ * 1664525u + 1013904223u;

    const std::string name = "Enemy" + std::to_string(enemyCount_);

    auto enemy = std::make_unique<Enemy>(name, spawnPosition, enemyConfig);

    // 注入依赖：导航网格、视线查询、自身子弹速度。
    // 这里传入的是 World 自身的方法，因此 Enemy 能查询地形但不依赖 World 类型。
    enemy->setNavGrid(&navGrid_);
    enemy->setLineOfSightQuery(makeLineOfSightQuery());
    // 子弹速度用于计算开火提前量。注入而非写进 EnemyConfig，
    // 是为了让"改弹道参数"只有一处真相。
    enemy->setProjectileSpeed(config_.enemyProjectile.speed);

    Enemy* raw = enemy.get();
    entityIndex_[raw->id()] = raw;
    entities_.push_back(std::move(enemy));
    ++enemyCount_;
    spawnedAnyEnemy_ = true;

    return raw;
}

Obstacle* World::spawnObstacle(std::string name, glm::vec2 position, glm::vec2 size,
                               bool provideCover) {
    auto obstacle = std::make_unique<Obstacle>(std::move(name), position, size, provideCover);

    Obstacle* raw = obstacle.get();
    entityIndex_[raw->id()] = raw;
    entities_.push_back(std::move(obstacle));

    // 同步到导航网格
    const glm::vec2 half = size * 0.5f;
    navGrid_.fillRect(position - half, position + half, true);

    // 掩体位置记录下来供 EQS 使用
    if (provideCover) {
        coverPositions_.push_back(position);
    }

    return raw;
}

Projectile* World::spawnProjectile(uint32_t ownerId, Faction faction, glm::vec2 muzzle,
                                   glm::vec2 direction, const ProjectileConfig& config) {
    // 零方向（朝向还没初始化 / 参数写错）时拒绝生成。
    // 若放任它生成，子弹会原地不动、永远飞不完射程，最终堆满实体列表——
    // 一个"静止的子弹"会一直占着 0.0 的速度参与碰撞检测，是纯粹的负担。
    const float length = vectorLength(direction);
    if (length < 1e-4f) {
        return nullptr;
    }
    const glm::vec2 unit = direction / length;

    // 沿枪口前推，避免子弹生成在开枪者身体里、
    // 下一子步就判定"撞到自己的碰撞体"而当场消失。
    const glm::vec2 origin = muzzle + unit * config.muzzleOffset;

    auto projectile = std::make_unique<Projectile>(ownerId, faction, origin, unit, config);

    Projectile* raw = projectile.get();
    entityIndex_[raw->id()] = raw;
    entities_.push_back(std::move(projectile));

    return raw;
}

bool World::removeEntity(uint32_t id) {
    const auto it = entityIndex_.find(id);
    if (it == entityIndex_.end()) {
        return false;
    }

    Entity* target = it->second;

    // 【修复】类型必须在实体析构**之前**记下来。
    // entities_.erase 会析构对应的 unique_ptr，之后 target 就是悬垂指针，
    // 原实现在 erase 之后才读 target->type() —— 属于 use-after-free。
    const EntityType removedType = target->type();
    const glm::vec2  removedPosition = target->position();
    // 障碍物还需要尺寸才能把它从导航网格里解除阻挡（见下）
    const glm::vec2 removedSize =
        (removedType == EntityType::Obstacle)
            ? static_cast<const Obstacle*>(target)->size()
            : glm::vec2{0.0f, 0.0f};

    entityIndex_.erase(it);

    // 从主列表中移除（unique_ptr 在此析构）
    entities_.erase(
        std::remove_if(entities_.begin(), entities_.end(),
                       [target](const std::unique_ptr<Entity>& entity) {
                           return entity.get() == target;
                       }),
        entities_.end());

    // 若移除的是敌人，需要同步清理掩体缓存之外的引用
    if (removedType == EntityType::Enemy) {
        // 从待移除队列里清掉，避免后续重复处理
        pendingRemoval_.erase(
            std::remove_if(pendingRemoval_.begin(), pendingRemoval_.end(),
                           [id](const std::pair<uint32_t, double>& entry) {
                               return entry.first == id;
                           }),
            pendingRemoval_.end());
    }

    // 障碍物被移除时，它留下的所有"派生状态"都要同步清掉
    if (removedType == EntityType::Obstacle) {
        // 【修复】掩体列表：否则 EQS 会继续把「已经不存在的掩体」当成可躲位置。
        coverPositions_.erase(
            std::remove_if(coverPositions_.begin(), coverPositions_.end(),
                           [&removedPosition](const glm::vec2& cover) {
                               const float dx = cover.x - removedPosition.x;
                               const float dy = cover.y - removedPosition.y;
                               return (dx * dx + dy * dy) < 1e-4f;
                           }),
            coverPositions_.end());

        // 【修复】导航网格：spawnObstacle 用 fillRect(..., true) 把这块区域
        // 标成阻挡，但此前**没有任何地方把它改回可通行**。障碍被移除后于是
        // 留下一堵"看不见的墙"：AI 会绕它走、isPositionWalkable 判它不可
        // 站立、子弹会被它挡住（projectileBlockedAt 用的就是这个网格）。
        const glm::vec2 half = removedSize * 0.5f;
        navGrid_.fillRect(removedPosition - half, removedPosition + half, false);
        // 间距场是网格的派生数据，阻挡状态变了必须重算，
        // 否则它描述的还是一分钟前的地形。
        navGrid_.computeClearanceField();
    }

    return true;
}

void World::cleanupDeadEntities(double gameTime) {
    // 敌人死亡后保留 3 秒（让玩家看到击杀反馈），之后移除
    constexpr double kCorpseLifetime = 3.0;

    for (const auto& entity : entities_) {
        if (entity->type() != EntityType::Enemy || !entity->isDead()) {
            continue;
        }

        const uint32_t id = entity->id();
        const bool alreadyPending = std::any_of(
            pendingRemoval_.begin(), pendingRemoval_.end(),
            [id](const std::pair<uint32_t, double>& entry) { return entry.first == id; });

        if (!alreadyPending) {
            pendingRemoval_.emplace_back(id, gameTime);
            ++enemiesKilled_;
        }
    }

    // 回收超时的尸体
    //
    // 【修复】原实现在循环里调用 removeEntity()，而后者对敌人会同步删除
    // pendingRemoval_ 中的条目 —— 外层迭代器 it 当场失效，随后的
    // it = pendingRemoval_.erase(it) 是在失效迭代器上操作（UB），
    // 且同一条目被删两次。改为先把到期 ID 收集起来再统一处理。
    std::vector<uint32_t> expired;
    for (const auto& entry : pendingRemoval_) {
        if (gameTime - entry.second >= kCorpseLifetime) {
            expired.push_back(entry.first);
        }
    }

    for (const uint32_t id : expired) {
        // removeEntity 内部已经把该条目从 pendingRemoval_ 中移除，
        // 这里只需要关心实体本身的析构。
        removeEntity(id);
    }
}

// ===========================================================================
//  导航与查询
// ===========================================================================
AI::LineOfSightQuery World::makeLineOfSightQuery() const {
    // 捕获 this —— Enemy 的生命周期短于 World，且 World 在 Enemy 存活期间不会移动，
    // 因此该捕获是安全的。
    return [this](glm::vec2 from, glm::vec2 to) -> bool {
        // 【精确几何，而不是格子膨胀】
        //
        // 原实现是"沿线段每半格采样、问格子可不可通行"。问题在于阻挡格是
        // 被膨胀过的：障碍压住格子一角，整格就变成阻挡（NavGrid::fillRect），
        // 于是每个掩体的阻挡范围比它看起来的样子向外多出 0~32 单位。
        // 后果是"贴墙就隐身、子弹也在离墙一格远的地方消失"——
        // 实测：墙面在 x=700、玩家贴在 x=688 时，敌人完全看不见他。
        //
        // 现在直接问"这条线段有没有穿过障碍矩形"（几何真相）。
        // 顺带也免疫了"障碍被移除后网格没清干净"这类幽灵墙问题：
        // 判定读的是活着的实体，不是派生数据。
        for (const auto& entity : entities_) {
            if (entity->type() != EntityType::Obstacle) {
                continue;
            }

            const auto* obstacle = static_cast<const Obstacle*>(entity.get());
            const glm::vec2 half = obstacle->size() * 0.5f;
            const glm::vec2 center = obstacle->position();

            if (segmentIntersectsRect(from, to, center - half, center + half)) {
                return false;
            }
        }
        return true;
    };
}

bool World::isPositionWalkable(glm::vec2 position, float entityRadius) const {
    // 【判定逻辑已收敛到 NavGrid::canStand】
    //
    // "这点能不能站"原来在这里实现了一份，AI 层（巡逻点选择、EQS 候选
    // 筛选）又各自实现了一份"格心可通行就算能站"的简化版——两把尺子必然
    // 打架：简化版会用 worldToGrid 的**钳制**把地图外的点判成可通行，
    // 于是 AI 的目标点落到地图外、永远走不到，敌人被边界顶住卡死
    // （这正是右下方那个卡住的敌人的成因）。
    //
    // 现在只有 canStand 一处实现：圆 vs 阻挡格 AABB + 越界即阻挡。
    // 它同时也解释了"为什么半径伸出地图外也必须判不可站"。
    return navGrid_.canStand(position, entityRadius);

    return true;
}

glm::vec2 World::snapToWalkable(glm::vec2 position, float entityRadius) const {
    if (!navGrid_.isInitialized() || isPositionWalkable(position, entityRadius)) {
        return position;
    }

    // 螺旋式向外搜索最近的可站立点
    const AI::GridCoord center = navGrid_.worldToGrid(position);
    constexpr int kMaxRadius = 12;

    for (int radius = 1; radius <= kMaxRadius; ++radius) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                // 只检查环上的点（跳过内部，已在更小半径检查过）
                if (std::abs(dx) != radius && std::abs(dy) != radius) {
                    continue;
                }
                const AI::GridCoord probe{center.x + dx, center.y + dy};
                if (!navGrid_.inBounds(probe)) {
                    continue;
                }
                const glm::vec2 world = navGrid_.gridToWorld(probe);
                if (isPositionWalkable(world, entityRadius)) {
                    return world;
                }
            }
        }
    }

    return position;
}

// ===========================================================================
//  主更新
// ===========================================================================
void World::update(double fixedDelta, double gameTime) {
    if (!initialized_) {
        return;
    }

    if (rebaseTimePending_) {
        // 重开后（或首次）推进的第一步：把当前时钟记为本局起点，
        // 这样 matchElapsedTime() 从 0 开始增长。
        matchStartGameTime_ = gameTime;
        rebaseTimePending_  = false;
    }
    elapsedGameTime_ = gameTime;

    // 上一帧的伤害事件在这里清空。UI 在 render 阶段读取，
    // 因此事件的生命周期恰好是「一个逻辑步到下一个逻辑步」。
    damageEvents_.clear();

    // 弹着残影按时间过期清理。
    // 与伤害事件不同，残影要跨越多步存活（kImpactMarkLifetime ≈ 0.11s，
    // 约 7 个逻辑步），否则在 60Hz 下只闪一帧，肉眼根本看不见。
    //
    // 用 erase-remove 惯用法而非 std::erase_if：后者对 vector 的重载是
    // C++20 才有的，而本项目锁在 C++17。
    if (!impactMarks_.empty()) {
        const double cutoff = gameTime - kImpactMarkLifetime;
        impactMarks_.erase(
            std::remove_if(impactMarks_.begin(), impactMarks_.end(),
                           [cutoff](const ImpactMark& mark) {
                               return mark.gameTime < cutoff;
                           }),
            impactMarks_.end());
    }

    // -----------------------------------------------------------------------
    //  1. 收集 AI 所需的输入数据（一次遍历，多个系统共享）
    // -----------------------------------------------------------------------
    collectPerceptionTargets();
    collectThreatSources();
    // 小队情报也在这里广播：它必须在敌人 AI 之前完成，
    // 否则这一步的行为树读到的还是上一帧的情报。
    broadcastSquadIntel(gameTime);

    // -----------------------------------------------------------------------
    //  2. 驱动所有敌人 AI
    // -----------------------------------------------------------------------
    const auto aiStart = std::chrono::steady_clock::now();

    if (!aiEnabled_) {
        // AI 冻结（调试用）：不跑行为树 / 感知 / EQS，
        // 同时清零敌人速度，避免它们带着冻结前的速度继续滑行。
        //
        // 【关键】还必须清掉「本步意图」标志：firedThisStep_ 的唯一清零
        // 点在 Enemy::updateAI 里，跳过 AI 之后它会一直停留在冻结前的值，
        // 于是下面的 processWeaponFire 每步都替它发射一颗子弹——"冻结"
        // 会变成"无限机枪"，而且因为是绕过行为树，连冷却都不生效。
        for (const auto& entity : entities_) {
            if (entity->type() == EntityType::Enemy) {
                auto* enemy = static_cast<Enemy*>(entity.get());
                enemy->clearStepIntent();
                enemy->setVelocity(glm::vec2{0.0f, 0.0f});
            }
        }
    } else {
        for (const auto& entity : entities_) {
            if (entity->type() != EntityType::Enemy) {
                continue;
            }
            auto* enemy = static_cast<Enemy*>(entity.get());

            // 把威胁源与掩体列表写入黑板（EQS 的规避 / 掩护测试分别读取）
            enemy->setThreatSources(threatSources_);
            enemy->setCoverPositions(coverPositions_);

            // 每个敌人只感知「敌对阵营」的目标
            enemy->updateAI(fixedDelta, gameTime, perceptionTargets_);
        }
    }

    const auto aiEnd = std::chrono::steady_clock::now();
    totalAITimeMs_ += std::chrono::duration<double, std::milli>(aiEnd - aiStart).count();
    ++aiUpdateCount_;

    // -----------------------------------------------------------------------
    //  3. 开火（把「想开火」变成一颗真的子弹）
    // -----------------------------------------------------------------------
    //
    // 顺序不能反：AI 在阶段 2 里决定了"谁要开火"，这里才把它变成实体。
    processWeaponFire(gameTime);

    // -----------------------------------------------------------------------
    //  4. 子弹飞行与命中
    // -----------------------------------------------------------------------
    //
    // 这一步是"攻击有实体"的核心。子弹按自己的速度一格一格往前飞，
    // 每一小段都检查是否撞上地形或目标，命中在这里才结算伤害——
    // 也就是说，伤害发生在"子弹到达"的那一刻，而不是"扣扳机"的那一刻。
    updateProjectiles(fixedDelta, gameTime);

    // -----------------------------------------------------------------------
    //  5. 回收子弹并留下弹着残影
    // -----------------------------------------------------------------------
    collectTerminatedProjectiles(gameTime);

    // -----------------------------------------------------------------------
    //  6. 物理更新 + 边界约束
    // -----------------------------------------------------------------------
    for (const auto& entity : entities_) {
        // 静态实体不需要更新；子弹的位移已经在阶段 4 里逐段算过了，
        // 若再走一次通用积分就等于一步走两倍距离（并且会跳掉碰撞检测）。
        if (entity->type() == EntityType::Obstacle ||
            entity->type() == EntityType::Projectile) {
            continue;
        }
        entity->update(fixedDelta);
    }

    resolveMovement(fixedDelta);

    // -----------------------------------------------------------------------
    //  7. 回收死亡实体
    // -----------------------------------------------------------------------
    cleanupDeadEntities(gameTime);

    // -----------------------------------------------------------------------
    //  8. 结算用时的锁定
    // -----------------------------------------------------------------------
    // 一旦分出胜负，就把"当时的本局用时"记下来：之后世界还会继续推进
    // （敌人退巡逻、子弹落地、尸体消失），但结算横幅上的数字不该再涨。
    // 回到 Running（例如胜利后按 N 增兵）时解除锁定，重新开始计时。
    const MatchOutcome outcomeNow = outcome();
    if (outcomeNow != MatchOutcome::Running && lastOutcome_ == MatchOutcome::Running) {
        outcomeSettledTime_ = matchElapsedTime();
    }
    lastOutcome_ = outcomeNow;
}

// ---------------------------------------------------------------------------
//  数据收集
// ---------------------------------------------------------------------------
void World::collectPerceptionTargets() {
    perceptionTargets_.clear();

    for (const auto& entity : entities_) {
        if (entity->type() != EntityType::Player) {
            continue;
        }

        AI::PerceptTarget target;
        target.id       = entity->id();
        target.position = entity->position();
        target.radius   = entity->radius();
        target.alive    = !entity->isDead();

        // 玩家的威胁权重最高——AI 优先关注玩家而不是其他敌人
        target.threat = entity->hasTag(EntityTag::HighValue) ? 1.0f : 0.6f;

        if (target.alive) {
            perceptionTargets_.push_back(target);
        }
    }

    // 注：这里只把玩家作为 AI 的感知目标。
    // 若要让敌人之间互相感知（友军伤害、阵型保持），在此追加即可。
    //
    // 另外注意：敌人之间的**情报共享**（"队友最后看到目标在哪"）不走感知
    // 列表，而是由 broadcastSquadIntel() 汇总后写进黑板。两者的区别很重要：
    // 感知列表 = "我现在看得见谁"（可开火），情报 = "别人说他在那儿"
    // （只能拿来搜索，不能拿来开枪）。
}

void World::broadcastSquadIntel(double gameTime) {
    // 汇总全队"最后已知目标位置"，取**最新鲜**的一条广播出去。
    //
    // 【为什么由 World 来做，而不是敌人互相查询】
    // 1. 敌人之间没有互相引用（保持个体解耦），World 是唯一能看到所有人的地方；
    // 2. 只有集中广播才能保证"同一份情报、同一时刻"到达所有人。若让敌人
    //    互相查询，谁先更新谁后更新会取决于遍历顺序，行为变得不可复现
    //    （而本项目对确定性有明确要求）。
    //
    // 【语义边界】广播的是"某个队友最后看到目标在哪"，不是实时坐标，
    // 更不是共享视野：收到情报的敌人依然要靠自己的感知才能进入交战分支。
    const Enemy* spotter = nullptr;
    double newestSeen = -1e9;

    for (const auto& entity : entities_) {
        if (entity->type() != EntityType::Enemy) {
            continue;
        }
        // 【刻意不跳过阵亡者】"他倒下前最后看到目标在哪"仍然是最有价值的一条
        // 情报。跳过阵亡者会让情报在报告者被击杀的瞬间消失 —— 而那一刻正是
        // 队友最需要知道敌人从哪个方向来的时候。情报本来就带时间戳，
        // 过期会自然失效（见下方 publishable），不需要靠"人还活着"来保证新鲜。
        const auto* enemy = static_cast<const Enemy*>(entity.get());
        if (!enemy->hasLastKnownPosition()) {
            continue;
        }
        const double seenTime = enemy->lastSeenEnemyTime();
        if (seenTime > newestSeen) {
            newestSeen = seenTime;
            spotter = enemy;
        }
    }

    // 情报有效期取报告者自己的档位（默认 12 秒）。超期的情报不再广播，
    // 并且会从黑板里清掉——否则一次偶遇会让全队永远朝一个旧坐标搜索。
    const bool publishable =
        spotter != nullptr &&
        (gameTime - newestSeen) <= spotter->config().squadIntelMemory;

    for (const auto& entity : entities_) {
        if (entity->type() != EntityType::Enemy) {
            continue;
        }
        auto* enemy = static_cast<Enemy*>(entity.get());
        if (publishable) {
            enemy->setSquadIntel(spotter->lastKnownEnemyPosition(), newestSeen);
        } else {
            enemy->clearSquadIntel();
        }
    }
}

void World::collectThreatSources() {
    threatSources_.clear();
    coverPositions_.clear();

    // 威胁源与掩体是两种完全不同的东西，早期版本把二者塞进同一个列表，
    // 导致 EQS 的「规避威胁」与「靠近掩体」两项测试互相打架，评分全面
    // 低于阈值、候选点被清空，AI 永远选不出位置。这里彻底拆开：
    //
    //   threatSources_  = 玩家（唯一的敌对火力来源）
    //   coverPositions_ = 可作为掩体的障碍物
    //
    // 注意不要把自己的位置或友军放进 threatSources_：
    // 那会让「远离威胁」变成「远离自己」，任何候选点都拿不到分数。
    for (const auto& entity : entities_) {
        if (entity->isDead()) {
            continue;
        }

        if (entity->type() == EntityType::Player) {
            threatSources_.push_back(entity->position());
        } else if (entity->type() == EntityType::Obstacle) {
            const auto* obstacle = static_cast<const Obstacle*>(entity.get());
            if (obstacle->providesCover()) {
                coverPositions_.push_back(entity->position());
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  开火（产生子弹）
// ---------------------------------------------------------------------------
void World::processWeaponFire(double gameTime) {
    // --- 玩家开火 ---
    Player* playerEntity = player();

    // tryConsumeFireRequest 同时承担三件事：取走请求、检查冷却、累加射击计数。
    // 之所以把它们收在一个出口，是因为旧版本把它们拆在两处
    // （Player::onUpdate 与 World::resolveCombat），结果是玩家的射击计数
    // 永远是 0——两个地方都在"消费同一个请求"，先到的那个把它吃掉了。
    if (playerEntity && !playerEntity->isDead() &&
        playerEntity->tryConsumeFireRequest(gameTime)) {
        // 同样只在真的生成出子弹时计数，保持"开火次数 == 子弹数"这条不变式
        // （零朝向时 spawnProjectile 会拒绝，此时不该记为一次开火）。
        if (spawnProjectile(playerEntity->id(), Faction::Player,
                            playerEntity->position(),
                            playerEntity->facing(),
                            config_.playerProjectile) != nullptr) {
            ++totalShotsFired_;
        }
    }

    // --- 敌人开火 ---
    //
    // 注意这里**完全不碰玩家**：不查视线、不做命中判定、不扣血。
    // 它只负责"把子弹放到世界"，子弹之后打在谁身上由 updateProjectiles 决定。
    // 旧版本在这里直接 applyDamage，命中在扣扳机的一瞬间就定了，
    // 于是掩体挡不住子弹、走位也躲不开——那是"一条线"而不是"一个实体"。
    //
    // 【为什么分两段：先收集、再发射】
    // spawnProjectile 会往 entities_ 里 push_back，而 entities_ 没有
    // reserve 过——一旦这一步的 push 触发扩容，正在进行的 range-for 所缓存的
    // 迭代器与引用全部失效（读已释放内存，UB）。交火越密集越容易命中，
    // 表现为随机崩溃或"这一步某个敌人莫名没开火"。
    //
    // 因此：先在只读遍历中把开火意图收集到局部数组，遍历结束后再统一发射。
    // 这也让"步进期间不得增删实体"这条不变量在代码结构上成立。
    // 暂存区用成员而不是每步临时构造：processWeaponFire 每逻辑步都跑，
    // 局部 vector + reserve 会变成"每步一次堆分配"的固定开销。
    pendingShots_.clear();

    for (const auto& entity : entities_) {
        if (entity->type() != EntityType::Enemy || entity->isDead()) {
            continue;
        }
        auto* enemy = static_cast<Enemy*>(entity.get());

        if (!enemy->firedThisStep()) {
            continue;
        }

        ProjectileConfig config = config_.enemyProjectile;
        // 单发伤害属于 AI 难度档位，由每个敌人自己的配置决定
        config.damage = enemy->config().damage;

        pendingShots_.push_back(PendingShot{enemy->id(), enemy->position(),
                                            enemy->facing(), config});
    }

    for (const PendingShot& shot : pendingShots_) {
        // 只有真的生成出子弹才计数：spawnProjectile 在零方向时会拒绝，
        // 那种情况下"开枪了但没有子弹"是自相矛盾的统计。
        if (spawnProjectile(shot.ownerId, Faction::Enemy,
                            shot.muzzle, shot.direction, shot.config) != nullptr) {
            ++totalShotsFired_;
        }
    }
}

// ---------------------------------------------------------------------------
//  子弹飞行与命中
// ---------------------------------------------------------------------------
bool World::projectileBlockedAt(glm::vec2 point) const {
    if (!navGrid_.isInitialized()) {
        return false;
    }

    // 出界视为撞墙。navGrid_.worldToGrid 会把越界坐标钳制到边界格，
    // 所以不能只依赖格子判定——必须先判断是否在网格内，
    // 否则飞出地图的子弹会被判定为"落在边界格上"而永远不消失。
    if (!navGrid_.contains(point)) {
        return true;
    }

    // 【精确几何，而不是格子膨胀】
    //
    // 原实现是 `!isWalkable(worldToGrid(point))`。但阻挡格是膨胀过的近似
    // （障碍压住格子一角就整格阻挡），于是子弹会在**离墙面最多 32 单位**
    // 的地方就被吃掉 —— 贴着墙的目标因此打不中（实测：目标贴在墙面
    // 12 单位处时子弹完全打不到它），配合视线问题合起来就是"靠墙无敌"。
    //
    // 现在直接问"点是否落在某个障碍矩形内"。子弹子步长 ≤ 6 单位、
    // 而掩体厚度 ≥ 40 单位，因此逐点判定不会漏检（穿不出去）。
    for (const auto& entity : entities_) {
        if (entity->type() != EntityType::Obstacle) {
            continue;
        }
        if (static_cast<const Obstacle*>(entity.get())->containsPoint(point)) {
            return true;
        }
    }

    return false;
}

Entity* World::findProjectileVictim(const Projectile& projectile) const {
    // 只打敌对阵营：玩家子弹打敌人，敌人子弹打玩家。
    const EntityType wanted = (projectile.faction() == Faction::Player)
                                  ? EntityType::Enemy
                                  : EntityType::Player;

    const glm::vec2 position = projectile.position();
    const float reach = projectile.radius();

    Entity* best = nullptr;
    float bestOverlap = 0.0f;

    for (const auto& entity : entities_) {
        if (entity->type() != wanted || entity->isDead()) {
            continue;
        }
        // 自己打不中自己。正常流程下子弹已经前推了 muzzleOffset，
        // 但这条判断是廉价的保险：万一将来有人把偏移改成 0，
        // 也不会出现"一开枪就把自己打死"。
        if (entity->id() == projectile.ownerId()) {
            continue;
        }

        const glm::vec2 delta = entity->position() - position;
        const float distance = vectorLength(delta);
        const float contact = entity->radius() + reach;

        if (distance > contact) {
            continue;
        }

        // 同一子步里可能有多个目标重叠，取"重叠最深"的那个：
        // 它才是子弹真正扎进去的对象。
        const float overlap = contact - distance;
        if (best == nullptr || overlap > bestOverlap) {
            best = entity.get();
            bestOverlap = overlap;
        }
    }

    return best;
}

void World::updateProjectiles(double fixedDelta, double gameTime) {
    const float delta = static_cast<float>(fixedDelta);
    if (delta <= 0.0f) {
        return;
    }

    for (const auto& entity : entities_) {
        if (entity->type() != EntityType::Projectile) {
            continue;
        }
        auto* projectile = static_cast<Projectile*>(entity.get());
        if (projectile->terminated() || projectile->isDead()) {
            continue;
        }

        const float stepDistance = projectile->config().speed * delta;
        if (stepDistance <= 0.0f) {
            continue;
        }

        // 【子步进】把这一步的位移切成若干小段，逐段推进并检查碰撞。
        //
        // 为什么不能一步走完：60Hz 下子弹每步位移约 13.7 个世界单位，
        // 而敌人的碰撞半径只有 11。一步跨过去完全可能"骑"在目标两侧，
        // 检测时既不在目标内、也没经过目标——子弹就这样穿过去了。
        // 切成 6 单位一段后，单段位移远小于最小目标直径，不可能漏检。
        const int substeps = std::max(
            1, static_cast<int>(std::ceil(stepDistance / kProjectileMaxSubstep)));
        const float segment = stepDistance / static_cast<float>(substeps);
        const glm::vec2 increment = projectile->direction() * segment;

        for (int step = 0; step < substeps; ++step) {
            const glm::vec2 next = projectile->position() + increment;

            // --- 1. 地形（墙 / 掩体 / 出界） ---
            // 放在目标判定之前：掩体要能真的挡子弹。
            if (projectileBlockedAt(next)) {
                projectile->setPosition(next);
                projectile->markHitTerrain();
                break;
            }

            projectile->setPosition(next);

            // --- 2. 目标 ---
            if (Entity* victim = findProjectileVictim(*projectile)) {
                // 先问一句"这一下算不算"。命中无敌帧的目标时 dealt 为 0，
                // 但子弹照样被吸收（markHitEntity 照常调用）——见 applyDamage
                // 里关于"为什么不让子弹穿过去"的说明。
                const bool wasInvulnerable = victim->isInvulnerable();

                const float dealt = applyDamage(victim->id(), projectile->damage(),
                                                projectile->ownerId(), next, gameTime);
                projectile->markHitEntity(victim->id(), dealt);

                if (dealt > 0.0f) {
                    totalDamageDealt_ += dealt;
                    ++totalProjectileHits_;

                    // 把伤害记到开枪者账上，供 HUD 显示"谁打出了多少伤害"。
                    // 开枪者可能已经在飞行途中死了（子弹是独立实体，不会
                    // 随主人一起消失），此时 findEntity 返回 nullptr，
                    // 跳过计账即可——不影响伤害本身已经结算的事实。
                    if (Entity* attacker = findEntity(projectile->ownerId())) {
                        if (attacker->type() == EntityType::Player) {
                            // 【修复】不再截断为 int：Player 内部改为浮点累加，
                            // 与 Enemy 口径一致（详见 Player::damageDealt）。
                            static_cast<Player*>(attacker)->addDamageDealt(dealt);
                        } else if (attacker->type() == EntityType::Enemy) {
                            static_cast<Enemy*>(attacker)->addDamageDealt(dealt);
                        }
                    }
                } else if (wasInvulnerable) {
                    // 打中了，但对方在无敌帧里。
                    //
                    // 【为什么要单独计数】
                    // 无敌帧是"看不见的规则"：如果不留下任何痕迹，玩家没法确认
                    // 自己躲掉了，开发者也没法确认它真的生效——只能读代码相信。
                    // 这个计数让"翻滚躲开子弹"变成 HUD 上会跳动的数字，
                    // 和"命中 26/26"一样，是可以当场验证的证据。
                    ++totalProjectilesDodged_;
                }
                break;
            }

            // --- 3. 射程 ---
            if (projectile->advance(segment)) {
                projectile->markExpired();
                break;
            }
        }
    }
}

void World::collectTerminatedProjectiles(double gameTime) {
    for (auto it = entities_.begin(); it != entities_.end();) {
        Entity* entity = it->get();
        if (entity->type() != EntityType::Projectile) {
            ++it;
            continue;
        }

        auto* projectile = static_cast<Projectile*>(entity);
        if (!projectile->terminated()) {
            ++it;
            continue;
        }

        // 留下弹着残影：回答"这一枪是从哪打来的"。
        // 注意起点用的是 launchPoint（出膛点）而不是"当前可见子弹位置"——
        // 子弹已经消失了，只有出膛点能给出方向。
        addImpactMark(projectile->launchPoint(), projectile->impactPoint(),
                      projectile->isPlayerShot(), projectile->hitEntity(),
                      projectile->ownerId(), gameTime);

        entityIndex_.erase(projectile->id());
        it = entities_.erase(it);
    }
}

void World::addImpactMark(glm::vec2 origin, glm::vec2 impact, bool isPlayerShot,
                          bool hit, uint32_t attackerId, double gameTime) {
    impactMarks_.push_back(
        ImpactMark{origin, impact, gameTime, isPlayerShot, hit, attackerId});
}

/// 弹道长度：残影绘制时用来判断这发是不是远距离空枪。
float ImpactMark::length() const noexcept {
    const glm::vec2 delta = impact - origin;
    return std::sqrt(delta.x * delta.x + delta.y * delta.y);
}

// ---------------------------------------------------------------------------
//  移动与约束
// ---------------------------------------------------------------------------
void World::resolveMovement(double fixedDelta) {
    (void)fixedDelta;

    for (const auto& entity : entities_) {
        // 障碍物是静态的；子弹不走"碰撞后弹开"这套逻辑——
        // 它撞上东西就该消失（由 updateProjectiles 处理）。
        // 如果让子弹也进 constrainEntity，子弹会被墙"推"着走，
        // 表现为沿着墙面滑行并拐弯，非常诡异。
        if (entity->type() == EntityType::Obstacle ||
            entity->type() == EntityType::Projectile) {
            continue;
        }
        constrainEntity(*entity);
    }
}

void World::constrainEntity(Entity& entity) const {
    // --- 1. 网格边界 ---
    //
    // 【边界取"导航网格实际覆盖的范围"，而不是 config_.gridExtent】
    //
    // 网格行列数是整除出来的：gridExtent.y = 1200、cellSize = 32 时只有
    // 37 行，覆盖到 y = 1184。如果这里用 1200 做钳制，实体会被允许走进
    // [1184, 1200) 这段"没有导航数据的地带"：那里 isPositionWalkable 一律
    // 为假（越界）、也不与任何障碍物矩形相交，于是 AI 会贴在那里不动，
    // 表现为"莫名停摆在边界外"。实测 60 秒无头运行里就有一个敌人
    // 停在 y = 1189 —— 正是这一段。
    glm::vec2 extent = config_.gridExtent;
    if (navGrid_.isInitialized()) {
        extent = navGrid_.origin()
               + glm::vec2{static_cast<float>(navGrid_.columns()) * navGrid_.cellSize(),
                           static_cast<float>(navGrid_.rows()) * navGrid_.cellSize()};
    }
    const float radius = entity.radius();

    glm::vec2 position = entity.position();

    // 用「撞墙反弹一半」而非硬钳制：直接钳制会让实体贴着墙卡住不动，
    // 反弹一半则让它自然滑开，手感更像有物理。
    if (position.x < radius) {
        position.x = radius;
        entity.setVelocity({-entity.velocity().x * 0.5f, entity.velocity().y});
    } else if (position.x > extent.x - radius) {
        position.x = extent.x - radius;
        entity.setVelocity({-entity.velocity().x * 0.5f, entity.velocity().y});
    }

    if (position.y < radius) {
        position.y = radius;
        entity.setVelocity({entity.velocity().x, -entity.velocity().y * 0.5f});
    } else if (position.y > extent.y - radius) {
        position.y = extent.y - radius;
        entity.setVelocity({entity.velocity().x, -entity.velocity().y * 0.5f});
    }

    // --- 2. 障碍物推出 ---
    // 圆形实体 vs 轴对齐矩形：求矩形上离实体最近的点，
    // 若距离小于半径则沿该方向推出。这是最简且足够稳定的碰撞解算。
    for (const auto& other : entities_) {
        if (other->type() != EntityType::Obstacle) {
            continue;
        }
        const auto* obstacle = static_cast<const Obstacle*>(other.get());

        const glm::vec2 half = obstacle->size() * 0.5f;
        const glm::vec2 center = obstacle->position();

        // 最近点（钳制到矩形范围）
        const glm::vec2 closest{
            std::clamp(position.x, center.x - half.x, center.x + half.x),
            std::clamp(position.y, center.y - half.y, center.y + half.y)
        };

        const glm::vec2 offset = position - closest;
        const float distance = vectorLength(offset);

        if (distance >= radius) {
            continue;   // 没相交
        }

        // 推出方向必须是**单位**法线，而且要和实际推出的轴一致 ——
        // 后面用它来抵消朝向墙面的速度分量。
        glm::vec2 normal{0.0f, 0.0f};

        if (distance < 1e-4f) {
            // 实体中心恰好落在矩形内部：沿最短轴推出。
            // 这种情况发生在生成位置不当时，必须有确定性的处理方式。
            //
            // 【修复】原实现算完推出位置后，法线却回退成
            // `position - center`（模长 = 半宽/高 + 半径，长墙可达数百），
            // 于是 intoWall ≈ 800·|v|、修正量 ≈ 6.4e5·|v| —— 速度被放大
            // 两三个数量级，表现为"贴着墙抖动/直接被弹飞"。
            // 现在按推出轴给单位法线。
            const float dxLeft   = position.x - (center.x - half.x);
            const float dxRight  = (center.x + half.x) - position.x;
            const float dyBottom = position.y - (center.y - half.y);
            const float dyTop    = (center.y + half.y) - position.y;

            // 取最小穿透量所在的轴（用"比较取最小"而不是浮点相等判断，
            // 避免相等比较在边界上行为不确定）。
            enum class Axis { Left, Right, Bottom, Top };
            Axis axis = Axis::Left;
            float minPenetration = dxLeft;
            if (dxRight < minPenetration)  { minPenetration = dxRight;  axis = Axis::Right; }
            if (dyBottom < minPenetration) { minPenetration = dyBottom; axis = Axis::Bottom; }
            if (dyTop < minPenetration)    { minPenetration = dyTop;    axis = Axis::Top; }

            switch (axis) {
                case Axis::Left:
                    position.x = center.x - half.x - radius;
                    normal = {-1.0f, 0.0f};
                    break;
                case Axis::Right:
                    position.x = center.x + half.x + radius;
                    normal = {1.0f, 0.0f};
                    break;
                case Axis::Bottom:
                    position.y = center.y - half.y - radius;
                    normal = {0.0f, -1.0f};
                    break;
                case Axis::Top:
                    position.y = center.y + half.y + radius;
                    normal = {0.0f, 1.0f};
                    break;
            }
        } else {
            // 沿法线方向推出到刚好接触
            normal = offset / distance;
            position = closest + normal * radius;
        }

        // 抵消朝向障碍物的速度分量，让实体沿墙面滑动而不是被反复推回
        const glm::vec2 velocity = entity.velocity();
        const float intoWall = velocity.x * normal.x + velocity.y * normal.y;
        if (intoWall < 0.0f) {
            entity.setVelocity(velocity - normal * intoWall);
        }
    }

    entity.setPosition(position);
}

// ---------------------------------------------------------------------------
//  伤害
// ---------------------------------------------------------------------------
float World::applyDamage(uint32_t targetId, float amount, uint32_t attackerId,
                         glm::vec2 impactPoint, double gameTime) {
    Entity* target = findEntity(targetId);
    if (!target || target->isDead()) {
        return 0.0f;
    }

    // 无敌帧：在"伤害即将生效"这一层被挡住。
    //
    // 【为什么不在子弹那一层判断】
    // 子弹只负责"撞到了谁"，不负责"撞了算不算"。把免疫放在这里，将来任何一种
    // 新伤害来源（近战、陷阱、AOE）都自动获得同样的行为；放在子弹里则每加一种
    // 来源都要记得补一次，漏掉一处就是"翻滚有时躲得掉有时躲不掉"。
    //
    // 返回值 0 表示"没有造成伤害"。调用方（updateProjectiles）会据此不计入命中，
    // 但子弹**仍然会被吸收**——它确实打到了身体，只是这一下没造成伤害。
    // 让子弹穿过去反而更糟：它会在无敌结束后从背后继续飞，变成一种诡异的
    // "延迟命中"。
    if (target->isInvulnerable()) {
        return 0.0f;
    }

    const float dealt = target->applyDamage(amount);
    if (dealt <= 0.0f) {
        return 0.0f;
    }

    DamageEvent event;
    event.attackerId = attackerId;
    event.targetId   = targetId;
    event.amount     = dealt;
    event.impactPoint = impactPoint;
    event.gameTime   = gameTime;
    // 【清理】原先对同一个 ID 调用了两次 findEntity（两次哈希查找），
    // 取一次指针即可。
    const Entity* attacker = (attackerId != 0) ? findEntity(attackerId) : nullptr;
    event.isPlayerAttack = (attacker != nullptr && attacker->type() == EntityType::Player);

    damageEvents_.push_back(event);
    return dealt;
}

// ---------------------------------------------------------------------------
//  统计与调试
// ---------------------------------------------------------------------------
double World::averageAITimeMs() const noexcept {
    if (aiUpdateCount_ == 0) {
        return 0.0;
    }
    return totalAITimeMs_ / static_cast<double>(aiUpdateCount_);
}

std::string World::debugString() const {
    char buffer[384];
    const Player* playerEntity = player();

    // bullets = 当前在空中的子弹数。它随交火实时变化，是"攻击是实体"
    // 最直接的证据：如果这个数恒为 0 而 shots 在涨，说明子弹一生成就没了。
    std::snprintf(buffer, sizeof(buffer),
                  "entities=%zu enemies=%zu/%zu t=%.1fs shots=%zu bullets=%zu hit=%zu "
                  "dodged=%zu dmg=%.0f kills=%d aiAvg=%.3fms",
                  entities_.size(),
                  aliveEnemyCount(), enemyCount_,
                  elapsedGameTime_,
                  totalShotsFired_,
                  projectileCount(),
                  totalProjectileHits_,
                  totalProjectilesDodged_,
                  static_cast<double>(totalDamageDealt_),
                  enemiesKilled_,
                  averageAITimeMs());

    std::string result(buffer);
    if (playerEntity) {
        result += "\n";
        result += playerEntity->debugString();
    }
    return result;
}

std::string World::dump() const {
    std::ostringstream stream;
    stream << "=== World ===\n";
    stream << navGrid_.debugSummary() << "\n";

    // 子弹是高频生灭的短暂对象，逐颗列出来会把 F3 的输出刷成一片噪声，
    // 反而盖住了真正需要看的东西（敌人状态）。这里只给一行汇总。
    const size_t bullets = projectileCount();
    stream << "实体列表 (" << entities_.size() << "，含 " << bullets << " 颗飞行中的子弹):\n";

    for (const auto& entity : entities_) {
        // 同样理由：跳过子弹本体
        if (entity->type() == EntityType::Projectile) {
            continue;
        }
        stream << "  " << entity->debugString() << "\n";
    }

    if (bullets > 0) {
        stream << "\n飞行中的子弹 (" << bullets << "):\n";
        for (const auto& entity : entities_) {
            if (entity->type() != EntityType::Projectile) {
                continue;
            }
            stream << "  " << entity->debugString() << "\n";
        }
    }

    stream << "\n敌人 AI 状态:\n";
    for (const auto& entity : entities_) {
        if (entity->type() != EntityType::Enemy) {
            continue;
        }
        const auto* enemy = static_cast<const Enemy*>(entity.get());
        stream << "  " << enemy->name() << ": " << enemy->aiDebugString() << "\n";
    }

    return stream.str();
}

} // namespace TacticalAI::Game
