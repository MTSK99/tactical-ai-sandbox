// ===========================================================================
//  src/debug/DebugUI.cpp
// ===========================================================================
#include "debug/DebugUI.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "raylib.h"

// imgui 后端可用性由 CMake 探测后定义。
// 未定义时走原生 HUD 降级路径——代码必须两条路都能编译。
#if defined(TACTICALAI_HAS_IMGUI_RAYLIB_BACKEND)
#  include "imgui.h"
#  include "backends/imgui_impl_raylib.h"
#  define TACTICALAI_IMGUI_AVAILABLE 1
#else
#  define TACTICALAI_IMGUI_AVAILABLE 0
#endif

namespace TacticalAI::Debug {
namespace {

// Colors 是命名空间，只能用 namespace 别名引入（using 声明不接受命名空间）
namespace Colors = Engine::Colors;

/// 把布尔值格式化为 ON/off。
/// 只有 imgui 面板会用到它；在没有 imgui 后端时它是未使用函数，
/// 因此标记 [[maybe_unused]] 而不是删掉（面板是主要 UI，不能为了消警告砍功能）。
[[maybe_unused]] const char* onOff(bool value) noexcept {
    return value ? "ON" : "off";
}

/// 面板宽度：固定 380 像素，够显示两列参数而不遮挡太多游戏画面。
constexpr float kPanelWidth = 380.0f;
constexpr float kPanelMargin = 12.0f;

} // namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
DebugUI::~DebugUI() {
    // 注意：不在此处调用 shutdown()，因为 ~DebugUI 可能发生在窗口销毁之后。
    // 由 main 显式调用 shutdown()，保证顺序正确。
}

bool DebugUI::initialize() {
    if (initialized_) {
        return imguiActive_;
    }

#if TACTICALAI_IMGUI_AVAILABLE
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    // 让面板更紧凑：默认样式的间距太大，一屏放不下几个参数
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 4.0f;
    style.FrameRounding     = 3.0f;
    style.ItemSpacing       = ImVec2(6.0f, 4.0f);
    style.WindowPadding     = ImVec2(8.0f, 8.0f);
    style.ScrollbarSize     = 12.0f;

    if (!ImGui_ImplRaylib_Init()) {
        std::fprintf(stderr, "[DebugUI] imgui raylib 后端初始化失败，降级为原生 HUD\n");
        ImGui::DestroyContext();
        imguiActive_ = false;
    } else {
        imguiActive_ = true;
        std::printf("[DebugUI] imgui 后端已启用（完整调参面板可用）\n");
    }
#else
    std::printf("[DebugUI] 未编译 imgui 后端，使用原生 HUD（只读，F1 切换）\n");
    imguiActive_ = false;
#endif

    initialized_ = true;
    return imguiActive_;
}

void DebugUI::shutdown() {
    if (!initialized_) {
        return;
    }

#if TACTICALAI_IMGUI_AVAILABLE
    if (imguiActive_) {
        ImGui_ImplRaylib_Shutdown();
        ImGui::DestroyContext();
        imguiActive_ = false;
    }
#endif

    initialized_ = false;
}

void DebugUI::beginFrame() {
    wantsMouseCapture_ = false;

#if TACTICALAI_IMGUI_AVAILABLE
    if (imguiActive_) {
        ImGui_ImplRaylib_NewFrame();
        ImGui::NewFrame();
    }
#endif
}

void DebugUI::endFrame() {
#if TACTICALAI_IMGUI_AVAILABLE
    if (imguiActive_) {
        ImGui::Render();
        ImGui_ImplRaylib_RenderDrawData(ImGui::GetDrawData());
    }
#endif
}

// ---------------------------------------------------------------------------
//  主绘制入口
// ---------------------------------------------------------------------------
void DebugUI::draw(const DebugUIBindings& bindings, const DebugUIActions& actions,
                   const Engine::LoopStats& loopStats) {
    if (!visible_) {
        return;
    }

#if !TACTICALAI_IMGUI_AVAILABLE
    // 没有 imgui 后端时走原生 HUD，用不到 actions（按钮只在面板里存在）
    (void)actions;
#endif

#if TACTICALAI_IMGUI_AVAILABLE
    if (imguiActive_) {
        drawImGuiPanels(bindings, actions, loopStats);
        return;
    }
#endif

    // 原生 HUD 降级路径
    if (bindings.world && bindings.drawFlags) {
        // 从状态条下方开始画。顶部那块是游戏自己的 HUD（HP / 子弹 / 命中），
        // 这里再画一遍就会文字压文字，看起来像乱码。
        drawFallbackHud(bindings, loopStats, kPanelMargin + bindings.topReserved);
    }
}

// ---------------------------------------------------------------------------
//  imgui 面板
// ---------------------------------------------------------------------------
#if TACTICALAI_IMGUI_AVAILABLE

void DebugUI::drawImGuiPanels(const DebugUIBindings& bindings, const DebugUIActions& actions,
                              const Engine::LoopStats& loopStats) {
    if (!bindings.world || !bindings.time || !bindings.drawFlags) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) {
        return;
    }

    // 固定在左上角，避免遮挡右下角的图例。
    // 纵向同样让开顶部状态条占用的高度（bindings 在这一层拿不到，
    // 用同样的方式从调用方注入；见 drawFallbackHud 处的说明）。
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + kPanelMargin,
                                   viewport->WorkPos.y + kPanelMargin + bindings.topReserved),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(kPanelWidth, viewport->WorkSize.y - kPanelMargin * 2.0f),
                             ImGuiCond_FirstUseEver);

    ImGui::Begin("Tactical AI Sandbox", &visible_, ImGuiWindowFlags_NoCollapse);

    // 鼠标悬停在面板上时，禁止相机拖拽
    wantsMouseCapture_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // --- 标签栏 ---
    if (ImGui::BeginTabBar("##MainTabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("概览")) {
            activeTab_ = 0;
            drawOverviewTab(bindings, loopStats);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("世界")) {
            activeTab_ = 1;
            drawWorldTab(bindings, actions);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("AI 调参")) {
            activeTab_ = 2;
            drawAITuningTab(bindings, actions);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("实体检视")) {
            activeTab_ = 3;
            drawEntityInspectTab(bindings);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("可视化")) {
            activeTab_ = 4;
            drawVisualizationTab(bindings);
            ImGui::EndTabItem();
        }

        // 自定义标签页
        for (CustomTab& tab : customTabs_) {
            if (ImGui::BeginTabItem(tab.first.c_str())) {
                if (tab.second) {
                    tab.second();
                }
                ImGui::EndTabItem();
            }
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
//  概览
// ---------------------------------------------------------------------------
void DebugUI::drawOverviewTab(const DebugUIBindings& bindings, const Engine::LoopStats& loopStats) {
    const Engine::Time& time = *bindings.time;
    const Engine::FrameTime& frame = time.frame();

    // --- 性能 ---
    ImGui::SeparatorText("性能");

    // 用颜色编码帧率：绿 = 流畅，黄 = 勉强，红 = 有问题。
    // 这比读数字快得多。
    ImVec4 fpsColor = ImVec4(0.47f, 0.90f, 0.67f, 1.0f);
    if (frame.fps < 50.0f) { fpsColor = ImVec4(1.0f, 0.75f, 0.27f, 1.0f); }
    if (frame.fps < 30.0f) { fpsColor = ImVec4(0.92f, 0.39f, 0.37f, 1.0f); }

    ImGui::TextColored(fpsColor, "%.1f FPS", static_cast<double>(frame.fps));
    ImGui::SameLine();
    ImGui::Text("| 帧 %.2f ms", frame.deltaTime * 1000.0);
    ImGui::SameLine();
    ImGui::Text("| 逻辑 %.2f ms", frame.fixedDelta * 1000.0);

    ImGui::Text("渲染帧: %llu", static_cast<unsigned long long>(frame.frameCount));
    ImGui::Text("逻辑步: %llu", static_cast<unsigned long long>(loopStats.totalFixedSteps));
    ImGui::Text("本帧补步: %d", frame.stepsThisFrame);

    // 健康度警告：这些指标异常时 AI 行为会不稳定
    if (loopStats.maxStepsInOneFrame >= Engine::Time::kMaxStepsPerFrame - 1) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.27f, 1.0f),
                           "警告: 单帧补步达上限 (%d)，逻辑可能跟不上",
                           loopStats.maxStepsInOneFrame);
    }
    if (loopStats.stepStarvation > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.27f, 1.0f),
                           "警告: 检测到逻辑饥饿（长时间未推进逻辑）");
    }

    ImGui::Spacing();

    // --- 时间控制 ---
    ImGui::SeparatorText("时间控制");

    ImGui::Text("游戏时间: %.2f s", time.gameTime());
    ImGui::Text("真实时间: %.2f s", time.realTime());
    ImGui::Text("累积器: %.3f ms", time.accumulator() * 1000.0);
    ImGui::Text("插值系数: %.3f", static_cast<double>(time.interpolationAlpha()));

    // 【修复】原先这里写的是
    //     ImGui::Checkbox("暂停", const_cast<bool*>(&time.isPaused()))
    // 而 Time::isPaused() 是按值返回的 bool —— 对纯右值取地址是 ill-formed
    // （MSVC C2102 / GCC、Clang 同样拒绝），只要启用 imgui 后端就无法编译。
    // 该勾选框与下一行的「暂停逻辑 (P)」功能完全重复，直接移除。

    // 暂停/单步按钮（用临时变量绕开 const 引用）
    bool paused = time.isPaused();
    if (ImGui::Checkbox("暂停逻辑 (P)", &paused)) {
        // 直接改时间状态：面板是唯一的例外，因为它就是要控制引擎
        bindings.time->setPaused(paused);
    }

    ImGui::SameLine();
    if (ImGui::Button("单步 (.)")) {
        if (bindings.time->isPaused()) {
            bindings.time->requestSingleStep();
        }
    }

    float timeScale = static_cast<float>(time.timeScale());
    if (ImGui::SliderFloat("时间缩放", &timeScale, 0.0f, 4.0f, "%.2fx")) {
        bindings.time->setTimeScale(static_cast<double>(timeScale));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("1x")) {
        bindings.time->setTimeScale(1.0);
    }

    float fixedDeltaMs = static_cast<float>(time.fixedDelta() * 1000.0);
    if (ImGui::SliderFloat("逻辑步长", &fixedDeltaMs, 4.0f, 50.0f, "%.1f ms")) {
        bindings.time->setFixedDelta(static_cast<double>(fixedDeltaMs) / 1000.0);
    }
    ImGui::TextDisabled("逻辑帧率: %.0f Hz", 1.0 / time.fixedDelta());

    ImGui::Spacing();

    // --- 世界统计 ---
    if (bindings.world) {
        ImGui::SeparatorText("世界");

        ImGui::Text("实体总数: %zu", bindings.world->entities().size());
        ImGui::Text("存活敌人: %zu / %zu",
                    bindings.world->aliveEnemyCount(), bindings.world->enemyCount());
        ImGui::Text("击杀数: %d", bindings.world->enemiesKilled());
        ImGui::Text("总射击: %zu", bindings.world->totalShotsFired());
        ImGui::Text("总伤害: %.0f", static_cast<double>(bindings.world->totalDamageDealt()));

        // AI 耗时是关键指标：超过逻辑步长就会导致帧率问题
        const double aiMs = bindings.world->averageAITimeMs();
        ImVec4 aiColor = ImVec4(0.47f, 0.90f, 0.67f, 1.0f);
        if (aiMs > time.fixedDelta() * 1000.0 * 0.3) {
            aiColor = ImVec4(1.0f, 0.75f, 0.27f, 1.0f);
        }
        if (aiMs > time.fixedDelta() * 1000.0 * 0.6) {
            aiColor = ImVec4(0.92f, 0.39f, 0.37f, 1.0f);
        }
        ImGui::TextColored(aiColor, "AI 平均耗时: %.3f ms", aiMs);
        ImGui::TextDisabled("逻辑步预算: %.2f ms", time.fixedDelta() * 1000.0);

        if (ImGui::Button("打印世界状态")) {
            std::printf("%s\n", bindings.world->dump().c_str());
        }
    }
}

// ---------------------------------------------------------------------------
//  世界
// ---------------------------------------------------------------------------
void DebugUI::drawWorldTab(const DebugUIBindings& bindings, const DebugUIActions& actions) {
    if (!bindings.world) {
        return;
    }
    Game::World& world = *bindings.world;

    ImGui::SeparatorText("操作");

    if (ImGui::Button("生成敌人 (N)", ImVec2(-1.0f, 0.0f)) && actions.spawnEnemy) {
        actions.spawnEnemy();
    }
    if (ImGui::Button("全部重新寻路 (R)", ImVec2(-1.0f, 0.0f)) && actions.requestRepath) {
        actions.requestRepath();
    }

    ImGui::Spacing();
    if (ImGui::Button("对玩家造成伤害", ImVec2(-1.0f, 0.0f)) && actions.damagePlayer) {
        actions.damagePlayer();
    }
    ImGui::TextDisabled("用于观察 AI 的撤退行为");

    ImGui::Spacing();
    if (ImGui::Button("清空所有敌人", ImVec2(-1.0f, 0.0f)) && actions.clearEnemies) {
        actions.clearEnemies();
    }

    // 重置按钮放在最下，且用红色提示它是破坏性操作
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
    if (ImGui::Button("重置世界", ImVec2(-1.0f, 0.0f)) && actions.resetWorld) {
        actions.resetWorld();
    }
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::SeparatorText("地形");

    const AI::NavGrid& grid = world.navGrid();
    ImGui::Text("网格: %d x %d", grid.columns(), grid.rows());
    ImGui::Text("格边长: %.1f", static_cast<double>(grid.cellSize()));
    ImGui::Text("可通行: %zu / %zu", grid.walkableCellCount(),
                static_cast<size_t>(grid.columns()) * static_cast<size_t>(grid.rows()));

    ImGui::Spacing();
    ImGui::SeparatorText("实体列表");

    // 实体表：用固定高度 + 滚动，避免实体多了把面板撑爆
    ImGui::BeginChild("##EntityList", ImVec2(0.0f, 240.0f), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);

    if (ImGui::BeginTable("##Entities", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("位置", ImGuiTableColumnFlags_WidthFixed, 96.0f);
        ImGui::TableSetupColumn("HP", ImGuiTableColumnFlags_WidthFixed, 46.0f);
        ImGui::TableHeadersRow();

        for (const auto& entity : world.entities()) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            const char* typeName = Game::toString(entity->type());
            if (entity->isDead()) {
                ImGui::TextDisabled("%s", typeName);
            } else {
                ImGui::TextUnformatted(typeName);
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", entity->name().c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.0f, %.0f",
                        static_cast<double>(entity->position().x),
                        static_cast<double>(entity->position().y));

            ImGui::TableSetColumnIndex(3);
            if (entity->type() == Game::EntityType::Obstacle) {
                ImGui::TextDisabled("--");
            } else {
                const float ratio = entity->healthRatio();
                ImVec4 hpColor = ImVec4(0.47f, 0.90f, 0.67f, 1.0f);
                if (ratio < 0.5f) { hpColor = ImVec4(1.0f, 0.75f, 0.27f, 1.0f); }
                if (ratio < 0.25f) { hpColor = ImVec4(0.92f, 0.39f, 0.37f, 1.0f); }
                ImGui::TextColored(hpColor, "%.0f", static_cast<double>(entity->health()));
            }
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
//  AI 调参
// ---------------------------------------------------------------------------
void DebugUI::drawAITuningTab(const DebugUIBindings& bindings, const DebugUIActions& actions) {
    if (!bindings.world) {
        return;
    }

    Game::World& world = *bindings.world;
    const bool hasEnemies = world.aliveEnemyCount() > 0;

    if (!hasEnemies) {
        ImGui::TextDisabled("当前没有存活的敌人");
        ImGui::TextDisabled("先生成一个敌人再调参");
        return;
    }

    // 取第一个存活敌人作为「配置代理」。
    // 所有敌人共享同一份 EnemyConfig 数值（通过 applyToAll 回写），
    // 这样调参影响全体，符合「统一调整 AI 强度」的意图。
    Game::Enemy* proxy = nullptr;
    for (const auto& entity : world.entities()) {
        if (entity->type() == Game::EntityType::Enemy && !entity->isDead()) {
            proxy = static_cast<Game::Enemy*>(entity.get());
            break;
        }
    }
    if (!proxy) {
        return;
    }

    // 把改动同步到所有敌人。
    // 单个敌人调参在沙盒里没有意义（会得到行为不一致的敌人群）。
    auto applyToAll = [&world](const Game::EnemyConfig& config) {
        for (const auto& entity : world.entities()) {
            if (entity->type() != Game::EntityType::Enemy) {
                continue;
            }
            auto* enemy = static_cast<Game::Enemy*>(entity.get());
            enemy->config() = config;
            enemy->perceptionConfig() = config.perception;
        }
    };

    Game::EnemyConfig config = proxy->config();
    AI::PerceptionConfig perception = config.perception;

    bool changed = false;
    bool perceptionChanged = false;

    // --- 感知 ---
    if (ImGui::CollapsingHeader("感知 (Perception)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("这些参数决定 AI 看起来「多聪明」");

        perceptionChanged |= ImGui::SliderFloat("视野距离", &perception.viewDistance,
                                                60.0f, 900.0f, "%.0f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("AI 能看多远。调大后 AI 会提前发现玩家");
        }

        // 内部存弧度，UI 显示角度——用临时变量转换，
        // 避免用户面对 0.785 这种不可读的数字
        float halfFovDegrees = perception.halfFovAngle * 180.0f / 3.14159265f;
        if (ImGui::SliderFloat("视野半角", &halfFovDegrees, 5.0f, 180.0f, "%.0f deg")) {
            perception.halfFovAngle = halfFovDegrees * 3.14159265f / 180.0f;
            perceptionChanged = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("总视野 = 半角 x 2。当前总视野 %.0f 度", halfFovDegrees * 2.0f);
        }

        perceptionChanged |= ImGui::SliderFloat("贴身察觉半径", &perception.proximityRadius,
                                                0.0f, 200.0f, "%.0f");

        float reactionMs = static_cast<float>(perception.reactionTime * 1000.0);
        if (ImGui::SliderFloat("反应延迟", &reactionMs, 0.0f, 1500.0f, "%.0f ms")) {
            perception.reactionTime = static_cast<double>(reactionMs) / 1000.0;
            perceptionChanged = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("看见目标后需要持续可见这么久才确认。\n"
                              "设为 0 会让 AI 瞬间反应，看起来很假");
        }

        float memorySeconds = static_cast<float>(perception.memoryDuration);
        if (ImGui::SliderFloat("记忆时长", &memorySeconds, 0.0f, 30.0f, "%.1f s")) {
            perception.memoryDuration = static_cast<double>(memorySeconds);
            perceptionChanged = true;
        }

        perceptionChanged |= ImGui::Checkbox("启用视线遮挡", &perception.enableLineOfSight);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("关闭后 AI 可以穿墙感知。\n用于对比「有/无遮挡」的行为差异");
        }
    }

    // --- 战斗 ---
    if (ImGui::CollapsingHeader("战斗 (Combat)", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::SliderFloat("理想交战距离", &config.preferredRange,
                                      60.0f, 700.0f, "%.0f");
        changed |= ImGui::SliderFloat("开火射程", &config.fireRange,
                                      60.0f, 900.0f, "%.0f");

        float cooldownMs = config.fireCooldown * 1000.0f;
        if (ImGui::SliderFloat("射击间隔", &cooldownMs, 100.0f, 3000.0f, "%.0f ms")) {
            config.fireCooldown = cooldownMs / 1000.0f;
            changed = true;
        }

        changed |= ImGui::SliderFloat("单发伤害", &config.damage, 1.0f, 30.0f, "%.0f");

        // 开火判据用"横向偏差"而不是角度：角度误差会随距离放大，
        // 而命中窗口是固定的，两者无法直接比较。详见 EnemyConfig::aimHitRadius。
        if (ImGui::SliderFloat("瞄准精度", &config.aimHitRadius, 3.0f, 60.0f, "%.0f 单位")) {
            changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("弹道在目标处的横向偏差小于此值才开火（世界单位）。\n"
                              "默认 14 = 敌人半径 11 + 子弹半径 3，即擦边即命中。\n"
                              "调大 = AI 更急于开火但命中率下降；调小 = 更耐心更精准");
        }

        float retreatPercent = config.retreatHealthRatio * 100.0f;
        if (ImGui::SliderFloat("撤退血量阈值", &retreatPercent, 0.0f, 90.0f, "%.0f%%")) {
            config.retreatHealthRatio = retreatPercent / 100.0f;
            changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("血量低于此比例触发撤退分支");
        }
    }

    // --- 移动 ---
    if (ImGui::CollapsingHeader("移动 (Movement)")) {
        changed |= ImGui::SliderFloat("移动速度", &config.moveSpeed, 30.0f, 400.0f, "%.0f");
        changed |= ImGui::SliderFloat("冲刺速度", &config.sprintSpeed, 30.0f, 600.0f, "%.0f");
        changed |= ImGui::SliderFloat("加速度", &config.acceleration, 100.0f, 3000.0f, "%.0f");
        changed |= ImGui::SliderFloat("碰撞半径", &config.radius, 4.0f, 30.0f, "%.1f");
    }

    // --- 行为 ---
    if (ImGui::CollapsingHeader("行为开关 (Behavior)", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::Checkbox("启用 EQS 选位", &config.useEQS);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("关闭后 AI 会直线冲向目标。\n"
                              "对比开关效果最能体现 EQS 的价值");

        }

        float repositionSeconds = static_cast<float>(config.repositionInterval);
        if (ImGui::SliderFloat("重新选位间隔", &repositionSeconds, 0.2f, 10.0f, "%.1f s")) {
            config.repositionInterval = static_cast<double>(repositionSeconds);
            changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("调太小会让 AI 频繁换位（看起来像原地打转）");
        }

        float searchSeconds = static_cast<float>(config.searchDuration);
        if (ImGui::SliderFloat("搜索持续时长", &searchSeconds, 0.0f, 30.0f, "%.1f s")) {
            config.searchDuration = static_cast<double>(searchSeconds);
            changed = true;
        }
    }

    // --- 应用改动 ---
    if (changed || perceptionChanged) {
        config.perception = perception;
        applyToAll(config);
    }

    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Button("全部重置为默认值", ImVec2(-1.0f, 0.0f))) {
        const Game::EnemyConfig defaults;
        applyToAll(defaults);
    }

    ImGui::Spacing();
    ImGui::TextDisabled("修改会立即应用到所有存活敌人");
}

// ---------------------------------------------------------------------------
//  实体检视
// ---------------------------------------------------------------------------
void DebugUI::drawEntityInspectTab(const DebugUIBindings& bindings) {
    if (!bindings.world) {
        return;
    }
    Game::World& world = *bindings.world;

    ImGui::SeparatorText("敌人 AI 检视");
    ImGui::TextDisabled("展开查看黑板、行为树与状态历史");

    size_t index = 0;
    for (const auto& entity : world.entities()) {
        if (entity->type() != Game::EntityType::Enemy) {
            continue;
        }
        const auto* enemy = static_cast<const Game::Enemy*>(entity.get());
        drawEnemyInspector(*enemy, index++);
    }

    if (index == 0) {
        ImGui::TextDisabled("没有敌人可检视");
    }
}

void DebugUI::drawEnemyInspector(const Game::Enemy& enemy, size_t index) {
    ImGui::PushID(static_cast<int>(enemy.id()));

    // 标题行：名字 + 状态 + 生命值，让收起的条目也有信息量
    char label[192];
    std::snprintf(label, sizeof(label), "#%zu %s  [%s]  hp %.0f/%.0f",
                  index,
                  enemy.name().c_str(),
                  Game::toString(enemy.state()),
                  static_cast<double>(enemy.health()),
                  static_cast<double>(enemy.maxHealth()));

    if (ImGui::CollapsingHeader(label)) {
        // --- 关键指标 ---
        ImGui::Text("位置: (%.0f, %.0f)",
                    static_cast<double>(enemy.position().x),
                    static_cast<double>(enemy.position().y));
        ImGui::Text("威胁等级: %.2f",
                    static_cast<double>(enemy.perception().threatLevel()));
        ImGui::Text("行为树: %s (tick %llu)",
                    AI::toString(enemy.behaviorTree().lastStatus()),
                    static_cast<unsigned long long>(enemy.behaviorTree().tickCount()));
        ImGui::Text("路径段数: %zu%s",
                    enemy.pathWaypoints().size(),
                    enemy.currentPath().partial ? " (部分)" : "");
        ImGui::Text("射击: %d | 伤害: %.0f",
                    enemy.shotsFired(), static_cast<double>(enemy.damageDealt()));

        // --- 黑板 ---
        if (ImGui::TreeNode("黑板 (Blackboard)")) {
            const AI::Blackboard& blackboard = enemy.blackboard();
            ImGui::TextDisabled("共 %zu 个键，累计 %llu 次写入",
                                blackboard.size(),
                                static_cast<unsigned long long>(blackboard.totalWrites()));

            if (ImGui::BeginTable("##BB", 4,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                                  ImVec2(0.0f, 200.0f))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("键", ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableSetupColumn("值", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("写者", ImGuiTableColumnFlags_WidthFixed, 76.0f);
                ImGui::TableSetupColumn("次数", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                ImGui::TableHeadersRow();

                for (const std::string& key : blackboard.keys()) {
                    const AI::BlackboardEntry* entry = blackboard.entry(key);
                    if (!entry) {
                        continue;
                    }

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(key.c_str());

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(AI::Blackboard::valueToString(*entry).c_str());

                    ImGui::TableSetColumnIndex(2);
                    // 写者名很重要：能看出是哪个节点在疯狂写这个键
                    ImGui::TextDisabled("%s", entry->lastWriter.c_str());

                    ImGui::TableSetColumnIndex(3);
                    // 写入次数异常高 = 性能问题信号
                    if (entry->writeCount > 3000) {
                        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.27f, 1.0f), "%d",
                                           entry->writeCount);
                    } else {
                        ImGui::TextDisabled("%d", entry->writeCount);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }

        // --- 行为树 ---
        if (ImGui::TreeNode("行为树结构")) {
            ImGui::TextUnformatted(enemy.behaviorTree().describe().c_str());
            ImGui::TextDisabled("节点总数: %zu", enemy.behaviorTree().nodeCount());
            ImGui::TreePop();
        }

        // --- 感知记录 ---
        if (ImGui::TreeNode("感知记录")) {
            const auto& records = enemy.perception().records();
            if (records.empty()) {
                ImGui::TextDisabled("无感知记录");
            } else {
                for (const AI::PerceptionRecord& record : records) {
                    ImVec4 color = ImVec4(0.55f, 0.55f, 0.60f, 1.0f);
                    if (record.visibleNow && record.confirmed) {
                        color = ImVec4(0.47f, 0.90f, 0.67f, 1.0f);
                    } else if (record.visibleNow) {
                        color = ImVec4(1.0f, 0.75f, 0.27f, 1.0f);
                    }

                    ImGui::TextColored(color, "目标 #%u  距离 %.0f  可见=%s  确认=%s",
                                       record.target.id,
                                       static_cast<double>(record.distance),
                                       onOff(record.visibleNow),
                                       onOff(record.confirmed));
                }
            }
            ImGui::TreePop();
        }

        // --- EQS 结果 ---
        if (ImGui::TreeNode("最近的 EQS 结果")) {
            const AI::EQSResult& result = enemy.lastPositionQuery();
            ImGui::Text("查询: %s", result.queryName.c_str());
            ImGui::Text("评估: %d 个候选", result.evaluatedCount);
            ImGui::Text("有效: %zu | 剔除: %zu",
                        result.candidates.size(), result.rejected.size());
            ImGui::Text("耗时: %.3f ms", result.elapsedMs);

            if (const AI::EQSCandidate* best = result.best()) {
                ImGui::Separator();
                ImGui::Text("最优位置: (%.0f, %.0f)  得分 %.3f",
                            static_cast<double>(best->position.x),
                            static_cast<double>(best->position.y),
                            static_cast<double>(best->score));

                for (size_t i = 0; i < best->testScores.size(); ++i) {
                    char testLabel[32];
                    std::snprintf(testLabel, sizeof(testLabel), "测试项 %zu", i);
                    ImGui::ProgressBar(best->testScores[i], ImVec2(-1.0f, 0.0f), testLabel);
                }
            }

            // 剔除原因统计：能发现「过滤器是不是设得太严了」
            if (!result.rejected.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("剔除原因示例:");
                const size_t showCount = std::min<size_t>(result.rejected.size(), 3);
                for (size_t i = 0; i < showCount; ++i) {
                    ImGui::BulletText("%s", result.rejected[i].rejectReason.c_str());
                }
            }
            ImGui::TreePop();
        }
    }

    ImGui::PopID();
}

// ---------------------------------------------------------------------------
//  可视化开关
// ---------------------------------------------------------------------------
void DebugUI::drawVisualizationTab(const DebugUIBindings& bindings) {
    if (!bindings.drawFlags) {
        return;
    }
    DebugDrawFlags& flags = *bindings.drawFlags;

    ImGui::SeparatorText("绘制开关");
    ImGui::TextDisabled("快捷键对应数字键 1-6");

    ImGui::Checkbox("感知视野锥 [1]", &flags.showPerception);
    ImGui::Checkbox("行为树状态 [2]", &flags.showBehaviorTree);
    ImGui::Checkbox("EQS 候选点 [3]", &flags.showEQSCandidates);
    ImGui::Checkbox("寻路路径 [4]", &flags.showPaths);
    // 【修复】showGrid（[5]）此前在面板里根本没有对应勾选框，
    // 只能通过"全部开启/关闭"间接控制，与 HUD 上的 [5] 提示对不上。
    ImGui::Checkbox("导航网格 [5]", &flags.showGrid);
    ImGui::Checkbox("实体与朝向", &flags.showEntities);
    ImGui::Checkbox("实体标签 [6]", &flags.showLabels);

    ImGui::Spacing();
    ImGui::SeparatorText("高级");

    // 【修复】[5] 属于 showGrid（导航网格，见 DebugDrawFlags::summary），
    // 这里原本把同一个编号标给了 showNavGridBlocked，两处语义冲突。
    ImGui::Checkbox("网格阻挡格（逐格）", &flags.showNavGridBlocked);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("逐格绘制阻挡区域，敌人多时会有性能开销");
    }
    ImGui::Checkbox("伤害事件", &flags.showDamageEvents);
    ImGui::Checkbox("威胁源", &flags.showThreatSources);

    ImGui::Spacing();
    ImGui::SeparatorText("快捷操作");

    if (ImGui::Button("全部开启", ImVec2(-1.0f, 0.0f))) {
        flags = DebugDrawFlags{};
        flags.showPerception = true;
        flags.showBehaviorTree = true;
        flags.showEQSCandidates = true;
        flags.showPaths = true;
        flags.showEntities = true;
        flags.showLabels = true;
        flags.showGrid = true;
        flags.showNavGridBlocked = true;
        flags.showDamageEvents = true;
        flags.showThreatSources = true;
    }
    if (ImGui::Button("全部关闭", ImVec2(-1.0f, 0.0f))) {
        // 【修复】DebugDrawFlags{} 的默认值里大多数字段是 true
        // （showEQSCandidates / showPaths / showEntities / showLabels /
        // showGrid / showDamageEvents / showShotTracers 默认开启），
        // 用默认值"重置"等于什么都没关掉。这里必须逐项置 false。
        flags.showPerception     = false;
        flags.showBehaviorTree   = false;
        flags.showEQSCandidates  = false;
        flags.showPaths          = false;
        flags.showEntities       = false;
        flags.showLabels         = false;
        flags.showGrid           = false;
        flags.showNavGridBlocked = false;
        flags.showDamageEvents   = false;
        flags.showThreatSources  = false;
        flags.showShotTracers    = false;
    }
    if (ImGui::Button("仅 AI 可视化", ImVec2(-1.0f, 0.0f))) {
        // 【修复】原先只改 6 个字段、其余保持原值，与按钮名不符。
        // 改为先把无关项关掉，再打开 AI 可视化相关的 4 项。
        flags.showBehaviorTree   = false;
        flags.showEntities       = false;
        flags.showDamageEvents   = false;
        flags.showThreatSources  = false;
        flags.showShotTracers    = false;

        flags.showPerception     = true;
        flags.showEQSCandidates  = true;
        flags.showPaths          = true;
        flags.showLabels         = true;
        flags.showGrid           = false;
        flags.showNavGridBlocked = false;
    }
}

#endif // TACTICALAI_IMGUI_AVAILABLE

// ---------------------------------------------------------------------------
//  原生 HUD 字体
// ---------------------------------------------------------------------------
// 静态成员定义。用静态（而非成员）的原因是原生 HUD 的绘制路径是 const 的，
// 而 drawFallbackHud 又被设计为不持有 Renderer 引用（保持 debug 层的独立性）。
::Font DebugUI::s_fallbackFont{};
bool   DebugUI::s_hasFallbackFont = false;

void DebugUI::setFallbackFont(::Font font) noexcept {
    s_fallbackFont    = font;
    s_hasFallbackFont = (font.texture.id != 0) && (font.glyphCount > 0);
}

::Font DebugUI::fallbackFont() noexcept {
    return s_hasFallbackFont ? s_fallbackFont : GetFontDefault();
}

// ---------------------------------------------------------------------------
//  原生 HUD 降级路径
// ---------------------------------------------------------------------------
void DebugUI::drawFallbackLine(const Engine::Renderer& renderer, const std::string& text,
                                float& y, float x, ::Color color, int fontSize) const {
    renderer.drawScreenText(text, {x, y}, fontSize, color);
    y += static_cast<float>(fontSize) + 4.0f;
}

void DebugUI::drawFallbackHud(const DebugUIBindings& bindings, const Engine::LoopStats& loopStats,
                              float startY) const {
    // 原生 HUD 用半透明底板 + 等宽文本，信息密度低于 imgui 但完全可读。
    // 它是「没有 imgui 时的保底」，因此只显示最关键的指标，不做交互。

    std::vector<std::pair<std::string, ::Color>> lines;

    const Engine::Time* time = bindings.time;
    const Game::World* world = bindings.world;

    if (time) {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "FPS %.0f | frame %llu | dt %.1fms",
                      static_cast<double>(time->fps()),
                      static_cast<unsigned long long>(time->frameCount()),
                      time->deltaTime() * 1000.0);
        lines.emplace_back(buffer, Colors::kTextPrimary);

        // 【修复】面板上加上"累计逻辑步"：它是判断"逻辑到底有没有在跑"最直接
        // 的数字，此前 fallback HUD 只显示本帧步数，暂停/补步时很难看出区别。
        // 顺带这也让 loopStats 参数真正被用上（此前是未使用参数）。
        std::snprintf(buffer, sizeof(buffer), "logic %.1fms steps %d | total %llu | game %.1fs",
                      time->fixedDelta() * 1000.0,
                      time->frame().stepsThisFrame,
                      static_cast<unsigned long long>(loopStats.totalFixedSteps),
                      time->gameTime());
        lines.emplace_back(buffer, Colors::kTextMuted);

        if (time->isPaused()) {
            lines.emplace_back("PAUSED  (. = step, P = resume)",
                               ::Color{255, 200, 90, 255});
        }
        if (time->timeScale() != 1.0) {
            std::snprintf(buffer, sizeof(buffer), "TIME SCALE %.2fx", time->timeScale());
            lines.emplace_back(buffer, ::Color{255, 200, 90, 255});
        }
    }

    if (world) {
        char buffer[200];
        std::snprintf(buffer, sizeof(buffer), "enemies %zu/%zu alive | kills %d | shots %zu",
                      world->aliveEnemyCount(), world->enemyCount(),
                      world->enemiesKilled(), world->totalShotsFired());
        lines.emplace_back(buffer, Colors::kTextPrimary);

        std::snprintf(buffer, sizeof(buffer), "AI avg %.3f ms | grid %dx%d",
                      world->averageAITimeMs(),
                      world->navGrid().columns(), world->navGrid().rows());
        lines.emplace_back(buffer, Colors::kTextMuted);
    }

    if (bindings.drawFlags) {
        lines.emplace_back(bindings.drawFlags->summary(), Colors::kAccent);
    }

    // 敌人状态速览：每个敌人一行
    if (world) {
        size_t shown = 0;
        for (const auto& entity : world->entities()) {
            if (entity->type() != Game::EntityType::Enemy || shown >= 6) {
                continue;
            }
            const auto* enemy = static_cast<const Game::Enemy*>(entity.get());

            char buffer[220];
            std::snprintf(buffer, sizeof(buffer),
                          "%-10s %-14s hp %3.0f thr %.2f fire %d",
                          enemy->name().c_str(),
                          Game::toString(enemy->state()),
                          static_cast<double>(enemy->health()),
                          static_cast<double>(enemy->perception().threatLevel()),
                          enemy->shotsFired());
            lines.emplace_back(buffer, enemy->isDead() ? ::Color{120, 120, 130, 255}
                                                       : Colors::kTextMuted);
            ++shown;
        }
    }

    // 计算底板尺寸
    int maxWidth = 0;
    for (const auto& [text, color] : lines) {
        (void)color;
        maxWidth = std::max(maxWidth, Engine::Renderer::measureText(text, 12));
    }

    constexpr float kPadding = 10.0f;
    constexpr float kLineHeight = 16.0f;

    const ::Rectangle panel{
        kPanelMargin,
        startY,
        static_cast<float>(maxWidth) + kPadding * 2.0f,
        static_cast<float>(lines.size()) * kLineHeight + kPadding * 2.0f
    };

    // 【清理】原先这里有一句孤立的 `bindings.drawFlags;` —— 无副作用的
    // 死语句（至多触发一条 "expression has no effect" 警告）。已删除。

    // 注意：这里通过 bindings 拿到 renderer 是不可能的（未传入），
    // 因此原生 HUD 的绘制由调用方（main）在世界绘制后调用本函数。
    // 为了让本函数自洽，我们直接使用 raylib 的绘制 API——
    // 这与「debug 层可以依赖 raylib」的分层约定一致。
    DrawRectangleRec(panel, Fade(Colors::kPanelBg, 0.82f));
    DrawRectangleLinesEx(panel, 1.0f, Fade(Colors::kPanelBorder, 0.9f));

    float y = panel.y + kPadding;
    // 使用注入的中文字体而非 GetFontDefault()：
    // 默认字体只含 ASCII，会让面板上的中文全部变成方块。
    const ::Font hudFont = fallbackFont();
    for (const auto& [text, color] : lines) {
        DrawTextEx(hudFont, text.c_str(),
                   ::Vector2{panel.x + kPadding, y},
                   12.0f, 0.0f, color);
        y += kLineHeight;
    }
}

// ---------------------------------------------------------------------------
//  自定义标签页
// ---------------------------------------------------------------------------
void DebugUI::registerCustomTab(std::string title, std::function<void()> drawFn) {
    customTabs_.emplace_back(std::move(title), std::move(drawFn));
}

} // namespace TacticalAI::Debug
