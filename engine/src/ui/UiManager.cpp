#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <imgui/imgui.h>

#include <myengine/core/Logger.h>
#include <myengine/core/ServiceLocator.h>
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

        ImGuiKey VirtualKeyToImGuiKey(const WPARAM wparam)
        {
            switch (wparam)
            {
                case VK_TAB: return ImGuiKey_Tab;
                case VK_LEFT: return ImGuiKey_LeftArrow;
                case VK_RIGHT: return ImGuiKey_RightArrow;
                case VK_UP: return ImGuiKey_UpArrow;
                case VK_DOWN: return ImGuiKey_DownArrow;
                case VK_PRIOR: return ImGuiKey_PageUp;
                case VK_NEXT: return ImGuiKey_PageDown;
                case VK_HOME: return ImGuiKey_Home;
                case VK_END: return ImGuiKey_End;
                case VK_INSERT: return ImGuiKey_Insert;
                case VK_DELETE: return ImGuiKey_Delete;
                case VK_BACK: return ImGuiKey_Backspace;
                case VK_SPACE: return ImGuiKey_Space;
                case VK_RETURN: return ImGuiKey_Enter;
                case VK_ESCAPE: return ImGuiKey_Escape;
                case VK_OEM_7: return ImGuiKey_Apostrophe;
                case VK_OEM_COMMA: return ImGuiKey_Comma;
                case VK_OEM_MINUS: return ImGuiKey_Minus;
                case VK_OEM_PERIOD: return ImGuiKey_Period;
                case VK_OEM_2: return ImGuiKey_Slash;
                case VK_OEM_1: return ImGuiKey_Semicolon;
                case VK_OEM_PLUS: return ImGuiKey_Equal;
                case VK_OEM_4: return ImGuiKey_LeftBracket;
                case VK_OEM_5: return ImGuiKey_Backslash;
                case VK_OEM_6: return ImGuiKey_RightBracket;
                case VK_OEM_3: return ImGuiKey_GraveAccent;
                case VK_CAPITAL: return ImGuiKey_CapsLock;
                case VK_SCROLL: return ImGuiKey_ScrollLock;
                case VK_NUMLOCK: return ImGuiKey_NumLock;
                case VK_SNAPSHOT: return ImGuiKey_PrintScreen;
                case VK_PAUSE: return ImGuiKey_Pause;
                case VK_NUMPAD0: return ImGuiKey_Keypad0;
                case VK_NUMPAD1: return ImGuiKey_Keypad1;
                case VK_NUMPAD2: return ImGuiKey_Keypad2;
                case VK_NUMPAD3: return ImGuiKey_Keypad3;
                case VK_NUMPAD4: return ImGuiKey_Keypad4;
                case VK_NUMPAD5: return ImGuiKey_Keypad5;
                case VK_NUMPAD6: return ImGuiKey_Keypad6;
                case VK_NUMPAD7: return ImGuiKey_Keypad7;
                case VK_NUMPAD8: return ImGuiKey_Keypad8;
                case VK_NUMPAD9: return ImGuiKey_Keypad9;
                case VK_DECIMAL: return ImGuiKey_KeypadDecimal;
                case VK_DIVIDE: return ImGuiKey_KeypadDivide;
                case VK_MULTIPLY: return ImGuiKey_KeypadMultiply;
                case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
                case VK_ADD: return ImGuiKey_KeypadAdd;
                case VK_LSHIFT: return ImGuiKey_LeftShift;
                case VK_LCONTROL: return ImGuiKey_LeftCtrl;
                case VK_LMENU: return ImGuiKey_LeftAlt;
                case VK_LWIN: return ImGuiKey_LeftSuper;
                case VK_RSHIFT: return ImGuiKey_RightShift;
                case VK_RCONTROL: return ImGuiKey_RightCtrl;
                case VK_RMENU: return ImGuiKey_RightAlt;
                case VK_RWIN: return ImGuiKey_RightSuper;
                case VK_APPS: return ImGuiKey_Menu;
                case '0': return ImGuiKey_0;
                case '1': return ImGuiKey_1;
                case '2': return ImGuiKey_2;
                case '3': return ImGuiKey_3;
                case '4': return ImGuiKey_4;
                case '5': return ImGuiKey_5;
                case '6': return ImGuiKey_6;
                case '7': return ImGuiKey_7;
                case '8': return ImGuiKey_8;
                case '9': return ImGuiKey_9;
                case 'A': return ImGuiKey_A;
                case 'B': return ImGuiKey_B;
                case 'C': return ImGuiKey_C;
                case 'D': return ImGuiKey_D;
                case 'E': return ImGuiKey_E;
                case 'F': return ImGuiKey_F;
                case 'G': return ImGuiKey_G;
                case 'H': return ImGuiKey_H;
                case 'I': return ImGuiKey_I;
                case 'J': return ImGuiKey_J;
                case 'K': return ImGuiKey_K;
                case 'L': return ImGuiKey_L;
                case 'M': return ImGuiKey_M;
                case 'N': return ImGuiKey_N;
                case 'O': return ImGuiKey_O;
                case 'P': return ImGuiKey_P;
                case 'Q': return ImGuiKey_Q;
                case 'R': return ImGuiKey_R;
                case 'S': return ImGuiKey_S;
                case 'T': return ImGuiKey_T;
                case 'U': return ImGuiKey_U;
                case 'V': return ImGuiKey_V;
                case 'W': return ImGuiKey_W;
                case 'X': return ImGuiKey_X;
                case 'Y': return ImGuiKey_Y;
                case 'Z': return ImGuiKey_Z;
                case VK_F1: return ImGuiKey_F1;
                case VK_F2: return ImGuiKey_F2;
                case VK_F3: return ImGuiKey_F3;
                case VK_F4: return ImGuiKey_F4;
                case VK_F5: return ImGuiKey_F5;
                case VK_F6: return ImGuiKey_F6;
                case VK_F7: return ImGuiKey_F7;
                case VK_F8: return ImGuiKey_F8;
                case VK_F9: return ImGuiKey_F9;
                case VK_F10: return ImGuiKey_F10;
                case VK_F11: return ImGuiKey_F11;
                case VK_F12: return ImGuiKey_F12;
                default: return ImGuiKey_None;
            }
        }

        void UpdateKeyModifiers(ImGuiIO& io)
        {
            io.AddKeyEvent(ImGuiMod_Ctrl, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (GetKeyState(VK_MENU) & 0x8000) != 0);
            io.AddKeyEvent(ImGuiMod_Super, (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0);
        }

        void ConfigureStyle()
        {
            ImGuiStyle& style = ImGui::GetStyle();
            style.WindowRounding = 8.0f;
            style.ChildRounding = 6.0f;
            style.FrameRounding = 5.0f;
            style.GrabRounding = 5.0f;
            style.PopupRounding = 6.0f;
            style.TabRounding = 5.0f;
            style.ScrollbarRounding = 8.0f;
            style.WindowPadding = ImVec2(12.0f, 10.0f);
            style.FramePadding = ImVec2(8.0f, 6.0f);
            style.ItemSpacing = ImVec2(8.0f, 6.0f);
            style.ItemInnerSpacing = ImVec2(6.0f, 5.0f);
            style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.11f, 0.96f);
            style.Colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.11f, 0.14f, 0.98f);
            style.Colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.17f, 0.20f, 1.0f);
            style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.26f, 0.31f, 1.0f);
            style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.27f, 0.33f, 0.39f, 1.0f);
            style.Colors[ImGuiCol_Header] = ImVec4(0.21f, 0.27f, 0.33f, 1.0f);
            style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.27f, 0.35f, 0.42f, 1.0f);
            style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.31f, 0.41f, 0.50f, 1.0f);
            style.Colors[ImGuiCol_Button] = ImVec4(0.22f, 0.28f, 0.34f, 1.0f);
            style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.29f, 0.37f, 0.45f, 1.0f);
            style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.35f, 0.44f, 0.53f, 1.0f);
            style.Colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.18f, 0.22f, 1.0f);
            style.Colors[ImGuiCol_TabHovered] = ImVec4(0.22f, 0.28f, 0.34f, 1.0f);
            style.Colors[ImGuiCol_TabSelected] = ImVec4(0.24f, 0.30f, 0.37f, 1.0f);
            style.Colors[ImGuiCol_TitleBg] = ImVec4(0.07f, 0.08f, 0.10f, 1.0f);
            style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.12f, 0.15f, 1.0f);
            style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.08f, 0.09f, 0.11f, 1.0f);
            style.Colors[ImGuiCol_Separator] = ImVec4(0.25f, 0.30f, 0.36f, 1.0f);
            style.Colors[ImGuiCol_CheckMark] = ImVec4(0.90f, 0.67f, 0.26f, 1.0f);
            style.Colors[ImGuiCol_DockingPreview] = ImVec4(0.90f, 0.67f, 0.26f, 0.65f);
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
    };

    UiManager::UiManager() = default;

    UiManager::~UiManager()
    {
        Shutdown();
    }

    bool UiManager::Initialize(render::IRenderAdapter& renderAdapter, core::Logger& logger, SceneEditorServices services)
    {
        if (initialized_)
        {
            return true;
        }

        IMGUI_CHECKVERSION();
        renderAdapter_ = &renderAdapter;
        logger_ = &logger;
        sceneEditor_.Initialize(std::move(services));
        initialized_ = true;
        logger_->Info("UiManager: initialized");
        return true;
    }

    void UiManager::Shutdown()
    {
        if (!initialized_)
        {
            return;
        }

        sceneEditor_.Shutdown();

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

    void UiManager::HandleWindowMessage(
        const core::WindowId windowId,
        const HWND hwnd,
        const UINT message,
        const WPARAM wparam,
        const LPARAM lparam)
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
        UpdateKeyModifiers(io);

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
                io.AddMousePosEvent(
                    static_cast<float>(static_cast<short>(LOWORD(lparam))),
                    static_cast<float>(static_cast<short>(HIWORD(lparam))));
                break;

            case WM_MOUSELEAVE:
                io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                break;

            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                break;

            case WM_LBUTTONUP:
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                break;

            case WM_RBUTTONDOWN:
            case WM_RBUTTONDBLCLK:
                io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
                break;

            case WM_RBUTTONUP:
                io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
                break;

            case WM_MBUTTONDOWN:
            case WM_MBUTTONDBLCLK:
                io.AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
                break;

            case WM_MBUTTONUP:
                io.AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
                break;

            case WM_MOUSEWHEEL:
                io.AddMouseWheelEvent(0.0f, static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / static_cast<float>(WHEEL_DELTA));
                break;

            case WM_MOUSEHWHEEL:
                io.AddMouseWheelEvent(
                    -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / static_cast<float>(WHEEL_DELTA),
                    0.0f);
                break;

            case WM_SETFOCUS:
                io.AddFocusEvent(true);
                break;

            case WM_KILLFOCUS:
                io.AddFocusEvent(false);
                break;

            case WM_CHAR:
                io.AddInputCharacterUTF16(static_cast<unsigned short>(wparam));
                break;

            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYUP:
            {
                const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
                const ImGuiKey key = VirtualKeyToImGuiKey(wparam);
                if (key != ImGuiKey_None)
                {
                    io.AddKeyEvent(key, down);
                }
                UpdateKeyModifiers(io);
                break;
            }

            default:
                break;
        }
    }

    void UiManager::Update(const float deltaTime)
    {
        if (!initialized_)
        {
            return;
        }

        for (auto& [windowId, windowContextPtr] : windows_)
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

            BuildWindowUi(windowContext);

            const auto* editorWindowState = core::ServiceLocator::GetEditorRuntimeState().FindWindowState(windowId);
            const bool gizmoCapture = editorWindowState != nullptr && (editorWindowState->gizmoHovered || editorWindowState->gizmoActive);
            windowContext.wantMouseCapture = io.WantCaptureMouse || gizmoCapture;
            windowContext.wantKeyboardCapture = io.WantCaptureKeyboard || io.WantTextInput;
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

    bool UiManager::CanStartSceneNavigation(const core::WindowId windowId, const float mouseX, const float mouseY) const
    {
        return sceneEditor_.CanStartSceneNavigation(windowId, mouseX, mouseY);
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
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
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

    void UiManager::BuildWindowUi(WindowUiContext& windowContext)
    {
        SceneEditorWindowContext context;
        context.windowId = windowContext.windowId;
        context.width = windowContext.width;
        context.height = windowContext.height;
        context.monoFont = windowContext.monoFont;
        context.stateLabel = stateLabel_;
        sceneEditor_.BuildWindowUi(context);
    }
}