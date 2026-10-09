# ---------------------------------------------------------------------------
# Dependencies.cmake
#
# 统一的第三方依赖解析模块。
#
# 解析策略（每个依赖按顺序尝试，命中即停）：
#   1. 用户指定的本地目录   -DTACTICALAI_<DEP>_DIR=<path>
#   2. 项目内 external/<dep>  (git submodule / 手动放置)
#   3. 系统 / 包管理器安装   (find_package)
#   4. FetchContent 远程拉取 (需网络，可用 -DTACTICALAI_USE_FETCHCONTENT=OFF 关闭)
#
# 设计目标：离线可用。只要 external/ 下有源码，就完全不依赖网络。
# ---------------------------------------------------------------------------

include(FetchContent)
include(CMakePrintHelpers)

# 若某个依赖的源码里带了自己的 CMakeLists，把它加入构建会污染全局编译选项，
# 这里统一用一个中间 target 承载，避免它的 EXCLUDE_FROM_ALL 行为影响我们。
option(TACTICALAI_USE_FETCHCONTENT "允许在网络可用时自动拉取缺失依赖" ON)

# 记录每个依赖最终采用了哪种方式，构建结束后打印，方便排查“链接到了哪个版本”。
set(TACTICALAI_DEPS_RESOLVED "" CACHE INTERNAL "")
set(TACTICALAI_DEPS_MISSING  "" CACHE INTERNAL "")

function(_tai_mark_resolved dep how)
    set(_list ${TACTICALAI_DEPS_RESOLVED})
    list(APPEND _list "${dep} <- ${how}")
    set(TACTICALAI_DEPS_RESOLVED "${_list}" CACHE INTERNAL "" FORCE)
endfunction()

function(_tai_mark_missing dep)
    set(_list ${TACTICALAI_DEPS_MISSING})
    list(APPEND _list "${dep}")
    set(TACTICALAI_DEPS_MISSING "${_list}" CACHE INTERNAL "" FORCE)
endfunction()

# 把 `NAME_DIR` 形式的缓存变量统一成变量名，供各处复用
function(_tai_user_dir dep out_var)
    string(TOUPPER "${dep}" _dep_upper)
    set(${out_var} "${TACTICALAI_${_dep_upper}_DIR}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# 1. raylib —— 窗口、输入、2D/3D 绘制后端
# ---------------------------------------------------------------------------
function(_tai_resolve_raylib)
    if(TARGET raylib)
        _tai_mark_resolved("raylib" "已存在的 target")
        return()
    endif()

    _tai_user_dir(raylib _user_dir)
    set(_candidates
        "${_user_dir}"
        "${CMAKE_SOURCE_DIR}/external/raylib"
        "${CMAKE_SOURCE_DIR}/third_party/raylib"
    )

    foreach(_dir IN LISTS _candidates)
        if(_dir AND EXISTS "${_dir}/CMakeLists.txt")
            message(STATUS "[deps] raylib: 使用本地源码 ${_dir}")
            set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
            set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
            set(CUSTOMIZE_BUILD OFF CACHE BOOL "" FORCE)
            set(PLATFORM Desktop CACHE STRING "" FORCE)
            add_subdirectory("${_dir}" "${CMAKE_BINARY_DIR}/_deps/raylib-build" EXCLUDE_FROM_ALL)
            _tai_mark_resolved("raylib" "本地源码 ${_dir}")
            return()
        endif()
    endforeach()

    find_package(raylib QUIET)
    if(raylib_FOUND)
        message(STATUS "[deps] raylib: 系统包 ${raylib_VERSION}")
        _tai_mark_resolved("raylib" "系统包")
        return()
    endif()

    if(TACTICALAI_USE_FETCHCONTENT)
        message(STATUS "[deps] raylib: 通过 FetchContent 拉取 (需要网络)")
        set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(raylib
            GIT_REPOSITORY https://github.com/raysan5/raylib.git
            GIT_TAG        5.5
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(raylib)
        if(TARGET raylib)
            _tai_mark_resolved("raylib" "FetchContent")
            return()
        endif()
    endif()

    _tai_mark_missing("raylib")
endfunction()

# ---------------------------------------------------------------------------
# 2. imgui —— 调试 UI。上游不带 CMakeLists，因此这里手工编译所需源文件。
# ---------------------------------------------------------------------------
function(_tai_resolve_imgui)
    if(TARGET tacticalai_imgui)
        _tai_mark_resolved("imgui" "已存在的 target")
        return()
    endif()

    _tai_user_dir(imgui _user_dir)
    set(_candidates
        "${_user_dir}"
        "${CMAKE_SOURCE_DIR}/external/imgui"
        "${CMAKE_SOURCE_DIR}/third_party/imgui"
    )

    set(_imgui_root "")
    foreach(_dir IN LISTS _candidates)
        if(_dir AND EXISTS "${_dir}/imgui.cpp")
            set(_imgui_root "${_dir}")
            break()
        endif()
    endforeach()

    if(_imgui_root STREQUAL "")
        _tai_mark_missing("imgui")
        return()
    endif()

    message(STATUS "[deps] imgui: 编译本地源码 ${_imgui_root}")

    set(_imgui_sources
        "${_imgui_root}/imgui.cpp"
        "${_imgui_root}/imgui_draw.cpp"
        "${_imgui_root}/imgui_tables.cpp"
        "${_imgui_root}/imgui_widgets.cpp"
        "${_imgui_root}/imgui_demo.cpp"
        "${_imgui_root}/backends/imgui_impl_raylib.cpp"
    )

    # 上游 raylib 后端并非所有版本都自带；缺失时降级为「无后端」模式，
    # 调试层会自动改用 raylib 原生绘制（见 AIDebugDraw），保证项目仍可编译运行。
    set(_has_raylib_backend FALSE)
    if(EXISTS "${_imgui_root}/backends/imgui_impl_raylib.cpp")
        set(_has_raylib_backend TRUE)
    endif()

    if(_has_raylib_backend)
        set(_imgui_sources
            "${_imgui_root}/imgui.cpp"
            "${_imgui_root}/imgui_draw.cpp"
            "${_imgui_root}/imgui_tables.cpp"
            "${_imgui_root}/imgui_widgets.cpp"
            "${_imgui_root}/backends/imgui_impl_raylib.cpp"
        )
    else()
        set(_imgui_sources
            "${_imgui_root}/imgui.cpp"
            "${_imgui_root}/imgui_draw.cpp"
            "${_imgui_root}/imgui_tables.cpp"
            "${_imgui_root}/imgui_widgets.cpp"
        )
    endif()

    add_library(tacticalai_imgui STATIC ${_imgui_sources})
    target_include_directories(tacticalai_imgui PUBLIC "${_imgui_root}")
    target_compile_features(tacticalai_imgui PUBLIC cxx_std_17)

    if(_has_raylib_backend)
        message(STATUS "[deps] imgui: 启用 imgui_impl_raylib 后端")
        target_compile_definitions(tacticalai_imgui PUBLIC TACTICALAI_HAS_IMGUI_RAYLIB_BACKEND=1)
        target_link_libraries(tacticalai_imgui PUBLIC raylib)
    else()
        message(STATUS "[deps] imgui: 未找到 raylib 后端，调试 UI 降级为 raylib 原生绘制")
    endif()

    # imgui 源码在 MSVC 下会刷大量警告，这里只压制它自己，不影响我们的代码
    if(MSVC)
        target_compile_options(tacticalai_imgui PRIVATE /W0)
    else()
        target_compile_options(tacticalai_imgui PRIVATE -w)
    endif()

    _tai_mark_resolved("imgui" "本地源码 ${_imgui_root}")
endfunction()

# ---------------------------------------------------------------------------
# 3. glm —— 数学库（header-only）
# ---------------------------------------------------------------------------
function(_tai_resolve_glm)
    if(TARGET glm::glm OR TARGET glm)
        _tai_mark_resolved("glm" "已存在的 target")
        return()
    endif()

    _tai_user_dir(glm _user_dir)
    set(_candidates
        "${_user_dir}"
        "${CMAKE_SOURCE_DIR}/external/glm"
        "${CMAKE_SOURCE_DIR}/third_party/glm"
    )

    foreach(_dir IN LISTS _candidates)
        if(_dir AND EXISTS "${_dir}/CMakeLists.txt")
            message(STATUS "[deps] glm: 使用本地源码 ${_dir}")
            set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
            set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
            add_subdirectory("${_dir}" "${CMAKE_BINARY_DIR}/_deps/glm-build" EXCLUDE_FROM_ALL)
            _tai_mark_resolved("glm" "本地源码 ${_dir}")
            return()
        endif()
    endforeach()

    find_package(glm QUIET)
    if(glm_FOUND)
        message(STATUS "[deps] glm: 系统包 ${glm_VERSION}")
        _tai_mark_resolved("glm" "系统包")
        return()
    endif()

    if(TACTICALAI_USE_FETCHCONTENT)
        message(STATUS "[deps] glm: 通过 FetchContent 拉取 (需要网络)")
        set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(glm
            GIT_REPOSITORY https://github.com/g-truc/glm.git
            GIT_TAG        1.0.1
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(glm)
        if(TARGET glm::glm OR TARGET glm)
            _tai_mark_resolved("glm" "FetchContent")
            return()
        endif()
    endif()

    _tai_mark_missing("glm")
endfunction()

# ---------------------------------------------------------------------------
# 4. nlohmann-json —— 数据 / 配置序列化（header-only）
# ---------------------------------------------------------------------------
function(_tai_resolve_json)
    if(TARGET nlohmann_json::nlohmann_json)
        _tai_mark_resolved("nlohmann-json" "已存在的 target")
        return()
    endif()

    _tai_user_dir(nlohmann-json _user_dir)
    # 兼容两种常见目录名：external/nlohmann-json 与 external/nlohmann
    set(_candidates
        "${_user_dir}"
        "${CMAKE_SOURCE_DIR}/external/nlohmann-json"
        "${CMAKE_SOURCE_DIR}/external/nlohmann"
        "${CMAKE_SOURCE_DIR}/third_party/nlohmann"
        "${CMAKE_SOURCE_DIR}/third_party/nlohmann-json"
    )

    foreach(_dir IN LISTS _candidates)
        if(_dir AND EXISTS "${_dir}/CMakeLists.txt")
            message(STATUS "[deps] nlohmann-json: 使用本地源码 ${_dir}")
            set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
            add_subdirectory("${_dir}" "${CMAKE_BINARY_DIR}/_deps/json-build" EXCLUDE_FROM_ALL)
            _tai_mark_resolved("nlohmann-json" "本地源码 ${_dir}")
            return()
        endif()
        # 仅头文件形式的放置方式（只要 nlohmann/json.hpp 存在即可）
        if(_dir AND EXISTS "${_dir}/nlohmann/json.hpp")
            message(STATUS "[deps] nlohmann-json: 使用本地头文件 ${_dir}")
            add_library(tacticalai_json INTERFACE)
            target_include_directories(tacticalai_json INTERFACE "${_dir}")
            add_library(nlohmann_json::nlohmann_json ALIAS tacticalai_json)
            _tai_mark_resolved("nlohmann-json" "本地头文件 ${_dir}")
            return()
        endif()
    endforeach()

    find_package(nlohmann_json QUIET)
    if(nlohmann_json_FOUND)
        message(STATUS "[deps] nlohmann-json: 系统包 ${nlohmann_json_VERSION}")
        _tai_mark_resolved("nlohmann-json" "系统包")
        return()
    endif()

    if(TACTICALAI_USE_FETCHCONTENT)
        message(STATUS "[deps] nlohmann-json: 通过 FetchContent 拉取 (需要网络)")
        set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG        v3.11.3
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(nlohmann_json)
        if(TARGET nlohmann_json::nlohmann_json)
            _tai_mark_resolved("nlohmann-json" "FetchContent")
            return()
        endif()
    endif()

    _tai_mark_missing("nlohmann-json")
endfunction()

# ---------------------------------------------------------------------------
# 5. GoogleTest —— 仅测试构建需要
# ---------------------------------------------------------------------------
function(_tai_resolve_gtest)
    if(TARGET GTest::gtest)
        _tai_mark_resolved("googletest" "已存在的 target")
        return()
    endif()

    set(_user_dir "${TACTICALAI_GOOGLETEST_DIR}")
    set(_candidates
        "${_user_dir}"
        "${CMAKE_SOURCE_DIR}/external/googletest"
        "${CMAKE_SOURCE_DIR}/third_party/googletest"
    )

    foreach(_dir IN LISTS _candidates)
        if(_dir AND EXISTS "${_dir}/CMakeLists.txt")
            message(STATUS "[deps] googletest: 使用本地源码 ${_dir}")
            set(BUILD_GMOCK   ON  CACHE BOOL "" FORCE)
            set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
            # gtest 自带的 C++ 标准检测在部分新编译器上会误报，显式对齐
            set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
            add_subdirectory("${_dir}" "${CMAKE_BINARY_DIR}/_deps/gtest-build" EXCLUDE_FROM_ALL)
            _tai_mark_resolved("googletest" "本地源码 ${_dir}")
            return()
        endif()
    endforeach()

    find_package(GTest QUIET)
    if(GTest_FOUND)
        message(STATUS "[deps] googletest: 系统包")
        _tai_mark_resolved("googletest" "系统包")
        return()
    endif()

    if(TACTICALAI_USE_FETCHCONTENT)
        message(STATUS "[deps] googletest: 通过 FetchContent 拉取 (需要网络)")
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0
            GIT_SHALLOW    TRUE
        )
        # gtest 1.14 在 CMake 4.x 下会因 cmake_minimum_required 过低而报错，
        # 显式放宽策略版本，避免 "Compatibility with CMake < 3.5 removed" 中断配置。
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
        FetchContent_MakeAvailable(googletest)
        if(TARGET GTest::gtest)
            _tai_mark_resolved("googletest" "FetchContent")
            return()
        endif()
    endif()

    _tai_mark_missing("googletest")
endfunction()

# ---------------------------------------------------------------------------
# 入口：一次性解析全部依赖
# ---------------------------------------------------------------------------
function(tacticalai_resolve_dependencies)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING
        "第三方子项目普遍使用低版本 cmake_minimum_required，需放宽策略下限")

    _tai_resolve_raylib()
    _tai_resolve_imgui()
    _tai_resolve_glm()
    _tai_resolve_json()

    if(TACTICALAI_BUILD_TESTS)
        _tai_resolve_gtest()
    endif()

    message(STATUS "")
    message(STATUS "========== 依赖解析结果 ==========")
    foreach(_line IN LISTS TACTICALAI_DEPS_RESOLVED)
        message(STATUS "  [OK]      ${_line}")
    endforeach()
    foreach(_dep IN LISTS TACTICALAI_DEPS_MISSING)
        message(STATUS "  [MISSING] ${_dep}")
    endforeach()
    if(TACTICALAI_DEPS_MISSING)
        message(STATUS "  提示: 运行 pwsh ./scripts/fetch_dependencies.ps1 可一键克隆缺失依赖")
    endif()
    message(STATUS "==================================")
    message(STATUS "")
endfunction()
