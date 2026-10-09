// ===========================================================================
//  src/game/World.h
//
//  游戏世界。所有实体的容器 + 各系统之间的协调者。
//
//  职责边界（很重要，为了保持分层清晰）：
//    World 做  : 拥有实体、维护导航网格、收集感知目标、结算伤害、处理生成/回收
//    World 不做: 渲染（debug 层负责）、输入解释（main 层负责）、
//                AI 决策本身（Enemy 自己负责）
//
//  实体所有权：World 用 unique_ptr 持有所有实体。
//  外部只拿 uint32_t ID 或原始指针（短期使用）。
//  这样「实体在遍历中被销毁」这类问题不会导致悬垂指针崩溃。
// ===========================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ai/Pathfinding.h"
#include "game/Enemy.h"
#include "game/Entity.h"
#include "game/Player.h"
#include "game/Projectile.h"

namespace TacticalAI::Game {

/// 沙盒关卡参数。集中管理，便于做「场景切换」。
struct WorldConfig {
    /// 导航网格覆盖范围（世界单位，从原点开始）。
    glm::vec2 gridExtent{1600.0f, 1200.0f};
    float     cellSize = 32.0f;

    /// 初始生成的敌人数量。
    int  initialEnemyCount = 3;

    /// 敌人之间的最小生成间距（避免叠在一起）。
    float minSpawnDistance = 220.0f;

    /// 玩家初始位置。
    glm::vec2 playerStart{300.0f, 600.0f};

    /// 是否在初始化时生成示例障碍物。
    bool generateDemoObstacles = true;

    /// 敌人是否使用 EQS 选位（关闭可对比行为差异）。
    bool enemiesUseEQS = true;

    /// 玩家子弹参数。
    ProjectileConfig playerProjectile = defaultPlayerProjectile();

    /// 敌人子弹参数。
    /// 注意 damage 字段会被每个敌人自己的 EnemyConfig::damage 覆盖——
    /// "单个敌人打多疼"属于 AI 难度档位的范畴，不该由世界统一决定。
    ProjectileConfig enemyProjectile = defaultEnemyProjectile();
};

/// 一局沙盒的结果。
///
/// 【为什么需要这个枚举】
/// 在它存在之前，打完一局没有任何结果：敌人全清完了没提示，玩家被打死了
/// 也不能重开。更糟的是玩家一死，`collectPerceptionTargets()` 会把玩家从
/// 感知列表里摘掉（`target.alive` 为假），于是敌人失去目标、行为树回落到
/// 巡逻，**世界上再也不会有子弹**。实测 30 秒与 60 秒两次无头运行的统计完全
/// 相同（shots=20 / dmg=100 / kills=0 / enemies 4/4）——模拟彻底停摆，
/// 连"观察失败现场"都做不到。
///
/// 有了这个枚举，"玩家已死"从一个静默的全局停摆变成一个明确的状态，
/// main 层据此冻结输入、显示结算、允许重开。
enum class MatchOutcome : uint8_t {
    Running = 0,  ///< 还在打
    Victory,      ///< 场上已无存活敌人
    Defeat        ///< 玩家阵亡
};

[[nodiscard]] const char* toString(MatchOutcome outcome) noexcept;

/// 伤害事件。由 World 在射击结算时产生，供调试 UI 显示。
struct DamageEvent {
    uint32_t  attackerId = 0;
    uint32_t  targetId   = 0;
    float     amount     = 0.0f;
    glm::vec2 impactPoint{0.0f, 0.0f};
    double    gameTime   = 0.0;
    bool      isPlayerAttack = false;
};

/// 弹着残影。一颗子弹**结束飞行时**产生一条，用于回答"这一枪是从哪打来的"。
///
/// 【与旧版 ShotTracer 的关系】
/// 旧版的曳光线是"开火瞬间就画出来"的：它和实际命中判定没有任何因果关系，
/// 只是把瞬时射线的结果描在屏幕上。现在伤害由真正飞行的 Projectile 结算，
/// 残影变成**事后记录**——子弹飞到哪、撞上什么、是不是命中，全部是事实。
///
/// 它之所以还存在，是因为在 4 个敌人同时开火的画面里，光靠"一闪而过的子弹"
/// 很难判断火力来自哪个方向。残影寿命刻意做得很短（kImpactMarkLifetime），
/// 主视觉仍然是飞行中的子弹本体。
struct ImpactMark {
    glm::vec2 origin{0.0f, 0.0f};    ///< 出膛位置（子弹的起点）
    glm::vec2 impact{0.0f, 0.0f};    ///< 终止位置（命中点 / 撞墙点 / 射程尽头）
    double    gameTime    = 0.0;     ///< 终止时刻，用于计算淡出
    bool      isPlayerShot = false;  ///< true = 玩家开的枪（暖黄），false = 敌人（红）
    bool      hit         = false;   ///< 是否打中了实体（撞墙/射程耗尽为 false）
    uint32_t  attackerId  = 0;

    /// 弹道长度（世界单位）。绘制时用来决定线宽/光晕是否值得画。
    [[nodiscard]] float length() const noexcept;
};

/// 残影在屏幕上停留的时长（秒）。到期后由 World 自动清理。
///
/// 【为什么是 0.11 而不是旧版的 0.18】
/// 旧的曳光线是唯一弹道视觉，必须活得久才看得见。现在子弹本身在飞，
/// 残影只承担"补一句从哪来"的职责，长了反而会糊成一片激光网，
/// 掩盖住真正在飞的那些子弹。
inline constexpr double kImpactMarkLifetime = 0.11;

/// 子弹推进时每个子步的最大距离（世界单位）。
///
/// 【这个常数的意义】
/// 60Hz 下子弹每步位移 = speed/60 ≈ 13.7 单位，而目标半径只有 11~12。
/// 如果一步走完，子弹会"跨过"目标而不触发碰撞（tunneling）。
/// 取 6.0 意味着每步至少切成 3 段，单段位移远小于最小目标直径，
/// 因此不可能漏检。再小就只是徒增循环开销。
inline constexpr float kProjectileMaxSubstep = 6.0f;

/// 游戏世界。
class World {
public:
    explicit World(WorldConfig config = {});
    ~World();

    World(const World&)            = delete;
    World& operator=(const World&) = delete;

    // -- 生命周期 ------------------------------------------------------------

    /// 构建初始场景（玩家、敌人、障碍物、导航网格）。
    void initialize();

    /// 清空并重建场景（对应「重开一局」）。
    void reset();

    /// 固定步长更新：驱动所有实体的 AI 与移动，并结算交互。
    /// @param fixedDelta 固定步长（秒）
    void update(double fixedDelta, double gameTime);

    /// 广播小队情报：把全队"最新鲜的一次目击"写进每个敌人的黑板，
    /// 让没看见目标的队友也有个方向可搜（敌人之间不互相引用，由 World 汇总）。
    void broadcastSquadIntel(double gameTime);

    /// 冻结 / 解冻敌人 AI（世界其余部分照常推进）。
    ///
    /// 用于调试：把 AI 停住、玩家仍能移动，便于逐帧观察某个敌人
    /// 对玩家动作的反应。此前 main.cpp 里的 F2 是把整个世界一起停掉，
    /// 与它自己的注释「只冻结 AI」相反。
    void setAIEnabled(bool enabled) noexcept { aiEnabled_ = enabled; }
    [[nodiscard]] bool aiEnabled() const noexcept { return aiEnabled_; }

    // -- 对局结果 ------------------------------------------------------------

    /// 当前对局结果。
    ///
    /// 【为什么是「实时计算」而不是「锁定」】
    /// 这是个沙盒，按 N 可以随时增兵。所以「清光敌人 → 胜利 → 按 N 增兵
    /// → 又是 Running」是刻意允许的：它让"胜利"变成一个可以随时看一眼
    /// 的状态，而不是一堵关上的墙。真正需要结束时按 R 重开即可。
    /// 当前对局结果。见实现里的注释：胜利会等敌方在飞的子弹落地。
    [[nodiscard]] MatchOutcome outcome() const;

    /// 是否还有"敌方阵营、仍在飞行"的子弹。用于胜负判定，
    /// 避免"击杀最后一个敌人 → 它的残弹打死玩家 → 胜负翻转"。
    [[nodiscard]] bool hasLiveEnemyProjectile() const noexcept;

    /// 本局是否已经分出胜负（outcome() != Running）。
    [[nodiscard]] bool isMatchOver() const noexcept {
        return outcome() != MatchOutcome::Running;
    }

    // -- 实体访问 ------------------------------------------------------------

    [[nodiscard]] Player* player() noexcept;
    [[nodiscard]] const Player* player() const noexcept;

    [[nodiscard]] const std::vector<std::unique_ptr<Entity>>& entities() const noexcept {
        return entities_;
    }

    /// 所有 Enemy 实体，**包含尚未回收的尸体**（尸体默认滞留 3 秒）。
    ///
    /// 需要"还活着的"请用 aliveEnemyCount()；需要对每个敌人下指令
    /// （请求寻路、批量击杀、清空）用这个。以前的注释写的是"所有存活的
    /// 敌人"，与实现不符——照那句话写的新代码会踩到尸体。
    [[nodiscard]] std::vector<Enemy*> enemies() const;
    [[nodiscard]] size_t enemyCount() const noexcept { return enemyCount_; }
    [[nodiscard]] size_t aliveEnemyCount() const;

    /// 按 ID 查找实体。找不到返回 nullptr。
    [[nodiscard]] Entity* findEntity(uint32_t id) noexcept;
    [[nodiscard]] const Entity* findEntity(uint32_t id) const noexcept;

    /// 正在飞行的子弹（未终止的）。每步都会变，调用方不要缓存结果。
    [[nodiscard]] std::vector<Projectile*> projectiles() const;

    /// 当前在空中的子弹数量。
    ///
    /// 故意每次实算而不缓存计数：子弹的生死发生在子步进循环里，
    /// 缓存值一旦漏减就会永久偏移，而"面板上的数字悄悄变错"
    /// 是最难发现的一类 bug。实体总数只有几十个，遍历的代价可以忽略。
    [[nodiscard]] size_t projectileCount() const noexcept;

    // -- 生成与移除 ----------------------------------------------------------

    /// 生成一个敌人。位置会自动避开已有实体（在 minSpawnDistance 内则重试）。
    [[nodiscard]] Enemy* spawnEnemy(std::optional<glm::vec2> position = std::nullopt);

    /// 生成一个障碍物。
    [[nodiscard]] Obstacle* spawnObstacle(std::string name, glm::vec2 position, glm::vec2 size,
                                          bool provideCover = false);

    /// 发射一颗子弹。
    ///
    /// @param ownerId   开枪者 ID
    /// @param faction   归属阵营（决定能伤害谁）
    /// @param muzzle    枪口位置（内部会自动沿 direction 前推 muzzleOffset）
    /// @param direction 飞行方向（无需归一化）
    /// @return 新子弹指针；参数非法（零方向）时返回 nullptr
    [[nodiscard]] Projectile* spawnProjectile(uint32_t ownerId, Faction faction,
                                             glm::vec2 muzzle, glm::vec2 direction,
                                             const ProjectileConfig& config);

    /// 移除实体（指针在调用后失效）。
    bool removeEntity(uint32_t id);

    /// 清理本步死亡的敌人（保留一小段时间以便看到死亡效果）。
    void cleanupDeadEntities(double gameTime);

    // -- 导航与查询 ----------------------------------------------------------

    [[nodiscard]] AI::NavGrid& navGrid() noexcept { return navGrid_; }
    [[nodiscard]] const AI::NavGrid& navGrid() const noexcept { return navGrid_; }

    /// 地形视线查询：两点之间是否无障碍物。
    /// 供感知系统与 EQS 使用，避免它们直接依赖 World。
    [[nodiscard]] AI::LineOfSightQuery makeLineOfSightQuery() const;

    /// 世界空间中的点是否可站立（不撞障碍物/墙）。
    ///
    /// @param entityRadius 实体半径。传入 0 只检查中心格；
    ///                     传入实体半径则额外做「膨胀检查」——圆与任何
    ///                     阻挡格相交即判不可站立。取 0 的快路径供
    ///                     不关心实体尺寸的调用方使用。
    [[nodiscard]] bool isPositionWalkable(glm::vec2 position,
                                          float entityRadius = 0.0f) const;

    /// 用导航网格把任意坐标吸附到最近的可站立点。
    [[nodiscard]] glm::vec2 snapToWalkable(glm::vec2 position,
                                           float entityRadius = 0.0f) const;

    // -- 伤害结算 ------------------------------------------------------------

    /// 对目标造成伤害并记录事件。
    /// @return 实际造成的伤害
    float applyDamage(uint32_t targetId, float amount, uint32_t attackerId,
                      glm::vec2 impactPoint, double gameTime);

    /// 本步产生的伤害事件（UI 显示用；World 会在下一步开头清空）。
    [[nodiscard]] const std::vector<DamageEvent>& damageEvents() const noexcept {
        return damageEvents_;
    }

    /// 最近的伤害事件（用于屏幕上的受击反馈）。
    [[nodiscard]] const DamageEvent* lastDamageEvent() const noexcept {
        return damageEvents_.empty() ? nullptr : &damageEvents_.back();
    }

    // -- 弹着残影 ------------------------------------------------------------

    /// 当前活跃的弹着残影（会自动按 kImpactMarkLifetime 过期清理）。
    [[nodiscard]] const std::vector<ImpactMark>& impactMarks() const noexcept {
        return impactMarks_;
    }

    /// 记录一条弹着残影。子弹终止时由 World 内部调用。
    void addImpactMark(glm::vec2 origin, glm::vec2 impact, bool isPlayerShot,
                       bool hit, uint32_t attackerId, double gameTime);

    // -- 统计数据（HUD 显示） ------------------------------------------------

    /// 累计开火次数（= 发射的子弹总数）。每颗子弹恰好对应一次开火。
    [[nodiscard]] size_t totalShotsFired() const noexcept { return totalShotsFired_; }
    /// 累计**命中实体**的子弹数。用于算命中率，以及验证"子弹真的在飞"。
    [[nodiscard]] size_t totalProjectileHits() const noexcept { return totalProjectileHits_; }

    /// 累计**打中无敌帧目标**的子弹数（被吸收、未造成伤害）。
    ///
    /// 这是"翻滚无敌生效"唯一可当场观测的证据。没有它，无敌帧就只能靠
    /// 读代码相信——而这正是之前"敌方没有冷却"那类问题拖了很久的原因：
    /// 声明了、注释写了，但没有任何数字能证明它真的在跑。
    [[nodiscard]] size_t totalProjectilesDodged() const noexcept {
        return totalProjectilesDodged_;
    }
    [[nodiscard]] float  totalDamageDealt() const noexcept { return totalDamageDealt_; }
    [[nodiscard]] int    enemiesKilled() const noexcept { return enemiesKilled_; }
    /// 外部时钟的当前值（游戏绝对时间）。
    ///
    /// ⚠ 它是"世界时钟"而不是"本局用时"：弹着残影与伤害闪光都用它做年龄
    /// 计算，必须与事件里记录的时间戳处于同一时钟，因此**不要**把它改成
    /// 相对值。结算画面要显示的"本局用时"请用 matchElapsedTime()。
    [[nodiscard]] double elapsedGameTime() const noexcept { return elapsedGameTime_; }

    /// 本局已进行的时长（秒）：重开后从 0 重新计。
    [[nodiscard]] double matchElapsedTime() const noexcept {
        return elapsedGameTime_ - matchStartGameTime_;
    }

    /// 结算显示用的「本局用时」：**分出胜负后就冻结在判定的那一刻**。
    ///
    /// 【为什么不能直接用 matchElapsedTime()】
    /// 失败/胜利之后世界仍在推进（敌人退巡逻、子弹落地、尸体消失），
    /// 所以实时用时还会一直往上涨。结算横幅上的"用时/坚持"若用实时值，
    /// 数字会像个忘了停的秒表——玩家盯着看会以为自己还在打。
    /// 若胜利后按 N 增兵继续（结果回到 Running），计时重新开始。
    ///
    /// 这里的判定是**实时**的（每次都问一次 outcome()），而不是读缓存的
    /// 上一步结果：结果可能在 update 之外改变（调试面板直接造成伤害、
    /// 按 N 增兵），用缓存会有一帧延迟，横幅会显示上一局的数字。
    /// 缓存的 outcomeSettledTime_ 只在"已分胜负"时使用，它由 update()
    /// 在判定发生的那一步写入，因此是准确的判定时刻。
    [[nodiscard]] double settledElapsedTime() const noexcept {
        if (outcome() == MatchOutcome::Running) {
            outcomeSettledTime_ = 0.0;   // 未分胜负 / 增兵续战：解除冻结
            return matchElapsedTime();
        }
        if (outcomeSettledTime_ <= 0.0) {
            // 结果是在 update() 之外变成"已分胜负"的：先按当前用时锁一次，
            // 避免横幅显示 0.0（下一步 update 会把它修正为准确时刻）。
            outcomeSettledTime_ = matchElapsedTime();
        }
        return outcomeSettledTime_;
    }

    /// 平均每步的 AI 耗时（毫秒），用于判断 AI 预算是否超支。
    [[nodiscard]] double averageAITimeMs() const noexcept;

    [[nodiscard]] const WorldConfig& config() const noexcept { return config_; }

    /// 一行世界状态摘要。
    [[nodiscard]] std::string debugString() const;

    /// 完整场景转储（实体列表 + 导航网格状态）。
    [[nodiscard]] std::string dump() const;

private:
    // -- 构建辅助 ------------------------------------------------------------

    void buildNavGrid();
    void generateDemoObstacles();

    /// 收集所有可作为 AI 感知目标的实体（按阵营）。
    void collectPerceptionTargets();

    /// 收集所有威胁源（掩体、敌人），供 EQS 使用。
    void collectThreatSources();

    /// 处理所有实体的射击意图（把「想开火」变成一颗真的子弹）。
    ///
    /// 注意这里**不再结算伤害**。这一步只负责把子弹放进世界，
    /// 命中与伤害全部推迟到 updateProjectiles 里——这正是"攻击有实体"
    /// 与"攻击是瞬时射线"的分界线，也是走位能生效的前提。
    void processWeaponFire(double gameTime);

    /// 推进所有子弹，并在飞行过程中结算命中。
    ///
    /// 用子步进（kProjectileMaxSubstep）而不是一步积分，因为一步位移
    /// 大于目标直径，会直接穿过去。
    void updateProjectiles(double fixedDelta, double gameTime);

    /// 子弹命中判定：找到被这颗子弹击中的实体。没有则返回 nullptr。
    ///
    /// 只考虑敌对阵营（不做友军伤害），且跳过开枪者自己。
    [[nodiscard]] Entity* findProjectileVictim(const Projectile& projectile) const;

    /// 子弹当前位置是否撞上了地形（墙 / 掩体 / 出界）。
    [[nodiscard]] bool projectileBlockedAt(glm::vec2 point) const;

    /// 回收已经终止的子弹，并为它们留下弹着残影。
    void collectTerminatedProjectiles(double gameTime);

    /// 把玩家移动与行为树移动意图转换为实体速度后，执行碰撞与边界处理。
    void resolveMovement(double fixedDelta);

    /// 把一个圆形实体约束在网格范围内并推开障碍物。
    void constrainEntity(Entity& entity) const;

    /// 找一个与已有实体保持距离的空闲生成点。
    /// @param clearanceRadius 膨胀检查用的实体半径（见 isPositionWalkable）
    [[nodiscard]] glm::vec2 findFreeSpawnPosition(int maxAttempts = 64,
                                                  float clearanceRadius = 0.0f) const;

    WorldConfig config_;

    std::vector<std::unique_ptr<Entity>> entities_;
    std::unordered_map<uint32_t, Entity*> entityIndex_;   ///< ID -> 裸指针，O(1) 查找

    AI::NavGrid navGrid_;
    bool initialized_ = false;

    // 每步复用的感知目标缓存，避免每帧重新分配
    std::vector<AI::PerceptTarget> perceptionTargets_;
    std::vector<glm::vec2>         threatSources_;
    std::vector<glm::vec2>         coverPositions_;

    std::vector<DamageEvent> damageEvents_;
    std::vector<ImpactMark>  impactMarks_;

    size_t enemyCount_ = 0;
    size_t totalShotsFired_ = 0;
    size_t totalProjectileHits_ = 0;
    size_t totalProjectilesDodged_ = 0;
    float  totalDamageDealt_ = 0.0f;
    int    enemiesKilled_ = 0;

    /// 本局是否曾经生成过敌人。
    ///
    /// 用来区分"打赢了"和"一开始就没有敌人"。后者不该判胜利——
    /// 否则 --enemies 0 会在第一帧就显示"胜利"，读起来像 bug。
    /// 用"曾经生成过"而不是 enemyCount_ > 0，是因为敌人死后 count 不归零，
    /// 而"曾经有过敌人且现在全死了"正是胜利的准确语义。
    bool   spawnedAnyEnemy_ = false;

    double elapsedGameTime_ = 0.0;
    uint64_t aiUpdateCount_ = 0;
    double   totalAITimeMs_ = 0.0;

    /// AI 是否参与更新（调试用冻结开关，见 setAIEnabled）。
    bool aiEnabled_ = true;

    /// 本局开始的游戏时间。用于把外部时钟换算成"本局已打多久"
    /// （matchElapsedTime）。reset() 之后第一次 update 会把它重新对齐到
    /// 当时的时钟，因此重开一定从 0 开始——注意 reset() 本身拿不到时钟，
    /// 必须靠 update 来对齐，否则"归零"会被下一步立刻覆盖掉。
    double matchStartGameTime_ = 0.0;
    bool   rebaseTimePending_  = true;

    /// 上一步的对局结果。用于把"结算用时"锁定在判定那一刻（见 settledElapsedTime）。
    MatchOutcome lastOutcome_ = MatchOutcome::Running;

    /// 判定胜负那一刻的"本局用时"。mutable 是因为它由 const 的
    /// settledElapsedTime() 在需要时补写（见那里的说明）。
    mutable double outcomeSettledTime_ = 0.0;

    /// 开火意图暂存区。见 processWeaponFire：先收集、后发射，
    /// 避免在遍历 entities_ 的过程中往里面 push_back（迭代器失效）。
    struct PendingShot {
        uint32_t         ownerId = 0;
        glm::vec2        muzzle{0.0f, 0.0f};
        glm::vec2        direction{0.0f, 0.0f};
        ProjectileConfig config{};
    };
    std::vector<PendingShot> pendingShots_;

    /// 死亡实体的回收计时（gameTime -> 待回收的实体 ID 列表）
    std::vector<std::pair<uint32_t, double>> pendingRemoval_;

    /// 生成点随机数种子固定，保证沙盒可复现
    uint32_t spawnSeed_ = 0x5EEDu;
};

} // namespace TacticalAI::Game
