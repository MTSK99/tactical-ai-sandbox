// ===========================================================================
//  src/engine/Input.h
//
//  输入系统。职责：把 raylib 的“逐帧轮询式”输入，包装成便于游戏逻辑消费的
//  状态快照，并支持「动作映射」而非硬编码键位。
//
//  三条重要语义（初学者最常见的 bug 来源）：
//    - Down    : 本帧是否按住（持续为真）
//    - Pressed : 本帧是否刚按下（仅在按下的那一帧为真，边沿触发）
//    - Released: 本帧是否刚松开（仅在松开的那一帧为真）
//
//  逻辑帧与渲染帧可能不等频。为避免固定步长逻辑在单帧内多次消费
//  Pressed 造成重复触发，本类提供 ConsumePressed() 主动消费语义。
//
//  【为什么 Consume 要跨帧锁存】
//  Pressed 只存活一帧，而 consume* 是在固定步里调用的。当渲染帧率高于
//  逻辑帧率时，有些渲染帧根本没有逻辑步（实测 120FPS 下 600 帧只有 313
//  个逻辑步）。这些帧里产生的按下事件若只存活一帧就会被整帧丢掉 ——
//  表现是"点了没反应 / 有时候射不出来"。
//  因此 consume* 读取的是"尚未被消费的按下事件"（跨帧锁存、有超时上限），
//  而 isKeyPressed / wasActionPressed 这类非消费读取仍是严格帧内语义。
// ===========================================================================
#pragma once

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "raylib.h"

namespace TacticalAI::Engine {

/// 可被绑定的逻辑动作。游戏层只关心语义动作，不关心具体按键，
/// 这样 remap（改键）与手柄支持都不需要改游戏代码。
enum class Action : int {
    MoveUp = 0,
    MoveDown,
    MoveLeft,
    MoveRight,
    Sprint,
    Interact,
    Aim,
    Fire,
    Dodge,
    TogglePerceptionDebug,
    ToggleBehaviorTreeDebug,
    ToggleEQSGridDebug,
    TogglePathDebug,
    ToggleDebugPanel,
    TogglePause,
    StepFrame,
    ToggleSlowMotion,
    SpawnEnemy,
    RerouteAgents,
    Restart,
    Max
};

/// 把 Action 转成可读字符串，用于 UI 显示与日志。
[[nodiscard]] const char* toString(Action action) noexcept;

/// 鼠标按键的轻量封装，避免到处写 MOUSE_BUTTON_LEFT 这类魔数。
enum class MouseButton : int {
    Left = 0,
    Right,
    Middle,
    Max
};

/// 单个鼠标按键的帧状态。
struct MouseButtonState {
    bool  down     = false;
    bool  pressed  = false;
    bool  released = false;
    bool  consumed = false;   ///< 已被逻辑消费，防止同一帧内重复触发
    int   pressCount = 0;     ///< 累计按下次数，便于统计点击频率

    /// 跨帧锁存的按下事件（见 Input::consumeKeyPressed 的说明）。
    bool  pressedLatched = false;
    int   latchFrames    = 0; ///< 已锁存但未被消费的帧数（超限即作废）
};

/// 输入系统。
///
/// 生命周期约定：每渲染帧调用一次 update()，且必须早于任何逻辑读取。
/// @code
///   input.update();
///   if (input.wasActionPressed(Action::Fire)) { ... }
/// @endcode
class Input {
public:
    Input();
    ~Input();

    // 输入系统不持有堆资源，禁止拷贝以避免意外复制状态
    Input(const Input&)            = delete;
    Input& operator=(const Input&) = delete;

    // -- 帧生命周期 ----------------------------------------------------------

    /// 采样本帧输入状态。在 GameLoop 每渲染帧开头调用一次。
    void update();

    /// 清空所有状态（窗口失焦、场景切换时调用，防止“卡键”）。
    void clear();

    // -- 键盘 ----------------------------------------------------------------

    [[nodiscard]] bool isKeyDown(KeyboardKey key) const;
    /// 边沿触发：本帧刚按下时为真。
    [[nodiscard]] bool isKeyPressed(KeyboardKey key) const;
    [[nodiscard]] bool isKeyReleased(KeyboardKey key) const;

    /// 消费式边沿触发：返回是否存在尚未被消费的按下事件，并把它取走。
    /// 同一个事件只会返回一次 true，适合固定步长逻辑循环。
    ///
    /// 【为什么是"尚未被消费"而不是"本帧"】
    /// 本函数每渲染帧调用一次 update() 采样，而游戏逻辑在**固定步**里消费。
    /// 渲染帧率高于逻辑帧率时，有些帧根本没有逻辑步（实测 120FPS 下
    /// 600 帧只有 313 步，近一半的帧没有逻辑步）。若事件只存活一帧，
    /// 落在这些帧里的按下就会被整帧丢掉 —— 表现是"点了没反应 /
    /// 有时候射不出来"。
    /// 因此按下事件会**锁存**到被消费为止（上限 kMaxLatchFrames 帧，
    /// 防止暂停很久之后突然补一次输入）。
    ///
    /// 注意这只解决"事件被帧对齐丢掉"，不改变"被拒绝的请求不排队"：
    /// 冷却期内按下的开火请求仍然会被 Player 拒绝（README 的约定），
    /// 只是它必定会被送到 Player 那里判定一次，而不是随机消失。
    [[nodiscard]] bool consumeKeyPressed(KeyboardKey key);

    // -- 鼠标 ----------------------------------------------------------------

    /// 本帧鼠标位置（屏幕像素坐标）。
    [[nodiscard]] Vector2 mousePosition() const noexcept { return mousePosition_; }
    /// 本帧鼠标位移增量（像素）。相机拖拽、视角旋转用这个。
    [[nodiscard]] Vector2 mouseDelta() const noexcept { return mouseDelta_; }
    /// 滚轮增量，向上为正。
    [[nodiscard]] float mouseWheel() const noexcept { return mouseWheel_; }

    [[nodiscard]] bool isMouseDown(MouseButton button) const;
    [[nodiscard]] bool isMousePressed(MouseButton button) const;
    [[nodiscard]] bool isMouseReleased(MouseButton button) const;
    /// 消费式边沿触发，语义与 consumeKeyPressed 完全相同（含跨帧锁存）。
    [[nodiscard]] bool consumeMousePressed(MouseButton button);

    /// 累计点击次数（自程序启动或上次 clear 起）。
    [[nodiscard]] int mousePressCount(MouseButton button) const;

    // -- 语义动作 ------------------------------------------------------------

    [[nodiscard]] bool isActionDown(Action action) const;
    [[nodiscard]] bool wasActionPressed(Action action) const;
    [[nodiscard]] bool wasActionReleased(Action action) const;
    /// 消费式：见 consumeKeyPressed 说明。
    [[nodiscard]] bool consumeActionPressed(Action action);

    /// 归一化移动轴。同时按住相反方向时自动抵消，斜向不会超速。
    /// @return x 为左右（右正），y 为前后（下正），模长不超过 1。
    [[nodiscard]] Vector2 moveAxis() const;

    /// 鼠标相对屏幕中心的方向（右正、下正，单位向量）。
    /// 战术沙盒里用它把「鼠标指向」直接映射成瞄准方向。
    [[nodiscard]] Vector2 aimDirection() const;

    // -- 改键 ----------------------------------------------------------------

    /// 把一个逻辑动作绑定到按键（会覆盖该动作原有绑定）。
    void bind(Action action, KeyboardKey key);
    /// 追加一个备选按键，实现「一个动作多个键位」。
    void addBinding(Action action, KeyboardKey key);
    /// 查询动作当前的按键绑定列表。
    ///
    /// 返回 const 引用而非值：键位查询在每帧的热路径上（moveAxis 一次就
    /// 查 4 个动作），按值返回意味着每次查询一次堆分配。
    /// 无绑定时返回静态空容器，调用方无需判空。
    [[nodiscard]] const std::vector<KeyboardKey>& bindings(Action action) const;
    /// 恢复默认键位。
    void resetBindings();
    /// 生成当前键位表的可读文本，供调试面板显示。
    [[nodiscard]] std::string bindingTableText() const;

    // -- 状态 ----------------------------------------------------------------

    /// 本帧是否有任意输入活动（用于判断玩家是否离开，触发 idle 行为）。
    [[nodiscard]] bool anyActivityThisFrame() const noexcept { return anyActivity_; }
    /// 距上次输入活动的秒数。
    [[nodiscard]] double idleSeconds() const noexcept { return idleSeconds_; }
    /// 推进 idle 计时，由 GameLoop 按 fixedDelta 调用。
    void tickIdle(double fixedDelta) noexcept;

    /// 本帧任意按键的字符串表示（用于「按 P 继续」这类提示）。空串表示无输入。
    [[nodiscard]] std::string pressedKeyName() const;

    // -- 测试注入接口 --------------------------------------------------------
    //
    // 【为什么需要它】没有窗口时 IsKeyDown / IsMouseButtonDown 恒为 false，
    // 而"按下事件会不会被帧对齐丢掉"这件事只能在输入层验证 —— 它正是
    // "点了没反应 / 有时候射不出来"的根因所在。
    //
    // 语义保证：**不调用这两个接口时，行为与没有它们时完全一致**
    // （内部有一个 testInputOverride_ 开关，只有注入过才切到注入状态）。
    // 因此它们对正式构建是零行为影响的，代价只是一次 bool 判断。

    /// 注入按键的物理按下状态（替代 raylib 的 IsKeyDown）。仅测试/回放使用。
    void setKeyDownForTest(KeyboardKey key, bool down);
    /// 注入鼠标按键的物理按下状态。仅测试/回放使用。
    void setMouseDownForTest(MouseButton button, bool down);

private:
    struct KeyState {
        bool down       = false;
        bool pressed    = false;
        bool released   = false;
        bool consumed   = false;   ///< 边沿事件是否已被逻辑消费
        int  pressCount = 0;

        /// 跨帧锁存的按下事件：直到被消费或超时（见 consumeKeyPressed）。
        bool pressedLatched = false;
        int  latchFrames    = 0;
    };

    using BindingMap = std::unordered_map<int, std::vector<KeyboardKey>>;

    [[nodiscard]] static int toIndex(Action action) noexcept;
    [[nodiscard]] static int toIndex(MouseButton button) noexcept;
    [[nodiscard]] KeyState* keyState(KeyboardKey key);
    [[nodiscard]] const KeyState* keyState(KeyboardKey key) const;

    /// 上一帧的按键按下位图，用于自行计算边沿。
    /// 不直接依赖 raylib 的 IsKeyPressed，是为了统一「消费」语义。
    std::array<KeyState, 512> keyStates_{};
    std::array<MouseButtonState, static_cast<size_t>(MouseButton::Max)> mouseStates_{};

    Vector2 mousePosition_{};
    Vector2 mouseDelta_{};
    Vector2 lastMousePosition_{};
    float   mouseWheel_ = 0.0f;

    BindingMap bindings_;

    bool   anyActivity_  = false;
    double idleSeconds_  = 0.0;

    /// 测试注入状态。只有调用过 setKeyDownForTest / setMouseDownForTest
    /// 之后才置位 —— 正式运行路径永远走 raylib 的真实输入。
    bool testInputOverride_ = false;
    std::array<bool, 512> testKeyDown_{};
    std::array<bool, static_cast<size_t>(MouseButton::Max)> testMouseDown_{};
};

} // namespace TacticalAI::Engine
