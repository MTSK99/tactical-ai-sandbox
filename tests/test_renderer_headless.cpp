// ===========================================================================
//  tests/test_renderer_headless.cpp
//
//  "无头"必须是**运行期属性**，而不是构建期宏。
//
//  【这些测试在防什么】
//  引擎里原本用 `#if !defined(TACTICALAI_HEADLESS_TEST)` 决定"跳过窗口相关
//  调用"，而那个宏只加在**测试目标**上，引擎库本身按正式配置编译 ——
//  于是那些守卫在测试构建里**全部失效**：
//
//      · 测试里构造 Renderer       → 会真的 InitWindow + LoadFontEx（开窗！）
//      · 测试里构造 RenderTarget   → 会真的 LoadRenderTexture（没有 GL 上下文 = UB）
//
//  也就是说"我们有 279 个无头测试"这句话里，藏着一个随时会咬人的假设。
//  现在"要不要窗口"是构造函数的显式参数（createWindow），与构建配置无关，
//  于是可以在没有显示设备的 CI 上直接构造并断言。
// ===========================================================================
#include <gtest/gtest.h>

#include "engine/Renderer.h"

using namespace TacticalAI::Engine;

TEST(EngineHeadlessTest, RendererConstructsWithoutTouchingOpenGL) {
    // 第二个参数是 createWindow = false：这条路径必须"什么都不做"。
    Renderer renderer(640, 360, "headless-test", /*createWindow=*/false);

    EXPECT_FALSE(renderer.cjkFontLoaded())
        << "无窗口时不应该尝试加载字体（LoadFontEx 需要 GL 上下文）";

    // 尺寸查询必须有值：布局与相机会直接用它，返回 0 会导致除零与偏移错乱
    EXPECT_EQ(renderer.screenWidth(), 640);
    EXPECT_EQ(renderer.screenHeight(), 360);

    // 没有窗口就谈不上"用户要关窗"
    EXPECT_FALSE(renderer.shouldClose());

    // 这些调用在没有窗口时应当安全地变成空操作（不崩、也不做任何 GL 调用）
    renderer.setTargetFPS(60);
    renderer.setVsync(false);
    renderer.setWindowTitle("ignored");
    renderer.setWindowMinSize(320, 240);
    renderer.toggleFullscreen();
}

TEST(EngineHeadlessTest, RenderTargetIsInertWithoutWindow) {
    RenderTarget target(64, 64);

    EXPECT_FALSE(target.isValid())
        << "没有 GL 上下文却拿到了有效纹理 —— 说明真的调用了 LoadRenderTexture";

    target.resize(128, 128);           // 不应触碰 GL
    EXPECT_EQ(target.width(), 128);
    EXPECT_EQ(target.height(), 128);

    target.begin();                    // 空操作
    target.end();                      // 空操作
}
