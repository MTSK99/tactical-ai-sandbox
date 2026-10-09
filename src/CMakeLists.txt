# ---------------------------------------------------------------------------
# src/CMakeLists.txt
#
# 分层构建：engine / game / ai / debug 四个静态库 + 最终可执行文件。
#
#   engine  <- 无内部依赖（仅依赖 raylib / imgui / glm / json）
#   ai      <- 依赖 engine（需要 Vector2、Time 等基础类型）
#   game    <- 依赖 engine + ai
#   debug   <- 依赖 engine + game + ai（调试层需要看到实体和 AI 状态）
#   TacticalAI-Sandbox <- 依赖以上全部 + main.cpp
# ---------------------------------------------------------------------------

# --- 字形码点自动生成 -------------------------------------------------------
#
# 【为什么要在构建期生成】
# 界面中文是否能显示，取决于字形码点表是否覆盖了文案里的每个字。
# 手工维护这份表必然会漏（历史上漏了 253 个字，表现为"某几个字变方块"，
# 极难被发现）。因此改为构建时扫描源码自动生成：
#   scripts/gen_glyphs.py  ->  src/engine/GeneratedGlyphs.h
#
# 脚本内部会比对内容，只在真正变化时才写文件，避免无谓的重新编译。
set(_glyph_header "${CMAKE_CURRENT_SOURCE_DIR}/engine/GeneratedGlyphs.h")
set(_glyph_script "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/gen_glyphs.py")

find_package(Python3 COMPONENTS Interpreter QUIET)

# 字形码点表的输入 = src 下的全部源码（见下方 DEPENDS 的说明）
file(GLOB_RECURSE _glyph_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.inl"
)

# 排除脚本的输出文件本身：它也在 src/engine/ 下，把它列为自己的依赖
# 会形成依赖环（ninja: dependency cycle）。
list(REMOVE_ITEM _glyph_sources "${_glyph_header}")

if(Python3_Interpreter_FOUND)
    # PYTHONIOENCODING=utf-8：CMake 捕获子进程输出后会按自己的编码转发，
    # 若 Python 用本地代码页（中文系统 = GBK）写 stdout，构建日志里的中文就会乱码。
    # 显式指定 UTF-8 保证脚本消息在构建输出中始终可读。
    add_custom_command(
        OUTPUT  "${_glyph_header}"
        COMMAND "${CMAKE_COMMAND}" -E env PYTHONIOENCODING=utf-8
                "${Python3_EXECUTABLE}" "${_glyph_script}"
                "${CMAKE_CURRENT_SOURCE_DIR}/.." "${_glyph_header}"
        # 【修复】依赖清单原先是手工枚举的 8 个文件。脚本扫描的是整个源码树，
        # 因此任何**新增**的、含中文界面文案的源文件都不会触发生成 ——
        # 码点表静默过期、中文重新变方块，而且没有任何构建错误提示，
        # 正是文件头注释里描述的历史 bug 的复发路径。
        # 改为用 GLOB + CONFIGURE_DEPENDS 把 src 下全部源码列为依赖：
        # CONFIGURE_DEPENDS 让 CMake 每次构建时重新检查 glob 结果，
        # 新增/删除文件会自动触发重新配置。
        DEPENDS "${_glyph_script}" ${_glyph_sources}
        COMMENT "生成界面字形码点表 (GeneratedGlyphs.h)"
        VERBATIM
    )

    # 独立目标，便于手动触发生成
    add_custom_target(generate_glyphs DEPENDS "${_glyph_header}")

    # 让引擎库依赖这份生成的头文件。注意 add_dependencies 必须在
    # tacticalai_engine 定义之后才能调用，因此这里只记录一个标记变量，
    # 待引擎库建好后（文件末尾）再建立依赖关系。
    set(TACTICALAI_GLYPH_HEADER "${_glyph_header}")
else()
    message(STATUS "[engine] 未找到 Python3，跳过字形码点自动生成。")
    message(STATUS "[engine] 将直接使用已存在的 GeneratedGlyphs.h（若缺失则中文无法显示）。")
    set(TACTICALAI_GLYPH_HEADER "")
endif()

# --- 引擎层 -----------------------------------------------------------------
add_library(tacticalai_engine STATIC
    engine/Time.cpp
    engine/Input.cpp
    engine/Renderer.cpp
    engine/CjkFont.cpp
    engine/GameLoop.cpp
)

# 引擎库必须等字形码点表生成完毕再编译（CjkFont.cpp 会 include 它）
if(TACTICALAI_GLYPH_HEADER)
    add_custom_target(tacticalai_glyphs DEPENDS "${TACTICALAI_GLYPH_HEADER}")
    add_dependencies(tacticalai_engine tacticalai_glyphs)
endif()
target_include_directories(tacticalai_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(tacticalai_engine
    PUBLIC  tacticalai_build_flags
    PUBLIC  raylib
    PUBLIC  glm::glm
)
target_link_libraries(tacticalai_engine PRIVATE tacticalai_imgui nlohmann_json::nlohmann_json)

# --- AI 层 ------------------------------------------------------------------
add_library(tacticalai_ai STATIC
    ai/Blackboard.cpp
    ai/BehaviorTree.cpp
    ai/Perception.cpp
    ai/EQS.cpp
    ai/Pathfinding.cpp
)
target_include_directories(tacticalai_ai PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(tacticalai_ai
    PUBLIC  tacticalai_build_flags
    PUBLIC  tacticalai_engine
    PUBLIC  glm::glm
)

# --- 游戏层 -----------------------------------------------------------------
add_library(tacticalai_game STATIC
    game/Entity.cpp
    game/World.cpp
    game/Player.cpp
    game/Enemy.cpp
    game/Projectile.cpp
    game/AutoPilot.cpp
)
target_include_directories(tacticalai_game PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(tacticalai_game
    PUBLIC  tacticalai_build_flags
    PUBLIC  tacticalai_ai
)

# --- 调试层 -----------------------------------------------------------------
add_library(tacticalai_debug STATIC
    debug/DebugUI.cpp
    debug/AIDebugDraw.cpp
)
target_include_directories(tacticalai_debug PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(tacticalai_debug
    PUBLIC  tacticalai_build_flags
    PUBLIC  tacticalai_game
    PUBLIC  tacticalai_imgui
)

# --- 可执行文件 -------------------------------------------------------------
add_executable(TacticalAI-Sandbox
    main.cpp
)
set_target_properties(TacticalAI-Sandbox PROPERTIES
    OUTPUT_NAME "TacticalAI-Sandbox"
    WIN32_EXECUTABLE OFF   # 保留控制台，便于查看日志输出
)
target_link_libraries(TacticalAI-Sandbox
    PRIVATE tacticalai_debug
    PRIVATE tacticalai_game
    PRIVATE tacticalai_ai
    PRIVATE tacticalai_engine
)

# 让可执行文件能直接找到 assets/ 目录（若后续添加资源）
target_compile_definitions(TacticalAI-Sandbox PRIVATE
    # 【修复】原先用 CMAKE_SOURCE_DIR（**最顶层**源码目录）。若本项目被
    # add_subdirectory 嵌入到别的工程里，取到的会是外层工程的目录，
    # 资源路径静默指向不存在的位置。改用本工程的源码目录。
    TACTICALAI_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../assets"
)
