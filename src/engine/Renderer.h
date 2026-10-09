// ===========================================================================
//  src/engine/Renderer.h
//
//  渲染层。职责：把「画什么」与「用 raylib 怎么画」解耦。
//
//  设计要点：
//   - 游戏层只调用本接口，不直接 include raylib 的绘制函数。
//     这样将来换后端（bgfx / SDL_gpu）只需替换本文件的实现。
//   - 渲染是分层的：世界空间 -> 调试绘制 -> UI。
//     通过 beginWorld/endWorld 显式管理 2D 相机的推入推出，避免缩放出错。
//   - RAII：RenderTarget 等资源用 RAII 包装，析构自动释放。
// ===========================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "raylib.h"
#include "glm/vec2.hpp"

namespace TacticalAI::Engine {

/// 调色板。集中管理，避免颜色魔数散落在各处；
/// 遵循「涨红跌绿」等既有约定之外的战术沙盒配色（深色底 + 高对比强调色）。
namespace Colors {
    constexpr Color kBackground   {  18,  20,  26, 255 };  // 近黑蓝，长时间观看不刺眼
    constexpr Color kGridMinor    {  38,  42,  52, 255 };
    constexpr Color kGridMajor    {  54,  60,  74, 255 };
    constexpr Color kAxisX        { 190,  70,  70, 255 };
    constexpr Color kAxisY        {  70, 150,  90, 255 };

    constexpr Color kPlayer       {  90, 175, 255, 255 };
    constexpr Color kEnemy        { 235, 100,  95, 255 };
    constexpr Color kEnemyAlert   { 255, 190,  70, 255 };
    constexpr Color kCover        { 120, 110,  95, 255 };
    constexpr Color kWall         {  78,  84,  96, 255 };

    constexpr Color kPerceptionFov{ 255, 220, 120,  40 };
    constexpr Color kPerceptionRay{ 255, 220, 120, 160 };
    constexpr Color kPath         { 120, 230, 170, 220 };
    constexpr Color kEQSBest      {  80, 255, 200, 255 };
    constexpr Color kEQSCandidate { 140, 140, 160, 120 };

    constexpr Color kTextPrimary  { 226, 230, 238, 255 };
    constexpr Color kTextMuted    { 140, 148, 162, 255 };
    constexpr Color kAccent       { 120, 210, 255, 255 };
    constexpr Color kWarning      { 255, 190,  70, 255 };
    constexpr Color kError        { 235, 100,  95, 255 };
    constexpr Color kSuccess      { 120, 230, 170, 255 };
    constexpr Color kPanelBg      {  26,  29,  38, 235 };
    constexpr Color kPanelBorder  {  58,  64,  78, 255 };
}

/// 2D 世界相机：把「世界坐标」映射到屏幕。
/// 战术沙盒是俯视 2D 场景，因此只需平移 + 缩放，不需要完整 3D 相机矩阵。
class Camera2DController {
public:
    Camera2DController() = default;

    void setTarget(glm::vec2 target) noexcept;
    void setZoom(float zoom) noexcept;
    void setOffset(glm::vec2 offset) noexcept;

    void moveBy(glm::vec2 delta) noexcept;
    void zoomBy(float factor) noexcept;

    [[nodiscard]] glm::vec2 target() const noexcept { return target_; }
    [[nodiscard]] float zoom() const noexcept { return zoom_; }
    [[nodiscard]] glm::vec2 offset() const noexcept { return offset_; }

    /// 世界坐标 -> 屏幕坐标
    [[nodiscard]] glm::vec2 worldToScreen(glm::vec2 world) const noexcept;
    /// 屏幕坐标 -> 世界坐标（鼠标拾取必备）
    [[nodiscard]] glm::vec2 screenToWorld(glm::vec2 screen) const noexcept;

    /// 转换到 raylib 原生结构，供 BeginMode2D 使用。
    [[nodiscard]] ::Camera2D toRaylibCamera() const noexcept;

private:
    glm::vec2 target_{0.0f, 0.0f};
    glm::vec2 offset_{0.0f, 0.0f};
    float     zoom_ = 1.0f;
    float     minZoom_ = 0.15f;
    float     maxZoom_ = 8.0f;
};

/// 文本对齐方式。
enum class TextAlign { Left, Center, Right };

/// 渲染器。
///
/// 所有绘制调用都必须落在 beginFrame()/endFrame() 之间，
/// 世界坐标内容必须落在 beginWorld()/endWorld() 之间。
class Renderer {
public:
    /// @param createWindow 是否创建窗口（默认是）。
    ///
    /// 【为什么"要不要窗口"是运行期参数，而不是构建期宏】
    /// 这里原先依赖 `TACTICALAI_HEADLESS_TEST` 宏来决定跳过窗口相关调用，
    /// 但那个宏只加在**测试目标**上，而引擎库本身按正式配置编译 ——
    /// 于是引擎里所有 `#if !defined(TACTICALAI_HEADLESS_TEST)` 守卫在测试
    /// 构建里**全部失效**：测试一旦构造 Renderer，就会真的去
    /// InitWindow / LoadFontEx / LoadRenderTexture（没有 GL 上下文时是 UB）。
    ///
    /// 改成显式参数之后，"能否无头运行"变成代码结构的属性，与构建配置无关：
    /// 传 false 时构造出的 Renderer 不碰任何 GL/GLFW 调用（字体不加载、
    /// 渲染纹理无效），因此可以在没有显示设备的 CI 里被单元测试直接构造。
    explicit Renderer(int screenWidth, int screenHeight, std::string_view title,
                      bool createWindow = true);
    ~Renderer();

    // 持有 raylib 的全局窗口状态，禁止拷贝
    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    // -- 帧生命周期 ----------------------------------------------------------

    /// 开始一帧：清屏、重置相机模式。返回 false 表示窗口收到关闭请求。
    [[nodiscard]] bool beginFrame();

    /// 进入世界空间（推入 2D 相机）。之后所有坐标按世界坐标解释。
    void beginWorld();

    /// 退出世界空间，回到屏幕空间。
    void endWorld();

    /// 结束一帧并呈现。
    void endFrame();

    // -- 窗口 ----------------------------------------------------------------

    [[nodiscard]] bool shouldClose() const noexcept;
    [[nodiscard]] int screenWidth() const noexcept;
    [[nodiscard]] int screenHeight() const noexcept;
    [[nodiscard]] glm::vec2 screenSize() const noexcept;
    [[nodiscard]] glm::vec2 screenCenter() const noexcept;

    void setTargetFPS(int fps) noexcept;

    /// 运行时开关垂直同步。
    ///
    /// 【修复】GameLoop::Config::enableVsync 此前是死配置：窗口创建时
    /// 无条件带上 FLAG_VSYNC_HINT，调用方怎么设都不生效，也没有任何提示。
    /// 现在由 GameLoop 在初始化时按配置显式应用。
    void setVsync(bool enabled) noexcept;
    void setWindowTitle(std::string_view title);
    void toggleFullscreen() noexcept;
    void setWindowMinSize(int width, int height) noexcept;

    [[nodiscard]] Camera2DController& camera() noexcept { return camera_; }
    [[nodiscard]] const Camera2DController& camera() const noexcept { return camera_; }

    // -- 世界空间绘制 --------------------------------------------------------

    void drawGrid(glm::vec2 origin, glm::vec2 extent, float cellSize) const;
    void drawLine(glm::vec2 from, glm::vec2 to, Color color, float thickness = 1.0f) const;
    void drawRect(glm::vec2 center, glm::vec2 size, Color color) const;
    void drawRectLines(glm::vec2 center, glm::vec2 size, Color color, float thickness = 1.0f) const;
    void drawCircle(glm::vec2 center, float radius, Color color) const;
    void drawCircleLines(glm::vec2 center, float radius, Color color, float thickness = 1.0f) const;
    void drawRing(glm::vec2 center, float innerRadius, float outerRadius, Color color) const;
    /// 扇形，用于绘制视野 (FOV)。角度为弧度，0 表示朝右，顺时针为正。
    void drawSector(glm::vec2 center, float radius, float startAngle, float endAngle, Color color) const;
    /// 箭头，用于标记朝向；长度决定视觉重量。
    void drawArrow(glm::vec2 from, glm::vec2 to, Color color, float thickness = 2.0f) const;
    /// 折线路径，用于绘制寻路结果。
    void drawPolyline(const std::vector<glm::vec2>& points, Color color, float thickness = 2.0f) const;
    /// 带背景的胶囊标签，用于在实体上方标注 AI 状态。
    void drawLabel(glm::vec2 worldAnchor, std::string_view text, Color color, int fontSize = 12) const;

    // -- 屏幕空间绘制（不受相机影响，用于 HUD） -------------------------------

    void drawScreenRect(Rectangle rect, Color color) const;
    void drawScreenRectLines(Rectangle rect, Color color, float thickness = 1.0f) const;
    void drawScreenText(std::string_view text, glm::vec2 position, int fontSize, Color color,
                        TextAlign align = TextAlign::Left) const;
    /// 带半透明底板的文本面板，提升可读性。
    void drawPanel(Rectangle rect, Color background, Color border) const;

    // -- 文本工具（无状态，供各处复用） --------------------------------------

    /// 测量文本渲染宽度（像素）。用 raylib 默认字体测量。
    [[nodiscard]] static int measureText(std::string_view text, int fontSize);

    /// 用**指定字体**测量文本渲染宽度（像素）。
    ///
    /// 【为什么需要这个重载】
    /// 无参版本内部用的是 raylib 默认 ASCII 字体，而项目实际用 CJK 字体
    /// 绘制。用默认字体量中文，每个汉字按缺省替代符计宽，结果系统性偏小
    /// ——标签底板宽度、Center/Right 对齐全部错位。凡是"用某个字体画、
    /// 又要用它量"的地方都应使用本重载。
    [[nodiscard]] static int measureText(std::string_view text, int fontSize, ::Font font);
    /// 获取内置字体。为空表示使用 raylib 默认字体。
    [[nodiscard]] const Font& defaultFont() const noexcept { return font_; }

    /// 字体加载是否成功。false 表示正在用 raylib 默认字体（不支持中文）。
    [[nodiscard]] bool cjkFontLoaded() const noexcept { return fontLoaded_; }
    /// 字体加载的说明文本（成功摘要或失败原因），适合打印到控制台排查问题。
    [[nodiscard]] const std::string& fontNotice() const noexcept { return fontNotice_; }

private:
    void initializeWindow(std::string_view title);

    Camera2DController camera_;
    Font   font_{};
    bool   fontLoaded_ = false;
    std::string fontNotice_;   ///< 字体加载结果说明，供启动日志与调试面板显示
    bool   worldModeActive_ = false;

    /// 无窗口时 screenWidth()/screenHeight() 返回的兜底尺寸。
    /// （有窗口时读真实尺寸；没有时返回构造时声明的尺寸，行为可预期。）
    int windowWidth_  = 1280;
    int windowHeight_ = 720;

    /// 本对象是否真的创建了窗口。析构时据此决定要不要 CloseWindow ——
    /// 避免"无头构造的 Renderer 顺手关掉了别人的窗口"。
    bool ownsWindow_ = false;
};

/// RAII 包装的渲染纹理。析构时自动 UnloadRenderTexture，
/// 避免手写 Resize 流程时遗漏释放导致显存泄漏。
class RenderTarget {
public:
    RenderTarget(int width, int height);
    ~RenderTarget();

    RenderTarget(const RenderTarget&)            = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;
    RenderTarget(RenderTarget&& other) noexcept;
    RenderTarget& operator=(RenderTarget&& other) noexcept;

    /// 重新分配尺寸。若尺寸未变则不做任何事。
    void resize(int width, int height);

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] RenderTexture2D& handle() noexcept { return texture_; }
    [[nodiscard]] const RenderTexture2D& handle() const noexcept { return texture_; }

    /// 以该纹理为绘制目标（推入）。
    void begin() const;
    /// 结束绘制目标（推出）。
    void end() const;

private:
    void release() noexcept;

    RenderTexture2D texture_{};
    int  width_  = 0;
    int  height_ = 0;
    bool valid_  = false;
};

} // namespace TacticalAI::Engine
