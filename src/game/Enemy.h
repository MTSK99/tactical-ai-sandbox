// ===========================================================================
//  src/game/Enemy.h
//
//  敌方 AI 实体。整个项目的「集大成者」：
//  它把感知、行为树、EQS、寻路四个模块串成一条完整的决策流水线。
//
//  每固定步的数据流：
//
//    Perception.update()          ← 我看到了什么？
//          ↓ 写入黑板
//    EQS.run(找掩体查询)           ← 哪个位置最好？
//          ↓ 结果写入黑板
//    Pathfinder.findPath()        ← 怎么去那里？
//          ↓ 路径写入黑板
//    BehaviorTree.tick()          ← 现在该做什么？
//          ↓ 输出移动意图
//    Entity::update()             ← 实际移动
//
//  注意顺序：感知必须在决策之前（否则用过期信息），
//  决策必须在移动到之前（否则移动没有意图）。这个顺序不能乱。
// ===========================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ai/BehaviorTree.h"
#include "ai/Blackboard.h"
#include "ai/EQS.h"
#include "ai/Pathfinding.h"
#include "ai/Perception.h"
#include "game/Entity.h"

namespace TacticalAI::Game {

/// 敌方 AI 的高层状态。用于调试显示与外部查询。
enum class AIState : uint8_t {
    Idle      = 0,   ///< 无目标，原地待命
    Patrolling,      ///< 有最后已知位置，前往搜索
    Engaging,        ///< 有确认目标，交战中
    Repositioning,   ///< 正在前往更好的位置（EQS 输出的位置）
    Retreating,      ///< 生命值过低，脱离接触
    Dead
};

[[nodiscard]] const char* toString(AIState state) noexcept;

/// 敌方配置。所有可调参数集中一处，便于做「AI 难度档位」。
struct EnemyConfig {
    // -- 移动 --
    //
    // 【速度是"AI 能不能打到人"的前置条件，不只是手感】
    // 玩家：走 160 / 冲刺 280。原来敌人是 130 / 200 ——
    //   走速比玩家走路还慢，永远追不上；
    //   冲刺只比玩家走路快 20，实测"以 20 单位/秒逼近"（射手永远差一步）。
    // 实测（玩家绕圈走位 60 秒）敌人有 28~34 秒处在射程之外，几乎无法还手。
    //
    // 现在的档位设计意图：
    //   走 145  —— 交战中走位，略慢于玩家走路，玩家仍能小幅拉开；
    //   冲刺 240 —— 只在"目标/线索在射程之外"时启用，能追上走路的玩家
    //               （80 单位/秒的逼近速度），但追不上冲刺的玩家（280）。
    // 一句话：走路跑不掉，冲刺能跑掉。
    float moveSpeed      = 145.0f;
    float sprintSpeed    = 240.0f;
    float acceleration   = 900.0f;
    float radius         = 11.0f;
    float maxHealth      = 70.0f;

    // -- 战斗 --
    float preferredRange      = 220.0f;  ///< 理想交战距离
    float retreatHealthRatio  = 0.3f;    ///< 低于此血量比例触发撤退
    /// 能开火的最大距离。与 defaultEnemyProjectile().maxRange 必须一致 ——
    /// 超出子弹射程开火等于打空枪（子弹飞到一半就消失）。
    ///
    /// 【调整：320 → 400】实测玩家走位时敌人有 2.4~6.8 秒的连续时段在射程外
    /// （玩家 160~280 单位/秒 vs 敌人 130），几乎没机会还手。射程放宽到 400
    /// 并配合"追击时冲刺"（见 Enemy::desiredMoveSpeed），敌人才有还手窗口。
    /// 仍然比玩家的 520 短，玩家依然可以拉开距离压制。
    float fireRange           = 400.0f;

    /// 两次开火之间的最小间隔（秒）。
    ///
    /// 【这个字段曾经是个谎】
    /// 它一直声明在这里，调参面板里甚至有一根"射击间隔"滑块在改它，
    /// 但 AimAndFire 从来没有读过它——注释写着"带冷却"，代码里没有冷却。
    /// 于是 AI 只要把枪口对准玩家，就会**每个逻辑步开一枪**（60Hz 下
    /// 每秒 60 发）。4 点伤害 × 60 = 240 DPS，玩家 100 血在 0.4 秒内被打空，
    /// 表现为"敌人能秒杀我"。
    ///
    /// 现在它是真的了。0.95s 意味着一个敌人每秒约 1.05 发，
    /// 单个敌人的理论 DPS 从 240 降到约 5.3。
    float fireCooldown        = 0.95f;

    float damage              = 5.0f;

    /// 开火判据：弹道在目标处的横向偏差小于这个值才开枪（世界单位）。
    ///
    /// 【为什么不是一个角度】
    /// 角度容差的误差会随距离放大：0.20 rad 在 100 单位外偏 20 个单位、
    /// 在 320 单位外偏 64 个单位。而命中窗口是固定的（目标半径 + 子弹半径）。
    /// 因此角度容差在近距离过严、远距离完全失效——它不是一个能和"打不打得中"
    /// 挂钩的量。横向偏差才是。
    ///
    /// 默认 14 = 敌人半径 11 + 子弹半径 3，正好等于"擦边命中"的临界值，
    /// 略留一点余量避免边界抖动。
    float aimHitRadius        = 14.0f;

    /// 开火提前量强度。0 = 完全朝目标当前位置开火（移动目标永远打不中），
    /// 1 = 完全按估计速度提前。
    ///
    /// 【为什么需要它】
    /// 攻击从"瞬时射线"改成"实体子弹"之后，命中发生在子弹飞到目标的
    /// 那一刻。如果 AI 仍然朝目标**开火瞬间的位置**射击，那么任何在
    /// 移动的玩家都会自动躲开所有子弹——AI 看起来会突然变傻。
    /// 0.85 是刻意留出的一点点误差：直线移动会被打中，急变换向仍能躲开。
    float aimPrediction       = 0.85f;

    // -- AI 行为 --
    double searchDuration     = 8.0;     ///< 丢失目标后搜索多久

    /// 队友共享情报的有效期（秒）。
    ///
    /// 队友的"最后已知位置"超过这个时长就不再被采纳——否则一次偶遇会让
    /// 全队永远朝一个几分钟前的坐标搜索。取值比 searchDuration 略长：
    /// 别人报的点值得多找一会儿，但也不能当成永久情报。
    double squadIntelMemory   = 24.0;

    /// 情报"值得再出动一次"的位移阈值（世界单位）。
    ///
    /// 【为什么需要它】
    /// 队友持续看着同一个位置时，情报的时间戳每步都在前进。若把"时间戳更新"
    /// 当成"有新线索"，被派去查看的敌人就会反复回到同一个点重新搜索 ——
    /// 表现为"跑到 last location 站桩发呆"。只有报告位置真的移动了这么多，
    /// 才值得再跑一趟。
    float searchRearmDistance = 120.0f;

    /// 到达最后已知位置后，观察多久才放弃搜索（秒）。
    ///
    /// 【为什么需要它，而不是"到了就放弃"】
    /// 玩家绕柱子躲避时，AI 走到最后已知位置时玩家可能只是刚被挡住。
    /// 立刻放弃等于"AI 明知道有人在附近却不看一眼"，行为看起来很蠢。
    /// 停一小段时间再看一眼，才像"我到这儿看看，人不在了"。
    ///
    /// 【调整：2.0 → 0.8，并且这段时间不再是"站桩"】
    /// 2 秒完全静止在画面上就是发呆。现在缩短到 0.8 秒，并且期间会左右
    /// 扫视（朝向摆动）——"到点、环顾、离开"比"到点、僵硬 2 秒"自然得多。
    double searchHoldDuration = 0.8;

    /// 允许开火的目标「新鲜度」（秒）：距上次**真正看见**目标超过这么久
    /// 就不再开枪。
    ///
    /// 【为什么需要它（此前缺失导致的 bug）】
    /// 开火用的目标位置来自 `kLastKnownPosition` —— 敌人**记忆**中的位置。
    /// 玩家绕到背后 / 躲进掩体 / 跑出视野之后，这个位置会停在原地，
    /// 而感知的「确认目标」会在 memoryDuration（默认 6 秒）内继续成立，
    /// 于是敌人会朝着一个**空位置**连开 6 秒（实测：玩家瞬移走后，
    /// 敌人对着 762 单位外的空点打出 7 发）。LoS 检查也救不了它 ——
    /// 那个空点本身当然是有视线的。
    ///
    /// 判据用 kTimeSinceSeenEnemy（由 Perception::timeSinceLastSeen 写入，
    /// 只在**真正看见**时归零，与"确认"不同）。0.15 秒 ≈ 9 个逻辑步，
    /// 足够容忍感知写入与开火判定之间的同帧先后关系，又短到玩家察觉不到。
    float fireTargetMaxAge = 0.15f;

    /// 失去视线后仍留在交战分支的宽限期（秒）。
    ///
    /// 【为什么不一直留在交战分支】
    /// 感知的"确认目标"在记忆期内一直为真，交战分支（优先级高于搜索）因此
    /// 会把敌人**钉在原地 6 秒**，对着最后已知位置干等。超过这个宽限期就
    /// 让位给搜索分支，敌人会真的走过去找（顺带这也是"敌人显得会找人"
    /// 而不是"站着发呆"的关键）。
    ///
    /// 0.5 秒的用意：玩家只是短暂绕到柱子后、退进掩体一瞬间时，敌人不该
    /// 立刻改变行为；但要长到足够看出"目标真的没了"。
    double engageLostGrace = 0.5;

    double repositionInterval = 2.5;     ///< 每隔多久重新评估位置
    bool   useEQS             = true;    ///< 关闭可对比「有/无 EQS」的行为差异

    // -- 感知 --
    AI::PerceptionConfig perception{};
};

/// 敌方单位。
class Enemy final : public Entity {
public:
    Enemy(std::string name, glm::vec2 position, EnemyConfig config = {});
    ~Enemy() override;

    Enemy(const Enemy&)            = delete;
    Enemy& operator=(const Enemy&) = delete;

    // -- 外部依赖注入 --------------------------------------------------------
    // 通过 setter 注入而非构造函数参数，因为这些依赖在 Enemy 创建时
    // 可能还不存在（World 先建实体再建网格）。

    /// 设置导航网格（寻路用）。传空指针则退化为直线移动。
    void setNavGrid(const AI::NavGrid* grid) noexcept { navGrid_ = grid; }
    /// 设置地形视线查询（感知遮挡、EQS 掩体判定共用）。
    void setLineOfSightQuery(AI::LineOfSightQuery query);

    /// 设置自身子弹的飞行速度（用于计算开火提前量）。
    ///
    /// 由 World 注入而不是放进 EnemyConfig：子弹速度是"武器"的属性，
    /// 改了 WorldConfig 里的弹道参数之后不需要再同步改一遍 AI 配置，
    /// 避免出现"子弹变快了但 AI 的提前量还按旧速度算"的错配。
    void setProjectileSpeed(float speed) noexcept {
        projectileSpeed_ = (speed > 1.0f) ? speed : 1.0f;
    }

    /// 设置威胁源列表（EQS 规避用），通常由 World 每步更新。
    void setThreatSources(const std::vector<glm::vec2>& threats);

    /// 设置掩体位置列表（EQS 掩护度测试用），同样由 World 每步更新。
    /// 与威胁源分开：威胁要躲开，掩体要靠近，混在一起会让评分失效。
    void setCoverPositions(const std::vector<glm::vec2>& covers);

    // -- 每步驱动 ------------------------------------------------------------

    /// 由 World 每固定步调用。
    /// @param fixedDelta 固定步长
    /// @param gameTime   当前游戏时间
    /// @param targets    当前可见的敌方目标（由 World 收集）
    void updateAI(double fixedDelta, double gameTime, const std::vector<AI::PerceptTarget>& targets);

    /// 由 World 调用：请求重新寻路（例如玩家移动到新位置时）。
    void requestRepath() noexcept;

    // -- 状态查询 ------------------------------------------------------------

    [[nodiscard]] AIState state() const noexcept { return state_; }
    [[nodiscard]] const AI::Blackboard& blackboard() const noexcept { return blackboard_; }
    [[nodiscard]] AI::Blackboard& blackboard() noexcept { return blackboard_; }
    [[nodiscard]] const AI::PerceptionSystem& perception() const noexcept { return *perception_; }
    [[nodiscard]] const AI::BehaviorTree& behaviorTree() const noexcept { return *behaviorTree_; }
    [[nodiscard]] const AI::EnvironmentQuerySystem& eqs() const noexcept { return *eqs_; }
    [[nodiscard]] const AI::PathResult& currentPath() const noexcept { return currentPath_; }
    [[nodiscard]] const std::vector<glm::vec2>& pathWaypoints() const noexcept {
        return currentPath_.waypoints;
    }

    /// 是否本步开火了（供 World 发射子弹）。
    [[nodiscard]] bool firedThisStep() const noexcept { return firedThisStep_; }

    /// 清空「本步意图」标志（开火 / 已瞄准）。
    ///
    /// 【为什么 World 需要它】
    /// 这两个标志的唯一清零点在 updateAI() 开头。只要 World 因为任何原因
    /// **跳过了** updateAI（典型的例子是调试用的 AI 冻结），标志就会停留在
    /// 上一步的值上：firedThisStep_ 恒为 true → World 每逻辑步都替它发射
    /// 一颗子弹，而且开火冷却判定（在行为树里）同样被跳过，表现为"冻结后
    /// 敌人变成无限机枪"。World 跳过 AI 时必须显式复位。
    void clearStepIntent() noexcept {
        firedThisStep_  = false;
        aimSteeredThisStep_ = false;
    }
    [[nodiscard]] int shotsFired() const noexcept { return shotsFired_; }
    [[nodiscard]] float damageDealt() const noexcept { return damageDealt_; }
    void addDamageDealt(float amount) noexcept { damageDealt_ += amount; }

    /// 距离下一次可以开火还剩多少秒（0 = 已就绪）。调试面板与测试用。
    [[nodiscard]] double fireCooldownRemaining(double gameTime) const noexcept;

    /// 估计的目标移动速度（由连续两次瞄准点差分得到）。调试可视化用。
    [[nodiscard]] glm::vec2 estimatedTargetVelocity() const noexcept {
        return targetVelocity_;
    }

    /// 当前瞄准的世界坐标点。
    [[nodiscard]] glm::vec2 aimPoint() const noexcept { return aimPoint_; }

    /// 最近的 EQS 查询结果（调试绘制用）。
    [[nodiscard]] const AI::EQSResult& lastPositionQuery() const noexcept {
        return lastPositionQuery_;
    }

    /// 当前移动目标（可能是 EQS 选出的位置，也可能是最后已知敌人位置）。
    [[nodiscard]] glm::vec2 moveTarget() const noexcept { return moveTarget_; }

    // -- 小队情报共享 --------------------------------------------------------

    /// 由 World 每固定步写入：某个队友最后看到目标的位置与时刻。
    ///
    /// 【语义边界：这是"情报"，不是"视野"】
    /// 收到情报的敌人只知道"目标曾经在那儿"，不会因此获得射击能力 ——
    /// 交战分支仍然要求自己确认到目标（感知系统的 confirmed）。
    /// 这样既能让没看见目标的队友赶过去包抄，又不会让全场敌人共享视野。
    void setSquadIntel(glm::vec2 position, double seenTime);

    /// 情报失效（过了有效期或报告者已阵亡）时清空。
    void clearSquadIntel() noexcept;

    /// 队友情报是否仍然有效（未过期且存在）。
    [[nodiscard]] bool hasFreshSquadIntel(double gameTime) const noexcept {
        return hasSquadIntel_ &&
               (gameTime - squadIntelTime_) <= config_.squadIntelMemory;
    }

    /// 当前收到的队友情报坐标（无效时返回自己的最后已知位置）。
    [[nodiscard]] glm::vec2 squadIntelPosition() const noexcept { return squadIntelPosition_; }

    /// 自己最近一次"确认看到目标"的时刻（供 World 汇总成小队情报）。
    [[nodiscard]] double lastSeenEnemyTime() const noexcept { return lastSeenEnemyTime_; }
    [[nodiscard]] bool hasLastKnownPosition() const noexcept { return hasLastKnownPosition_; }
    [[nodiscard]] glm::vec2 lastKnownEnemyPosition() const noexcept {
        return lastKnownEnemyPosition_;
    }

    [[nodiscard]] const EnemyConfig& config() const noexcept { return config_; }
    EnemyConfig& config() noexcept { return config_; }

    /// 感知配置（转发到 PerceptionSystem，便于运行时调参）。
    [[nodiscard]] AI::PerceptionConfig& perceptionConfig() noexcept {
        return perception_->config();
    }

    [[nodiscard]] std::string debugString() const override;

    /// AI 决策的完整快照，供调试面板展开显示。
    [[nodiscard]] std::string aiDebugString() const;

private:
    // -- 初始化 --------------------------------------------------------------

    /// 构建行为树。这是整个 AI 的「剧本」，建议对照注释阅读。
    void buildBehaviorTree();

    /// 构建 EQS 查询（寻找掩体的环境查询）。
    void buildEQSQueries();

    // -- 流水线各阶段 --------------------------------------------------------

    /// 阶段 1：感知更新 + 写入黑板。
    void updatePerception(double gameTime, const std::vector<AI::PerceptTarget>& targets);

    /// 阶段 2：必要时执行 EQS 位置查询。
    void updatePositionQueries(double gameTime);

    /// 从行为树的 TickContext 组装 EQS 查询上下文。
    /// 行为树的动作节点只拿到 TickContext，而 EQS 需要「自身位置/朝向、
    /// 当前目标、威胁源、游戏时间」，因此在此处做一次适配。
    [[nodiscard]] AI::EQSContext buildEQSContext(const AI::TickContext& context) const;

    /// 阶段 3：必要时重新寻路。
    void updatePathfinding();

    /// 阶段 4：行为树 tick，产出决策。
    void updateDecision(double fixedDelta, double gameTime);

    /// 阶段 5：把行为树的决策翻译成实际运动。
    void applyMovement(double fixedDelta);

    /// 当前状态下的期望移动速度（撤退时取 sprintSpeed，其余取 moveSpeed）。
    /// 与 applyMovement 里的速度上限使用同一判据，避免"上限很高但期望速度
    /// 很低"这种配置静默失效。
    [[nodiscard]] float desiredMoveSpeed() const noexcept;

    /// 沿当前路径朝目标推进，返回是否已经到达。
    [[nodiscard]] bool followPath(double fixedDelta, glm::vec2 destination);

    /// 当前可用的「搜索线索」：自己的记忆优先，其次是队友共享的情报。
    ///
    /// 【为什么要有这一层】
    /// 搜索分支的启动条件与目标点都来自"最后已知位置"。把这两处判断收敛
    /// 到一个函数，才能保证"条件成立"与"知道往哪走"永远一致——分开写的话，
    /// 很容易出现"条件说能搜，动作却拿到一个零向量"这种半挂状态。
    ///
    /// @return false 表示既没有自己的记忆、也没有可用的队友情报
    [[nodiscard]] bool searchLead(double gameTime, glm::vec2& outPosition) const noexcept;

    /// 朝当前移动目标直线靠近（无导航网格时的降级方案）。
    [[nodiscard]] bool moveDirectlyToward(glm::vec2 destination, double fixedDelta);

    /// 平滑转向某个方向（模拟转身速度限制，避免瞬间转向的机械感）。
    void steerFacingToward(glm::vec2 desiredDirection, double fixedDelta);

    /// 计算朝向与目标方向的夹角（弧度）。
    [[nodiscard]] float facingError(glm::vec2 targetDirection) const;

    /// 计算弹道的横向偏差（世界单位）：如果现在开火，子弹会从目标身边
    /// 多少距离处掠过。0 表示正对，大于目标半径就是打不中。
    [[nodiscard]] float lateralAimError(glm::vec2 targetPoint) const;

    /// 计算带提前量的瞄准点（预测子弹与目标的交汇位置）。
    ///
    /// 会顺带更新内部的目标速度估计，因此不是 const —— 它承担"观察"的职责。
    /// @param targetPosition 感知到的目标位置（原始，非预测）
    [[nodiscard]] glm::vec2 predictedAimPoint(glm::vec2 targetPosition, double gameTime);

    /// 设置状态并记录状态切换（用于调试查看状态历史）。
    void setState(AIState state, double gameTime);

    /// 刷新黑板中的自身状态键。
    void writeSelfStateToBlackboard();

    // -- 依赖 --
    const AI::NavGrid* navGrid_ = nullptr;

    /// 地形视线查询的本地副本。
    ///
    /// 感知系统内部也持有同一份（用于判断"看不看得见"），但那是私有的。
    /// 开火前还需要再问一次"枪口到目标之间有没有墙"——因为瞄准点用的是
    /// **最后已知位置**，它可能已经被掩体挡住。没有这次的子弹会一头撞在
    /// 墙上，视觉上就是"AI 在对着墙开枪"。
    AI::LineOfSightQuery losQuery_;

    // -- AI 模块（每个 Enemy 独立持有，保证个体差异） --
    std::unique_ptr<AI::PerceptionSystem>      perception_;
    std::unique_ptr<AI::BehaviorTree>          behaviorTree_;
    std::unique_ptr<AI::EnvironmentQuerySystem> eqs_;
    AI::Blackboard blackboard_;
    AI::Pathfinder pathfinder_;

    // -- EQS 查询定义 --
    AI::EQSQuery coverSeekQuery_;      ///< 找掩体（能打到目标、不易被打到）
    AI::EQSQuery retreatQuery_;        ///< 撤退点（远离所有威胁）
    AI::EQSQuery approachQuery_;       ///< 接近点（拉近到理想射程）
    AI::EQSResult lastPositionQuery_;

    // -- 运行时状态 --
    EnemyConfig config_;
    AIState     state_ = AIState::Idle;

    AI::PathResult currentPath_;
    size_t  pathCursor_ = 0;
    /// 上次成功寻路的时刻。初值取极小值，让第一次"路径走完却没到"时
    /// 的节流判断立刻放行。
    double  lastPathTime_ = -1e9;
    double  lastPositionQueryTime_ = 0.0;
    double  lastRepositionTime_ = -1e9;  ///< 上次 EQS 选位时间（节流用）
    bool    engagePositionSelected_ = false;
    double  lastRetreatTime_ = -1e9;     ///< 上次撤退点评估时间（节流用）
    bool    retreatPositionSelected_ = false;
    double  lastFacingUpdateTime_ = 0.0;
    double  searchStartTime_ = 0.0;
    double  lastSeenEnemyTime_ = -1000.0;

    // -- 小队情报（队友共享的"最后已知位置"）--
    glm::vec2 squadIntelPosition_{0.0f, 0.0f};
    bool      hasSquadIntel_  = false;
    double    squadIntelTime_ = -1e9;   ///< 该情报对应的"目标被看到"的时刻

    /// 上一次已经"去查看过"的线索位置。
    /// 用于避免队友持续报同一个点时反复回到原地重新搜索（那看起来就是发呆）。
    glm::vec2 lastInvestigatedLead_{0.0f, 0.0f};
    bool      hasInvestigatedLead_ = false;

    /// 上次开火的游戏时间。初值为极大的负数：第一枪永远不受冷却限制。
    double  lastFireTime_ = -1e9;

    /// 最近一次 updateAI 使用的游戏时间。aiDebugString() 不接收 gameTime，
    /// 因此缓存一份，才能在不改接口的前提下报告"冷却还剩多久"。
    double  currentTime_ = 0.0;

    /// 自身子弹速度（由 World 注入），用于换算开火提前量。
    float   projectileSpeed_ = 760.0f;

    // -- 目标运动估计（开火提前量用） --
    glm::vec2 prevAimPoint_{0.0f, 0.0f};   ///< 上一步的瞄准点（原始目标位置）
    double    prevAimTime_ = 0.0;
    bool      hasPrevAimPoint_ = false;
    glm::vec2 targetVelocity_{0.0f, 0.0f}; ///< 平滑后的目标速度估计

    glm::vec2 moveTarget_{0.0f, 0.0f};
    glm::vec2 aimPoint_{0.0f, 0.0f};
    glm::vec2 lastKnownEnemyPosition_{0.0f, 0.0f};
    glm::vec2 desiredFacing_{1.0f, 0.0f};

    // -- 巡逻（无目标时的主动搜索行为） --
    glm::vec2 patrolHome_{0.0f, 0.0f};   ///< 巡逻圆心（出生点）
    glm::vec2 patrolTarget_{0.0f, 0.0f}; ///< 当前巡逻点
    int       patrolIndex_ = 0;          ///< 巡逻点序号，循环取模
    bool      patrolInitialized_ = false;
    double    lastPatrolRetargetTime_ = -1e9;  ///< 上次选点时间（节流用）

    /// 巡逻「卡住」监测：始终走不到目标时用于强制换点。
    /// 目标可能因为地形变化/够不到而永远无法抵达，而换点的常规条件依赖
    /// "已到达"——没有这个兜底，一次坏选点就是永久静止。
    float  patrolBestDistance_ = 1e9f;   ///< 本次目标上到达过的最近距离
    double patrolStuckTime_    = 0.0;    ///< 持续没有靠近目标的时长（秒）

    bool  repathRequested_ = true;   ///< 初始为 true：第一次更新就该有路径
    glm::vec2 lastPathGoal_{0.0f, 0.0f};  ///< 上次寻路时的目标点（见 updatePathfinding）
    bool  firedThisStep_ = false;
    bool  hasLastKnownPosition_ = false;

    // -- 搜索分支的状态 --
    //
    // 【为什么需要这两个字段】
    // 「到达最后已知位置 → 原地观察一会儿 → 放弃」这个行为需要区分
    // "这一刻刚到"和"已经站了一会儿"。只有一个 bool 是不够的：
    // 每次 arrived 为真都会重置计时，于是永远等不满一帧就"放弃"。
    double searchArrivedTime_ = -1e9;  ///< 到达时刻；-1e9 表示尚未到达
    bool   searchAbandoned_   = false; ///< 观察期满，已放弃搜索

    /// 本步是否已在行为树里完成瞄准转向。
    /// 用于让 applyMovement 不要再用"移动方向"覆盖枪口朝向。
    bool  aimSteeredThisStep_ = false;

    int   shotsFired_ = 0;
    float damageDealt_ = 0.0f;

    /// 状态切换历史（最近若干条），调试时能看出 AI「反复横跳」的问题。
    std::vector<std::pair<double, AIState>> stateHistory_;
};

} // namespace TacticalAI::Game
