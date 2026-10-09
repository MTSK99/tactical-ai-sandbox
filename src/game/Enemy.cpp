// ===========================================================================
//  src/game/Enemy.cpp
// ===========================================================================
#include "game/Enemy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

// 行为树的链式构造器（模板，需要内联实现可见）
#include "ai/BehaviorTree.inl"

namespace TacticalAI::Game {

// Enemy 是「AI 层的第一消费者」：这一个文件里就要用到行为树的各种节点、
// 黑板、EQS 上下文与结果等十余个 AI 类型。逐个写全限定名会让行为树
// 那段本应「接近伪代码」的构建逻辑变得冗长难读，因此在此文件范围内
// 开放 AI 命名空间。作用域仅限本编译单元，不会外溢到其他 Game 文件。
using namespace AI;

// 行为树构造器（bt::seq / bt::sel / bt::par 等）单独开放，
// 让 buildBehaviorTree() 里那棵树能直接写成接近伪代码的形式。
using namespace AI::bt;

namespace {

constexpr float kPi = 3.14159265358979323846f;

/// 判定"走到目的地"的距离阈值。
///
/// 之前这里散落着 14 与 20 两个字面量，20 那条其实永远用不到（能走到
/// 那一步时距离必然已经小于 14），读代码的人会以为到达半径是 20。
/// 统一成一个具名常量，语义只有一处。
constexpr float kArrivalRadius = 14.0f;

/// 接近终点时的减速起始距离：越接近目标速度越低，避免冲过头来回振荡。
constexpr float kSlowdownDistance = 60.0f;

/// 减速时的最低速度，保证最后一段不会慢到几乎停住。
constexpr float kMinApproachSpeed = 20.0f;

inline float vectorLength(glm::vec2 v) noexcept {
    return std::sqrt(v.x * v.x + v.y * v.y);
}

inline glm::vec2 normalizeOr(glm::vec2 v, glm::vec2 fallback) noexcept {
    const float len = vectorLength(v);
    if (len < 1e-6f) {
        return fallback;
    }
    return {v.x / len, v.y / len};
}

/// 把角度差规约到 [-pi, pi]。
inline float wrapAngle(float angle) noexcept {
    while (angle >  kPi) { angle -= 2.0f * kPi; }
    while (angle < -kPi) { angle += 2.0f * kPi; }
    return angle;
}

/// 取「当前主要目标（也就是玩家）的位置」——**不是**移动目标点。
///
/// ⚠ 注意区别：它读的是 `kLastKnownPosition`（敌人记忆中的玩家位置），
/// 而不是 `kMoveTarget`（EQS 选出的走位点）。曾经的撤退分支误用了这个
/// 函数，结果"撤离"变成了朝玩家冲锋。需要"往哪走"时请用 `moveTarget_`。
inline bool resolveTargetPosition(const AI::Blackboard& blackboard, glm::vec2& out) {
    if (const auto lastKnown =
            blackboard.get<glm::vec2>(AI::Blackboard::Keys::kLastKnownPosition)) {
        out = *lastKnown;
        return true;
    }
    return false;
}

/// 往黑板里写决策输出。单独抽出来是为了统一 lastWriter 名字便于追溯。
constexpr const char* kDecisionWriter = "BehaviorTree";
constexpr const char* kMovementWriter = "Movement";

} // namespace

const char* toString(AIState state) noexcept {
    // 【关于中文】
    // 这些字符串会直接渲染在实体头顶的标签上，因此用中文，
    // 与界面其余部分（HUD、图例、面板）保持一致。
    // 早期版本混用英文（Patrolling / Engaging），
    // 在满屏中文的界面上显得割裂且不易读。
    //
    // 注意：对应的码点必须出现在 GeneratedGlyphs.h 里，
    // 否则会显示成方块。该表由 scripts/gen_glyphs.py 扫描源码自动生成，
    // 因此这里的字面量会被自动收录，无需手工维护。
    switch (state) {
        case AIState::Idle:          return "待机";
        case AIState::Patrolling:    return "巡逻";
        case AIState::Engaging:      return "交战";
        case AIState::Repositioning: return "转移";
        case AIState::Retreating:    return "撤退";
        case AIState::Dead:          return "死亡";
    }
    return "未知";
}

// ===========================================================================
//  构造
// ===========================================================================
Enemy::Enemy(std::string name, glm::vec2 position, EnemyConfig config)
    : Entity(EntityType::Enemy, std::move(name), position),
      config_(config) {
    setMaxHealth(config_.maxHealth);
    setHealth(config_.maxHealth);
    setRadius(config_.radius);
    addTag(EntityTag::Hostile);

    // 朝向由 AI 全权掌控：巡逻时朝向移动方向（由 desiredFacing_ 驱动），
    // 交战时朝向目标。绝不能让"朝向 = 速度方向"插一脚——那会让敌人
    // 在侧移时把枪口甩开，子弹全部打偏。
    setFaceVelocityDirection(false);

    // 感知系统的视线查询先留空，由 World 通过 setLineOfSightQuery 注入
    perception_ = std::make_unique<AI::PerceptionSystem>(config_.perception, nullptr);

    eqs_ = std::make_unique<AI::EnvironmentQuerySystem>();
    behaviorTree_ = std::make_unique<AI::BehaviorTree>(name_ + ".BT");

    buildEQSQueries();
    buildBehaviorTree();

    // 初始把自身状态写进黑板，避免行为树第一次 tick 读到空值
    writeSelfStateToBlackboard();

    moveTarget_ = position;
    aimPoint_ = position + glm::vec2(100.0f, 0.0f);

    // 巡逻圆心 = 出生点。无目标时 AI 会围绕它巡环，主动创造接触机会。
    patrolHome_ = position;
    patrolTarget_ = position;
}

Enemy::~Enemy() = default;

void Enemy::setLineOfSightQuery(AI::LineOfSightQuery query) {
    // 本地留一份：开火前的"枪口到目标之间有没有墙"要用它，见 losQuery_ 的注释。
    losQuery_ = query;

    if (!perception_) {
        return;
    }
    // 【修复】原先这里整体重建 PerceptionSystem（`make_unique` 一个新实例），
    // 于是 records_ 里的最后已知位置、已确认目标、反应计时全部清零 ——
    // 「换一次查询函数，AI 就失忆一次」。若在运行中调用（例如关卡动态加载
    // 地形后重设查询），还会观察到敌人突然忘记玩家在哪。
    // 现在改用 setter 换函数，运行时状态完全保留。
    //
    // 注意 query 被复制而非移动进感知系统：本地还需要留一份。
    perception_->setLineOfSightQuery(query);
}

void Enemy::setThreatSources(const std::vector<glm::vec2>& threats) {
    // 【优化】内容没变就不写。set() 会拷贝整个 vector（含堆分配），
    // 而 World 每逻辑步对每个敌人都调用一次，等于每步 N 次分配。
    // 威胁源/掩体在绝大多数步里是完全不变的。
    if (const std::vector<glm::vec2>* stored =
            blackboard_.getPtr<std::vector<glm::vec2>>("world.threatSources")) {
        if (*stored == threats) {
            return;
        }
    }
    // 黑板中存威胁源列表，供 EQS 上下文与行为树条件读取
    blackboard_.set("world.threatSources", threats, "World");
}

void Enemy::setSquadIntel(glm::vec2 position, double seenTime) {
    // 【"新情报"的判定】只有报告时刻**更晚**时才算新线索。
    // World 每步都会广播同一份情报（只要那个队友还在看着目标），
    // 若每步都当成新线索，就会每步把"已放弃搜索"清掉 —— 敌人会因为
    // 队友一直看着目标而永远搜索下去，而不是走过去确认一下再放弃。
    const bool isNewerReport = (seenTime > squadIntelTime_ + 1e-6);

    squadIntelPosition_ = position;
    squadIntelTime_     = seenTime;
    hasSquadIntel_      = true;

    // 【只有"位置真的变了"才值得重新出动】
    //
    // 队友一直盯着同一个地方时，报告的时间戳每步都在更新。若把"时间戳更新"
    // 当成"有新线索"，已经去查看过（并放弃）的敌人会被反复唤醒：走到那个点、
    // 站着观察、放弃、再被唤醒……画面上就是"跑到 last location 点位发呆"。
    //
    // 判据因此改成"报告位置相对上次查看过的点移动了足够远"。
    const bool movedSinceLastCheck =
        !hasInvestigatedLead_ ||
        vectorLength(position - lastInvestigatedLead_) > config_.searchRearmDistance;

    if (isNewerReport && movedSinceLastCheck) {
        // 队友在新的地方看到了目标：允许重新出动去那边看一眼。
        searchAbandoned_   = false;
        searchArrivedTime_ = -1e9;
    }

    // 同步进黑板，便于调试面板/行为树条件直接观察（也保证"情报"这件事
    // 只有一个来源：World 写的值）。
    blackboard_.set(AI::Blackboard::Keys::kSquadIntelPosition, position, "World", seenTime);
    blackboard_.set(AI::Blackboard::Keys::kSquadIntelTime, seenTime, "World", seenTime);
}

void Enemy::clearSquadIntel() noexcept {
    hasSquadIntel_ = false;
    (void)blackboard_.erase(AI::Blackboard::Keys::kSquadIntelPosition);
    (void)blackboard_.erase(AI::Blackboard::Keys::kSquadIntelTime);
}

bool Enemy::searchLead(double gameTime, glm::vec2& outPosition) const noexcept {
    // 1) 自己的记忆（searchDuration 内有效）
    if (hasLastKnownPosition_ && (gameTime - lastSeenEnemyTime_) < config_.searchDuration) {
        outPosition = lastKnownEnemyPosition_;
        return true;
    }

    // 2) 队友共享的情报：自己没线索时，"别人最后看到的位置"就是最好的方向。
    //    这条正是"队友之间更新 last location" —— 没看见目标的敌人不再原地
    //    打转，而是朝队友报的点包抄过去。
    if (hasFreshSquadIntel(gameTime)) {
        outPosition = squadIntelPosition_;
        return true;
    }

    return false;
}

void Enemy::setCoverPositions(const std::vector<glm::vec2>& covers) {
    if (const std::vector<glm::vec2>* stored =
            blackboard_.getPtr<std::vector<glm::vec2>>("world.coverPositions")) {
        if (*stored == covers) {
            return;
        }
    }
    // 掩体位置单独存放：EQS 的「靠近掩体」测试读这个键，
    // 与威胁源彻底分离，避免两种语义互相污染（详见 World::collectThreatSources）。
    blackboard_.set("world.coverPositions", covers, "World");
}

// ===========================================================================
//  EQS 查询定义
// ===========================================================================
void Enemy::buildEQSQueries() {
    // 统一的「候选点必须站得下」筛选器。
    //
    // 【为什么三个查询都要挂上它】
    // EQS 的候选点由 gridGenerator 在目标附近撒网格生成，完全不看地形——
    // 其中相当一部分会落在墙里、地图外，或者"虽然格心可通行但身体会插进
    // 墙里"。选中这样的点会带来两种可见的坏行为：
    //   · 寻路找不到目标 → 退化为直线推进 → 敌人贴着墙原地推（看起来卡住）
    //   · 走进墙体边缘 → 被碰撞解算反复推回 → 抖动
    //
    // 判定用 NavGrid::canStand（半径感知 + 越界即阻挡），与寻路、生成点
    // 使用同一把尺子。构造期 navGrid_ 可能还是 nullptr（World 先建实体、
    // 后注入网格），所以这里必须捕获 this 而不是值。
    const auto standable = [this](const glm::vec2& candidate, const AI::EQSContext&) {
        if (navGrid_ == nullptr || !navGrid_->isInitialized()) {
            return true;   // 没有网格信息时不限制（测试里常这样构造）
        }
        return navGrid_->canStand(candidate, radius());
    };

    // -----------------------------------------------------------------------
    //  查询 1：寻找掩体位置
    //  意图：「一个我能打到目标、但不容易被目标打到的位置」
    // -----------------------------------------------------------------------
    coverSeekQuery_.name = name_ + ".FindCover";
    coverSeekQuery_.maxResults = 1;
    coverSeekQuery_.minAcceptableScore = 0.05f;

    // 以自身为中心、目标方向偏移的网格。
    // 生成器在查询执行时才拿到上下文，因此可以围绕「自身与目标的中点」布点，
    // 这比围绕自身布点更容易找到有战术意义的位置。
    coverSeekQuery_.generator = [](const AI::EQSContext& context) {
        // 在自身周围 320 半径内布 64 间距的网格
        const glm::vec2 center = context.hasTarget
                                     ? (context.selfPosition + context.targetPosition) * 0.5f
                                     : context.selfPosition;
        return AI::EnvironmentQuerySystem::gridGenerator(center, 220.0f, 44.0f, 6.0f)(context);
    };

    // 测试 1：离目标不能太远（还要能打到）
    coverSeekQuery_.tests.push_back(
        AI::EnvironmentQuerySystem::distanceToTargetTest(
            /*weight=*/0.25f, /*optimal=*/120.0f, /*max=*/500.0f));

    // 测试 2：远离威胁源（不被压制）
    coverSeekQuery_.tests.push_back(
        AI::EnvironmentQuerySystem::threatAvoidanceTest(/*weight=*/0.30f, /*safeRadius=*/260.0f));

    // 测试 3：离自己近（容易走到，避免为了「完美掩体」跑半张地图）
    coverSeekQuery_.tests.push_back(
        AI::EnvironmentQuerySystem::distanceFromSelfTest(/*weight=*/0.25f, /*max=*/400.0f));

    // 测试 4：掩护度
    //
    // 【这个测试项原本是"距离代理"，现已换成真实几何判定】
    //
    // 旧实现只算「候选点到最近掩体中心的距离」，于是它测的是
    // "我离某个掩体多近"，而不是"这个掩体对我的交火有没有用"。
    // 后果很具体：掩体落在AI 的**侧后方**时，距离同样近、得分同样高，
    // 但它一点用都没有——AI 会为一个毫无作用的掩体跑过去。
    // 权重 0.20，占四分之一的决策依据却是无效的。
    //
    // 新判据见 EnvironmentQuerySystem::coverEffectivenessTest 的注释：
    // 量的是「掩体到**我→目标连线**的距离」，即掩体是否处在交火路径上。
    //
    // 判据本身在 AI 层（EQS.cpp），那里可以独立单测；
    // 本项目上一版把它写成 lambda 塞在这里，结果**无法测试**——
    // 只能靠跑无头模拟间接观察，而那很难区分"掩体选对了"
    // 与"恰好选了个别的位置"。凡是可复用的判据都该进 AI 层。
    coverSeekQuery_.tests.push_back(
        AI::EnvironmentQuerySystem::coverEffectivenessTest(/*weight=*/0.20f));

    // 硬约束：候选点必须能与目标通视，否则 AI 会躲进掩体后再也开不了枪。
    //
    // 【这一条是本次改动里最关键的部分，删掉它整个修复就是负的】
    // AimAndFire 里有一道 LoS 检查（"弹道别对着掩体开"）：
    // 枪口到目标之间被挡就返回 Running，一枪都不开。
    // 而"掩护度"按新定义天然偏好「我看不见目标」的点——
    // 于是 AI 会一本正经地找一个打不了枪的掩体蹲着，
    // 表现从"选错位置"退化成"完全不开火"。比原来的 bug 更糟。
    //
    // 两条约束合起来才是完整的战术语义：
    //   CoverEffectiveness 高 = 我与目标之间有东西挡着（我比较安全）
    //   本过滤器             = 但我仍能通视目标（我还能还手）
    // 也就是「有掩护的战斗位」而不是「安全但无力的躲藏点」。
    //
    // 捕获 this 而非 navGrid_ 的值：buildEQSQueries 在构造函数里被调用，
    // 此时 World 尚未通过 setNavGrid 注入网格（navGrid_ 仍为 nullptr）。
    coverSeekQuery_.candidateFilter =
        [this, standable](const glm::vec2& candidate, const AI::EQSContext& context) {
            // 约束一：候选点必须真的站得下（半径感知 + 不许出图）。
            // 旧写法是 `isWalkable(worldToGrid(candidate))`——那只看了格心，
            // 而且会被钳制后的坐标骗过（地图外的点被当成边界格可通行）。
            if (!standable(candidate, context)) {
                return false;
            }

            // 新增约束：必须能与目标通视，否则这个位置毫无用处
            // （理由见上方注释）。
            //
            // 注意 losQuery_ 而不是 navGrid_：losQuery_ 是 World 注入的
            // 同一份查询，感知系统与开火判断都用它。用同一份判据才能保证
            // 「EQS 认为能打」与「AimAndFire 真的能开火」一致——
            // 若这里用另一套判据，两边就会打架。
            if (context.hasTarget && losQuery_ && !losQuery_(candidate, context.targetPosition)) {
                return false;
            }
            return true;
        };

    // -----------------------------------------------------------------------
    //  查询 2：撤退位置
    //  意图：「离所有威胁尽可能远，且自己能走到」
    // -----------------------------------------------------------------------
    retreatQuery_.name = name_ + ".FindRetreat";
    retreatQuery_.maxResults = 1;

    retreatQuery_.generator = [](const AI::EQSContext& context) {
        // 撤退点应该在「背离目标」的方向上：以自身为起点，
        // 朝目标反方向偏移一段距离作为搜索中心。
        glm::vec2 awayDirection{1.0f, 0.0f};
        if (context.hasTarget) {
            awayDirection = normalizeOr(context.selfPosition - context.targetPosition,
                                        glm::vec2(1.0f, 0.0f));
        }
        const glm::vec2 center = context.selfPosition + awayDirection * 260.0f;
        return AI::EnvironmentQuerySystem::gridGenerator(center, 180.0f, 45.0f, 8.0f)(context);
    };

    retreatQuery_.tests.push_back(
        AI::EnvironmentQuerySystem::distanceFromTargetTest(/*weight=*/0.5f,
                                                           /*safe=*/300.0f,
                                                           /*max=*/700.0f));
    retreatQuery_.tests.push_back(
        AI::EnvironmentQuerySystem::threatAvoidanceTest(/*weight=*/0.3f, /*safeRadius=*/300.0f));
    retreatQuery_.tests.push_back(
        AI::EnvironmentQuerySystem::distanceFromSelfTest(/*weight=*/0.2f, /*max=*/500.0f));

    // 撤退点也必须站得下：否则"撤离"会变成"贴着墙推"，
    // 看起来和卡住没有区别。
    retreatQuery_.candidateFilter = standable;

    // -----------------------------------------------------------------------
    //  查询 3：接近位置（拉近到理想射程）
    // -----------------------------------------------------------------------
    approachQuery_.name = name_ + ".FindApproach";
    approachQuery_.maxResults = 1;

    approachQuery_.generator = [](const AI::EQSContext& context) {
        // 在自身周围布点，测试项会挑出「接近但不过近」的位置
        return AI::EnvironmentQuerySystem::gridGenerator(context.selfPosition, 200.0f, 40.0f,
                                                         5.0f)(context);
    };

    approachQuery_.tests.push_back(
        AI::EnvironmentQuerySystem::distanceToTargetTest(/*weight=*/0.6f,
                                                         /*optimal=*/200.0f,
                                                         /*max=*/450.0f));
    approachQuery_.tests.push_back(
        AI::EnvironmentQuerySystem::threatAvoidanceTest(/*weight=*/0.4f, /*safeRadius=*/180.0f));

    approachQuery_.candidateFilter = standable;
}

// ===========================================================================
//  行为树构建
// ===========================================================================
void Enemy::buildBehaviorTree() {
    // 本文件的 lambda 大量引用 AI 层类型（EQSContext / EQSResult / 各节点类），
    // 逐个写 using 只会让签名更难读，统一引入见文件顶部。

    // 便捷的类型别名：让下面这棵树读起来接近伪代码
    //
    // 关键：这里的 condition() 必须让条件「每步重新求值」。
    //
    // ConditionNode 默认会缓存首次求值的结果（reEvaluateEveryTick=false），
    // 这对「血量 > 30%」这类在一个分支执行期内应当保持稳定的条件是对的。
    // 但在本树中，所有条件判断的都是**外部世界**（是否看见敌人、有没有
    // 最后已知位置、距上次看见多久），它们会逐帧变化。若沿用默认缓存，
    // 条件的结果会在第一次求值后被永久冻结——表现为「敌人一旦没看见玩家
    // 就再也看不见」，因为 HasEngageableTarget 在 t=0 被缓存为 false 后
    // 永不重算。
    //
    // 对于反应式行为树，外部世界的条件必须每步重新采样。
    auto condition = [](std::string name, ConditionNode::Predicate predicate) {
        return std::make_unique<ConditionNode>(std::move(name), std::move(predicate),
                                               /*reEvaluateEveryTick=*/true);
    };
    auto action = [](std::string name, ActionNode::Action fn) {
        return std::make_unique<ActionNode>(std::move(name), std::move(fn));
    };

    // 注意：这些 lambda 捕获的是 `this`，因为动作需要修改 Enemy 状态。
    // 行为树的生命周期严格短于 Enemy，因此裸指针捕获是安全的。

    // -----------------------------------------------------------------------
    //  根选择器：按优先级从高到低排列
    //
    //    1. 血量过低        -> 撤退（最高优先级：活下来比什么都重要）
    //    2. 有确认目标      -> 交战（EQS 选位 + 射击）
    //    3. 有最后已知位置  -> 搜索前进
    //    4. 什么都没有      -> 巡逻（主动搜索）
    //
    // 这个选择器**必须设为反应式**。
    //
    // 默认的非反应式选择器会「记住」当前 Running 的分支，直到它结束才
    // 重新评估更高优先级的分支。对本树而言，巡逻分支永远返回 Running，
    // 于是 AI 一旦进入巡逻就再也不会回头检查「我是不是看见敌人了」——
    // 表现为敌人明明从玩家面前走过却毫无反应，战斗永远不触发。
    //
    // 反应式语义让每一步都从「撤退」开始重新评估，使 AI 能对外部威胁
    // 的突然出现做出即时反应。这正是「战术 AI 可信度」的核心。
    // -----------------------------------------------------------------------
    auto root = reactiveSel(name_ + ".Root",

        // ---------- 分支 1：撤退 ----------
        seq(name_ + ".RetreatBranch",
            condition(name_ + ".LowHealth",
                      [this](const TickContext&) {
                          return healthRatio() < config_.retreatHealthRatio && !isDead();
                      }),
            // 撤退点的重评估节流同样内联处理：cooldown() 在冷却期返回
            // Failure 会让 Sequence 短路，AI 受伤后反而站着不动。
            action(name_ + ".FindRetreatPosition",
                   [this](TickContext& context) {
                       if (isDead()) { return NodeStatus::Failure; }

                       // 节流窗口内沿用上次撤退点
                       if (retreatPositionSelected_ &&
                           context.gameTime - lastRetreatTime_ < 1.5) {
                           return NodeStatus::Success;
                       }

                       EQSContext eqsContext = buildEQSContext(context);
                       const EQSResult result = eqs_->run(retreatQuery_, eqsContext);
                       lastPositionQuery_ = result;

                       lastRetreatTime_ = context.gameTime;
                       retreatPositionSelected_ = true;

                       if (const auto position = result.bestPosition()) {
                           moveTarget_ = *position;
                           blackboard_.set(Blackboard::Keys::kMoveTarget, *position,
                                           "RetreatBranch");
                           setState(AIState::Retreating, context.gameTime);
                           repathRequested_ = true;
                       }
                       // 即使 EQS 没选出位置也返回 Success：让下面的移动动作
                       // 继续沿用旧目标撤离，而不是让整条撤退分支失效。
                       return NodeStatus::Success;
                   }),
            // 移动动作持续 Running 直到到达
            action(name_ + ".MoveToRetreat",
                   [this](TickContext& context) {
                       if (isDead()) { return NodeStatus::Failure; }

                       // 【修复】这里**不能**用 resolveTargetPosition()。
                       // 那个辅助函数读的是 kLastKnownPosition（玩家最后已知
                       // 位置），它一写入就长期存在；于是"撤退"会变成"朝玩家
                       // 冲锋"——低血量的敌人反而直扑玩家。
                       //
                       // 撤退点由上面的 FindRetreatPosition 写入 moveTarget_
                       // 与 kMoveTarget，直接用它才是正确语义。
                       // 【修复】这里**不能**用 resolveTargetPosition()：
                       // 那个辅助函数读的是 kLastKnownPosition，也就是**玩家
                       // 的位置**，而撤退要走的是 FindRetreatPosition 选出的
                       // 撤退点（moveTarget_ / kMoveTarget）。
                       //
                       // 读错键的直接后果体现在 followPath 的降级路径上：
                       // 它有两条分支会在"路径耗尽/寻路失败"时改用
                       // moveDirectlyToward(destination) —— 一旦 destination
                       // 是玩家位置，敌人就会掉头直扑玩家；另外到达判定也会
                       // 拿"到玩家的距离"来算，撤退动作因此永远判不到达。
                       // （正常路径下这个错误会被"撤退点每 1.5 秒重选 + 寻路
                       // 路径占主导"掩盖住，所以它不容易在画面上看出来。）
                       const bool arrived = followPath(context.fixedDelta, moveTarget_);
                       return arrived ? NodeStatus::Success : NodeStatus::Running;
                   })),

        // ---------- 分支 2：交战 ----------
        seq(name_ + ".EngageBranch",
            condition(name_ + ".HasEngageableTarget",
                      [this](const TickContext&) {
                          // 类型必须与感知写入时一致（EntityId 别名），
                          // 否则 has<T>() 会因 type_index 不匹配而恒为 false，
                          // AI 会永远停在非交战分支。
                          if (!blackboard_.has<EntityId>(Blackboard::Keys::kNearestEnemy)) {
                              return false;
                          }

                          // 【失联太久就不再赖在交战分支】
                          //
                          // 感知的"确认目标"在 memoryDuration（默认 6 秒）内
                          // 一直为真 —— 那是"记忆"的定义，不是"还看得见"。
                          // 拿它当交战门槛，敌人就会在目标准消失之后原地站 6 秒，
                          // 对着最后已知位置干等（并且一直开枪，见 AimAndFire）。
                          //
                          // 超过宽限期交给搜索分支：真的走过去找，而不是站着发呆。
                          // 注意用 kTimeSinceSeenEnemy（真看见才归零），
                          // 不能用"确认"，否则等于没改。
                          const float sinceSeen = blackboard_.getOr<float>(
                              Blackboard::Keys::kTimeSinceSeenEnemy, 1e6f);
                          return static_cast<double>(sinceSeen) <= config_.engageLostGrace;
                      }),

            // 2a. 位置选择（EQS）。
            //
            // 关键设计约束：这个动作在 Sequence 中位于「移动与射击」之前，
            // 因此它**绝不能返回 Failure**，否则整条交战分支会短路，AI 会
            // 在明明看得见目标时退回到巡逻分支（这正是无头测试中
            // state=Patrolling / shots=0 的原因）。
            //
            // 早期版本把它包在 cooldown() 里：CooldownNode 在冷却期返回
            // Failure，而 Sequence 遇 Failure 即短路 → 每次选位成功后
            // 的 repositionInterval 秒内整条交战分支都失效。改为把节流
            // 逻辑内联：不满足重算条件时直接沿用当前 moveTarget_ 并返回
            // Success，成功链因此从不被打断。
            action(name_ + ".SelectCombatPosition",
                   [this](TickContext& context) {
                       const float distance =
                           blackboard_.getOr<float>(Blackboard::Keys::kNearestEnemyDist, 1e6f);

                       const bool cooldownElapsed =
                           context.gameTime - lastRepositionTime_ >= config_.repositionInterval;

                       // 【紧急重评估：目标跑远了我们却站在原地】
                       //
                       // 节流窗口 2.5 秒内不重新选位本身是对的（否则 AI 像在原地
                       // 打转），但如果敌人**已经到位**、而目标在此期间跑到射程外
                       // （玩家 2.5 秒能跑 450 单位），继续等窗口就等于罚站。
                       // 实测这是"敌人长时间够不着、不开枪"的最大来源：
                       // 走位目标下每个敌人有 2.4 秒以上的连续时段卡在这里。
                       //
                       // 因此：站定 + 目标已脱离理想交战带 ⇒ 立刻重新选位。
                       const bool standingStill =
                           vectorLength(moveTarget_ - position()) < 24.0f;
                       const bool targetEscaped =
                           distance > config_.fireRange ||
                           distance > config_.preferredRange * 1.35f;
                       const bool urgentReevaluate = standingStill && targetEscaped;

                       // 节流窗口内、且当前决策依然成立：沿用上一次的决策。
                       if (!cooldownElapsed && engagePositionSelected_ && !urgentReevaluate) {
                           return NodeStatus::Success;
                       }

                       // 一次「有效评估」：无论走哪条路径，成功后都要打时间戳。
                       const auto commit = [this, &context]() {
                           lastRepositionTime_ = context.gameTime;
                           engagePositionSelected_ = true;
                           return NodeStatus::Success;
                       };

                       if (!config_.useEQS) {
                           // EQS 关闭时退化为「直接朝目标走」
                           glm::vec2 targetPosition;
                           if (resolveTargetPosition(blackboard_, targetPosition)) {
                               moveTarget_ = targetPosition;
                               blackboard_.set(Blackboard::Keys::kMoveTarget, targetPosition,
                                               "EngageNoEQS");
                               repathRequested_ = true;
                               return commit();
                           }
                           // 拿不到目标也返回 Success（保持交战分支存活），
                           // 让并行子节点里的瞄准逻辑去处理。
                           return commit();
                       }

                       // 距离合适就原地打，不折腾位置。
                       // 这是「有 EQS 但不过度使用」的关键——
                       // 每帧都换位置会让 AI 看起来像在原地打转。
                       const float idealRange = config_.preferredRange;
                       if (distance < idealRange * 1.35f && distance > idealRange * 0.6f) {
                           const glm::vec2 selfPos = position();
                           blackboard_.set(Blackboard::Keys::kMoveTarget, selfPos,
                                           "EngageHoldPosition");
                           moveTarget_ = selfPos;
                           return commit();
                       }

                       EQSContext eqsContext = buildEQSContext(context);
                       const EQSQuery& query = (distance > idealRange * 1.35f)
                                                   ? approachQuery_
                                                   : coverSeekQuery_;
                       const EQSResult result = eqs_->run(query, eqsContext);
                       lastPositionQuery_ = result;

                       if (const auto position = result.bestPosition()) {
                           moveTarget_ = *position;
                           blackboard_.set(Blackboard::Keys::kMoveTarget, *position,
                                           "EngageReposition");
                           setState(AIState::Repositioning, context.gameTime);
                           repathRequested_ = true;
                           return commit();
                       }

                       // EQS 选不出位置（候选全被过滤 / 分数过低）时的兜底：
                       // 直接朝目标推进。有位置优选，没有位置也要能打，
                       // 这是战斗 AI 的底线。
                       glm::vec2 fallbackTarget;
                       if (resolveTargetPosition(blackboard_, fallbackTarget)) {
                           moveTarget_ = fallbackTarget;
                           blackboard_.set(Blackboard::Keys::kMoveTarget, fallbackTarget,
                                           "EngageFallback");
                           repathRequested_ = true;
                       }
                       return commit();
                   }),

            // 2b. 移动（与射击并行）
            // 策略放在名称之后、子节点之前：移动与射击互不阻塞，
            // 任一成功即整体成功（射击成功不必等移动走完）。
            par(name_ + ".EngageParallel",
                ParallelNode::SuccessPolicy::RequireOne,
                ParallelNode::FailurePolicy::RequireAll,

                // 并行子节点 A：移动到战斗位置
                action(name_ + ".CombatMove",
                       [this](TickContext& context) {
                           if (isDead()) { return NodeStatus::Failure; }
                           if (const auto target = blackboard_.get<glm::vec2>(
                                   Blackboard::Keys::kMoveTarget)) {
                               moveTarget_ = *target;
                           }
                           const bool arrived = followPath(context.fixedDelta, moveTarget_);
                           return arrived ? NodeStatus::Success : NodeStatus::Running;
                       }),

                // 并行子节点 B：瞄准与射击（带冷却）
                action(name_ + ".AimAndFire",
                       [this](TickContext& context) {
                           if (isDead()) { return NodeStatus::Failure; }

                           glm::vec2 targetPosition;
                           if (!resolveTargetPosition(blackboard_, targetPosition)) {
                               return NodeStatus::Failure;
                           }

                           // 瞄准点带提前量：子弹要飞一段时间才到，
                           // 因此必须朝"目标将会在的位置"开枪，而不是它现在的位置。
                           // 详见 EnemyConfig::aimPrediction 的注释。
                           const glm::vec2 aimTarget = predictedAimPoint(targetPosition,
                                                                        context.gameTime);
                           aimPoint_ = aimTarget;   // 供调试绘制

                           // 转向目标。转身是渐进的，因此瞄准需要时间，
                           // 这自然产生了「AI 也需要瞄准」的感觉。
                           //
                           // 【先转向，再判射程】
                           //
                           // 原实现在"超出射程"时直接返回，连枪口都不转。后果是
                           // 追击途中枪口一直被 applyMovement 拽向**移动方向**，
                           // 等终于进入射程时还要额外花 0.3~0.9 秒把枪口转回来
                           // 瞄准 —— 实测这部分占掉了开火间隔的约 30%。
                           // 现在无论远近都保持枪口指向目标，进入射程即可开火。
                           steerFacingToward(normalizeOr(aimTarget - position(), facing_),
                                             context.fixedDelta);
                           aimSteeredThisStep_ = true;

                           const float distance = vectorLength(targetPosition - position());
                           if (distance > config_.fireRange) {
                               // 够不着：继续瞄准 + 继续追（见 desiredMoveSpeed 的
                               // "够不着就全速追击"），但这一枪不开。
                               // 返回 Running 而不是 Failure：交战分支必须保持存活，
                               // 否则 Sequence 短路会让 AI 丢掉目标转去巡逻。
                               return NodeStatus::Running;
                           }

                           // 【开火条件 0：目标此刻还得"在"】
                           //
                           // targetPosition 来自 kLastKnownPosition，也就是敌人
                           // **记忆**中的位置。玩家绕到背后 / 躲进掩体 / 跑出
                           // 视野之后，这个位置会停在原地，而"确认目标"在
                           // memoryDuration 内仍然成立 —— 于是敌人会对着一个
                           // 空位置连续开枪（实测：玩家瞬移走后，敌人对着
                           // 762 单位外的空点打出 7 发，持续整整 6 秒）。
                           //
                           // 下面的 LoS 检查救不了它：那个空点自己有视线。
                           // 判据必须换成"距上次**真正看见**多久"——
                           // kTimeSinceSeenEnemy 由 Perception::timeSinceLastSeen
                           // 写入，只在真看见时归零（"确认"不等于"看见"）。
                           //
                           // 返回 Running 而不是 Failure：仍然保持交战分支存活，
                           // 目标一旦露头就能立刻接上（转身/冷却都还在保持）。
                           if (blackboard_.getOr<float>(Blackboard::Keys::kTimeSinceSeenEnemy, 1e6f) >
                               config_.fireTargetMaxAge) {
                               return NodeStatus::Running;   // 记忆里的位置：瞄着，但别开枪
                           }

                           // 【开火条件：弹道真的会打中，而不是"角度差不多"】
                           //
                           // 旧版本用固定角度容差（0.20 rad ≈ 11.5°）判断"对准了"。
                           // 在瞬时命中的模型下这没问题——反正伤害一定会结算，
                           // 角度只影响画面。
                           //
                           // 但子弹真的要沿 facing 飞出去之后，11.5° 的容差在
                           // 220 单位的交战距离上意味着横向偏差高达 44 个单位，
                           // 而命中窗口只有"敌人半径 11 + 子弹半径 3" = 14。
                           // 也就是说：照旧逻辑开火，命中率几乎是 0。
                           //
                           // 正确的判据是把角度换算成**横向偏差**，再和目标
                           // 尺寸比较——也就是"这一枪会不会打到人"。
                           // 角度会随距离放大误差，横向偏差不会，所以只有后者
                           // 才是一个与距离无关的、可以直接和半径比较的量。
                           if (lateralAimError(aimTarget) > config_.aimHitRadius) {
                               return NodeStatus::Running;   // 还打不中，继续瞄
                           }

                           // 弹道别对着掩体开。瞄准点用的是"最后已知位置"，
                           // 目标可能已经躲到墙后面了——这种情况下开枪只会
                           // 把子弹送进掩体，浪费一次冷却，画面上还像 AI 在打墙。
                           if (losQuery_ && !losQuery_(position(), aimTarget)) {
                               return NodeStatus::Running;   // 没有通路，等目标露头
                           }

                           // 【冷却】对准了不等于能开火。
                           //
                           // 这一句是"敌人能秒杀我"的修复点。缺了它，AI 每个
                           // 逻辑步都会开一枪（60Hz → 每秒 60 发），单体 DPS 高达
                           // 伤害 × 60。加上冷却后，射速由 fireCooldown 决定。
                           //
                           // 冷却未到时返回 Running 而不是 Failure：Failure 会让
                           // Sequence 短路，整条交战分支（包括移动）都会失效，
                           // AI 会在冷却期间僵在原地。Running 则保持分支存活。
                           if (context.gameTime - lastFireTime_ <
                               static_cast<double>(config_.fireCooldown)) {
                               return NodeStatus::Running;   // 枪管还在冷却
                           }

                           // 对准且冷却已过：记录开火意图，由 World 发射一颗真的子弹。
                           lastFireTime_ = context.gameTime;
                           blackboard_.set(Blackboard::Keys::kShouldFire, true, kDecisionWriter);
                           firedThisStep_ = true;
                           ++shotsFired_;
                           setState(AIState::Engaging, context.gameTime);
                           return NodeStatus::Success;
                       }))),

        // ---------- 分支 3：搜索（有线索但看不见目标） ----------
        //
        // 线索有两个来源：自己的记忆、队友共享的情报（见 Enemy::searchLead）。
        // 名称从 HasLastKnownPosition 改为 HasSearchLead：它现在不只认"自己
        // 看到的"，还认"队友报的"，旧名字会让人以为这里只看自己的记忆。
        seq(name_ + ".SearchBranch",
            condition(name_ + ".HasSearchLead",
                      [this](const TickContext& context) {
                          // 只在「没有确认目标」时才会走到这里（选择器已保证），
                          // 因此只需检查线索是否还有效
                          //
                          // 已经搜完并放弃：在重新发现目标（或收到更新鲜的队友
                          // 情报）之前不再重试。不加这一条的话，动作每步都会
                          // 立刻返回 Failure ——行为上没错（等于一直巡逻），
                          // 但每步都白跑一次 followPath。
                          if (searchAbandoned_) {
                              return false;
                          }
                          glm::vec2 lead{0.0f, 0.0f};
                          return searchLead(context.gameTime, lead);
                      }),
            action(name_ + ".SearchLastKnown",
                   [this](TickContext& context) {
                       if (isDead()) { return NodeStatus::Failure; }

                       setState(AIState::Patrolling, context.gameTime);

                       // 条件已保证线索有效，这里再取一次是为了拿到"往哪走"：
                       // 自己的记忆优先，其次队友情报。
                       glm::vec2 lead{0.0f, 0.0f};
                       if (searchLead(context.gameTime, lead)) {
                           // 线索换了个地方就重新寻路。不显式请求的话，
                           // 敌人会沿着"上一条路"（多半是巡逻路线）继续走，
                           // 看似在执行搜索，实际越走越远。
                           if (vectorLength(lead - moveTarget_) > 48.0f) {
                               repathRequested_ = true;
                           }
                           moveTarget_ = lead;

                           // 记下"正在查看的线索点"：队友之后继续报同一个位置时，
                           // 不该再把我们重新唤醒一次（见 setSquadIntel 的说明）。
                           lastInvestigatedLead_ = lead;
                           hasInvestigatedLead_  = true;
                       }

                       const bool arrived = followPath(context.fixedDelta, moveTarget_);

                       if (!arrived) {
                           // 还在路上：清掉"已到达"的记录。
                           // 目标移动会让 arrived 反复真假交替，不清就会
                           // 拿上一段路的到达时刻去算观察时间。
                           searchArrivedTime_ = -1e9;
                           searchAbandoned_   = false;
                           return NodeStatus::Running;
                       }

                       // 到了。记录到达时刻（只在第一次到达时写）。
                       if (searchArrivedTime_ < -1e8) {
                           searchArrivedTime_ = context.gameTime;
                       }

                       // --- 观察窗口：环顾四周，而不是站桩 ---
                       //
                       // 期间保持 Running —— 站住不动但仍在"做搜索这件事"，
                       // 所以不该让选择器往下走。
                       //
                       // 【为什么必须摆动朝向】原先这两秒是完全静止的：位置不动、
                       // 朝向不动，画面上就是一个人在原地发呆。改成以"线索方向"
                       // 为中心左右扫视（sin 曲线来回摆），观感是"到点、环顾、
                       // 没人、走"。摆动由 applyMovement 里的渐进转向跟随，
                       // 因此是有惯性的转头，不是瞬间贴脸。
                       const double holdElapsed = context.gameTime - searchArrivedTime_;
                       if (holdElapsed < config_.searchHoldDuration) {
                           constexpr float kTwoPi = 6.28318530717958647692f;
                           constexpr float kSearchSweepAngle = 0.9f;   // ±约 52°

                           const glm::vec2 toLead =
                               normalizeOr(moveTarget_ - position(), facing_);
                           const float baseAngle = std::atan2(toLead.y, toLead.x);
                           const float duration =
                               static_cast<float>(std::max(config_.searchHoldDuration, 1e-3));
                           const float progress = static_cast<float>(holdElapsed) / duration;
                           const float sweep = std::sin(progress * kTwoPi) * kSearchSweepAngle;

                           desiredFacing_ = {std::cos(baseAngle + sweep),
                                             std::sin(baseAngle + sweep)};
                           return NodeStatus::Running;
                       }

                       // 【修复】这里原先是
                       //     if (arrived) { return Running; }  return Running;
                       // 两个分支返回同一个值，"放弃"从未发生，
                       // AI 到了最后已知位置就永久站住，直到 8s 后
                       // HasLastKnownPosition 条件失效才切巡逻。
                       //
                       // 放弃搜索的正确做法是让本分支**失败**：Sequence 遇
                       // Failure 短路，整个 SearchBranch 返回 Failure，
                       // 根选择器（reactive）随即落到下一个分支——巡逻。
                       // 返回 Success 则是错的：它会让根选择器认为
                       // "搜索成功了"，同样停在原地，且不给出任何放弃信号。
                       searchAbandoned_ = true;
                       return NodeStatus::Failure;
                   })),

        // ---------- 分支 4：巡逻 / 待命 ----------
        // 注意：这里必须让 AI「动起来」，而不是原地静止扫视。
        //
        // 早期的实现让 Idle 分支把速度清零、只做原地扫视，结果在无头冒烟
        // 测试里暴露出严重问题：默认场景中敌人的初始朝向与玩家位置恰好
        // 落在 90° 视野锥之外，于是 AI 永远看不见玩家 → 永远停在 Idle
        // → 永远不动 → 更不可能看见玩家。一个「站桩装死」的敌人不是
        // 战术 AI，而是场景装饰。
        //
        // 真实的巡逻 AI 应当主动扩大接触概率：沿着一组巡逻点移动，
        // 在移动中自然改变位置与朝向，从而有机会发现目标。这里用
        // 「围绕出生点的环形巡逻点」实现，既无需额外数据，又能覆盖
        // 各个方向，逐帧提高发现玩家的概率。
        //
        // 实现注意：巡逻「选点」与「移动」必须在同一个动作里完成，
        // 不能拆成 cooldown(选点) + 移动 的 Sequence。
        // 原因是 CooldownNode 在冷却期内返回 Failure，而 Sequence 遇
        // Failure 即短路——那会让整个巡逻分支在冷却期整个失效，AI 原地
        // 停住（这正是上一版的表现：敌人走到第一个巡逻点后就不动了）。
        // 把选点逻辑内联进移动动作，用标志位控制重算频率，既能节流又
        // 不会破坏 Sequence 的成功链。
        action(name_ + ".Patrol",
               [this](TickContext& context) {
                   if (isDead()) { return NodeStatus::Failure; }

                   setState(AIState::Patrolling, context.gameTime);

                   // --- 节流：距上次选点不足 kRetargetInterval 秒时复用旧点 ---
                   constexpr double kRetargetInterval = 1.0;

                   // 到达巡逻点的判定距离。比移动的到达阈值宽松：巡逻不需要
                   // 精确踩点，走到附近就算到过。
                   constexpr float kPatrolArriveDistance = 48.0f;

                   // 「迟迟走不到」的超时。没有它的话，只要选出一个够不到的
                   // 目标（比如地图外），敌人就会永远朝它推进、被边界顶住不动——
                   // 而且因为换点条件依赖"到达"，它连换点都做不到。
                   constexpr double kStuckTimeout = 2.5;

                   const float toTarget = vectorLength(patrolTarget_ - position());
                   const bool reached = toTarget < kPatrolArriveDistance;

                   // 进度监测：只有真的在靠近目标时才重置计时器。
                   if (toTarget < patrolBestDistance_ - 8.0f) {
                       patrolBestDistance_ = toTarget;
                       patrolStuckTime_ = 0.0;
                   } else if (!reached) {
                       patrolStuckTime_ += context.fixedDelta;
                   }

                   const bool throttled =
                       context.gameTime - lastPatrolRetargetTime_ >= kRetargetInterval;
                   const bool stuck = patrolStuckTime_ >= kStuckTimeout;

                   if (!patrolInitialized_ || ((reached || stuck) && throttled)) {
                       // 环形巡逻点：以出生点为圆心，按序号取角度。
                       //
                       // 【为什么是多个半径】
                       // 靠地图边缘或掩体密集处的敌人，260 半径的环上可能一个
                       // 可站立点都没有；若直接退化成"原地待命"，它就永久站住
                       // 不动（看起来就是"自己卡住了"）。所以半径由远及近逐圈
                       // 尝试，保证贴边的敌人至少能在近处巡逻。
                       constexpr float kPatrolRadii[] = {260.0f, 190.0f, 130.0f, 70.0f};
                       constexpr int   kPatrolPoints = 6;
                       constexpr float kTwoPi = 6.28318530717958647692f;
                       const float bodyRadius = radius();

                       bool found = false;
                       for (const float ringRadius : kPatrolRadii) {
                           for (int attempt = 0; attempt < kPatrolPoints && !found; ++attempt) {
                               const int index = patrolIndex_ % kPatrolPoints;
                               const float angle =
                                   (static_cast<float>(index) / static_cast<float>(kPatrolPoints))
                                   * kTwoPi;
                               const glm::vec2 candidate =
                                   patrolHome_ +
                                   glm::vec2(std::cos(angle), std::sin(angle)) * ringRadius;

                               ++patrolIndex_;

                               // 【关键】用"我这副身体能不能站在这里"判定，
                               // 而不是"格心可不可通行"。
                               //
                               // 旧写法 `isWalkable(worldToGrid(candidate))` 有两
                               // 个坑：不考虑实体半径（贴墙点被选中，走过去被推来
                               // 推去），更致命的是 worldToGrid 会把越界坐标**钳制**
                               // 到边界格——地图外的点因此被判成可通行，敌人被派到
                               // 地图外再也走不到，被边界约束顶住 → 卡死。
                               if (navGrid_ && navGrid_->isInitialized() &&
                                   !navGrid_->canStand(candidate, bodyRadius)) {
                                   continue;   // 这点站不下，试下一个
                               }
                               patrolTarget_ = candidate;
                               found = true;
                           }
                           if (found) {
                               break;
                           }
                       }

                       if (!found) {
                           // 连近圈都站不下（理论上只有整块区域被封死才会发生）：
                           // 原地待命，至少不要朝一个够不到的点硬推。
                           patrolTarget_ = position();
                       }

                       patrolInitialized_ = true;
                       lastPatrolRetargetTime_ = context.gameTime;
                       patrolStuckTime_ = 0.0;
                       patrolBestDistance_ = 1e9f;   // 新目标的进度重新计
                       moveTarget_ = patrolTarget_;
                       repathRequested_ = true;
                   }

                   // --- 移动 ---
                   // Running 而非 Success：巡逻是持续行为，永不「完成」，
                   // 这样它不会让根选择器短路掉后续分支。
                   //
                   // 到达判定由上面的 "toTarget < 48" 负责（巡逻点之间比较宽松），
                   // 因此这里刻意忽略 followPath 的返回值。
                   (void)followPath(context.fixedDelta, patrolTarget_);
                   return NodeStatus::Running;
               }));

    // 根选择器已通过 reactiveSel() 设为反应式：每步从最高优先级分支
    // 重新评估。若缺少这一步，巡逻分支会「粘住」选择器，交战分支永不触发。
    behaviorTree_->setRoot(std::move(root));
}

// ===========================================================================
//  每步驱动
// ===========================================================================
void Enemy::updateAI(double fixedDelta, double gameTime,
                     const std::vector<AI::PerceptTarget>& targets) {
    firedThisStep_ = false;
    aimSteeredThisStep_ = false;
    currentTime_ = gameTime;

    if (isDead()) {
        setState(AIState::Dead, gameTime);
        velocity_ = {0.0f, 0.0f};
        return;
    }

    // --- 流水线阶段 1：感知 ---
    updatePerception(gameTime, targets);

    // --- 阶段 2：位置查询（EQS）---
    updatePositionQueries(gameTime);

    // --- 阶段 3：寻路 ---
    updatePathfinding();

    // --- 阶段 4：决策 ---
    updateDecision(fixedDelta, gameTime);

    // --- 阶段 5：移动 ---
    applyMovement(fixedDelta);

    // 状态历史记录（环形保留最近 16 条）
    if (stateHistory_.empty() || stateHistory_.back().second != state_) {
        stateHistory_.emplace_back(gameTime, state_);
        if (stateHistory_.size() > 16) {
            stateHistory_.erase(stateHistory_.begin());
        }
    }
}

void Enemy::requestRepath() noexcept {
    repathRequested_ = true;
}

// ---------------------------------------------------------------------------
//  阶段 1：感知
// ---------------------------------------------------------------------------
void Enemy::updatePerception(double gameTime, const std::vector<AI::PerceptTarget>& targets) {
    perception_->update(position(), facing_, targets, gameTime);
    perception_->writeToBlackboard(blackboard_, position(), gameTime);

    // 记录「最后已知敌人位置」——即使当前看不到，也要让搜索分支有方向。
    // 这是 AI 显得有记忆的关键。
    if (const auto lastKnown = perception_->lastKnownEnemyPosition()) {
        lastKnownEnemyPosition_ = *lastKnown;
        hasLastKnownPosition_ = true;

        // -------------------------------------------------------------------
        // 【"重新确认目标"必须是"此刻真的看见"，不能是"还记得"】
        //
        // 原先这里用的是 perception_->hasConfirmedTarget()，而它在**丢失
        // 视线之后仍然为真**（memoryDuration 默认 6 秒内一直有效）——
        // 那正是"记忆"的定义。
        //
        // 后果一（站桩发呆）：搜索分支找到最后已知位置、观察、放弃之后，
        //   下一步这里又把 searchAbandoned_ 清掉、把到达时刻重置，
        //   于是敌人**每一步都在原地重新开始"到达 → 观察 0.8 秒 → 放弃"**，
        //   画面上就是站在那个点上发呆（实测在原地站了 3.5 秒以上，
        //   直到 6 秒记忆过期才真正离开）。
        //
        // 后果二（情报时间戳失真）：lastSeenEnemyTime_ 是"最近一次**看见**
        //   目标"的时刻，World 用它当作小队情报的时间戳。在"只是还记得"
        //   期间持续刷新它，等于把情报一直标成"刚刚看到的"。
        //
        // 判据换成"距上次真正看见的时间"（perception 只在真看见时归零它）。
        // 留一点点抖动容差：视线在掩体边缘反复通断时，不应该每断一帧就
        // 重置搜索进度。
        constexpr double kSeenNowTolerance = 0.05;   // ≈ 3 个逻辑步
        const bool seenNow =
            perception_->timeSinceLastSeen(gameTime) <= kSeenNowTolerance;

        if (seenNow) {
            lastSeenEnemyTime_ = gameTime;

            // 真的又看见了 = 有了新线索，把「已放弃搜索」清掉。
            // 不清的话，AI 放弃一次之后就再也不会搜索了：记忆还在
            // （6s 内），但条件被 searchAbandoned_ 挡住，于是它会一路
            // 巡逻到底，哪怕玩家就站在它刚搜过的位置旁边。
            searchAbandoned_   = false;
            searchArrivedTime_ = -1e9;
        }
    }

    writeSelfStateToBlackboard();
}

// ---------------------------------------------------------------------------
//  阶段 2：位置查询
// ---------------------------------------------------------------------------
AI::EQSContext Enemy::buildEQSContext(const AI::TickContext& context) const {
    AI::EQSContext ctx;

    // 自身状态：直接取自实体，避免再绕一层黑板（性能与可读性都更好）
    ctx.selfPosition = position();
    ctx.selfFacing   = facing_;
    ctx.gameTime     = context.gameTime;

    // 当前目标：优先取感知写入的「最后已知位置」。
    // 之所以不直接用实时坐标，是因为 EQS 需要在失去视野后继续评估
    // 「往最后出现的方向找掩体」这种意图，而不是瞬间退化为无目标。
    //
    // 【必须加时间窗】hasLastKnownPosition_ 一旦置位就再也不会被清掉
    // （只有"重新确认目标"会刷新它的时间戳）。没有这个窗口的话，AI 早已
    // 放弃搜索、回到巡逻，EQS 却仍然围绕一个几分钟前的坐标布点、评分，
    // 表现为"敌人对着空地认真挑选掩体"。
    //
    // 线索来源与搜索分支保持一致（自己的记忆 → 队友情报），否则会出现
    // "行为树认为有线索、EQS 却认为没目标"的割裂：选位退化成围绕自身布点，
    // 而移动又朝最后已知位置走，两者互相打架。
    glm::vec2 leadPosition{0.0f, 0.0f};
    if (searchLead(context.gameTime, leadPosition)) {
        ctx.hasTarget      = true;
        ctx.targetPosition = leadPosition;
    }

    // 威胁源：由 World 每步写入黑板，用于撤退点评估
    ctx.threatSources =
        blackboard_.getOr<std::vector<glm::vec2>>("world.threatSources", {});

    // 掩体位置：与威胁源分开，供掩护度测试使用
    ctx.coverPositions =
        blackboard_.getOr<std::vector<glm::vec2>>("world.coverPositions", {});

    return ctx;
}

void Enemy::updatePositionQueries(double gameTime) {
    // EQS 的实际执行放在行为树的动作节点里（因为需要 TickContext）。
    // 这里只做「是否需要刷新」的时间戳维护，并处理威胁源列表的更新。
    lastPositionQueryTime_ = gameTime;
}

// ---------------------------------------------------------------------------
//  阶段 3：寻路
// ---------------------------------------------------------------------------
void Enemy::updatePathfinding() {
    // 【为什么不能只信任 repathRequested_】
    // 每个"改 moveTarget_"的动作节点都必须自己记得请求重寻路。漏掉一处
    // 的后果不是"不寻路"，而是更隐蔽的**沿着上一条路继续走**：
    // 例如从巡逻切入搜索时，moveTarget_ 已经指向最后已知位置，路径却还是
    // 通往旧巡逻点的那条 —— 敌人会先认真地把老路走完。
    // （实测：拿到队友情报的敌人 moveTarget 已经是被报告的位置，
    //   但路径终点仍是 260 单位外的巡逻点。）
    //
    // 因此这里加一道兜底：目标点相对"上次寻路时的目标"偏移超过一个阈值，
    // 就强制重算。阈值取 ~1.5 格，避免目标每步轻微移动就反复跑 A*。
    constexpr float kRepathGoalTolerance = 48.0f;
    const bool goalMoved =
        vectorLength(moveTarget_ - lastPathGoal_) > kRepathGoalTolerance;

    if (!repathRequested_ && !goalMoved) {
        return;
    }
    repathRequested_ = false;

    // 无导航网格：跳过寻路，移动阶段会退化为直线靠近
    if (!navGrid_ || !navGrid_->isInitialized()) {
        currentPath_.waypoints.clear();
        currentPath_.success = false;
        pathCursor_ = 0;
        return;
    }

    // 目标点没变且路径仍有效时，只重算平滑即可。
    // 这里简单起见每次请求都完整重算；真实项目里应做「目标移动阈值」
    // 判断（例如目标移动超过 40 单位才重新寻路）。
    AI::PathRequest request;
    request.start = position();
    request.goal = moveTarget_;
    request.allowPartialPath = true;
    request.allowDiagonal = true;

    currentPath_ = pathfinder_.findPath(*navGrid_, request);
    pathCursor_ = 0;

    // 记下寻路时刻：followPath 在"路径走完但没到目的地"时用它做重算节流
    lastPathTime_ = currentTime_;

    // 记下本次寻路的目标：上面用它判断"目标是否已经明显改变"
    lastPathGoal_ = moveTarget_;
}

// ---------------------------------------------------------------------------
//  阶段 4：决策
// ---------------------------------------------------------------------------
void Enemy::updateDecision(double fixedDelta, double gameTime) {
    AI::TickContext context{
        /*blackboard=*/blackboard_,
        /*fixedDelta=*/fixedDelta,
        /*gameTime=*/gameTime,
        /*tickIndex=*/behaviorTree_->tickCount(),
        /*aborted=*/false,
        /*agentName=*/name_
    };

    behaviorTree_->tick(context);
}

// ---------------------------------------------------------------------------
//  阶段 5：移动
// ---------------------------------------------------------------------------
void Enemy::applyMovement(double fixedDelta) {
    // 朝向由行为树设置（desiredFacing_），这里做渐进的转向插值。
    //
    // 【例外：本步已经瞄准过】
    // AimAndFire 在行为树里已经把朝向转向了目标。而 applyMovement 在整棵
    // 行为树跑完之后才执行，如果这里再无条件按"移动方向"转一次，就等于
    // 把刚对准的枪口又拽回走路方向——两步转向互相抵消，朝向卡在一个
    // 固定偏差上，AI 会以恒定的横向误差永远打偏。
    //
    // 让瞄准赢：交战中"朝哪走"是次要的，"枪口对不对"才是决定命中与否的。
    // 顺带这也让敌人自然表现出"侧移射击"，比边走边转枪口更像受过训练。
    if (!aimSteeredThisStep_) {
        steerFacingToward(desiredFacing_, fixedDelta);
    }

    // 速度上限必须与 followPath / moveDirectlyToward 用的是**同一个判据**。
    //
    // 【这里曾经把冲刺夹回去】上限原先自己写了一套（只认"撤退时冲刺"），
    // 于是 desiredMoveSpeed() 里新加的"够不着就全速追击"完全无效：
    // 追击动作确实请求了 sprintSpeed，但这一步又把它夹回 moveSpeed。
    // 实测表现为"敌人的最大速度永远是 130"，追不上玩家的 160~280。
    const float maxSpeed = desiredMoveSpeed();

    // 移动到目标的速度由 followPath / moveDirectlyToward 在行为中设置，
    // 这里统一裁剪，防止行为树写出过大的速度。
    //
    // 【修复】原先传的是 {maxSpeed, maxSpeed}，而 clampVelocity 内部取的是
    // 该向量的模长（|{s, s}| = s·√2），于是实际上限被放大到 1.41 倍：
    // 130 的撤退速度实际能跑到 184。改成标量重载后上限就是 maxSpeed 本身。
    clampVelocity(maxSpeed);

    if (state_ == AIState::Idle) {
        velocity_ = {0.0f, 0.0f};
    }
}

float Enemy::desiredMoveSpeed() const noexcept {
    // 【修复】撤退时"冲刺"此前只是把 clamp 上限设成 sprintSpeed，而
    // followPath / moveDirectlyToward 的期望速度始终取 moveSpeed ——
    // 上限永远用不到，sprintSpeed 是个死配置（调参面板上的滑块也没用）。
    // 现在期望速度与上限用同一套判据，撤退真的会加速脱离。
    if (state_ == AIState::Retreating) {
        return config_.sprintSpeed;
    }

    // 【目标/线索在远处 → 全速赶过去】
    //
    // 实测（玩家绕圈走位 60 秒）：敌人有一半以上时间处在射程之外，
    // 而它一直按走速"小跑着追"，玩家走 160、冲刺 280 —— 永远追不上，
    // 画面上就是"AI 追着跑却不开枪"。
    //
    // 【为什么"有效距离"要取两个来源的较大者】
    //   · 到线索点的距离：失去视线后仍有效，但它是个**静止**的点，
    //     越走越近 —— 单用它会在真正的追击途中停止冲刺。
    //   · 黑板里的 kNearestEnemyDist：可见时是实时距离；一旦失去视线，
    //     它会停留在最后目击时的值（实测真实距离 400+ 时黑板上还写着
    //     13/58/195），单用它又会在"目标跑到远处"时压掉冲刺。
    //
    // 【不是所有"离得远"都该冲刺】
    // 已经放弃搜索、正在自己的巡逻环上溜达的敌人不该冲刺 —— 那看起来像
    // 被什么东西追着跑，而且巡逻本来就不赶时间。因此只有两种情况算"赶路"：
    //   1. 脚下的目标就是那条线索（正在赶往线索点）；
    //   2. 已经确认到目标（交战中），正在接近。
    constexpr float kHurryDistanceFactor = 1.35f;
    constexpr float kHeadingToLeadTolerance = 64.0f;

    float targetDistance = 0.0f;
    bool  hurrying = false;

    glm::vec2 lead{0.0f, 0.0f};
    if (searchLead(currentTime_, lead)) {
        targetDistance = vectorLength(lead - position());
        if (vectorLength(moveTarget_ - lead) <= kHeadingToLeadTolerance) {
            hurrying = true;
        }
    }

    if (const float* blackboardDistance =
            blackboard_.getPtr<float>(AI::Blackboard::Keys::kNearestEnemyDist)) {
        targetDistance = std::max(targetDistance, *blackboardDistance);
        if (state_ == AIState::Engaging || state_ == AIState::Repositioning) {
            hurrying = true;
        }
    }

    // 阈值取"舒适交战带的外沿"而不是硬性射程：用射程（400）当阈值时，
    // 黑板距离（失去视线后就停止更新，往往比真实值小）经常刚好卡在 400
    // 以下 —— 实测冲刺几乎不触发。超过 1.35×理想射程就是"离得太远"。
    if (hurrying && targetDistance > config_.preferredRange * kHurryDistanceFactor) {
        return config_.sprintSpeed;
    }

    return config_.moveSpeed;
}

bool Enemy::followPath(double fixedDelta, glm::vec2 destination) {
    const float delta = static_cast<float>(fixedDelta);

    // 没有可用路径：退化为直线移动。
    // 这让 AI 在网格未初始化时仍然「能动」，便于早期调试。
    if (currentPath_.waypoints.empty() || !currentPath_.success) {
        return moveDirectlyToward(destination, fixedDelta);
    }

    // 用前瞻点（而非下一个路点）作为移动目标，消除走一步停一步的机械感
    const float lookAhead = 24.0f;
    const auto aheadPoint = AI::Pathfinder::pointAhead(currentPath_.waypoints, pathCursor_,
                                                       position(), lookAhead);

    const glm::vec2 toDestination = destination - position();
    const float destinationDistance = vectorLength(toDestination);

    // 【关键分支：路走完了，但人还没到】
    //
    // 这条路径是 allowPartialPath 允许下的**截断路径**——A* 没能连通到
    // 目的地，返回的是"能走到的最远处"。游标一旦走到它的末尾，
    // pointAhead 就返回 nullopt，看起来和"已到达"一模一样。
    //
    // 早期实现把两者不加区分地当作"到达"，直接刹车停下。后果是 AI 会
    // **永久冻结在半路**：目标点没变 → 不触发重新寻路 → 每一步都判"到达"
    // → 永远停着。实测中 4 个敌人里有 3 个就这样卡在墙角不动，
    // 只有 1 个会真正走到玩家面前交战，整场战斗看起来像"AI 集体发呆"。
    //
    // 正确做法：把它当作"路线失效"，重新寻路，同时先朝目的地直线靠拢。
    // 靠拢过程中位置变化了，A* 往往就能绕开原先的阻塞点连通路。
    if (!aheadPoint && destinationDistance >= kArrivalRadius) {
        // 节流：不能每步都重算 A*（那是 60 次/秒/敌人的开销）。
        // 0.5 秒一次足以跟上目标移动，也让卡住的敌人不至于把 CPU 吃满。
        constexpr double kRepathInterval = 0.5;
        if (currentTime_ - lastPathTime_ >= kRepathInterval) {
            repathRequested_ = true;
        }
        return moveDirectlyToward(destination, fixedDelta);
    }

    if (!aheadPoint || destinationDistance < kArrivalRadius) {
        // 到达：减速停下
        const float damping = std::max(0.0f, 1.0f - 8.0f * delta);
        velocity_ *= damping;
        return destinationDistance < kArrivalRadius;
    }

    const glm::vec2 toNext = *aheadPoint - position();
    const float distance = vectorLength(toNext);
    if (distance < 1e-4f) {
        return false;
    }

    const glm::vec2 direction = toNext / distance;

    // 接近终点时减速，避免冲过头来回振荡
    const float cruiseSpeed = desiredMoveSpeed();
    const float desiredSpeed = (destinationDistance < kSlowdownDistance)
                                   ? cruiseSpeed * (destinationDistance / kSlowdownDistance)
                                   : cruiseSpeed;
    const glm::vec2 desiredVelocity = direction * std::max(desiredSpeed, kMinApproachSpeed);

    // 向期望速度逼近（加速度限制）
    const glm::vec2 toDesired = desiredVelocity - velocity_;
    const float changeLength = vectorLength(toDesired);
    const float maxChange = config_.acceleration * delta;

    if (changeLength <= maxChange) {
        velocity_ = desiredVelocity;
    } else {
        velocity_ += (toDesired / changeLength) * maxChange;
    }

    desiredFacing_ = direction;
    blackboard_.set(AI::Blackboard::Keys::kPathValid, true, kMovementWriter);
    blackboard_.set(AI::Blackboard::Keys::kPathAge,
                    static_cast<float>(pathCursor_), kMovementWriter);
    return false;
}

bool Enemy::moveDirectlyToward(glm::vec2 destination, double fixedDelta) {
    const float delta = static_cast<float>(fixedDelta);

    const glm::vec2 toDestination = destination - position();
    const float distance = vectorLength(toDestination);

    // 无路径信息时视为「已到达」，避免无网格环境下实体乱跑
    if (distance < kArrivalRadius) {
        const float damping = std::max(0.0f, 1.0f - 8.0f * delta);
        velocity_ *= damping;
        return true;
    }

    const glm::vec2 direction = toDestination / distance;
    const glm::vec2 desiredVelocity = direction * desiredMoveSpeed();

    const glm::vec2 toDesired = desiredVelocity - velocity_;
    const float changeLength = vectorLength(toDesired);
    const float maxChange = config_.acceleration * delta;

    if (changeLength <= maxChange) {
        velocity_ = desiredVelocity;
    } else {
        velocity_ += (toDesired / changeLength) * maxChange;
    }

    desiredFacing_ = direction;
    blackboard_.set(AI::Blackboard::Keys::kPathValid, false, kMovementWriter);
    return false;
}

void Enemy::steerFacingToward(glm::vec2 desiredDirection, double fixedDelta) {
    const glm::vec2 desired = normalizeOr(desiredDirection, facing_);

    // 用角度插值而非向量插值：向量插值在 180 度反向时会退化为零向量，
    // 导致朝向突然跳变。角度插值天然处理这种情况。
    const float currentAngle = std::atan2(facing_.y, facing_.x);
    const float desiredAngle = std::atan2(desired.y, desired.x);

    const float error = wrapAngle(desiredAngle - currentAngle);

    // 转身速率：约 6 弧度/秒（≈ 344°/s），足够灵活但不至于瞬转。
    // 这个值直接决定 AI 看起来「笨重」还是「敏捷」。
    constexpr float kTurnRate = 6.0f;
    const float maxTurn = kTurnRate * static_cast<float>(fixedDelta);

    const float turn = std::clamp(error, -maxTurn, maxTurn);
    const float newAngle = currentAngle + turn;

    facing_ = {std::cos(newAngle), std::sin(newAngle)};
}

float Enemy::lateralAimError(glm::vec2 targetPoint) const {
    const glm::vec2 toTarget = targetPoint - position();
    const float distance = vectorLength(toTarget);
    if (distance < 1e-4f) {
        return 0.0f;   // 已经贴脸了，不存在"打偏"这回事
    }

    // 单位方向与朝向的叉积 = 夹角的正弦值。
    // 乘以距离就得到「弹道在目标处的横向偏差」，单位是世界单位，
    // 可以直接和"目标半径 + 子弹半径"比较。
    const glm::vec2 unit = toTarget / distance;
    const float sinAngle = facing_.x * unit.y - facing_.y * unit.x;
    return std::fabs(sinAngle) * distance;
}

float Enemy::facingError(glm::vec2 targetDirection) const {
    const glm::vec2 target = normalizeOr(targetDirection, facing_);
    const float targetAngle = std::atan2(target.y, target.x);
    const float currentAngle = std::atan2(facing_.y, facing_.x);
    return std::fabs(wrapAngle(targetAngle - currentAngle));
}

double Enemy::fireCooldownRemaining(double gameTime) const noexcept {
    const double remaining =
        static_cast<double>(config_.fireCooldown) - (gameTime - lastFireTime_);
    return (remaining > 0.0) ? remaining : 0.0;
}

glm::vec2 Enemy::predictedAimPoint(glm::vec2 targetPosition, double gameTime) {
    // -----------------------------------------------------------------------
    // 1. 用连续两次观察到的位置差分出目标速度
    // -----------------------------------------------------------------------
    //
    // 【为什么不直接读目标的速度向量】
    // 感知输出里只有位置。更根本的是：敌人不该"读取"玩家的真实速度——
    // 那是开了天眼的作弊行为。用自己先后看到的两点位置去差分，
    // 是任何实体都能做到的事，而且预测精度会随观察时间自然收敛。
    const double dt = gameTime - prevAimTime_;
    if (hasPrevAimPoint_ && dt > 1e-4) {
        const glm::vec2 delta = targetPosition - prevAimPoint_;
        const float jump = vectorLength(delta);

        // 一步之内跳了 120 个单位（= 7200 单位/秒）不可能是移动，
        // 只能是"目标换人了"：旧目标被打死、或短暂丢失后重新锁定。
        // 这时旧的速度估计对新目标毫无意义，必须清掉，
        // 否则敌人会朝一个凭空捏造的方向提前量开火。
        constexpr float kTargetSwitchJump = 120.0f;

        if (jump > kTargetSwitchJump) {
            targetVelocity_ = {0.0f, 0.0f};
        } else {
            const glm::vec2 raw = delta / static_cast<float>(dt);

            // 指数平滑：单步差分噪声很大（感知抖动、玩家急变换向），
            // 直接用会让 AI 的枪口剧烈抖动，看起来像帕金森。
            constexpr float kSmoothing = 0.25f;
            targetVelocity_ = targetVelocity_ * (1.0f - kSmoothing) + raw * kSmoothing;

            // 限幅：玩家冲刺上限 280，留些余量。这个夹子同时也是
            // 一道安全阀——任何异常的差分结果都不会让提前量飞出屏幕。
            constexpr float kMaxTargetSpeed = 420.0f;
            const float speed = vectorLength(targetVelocity_);
            if (speed > kMaxTargetSpeed) {
                targetVelocity_ = targetVelocity_ * (kMaxTargetSpeed / speed);
            }
        }
    }

    prevAimPoint_ = targetPosition;
    prevAimTime_ = gameTime;
    hasPrevAimPoint_ = true;

    // -----------------------------------------------------------------------
    // 2. 提前量 = 目标速度 × 子弹飞行时间
    // -----------------------------------------------------------------------
    const float distance = vectorLength(targetPosition - position());
    const float flightTime = distance / projectileSpeed_;

    return targetPosition +
           targetVelocity_ * (flightTime * config_.aimPrediction);
}

// ---------------------------------------------------------------------------
//  黑板写入
// ---------------------------------------------------------------------------
void Enemy::writeSelfStateToBlackboard() {
    blackboard_.set(AI::Blackboard::Keys::kSelfPosition, position(), "Self");
    blackboard_.set(AI::Blackboard::Keys::kSelfHealth, health(), "Self");
    blackboard_.set(AI::Blackboard::Keys::kSelfHealthRatio, healthRatio(), "Self");

    // 【优化】状态字符串每步都重新构造并写入（一次堆分配），而 state_ 在
    // 绝大多数步里是不变的。用零拷贝读取先比对，内容没变就不写。
    const std::string stateText(toString(state_));
    if (const std::string* stored =
            blackboard_.getPtr<std::string>(AI::Blackboard::Keys::kSelfState)) {
        if (*stored != stateText) {
            blackboard_.set(AI::Blackboard::Keys::kSelfState, stateText, "Self");
        }
    } else {
        blackboard_.set(AI::Blackboard::Keys::kSelfState, stateText, "Self");
    }
    blackboard_.set(AI::Blackboard::Keys::kSelfFacing, facing_, "Self");
}

// ---------------------------------------------------------------------------
//  状态
// ---------------------------------------------------------------------------
void Enemy::setState(AIState state, double gameTime) {
    if (state_ == state) {
        return;
    }
    state_ = state;
    blackboard_.set(AI::Blackboard::Keys::kCurrentAction, std::string(toString(state)),
                    kDecisionWriter);
    (void)gameTime;
}

// ---------------------------------------------------------------------------
//  调试
// ---------------------------------------------------------------------------
std::string Enemy::debugString() const {
    char buffer[320];
    std::snprintf(buffer, sizeof(buffer),
                  "#%u Enemy \"%s\" pos=(%.0f,%.0f) hp=%.0f/%.0f state=%s %s",
                  id(), name_.c_str(),
                  static_cast<double>(position_.x), static_cast<double>(position_.y),
                  static_cast<double>(health()), static_cast<double>(maxHealth_),
                  toString(state_),
                  isDead() ? "[DEAD]" : "");
    return std::string(buffer);
}

std::string Enemy::aiDebugString() const {
    char buffer[640];

    const float nearestDistance = blackboard_.getOr<float>(
        AI::Blackboard::Keys::kNearestEnemyDist, -1.0f);

    // 队友情报只在有效时附在末尾：平时刷一堆 "-" 只会淹没真正有用的信息
    char intel[64];
    intel[0] = '\0';
    if (hasFreshSquadIntel(currentTime_)) {
        std::snprintf(intel, sizeof(intel), " | intel=(%.0f,%.0f)",
                      static_cast<double>(squadIntelPosition_.x),
                      static_cast<double>(squadIntelPosition_.y));
    }

    std::snprintf(buffer, sizeof(buffer),
                  "state=%s | threat=%.2f | nearestDist=%.0f | path=%zu pts | "
                  "BT=%s | EQS=%zu cand | shots=%d | cd=%.2fs | tgtSpd=%.0f | aimErr=%.0f%s",
                  toString(state_),
                  static_cast<double>(perception_->threatLevel()),
                  static_cast<double>(nearestDistance),
                  currentPath_.waypoints.size(),
                  AI::toString(behaviorTree_->lastStatus()),
                  lastPositionQuery_.candidates.size(),
                  shotsFired_,
                  fireCooldownRemaining(currentTime_),
                  static_cast<double>(vectorLength(targetVelocity_)),
                  static_cast<double>(lateralAimError(aimPoint_)),
                  intel);
    return std::string(buffer);
}

} // namespace TacticalAI::Game
