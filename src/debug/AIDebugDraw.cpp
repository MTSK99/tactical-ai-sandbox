// ===========================================================================
//  src/debug/AIDebugDraw.cpp
// ===========================================================================
#include "debug/AIDebugDraw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace TacticalAI::Debug {
namespace {

// Colors 是命名空间而非类型，必须用 namespace 别名引入；
// 写成 `using Engine::Colors;` 会被编译器拒绝（using 声明不能命名命名空间）。
namespace Colors = Engine::Colors;
using Engine::Renderer;
using AI::NodeStatus;

constexpr float kTwoPi = 6.28318530717958647692f;

/// 按节点状态给颜色：绿=成功、红=失败、黄=运行中、灰=未执行。
Color colorForStatus(NodeStatus status) {
    switch (status) {
        case NodeStatus::Success: return Colors::kSuccess;
        case NodeStatus::Failure: return Colors::kError;
        case NodeStatus::Running: return Colors::kWarning;
        case NodeStatus::Invalid: return Colors::kTextMuted;
    }
    return Colors::kTextMuted;
}

/// 按 AI 状态给颜色，让「敌人在想什么」一眼可见。
Color colorForAIState(Game::AIState state) {
    switch (state) {
        case Game::AIState::Idle:          return Colors::kTextMuted;
        case Game::AIState::Patrolling:    return Color{160, 200, 255, 255};
        case Game::AIState::Engaging:      return Colors::kEnemy;
        case Game::AIState::Repositioning: return Colors::kWarning;
        case Game::AIState::Retreating:    return Color{255, 120, 200, 255};
        case Game::AIState::Dead:          return Color{70, 70, 80, 255};
    }
    return Colors::kTextMuted;
}

/// 用分数（0..1）在「红 -> 黄 -> 绿」之间插值。
/// 比单色深浅更容易分辨，也更符合直觉（绿=好）。
Color colorForScore(float score) {
    score = std::clamp(score, 0.0f, 1.0f);

    const unsigned char red   = static_cast<unsigned char>((1.0f - score) * 220.0f + 35.0f);
    const unsigned char green = static_cast<unsigned char>(score * 200.0f + 40.0f);

    return Color{red, green, 90, 230};
}

inline float distanceBetween(glm::vec2 a, glm::vec2 b) noexcept {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

// ---------------------------------------------------------------------------
//  DebugDrawFlags
// ---------------------------------------------------------------------------
std::string DebugDrawFlags::summary() const {
    std::string text = "调试:";
    auto append = [&text](const char* name, bool enabled) {
        text += " ";
        text += name;
        text += enabled ? "=ON " : "=off";
    };

    append("感知[1]", showPerception);
    append("行为树[2]", showBehaviorTree);
    append("EQS[3]", showEQSCandidates);
    append("路径[4]", showPaths);
    append("网格[5]", showGrid);
    append("标签[6]", showLabels);
    append("子弹[7]", showShotTracers);
    return text;
}

// ---------------------------------------------------------------------------
//  主绘制
// ---------------------------------------------------------------------------
void AIDebugDraw::draw(const Renderer& renderer, const Game::World& world) {
    // 【修复】网格的两个开关任一开启都要进入绘制，否则单独勾选
    // showNavGridBlocked 没有任何视觉效果。
    if (flags_.showGrid || flags_.showNavGridBlocked) {
        drawNavGrid(renderer, world);
    }

    // 子弹与残影都画在实体之下：子弹应该从"人身上"飞过，而不是盖住人。
    // 这既是视觉优先级，也让实体始终保持可辨认。
    if (flags_.showShotTracers) {
        drawImpactMarks(renderer, world);
        drawProjectiles(renderer, world);
    }

    if (flags_.showEntities) {
        drawEntities(renderer, world);
    }

    if (flags_.showThreatSources) {
        drawThreatSources(renderer, world);
    }

    // 逐敌人绘制 AI 相关可视化
    for (const auto& entity : world.entities()) {
        if (entity->type() != Game::EntityType::Enemy) {
            continue;
        }
        const auto* enemy = static_cast<const Game::Enemy*>(entity.get());

        if (flags_.showPerception) {
            drawPerception(renderer, *enemy);
        }
        if (flags_.showEQSCandidates) {
            drawEQSCandidates(renderer, *enemy);
        }
        if (flags_.showPaths) {
            drawPath(renderer, *enemy);
        }
        if (flags_.showLabels) {
            drawEntityLabel(renderer, *enemy);
        }
    }

    if (flags_.showDamageEvents) {
        drawDamageFlashes(renderer, world);
    }
}

// ---------------------------------------------------------------------------
//  导航网格
// ---------------------------------------------------------------------------
void AIDebugDraw::drawNavGrid(const Renderer& renderer, const Game::World& world) const {
    const AI::NavGrid& grid = world.navGrid();
    if (!grid.isInitialized()) {
        return;
    }

    // 不逐格绘制线框（几千个格子会很慢）。改为：
    //  1. 用点阵稀疏标记可通行格中心，步长根据缩放自适应
    //  2. 仅在 showNavGridBlocked 开启时才额外标出阻挡格
    //
    // 【修复】原实现在没有 showNavGridBlocked 时直接 return，导致「导航
    // 网格 [5]」开关单独开启时什么都画不出来（快捷键 [5] 因此形同虚设，
    // 而 DebugUI 里又把 [5] 标给了 showNavGridBlocked，两处语义不一致）。
    // 现在两个开关各管一件事：showGrid 画可通行点阵，showNavGridBlocked
    // 额外叠加阻挡色块。
    const float cellSize = grid.cellSize();
    const float zoom = renderer.camera().zoom();

    // 稀疏步长：缩放越小，跳过的格子越多，保持屏幕上的点密度稳定
    const int stride = std::max(1, static_cast<int>(2.0f / std::max(zoom, 0.15f)));

    if (flags_.showGrid) {
        const float dotSize = std::max(cellSize * 0.12f, 1.0f);
        for (int y = 0; y < grid.rows(); y += stride) {
            for (int x = 0; x < grid.columns(); x += stride) {
                const AI::GridCoord coord{x, y};
                if (!grid.isWalkable(coord)) {
                    continue;
                }
                const glm::vec2 center = grid.gridToWorld(coord);
                renderer.drawRect(center, glm::vec2{dotSize, dotSize},
                                  Color{120, 160, 200, 70});
            }
        }
    }

    if (!flags_.showNavGridBlocked) {
        return;
    }

    for (int y = 0; y < grid.rows(); y += stride) {
        for (int x = 0; x < grid.columns(); x += stride) {
            const AI::GridCoord coord{x, y};
            if (grid.isWalkable(coord)) {
                continue;
            }
            const glm::vec2 center = grid.gridToWorld(coord);
            renderer.drawRect(center, glm::vec2{cellSize, cellSize},
                              Color{90, 40, 45, 60});
        }
    }
}

// ---------------------------------------------------------------------------
//  实体
// ---------------------------------------------------------------------------
void AIDebugDraw::drawEntities(const Renderer& renderer, const Game::World& world) const {
    for (const auto& entity : world.entities()) {
        const glm::vec2 position = entity->position();

        switch (entity->type()) {
            case Game::EntityType::Player: {
                const auto* player = static_cast<const Game::Player*>(entity.get());
                const Color color = player->isDead() ? Color{80, 80, 90, 255} : Colors::kPlayer;

                // --- 翻滚残影 ---
                //
                // 画在实体之前，让它落在身体下方，看起来像"从那里冲过来"。
                //
                // 【残影为什么沿 -rollDirection 铺开，而不是记录历史位置】
                // 记录历史需要每步往列表里塞点、还要处理长度上限与清理，
                // 而翻滚轨迹是一条直线（速度方向在整段翻滚里不变），
                // 所以用"反向铺开"能表达同样的信息，代价是零状态。
                // 这依赖"翻滚方向恒定"这个前提——将来若加了曲线翻滚，
                // 这里必须改回真实的历史轨迹。
                if (player->isRolling()) {
                    constexpr int kGhostCount = 4;
                    for (int i = kGhostCount; i >= 1; --i) {
                        const float t = static_cast<float>(i) /
                                        static_cast<float>(kGhostCount + 1);
                        const glm::vec2 ghost =
                            position - player->rollDirection() * (entity->radius() * 1.9f *
                                                                  static_cast<float>(i));
                        renderer.drawCircle(ghost, entity->radius() * (1.0f - 0.4f * t),
                                            Fade(Colors::kPlayer, 0.34f * (1.0f - t)));
                    }
                }

                renderer.drawCircle(position, entity->radius(), color);
                renderer.drawCircleLines(position, entity->radius(), Fade(RAYWHITE, 0.5f), 1.5f);

                // --- 无敌帧护盾环 ---
                //
                // 【为什么必须画出来】
                // 无敌帧是一条完全不可见的规则。不画的话，玩家躲开了子弹只会
                // 觉得"运气好"，被打了才会怀疑"翻滚到底有没有用"；开发者也没法
                // 确认它生效。画成一个明确的环，"这段时间打不中我"就变成了
                // 能当场看见的事实——和 HUD 上的"闪避 N"计数互相印证。
                //
                // 用与玩家蓝色差异明显的青白色，避免被误认成血条或选中高亮。
                if (player->isInvulnerable()) {
                    renderer.drawCircleLines(position, entity->radius() * 1.85f,
                                             Color{150, 238, 255, 235}, 2.5f);
                    renderer.drawCircleLines(position, entity->radius() * 1.35f,
                                             Color{150, 238, 255, 120}, 1.5f);
                }

                // 朝向箭头：长度 = 半径 * 2，一眼看出面朝哪
                renderer.drawArrow(position, position + player->facing() * (entity->radius() * 2.5f),
                                   Color{230, 245, 255, 255}, 2.5f);

                // 【这里本来有一条翻滚进度条，被删掉了】
                //
                // 第一版画在血条正下方，和血条只隔 2 像素，糊成一块；
                // 第二版挪到身体下方，又正好落在无敌护盾环的内侧，
                // 两道青色弧线叠在一起，谁也读不出来。
                //
                // 试过用扇形做环形进度（drawSector 存在，技术上可行），
                // 但玩家半径只有 12，环形进度、护盾环、身体三者在这个尺度上
                // 必然互相咬合，没有一种摆法能同时干净。
                //
                // 最后决定不做：这条进度条本来就是**冗余**的——
                //   · 无敌与否 → 护盾环（一眼可见）
                //   · 翻滚与否 → 残影（一眼可见）
                //   · 还剩多久 → HUD 上的"翻滚中[无敌] / 翻滚中(无无敌)"读数
                // 信息都已经有了。再加一条读不出来的条，只是污染画面，
                // 而"画面干净"正是这一轮修掉 HUD 叠压时费劲争取来的东西。

                // 血条
                const float ratio = player->healthRatio();
                renderer.drawRect({position.x, position.y - entity->radius() - 12.0f},
                                  glm::vec2{entity->radius() * 2.4f, 4.0f},
                                  Color{45, 48, 58, 220});
                renderer.drawRect({position.x - entity->radius() * 1.2f * (1.0f - ratio),
                                   position.y - entity->radius() - 12.0f},
                                  glm::vec2{entity->radius() * 2.4f * ratio, 4.0f},
                                  Colors::kSuccess);
                break;
            }

            case Game::EntityType::Enemy: {
                const auto* enemy = static_cast<const Game::Enemy*>(entity.get());
                const bool dead = enemy->isDead();

                const Color bodyColor = dead
                                            ? Color{60, 62, 70, 255}
                                            : colorForAIState(enemy->state());

                renderer.drawCircle(position, entity->radius(), bodyColor);
                renderer.drawCircleLines(position, entity->radius(),
                                         Fade(RAYWHITE, dead ? 0.15f : 0.55f), 1.5f);

                if (!dead) {
                    renderer.drawArrow(position,
                                       position + enemy->facing() * (entity->radius() * 2.5f),
                                       Color{255, 235, 235, 255}, 2.5f);
                }

                // 血条
                const float ratio = enemy->healthRatio();
                renderer.drawRect({position.x, position.y - entity->radius() - 12.0f},
                                  glm::vec2{entity->radius() * 2.4f, 4.0f},
                                  Color{45, 48, 58, 220});
                renderer.drawRect({position.x - entity->radius() * 1.2f * (1.0f - ratio),
                                   position.y - entity->radius() - 12.0f},
                                  glm::vec2{entity->radius() * 2.4f * ratio, 4.0f},
                                  ratio > 0.3f ? Colors::kEnemy
                                               : Color{255, 90, 90, 255});
                break;
            }

            case Game::EntityType::Obstacle: {
                const auto* obstacle = static_cast<const Game::Obstacle*>(entity.get());
                const Color color = obstacle->providesCover()
                                        ? Colors::kCover
                                        : Colors::kWall;

                renderer.drawRect(position, obstacle->size(), color);
                renderer.drawRectLines(position, obstacle->size(),
                                       Fade(RAYWHITE, 0.12f), 1.0f);

                // 掩体用角标提示（小三角），避免与普通墙混淆
                if (obstacle->providesCover()) {
                    const glm::vec2 corner = position + obstacle->size() * 0.35f;
                    renderer.drawCircle(corner, 3.0f, Color{255, 210, 120, 180});
                }
                break;
            }

            default:
                break;
        }
    }
}

// ---------------------------------------------------------------------------
//  感知
// ---------------------------------------------------------------------------
void AIDebugDraw::drawPerception(const Renderer& renderer, const Game::Enemy& enemy) const {
    if (enemy.isDead()) {
        return;
    }

    const glm::vec2 position = enemy.position();
    const glm::vec2 facing = enemy.facing();
    const AI::PerceptionConfig& config = enemy.perception().config();

    const float baseAngle = std::atan2(facing.y, facing.x);
    const float halfFov = config.halfFovAngle;

    // --- 视野扇形 ---
    // 半透明填充 + 边缘线，既能看出范围又不遮挡底下的实体
    renderer.drawSector(position, config.viewDistance,
                        baseAngle - halfFov, baseAngle + halfFov,
                        Colors::kPerceptionFov);

    const glm::vec2 leftEdge{
        position.x + std::cos(baseAngle - halfFov) * config.viewDistance,
        position.y + std::sin(baseAngle - halfFov) * config.viewDistance
    };
    const glm::vec2 rightEdge{
        position.x + std::cos(baseAngle + halfFov) * config.viewDistance,
        position.y + std::sin(baseAngle + halfFov) * config.viewDistance
    };

    renderer.drawLine(position, leftEdge, Fade(Colors::kPerceptionRay, 0.35f), 1.0f);
    renderer.drawLine(position, rightEdge, Fade(Colors::kPerceptionRay, 0.35f), 1.0f);

    // --- 贴身察觉半径 ---
    // 用虚线感的细圆表示「背后也能发现」的范围
    renderer.drawCircleLines(position, config.proximityRadius,
                             Fade(Colors::kWarning, 0.30f), 1.0f);

    // --- 视线射线 ---
    // 绿色 = 当前可见且已确认，黄色 = 可见但还在反应延迟中，灰色 = 记忆中
    for (const AI::PerceptionRecord& record : enemy.perception().records()) {
        const glm::vec2 target = record.target.position;

        Color rayColor = Color{110, 110, 125, 120};   // 默认：记忆中的位置
        if (record.visibleNow) {
            rayColor = record.confirmed ? Colors::kSuccess : Colors::kWarning;
        }

        renderer.drawLine(position, target, Fade(rayColor, 0.8f), 1.5f);

        // 目标位置画一个小圆环标记感知点
        renderer.drawCircleLines(target, 8.0f, rayColor, 1.5f);

        // 已确认目标额外画一个十字，强调「AI 已经锁定它了」
        if (record.confirmed) {
            renderer.drawLine({target.x - 12.0f, target.y}, {target.x + 12.0f, target.y},
                              rayColor, 1.0f);
            renderer.drawLine({target.x, target.y - 12.0f}, {target.x, target.y + 12.0f},
                              rayColor, 1.0f);
        }
    }

    // --- 最后已知位置（记忆）---
    // 用方框标出，能直观看到「AI 正在朝一个过期的位置搜索」
    if (!enemy.perception().hasConfirmedTarget()) {
        if (const auto lastKnown = enemy.perception().lastKnownEnemyPosition()) {
            const float size = 18.0f;
            renderer.drawRectLines(*lastKnown, glm::vec2{size, size},
                                   Fade(Colors::kTextMuted, 0.7f), 1.5f);
            renderer.drawLabel(*lastKnown + glm::vec2(0.0f, -14.0f), "LAST KNOWN",
                               Colors::kTextMuted, 10);
        }
    }
}

// ---------------------------------------------------------------------------
//  EQS 候选点
// ---------------------------------------------------------------------------
void AIDebugDraw::drawEQSCandidates(const Renderer& renderer, const Game::Enemy& enemy) const {
    const AI::EQSResult& result = enemy.lastPositionQuery();
    if (result.evaluatedCount == 0) {
        return;
    }

    // 被剔除的候选：暗淡的小点，显示「AI 考虑过但排除了」
    for (const AI::EQSCandidate& candidate : result.rejected) {
        renderer.drawCircle(candidate.position, 2.0f,
                            Fade(Colors::kEQSCandidate, 0.22f));
    }

    // 有效候选：按分数着色，一眼看出「好位置」聚集在哪里
    for (const AI::EQSCandidate& candidate : result.candidates) {
        const Color color = colorForScore(candidate.score);
        const float radius = 3.0f + candidate.score * 2.5f;
        renderer.drawCircle(candidate.position, radius, Fade(color, 0.75f));
    }

    // 最优候选：高亮 + 得分明细
    if (const AI::EQSCandidate* best = result.best()) {
        renderer.drawCircleLines(best->position, 10.0f, Colors::kEQSBest, 2.0f);
        renderer.drawLine(enemy.position(), best->position,
                          Fade(Colors::kEQSBest, 0.55f), 1.5f);

        // 在最优点的上方显示各测试项得分条
        //
        // 【清理】原先这里先 reserve 再立刻用硬编码的 4 个名字整体覆盖
        // （reserve 的结果被丢弃），而且 drawScoreBreakdown 从未使用过
        // testNames —— 硬编码名单与实际 testScores 数量也没有任何约束，
        // 属于"看起来在传名字、其实完全没用"的死参数。去掉参数，
        // 避免后来者误以为名字已经接上了。
        drawScoreBreakdown(renderer, best->position, *best);
    }
}

void AIDebugDraw::drawScoreBreakdown(const Renderer& renderer, glm::vec2 anchor,
                                     const AI::EQSCandidate& candidate) const {
    if (candidate.testScores.empty()) {
        return;
    }

    // 在反缩放坐标系里绘制，保证无论相机怎么缩放，条形图大小恒定可读
    const float zoom = (renderer.camera().zoom() > 1e-4f) ? renderer.camera().zoom() : 1.0f;
    const float inverse = 1.0f / zoom;

    constexpr float kBarWidth = 46.0f;
    constexpr float kBarHeight = 5.0f;
    constexpr float kGap = 2.0f;
    constexpr float kPad = 4.0f;

    const float panelWidth = (kBarWidth + kPad * 2.0f) * inverse;
    const float panelHeight = (static_cast<float>(candidate.testScores.size())
                               * (kBarHeight + kGap) + kPad * 2.0f) * inverse;

    const ::Rectangle panel{
        anchor.x + 14.0f * inverse,
        anchor.y - panelHeight * 0.5f,
        panelWidth,
        panelHeight
    };

    renderer.drawScreenRect(panel, Fade(Colors::kPanelBg, 0.85f));
    renderer.drawScreenRectLines(panel, Fade(Colors::kPanelBorder, 0.9f), 1.0f);

    for (size_t i = 0; i < candidate.testScores.size(); ++i) {
        const float score = std::clamp(candidate.testScores[i], 0.0f, 1.0f);

        const float barY = panel.y + (kPad + static_cast<float>(i) * (kBarHeight + kGap)) * inverse;
        const float barX = panel.x + kPad * inverse;

        // 底槽
        renderer.drawScreenRect(
            ::Rectangle{barX, barY, kBarWidth * inverse, kBarHeight * inverse},
            Color{40, 44, 54, 230});

        // 填充：长度代表该测试项的得分
        renderer.drawScreenRect(
            ::Rectangle{barX, barY, kBarWidth * inverse * score, kBarHeight * inverse},
            colorForScore(score));
    }
}

// ---------------------------------------------------------------------------
//  路径
// ---------------------------------------------------------------------------
void AIDebugDraw::drawPath(const Renderer& renderer, const Game::Enemy& enemy) const {
    const std::vector<glm::vec2>& waypoints = enemy.pathWaypoints();
    if (waypoints.size() < 2) {
        return;
    }

    // 路径线用半透明：不遮挡实体，但足够看清走向
    const bool partial = enemy.currentPath().partial;
    const Color pathColor = partial
                                ? Color{255, 190, 90, 200}   // 部分路径用橙色区分
                                : Colors::kPath;

    renderer.drawPolyline(waypoints, pathColor, 2.0f);

    // 路点用小圆点标记，中间的稀疏一些（只标记每隔一个，避免太密）
    for (size_t i = 0; i < waypoints.size(); ++i) {
        const bool isEndpoint = (i == 0) || (i + 1 == waypoints.size());
        if (!isEndpoint && (i % 2 != 0)) {
            continue;
        }

        const float radius = isEndpoint ? 4.0f : 2.5f;
        renderer.drawCircle(waypoints[i], radius,
                            isEndpoint ? Fade(pathColor, 1.0f) : Fade(pathColor, 0.55f));
    }

    // 终点用方框强调（与中间路点区分）
    renderer.drawRectLines(waypoints.back(), glm::vec2{10.0f, 10.0f},
                           Fade(pathColor, 0.9f), 1.5f);

    // 画一条从 AI 到移动目标的虚线，显示「当前意图」
    renderer.drawLine(enemy.position(), enemy.moveTarget(),
                      Fade(Colors::kAccent, 0.25f), 1.0f);
}

// ---------------------------------------------------------------------------
//  标签
// ---------------------------------------------------------------------------
void AIDebugDraw::drawEntityLabel(const Renderer& renderer, const Game::Enemy& enemy) const {
    if (enemy.isDead()) {
        renderer.drawLabel(enemy.position() + glm::vec2(0.0f, -30.0f),
                           enemy.name() + " [DEAD]", Color{120, 120, 130, 255}, 11);
        return;
    }

    // 第一行：状态 + 行为树根节点结果
    char line1[128];
    std::snprintf(line1, sizeof(line1), "%s  %s",
                  Game::toString(enemy.state()),
                  AI::toString(enemy.behaviorTree().lastStatus()));

    // 第二行：关键数值（威胁等级 + 路径长度 + 射击数）
    char line2[160];
    std::snprintf(line2, sizeof(line2), "thr %.2f | seg %zu | fire %d",
                  static_cast<double>(enemy.perception().threatLevel()),
                  enemy.pathWaypoints().size(),
                  enemy.shotsFired());

    const Color stateColor = colorForAIState(enemy.state());

    // 两行标签叠放，第二行用弱化颜色
    renderer.drawLabel(enemy.position() + glm::vec2(0.0f, -30.0f), line1, stateColor, 11);
    renderer.drawLabel(enemy.position() + glm::vec2(0.0f, -46.0f), line2,
                       Colors::kTextMuted, 10);

    // 开火瞬间的高亮环：让「AI 在射击」这件事在视觉上立刻可辨
    if (enemy.firedThisStep()) {
        renderer.drawCircleLines(enemy.position(), enemy.radius() + 14.0f,
                                 Color{255, 240, 180, 220}, 2.0f);
    }

    // 射击次数累计显示为一个小条，反映这个敌人打得凶不凶
    if (enemy.shotsFired() > 0) {
        const float barWidth = std::min(static_cast<float>(enemy.shotsFired()) * 2.0f, 40.0f);
        renderer.drawRect(enemy.position() + glm::vec2(0.0f, enemy.radius() + 18.0f),
                          glm::vec2{barWidth, 3.0f}, Fade(Colors::kEnemy, 0.8f));
    }
}

// ---------------------------------------------------------------------------
//  威胁源
// ---------------------------------------------------------------------------
void AIDebugDraw::drawThreatSources(const Renderer& renderer, const Game::World& world) const {
    (void)world;

    // 威胁源由 World 收集，这里从每个敌人的黑板读取（数据一致）
    for (const auto& entity : world.entities()) {
        if (entity->type() != Game::EntityType::Enemy || entity->isDead()) {
            continue;
        }
        const auto* enemy = static_cast<const Game::Enemy*>(entity.get());

        // 从黑板读取威胁源列表——这是 EQS 实际使用的数据，
        // 因此可视化它才能真实反映 AI 的「认知」而非「我们的假设」
        if (const auto threats = enemy->blackboard().get<std::vector<glm::vec2>>(
                "world.threatSources")) {
            for (const glm::vec2& threat : *threats) {
                renderer.drawCircleLines(threat, 16.0f, Fade(Colors::kError, 0.25f), 1.0f);
                renderer.drawCircle(threat, 2.0f, Fade(Colors::kError, 0.45f));
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  子弹与弹着残影
// ---------------------------------------------------------------------------
namespace {

/// 子弹配色：玩家暖黄，敌人偏红。
/// 与伤害闪光、残影共用同一套色，保证"同一种东西在界面上只有一种颜色"，
/// 玩家不需要查图例就能判断"这颗子弹是谁的"。
Color projectileColor(bool isPlayerShot) noexcept {
    return isPlayerShot ? Color{255, 224, 150, 255}   // 玩家：暖黄
                        : Color{255, 128, 118, 255};  // 敌人：红
}

} // namespace

void AIDebugDraw::drawProjectiles(const Renderer& renderer, const Game::World& world) const {
    for (const Game::Projectile* projectile : world.projectiles()) {
        if (projectile == nullptr || projectile->terminated()) {
            continue;
        }

        const Color base = projectileColor(projectile->isPlayerShot());
        const glm::vec2 head = projectile->position();

        // --- 拖尾 ---
        // 从"当前位置往回退 trailLength"连到当前位置。
        // 这条线的长度由子弹速度决定，是它**真实走过的**路线的一段，
        // 而不是凭空画出来的整条弹道。所以子弹一停下，拖尾也就消失。
        //
        // 之所以需要拖尾：一颗只有 3 像素半径的圆点在快速移动时
        // 人眼几乎捕捉不到（尤其在 60 FPS 下每帧跳十几个单位）。
        // 拖尾把"瞬时位置"变成"运动矢量"，速度感与方向感都出来了。
        const glm::vec2 tail = head - projectile->direction() * projectile->trailLength();
        renderer.drawLine(tail, head, Fade(base, 0.55f), 2.0f);

        // --- 弹体 ---
        // 外层柔光 + 内层高亮核心：小半径的实心圆在深色背景上容易糊掉，
        // 套一圈半透明大圆能显著提升可辨识度。
        renderer.drawCircle(head, projectile->radius() * 2.4f, Fade(base, 0.30f));
        renderer.drawCircle(head, projectile->radius(), base);
        renderer.drawCircle(head, projectile->radius() * 0.5f, Color{255, 255, 245, 255});
    }
}

void AIDebugDraw::drawImpactMarks(const Renderer& renderer, const Game::World& world) const {
    const std::vector<Game::ImpactMark>& marks = world.impactMarks();
    if (marks.empty()) {
        return;
    }

    const double now = world.elapsedGameTime();

    for (const Game::ImpactMark& mark : marks) {
        // t: 0 = 刚落地，1 = 完全淡出。
        // 用 sqrt 而不是线性：弹着瞬间最亮、之后快速衰减，
        // 视觉上更像"一闪而过"，而不是慢慢褪色的激光。
        const double age = now - mark.gameTime;
        if (age < 0.0) {
            continue;   // 时钟回拨（暂停/单步），跳过这一条
        }
        const float life = std::clamp(
            static_cast<float>(age / Game::kImpactMarkLifetime), 0.0f, 1.0f);
        const float alpha = std::sqrt(1.0f - life);

        if (alpha <= 0.01f) {
            continue;
        }

        const Color base = projectileColor(mark.isPlayerShot);

        // --- 残余弹道 ---
        // 画得比飞行中的拖尾更细更淡：它的职责只是"指个方向"，
        // 不该抢走飞行中子弹的注意力。
        renderer.drawLine(mark.origin, mark.impact, Fade(base, alpha * 0.42f), 1.3f);

        // --- 枪口闪光 ---
        // 只在残影"年轻"时出现（前 25% 生命期），模拟枪口的瞬时光芒。
        if (life < 0.25f) {
            const float muzzleAlpha = (0.25f - life) / 0.25f;
            renderer.drawCircle(mark.origin, 6.0f * muzzleAlpha + 2.0f,
                                Fade(Color{255, 245, 200, 255}, muzzleAlpha * 0.85f));
            renderer.drawCircleLines(mark.origin, 10.0f * muzzleAlpha + 3.0f,
                                     Fade(base, muzzleAlpha * 0.7f), 1.5f);
        }

        // --- 弹着点 ---
        // 命中实体用实心圆 + 外环（有明确的"打中了"反馈），
        // 撞墙 / 打空用一个小十字，与命中的圆环区分开。
        if (life < 0.45f) {
            const float hitAlpha = (0.45f - life) / 0.45f;

            if (mark.hit) {
                renderer.drawCircle(mark.impact, 5.0f * hitAlpha + 2.0f,
                                    Fade(base, hitAlpha * 0.8f));
                renderer.drawCircleLines(mark.impact, 12.0f * hitAlpha + 4.0f,
                                         Fade(base, hitAlpha), 2.0f);
            } else {
                const float arm = 6.0f * hitAlpha + 3.0f;
                renderer.drawLine({mark.impact.x - arm, mark.impact.y - arm},
                                  {mark.impact.x + arm, mark.impact.y + arm},
                                  Fade(base, hitAlpha * 0.7f), 1.2f);
                renderer.drawLine({mark.impact.x - arm, mark.impact.y + arm},
                                  {mark.impact.x + arm, mark.impact.y - arm},
                                  Fade(base, hitAlpha * 0.7f), 1.2f);
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  伤害闪光
// ---------------------------------------------------------------------------
void AIDebugDraw::drawDamageFlashes(const Renderer& renderer, const Game::World& world) const {
    const double now = world.elapsedGameTime();

    for (const Game::DamageEvent& event : world.damageEvents()) {
        // 【修复】damageFlashDuration_ 与 setDamageFlashDuration() 此前是
        // 完全没人读取的死 API：闪光从不淡出，只能等 World 清理事件时
        // "啪"地消失。这里按事件年龄做淡出，让配置真正生效。
        double fade = 1.0;
        if (damageFlashDuration_ > 0.0) {
            const double age = now - event.gameTime;
            if (age < 0.0) {
                continue;   // 时钟回拨（暂停/单步）
            }
            fade = 1.0 - std::clamp(age / damageFlashDuration_, 0.0, 1.0);
        }
        if (fade <= 0.01) {
            continue;
        }

        // 用伤害量决定闪光大小与颜色强度
        const float intensity = std::clamp(event.amount / 20.0f, 0.3f, 1.0f);

        const Color color = event.isPlayerAttack
                                ? Color{255, 220, 140, 255}   // 玩家造成：暖黄
                                : Color{255, 110, 110, 255};  // 敌人造成：红

        renderer.drawCircle(event.impactPoint, 6.0f + intensity * 8.0f,
                            Fade(color, static_cast<float>(0.35 * fade)));
        renderer.drawCircleLines(event.impactPoint, 10.0f + intensity * 10.0f,
                                 Fade(color, static_cast<float>(0.8 * fade)), 2.0f);

        // 伤害数字：直接画在命中点上方
        char text[32];
        std::snprintf(text, sizeof(text), "-%.0f", static_cast<double>(event.amount));
        renderer.drawLabel(event.impactPoint + glm::vec2(0.0f, -26.0f), text, color, 12);
    }
}

// ---------------------------------------------------------------------------
//  屏幕叠加
// ---------------------------------------------------------------------------
void AIDebugDraw::drawOverlay(const Renderer& renderer, const Game::World& world) {
    (void)world;

    // 右下角的图例：解释各种视觉元素的含义。
    // 没有图例的调试可视化几乎没用——没人能记住每种颜色代表什么。
    if (!flags_.showEQSCandidates && !flags_.showPerception && !flags_.showPaths &&
        !flags_.showShotTracers) {
        return;
    }

    const glm::vec2 screen = renderer.screenSize();
    constexpr float kLegendWidth = 210.0f;
    constexpr float kLineHeight = 17.0f;

    std::vector<std::pair<Color, std::string>> entries;

    if (flags_.showShotTracers) {
        entries.emplace_back(Color{255, 224, 150, 255}, "玩家子弹 / 弹道");
        entries.emplace_back(Color{255, 128, 118, 255}, "敌人子弹 / 弹道");
        entries.emplace_back(Color{255, 255, 245, 255}, "弹着点 (命中 / 未命中)");
    }
    if (flags_.showPerception) {
        entries.emplace_back(Fade(Colors::kPerceptionFov, 0.9f), "感知视野锥 (FOV)");
        entries.emplace_back(Colors::kSuccess, "视线通畅 / 已锁定目标");
        entries.emplace_back(Colors::kWarning, "可见但反应延迟中");
        entries.emplace_back(Color{110, 110, 125, 200}, "记忆中的最后位置");
    }
    if (flags_.showEQSCandidates) {
        entries.emplace_back(Colors::kEQSBest, "EQS 最优位置");
        entries.emplace_back(colorForScore(0.5f), "EQS 候选点 (按分数着色)");
    }
    if (flags_.showPaths) {
        entries.emplace_back(Colors::kPath, "寻路路径");
        entries.emplace_back(Color{255, 190, 90, 220}, "部分路径 (目标不可达)");
    }

    if (entries.empty()) {
        return;
    }

    const float panelHeight = kLineHeight * static_cast<float>(entries.size() + 1) + 12.0f;
    const ::Rectangle panel{
        screen.x - kLegendWidth - 16.0f,
        screen.y - panelHeight - 16.0f,
        kLegendWidth,
        panelHeight
    };

    renderer.drawPanel(panel, Fade(Colors::kPanelBg, 0.88f), Fade(Colors::kPanelBorder, 0.9f));

    renderer.drawScreenText("图例", {panel.x + 10.0f, panel.y + 6.0f}, 12, Colors::kAccent);

    float y = panel.y + 6.0f + kLineHeight;
    for (const auto& [color, label] : entries) {
        renderer.drawScreenRect({panel.x + 10.0f, y + 5.0f, 10.0f, 8.0f}, color);
        renderer.drawScreenText(label, {panel.x + 26.0f, y}, 11, Colors::kTextPrimary);
        y += kLineHeight;
    }
}

} // namespace TacticalAI::Debug
