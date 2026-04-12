#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <imgui/imgui.h>

#include <myengine/core/Logger.h>
#include <myengine/core/ServiceLocator.h>
#include <myengine/physics/PhysicsWorldState.h>
#include <myengine/ui/UiManager.h>

namespace myengine::ui
{
    namespace
    {
        ImTextureID RenderTextureToImGuiTextureId(const render::TextureHandle texture)
        {
            return static_cast<ImTextureID>(static_cast<ImU64>(texture.value));
        }

        render::TextureHandle ImGuiTextureIdToRenderTexture(const ImTextureID textureId)
        {
            return render::TextureHandle{static_cast<std::uint32_t>(static_cast<ImU64>(textureId))};
        }

        void ConfigureStyle()
        {
            ImGuiStyle& style = ImGui::GetStyle();
            style.WindowRounding = 14.0f;
            style.ChildRounding = 12.0f;
            style.FrameRounding = 10.0f;
            style.PopupRounding = 10.0f;
            style.GrabRounding = 10.0f;
            style.ScrollbarRounding = 12.0f;
            style.TabRounding = 10.0f;
            style.WindowPadding = ImVec2(16.0f, 16.0f);
            style.FramePadding = ImVec2(10.0f, 8.0f);
            style.ItemSpacing = ImVec2(8.0f, 8.0f);
            style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
            style.CellPadding = ImVec2(8.0f, 8.0f);
            style.ScrollbarSize = 14.0f;
            style.IndentSpacing = 16.0f;
            style.SeparatorTextBorderSize = 1.0f;
            style.SeparatorTextPadding = ImVec2(0.0f, 4.0f);
            style.Colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.09f, 0.15f, 0.95f);
            style.Colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.14f, 0.22f, 0.82f);
            style.Colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.13f, 0.21f, 0.98f);
            style.Colors[ImGuiCol_Border] = ImVec4(0.26f, 0.41f, 0.56f, 0.65f);
            style.Colors[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.21f, 0.33f, 0.95f);
            style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.30f, 0.45f, 1.0f);
            style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.19f, 0.36f, 0.53f, 1.0f);
            style.Colors[ImGuiCol_Button] = ImVec4(0.13f, 0.28f, 0.42f, 1.0f);
            style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.17f, 0.36f, 0.54f, 1.0f);
            style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.22f, 0.45f, 0.64f, 1.0f);
            style.Colors[ImGuiCol_Header] = ImVec4(0.11f, 0.24f, 0.36f, 0.95f);
            style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.15f, 0.31f, 0.47f, 1.0f);
            style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.19f, 0.39f, 0.58f, 1.0f);
            style.Colors[ImGuiCol_TitleBg] = ImVec4(0.05f, 0.09f, 0.15f, 1.0f);
            style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.07f, 0.12f, 0.19f, 1.0f);
            style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.03f, 0.06f, 0.10f, 0.65f);
            style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.16f, 0.30f, 0.44f, 1.0f);
            style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.20f, 0.38f, 0.55f, 1.0f);
            style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.24f, 0.45f, 0.63f, 1.0f);
            style.Colors[ImGuiCol_CheckMark] = ImVec4(0.47f, 0.84f, 0.91f, 1.0f);
            style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.28f, 0.51f, 0.70f, 0.35f);
        }

        void DrawMetricCard(const char* label, const std::string& value)
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.14f, 0.22f, 0.92f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
            ImGui::BeginChild(label, ImVec2(0.0f, 62.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::TextDisabled("%s", label);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f);
            ImGui::Text("%s", value.c_str());
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        void DrawControlRow(const char* key, const char* description)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.04f, 0.09f, 0.14f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.04f, 0.09f, 0.14f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.04f, 0.09f, 0.14f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
            ImGui::Button(key, ImVec2(-FLT_MIN, 0.0f));
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", description);
        }
    }

    struct UiManager::WindowUiContext
    {
        core::WindowId windowId = 0;
        HWND hwnd = nullptr;
        render::RenderSurfaceHandle surface;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        ImGuiContext* imguiContext = nullptr;
        ImFont* bodyFont = nullptr;
        ImFont* monoFont = nullptr;
        render::TextureHandle fontTexture{};
        std::vector<render::UiVertex> scratchVertices;
        std::vector<std::uint32_t> scratchIndices;
        std::vector<std::uint32_t> scratchCommandIndices;
        bool wantMouseCapture = false;
        bool wantKeyboardCapture = false;

        ~WindowUiContext() = default;
    };

    UiManager::UiManager() = default;

    UiManager::~UiManager()
    {
        Shutdown();
    }

    bool UiManager::Initialize(render::IRenderAdapter& renderAdapter, core::Logger& logger, UiCallbacks callbacks)
    {
        if (initialized_)
        {
            return true;
        }

        IMGUI_CHECKVERSION();
        renderAdapter_ = &renderAdapter;
        logger_ = &logger;
        callbacks_ = std::move(callbacks);
        initialized_ = true;
        logger_->Info("UiManager: ImGui initialized");
        return true;
    }

    void UiManager::Shutdown()
    {
        if (!initialized_)
        {
            return;
        }

        for (auto& [_, windowContext] : windows_)
        {
            if (windowContext == nullptr)
            {
                continue;
            }

            if (renderAdapter_ != nullptr && windowContext->fontTexture.IsValid())
            {
                renderAdapter_->DestroyTexture(windowContext->fontTexture);
                windowContext->fontTexture = {};
            }

            if (windowContext->imguiContext != nullptr)
            {
                ImGui::SetCurrentContext(windowContext->imguiContext);
                ImGui::DestroyContext(windowContext->imguiContext);
                windowContext->imguiContext = nullptr;
            }
        }

        ImGui::SetCurrentContext(nullptr);
        windows_.clear();
        renderAdapter_ = nullptr;
        logger_ = nullptr;
        callbacks_ = {};
        initialized_ = false;
    }

    bool UiManager::RegisterWindow(
        const core::WindowId windowId,
        const HWND hwnd,
        const render::RenderSurfaceHandle surface,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (!initialized_)
        {
            return false;
        }

        auto windowContext = CreateWindowContext(windowId, hwnd, surface, width, height);
        if (!windowContext)
        {
            return false;
        }

        windows_[windowId] = std::move(windowContext);
        return true;
    }

    void UiManager::UnregisterWindow(const core::WindowId windowId)
    {
        const auto it = windows_.find(windowId);
        if (it == windows_.end())
        {
            return;
        }

        if (it->second != nullptr)
        {
            if (renderAdapter_ != nullptr && it->second->fontTexture.IsValid())
            {
                renderAdapter_->DestroyTexture(it->second->fontTexture);
            }

            if (it->second->imguiContext != nullptr)
            {
                ImGui::SetCurrentContext(it->second->imguiContext);
                ImGui::DestroyContext(it->second->imguiContext);
            }
        }

        windows_.erase(it);
        ImGui::SetCurrentContext(nullptr);
    }

    void UiManager::HandleWindowMessage(const core::WindowId windowId, const HWND hwnd, const UINT message, const WPARAM wparam, const LPARAM lparam)
    {
        if (!initialized_)
        {
            return;
        }

        (void)hwnd;

        const auto it = windows_.find(windowId);
        if (it == windows_.end() || it->second == nullptr || it->second->imguiContext == nullptr)
        {
            return;
        }

        auto& windowContext = *it->second;
        ImGui::SetCurrentContext(windowContext.imguiContext);
        ImGuiIO& io = ImGui::GetIO();

        switch (message)
        {
            case WM_SIZE:
            {
                const auto newWidth = static_cast<std::uint32_t>(LOWORD(lparam));
                const auto newHeight = static_cast<std::uint32_t>(HIWORD(lparam));
                if (newWidth > 0 && newHeight > 0)
                {
                    windowContext.width = newWidth;
                    windowContext.height = newHeight;
                }
                break;
            }

            case WM_MOUSEMOVE:
            {
                io.AddMousePosEvent(
                    static_cast<float>(static_cast<short>(LOWORD(lparam))),
                    static_cast<float>(static_cast<short>(HIWORD(lparam))));
                break;
            }

            case WM_MOUSELEAVE:
            {
                io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                break;
            }

            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
            {
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                break;
            }

            case WM_LBUTTONUP:
            {
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                break;
            }

            case WM_MOUSEWHEEL:
            {
                io.AddMouseWheelEvent(0.0f, static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / static_cast<float>(WHEEL_DELTA));
                break;
            }

            case WM_MOUSEHWHEEL:
            {
                io.AddMouseWheelEvent(
                    -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / static_cast<float>(WHEEL_DELTA),
                    0.0f);
                break;
            }

            case WM_SETFOCUS:
            {
                io.AddFocusEvent(true);
                break;
            }

            case WM_KILLFOCUS:
            {
                io.AddFocusEvent(false);
                break;
            }

            default:
            {
                break;
            }
        }
    }

    void UiManager::Update(const float deltaTime)
    {
        if (!initialized_)
        {
            return;
        }

        for (auto& [_, windowContextPtr] : windows_)
        {
            if (windowContextPtr == nullptr || windowContextPtr->imguiContext == nullptr)
            {
                continue;
            }

            auto& windowContext = *windowContextPtr;
            ImGui::SetCurrentContext(windowContext.imguiContext);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(
                static_cast<float>(std::max<std::uint32_t>(windowContext.width, 1u)),
                static_cast<float>(std::max<std::uint32_t>(windowContext.height, 1u)));
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
            io.DeltaTime = std::max(deltaTime, 1.0f / 600.0f);
            ImGui::NewFrame();
            BuildWindowUi(windowContext, deltaTime);
            windowContext.wantMouseCapture = io.WantCaptureMouse;
            windowContext.wantKeyboardCapture = false;
            ImGui::Render();
        }
    }

    void UiManager::RenderWindow(const core::WindowId windowId)
    {
        if (!initialized_ || renderAdapter_ == nullptr)
        {
            return;
        }

        const auto it = windows_.find(windowId);
        if (it == windows_.end() || it->second == nullptr || it->second->imguiContext == nullptr)
        {
            return;
        }

        auto& windowContext = *it->second;
        ImGui::SetCurrentContext(windowContext.imguiContext);
        ImDrawData* drawData = ImGui::GetDrawData();
        if (drawData == nullptr ||
            drawData->CmdListsCount == 0 ||
            drawData->TotalVtxCount <= 0 ||
            drawData->TotalIdxCount <= 0 ||
            drawData->DisplaySize.x <= 0.0f ||
            drawData->DisplaySize.y <= 0.0f)
        {
            return;
        }

        const ImVec2 clipOff = drawData->DisplayPos;
        const ImVec2 clipScale = drawData->FramebufferScale;

        for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex)
        {
            const ImDrawList* drawList = drawData->CmdLists[listIndex];
            if (drawList == nullptr)
            {
                continue;
            }

            windowContext.scratchVertices.resize(static_cast<std::size_t>(drawList->VtxBuffer.Size));
            for (int vertexIndex = 0; vertexIndex < drawList->VtxBuffer.Size; ++vertexIndex)
            {
                const ImDrawVert& sourceVertex = drawList->VtxBuffer[vertexIndex];
                render::UiVertex& destinationVertex = windowContext.scratchVertices[static_cast<std::size_t>(vertexIndex)];
                destinationVertex.position = {sourceVertex.pos.x, sourceVertex.pos.y};
                destinationVertex.color = {
                    static_cast<std::uint8_t>((sourceVertex.col >> IM_COL32_R_SHIFT) & 0xff),
                    static_cast<std::uint8_t>((sourceVertex.col >> IM_COL32_G_SHIFT) & 0xff),
                    static_cast<std::uint8_t>((sourceVertex.col >> IM_COL32_B_SHIFT) & 0xff),
                    static_cast<std::uint8_t>((sourceVertex.col >> IM_COL32_A_SHIFT) & 0xff),
                };
                destinationVertex.uv = {sourceVertex.uv.x, sourceVertex.uv.y};
            }

            windowContext.scratchIndices.resize(static_cast<std::size_t>(drawList->IdxBuffer.Size));
            for (int index = 0; index < drawList->IdxBuffer.Size; ++index)
            {
                windowContext.scratchIndices[static_cast<std::size_t>(index)] = static_cast<std::uint32_t>(drawList->IdxBuffer[index]);
            }

            for (int cmdIndex = 0; cmdIndex < drawList->CmdBuffer.Size; ++cmdIndex)
            {
                const ImDrawCmd& command = drawList->CmdBuffer[cmdIndex];
                if (command.UserCallback != nullptr)
                {
                    if (command.UserCallback != ImDrawCallback_ResetRenderState)
                    {
                        command.UserCallback(drawList, &command);
                    }
                    continue;
                }

                ImVec2 clipMin((command.ClipRect.x - clipOff.x) * clipScale.x, (command.ClipRect.y - clipOff.y) * clipScale.y);
                ImVec2 clipMax((command.ClipRect.z - clipOff.x) * clipScale.x, (command.ClipRect.w - clipOff.y) * clipScale.y);
                if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y)
                {
                    continue;
                }

                const std::size_t baseVertex = static_cast<std::size_t>(command.VtxOffset);
                const std::size_t firstIndex = static_cast<std::size_t>(command.IdxOffset);
                const std::size_t elementCount = static_cast<std::size_t>(command.ElemCount);
                if (baseVertex >= windowContext.scratchVertices.size() ||
                    firstIndex + elementCount > windowContext.scratchIndices.size())
                {
                    continue;
                }

                windowContext.scratchCommandIndices.resize(elementCount);
                std::uint32_t maxReferencedIndex = 0;
                bool validCommand = true;
                for (std::size_t elementIndex = 0; elementIndex < elementCount; ++elementIndex)
                {
                    const std::uint32_t sourceIndex = windowContext.scratchIndices[firstIndex + elementIndex];
                    if (sourceIndex < baseVertex)
                    {
                        validCommand = false;
                        break;
                    }

                    const std::uint32_t rebasedIndex = sourceIndex - static_cast<std::uint32_t>(baseVertex);
                    windowContext.scratchCommandIndices[elementIndex] = rebasedIndex;
                    maxReferencedIndex = std::max(maxReferencedIndex, rebasedIndex);
                }

                if (!validCommand)
                {
                    continue;
                }

                const std::size_t commandVertexCount = static_cast<std::size_t>(maxReferencedIndex) + 1u;
                if (baseVertex + commandVertexCount > windowContext.scratchVertices.size())
                {
                    continue;
                }

                const int scissorLeft = std::clamp(static_cast<int>(clipMin.x), 0, static_cast<int>(windowContext.width));
                const int scissorTop = std::clamp(static_cast<int>(clipMin.y), 0, static_cast<int>(windowContext.height));
                const int scissorRight = std::clamp(static_cast<int>(clipMax.x), 0, static_cast<int>(windowContext.width));
                const int scissorBottom = std::clamp(static_cast<int>(clipMax.y), 0, static_cast<int>(windowContext.height));
                if (scissorRight <= scissorLeft || scissorBottom <= scissorTop)
                {
                    continue;
                }

                render::UiDrawData uiDrawData;
                uiDrawData.vertices = windowContext.scratchVertices.data() + baseVertex;
                uiDrawData.vertexCount = commandVertexCount;
                uiDrawData.indices = windowContext.scratchCommandIndices.data();
                uiDrawData.indexCount = elementCount;
                uiDrawData.texture = ImGuiTextureIdToRenderTexture(command.GetTexID());
                uiDrawData.translationX = -clipOff.x;
                uiDrawData.translationY = -clipOff.y;
                uiDrawData.scissorEnabled = true;
                uiDrawData.scissor = {
                    scissorLeft,
                    scissorTop,
                    scissorRight,
                    scissorBottom,
                };

                renderAdapter_->DrawUiGeometry(windowContext.surface, uiDrawData);
            }
        }
    }

    void UiManager::SetStateLabel(std::string stateLabel)
    {
        stateLabel_ = std::move(stateLabel);
    }

    bool UiManager::WantsMouseCapture(const core::WindowId windowId) const
    {
        const auto it = windows_.find(windowId);
        return it != windows_.end() && it->second != nullptr ? it->second->wantMouseCapture : false;
    }

    bool UiManager::WantsKeyboardCapture(const core::WindowId windowId) const
    {
        const auto it = windows_.find(windowId);
        return it != windows_.end() && it->second != nullptr ? it->second->wantKeyboardCapture : false;
    }

    std::unique_ptr<UiManager::WindowUiContext> UiManager::CreateWindowContext(
        const core::WindowId windowId,
        const HWND hwnd,
        const render::RenderSurfaceHandle surface,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        auto windowContext = std::make_unique<WindowUiContext>();
        windowContext->windowId = windowId;
        windowContext->hwnd = hwnd;
        windowContext->surface = surface;
        windowContext->width = width;
        windowContext->height = height;

        windowContext->imguiContext = ImGui::CreateContext();
        if (windowContext->imguiContext == nullptr)
        {
            if (logger_ != nullptr)
            {
                logger_->Error("UiManager: failed to create ImGui context for window " + std::to_string(windowId));
            }
            return {};
        }

        ImGui::SetCurrentContext(windowContext->imguiContext);
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        io.ConfigWindowsMoveFromTitleBarOnly = true;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;

        ConfigureStyle();

        const ImWchar* glyphRanges = io.Fonts->GetGlyphRangesCyrillic();
        const auto fontRoot = std::filesystem::path(MYENGINE_SOURCE_DIR) / "assets/fonts";
        const auto interRegularPath = (fontRoot / "Inter-Regular.ttf").string();
        const auto jetBrainsMonoPath = (fontRoot / "JetBrainsMono-Regular.ttf").string();

        windowContext->bodyFont = io.Fonts->AddFontFromFileTTF(interRegularPath.c_str(), 17.0f, nullptr, glyphRanges);
        windowContext->monoFont = io.Fonts->AddFontFromFileTTF(jetBrainsMonoPath.c_str(), 15.0f, nullptr, glyphRanges);
        if (windowContext->bodyFont != nullptr)
        {
            io.FontDefault = windowContext->bodyFont;
        }
        if (windowContext->monoFont == nullptr)
        {
            windowContext->monoFont = io.FontDefault;
        }

        unsigned char* fontPixels = nullptr;
        int fontWidth = 0;
        int fontHeight = 0;
        io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);
        if (fontPixels == nullptr || fontWidth <= 0 || fontHeight <= 0)
        {
            if (logger_ != nullptr)
            {
                logger_->Error("UiManager: failed to build ImGui font atlas for window " + std::to_string(windowId));
            }
            ImGui::DestroyContext(windowContext->imguiContext);
            windowContext->imguiContext = nullptr;
            return {};
        }

        render::TextureData fontTextureData;
        fontTextureData.width = static_cast<std::uint32_t>(fontWidth);
        fontTextureData.height = static_cast<std::uint32_t>(fontHeight);
        fontTextureData.channels = 4;
        fontTextureData.srgb = false;
        fontTextureData.pixelsRgba8.assign(fontPixels, fontPixels + static_cast<std::size_t>(fontWidth * fontHeight * 4));
        windowContext->fontTexture = renderAdapter_->CreateTexture(fontTextureData);
        if (!windowContext->fontTexture.IsValid())
        {
            if (logger_ != nullptr)
            {
                logger_->Error("UiManager: failed to create ImGui font texture for window " + std::to_string(windowId));
            }
            ImGui::DestroyContext(windowContext->imguiContext);
            windowContext->imguiContext = nullptr;
            return {};
        }

        io.Fonts->SetTexID(RenderTextureToImGuiTextureId(windowContext->fontTexture));
        io.Fonts->ClearTexData();

        return windowContext;
    }

    void UiManager::BuildWindowUi(WindowUiContext& windowContext, const float deltaTime) const
    {
        const auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();
        const float fps = deltaTime > 0.0001f ? 1.0f / deltaTime : 0.0f;

        std::ostringstream gravityStream;
        gravityStream.setf(std::ios::fixed);
        gravityStream.precision(2);
        gravityStream << physicsState.gravityStrength;

        const float margin = 18.0f;
        const float viewportWidth = static_cast<float>(std::max<std::uint32_t>(windowContext.width, 480u));
        const float viewportHeight = static_cast<float>(std::max<std::uint32_t>(windowContext.height, 360u));
        const float panelWidth = std::clamp(viewportWidth * 0.27f, 320.0f, 380.0f);
        const float eventPanelHeight = std::min(200.0f, std::max(132.0f, viewportHeight * 0.22f));
        const float controlPanelHeight = std::max(320.0f, viewportHeight - eventPanelHeight - margin * 3.0f);

        const ImGuiWindowFlags panelFlags =
            ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings;

        ImGui::SetNextWindowPos(ImVec2(margin, margin), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(panelWidth, controlPanelHeight), ImGuiCond_Always);
        if (ImGui::Begin("Physics Control Deck", nullptr, panelFlags))
        {
            ImGui::TextColored(ImVec4(0.48f, 0.85f, 0.91f, 1.0f), "MYENGINE PHYSICS");
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.97f, 1.0f, 1.0f));
            ImGui::SetWindowFontScale(1.18f);
            ImGui::TextUnformatted("Control Deck");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();
            ImGui::TextDisabled("%s", stateLabel_.c_str());
            ImGui::Spacing();

            if (ImGui::BeginTable("runtime_metrics", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
            {
                const std::array<std::pair<const char*, std::string>, 6> metrics = {{
                    {"FPS", std::to_string(static_cast<int>(fps + 0.5f))},
                    {"Bodies", std::to_string(physicsState.stats.rigidbodyCount)},
                    {"Collisions", std::to_string(physicsState.stats.collisionPairs)},
                    {"Triggers", std::to_string(physicsState.stats.triggerPairs)},
                    {"Gravity", gravityStream.str()},
                    {"Physics", physicsState.physicsPaused ? "Paused" : "Running"},
                }};

                for (const auto& [label, value] : metrics)
                {
                    ImGui::TableNextColumn();
                    DrawMetricCard(label, value);
                }
                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::SeparatorText("Spawn");
            if (ImGui::Button("Spawn Box", ImVec2(-FLT_MIN, 0.0f)) && callbacks_.spawnBox)
            {
                callbacks_.spawnBox(windowContext.windowId);
            }
            if (ImGui::Button("Spawn Sphere", ImVec2(-FLT_MIN, 0.0f)) && callbacks_.spawnSphere)
            {
                callbacks_.spawnSphere(windowContext.windowId);
            }
            if (ImGui::Button("Spawn Burst", ImVec2(-FLT_MIN, 0.0f)) && callbacks_.spawnBurst)
            {
                callbacks_.spawnBurst(windowContext.windowId);
            }

            ImGui::Spacing();
            ImGui::SeparatorText("Scene");
            if (ImGui::BeginTable("scene_buttons", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
            {
                ImGui::TableNextColumn();
                if (ImGui::Button(physicsState.physicsPaused ? "Resume Physics" : "Pause Physics", ImVec2(-FLT_MIN, 0.0f)) && callbacks_.togglePause)
                {
                    callbacks_.togglePause();
                }

                ImGui::TableNextColumn();
                if (ImGui::Button("Toggle Debug", ImVec2(-FLT_MIN, 0.0f)) && callbacks_.toggleDebugDraw)
                {
                    callbacks_.toggleDebugDraw();
                }

                ImGui::TableNextColumn();
                if (ImGui::Button("Gravity -", ImVec2(-FLT_MIN, 0.0f)) && callbacks_.adjustGravity)
                {
                    callbacks_.adjustGravity(-1.0f);
                }

                ImGui::TableNextColumn();
                if (ImGui::Button("Gravity +", ImVec2(-FLT_MIN, 0.0f)) && callbacks_.adjustGravity)
                {
                    callbacks_.adjustGravity(1.0f);
                }

                ImGui::EndTable();
            }

            if (ImGui::Button("Reset Scene", ImVec2(-FLT_MIN, 0.0f)) && callbacks_.resetScene)
            {
                callbacks_.resetScene();
            }

            ImGui::Spacing();
            ImGui::SeparatorText("Controls");
            if (ImGui::BeginTable("control_hints", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings))
            {
                ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 88.0f);
                ImGui::TableSetupColumn("hint", ImGuiTableColumnFlags_WidthStretch);
                DrawControlRow("WASD", "move the physics box");
                DrawControlRow("SPACE", "apply upward movement");
                DrawControlRow("RMB", "hold for free camera");
                DrawControlRow("F3", "toggle debug draw");
                ImGui::EndTable();
            }
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(margin, margin * 2.0f + controlPanelHeight), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(panelWidth, eventPanelHeight), ImGuiCond_Always);
        if (ImGui::Begin("Physics Events", nullptr, panelFlags))
        {
            ImGui::TextDisabled("latest collision and trigger messages");
            ImGui::Separator();

            if (windowContext.monoFont != nullptr)
            {
                ImGui::PushFont(windowContext.monoFont);
            }

            if (ImGui::BeginChild("event_log_child", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
            {
                if (physicsState.recentEvents.empty())
                {
                    ImGui::TextDisabled("No collision events yet.");
                }
                else
                {
                    for (const auto& eventText : physicsState.recentEvents)
                    {
                        ImGui::TextUnformatted(eventText.c_str());
                    }

                    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                    {
                        ImGui::SetScrollHereY(1.0f);
                    }
                }
            }
            ImGui::EndChild();

            if (windowContext.monoFont != nullptr)
            {
                ImGui::PopFont();
            }
        }
        ImGui::End();
    }
}