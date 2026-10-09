// ===========================================================================
//  src/engine/Renderer.cpp
// ===========================================================================
#include "engine/Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

#include "engine/CjkFont.h"

namespace TacticalAI::Engine {
namespace {

/// 世界 -> raylib 的向量转换。
/// glm::vec2 与 raylib::Vector2 布局相同（两个 float），但保持显式转换更安全。
inline ::Vector2 toRaylib(glm::vec2 v) noexcept {
    return ::Vector2{v.x, v.y};
}

inline ::Color toRaylib(Color c) noexcept {
    return c;  // 同一类型，占位以保持调用点对称
}

} // namespace

// ===========================================================================
//  Camera2DController
// ===========================================================================
void Camera2DController::setTarget(glm::vec2 target) noexcept {
    target_ = target;
}

void Camera2DController::setZoom(float zoom) noexcept {
    zoom_ = std::clamp(zoom, minZoom_, maxZoom_);
}

void Camera2DController::setOffset(glm::vec2 offset) noexcept {
    offset_ = offset;
}

void Camera2DController::moveBy(glm::vec2 delta) noexcept {
    target_ += delta;
}

void Camera2DController::zoomBy(float factor) noexcept {
    if (factor > 0.0f) {
        setZoom(zoom_ * factor);
    }
}

glm::vec2 Camera2DController::worldToScreen(glm::vec2 world) const noexcept {
    return (world - target_) * zoom_ + offset_;
}

glm::vec2 Camera2DController::screenToWorld(glm::vec2 screen) const noexcept {
    if (zoom_ <= 0.0f) {
        return screen;
    }
    return (screen - offset_) / zoom_ + target_;
}

::Camera2D Camera2DController::toRaylibCamera() const noexcept {
    ::Camera2D camera{};
    camera.offset   = toRaylib(offset_);
    camera.target   = toRaylib(target_);
    camera.rotation = 0.0f;
    camera.zoom     = zoom_;
    return camera;
}

// ===========================================================================
//  Renderer
// ===========================================================================
Renderer::Renderer(int screenWidth, int screenHeight, std::string_view title,
                   bool createWindow) {
    if (screenWidth > 0) {
        windowWidth_ = screenWidth;
    }
    if (screenHeight > 0) {
        windowHeight_ = screenHeight;
    }

    // -----------------------------------------------------------------------
    // 无头构造：不碰任何 GL / GLFW 调用，只把尺寸记下来。
    //
    // 这条路径是给单元测试与 CI 用的（没有显示设备）。它必须"什么都不做"：
    // InitWindow / LoadFontEx / LoadRenderTexture 在没有 GL 上下文时都是 UB。
    // -----------------------------------------------------------------------
    if (!createWindow) {
        fontNotice_ = "无头模式：未创建窗口，使用 raylib 默认字体（不含中文字形）";
        camera_.setOffset(glm::vec2{static_cast<float>(windowWidth_) * 0.5f,
                                    static_cast<float>(windowHeight_) * 0.5f});
        camera_.setZoom(1.0f);
        return;
    }

    initializeWindow(title);
    ownsWindow_ = IsWindowReady();

    // 加载带中文字形的字体。必须在窗口创建之后——
    // LoadFontEx 需要 OpenGL 上下文才能把字形烘进纹理。
    //
    // 这是「窗口里中文显示为方块」的根治点：raylib 默认字体只含 ASCII，
    // 不显式加载 CJK 字体，任何中文都画不出来。
    //
    // baseSize 选 16 而非 32：HUD 的字号在 10~16 之间，把烘焙尺寸设成
    // 与常用显示尺寸接近，可以避免 raylib 因「字形比期望尺寸还大」而
    // 刷 WARNING（DrawTextEx 放大字形会走另一条更模糊的路径）。
    {
        CjkFontLoadResult fontResult = loadDefaultCjkFont(16);
        if (fontResult.success) {
            font_       = fontResult.font;
            fontLoaded_ = true;
        }
        fontNotice_ = fontResult.message;
    }

    // 若目标分辨率与配置不符（高 DPI 缩放），同步设置窗口尺寸
    if (screenWidth > 0 && screenHeight > 0) {
        SetWindowSize(screenWidth, screenHeight);
    }

    camera_.setOffset(glm::vec2{static_cast<float>(GetScreenWidth()) * 0.5f,
                                static_cast<float>(GetScreenHeight()) * 0.5f});
    camera_.setZoom(1.0f);
}

Renderer::~Renderer() {
    if (fontLoaded_) {
        UnloadFont(font_);
        fontLoaded_ = false;
    }
    // 只关自己创建的窗口：无头构造的实例不该顺手关掉别人的窗口。
    if (ownsWindow_ && IsWindowReady()) {
        CloseWindow();
    }
}

void Renderer::initializeWindow(std::string_view title) {
    // 幂等：已经有窗口就不重复创建（重复 InitWindow 会丢状态）。
    // 「不要在无头环境里调用本函数」现在由构造函数的 createWindow 参数保证，
    // 不再依赖构建期宏。
    if (IsWindowReady()) {
        return;
    }

    // 屏蔽 raylib 的信息级日志，保持控制台整洁。
    //
    // 注意这里用 LOG_WARNING 而不是 LOG_ERROR：raylib 在烘焙字形时会对
    // 「字形高度比 baseSize 大 1px」的字体打 WARNING（simhei.ttf 的竖线
    // '|' 就是这种情况）。这是字体本身的度量特性，不影响渲染结果，
    // 但会刷满整个控制台、把真正的错误淹没掉。因此降到 LOG_ERROR——
    // 只保留确实需要处理的错误。
    SetTraceLogLevel(LOG_ERROR);

    // 这些标志必须在 InitWindow 之前设置
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE);

    const std::string titleString(title);
    InitWindow(1280, 720, titleString.c_str());
    SetWindowMinSize(640, 360);
    SetExitKey(KEY_NULL);  // 禁用 ESC 关窗；ESC 留给游戏逻辑（如取消选择）
}

bool Renderer::beginFrame() {
    if (shouldClose()) {
        return false;
    }

    worldModeActive_ = false;

    BeginDrawing();
    ClearBackground(Colors::kBackground);
    return true;
}

void Renderer::beginWorld() {
    // 每次进入世界空间都按当前窗口尺寸刷新 offset，
    // 否则窗口缩放后世界会偏移到屏幕角落。
    camera_.setOffset(glm::vec2{static_cast<float>(GetScreenWidth()) * 0.5f,
                                static_cast<float>(GetScreenHeight()) * 0.5f});

    ::Camera2D native = camera_.toRaylibCamera();
    BeginMode2D(native);
    worldModeActive_ = true;
}

void Renderer::endWorld() {
    if (worldModeActive_) {
        EndMode2D();
        worldModeActive_ = false;
    }
}

void Renderer::endFrame() {
    // 若调用方忘记 endWorld，这里兜底，防止 EndDrawing 在错误的相机模式下降级
    endWorld();
    EndDrawing();
}

bool Renderer::shouldClose() const noexcept {
    // 没有窗口就谈不上"用户要关窗"（无头构造的实例走这条路径）。
    return ::IsWindowReady() ? ::WindowShouldClose() : false;
}

int Renderer::screenWidth() const noexcept {
    // 有窗口读真实尺寸；没有则返回构造时声明的尺寸 —— 调用方（布局、相机）
    // 拿到的永远是一个正数，不必到处判断"有没有窗口"。
    return ::IsWindowReady() ? ::GetScreenWidth() : windowWidth_;
}

int Renderer::screenHeight() const noexcept {
    return ::IsWindowReady() ? ::GetScreenHeight() : windowHeight_;
}

glm::vec2 Renderer::screenSize() const noexcept {
    return glm::vec2{static_cast<float>(screenWidth()), static_cast<float>(screenHeight())};
}

glm::vec2 Renderer::screenCenter() const noexcept {
    return screenSize() * 0.5f;
}

void Renderer::setTargetFPS(int fps) noexcept {
    if (!::IsWindowReady()) {
        return;   // 无窗口：没有帧率可设（无头构造的实例）
    }
    ::SetTargetFPS(fps);
}

void Renderer::setVsync(bool enabled) noexcept {
    if (!::IsWindowReady()) {
        return;
    }
    if (enabled) {
        ::SetWindowState(FLAG_VSYNC_HINT);
    } else {
        ::ClearWindowState(FLAG_VSYNC_HINT);
    }
}

void Renderer::setWindowTitle(std::string_view title) {
    if (!::IsWindowReady()) {
        return;
    }
    const std::string text(title);
    ::SetWindowTitle(text.c_str());
}

void Renderer::toggleFullscreen() noexcept {
    if (!::IsWindowReady()) {
        return;
    }
    ::ToggleFullscreen();
}

void Renderer::setWindowMinSize(int width, int height) noexcept {
    if (!::IsWindowReady()) {
        return;
    }
    ::SetWindowMinSize(width, height);
}

// ---------------------------------------------------------------------------
// 世界空间绘制
// ---------------------------------------------------------------------------
void Renderer::drawGrid(glm::vec2 origin, glm::vec2 extent, float cellSize) const {
    if (cellSize <= 0.0f) {
        return;
    }

    const float x0 = origin.x;
    const float y0 = origin.y;
    const float x1 = origin.x + extent.x;
    const float y1 = origin.y + extent.y;

    // 竖线
    int index = 0;
    for (float x = x0; x <= x1 + 0.001f; x += cellSize, ++index) {
        // 每 8 格加深一次，形成「主/次网格」，便于估算距离
        const Color color = (index % 8 == 0) ? Colors::kGridMajor : Colors::kGridMinor;
        DrawLineEx(::Vector2{x, y0}, ::Vector2{x, y1}, 1.0f, color);
    }

    // 横线
    index = 0;
    for (float y = y0; y <= y1 + 0.001f; y += cellSize, ++index) {
        const Color color = (index % 8 == 0) ? Colors::kGridMajor : Colors::kGridMinor;
        DrawLineEx(::Vector2{x0, y}, ::Vector2{x1, y}, 1.0f, color);
    }

    // 坐标轴：帮助判断世界原点位置
    if (y0 <= 0.0f && y1 >= 0.0f) {
        DrawLineEx(::Vector2{x0, 0.0f}, ::Vector2{x1, 0.0f}, 1.5f, Colors::kAxisX);
    }
    if (x0 <= 0.0f && x1 >= 0.0f) {
        DrawLineEx(::Vector2{0.0f, y0}, ::Vector2{0.0f, y1}, 1.5f, Colors::kAxisY);
    }
}

void Renderer::drawLine(glm::vec2 from, glm::vec2 to, Color color, float thickness) const {
    if (thickness <= 1.0f) {
        DrawLineV(toRaylib(from), toRaylib(to), toRaylib(color));
    } else {
        DrawLineEx(toRaylib(from), toRaylib(to), thickness, toRaylib(color));
    }
}

void Renderer::drawRect(glm::vec2 center, glm::vec2 size, Color color) const {
    // 约定：传中心点而非左上角，与实体的 position 语义保持一致
    const ::Rectangle rect{center.x - size.x * 0.5f,
                           center.y - size.y * 0.5f,
                           size.x, size.y};
    DrawRectangleRec(rect, toRaylib(color));
}

void Renderer::drawRectLines(glm::vec2 center, glm::vec2 size, Color color, float thickness) const {
    const ::Rectangle rect{center.x - size.x * 0.5f,
                           center.y - size.y * 0.5f,
                           size.x, size.y};
    if (thickness <= 1.0f) {
        DrawRectangleLinesEx(rect, 1.0f, toRaylib(color));
    } else {
        DrawRectangleLinesEx(rect, thickness, toRaylib(color));
    }
}

void Renderer::drawCircle(glm::vec2 center, float radius, Color color) const {
    DrawCircleV(toRaylib(center), radius, toRaylib(color));
}

void Renderer::drawCircleLines(glm::vec2 center, float radius, Color color, float thickness) const {
    // raylib 的 DrawCircleLines 只有 1px 版本，用分段线自行实现可控线宽
    constexpr int kSegments = 48;
    constexpr float kTwoPi = 6.28318530717958647692f;

    ::Vector2 previous{center.x + radius, center.y};
    for (int i = 1; i <= kSegments; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(kSegments);
        const ::Vector2 current{center.x + std::cos(angle) * radius,
                                center.y + std::sin(angle) * radius};
        DrawLineEx(previous, current, thickness, toRaylib(color));
        previous = current;
    }
}

void Renderer::drawRing(glm::vec2 center, float innerRadius, float outerRadius, Color color) const {
    DrawRing(toRaylib(center), innerRadius, outerRadius, 0.0f, 360.0f, 64, toRaylib(color));
}

void Renderer::drawSector(glm::vec2 center, float radius, float startAngle, float endAngle,
                          Color color) const {
    // raylib 的扇形角度单位为度，这里统一收弧度输入，内部转换
    constexpr float kRadToDeg = 57.29577951308232f;
    DrawCircleSector(toRaylib(center), radius,
                     startAngle * kRadToDeg, endAngle * kRadToDeg, 48, toRaylib(color));
}

void Renderer::drawArrow(glm::vec2 from, glm::vec2 to, Color color, float thickness) const {
    DrawLineEx(toRaylib(from), toRaylib(to), thickness, toRaylib(color));

    const glm::vec2 delta = to - from;
    const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    if (length < 1e-4f) {
        return;
    }

    // 箭头头部长度随整体长度缩放，短箭头不会变成一坨
    const glm::vec2 direction = delta / length;
    const float headLength = std::min(length * 0.35f, thickness * 4.0f);
    constexpr float kHeadAngle = 0.45f;  // 约 26 度

    const float cosA = std::cos(kHeadAngle);
    const float sinA = std::sin(kHeadAngle);

    const glm::vec2 left{direction.x * cosA - direction.y * sinA,
                         direction.x * sinA + direction.y * cosA};
    const glm::vec2 right{direction.x * cosA + direction.y * sinA,
                          -direction.x * sinA + direction.y * cosA};

    const glm::vec2 tip = to;
    DrawLineEx(toRaylib(tip), toRaylib(tip - left * headLength), thickness, toRaylib(color));
    DrawLineEx(toRaylib(tip), toRaylib(tip - right * headLength), thickness, toRaylib(color));
}

void Renderer::drawPolyline(const std::vector<glm::vec2>& points, Color color,
                            float thickness) const {
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        DrawLineEx(toRaylib(points[i]), toRaylib(points[i + 1]), thickness, toRaylib(color));
    }
}

void Renderer::drawLabel(glm::vec2 worldAnchor, std::string_view text, Color color,
                         int fontSize) const {
    if (text.empty()) {
        return;
    }

    // 在有缩放的世界空间里，文字会被拉大或缩小，因此反算缩放，
    // 让标签在任何 zoom 下都保持可读的字号。
    const float zoom = (camera_.zoom() > 1e-4f) ? camera_.zoom() : 1.0f;
    const float inverse = 1.0f / zoom;

    const std::string value(text);
    // 【修复】测量必须与实际绘制用同一套字体。
    // MeasureText() 走的是 raylib 默认 ASCII 字体，而下面 DrawTextEx 用的是
    // CJK 字体 —— 中文在默认字体里按替代符计宽，导致标签底板宽度、
    // 居中对齐整体偏小错位。
    const int textWidth = measureText(value, fontSize,
                                      fontLoaded_ ? font_ : GetFontDefault());

    const float padding = 4.0f * inverse;
    const float width  = static_cast<float>(textWidth) * inverse + padding * 2.0f;
    const float height = static_cast<float>(fontSize) * inverse + padding * 2.0f;

    const ::Rectangle background{
        worldAnchor.x - width * 0.5f,
        worldAnchor.y - height,
        width,
        height
    };

    DrawRectangleRec(background, Fade(Colors::kPanelBg, 0.85f));
    DrawRectangleLinesEx(background, 1.0f * inverse, Fade(color, 0.7f));

    ::Vector2 textPosition{worldAnchor.x - width * 0.5f + padding,
                           worldAnchor.y - height + padding};
    DrawTextEx(fontLoaded_ ? font_ : GetFontDefault(), value.c_str(), textPosition,
               static_cast<float>(fontSize) * inverse, 0.0f, color);
}

// ---------------------------------------------------------------------------
// 屏幕空间绘制
// ---------------------------------------------------------------------------
void Renderer::drawScreenRect(Rectangle rect, Color color) const {
    DrawRectangleRec(rect, color);
}

void Renderer::drawScreenRectLines(Rectangle rect, Color color, float thickness) const {
    DrawRectangleLinesEx(rect, thickness, color);
}

void Renderer::drawScreenText(std::string_view text, glm::vec2 position, int fontSize,
                              Color color, TextAlign align) const {
    if (text.empty()) {
        return;
    }

    const std::string value(text);
    // 【修复】同上：用实际绘制字体测量，否则 Center/Right 对齐偏移。
    const int textWidth = measureText(value, fontSize,
                                      fontLoaded_ ? font_ : GetFontDefault());

    float x = position.x;
    switch (align) {
        case TextAlign::Center: x -= static_cast<float>(textWidth) * 0.5f; break;
        case TextAlign::Right:  x -= static_cast<float>(textWidth);        break;
        case TextAlign::Left:   break;
    }

    DrawTextEx(fontLoaded_ ? font_ : GetFontDefault(), value.c_str(),
               ::Vector2{x, position.y}, static_cast<float>(fontSize), 0.0f, color);
}

void Renderer::drawPanel(Rectangle rect, Color background, Color border) const {
    DrawRectangleRec(rect, background);
    DrawRectangleLinesEx(rect, 1.0f, border);
}

int Renderer::measureText(std::string_view text, int fontSize) {
    if (text.empty()) {
        return 0;
    }
    const std::string value(text);
    return MeasureText(value.c_str(), fontSize);
}

int Renderer::measureText(std::string_view text, int fontSize, ::Font font) {
    if (text.empty()) {
        return 0;
    }
    const std::string value(text);
    // DrawTextEx 的行距参数是 0，这里保持一致，测量结果才与实际渲染吻合
    return static_cast<int>(MeasureTextEx(font, value.c_str(),
                                          static_cast<float>(fontSize), 0.0f).x);
}

// ===========================================================================
//  RenderTarget —— RAII 渲染纹理
// ===========================================================================
RenderTarget::RenderTarget(int width, int height) {
    resize(width, height);
}

RenderTarget::~RenderTarget() {
    release();
}

RenderTarget::RenderTarget(RenderTarget&& other) noexcept
    : texture_(other.texture_), width_(other.width_), height_(other.height_),
      valid_(other.valid_) {
    // 移动后源对象必须失效，否则析构时会重复释放同一纹理
    other.texture_ = RenderTexture2D{};
    other.width_  = 0;
    other.height_ = 0;
    other.valid_  = false;
}

RenderTarget& RenderTarget::operator=(RenderTarget&& other) noexcept {
    if (this != &other) {
        release();

        texture_ = other.texture_;
        width_   = other.width_;
        height_  = other.height_;
        valid_   = other.valid_;

        other.texture_ = RenderTexture2D{};
        other.width_  = 0;
        other.height_ = 0;
        other.valid_  = false;
    }
    return *this;
}

void RenderTarget::release() noexcept {
    if (valid_) {
        UnloadRenderTexture(texture_);
        texture_ = RenderTexture2D{};
        valid_ = false;
    }
    width_  = 0;
    height_ = 0;
}

void RenderTarget::resize(int width, int height) {
    // 钳制到合法范围：raylib 不接受 0 或负尺寸的 FBO
    width  = std::max(width, 1);
    height = std::max(height, 1);

    if (valid_ && width == width_ && height == height_) {
        return;
    }

    release();

    // 没有窗口就没有 GL 上下文：此时**不能**创建渲染纹理（UB），
    // 保持 valid_ = false，begin()/end() 自然变成空操作。
    // 判定用运行期的 IsWindowReady()，不依赖构建期宏 —— 见 Renderer 构造函数的说明。
    if (!::IsWindowReady()) {
        width_  = width;
        height_ = height;
        return;
    }

    texture_ = LoadRenderTexture(width, height);

    // 【修复】原先不检查加载结果：纹理创建失败时 texture_.id == 0，
    // 却仍设置过滤并把 valid_ 置 true，之后 begin() 会对一个无效 FBO
    // 调 BeginTextureMode。现在只有真正拿到纹理才算有效。
    if (texture_.id != 0) {
        // 渲染纹理在 OpenGL 中是上下翻转的，设置纹理过滤保证缩放时清晰
        SetTextureFilter(texture_.texture, TEXTURE_FILTER_BILINEAR);
        valid_ = true;
    } else {
        std::fprintf(stderr, "[RenderTarget] 渲染纹理创建失败 (%dx%d)\n", width, height);
    }

    width_  = width;
    height_ = height;
}

bool RenderTarget::isValid() const noexcept {
    return valid_;
}

void RenderTarget::begin() const {
    if (!valid_ || !::IsWindowReady()) {
        return;
    }
    BeginTextureMode(texture_);
}

void RenderTarget::end() const {
    if (!valid_ || !::IsWindowReady()) {
        return;
    }
    EndTextureMode();
}

} // namespace TacticalAI::Engine
