// ===========================================================================
//  src/engine/Input.cpp
// ===========================================================================
#include "engine/Input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace TacticalAI::Engine {
namespace {

/// 引擎内约定的按键状态数组容量。raylib 的 KeyboardKey 最大为 348，
/// 512 已留出余量，且足够小到可以按值遍历。
constexpr int kKeyStateCapacity = 512;

/// 按下事件的锁存上限（帧）。见 Input::update 里的详细说明。
/// 取 8 帧：240FPS 下一个逻辑步最多跨 4 帧，留一倍余量；
/// 同时又足够短，不会让"很久以前的按键"突然补触发一次。
constexpr int kMaxLatchFrames = 8;

/// 安全地钳制按键索引，越界时返回 -1 表示「无效按键」。
int keyToIndex(KeyboardKey key) noexcept {
    const int index = static_cast<int>(key);
    if (index < 0 || index >= kKeyStateCapacity) {
        return -1;
    }
    return index;
}

/// 把按键枚举转成可读名称。
///
/// raylib 5.5 移除了旧版的 GetKeyName 辅助函数，这里改为自带一张
/// 常用键位表：字母键、数字键、功能键、方向键与小键盘均覆盖。
/// 未收录的键返回空串，调用方据此回退到数字编号显示。
const char* keyName(KeyboardKey key) noexcept {
    const int code = static_cast<int>(key);

    // 字母键 A-Z：raylib 中连续排列，可直接偏移映射
    if (code >= KEY_A && code <= KEY_Z) {
        static const char* const kLetters[] = {
            "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
            "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
        return kLetters[code - KEY_A];
    }

    // 数字键 0-9：raylib 中同样是连续排列
    if (code >= KEY_ZERO && code <= KEY_NINE) {
        static const char* const kDigits[] = {"0", "1", "2", "3", "4",
                                              "5", "6", "7", "8", "9"};
        return kDigits[code - KEY_ZERO];
    }

    // 功能键 F1-F12 亦为连续排列
    if (code >= KEY_F1 && code <= KEY_F12) {
        static const char* const kFunctionKeys[] = {
            "F1", "F2", "F3", "F4", "F5", "F6",
            "F7", "F8", "F9", "F10", "F11", "F12"};
        return kFunctionKeys[code - KEY_F1];
    }

    // 其余零散键位逐个匹配即可，数量有限。
    switch (key) {
        case KEY_SPACE:         return "Space";
        case KEY_ESCAPE:        return "Esc";
        case KEY_ENTER:         return "Enter";
        case KEY_TAB:           return "Tab";
        case KEY_BACKSPACE:     return "Backspace";
        case KEY_LEFT_SHIFT:    return "LShift";
        case KEY_RIGHT_SHIFT:   return "RShift";
        case KEY_LEFT_CONTROL:  return "LCtrl";
        case KEY_RIGHT_CONTROL: return "RCtrl";
        case KEY_LEFT_ALT:      return "LAlt";
        case KEY_RIGHT_ALT:     return "RAlt";
        case KEY_UP:            return "Up";
        case KEY_DOWN:          return "Down";
        case KEY_LEFT:          return "Left";
        case KEY_RIGHT:         return "Right";
        case KEY_COMMA:         return ",";
        case KEY_PERIOD:        return ".";
        case KEY_SLASH:         return "/";
        case KEY_SEMICOLON:     return ";";
        case KEY_APOSTROPHE:    return "'";
        case KEY_LEFT_BRACKET:  return "[";
        case KEY_RIGHT_BRACKET: return "]";
        case KEY_MINUS:         return "-";
        case KEY_EQUAL:         return "=";
        default:                return "";
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Action -> 字符串
// ---------------------------------------------------------------------------
const char* toString(Action action) noexcept {
    switch (action) {
        case Action::MoveUp:                 return "MoveUp";
        case Action::MoveDown:               return "MoveDown";
        case Action::MoveLeft:               return "MoveLeft";
        case Action::MoveRight:              return "MoveRight";
        case Action::Sprint:                 return "Sprint";
        case Action::Interact:               return "Interact";
        case Action::Aim:                    return "Aim";
        case Action::Dodge:                  return "Dodge";
        case Action::Fire:                   return "Fire";
        case Action::TogglePerceptionDebug:  return "TogglePerceptionDebug";
        case Action::ToggleBehaviorTreeDebug:return "ToggleBehaviorTreeDebug";
        case Action::ToggleEQSGridDebug:     return "ToggleEQSGridDebug";
        case Action::TogglePathDebug:        return "TogglePathDebug";
        case Action::ToggleDebugPanel:       return "ToggleDebugPanel";
        case Action::TogglePause:            return "TogglePause";
        case Action::StepFrame:              return "StepFrame";
        case Action::ToggleSlowMotion:       return "ToggleSlowMotion";
        case Action::SpawnEnemy:             return "SpawnEnemy";
        case Action::RerouteAgents:          return "RerouteAgents";
        case Action::Restart:               return "Restart";
        case Action::Max:                    break;
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------
Input::Input() {
    resetBindings();
}

Input::~Input() = default;

// ---------------------------------------------------------------------------
// 默认键位表
// ---------------------------------------------------------------------------
void Input::resetBindings() {
    bindings_.clear();

    bind(Action::MoveUp,    KEY_W);
    addBinding(Action::MoveUp, KEY_UP);
    bind(Action::MoveDown,  KEY_S);
    addBinding(Action::MoveDown, KEY_DOWN);
    bind(Action::MoveLeft,  KEY_A);
    addBinding(Action::MoveLeft, KEY_LEFT);
    bind(Action::MoveRight, KEY_D);
    addBinding(Action::MoveRight, KEY_RIGHT);

    bind(Action::Sprint,   KEY_LEFT_SHIFT);
    bind(Action::Interact, KEY_E);

    // 空格给了翻滚（动作游戏里最顺手的重键位）。
    // 原本占用空格的"瞄准"挪到左 Ctrl；同时 main 里也接受鼠标右键——
    // 这是一个鼠标瞄准的游戏，"按住右键放慢脚步"比按 Ctrl 自然得多。
    bind(Action::Dodge,    KEY_SPACE);
    bind(Action::Aim,      KEY_LEFT_CONTROL);
    bind(Action::Fire,     KEY_F);

    // 调试开关集中放在数字键与功能键区，互不冲突
    bind(Action::TogglePerceptionDebug,   KEY_ONE);
    bind(Action::ToggleBehaviorTreeDebug, KEY_TWO);
    bind(Action::ToggleEQSGridDebug,      KEY_THREE);
    bind(Action::TogglePathDebug,         KEY_FOUR);
    bind(Action::ToggleDebugPanel,        KEY_F1);
    bind(Action::TogglePause,             KEY_P);
    bind(Action::StepFrame,               KEY_PERIOD);
    bind(Action::ToggleSlowMotion,        KEY_TAB);
    bind(Action::SpawnEnemy,              KEY_N);
    bind(Action::RerouteAgents,           KEY_R);

    // 重开绑回车而不是 R —— R 已经给了「全部重新寻路」，
    // 而回车是「确认 / 继续」这类语义的自然键位。
    bind(Action::Restart,                 KEY_ENTER);
    addBinding(Action::Restart,           KEY_KP_ENTER);
}

void Input::bind(Action action, KeyboardKey key) {
    bindings_[toIndex(action)] = {key};
}

void Input::addBinding(Action action, KeyboardKey key) {
    auto& list = bindings_[toIndex(action)];
    if (std::find(list.begin(), list.end(), key) == list.end()) {
        list.push_back(key);
    }
}

const std::vector<KeyboardKey>& Input::bindings(Action action) const {
    // 【修复】原先按值返回，每次查询都构造一个 vector（堆分配）：
    // moveAxis() 一次就要调 4 次 isActionDown，游戏层每逻辑步再轮询若干
    // 动作 —— 每帧十几次无谓分配。改为返回 const 引用（静态空容器兜底），
    // 遍历方不会因此产生拷贝。
    static const std::vector<KeyboardKey> kEmpty;
    const auto it = bindings_.find(toIndex(action));
    return (it != bindings_.end()) ? it->second : kEmpty;
}

std::string Input::bindingTableText() const {
    std::string text;
    for (int i = 0; i < toIndex(Action::Max); ++i) {
        const auto action = static_cast<Action>(i);
        const auto& keys = bindings(action);
        if (keys.empty()) {
            continue;
        }
        text += toString(action);
        text += " = ";
        for (size_t k = 0; k < keys.size(); ++k) {
            if (k > 0) {
                text += " / ";
            }
            // 【修复】原先打印的是键码数字（"Space = 32"），而本文件已经有
            // keyName()。面板是给人看的，优先给名字；未收录的键再回退数字。
            const char* name = keyName(keys[k]);
            if (name != nullptr && *name != '\0') {
                text += name;
            } else {
                text += std::to_string(static_cast<int>(keys[k]));
            }
        }
        text += '\n';
    }
    return text;
}

// ---------------------------------------------------------------------------
// 帧更新
// ---------------------------------------------------------------------------
void Input::update() {
    anyActivity_ = false;

#if !defined(TACTICALAI_HEADLESS_TEST)
    // ---- 焦点生命周期 ----
    //
    // 【为什么必须在 Input 内部处理】
    // 失焦（alt-tab / 点到别的窗口）期间按键事件不再送达，但"按下"状态会
    // 冻结在失焦前的那一刻：用户切回来时按住的键会被认为仍然按住（卡键），
    // 而且 mousePosition_ 也停在旧位置，恢复焦点那一帧的位移会是一次跨屏
    // 巨量跳变。头文件里原本就承诺了 clear() 用于"窗口失焦时防卡键"，
    // 但那个方法在全项目里从来没有被调用过——承诺没有落地。
    // 现在把它接进更新循环：失焦就清空全部状态并重新采样鼠标位置。
    //
    // 【为什么还要 IsWindowReady()】
    // 这段原先只由 TACTICALAI_HEADLESS_TEST 宏保护。但那个宏只加在**测试
    // 目标**上，引擎库本身是按正式配置编译的 —— 于是单元测试里
    // IsWindowFocused() 恒为 false，每一帧都会走进这里把全部输入状态清零，
    // 输入层因此完全无法被测试（注入的按键会被立刻抹掉）。
    // 换成运行时判断"窗口是否真的存在"：有窗口时行为一字不变，
    // 没窗口时（测试/无头）不再无谓地清空状态。
    if (IsWindowReady() && !IsWindowFocused()) {
        clear();
        anyActivity_ = false;
        return;
    }
#endif

    // ---- 键盘：自行计算边沿，以便支持消费语义 ----
    for (int i = 0; i < kKeyStateCapacity; ++i) {
        KeyState& state = keyStates_[static_cast<size_t>(i)];

        const bool nowDown =
            testInputOverride_ ? testKeyDown_[static_cast<size_t>(i)]
                               : IsKeyDown(static_cast<KeyboardKey>(i));

        state.pressed  = nowDown && !state.down;
        state.released  = !nowDown && state.down;
        state.down      = nowDown;

        // 【修复】按下事件必须锁存到被逻辑消费为止。
        //
        // 本函数每**渲染帧**调用一次，而游戏逻辑在**固定步**里消费。渲染帧率
        // 高于逻辑帧率时有些帧没有逻辑步（实测截图：120FPS 下 600 帧只有
        // 313 个逻辑步，即近一半的帧没有逻辑步）。原实现在这里每帧重算
        // state.pressed，于是落在"没有逻辑步"那一帧的按下事件在下一帧
        // 就消失了 —— 整帧丢掉一次点击。
        //
        // 现象正是"射击很怪、有时候射不出来"：点击能否命中完全取决于它
        // 落在哪一帧（帧率越高丢得越多）。
        //
        // 锁存上限 kMaxLatchFrames：纯属兜底，避免长时间没有逻辑步
        // （例如暂停很久）之后突然"补"一次输入。一个逻辑步在 240FPS 下
        // 最多跨 4 帧，8 帧足够覆盖，又短到不会让玩家感觉"按了没反应、
        // 过一秒才开枪"。
        if (state.pressed) {
            state.pressedLatched = true;
            state.latchFrames    = 0;
        } else if (state.pressedLatched && ++state.latchFrames > kMaxLatchFrames) {
            state.pressedLatched = false;
            state.latchFrames    = 0;
        }

        // 上一帧未消费的**帧内**边沿标记（consumed）在下一帧失效。
        // 注意：跨帧锁存(是否会被消费)由上面的 pressedLatched 负责，
        // 两者是不同的问题 —— 这里只跟踪"本帧边沿是否已读走"。
        if (state.pressed || state.released) {
            state.consumed = false;
        }

        if (state.pressed) {
            ++state.pressCount;
            anyActivity_ = true;
        }
    }

    // ---- 鼠标按键 ----
    for (int b = 0; b < static_cast<int>(MouseButton::Max); ++b) {
        MouseButtonState& state = mouseStates_[static_cast<size_t>(b)];
        const bool nowDown =
            testInputOverride_ ? testMouseDown_[static_cast<size_t>(b)]
                               : IsMouseButtonDown(b);

        state.pressed   = nowDown && !state.down;
        state.released  = !nowDown && state.down;
        state.down      = nowDown;

        // 与键盘同理：鼠标点击同样可能在"没有逻辑步的帧"里发生，
        // 而开火默认绑在鼠标左键上。
        if (state.pressed) {
            state.pressedLatched = true;
            state.latchFrames    = 0;
        } else if (state.pressedLatched && ++state.latchFrames > kMaxLatchFrames) {
            state.pressedLatched = false;
            state.latchFrames    = 0;
        }

        if (state.pressed || state.released) {
            state.consumed = false;
        }
        if (state.pressed) {
            ++state.pressCount;
            anyActivity_ = true;
        }
    }

    // ---- 鼠标位置与位移 ----
    mousePosition_ = GetMousePosition();

    // raylib 自带 GetMouseDelta，但其值受 MouseScale 影响；
    // 这里自行求差，保证与 mousePosition 严格一致。
    mouseDelta_ = {mousePosition_.x - lastMousePosition_.x,
                   mousePosition_.y - lastMousePosition_.y};
    lastMousePosition_ = mousePosition_;

    // 移动鼠标也算"有输入"：否则玩家一边走位一边瞄准时（没有新的按下
    // 事件），idle 计时会一直增长，被上层误判成"玩家已经离开"。
    if (std::fabs(mouseDelta_.x) > 0.5f || std::fabs(mouseDelta_.y) > 0.5f) {
        anyActivity_ = true;
    }

    mouseWheel_ = GetMouseWheelMove();
    if (std::fabs(mouseWheel_) > 0.001f) {
        anyActivity_ = true;
    }
}

void Input::clear() {
    for (auto& state : keyStates_) {
        state = KeyState{};
    }
    for (auto& state : mouseStates_) {
        state = MouseButtonState{};
    }
    mouseDelta_ = {0.0f, 0.0f};
    mouseWheel_ = 0.0f;
    anyActivity_ = false;

    // 【修复】原先这里写的是 `lastMousePosition_ = mousePosition_`，
    // 那是**空操作**：update() 末尾本来就让两者相等。真正要解决的是
    // "失焦期间鼠标动过、而 mousePosition_ 还停在旧值"，所以必须
    // 重新采样当前位置；否则恢复焦点后的第一帧仍会产生一次跨屏巨量位移。
#if !defined(TACTICALAI_HEADLESS_TEST)
    mousePosition_ = GetMousePosition();
#endif
    lastMousePosition_ = mousePosition_;
}

// ---------------------------------------------------------------------------
// 测试注入
// ---------------------------------------------------------------------------
void Input::setKeyDownForTest(KeyboardKey key, bool down) {
    const int index = keyToIndex(key);
    if (index < 0) {
        return;
    }
    testInputOverride_ = true;
    testKeyDown_[static_cast<size_t>(index)] = down;
}

void Input::setMouseDownForTest(MouseButton button, bool down) {
    const int index = toIndex(button);
    if (index < 0 || index >= static_cast<int>(MouseButton::Max)) {
        return;
    }
    testInputOverride_ = true;
    testMouseDown_[static_cast<size_t>(index)] = down;
}

void Input::tickIdle(double fixedDelta) noexcept {
    if (anyActivity_) {
        idleSeconds_ = 0.0;
    } else {
        idleSeconds_ += fixedDelta;
    }
}

// ---------------------------------------------------------------------------
// 键盘查询
// ---------------------------------------------------------------------------
Input::KeyState* Input::keyState(KeyboardKey key) {
    const int index = keyToIndex(key);
    return (index < 0) ? nullptr : &keyStates_[static_cast<size_t>(index)];
}

const Input::KeyState* Input::keyState(KeyboardKey key) const {
    const int index = keyToIndex(key);
    return (index < 0) ? nullptr : &keyStates_[static_cast<size_t>(index)];
}

bool Input::isKeyDown(KeyboardKey key) const {
    const KeyState* state = keyState(key);
    return state && state->down;
}

bool Input::isKeyPressed(KeyboardKey key) const {
    const KeyState* state = keyState(key);
    return state && state->pressed;
}

bool Input::isKeyReleased(KeyboardKey key) const {
    const KeyState* state = keyState(key);
    return state && state->released;
}

bool Input::consumeKeyPressed(KeyboardKey key) {
    KeyState* state = keyState(key);
    if (!state || !state->pressedLatched) {
        return false;
    }
    // 取走锁存事件：同一个事件只会被返回一次，无论调用方是渲染帧还是逻辑步。
    state->pressedLatched = false;
    state->latchFrames    = 0;
    state->consumed       = true;
    return true;
}

// ---------------------------------------------------------------------------
// 鼠标查询
// ---------------------------------------------------------------------------
bool Input::isMouseDown(MouseButton button) const {
    return mouseStates_[static_cast<size_t>(toIndex(button))].down;
}

bool Input::isMousePressed(MouseButton button) const {
    return mouseStates_[static_cast<size_t>(toIndex(button))].pressed;
}

bool Input::isMouseReleased(MouseButton button) const {
    return mouseStates_[static_cast<size_t>(toIndex(button))].released;
}

bool Input::consumeMousePressed(MouseButton button) {
    const int index = toIndex(button);
    if (index < 0 || index >= static_cast<int>(MouseButton::Max)) {
        return false;
    }
    MouseButtonState& state = mouseStates_[static_cast<size_t>(index)];
    // 与 consumeKeyPressed 同一套语义：取走跨帧锁存的按下事件。
    if (!state.pressedLatched) {
        return false;
    }
    state.pressedLatched = false;
    state.latchFrames    = 0;
    state.consumed       = true;
    return true;
}

int Input::mousePressCount(MouseButton button) const {
    return mouseStates_[static_cast<size_t>(toIndex(button))].pressCount;
}

// ---------------------------------------------------------------------------
// 语义动作
// ---------------------------------------------------------------------------
bool Input::isActionDown(Action action) const {
    for (KeyboardKey key : bindings(action)) {
        if (isKeyDown(key)) {
            return true;
        }
    }
    return false;
}

bool Input::wasActionPressed(Action action) const {
    for (KeyboardKey key : bindings(action)) {
        if (isKeyPressed(key)) {
            return true;
        }
    }
    return false;
}

bool Input::wasActionReleased(Action action) const {
    for (KeyboardKey key : bindings(action)) {
        if (isKeyReleased(key)) {
            return true;
        }
    }
    return false;
}

bool Input::consumeActionPressed(Action action) {
    for (KeyboardKey key : bindings(action)) {
        if (consumeKeyPressed(key)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 派生输入
// ---------------------------------------------------------------------------
Vector2 Input::moveAxis() const {
    float x = 0.0f;
    float y = 0.0f;

    if (isActionDown(Action::MoveRight)) { x += 1.0f; }
    if (isActionDown(Action::MoveLeft))  { x -= 1.0f; }
    if (isActionDown(Action::MoveDown))  { y += 1.0f; }
    if (isActionDown(Action::MoveUp))    { y -= 1.0f; }

    // 归一化：斜向移动不应比正向更快
    const float length = std::sqrt(x * x + y * y);
    if (length > 1.0f) {
        x /= length;
        y /= length;
    }
    return {x, y};
}

Vector2 Input::aimDirection() const {
    // 屏幕坐标系：y 向下为正，与世界坐标一致，因此无需翻转
    const float screenWidth  = static_cast<float>(GetScreenWidth());
    const float screenHeight = static_cast<float>(GetScreenHeight());

    const float dx = mousePosition_.x - screenWidth  * 0.5f;
    const float dy = mousePosition_.y - screenHeight * 0.5f;

    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-4f) {
        // 鼠标恰好在中心：退化为「朝右」，避免除零产生 NaN
        return {1.0f, 0.0f};
    }
    return {dx / length, dy / length};
}

std::string Input::pressedKeyName() const {
    // 遍历本帧所有按下事件，返回第一个可读名称。
    // raylib 的 GetKeyPressed 会消费事件队列，故此处不用它。
    for (int i = 0; i < kKeyStateCapacity; ++i) {
        const KeyState& state = keyStates_[static_cast<size_t>(i)];
        if (!state.pressed) {
            continue;
        }
        const auto key = static_cast<KeyboardKey>(i);
        const char* name = keyName(key);
        if (name && name[0] != '\0') {
            return std::string(name);
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// 索引转换
// ---------------------------------------------------------------------------
int Input::toIndex(Action action) noexcept {
    const int index = static_cast<int>(action);
    // 上下界都要钳：原先只挡了负数，传入非法值（例如 static_cast<Action>(999)）
    // 会在 bindings_ 里留下一个永远不会被清理的"幽灵键位"。
    // 与下面 toIndex(MouseButton) 的做法保持一致。
    const int max = static_cast<int>(Action::Max);
    return (index < 0 || index >= max) ? 0 : index;
}

int Input::toIndex(MouseButton button) noexcept {
    const int index = static_cast<int>(button);
    // MouseButton 只有 3 个合法值，越界时钳到 0，避免数组越界写
    return (index < 0 || index >= static_cast<int>(MouseButton::Max)) ? 0 : index;
}

} // namespace TacticalAI::Engine
