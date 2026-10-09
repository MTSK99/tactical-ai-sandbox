// ===========================================================================
//  src/debug/DebugUI.h
//
//  运行时调试面板。
//
//  两种呈现方式，自动降级：
//    1. 有 imgui 后端时 —— 完整可交互面板（滑块调参、树形展开、表格排序）
//    2. 无 imgui 时      —— 用 raylib 原生绘制的只读文本 HUD
//
//  降级路径是刻意设计的：调参能力重要，但「打不开面板」比「面板简陋」
//  更影响开发效率。同时这让项目在没有 imgui 后端的环境（如某些 CI）也能跑。
//
//  面板的价值在于「不用重启进程就能改数值」：
//  视野角度、反应时间、EQS 权重这类参数，改一次编译一次是灾难性的低效。
// ===========================================================================
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "debug/AIDebugDraw.h"
#include "engine/GameLoop.h"
#include "engine/Input.h"
#include "engine/Renderer.h"
#include "game/World.h"

namespace TacticalAI::Debug {

/// 面板上可调的动作（由外部实现，面板只负责触发）。
struct DebugUIActions {
    std::function<void()> togglePause;
    std::function<void()> stepFrame;
    std::function<void()> resetWorld;
    std::function<void()> spawnEnemy;
    std::function<void()> clearEnemies;
    std::function<void()> killAllEnemies;
    std::function<void()> toggleSlowMotion;
    std::function<void()> requestRepath;    ///< 强制所有敌人重新寻路
    std::function<void()> dumpWorldState;   ///< 打印完整场景状态到控制台
    std::function<void()> damagePlayer;     ///< 对玩家造成伤害（测试撤退行为）
};

/// 面板关注的运行时参数（引用外部变量，实现双向绑定）。
struct DebugUIBindings {
    Engine::Time*     time              = nullptr;
    Game::World*      world             = nullptr;
    DebugDrawFlags*   drawFlags         = nullptr;
    Engine::Input*    input             = nullptr;
    Game::EnemyConfig* enemyConfig       = nullptr;   ///< 应用到所有敌人的共享配置
    Game::WorldConfig* worldConfig       = nullptr;
    bool*             brainFrozen        = nullptr;   ///< 冻结 AI 决策（只观察不更新）

    /// 屏幕顶部已被游戏自身状态条占住的像素高度，由 main 填入。
    ///
    /// 为什么需要它：main 会在左上角画一条状态条（HP / 子弹 / 命中 / 击杀），
    /// 调试 HUD 早年也从 kPanelMargin 开始画，两者完全重叠。两套文字压在一起
    /// 的观感就是"乱码"——同一块像素上既读不出中文也读不出英文，比不显示更糟。
    ///
    /// 这个值必须由知道状态条多高的那一方（main）提供，而不是在调试层里
    /// 另写一个魔数：两处各写各的，改了一处就会悄悄错位。
    float topReserved = 0.0f;
};

/// 调试 UI 面板。
class DebugUI {
public:
    DebugUI() = default;
    ~DebugUI();

    DebugUI(const DebugUI&)            = delete;
    DebugUI& operator=(const DebugUI&) = delete;

    /// 初始化后端。若 imgui 的 raylib 后端可用则启用完整面板，
    /// 否则自动降级为原生 HUD。必须在窗口创建之后调用。
    /// @return 是否启用了 imgui
    bool initialize();

    /// 关闭后端（由 main 在窗口销毁前调用）。
    void shutdown();

    /// 每渲染帧开始新的 UI 帧。
    void beginFrame();

    /// 绘制面板内容。
    void draw(const DebugUIBindings& bindings, const DebugUIActions& actions,
              const Engine::LoopStats& loopStats);

    /// 结束 UI 帧并提交绘制。
    void endFrame();

    [[nodiscard]] bool isVisible() const noexcept { return visible_; }
    void setVisible(bool visible) noexcept { visible_ = visible; }
    void toggleVisible() noexcept { visible_ = !visible_; }

    /// 是否正在使用 imgui 后端（无后端时为原生 HUD）。
    [[nodiscard]] bool usingImGui() const noexcept { return imguiActive_; }

    /// 面板是否正被鼠标操作（用于避免相机拖拽与面板冲突）。
    [[nodiscard]] bool wantsMouseCapture() const noexcept { return wantsMouseCapture_; }

    /// 受冻结开关影响的 AI 更新计数。
    [[nodiscard]] uint64_t frozenFrameCount() const noexcept { return frozenFrames_; }

    /// 在面板上注册一个自定义标签页（供后续扩展，如「EQS 权重实验」）。
    using CustomTab = std::pair<std::string, std::function<void()>>;
    void registerCustomTab(std::string title, std::function<void()> drawFn);

    /// 设置原生 HUD 绘制文字所用的字体。
    ///
    /// 【为什么需要这个方法】
    /// 原生 HUD（无 imgui 时的降级路径）直接调 raylib 的 DrawTextEx 绘制，
    /// 此前传的是 GetFontDefault()——而默认字体只含 ASCII，导致面板上的
    /// 中文全部显示为方块。
    /// 这里让 main 在 Renderer 建好之后把「承载了中文字形的字体」注入进来，
    /// 原生 HUD 就能正确显示中文了。
    ///
    /// @param font 字体；传入的 Font 必须是长期有效的（由 Renderer 持有）。
    static void setFallbackFont(::Font font) noexcept;

    /// 取原生 HUD 当前应使用的字体。
    /// 未注入过（或无中文字形）时返回 raylib 默认字体——此时中文会显示为方块，
    /// 但至少不会崩溃。
    [[nodiscard]] static ::Font fallbackFont() noexcept;

private:
    // -- 原生 HUD 用字体 -----------------------------------------------------

    /// 原生 HUD 的绘制字体。未设置时回退到 raylib 默认字体（不含中文）。
    static ::Font s_fallbackFont;
    static bool   s_hasFallbackFont;

    // -- imgui 路径 ----------------------------------------------------------

    void drawImGuiPanels(const DebugUIBindings& bindings, const DebugUIActions& actions,
                         const Engine::LoopStats& loopStats);

    /// 「概览」标签页：帧率、时间、循环健康度。
    void drawOverviewTab(const DebugUIBindings& bindings, const Engine::LoopStats& loopStats);

    /// 「世界」标签页：实体列表、生成/清除操作。
    void drawWorldTab(const DebugUIBindings& bindings, const DebugUIActions& actions);

    /// 「AI 调参」标签页：感知、行为树、EQS、寻路的实时参数。
    void drawAITuningTab(const DebugUIBindings& bindings, const DebugUIActions& actions);

    /// 「实体详情」标签页：逐个实体展开黑板与行为树。
    void drawEntityInspectTab(const DebugUIBindings& bindings);

    /// 「可视化」标签页：调试绘制开关。
    void drawVisualizationTab(const DebugUIBindings& bindings);

    /// 绘制单个敌人的详细检视（黑板 + 行为树 + 状态历史）。
    void drawEnemyInspector(const Game::Enemy& enemy, size_t index);

    // -- 原生 HUD 降级路径 ---------------------------------------------------

    void drawFallbackHud(const DebugUIBindings& bindings, const Engine::LoopStats& loopStats,
                         float startY) const;
    void drawFallbackLine(const Engine::Renderer& renderer, const std::string& text,
                          float& y, float x, ::Color color, int fontSize) const;

    bool visible_      = true;
    bool imguiActive_  = false;
    bool initialized_  = false;
    bool wantsMouseCapture_ = false;

    int  activeTab_ = 0;
    uint64_t frozenFrames_ = 0;

    /// 实体检视的展开状态（按实体 ID 记录，避免重排后错位）
    std::vector<uint32_t> expandedEntityIds_;

    std::vector<CustomTab> customTabs_;

    /// 原生 HUD 的滚动偏移（内容超出屏幕时可滚）
    float hudScroll_ = 0.0f;
};

} // namespace TacticalAI::Debug
