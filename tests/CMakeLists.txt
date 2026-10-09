# ---------------------------------------------------------------------------
# tests/CMakeLists.txt —— GoogleTest 单元测试
#
# 只测试纯逻辑模块（AI 层 / 工具函数），不启动窗口，因此测试可无头运行。
# ---------------------------------------------------------------------------
if(NOT TACTICALAI_BUILD_TESTS)
    message(STATUS "[tests] TACTICALAI_BUILD_TESTS=OFF，跳过测试目标")
    return()
endif()

if(NOT TARGET GTest::gtest)
    message(WARNING
        "[tests] 未找到 GoogleTest，已自动关闭测试目标。\n"
        "        运行 pwsh ./scripts/fetch_dependencies.ps1 获取依赖后重新配置。")
    return()
endif()

include(GoogleTest)

add_executable(TacticalAI-Tests
    test_blackboard.cpp
    test_behavior_tree.cpp
    test_pathfinding.cpp
    test_eqs.cpp
    test_cover_effectiveness.cpp
    test_time.cpp
    test_cjk_font.cpp
    test_input.cpp
    test_renderer_headless.cpp
    test_projectile.cpp
    test_combat_cooldown.cpp
    test_dodge_roll.cpp
    test_match_outcome.cpp
    test_walkable.cpp
    test_enemy_search.cpp
)
target_link_libraries(TacticalAI-Tests
    PRIVATE tacticalai_build_flags
    PRIVATE tacticalai_ai
    PRIVATE tacticalai_engine
    PRIVATE tacticalai_game
    PRIVATE GTest::gtest
    PRIVATE GTest::gtest_main
)

# 【注意这个宏的作用范围】它只作用于**测试自己的翻译单元**，
# engine/ai/game 三个库仍是按正式配置编译的。
# 也就是说：不要指望引擎里的 `#if !defined(TACTICALAI_HEADLESS_TEST)` 守卫
# 能在测试构建里生效 —— 它们在这里是**失效**的。
# "能不能无头运行"应当由运行期参数决定（见 Renderer 的 createWindow 参数
# 与 tests/test_renderer_headless.cpp），本宏只作为"我是测试构建"的标记保留。
target_compile_definitions(TacticalAI-Tests PRIVATE TACTICALAI_HEADLESS_TEST=1)

gtest_discover_tests(TacticalAI-Tests
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    PROPERTIES LABELS "unit"
)
