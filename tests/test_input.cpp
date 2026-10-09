// ===========================================================================
//  tests/test_input.cpp
//
//  输入边沿事件的「跨帧存活」回归测试。
//
//  【这些测试在防什么】
//  输入按渲染帧采样（Input::update 每渲染帧一次），而游戏逻辑在固定步里
//  消费（SandboxLayer::onFixedUpdate / GameLoop::stepFixed）。当渲染帧率
//  高于逻辑帧率时，有些渲染帧根本没有逻辑步 —— 实测截图里 120FPS 下
//  600 帧只有 313 个逻辑步，也就是**近一半的帧没有逻辑步**。
//
//  若"按下"事件只存活一帧，落在这些帧里的按键/点击就会被整帧丢掉。
//  玩家看到的正是"射击很怪、有时候射不出来"：这一枪出不出现完全取决于
//  点击落在哪一帧，而且帧率越高丢得越多 —— 在 60FPS 的开发者机器上
//  几乎看不出来，换台高刷显示器就变成"手感莫名其妙"。
// ===========================================================================
#include <gtest/gtest.h>

#include <memory>

#include "engine/Input.h"
#include "game/World.h"

using namespace TacticalAI::Engine;

namespace {

/// 取开火键：从绑定表里读，而不是硬编码 KEY_F——改键之后测试依然有效。
KeyboardKey fireKey() {
    Input input;
    const std::vector<KeyboardKey>& keys = input.bindings(Action::Fire);
    return keys.empty() ? KEY_F : keys.front();
}

} // namespace

// ---------------------------------------------------------------------------
//  核心：没有逻辑步的帧不该吞掉按下事件
// ---------------------------------------------------------------------------

TEST(InputTest, PressSurvivesFramesWithoutLogicStep) {
    Input input;
    const KeyboardKey key = fireKey();

    input.setKeyDownForTest(key, true);
    input.update();                 // 第 1 帧：产生按下边沿
    input.update();                 // 第 2 帧：这一帧没有逻辑步，没人来消费
    input.update();                 // 第 3 帧：仍然没有逻辑步
    input.setKeyDownForTest(key, false);
    input.update();                 // 键已抬起

    EXPECT_TRUE(input.consumeActionPressed(Action::Fire))
        << "按下事件在没有逻辑步的帧里被丢掉了 —— 玩家感觉是「点了没反应」";
}

TEST(InputTest, MouseClickSurvivesFramesWithoutLogicStep) {
    // 鼠标点击同样如此：调试面板的交互按钮全部走 consumeMousePressed。
    Input input;

    // 注意：raylib 也在全局命名空间里定义了同名枚举，这里必须写全限定名。
    constexpr auto kLeft = TacticalAI::Engine::MouseButton::Left;

    input.setMouseDownForTest(kLeft, true);
    input.update();
    input.update();                 // 没有逻辑步
    input.setMouseDownForTest(kLeft, false);
    input.update();

    EXPECT_TRUE(input.consumeMousePressed(kLeft))
        << "鼠标点击在没有逻辑步的帧里被丢掉了";
}

// ---------------------------------------------------------------------------
//  反向约束：锁存不能变成"连发"，也不能无限期留着
// ---------------------------------------------------------------------------

TEST(InputTest, LatchedPressIsConsumedOnlyOnce) {
    Input input;
    const KeyboardKey key = fireKey();

    input.setKeyDownForTest(key, true);
    input.update();
    ASSERT_TRUE(input.consumeActionPressed(Action::Fire));

    input.update();                 // 键仍然按住，但没有新的边沿
    EXPECT_FALSE(input.consumeActionPressed(Action::Fire))
        << "同一个按下事件被消费了两次（按住不放会变成连发）";
}

TEST(InputTest, HoldingAKeyDoesNotRepeatPresses) {
    // 按住与连续点按必须是两件事：本作是半自动（按一次打一发）。
    Input input;
    const KeyboardKey key = fireKey();

    input.setKeyDownForTest(key, true);
    input.update();

    int consumed = 0;
    for (int i = 0; i < 30; ++i) {
        input.update();             // 键一直按住，无新边沿
        if (input.consumeActionPressed(Action::Fire)) {
            ++consumed;
        }
    }

    EXPECT_EQ(consumed, 1) << "按住 F 不放被识别成连续按下（半自动变成了自动连发）";
}

TEST(InputTest, LatchedPressExpiresWhenNobodyConsumesIt) {
    // 锁存必须有上限：否则暂停很久之后恢复的第一帧会"补"一次输入，
    // 表现为"恢复游戏的瞬间莫名开了一枪 / 滚了一下"。
    Input input;
    const KeyboardKey key = fireKey();

    input.setKeyDownForTest(key, true);
    input.update();
    input.setKeyDownForTest(key, false);

    for (int i = 0; i < 20; ++i) {
        input.update();             // 长时间没有逻辑步（例如暂停）
    }

    EXPECT_FALSE(input.consumeActionPressed(Action::Fire))
        << "很久以前的按下事件被补触发了一次";
}

// ---------------------------------------------------------------------------
//  边界：非消费式读取仍是严格帧内语义
// ---------------------------------------------------------------------------

TEST(InputTest, NonConsumingReadersStayFrameScoped) {
    // isKeyPressed / wasActionPressed 不消费事件，必须保持"仅按下那一帧为真"，
    // 否则 DebugUI 之类按帧读取的代码会一直看到 true。
    Input input;
    const KeyboardKey key = fireKey();

    input.setKeyDownForTest(key, true);
    input.update();
    EXPECT_TRUE(input.isKeyPressed(key));

    input.update();
    EXPECT_FALSE(input.isKeyPressed(key))
        << "isKeyPressed 变成了跨帧为真（非消费式读取的语义被污染）";
}

// ---------------------------------------------------------------------------
//  端到端：渲染帧率高于逻辑帧率时，每一次点击都必须变成一发子弹
// ---------------------------------------------------------------------------

TEST(InputTest, HighFrameRateClicksAllBecomeShots) {
    // 完整复现玩家实测到的那个循环：
    //   · 渲染 120FPS（每帧 8.33ms）
    //   · 逻辑 60Hz（隔一帧跑一次固定步）—— 于是有近一半的渲染帧没有逻辑步
    //   · 玩家每 0.12 秒点一次开火（约 8.3 次/秒，人手快速点击）
    //
    // 期望：每一次点击都变成一发子弹。两条实现缺陷都会让这条测试失败：
    //   1. 输入边沿只存活一帧 → 落在"没有逻辑步"那一帧的点击被整帧丢掉；
    //   2. 玩家开火冷却高于人手点击间隔 → 点击被冷却静默拒绝。
    //      （冷却 0.15s 时 0.12s 的点击会有一半被吞；改成 0.10s 之后不再发生）
    TacticalAI::Game::WorldConfig config;
    config.generateDemoObstacles = false;
    config.initialEnemyCount     = 0;
    TacticalAI::Game::World world(config);
    world.initialize();

    TacticalAI::Game::Player* player = world.player();
    ASSERT_NE(player, nullptr);

    Input input;
    const std::vector<KeyboardKey>& keys = input.bindings(Action::Fire);
    ASSERT_FALSE(keys.empty());
    const KeyboardKey key = keys.front();

    constexpr double kFixedDelta = 1.0 / 60.0;   // 逻辑步
    constexpr int    kFrames     = 120 * 10;     // 渲染 10 秒 @120FPS

    int clicks = 0;
    double gameTime = 0.0;

    for (int frame = 0; frame < kFrames; ++frame) {
        // 每 14 帧点击一次 ≈ 0.117 秒一次（约 8.6 次/秒，人手快速点击）
        const bool clicking = (frame % 14 == 0);
        if (clicking) {
            ++clicks;
        }
        input.setKeyDownForTest(key, clicking);

        input.update();          // 每渲染帧采样一次（与真实循环一致）

        // 每两帧推进一次逻辑（120FPS → 60Hz）
        if (frame % 2 == 0) {
            if (input.consumeActionPressed(Action::Fire)) {
                player->requestFire();   // 游戏层唯一的开火请求入口
            }
            world.update(kFixedDelta, gameTime);
            gameTime += kFixedDelta;
        }
    }

    EXPECT_GT(clicks, 60) << "测试前提：应当产生了足够多次点击（实际 " << clicks << "）";
    EXPECT_EQ(player->shotsFired(), clicks)
        << clicks << " 次点击只打出了 " << player->shotsFired() << " 发子弹 —— "
        << "有点击被丢掉了（帧对齐吞事件，或被冷却静默拒绝）";
}
