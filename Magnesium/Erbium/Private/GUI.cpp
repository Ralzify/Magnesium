#include "pch.h"
#include "../Public/GUI.h"
#include <d3d11.h>
#include "../../ImGui/imgui.h"
#include "../../ImGui/imgui_stdlib.h"
#include "../../ImGui/imgui_impl_win32.h"
#include "../../ImGui/imgui_impl_dx11.h"
#include "../Public/Calendar.h"
#include "../Public/Configuration.h"
#include "../Public/Events.h"
#include "../Public/Misc.h"
#include "../../FortniteGame/Public/BattleRoyaleGamePhaseLogic.h"
#include "../../FortniteGame/Public/BuildingSMActor.h"
#include "../../Engine/Public/NetDriver.h"
#include "../../FortniteGame/Public/FortPhysicsPawn.h"
#include <sstream>
#include <fstream>
#include <string>
#include <Windows.h>
#include <Shellapi.h>
#include <chrono>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "EmbeddedImage.h"
#include "Icon.h" // ATLAS logo (ported menu branding)

#pragma comment(lib, "d3d11.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

UINT g_ResizeWidth = 0, g_ResizeHeight = 0;

ID3D11ShaderResourceView* g_EmbedTexture = nullptr;
int EmbedWidth = 0;
int EmbedHeight = 0;

// Ported ATLAS-style menu logo
ID3D11ShaderResourceView* g_LogoTexture = nullptr;
int g_LogoW = 0, g_LogoH = 0;

// Grey/dark theme accent (highlights, active tab, checkmarks, sliders) - soft cool silver
#define ACCENT_R 0.780f
#define ACCENT_G 0.820f
#define ACCENT_B 0.910f
static ImVec4 Accent(float a = 1.f) { return ImVec4(ACCENT_R, ACCENT_G, ACCENT_B, a); }
static ImVec4 AccentDk(float a = 1.f) { return ImVec4(0.50f, 0.54f, 0.64f, a); } // darker for active

bool LoadTextureFromMemory(const unsigned char* buffer, int buffer_size, ID3D11Device* d3dDevice, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height)
{
    int image_width = 0;
    int image_height = 0;
    unsigned char* image_data = stbi_load_from_memory(buffer, buffer_size, &image_width, &image_height, NULL, 4);

    if (image_data == NULL)
        return false;

    // Full mip chain + GPU-generated mips so large source images downscale smoothly
    // to small on-screen sizes (e.g. the 1024px logo) instead of looking pixelated.
    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = image_width;
    desc.Height = image_height;
    desc.MipLevels = 0; // 0 => allocate a full mip chain
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    desc.CPUAccessFlags = 0;

    ID3D11Texture2D* pTexture = NULL;
    d3dDevice->CreateTexture2D(&desc, NULL, &pTexture);
    if (pTexture == NULL)
    {
        stbi_image_free(image_data);
        return false;
    }

    ID3D11DeviceContext* ctx = NULL;
    d3dDevice->GetImmediateContext(&ctx);
    ctx->UpdateSubresource(pTexture, 0, NULL, image_data, image_width * 4, 0);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    ZeroMemory(&srvDesc, sizeof(srvDesc));
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = (UINT)-1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    d3dDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
    if (*out_srv)
        ctx->GenerateMips(*out_srv);
    if (ctx)
        ctx->Release();
    pTexture->Release();

    *out_width = image_width;
    *out_height = image_height;

    stbi_image_free(image_data);

    return true;
}

void Hyperlink(const char* label, const char* url)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.6f, 1.0f, 1.0f));
    ImGui::Text("%s", label);
    ImGui::PopStyleColor();

    if (ImGui::IsItemHovered())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Would you like to be redirected to this link?");
    }

    if (ImGui::IsItemClicked())
    {
        ShellExecuteA(0, "open", url, 0, 0, SW_SHOWNORMAL);
    }
}

void SmallSeparator(float Width, float Thickness = 1.0f)
{
    ImVec2 Pos = ImGui::GetCursorScreenPos();
    auto* Draw = ImGui::GetWindowDrawList();

    Draw->AddLine(Pos, ImVec2(Pos.x + Width, Pos.y), ImGui::GetColorU32(ImGuiCol_Separator), Thickness);

    ImGui::Dummy(ImVec2(Width, Thickness + 4));
}

// One full-width vertical tab in the left sidebar. Sets *activeUI to uiValue on click.
static void SidebarTab(const char* label, int uiValue, float yPos, float tabH, int* activeUI)
{
    ImGui::PushID(uiValue);
    const bool active = (*activeUI == uiValue);

    ImGui::SetCursorPos(ImVec2(0.f, yPos));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, tabH);

    if (ImGui::InvisibleButton("##tab", size))
        *activeUI = uiValue;
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active)
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(Accent(0.12f)));
    else if (hovered)
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(Accent(0.06f)));

    ImVec4 textCol = active ? Accent() : ImVec4(0.60f, 0.63f, 0.69f, 1.f);
    if (!active && hovered) textCol = ImVec4(0.85f, 0.87f, 0.92f, 1.f);

    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + 26.f, p.y + (size.y - ts.y) * 0.5f), ImGui::GetColorU32(textCol), label);

    ImGui::PopID();
}

static const char* GetSelectedPlaylistModeName()
{
    switch (GUI::SelectedPlaylist)
    {
    case (int)Playlist::Solos:
        return "Solos";
    case (int)Playlist::Duos:
        return "Duos";
    case (int)Playlist::Trios:
        return "Trios";
    case (int)Playlist::Squads:
        return "Squads";
    case (int)Playlist::ZBSolos:
        return "Zero Build Solos";
    case (int)Playlist::ZBDuos:
        return "Zero Build Duos";
    case (int)Playlist::ZBTrios:
        return "Zero Build Trios";
    case (int)Playlist::ZBSquads:
        return "Zero Build Squads";
    case (int)Playlist::Playground:
        return "Playground";
    case (int)Playlist::Creative:
        return "Creative";
    case (int)Playlist::OneShotSolos:
        return "One Shot Solos";
    case (int)Playlist::OneShotDuos:
        return "One Shot Duos";
    case (int)Playlist::OneShotSquads:
        return "One Shot Squads";
    case (int)Playlist::SiphonSolos:
        return "Siphon Solos";
    case (int)Playlist::SiphonDuos:
        return "Siphon Duos";
    case (int)Playlist::SiphonSquads:
        return "Siphon Squads";
    case (int)Playlist::UnvSolos:
        return "Unvaulted Solos";
    case (int)Playlist::UnvDuos:
        return "Unvaulted Duos";
    case (int)Playlist::UnvTrios:
        return "Unvaulted Trios";
    case (int)Playlist::UnvSquads:
        return "Unvaulted Squads";
    case (int)Playlist::SlideSolos:
        return "Slide Solos";
    case (int)Playlist::SlideDuos:
        return "Slide Duos";
    case (int)Playlist::FILSolos:
        return "Floor Is Lava Solos";
    case (int)Playlist::FILDuos:
        return "Floor Is Lava Duos";
    case (int)Playlist::FILSquads:
        return "Floor Is Lava Squads";
    case (int)Playlist::TournamentSolos:
        return "Tournament Solos";
    case (int)Playlist::TournamentDuos:
        return "Tournament Duos";
    case (int)Playlist::TournamentTrios:
        return "Tournament Trios";
    case (int)Playlist::TournamentSquads:
        return "Tournament Squads";
    case (int)Playlist::ArenaSolos:
        return "Arena Solos";
    case (int)Playlist::ArenaDuos:
        return "Arena Duos";
    case (int)Playlist::ArenaTrios:
        return "Arena Trios";
    case (int)Playlist::ArenaSquads:
        return "Arena Squads";
    case (int)Playlist::ArenaZBSolos:
        return "Arena Zero Build Solos";
    case (int)Playlist::ArenaZBDuos:
        return "Arena Zero Build Duos";
    case (int)Playlist::ArenaZBTrios:
        return "Arena Zero Build Trios";
    case (int)Playlist::ArenaZBSquads:
        return "Arena Zero Build Squads";
    case (int)Playlist::Gav:
        return "Gav 1v1 Map";
    case (int)Playlist::Retrac1v1:
        return "Retrac 1v1 Map";
    case (int)Playlist::RetracTurtle:
        return "Retrac Turtle Fights";
    case (int)Playlist::RetracWater:
        return "Retrac Water Map";
    case (int)Playlist::TiltedZW:
        return "Tilted FFA";
    case (int)Playlist::OnlyUp:
        return "Only Up Map";
    case (int)Playlist::Twine1v1:
        return "Twine 1v1 Map";
    case (int)Playlist::Boxfight:
        return "Boxfights";
    case (int)Playlist::Backrooms:
        return "Backrooms Map";
    case (int)Playlist::Event:
        return "Event Playlist";
    case (int)Playlist::Custom:
        return "Custom";
    default:
        return "Unknown";
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

auto WindowWidth = 800;
auto WindowHeight = 600;

inline std::vector<std::pair<AFortPlayerControllerAthena*, UNetConnection*>> AllControllers;

void GUI::Init()
{
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASS wc{};
    wc.lpszClassName = L"ErbiumWC";
    wc.lpfnWndProc = WndProc;
    RegisterClass(&wc);

    wchar_t buffer[67];
    swprintf_s(buffer, VersionInfo.EngineVersion >= 5.0 ? L"Magnesium (FN %.2f, UE %.1f)" : (VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? L"Magnesium (FN %.2f, UE %.2f)" : L"Magnesium (FN %.1f, UE %.2f)"), VersionInfo.FortniteVersion, VersionInfo.EngineVersion);
    auto hWnd = CreateWindow(wc.lpszClassName, buffer, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME, 100, 100, (int)(WindowWidth * main_scale), (int)(WindowHeight * main_scale), nullptr, nullptr, nullptr, nullptr);

    IDXGISwapChain* g_pSwapChain = nullptr;
    ID3D11Device* g_pd3dDevice = nullptr;
    ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;

    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    //createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return;

    LoadTextureFromMemory(embedded_image, sizeof(embedded_image), g_pd3dDevice, &g_EmbedTexture, &EmbedWidth, &EmbedHeight);
    LoadTextureFromMemory(Icon, sizeof(Icon), g_pd3dDevice, &g_LogoTexture, &g_LogoW, &g_LogoH);

    ID3D11RenderTargetView* g_mainRenderTargetView;

    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();

    ShowWindow(hWnd, SW_SHOWDEFAULT);
    UpdateWindow(hWnd);
    DWORD dwMyID = ::GetCurrentThreadId();
    DWORD dwCurID = ::GetWindowThreadProcessId(hWnd, NULL);
    AttachThreadInput(dwCurID, dwMyID, TRUE);
    SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
    SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);
    SetActiveWindow(hWnd);
    AttachThreadInput(dwCurID, dwMyID, FALSE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.IniFilename = NULL;
    //io.DisplaySize = ImGui::GetMainViewport()->Size;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImFontConfig FontConfig;
    FontConfig.FontDataOwnedByAtlas = false;
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF((void*)font, sizeof(font), 17.f, &FontConfig);

    auto& mStyle = ImGui::GetStyle();
    mStyle.WindowRounding = 0.f;
    mStyle.ItemSpacing = ImVec2(20, 6);
    mStyle.ItemInnerSpacing = ImVec2(8, 4);
    mStyle.FrameRounding = 4.5f;
    mStyle.GrabMinSize = 14.0f;
    mStyle.GrabRounding = 16.0f;
    mStyle.ScrollbarSize = 18.0f;
    mStyle.ScrollbarRounding = 16.0f;

    ImGuiStyle& style = mStyle;
    auto C = [](float r, float g, float b, float a = 1.f) { return ImVec4(r, g, b, a); };
    ImVec4* col = style.Colors;

    // Neutral graphite dark/grey theme (slight cool tint), ATLAS-style spread.
    col[ImGuiCol_WindowBg]             = C(0.090f, 0.094f, 0.106f, 1.00f); // graphite background
    col[ImGuiCol_ChildBg]              = C(0.090f, 0.094f, 0.106f, 1.00f);
    col[ImGuiCol_PopupBg]              = C(0.063f, 0.067f, 0.078f, 0.98f); // bar
    col[ImGuiCol_Border]               = C(0.196f, 0.204f, 0.227f, 1.00f);
    col[ImGuiCol_BorderShadow]         = C(0.f, 0.f, 0.f, 0.f);
    col[ImGuiCol_Text]                 = C(0.882f, 0.894f, 0.918f, 1.00f);
    col[ImGuiCol_TextDisabled]         = C(0.435f, 0.451f, 0.490f, 1.00f);
    col[ImGuiCol_TextSelectedBg]       = Accent(0.28f);
    col[ImGuiCol_FrameBg]              = C(0.137f, 0.145f, 0.165f, 1.00f);
    col[ImGuiCol_FrameBgHovered]       = C(0.176f, 0.184f, 0.208f, 1.00f);
    col[ImGuiCol_FrameBgActive]        = C(0.216f, 0.227f, 0.255f, 1.00f);
    col[ImGuiCol_TitleBg]              = C(0.063f, 0.067f, 0.078f, 1.00f);
    col[ImGuiCol_TitleBgActive]        = C(0.063f, 0.067f, 0.078f, 1.00f);
    col[ImGuiCol_TitleBgCollapsed]     = C(0.063f, 0.067f, 0.078f, 0.90f);
    col[ImGuiCol_MenuBarBg]            = C(0.063f, 0.067f, 0.078f, 1.00f);
    col[ImGuiCol_CheckMark]            = Accent();
    col[ImGuiCol_SliderGrab]           = Accent();
    col[ImGuiCol_SliderGrabActive]     = AccentDk();
    col[ImGuiCol_Button]               = C(0.157f, 0.165f, 0.188f, 1.00f);
    col[ImGuiCol_ButtonHovered]        = C(0.216f, 0.227f, 0.255f, 1.00f);
    col[ImGuiCol_ButtonActive]         = C(0.255f, 0.267f, 0.298f, 1.00f);
    col[ImGuiCol_Header]               = Accent(0.14f);
    col[ImGuiCol_HeaderHovered]        = Accent(0.22f);
    col[ImGuiCol_HeaderActive]         = Accent(0.30f);
    col[ImGuiCol_Separator]            = C(0.196f, 0.204f, 0.227f, 1.00f);
    col[ImGuiCol_SeparatorHovered]     = Accent(0.40f);
    col[ImGuiCol_SeparatorActive]      = Accent(0.70f);
    col[ImGuiCol_ScrollbarBg]          = C(0.063f, 0.067f, 0.078f, 0.40f);
    col[ImGuiCol_ScrollbarGrab]        = C(0.196f, 0.204f, 0.227f, 1.00f);
    col[ImGuiCol_ScrollbarGrabHovered] = C(0.255f, 0.267f, 0.298f, 1.00f);
    col[ImGuiCol_ScrollbarGrabActive]  = C(0.318f, 0.333f, 0.369f, 1.00f);
    col[ImGuiCol_Tab]                  = C(0.063f, 0.067f, 0.078f, 1.00f);
    col[ImGuiCol_TabHovered]           = Accent(0.18f);
    col[ImGuiCol_TabSelected]          = C(0.090f, 0.094f, 0.106f, 1.00f);
    col[ImGuiCol_PlotLines]            = Accent();
    col[ImGuiCol_PlotHistogram]        = Accent();
    //ImGui::StyleColorsDark();

    //ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplWin32_Init(hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    bool done = false;
    bool g_SwapChainOccluded = false;

    while (!done)
    {
        MSG msg;

        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }

        if (done)
            break;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            Sleep(10);
            continue;
        }

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            g_mainRenderTargetView->Release();

            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;

            ID3D11Texture2D* pBackBuffer;
            g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
            g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
            pBackBuffer->Release();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (FConfiguration::bAutoStartEvent && !FConfiguration::bEventStarted && FConfiguration::EventStartBaseTime > 0.f)
        {
            float CurrentTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
            if (CurrentTime >= FConfiguration::EventStartBaseTime + FConfiguration::EventStartTime)
            {
                FConfiguration::bEventStarted = true;
                printf("[Events] Auto-starting event at T=%.1f\n", CurrentTime);
                Events::StartEvent();
            }
        }

        main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(WindowWidth * main_scale, WindowHeight * main_scale), ImGuiCond_Always);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::Begin("Magnesium", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();

        const float W = ImGui::GetWindowWidth();
        const float H = ImGui::GetWindowHeight();
        const ImVec2 wp = ImGui::GetWindowPos();
        const float TopBarH = 48.f;
        const float SidebarW = 150.f;

        int SelectedUI = 0;
        int hasEvent = 0;
        if (hasEvent == 0)
        {
            hasEvent = 1;
            for (auto& Event : Events::EventsArray)
            {
                if (Event.EventVersion != VersionInfo.FortniteVersion)
                    continue;
                hasEvent = 2;
            }
        }

        // ---- Top bar (#284a2c): logo + branding ----
        ImGui::GetWindowDrawList()->AddRectFilled(wp, ImVec2(wp.x + W, wp.y + TopBarH),
            ImGui::GetColorU32(ImVec4(0.063f, 0.067f, 0.078f, 1.f)));
        {
            const float LogoSize = 32.f;
            const float PadL = 14.f;
            ImGui::SetCursorPos(ImVec2(PadL, (TopBarH - LogoSize) * 0.5f));
            if (g_LogoTexture)
                ImGui::Image((void*)g_LogoTexture, ImVec2(LogoSize, LogoSize));
            else
                ImGui::Dummy(ImVec2(LogoSize, LogoSize));

            ImGui::SameLine(PadL + LogoSize + 10.f);
            const float TitleY = (TopBarH - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::SetCursorPosY(TitleY);
            ImGui::PushStyleColor(ImGuiCol_Text, Accent());
            ImGui::Text("MAGNESIUM");
            ImGui::PopStyleColor();
            ImGui::SameLine(0.f, 7.f);
            ImGui::SetCursorPosY(TitleY);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.62f, 0.68f, 1.f));
            ImGui::Text("| Gameserver");
            ImGui::PopStyleColor();
            ImGui::SameLine(0.f, 8.f);
            ImGui::SetCursorPosY(TitleY);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.46f, 0.48f, 0.54f, 1.f));
            ImGui::TextUnformatted("v2.0.0");
            ImGui::PopStyleColor();

            // FN / UE versions on the right, aligned to the visible viewport so they
            // never run off the window edge.
            char ver[48];
            snprintf(ver, sizeof(ver), "FN %.2f  \xC2\xB7  UE %.2f", VersionInfo.FortniteVersion, VersionInfo.EngineVersion);
            const float verW = ImGui::CalcTextSize(ver).x;
            const float rightEdge = ImGui::GetIO().DisplaySize.x;
            ImGui::SetCursorPos(ImVec2(rightEdge - verW - 18.f, TitleY));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.54f, 0.56f, 0.62f, 0.92f));
            ImGui::TextUnformatted(ver);
            ImGui::PopStyleColor();
        }

        // divider lines (drawn on top so child backgrounds don't cover them)
        {
            ImDrawList* fdl = ImGui::GetForegroundDrawList();
            const ImU32 lineCol = IM_COL32(50, 52, 58, 255);
            fdl->AddLine(ImVec2(wp.x, wp.y + TopBarH), ImVec2(wp.x + W, wp.y + TopBarH), lineCol, 1.f);
            fdl->AddLine(ImVec2(wp.x + SidebarW, wp.y + TopBarH), ImVec2(wp.x + SidebarW, wp.y + H), lineCol, 1.f);
        }

        // ---- Sidebar (#284a2c): vertical tabs replacing the old tab bar ----
        static int s_ActiveUI = 0;
        ImGui::SetCursorPos(ImVec2(0.f, TopBarH));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.063f, 0.067f, 0.078f, 1.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::BeginChild("##sidebar", ImVec2(SidebarW, H - TopBarH), false, ImGuiWindowFlags_NoScrollbar);
        {
            const float TabH = 38.f;
            const float TabsTop = 12.f;
            float yy = TabsTop;
            float activeY = -1.f;
            const bool inMatch = !FConfiguration::bReadyToStart;

            struct TabDef { const char* label; int ui; bool show; };
            TabDef tabs[] = {
                { "Match",      0, true },
                { "Lategame",   3, inMatch },
                { "Playlist",   1, inMatch },
                { "Creative",   5, inMatch && SelectedPlaylist == static_cast<int>(Playlist::Creative) },
                { "Custom Map", 6, inMatch && FConfiguration::bIsCustomMap },
                { "Player Bot", 4, inMatch },
                { "Players",    2, gsStatus >= Joinable },
                { "Trickshot",  7, FConfiguration::bEnableTrickshotTab },
                { "Credits",    8, true },
            };

            for (auto& t : tabs)
            {
                if (!t.show) continue;
                SidebarTab(t.label, t.ui, yy, TabH, &s_ActiveUI);
                if (s_ActiveUI == t.ui) activeY = yy + TabH * 0.5f;
                yy += TabH;
            }

            if (activeY < 0.f) { s_ActiveUI = 0; activeY = TabsTop + TabH * 0.5f; }

            static float s_IndY = -1.f;
            if (s_IndY < 0.f) s_IndY = activeY;
            float lerp = ImGui::GetIO().DeltaTime * 16.f; if (lerp > 1.f) lerp = 1.f;
            s_IndY += (activeY - s_IndY) * lerp;
            const ImVec2 sbPos = ImGui::GetWindowPos();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(sbPos.x, sbPos.y + s_IndY - 9.f), ImVec2(sbPos.x + 3.f, sbPos.y + s_IndY + 9.f),
                ImGui::GetColorU32(Accent()), 2.f);

            // Primary CTA pinned to the bottom of the sidebar: start the server.
            if (gsStatus == NotReady && !FConfiguration::bReadyToStart)
            {
                const float bMargin = 12.f;
                const float bH = 40.f;
                const float bW = SidebarW - bMargin * 2.f;
                // Anchor to the visible viewport height (the window is taller than the
                // actual client area, so using H would push this off-screen).
                const float visH = ImGui::GetIO().DisplaySize.y;
                ImGui::SetCursorPos(ImVec2(bMargin, (visH - TopBarH) - bH - bMargin));
                const ImVec2 bp = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##startserver", ImVec2(bW, bH)))
                    FConfiguration::bReadyToStart = true;
                const bool bHov = ImGui::IsItemHovered();
                const bool bAct = ImGui::IsItemActive();
                ImDrawList* bdl = ImGui::GetWindowDrawList();
                const ImVec4 fillC = bAct ? ImVec4(0.62f, 0.66f, 0.78f, 1.f)
                                    : (bHov ? ImVec4(0.88f, 0.91f, 0.97f, 1.f) : Accent());
                bdl->AddRectFilled(bp, ImVec2(bp.x + bW, bp.y + bH), ImGui::GetColorU32(fillC), 6.f);

                const char* bLbl = "START SERVER";
                const ImVec2 bts = ImGui::CalcTextSize(bLbl);
                const ImVec2 tpos(bp.x + (bW - bts.x) * 0.5f, bp.y + (bH - bts.y) * 0.5f);
                const ImU32 tcol = IM_COL32(16, 18, 22, 255);
                bdl->AddText(tpos, tcol, bLbl);
                bdl->AddText(ImVec2(tpos.x + 1.f, tpos.y), tcol, bLbl); // faux-bold
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        SelectedUI = s_ActiveUI;

        // ---- Content panel (inset; transparent so the #32703b background shows) ----
        const float ContentPadX = 22.f;
        ImGui::SetCursorPos(ImVec2(SidebarW + ContentPadX, TopBarH + 14.f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::BeginChild("##content", ImVec2((W - SidebarW) - ContentPadX * 2.f, (H - TopBarH) - 26.f), false);
        float Width = 260.0f;
        float Height = 0.0f;

        static char commandBuffer[1024] = { 0 };
        static char playlistBuffer[1024] = { 0 };
        switch (SelectedUI)
        {
        case 0:
        {
            ImGui::Text("Match Information:");
            SmallSeparator(Width);

            ImGui::Text("- Status: ");
            ImGui::SameLine(0.0f, 0.0f);

            ImVec4 Color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // default white

            if (!FConfiguration::bReadyToStart)
            {
                Color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f); // gray
                ImGui::TextColored(Color, "Configuring...");
            }
            else if (gsStatus == NotReady)
            {
                Color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // yellow
                ImGui::TextColored(Color, "Setting up the server...");
            }
            else if (gsStatus == Joinable)
            {
                Color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); // green
                ImGui::TextColored(Color, "Joinable!");
            }
            else if (gsStatus == StartedMatch)
            {
                Color = ImVec4(1.0f, 0.65f, 0.0f, 1.0f); // orange
                ImGui::TextColored(Color, "Match Started.");
            }
            else if (gsStatus == Ended)
            {
                Color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); // red
                ImGui::TextColored(Color, "Match Ended.");
            }
            else
            {
                ImGui::TextColored(Color, "N/A");
            }

            ImGui::Text("- Mode: %s", GetSelectedPlaylistModeName());
            if (FConfiguration::bInfiniteRender)
            {
                ImGui::TextUnformatted("- Infinite Render: ");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Enabled");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextUnformatted(" (only works on the last player to join!)");
            }
            ImGui::Text("- Server Port: %d", FConfiguration::Port);
            ImGui::Text("- Server Tick Rate: %.1f", FConfiguration::MaxTickRate);

            if (gsStatus >= Joinable)
            {
                auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;

                int AliveCount = 0;

                if (GameMode)
                    AliveCount = GameMode->AlivePlayers.Num();

                ImGui::Text("- Players: %d", AliveCount);

                ImGui::Text((std::string("- Uptime: ") + std::to_string((int)floor(UGameplayStatics::GetTimeSeconds(GameMode))) + "s").c_str());

                static std::string LastElimStatusMessage;
                static std::chrono::high_resolution_clock::time_point AddMessageTime;

                if (!FConfiguration::ElimStatusMessage.empty() && FConfiguration::ElimStatusMessage != LastElimStatusMessage)
                {
                    LastElimStatusMessage = FConfiguration::ElimStatusMessage;
                    AddMessageTime = std::chrono::high_resolution_clock::now();
                }

                if (!LastElimStatusMessage.empty() && duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - AddMessageTime).count() < 120)
                {
                    ImVec4 KillerColor = ImVec4(0x4e / 255.f, 0x86 / 255.f, 0xa5 / 255.f, 1.0f);
                    ImVec4 EliminatedColor = ImVec4(0xa5 / 255.f, 0x56 / 255.f, 0x4c / 255.f, 1.0f);
                    ImVec4 WeaponColor = ImVec4(1.0f, 0.84f, 0.0f, 1.0f);

                    ImGui::TextUnformatted("- ");
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextColored(KillerColor, "%s", FConfiguration::ElimKillerName.c_str());
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextUnformatted(" eliminated ");
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextColored(EliminatedColor, "%s", FConfiguration::ElimEliminatedName.c_str());
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextUnformatted(" from ");
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::Text("%sm!", FConfiguration::ElimDistance.c_str());
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextUnformatted(" (");
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextColored(WeaponColor, "%s", FConfiguration::ElimWeaponName.c_str());
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextUnformatted(")");

                    static bool bHasLogged = false;

                    if (!bHasLogged)
                    {
                        printf("- %s eliminated %s from %sm! (%s)\n", FConfiguration::ElimKillerName.c_str(), FConfiguration::ElimEliminatedName.c_str(), FConfiguration::ElimDistance.c_str(), FConfiguration::ElimWeaponName.c_str());
                        bHasLogged = true;
                    }
                }
                else
                {
                    LastElimStatusMessage.clear();
                    FConfiguration::ElimStatusMessage.clear();
                }
            }

            bool bIsGavMap = (SelectedPlaylist == static_cast<int>(Playlist::Gav));
            bool bIsEventPlaylist = (SelectedPlaylist == static_cast<int>(Playlist::Event));
            bool bIsRetrac1v1 = (SelectedPlaylist == static_cast<int>(Playlist::Retrac1v1));
            bool bIsRetracTurtle = (SelectedPlaylist == static_cast<int>(Playlist::RetracTurtle));
            bool bIsCreative = (SelectedPlaylist == static_cast<int>(Playlist::Creative));
            bool bIsOnlyUp = (SelectedPlaylist == static_cast<int>(Playlist::OnlyUp));
            bool bIsTiltedZW = (SelectedPlaylist == static_cast<int>(Playlist::TiltedZW));
            bool bIsTwine = (SelectedPlaylist == static_cast<int>(Playlist::Twine1v1));
            bool bIsBoxfight = (SelectedPlaylist == static_cast<int>(Playlist::Boxfight));
            bool bIsBackrooms = (SelectedPlaylist == static_cast<int>(Playlist::Backrooms));
            bool bShowsOnlyUpPreGameConfig = bIsOnlyUp && gsStatus < Joinable;
            bool bShowsDefaultPreGameConfig = !bIsGavMap && !bIsEventPlaylist && !bIsRetrac1v1 && !bIsRetracTurtle && !bIsCreative && !bIsOnlyUp && !bIsTiltedZW && !bIsTwine && !bIsBoxfight && !bIsBackrooms;
            bool bShowsDefaultMatchSettings = bShowsDefaultPreGameConfig;

            if (gsStatus <= Joinable && (bShowsOnlyUpPreGameConfig || bShowsDefaultPreGameConfig))
            {
                ImGui::Spacing();
                ImGui::Spacing();

                static bool bStartedBus = false;

                if (!bStartedBus)
                {
                    ImGui::Text("Pre-Game Configuration:");
                    SmallSeparator(Width);

                    if (bShowsOnlyUpPreGameConfig)
                    {
                        ImGui::Checkbox("Disable Jump Fatigue", &FConfiguration::bDisableJumpFatigue);
                        ImGui::Checkbox("Player Has Pickaxe", &FConfiguration::bHasPickaxe);
                    }
                    else if (bShowsDefaultPreGameConfig)
                    {
                        if (gsStatus <= Joinable)
                        {
                            if (gsStatus < Joinable)
                            {
                                ImGui::Checkbox("Auto Bus Start", &FConfiguration::bAutoBusStart);

                                static bool bInitializedZone = false;

                                if (!bInitializedZone)
                                {
                                    FConfiguration::LateGameZone = FConfiguration::IsS27() ? 3 : 4;
                                    bInitializedZone = true;
                                }

                                if (VersionInfo.FortniteVersion == 19.20)
                                    FConfiguration::bAutoDump = false;

                                ImGui::Checkbox("Auto Dump Text", &FConfiguration::bAutoDump);

                                ImGui::Checkbox("Use Custom Map", &FConfiguration::bIsCustomMap);

                                ImGui::Checkbox("Enable Trickshot Tab", &FConfiguration::bEnableTrickshotTab);

                                static bool bInitializedConfig = false;

                                if (FConfiguration::bEnableTrickshotTab)
                                {
                                    if (!bInitializedConfig)
                                    {
                                        if (FConfiguration::bLateGame)
                                            FConfiguration::RandomizeKills = true;

                                        FConfiguration::RandomizeLevels = true;
                                        FConfiguration::bDisableJumpFatigue = true;
                                        FConfiguration::bAutoPauseTODM = true;

                                        if (IsArenaPlaylist() || IsTournamentPlaylist())
                                        {
                                            FConfiguration::RandomizeArenaPoints = true;
                                        }

                                        bInitializedConfig = true;
                                    }
                                }

                                if (FConfiguration::bAutoBusStart)
                                {
                                    ImGui::PushItemWidth(Width);
                                    ImGui::SliderFloat("Bus Start Delay", &FConfiguration::BusStartDelay, 0.0f, 300.0f, "%.1f seconds");
                                    ImGui::PopItemWidth();
                                }

                                ImGui::PushItemWidth(Width);
                                ImGui::SliderFloat("Max Tick Rate", &FConfiguration::MaxTickRate, 5.0f, 180.0f, "%.1f seconds");
                                ImGui::PopItemWidth();
                            }

                            ImGui::Spacing();

                            if (gsStatus == Joinable && ImGui::Button("Start Bus Early", ImVec2(Width, Height)))
                            {
                                auto Time = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
                                auto WarmupDuration = 10.f;

                                auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
                                auto GameState = GameMode->GameState;

                                if (GameState->HasWarmupCountdownEndTime())
                                {
                                    GameState->WarmupCountdownStartTime = Time;
                                    GameState->WarmupCountdownEndTime = Time + WarmupDuration;
                                    GameMode->WarmupCountdownDuration = WarmupDuration;
                                    GameMode->WarmupEarlyCountdownDuration = WarmupDuration;
                                }

                                if (VersionInfo.FortniteVersion > 25.20)
                                {
                                    auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());

                                    if (GamePhaseLogic->HasWarmupCountdownEndTime())
                                    {
                                        GamePhaseLogic->WarmupCountdownStartTime = Time;
                                        GamePhaseLogic->WarmupCountdownEndTime = Time + WarmupDuration;
                                        GamePhaseLogic->WarmupCountdownDuration = WarmupDuration;
                                        GamePhaseLogic->WarmupEarlyCountdownDuration = WarmupDuration;
                                    }
                                }

                                bStartedBus = true;
                            }
                        }
                    }
                }
            }

            if (bShowsDefaultMatchSettings)
            {
            ImGui::Spacing();
            ImGui::Spacing();

            ImGui::Text("Match Settings:");
            SmallSeparator(Width);

            if (gsStatus >= Joinable && gsStatus < Ended && VersionInfo.FortniteVersion > 5.41 && VersionInfo.FortniteVersion <= 16.00)
            {
                if (ImGui::Checkbox("Glider Redeploy", &FConfiguration::bGliderRedeploy))
                {
                    auto GliderGameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
                    auto GliderGameState = GliderGameMode->GameState;
                    if (GliderGameState->HasDefaultGliderRedeployCanRedeploy())
                        GliderGameState->DefaultGliderRedeployCanRedeploy = FConfiguration::bGliderRedeploy ? 1.0f : 0.0f;
                }
            }

            if ((VersionInfo.FortniteVersion >= 8.00 || gsStatus < Joinable) && VersionInfo.FortniteVersion > 2.50)
            {
                if (ImGui::Checkbox("Infinite Respawns", &FConfiguration::bForceRespawns))
                {
                    if (gsStatus >= Joinable)
                    {
                        auto RespawnPlaylist = FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);
                        if (!RespawnPlaylist)
                            RespawnPlaylist = FindObject<UFortPlaylistAthena>(L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

                        if (RespawnPlaylist)
                        {
                            if (FConfiguration::bForceRespawns)
                            {
                                if (RespawnPlaylist->HasbRespawnInAir())
                                    RespawnPlaylist->bRespawnInAir = true;
                                if (RespawnPlaylist->HasRespawnHeight())
                                {
                                    RespawnPlaylist->RespawnHeight.Curve.CurveTable = nullptr;
                                    RespawnPlaylist->RespawnHeight.Curve.RowName = FName();
                                    RespawnPlaylist->RespawnHeight.Value = FConfiguration::RespawnHeight;
                                }
                                if (RespawnPlaylist->HasRespawnTime())
                                {
                                    RespawnPlaylist->RespawnTime.Curve.CurveTable = nullptr;
                                    RespawnPlaylist->RespawnTime.Curve.RowName = FName();
                                    RespawnPlaylist->RespawnTime.Value = FConfiguration::RespawnTime;
                                }

                                if (RespawnPlaylist->HasRespawnType())
                                {
                                    if (FConfiguration::PermanentRespawn)
                                        RespawnPlaylist->RespawnType = 1;
                                    else
                                        RespawnPlaylist->RespawnType = 2;
                                }
                            }
                            else
                            {
                                if (RespawnPlaylist->HasRespawnType())
                                    RespawnPlaylist->RespawnType = 0;
                            }
                        }
                    }
                }

                if (FConfiguration::bForceRespawns)
                {
                    if (ImGui::Checkbox("Storm Respawns", &FConfiguration::PermanentRespawn))
                    {
                        if (gsStatus >= Joinable)
                        {
                            auto RespawnPlaylist = FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);
                            if (!RespawnPlaylist)
                                RespawnPlaylist = FindObject<UFortPlaylistAthena>(L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

                            if (RespawnPlaylist && RespawnPlaylist->HasRespawnType())
                                RespawnPlaylist->RespawnType = FConfiguration::PermanentRespawn ? 1 : 2;
                        }
                    }

                    ImGui::Checkbox("Keep Inventory on Respawn", &FConfiguration::bKeepInventory);
                    ImGui::Checkbox("Midzone Respawns", &FConfiguration::bMidZoneRespawning);

                    ImGui::PushItemWidth(Width);
                    if (ImGui::SliderInt("Respawn Time", &FConfiguration::RespawnTime, 1, 10))
                    {
                        if (gsStatus >= Joinable)
                        {
                            auto RespawnPlaylist = FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);
                            if (!RespawnPlaylist)
                                RespawnPlaylist = FindObject<UFortPlaylistAthena>(L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

                            if (RespawnPlaylist && RespawnPlaylist->HasRespawnTime())
                            {
                                RespawnPlaylist->RespawnTime.Curve.CurveTable = nullptr;
                                RespawnPlaylist->RespawnTime.Curve.RowName = FName();
                                RespawnPlaylist->RespawnTime.Value = FConfiguration::RespawnTime;
                            }
                        }
                    }
                    ImGui::PopItemWidth();

                    ImGui::PushItemWidth(Width);
                    if (ImGui::SliderInt("Respawn Height", &FConfiguration::RespawnHeight, 1000, 50000))
                    {
                        if (gsStatus >= Joinable)
                        {
                            auto RespawnPlaylist = FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);
                            if (!RespawnPlaylist)
                                RespawnPlaylist = FindObject<UFortPlaylistAthena>(L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

                            if (RespawnPlaylist && RespawnPlaylist->HasRespawnHeight())
                            {
                                RespawnPlaylist->RespawnHeight.Curve.CurveTable = nullptr;
                                RespawnPlaylist->RespawnHeight.Curve.RowName = FName();
                                RespawnPlaylist->RespawnHeight.Value = FConfiguration::RespawnHeight;
                            }
                        }
                    }
                    ImGui::PopItemWidth();
                }
            }
            }

            if (gsStatus >= Joinable)
            {
                ImGui::Checkbox("Infinite Materials", &FConfiguration::bInfiniteMats);
                ImGui::Checkbox("Infinite Ammo", &FConfiguration::bInfiniteAmmo);
                ImGui::Checkbox("Toggle Cheat Commands", &FConfiguration::bEnableCheats);
                ImGui::Checkbox("Enable Trickshot Tab", &FConfiguration::bEnableTrickshotTab);
                ImGui::Checkbox("Siphon", &FConfiguration::bSiphon);

                if (FConfiguration::bSiphon)
                {
                    ImGui::SetNextItemWidth(260.0f);
                    ImGui::InputInt("Siphon Amount", &FConfiguration::SiphonAmount);

                    std::vector<const char*> SiphonAnimations = { "Default" };

                    if (VersionInfo.FortniteVersion >= 11.00)
                    {
                        SiphonAnimations.push_back("Slurp");
                        SiphonAnimations.push_back("Bandage Bazooka");
                    }

                    if (VersionInfo.FortniteVersion >= 12.50)
                    {
                        SiphonAnimations.push_back("Orange Paint");
                        SiphonAnimations.push_back("Purple Paint");
                    }

                    SiphonAnimations.push_back("Health Siphon");

                    if (VersionInfo.FortniteVersion >= 19.00)
                    {
                        SiphonAnimations.push_back("Med Mist");
                    }

                    if (VersionInfo.FortniteVersion >= 11.40)
                    {
                        SiphonAnimations.push_back("Upgrade Weapon");
                    }

                    if (FConfiguration::SiphonAnimType >= (int)SiphonAnimations.size())
                        FConfiguration::SiphonAnimType = 0;

                    ImGui::SetNextItemWidth(260.0f);
                    ImGui::Combo("Siphon Animation", &FConfiguration::SiphonAnimType, SiphonAnimations.data(), (int)SiphonAnimations.size());
                }

                if (ImGui::Button("Reset Player Builds", ImVec2(Width, Height)))
                {
                    TArray<ABuildingSMActor*> Builds;
                    Utils::GetAll<ABuildingSMActor>(Builds);

                    for (auto& Build : Builds)
                    {
                        if (Build && Build->bPlayerPlaced)
                            Build->SilentDie(true);
                    }

                    Builds.Free();
                }

                if (ImGui::Button("Destroy Floor Loot", ImVec2(Width, Height)))
                {
                    TArray<AActor*> Pickups;
                    Utils::GetAll<AActor>(AFortPickupAthena::StaticClass(), Pickups);

                    for (auto& Pickup : Pickups)
                    {
                        if (Pickup)
                            Pickup->K2_DestroyActor();
                    }

                    Pickups.Free();
                }
            }

            if (gsStatus == StartedMatch)
            {
                ImGui::Spacing();
                ImGui::Spacing();

                ImGui::Text("Storm Settings:");
                SmallSeparator(Width);

                if (!UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bPausedZone)
                {
                    if (ImGui::Button("Pause Safe Zone", ImVec2(Width, Height)))
                    {
                        UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bPausedZone = true;
                        auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
                        if (GameMode->HasbSafeZonePaused())
                            GameMode->bSafeZonePaused = true;
                        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"pausesafezone"), nullptr);
                    }
                }
                else
                {
                    if (ImGui::Button("Resume Safe Zone", ImVec2(Width, Height)))
                    {
                        UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bPausedZone = false;
                        auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
                        if (GameMode->HasbSafeZonePaused())
                            GameMode->bSafeZonePaused = false;
                        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"pausesafezone"), nullptr);
                    }
                }

                if (ImGui::Button("Skip Safe Zone", ImVec2(Width, Height)))
                {
                    auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;

                    if (GameMode->HasSafeZoneIndicator())
                    {
                        if (GameMode->SafeZoneIndicator)
                        {
                            GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
                            GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime = GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime + 0.05f;
                        }
                    }
                    else
                    {
                        auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());

                        if (GamePhaseLogic->SafeZoneIndicator)
                        {
                            GamePhaseLogic->SafeZoneIndicator->SafeZoneStartShrinkTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
                            GamePhaseLogic->SafeZoneIndicator->SafeZoneFinishShrinkTime = GamePhaseLogic->SafeZoneIndicator->SafeZoneStartShrinkTime + 0.05f;
                        }
                    }

                    // UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"skipsafezone"), nullptr);
                }

                if (ImGui::Button("Start Shrinking Safe Zone", ImVec2(Width, Height)))
                {
                    auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;

                    if (GameMode->HasSafeZoneIndicator())
                    {
                        if (GameMode->SafeZoneIndicator)
                            GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
                    }
                    else
                    {
                        auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());

                        if (GamePhaseLogic->SafeZoneIndicator)
                            GamePhaseLogic->SafeZoneIndicator->SafeZoneStartShrinkTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
                    }

                    // UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"startshrinksafezone"), nullptr);
                }
            }

            if (gsStatus > Joinable)
            {
                if (hasEvent == 2)
                {
                    for (auto& Event : Events::EventsArray)
                    {
                        if (Event.EventVersion == VersionInfo.FortniteVersion && Event.PlaylistPath && wcscmp(FConfiguration::Playlist, Event.PlaylistPath) == 0)
                        {
                            if (!FConfiguration::bEventStarted)
                            {
                                ImGui::Spacing();
                                ImGui::Spacing();

                                ImGui::Text("Event:");
                                SmallSeparator(Width);

                                if (ImGui::Button("Manually Start Event", ImVec2(Width, Height)))
                                    Events::StartEvent();
                            }
                        }
                    }
                }
            }

            if (gsStatus >= Joinable)
            {
                ImGui::Spacing();
                ImGui::Spacing();

                ImGui::Text("Server Console:");
                SmallSeparator(Width);

                ImGui::SetNextItemWidth(260.0f);
                ImGui::InputText("", commandBuffer, IM_ARRAYSIZE(commandBuffer));

                if (ImGui::Button("Execute Console Command", ImVec2(Width, Height)))
                {
                    std::string str = commandBuffer;
                    auto wstr = std::wstring(str.begin(), str.end());

                    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(wstr.c_str()), nullptr);
                }
            }

            // "Start Server" button moved to the sidebar (bottom) for easy access.

            break;
        }
        case 1:
        {
            static bool bInitializedPlaylist = false;

            if (!bInitializedPlaylist)
            {
                if (VersionInfo.FortniteVersion == 11.31 || VersionInfo.FortniteVersion == 12.41)
                    SelectedPlaylist = static_cast<int>(Playlist::UnvSolos);

                bInitializedPlaylist = true;
            }

            if (VersionInfo.FortniteVersion == 7.40 || VersionInfo.FortniteVersion == 14.40 || VersionInfo.FortniteVersion == 27.11 || VersionInfo.FortniteVersion == 30.00)
            {
                ImGui::Text("Custom Playlists - (Requires PAK Files):");
                SmallSeparator(Width);

                if (VersionInfo.FortniteVersion == 7.40)
                {
                    ImGui::RadioButton("Backrooms Map", &SelectedPlaylist, (int)Playlist::Backrooms);
                }

                if (VersionInfo.FortniteVersion == 27.11)
                {
                    ImGui::RadioButton("Gav 1v1 Map", &SelectedPlaylist, (int)Playlist::Gav);
                    ImGui::RadioButton("Only Up Map", &SelectedPlaylist, (int)Playlist::OnlyUp);
                    ImGui::RadioButton("Tilted FFA", &SelectedPlaylist, (int)Playlist::TiltedZW);
                }

                if (VersionInfo.FortniteVersion == 14.40)
                {
                    ImGui::RadioButton("Retrac 1v1 Map", &SelectedPlaylist, (int)Playlist::Retrac1v1);
                    ImGui::RadioButton("Retrac Turtle Fights", &SelectedPlaylist, (int)Playlist::RetracTurtle);
                    //ImGui::RadioButton("Retrac Water Map", &SelectedPlaylist, (int)Playlist::RetracWater);
                    //ImGui::RadioButton("Twine 1v1 Map", &SelectedPlaylist, (int)Playlist::Twine1v1);
                }

                if (VersionInfo.FortniteVersion == 30.00)
                {
                    ImGui::RadioButton("Boxfights", &SelectedPlaylist, (int)Playlist::Boxfight);
                }

                ImGui::Spacing();
                ImGui::Text("Playlists:");
                SmallSeparator(Width);
            }

            ImGui::RadioButton("Solos", &SelectedPlaylist, (int)Playlist::Solos);
            ImGui::RadioButton("Duos", &SelectedPlaylist, (int)Playlist::Duos);

            if (VersionInfo.FortniteVersion >= 7.40) // 7.30 content update idfk
            {
                ImGui::RadioButton("Trios", &SelectedPlaylist, (int)Playlist::Trios);
            }

            ImGui::RadioButton("Squads", &SelectedPlaylist, (int)Playlist::Squads);

            if (VersionInfo.FortniteVersion >= 20.00)
            {
                ImGui::RadioButton("Zero Build Solos", &SelectedPlaylist, (int)Playlist::ZBSolos);
                ImGui::RadioButton("Zero Build Duos", &SelectedPlaylist, (int)Playlist::ZBDuos);
                ImGui::RadioButton("Zero Build Trios", &SelectedPlaylist, (int)Playlist::ZBTrios);
                ImGui::RadioButton("Zero Build Squads", &SelectedPlaylist, (int)Playlist::ZBSquads);
            }

            if (VersionInfo.FortniteVersion >= 8.20)
            {
                ImGui::RadioButton("Arena Solos", &SelectedPlaylist, (int)Playlist::ArenaSolos);
                ImGui::RadioButton("Arena Duos", &SelectedPlaylist, (int)Playlist::ArenaDuos);
                ImGui::RadioButton("Arena Trios", &SelectedPlaylist, (int)Playlist::ArenaTrios);
                ImGui::RadioButton("Arena Squads", &SelectedPlaylist, (int)Playlist::ArenaSquads);

                if (VersionInfo.FortniteVersion >= 20.00)
                {
                    ImGui::RadioButton("Arena Zero Build Solos", &SelectedPlaylist, (int)Playlist::ArenaZBSolos);
                    ImGui::RadioButton("Arena Zero Build Duos", &SelectedPlaylist, (int)Playlist::ArenaZBDuos);
                    ImGui::RadioButton("Arena Zero Build Trios", &SelectedPlaylist, (int)Playlist::ArenaZBTrios);
                    ImGui::RadioButton("Arena Zero Build Squads", &SelectedPlaylist, (int)Playlist::ArenaZBSquads);
                }
            }

            if (VersionInfo.FortniteVersion >= 6.10)
            {
                ImGui::RadioButton("Tournament Solos", &SelectedPlaylist, (int)Playlist::TournamentSolos);
                ImGui::RadioButton("Tournament Duos", &SelectedPlaylist, (int)Playlist::TournamentDuos);
                ImGui::RadioButton("Tournament Trios", &SelectedPlaylist, (int)Playlist::TournamentTrios);
                ImGui::RadioButton("Tournament Squads", &SelectedPlaylist, (int)Playlist::TournamentSquads);
            }

            if (VersionInfo.FortniteVersion >= 7.10)
            {
                ImGui::RadioButton("One Shot Solos", &SelectedPlaylist, (int)Playlist::OneShotSolos);
                ImGui::RadioButton("One Shot Duos", &SelectedPlaylist, (int)Playlist::OneShotDuos);
                ImGui::RadioButton("One Shot Squads", &SelectedPlaylist, (int)Playlist::OneShotSquads);
                ImGui::RadioButton("Siphon Solos", &SelectedPlaylist, (int)Playlist::SiphonSolos);
                ImGui::RadioButton("Siphon Duos", &SelectedPlaylist, (int)Playlist::SiphonDuos);
                ImGui::RadioButton("Siphon Squads", &SelectedPlaylist, (int)Playlist::SiphonSquads);
                ImGui::RadioButton("Unvaulted Solos", &SelectedPlaylist, (int)Playlist::UnvSolos);
                ImGui::RadioButton("Unvaulted Duos", &SelectedPlaylist, (int)Playlist::UnvDuos);
                ImGui::RadioButton("Unvaulted Trios", &SelectedPlaylist, (int)Playlist::UnvTrios);
                ImGui::RadioButton("Unvaulted Squads", &SelectedPlaylist, (int)Playlist::UnvSquads);
                ImGui::RadioButton("Slide Solos", &SelectedPlaylist, (int)Playlist::SlideSolos);
                ImGui::RadioButton("Slide Duos", &SelectedPlaylist, (int)Playlist::SlideDuos);
            }

            if ((VersionInfo.FortniteVersion >= 8.20 && VersionInfo.FortniteVersion <= 10.40) || VersionInfo.FortniteVersion >= 15.20)
            {
                ImGui::RadioButton("Floor Is Lava Solos", &SelectedPlaylist, (int)Playlist::FILSolos);
                ImGui::RadioButton("Floor Is Lava Duos", &SelectedPlaylist, (int)Playlist::FILDuos);
                ImGui::RadioButton("Floor Is Lava Squads", &SelectedPlaylist, (int)Playlist::FILSquads);
            }

            if (VersionInfo.FortniteVersion >= 4.5 && VersionInfo.FortniteVersion < 11.31)
            {
                ImGui::RadioButton("Playground", &SelectedPlaylist, (int)Playlist::Playground);
            }

            if (VersionInfo.FortniteVersion >= 7.00)
            {
                ImGui::RadioButton("Creative ", &SelectedPlaylist, (int)Playlist::Creative);
            }

            for (auto& Event : Events::EventsArray)
            {
                if (Event.EventVersion == VersionInfo.FortniteVersion)
                {
                    ImGui::RadioButton("Event Playlist", &SelectedPlaylist, (int)Playlist::Event);
                }
            }

            ImGui::RadioButton("Custom", &SelectedPlaylist, (int)Playlist::Custom);

            switch (SelectedPlaylist)
            {
            case (int)Playlist::Solos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
                break;
            }
            case (int)Playlist::Duos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Playlist_DefaultDuo.Playlist_DefaultDuo";
                break;
            }
            case (int)Playlist::Trios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Trios/Playlist_Trios.Playlist_Trios";
                break;
            }
            case (int)Playlist::Squads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Playlist_DefaultSquad.Playlist_DefaultSquad";
                break;
            }
            case (int)Playlist::ZBSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Playlist_NoBuildBR_Solo.Playlist_NoBuildBR_Solo";
                break;
            }
            case (int)Playlist::ZBDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Playlist_NoBuildBR_Duo.Playlist_NoBuildBR_Duo";
                break;
            }
            case (int)Playlist::ZBTrios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Playlist_NoBuildBR_Trio.Playlist_NoBuildBR_Trio";
                break;
            }
            case (int)Playlist::ZBSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Playlist_NoBuildBR_Squad.Playlist_NoBuildBR_Squad";
                break;
            }
            case (int)Playlist::ArenaZBSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Solo.Playlist_ShowdownAlt_NoBuildBR_Solo";
                break;
            }
            case (int)Playlist::ArenaZBDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Duos.Playlist_ShowdownAlt_NoBuildBR_Duos";
                break;
            }
            case (int)Playlist::ArenaZBTrios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Trios.Playlist_ShowdownAlt_NoBuildBR_Trios";
                break;
            }
            case (int)Playlist::ArenaZBSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Squads.Playlist_ShowdownAlt_NoBuildBR_Squads";
                break;
            }
            case (int)Playlist::OneShotSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Low/Playlist_Low_Solo.Playlist_Low_Solo";
                break;
            }
            case (int)Playlist::OneShotDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Low/Playlist_Low_Duos.Playlist_Low_Duos";
                break;
            }
            case (int)Playlist::OneShotSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Low/Playlist_Low_Squad.Playlist_Low_Squad";
                break;
            }
            case (int)Playlist::SiphonSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Vamp/Playlist_Vamp_Solo.Playlist_Vamp_Solo";
                break;
            }
            case (int)Playlist::SiphonDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Vamp/Playlist_Vamp_Duos.Playlist_Vamp_Duos";
                break;
            }
            case (int)Playlist::SiphonSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Vamp/Playlist_Vamp_Squad.Playlist_Vamp_Squad";
                break;
            }
            case (int)Playlist::UnvSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Unvaulted/Playlist_Unvaulted_Solo.Playlist_Unvaulted_Solo";
                break;
            }
            case (int)Playlist::UnvDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Unvaulted/Playlist_Unvaulted_Duos.Playlist_Unvaulted_Duos";
                break;
            }
            case (int)Playlist::UnvTrios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Unvaulted/Playlist_Unvaulted_Trios.Playlist_Unvaulted_Trios";
                break;
            }
            case (int)Playlist::UnvSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Unvaulted/Playlist_Unvaulted_Squads.Playlist_Unvaulted_Squads";
                break;
            }
            case (int)Playlist::SlideSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Slide/Playlist_Slide_Solo.Playlist_Slide_Solo";
                break;
            }
            case (int)Playlist::SlideDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Slide/Playlist_Slide_Duos.Playlist_Slide_Duos";
                break;
            }
            case (int)Playlist::TournamentSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_Showdown_Solo.Playlist_Showdown_Solo";
                break;
            }
            case (int)Playlist::TournamentDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_Showdown_Duos.Playlist_Showdown_Duos";
                break;
            }
            case (int)Playlist::TournamentTrios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_Showdown_Trios.Playlist_Showdown_Trios";
                break;
            }
            case (int)Playlist::TournamentSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_Showdown_Squads.Playlist_Showdown_Squads";
                break;
            }
            case (int)Playlist::ArenaSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Solo.Playlist_ShowdownAlt_Solo";
                break;
            }
            case (int)Playlist::ArenaDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Duos.Playlist_ShowdownAlt_Duos";
                break;
            }
            case (int)Playlist::ArenaTrios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Trios.Playlist_ShowdownAlt_Trios";
                break;
            }
            case (int)Playlist::ArenaSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Squads.Playlist_ShowdownAlt_Squads";
                break;
            }
            case (int)Playlist::FILSolos:
            {
                if (VersionInfo.FortniteVersion <= 10.40)
                    FConfiguration::Playlist = L"/Game/Athena/Playlists/Fill/Playlist_Fill_Solo.Playlist_Fill_Solo";
                else
                    FConfiguration::Playlist = L"/Melt/Playlists/Playlist_Melt_Solo.Playlist_Melt_Solo";
                break;
            }
            case (int)Playlist::FILDuos:
            {
                if (VersionInfo.FortniteVersion <= 10.40)
                    FConfiguration::Playlist = L"/Game/Athena/Playlists/Fill/Playlist_Fill_Duos.Playlist_Fill_Duos";
                else
                    FConfiguration::Playlist = L"/Melt/Playlists/Playlist_Melt_Duos.Playlist_Melt_Duos";
                break;
            }
            case (int)Playlist::FILSquads:
            {
                if (VersionInfo.FortniteVersion <= 10.40)
                    FConfiguration::Playlist = L"/Game/Athena/Playlists/Fill/Playlist_Fill_Squads.Playlist_Fill_Squads";
                else
                    FConfiguration::Playlist = L"/Melt/Playlists/Playlist_Melt_Squads.Playlist_Melt_Squads";
                break;
            }
            case (int)Playlist::Playground:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Playground/Playlist_Playground.Playlist_Playground";
                FConfiguration::bLateGame = false;
                break;
            }
            case (int)Playlist::Creative:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Creative/Playlist_PlaygroundV2.Playlist_PlaygroundV2";
                FConfiguration::bLateGame = false;
                break;
            }
            case (int)Playlist::Gav:
            {
                FConfiguration::Playlist = L"/Game/Gav/Levels/GM_1v1/Playlist_Arena_DefaultSolo_Respawn.Playlist_Arena_DefaultSolo_Respawn";
                FConfiguration::bLateGame = false;
                break;
            }
            case (int)Playlist::Retrac1v1:
            {
				FConfiguration::Playlist = L"/Buddy/Playlist/Playlist_Retrac_1v1.Playlist_Retrac_1v1";
                FConfiguration::bLateGame = false;
                break;
            }
            case (int)Playlist::RetracTurtle:
            {
				FConfiguration::Playlist = L"/Buddy/Playlist/Playlist_Retrac_Turtle.Playlist_Retrac_Turtle";
                FConfiguration::bLateGame = false;
                break;
            }
            case (int)Playlist::RetracWater:
            {
				FConfiguration::Playlist = L"/Game/Retrac/Playlists/Playlist_ShowdownAlt_Solo_Retrac.Playlist_ShowdownAlt_Solo_Retrac";
                FConfiguration::bLateGame = false;
                break;
            }
            case (int)Playlist::TiltedZW:
            {
                FConfiguration::Playlist = L"/Game/Jett/TiltedZW/Playlist_TiltedZW_Jett.Playlist_TiltedZW_Jett";
                FConfiguration::bLateGame = false;
                break;
			}
            case (int)Playlist::OnlyUp:
            {
                FConfiguration::Playlist = L"/Game/Jett/Playlist_OnlyUp_Jett.Playlist_OnlyUp_Jett";
                FConfiguration::bLateGame = false;
                break;
			}
            case (int)Playlist::Twine1v1:
            {
                FConfiguration::Playlist = L"/Buddy/Playlists/Playlist_1v1Twine.Playlist_1v1Twine";
                FConfiguration::bLateGame = false;
                break;
			}
            case (int)Playlist::Boxfight:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Respawn/Playlist_Respawn_Solo.Playlist_Respawn_Solo";
                FConfiguration::bLateGame = false;
                break;
			}
            case (int)Playlist::Backrooms:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
                FConfiguration::bLateGame = false;
                break;
            }
            case (int)Playlist::Event:
            {
                for (auto& Event : Events::EventsArray)
                {
                    if (Event.EventVersion == VersionInfo.FortniteVersion)
                    {
                        if (Event.PlaylistPath != nullptr)
                            FConfiguration::Playlist = Event.PlaylistPath;

                        FConfiguration::bLateGame = false;
                        FConfiguration::bAutoStartEvent = true;
                        break;
                    }
                }
                break;
            }
            case (int)Playlist::Custom:
            {
                break;
            }
            default:
            {
                break;
            }
            }

            if (SelectedPlaylist == (int)Playlist::Custom)
            {
                ImGui::SetNextItemWidth(260.0f);
                ImGui::InputText("", playlistBuffer, IM_ARRAYSIZE(playlistBuffer));

                if (ImGui::Button("Set Playlist", ImVec2(Width, Height)))
                {
                    std::string str = playlistBuffer;
                    static std::wstring persistentWstr;
                    persistentWstr = std::wstring(str.begin(), str.end());

                    FConfiguration::Playlist = persistentWstr.c_str();
                }
            }

            if (SelectedPlaylist == (int)Playlist::Event)
            {
                ImGui::Spacing();
                ImGui::Spacing();

                ImGui::Checkbox("Auto Start Event", &FConfiguration::bAutoStartEvent);

                if (FConfiguration::bAutoStartEvent)
                {
                    ImGui::PushItemWidth(Width);
                    ImGui::SliderFloat("Event Start Time", &FConfiguration::EventStartTime, 30.0f, 300.0f, "%.1f seconds");
                    ImGui::PopItemWidth();
                }
            }

            break;
        }
        case 2:
        {
            auto World = UWorld::GetWorld();

            if (!World) 
                break;

            static int InspectedPlayerIdx = -1;
            static bool bIsInspecting = false;

            UObject* NetDriver = World->NetDriver;

            if (!NetDriver) 
                break;

            UNetDriver* Driver = static_cast<UNetDriver*>(NetDriver);

            auto& ClientConnections = Driver->ClientConnections;
            AllControllers.clear();

            for (int i = 0; i < ClientConnections.Num(); i++)
            {
                auto Connection = ClientConnections[i];

                if (!Connection || !Connection->PlayerController) 
                    continue;

                AllControllers.push_back(std::make_pair((AFortPlayerControllerAthena*)Connection->PlayerController, Connection));
            }

            if (!bIsInspecting)
            {
                ImGui::Text(("Players Connected: " + std::to_string(AllControllers.size())).c_str());
                SmallSeparator(Width);

                for (int i = 0; i < AllControllers.size(); i++)
                {
                    auto& CurrentPair = AllControllers[i];
                    auto CurrentPlayerState = CurrentPair.first->PlayerState;

                    if (!CurrentPlayerState)
                    {
                        printf("PlayerState is null!\n");
                        continue;
                    }

                    auto Connection = CurrentPair.second;

                    std::string ButtonLabel = GUI::GetPlayerName(CurrentPlayerState, Connection);
                    auto RequestURL = GUI::GetRequestURL(Connection);

                    if (RequestURL && RequestURL->Data && RequestURL->NumElements)
                    {
                        auto RequestURLStr = RequestURL->ToString();
                        std::size_t pos = RequestURLStr.find("Name=");

                        if (pos != std::string::npos)
                        {
                            std::size_t end_pos = RequestURLStr.find('?', pos);
                            if (end_pos != std::string::npos)
                                ButtonLabel = RequestURLStr.substr(pos + 5, end_pos - pos - 5);
                            else
                                ButtonLabel = RequestURLStr.substr(pos + 5);
                        }
                    }

                    if (ButtonLabel.empty())
                        ButtonLabel = GUI::GetPlayerNameFromConnection(Connection);

                    if (ButtonLabel.empty())
                        ButtonLabel = std::string("Player ") + std::to_string(i + 1);

                    if (ImGui::Button(ButtonLabel.c_str(), ImVec2(Width, Height)))
                    {
                        InspectedPlayerIdx = i;
                        bIsInspecting = true;
                    }
                }
            }
            else
            {
                if (InspectedPlayerIdx >= AllControllers.size())
                {
                    bIsInspecting = false;
                    break;
                }

                auto TargetPC = AllControllers[InspectedPlayerIdx].first;
                auto TargetPS = TargetPC->PlayerState;
                auto TargetPawn = (AFortPlayerPawnAthena*)TargetPC->Pawn;

                if (!TargetPC || !TargetPawn || !TargetPS)
                {
                    bIsInspecting = false;
                    break;
				}

                if (ImGui::Button("Back", ImVec2(Width, Height)))
                {
                    bIsInspecting = false;
                    break;
                }

                ImGui::Text("Player Information:");
                SmallSeparator(Width);

                auto InspectedPlayerState = (AFortPlayerStateAthena*)AllControllers[InspectedPlayerIdx].first->PlayerState;
                auto InspectedConnection = AllControllers[InspectedPlayerIdx].second;

                std::string DisplayName = GUI::GetPlayerName(InspectedPlayerState, InspectedConnection);

                if (DisplayName.empty())
                    DisplayName = std::string("Player ") + std::to_string(InspectedPlayerIdx + 1);

                ImGui::TextUnformatted("Inspecting Player: ");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ImVec4(1.f, 1.f, 0.f, 1.0f), DisplayName.c_str());

				ImGui::Text("Join Order: #%d", InspectedPlayerIdx + 1);

                //ImGui::Text("Ping: %f ms", TargetPS->GetPingInMilliseconds()); // ig it js doesn't exist on some versions

                ImGui::Text("Kills: %d", TargetPS->HasKillScore() ? TargetPS->KillScore : TargetPS->Kills);

                ImGui::TextUnformatted("Health: ");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ImVec4(0.372f, 0.792f, 0.255f, 1.0f), "%.0f", TargetPawn->GetHealth());
                ImGui::TextUnformatted("Shield: ");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ImVec4(0.278f, 0.612f, 0.945f, 1.0f), "%.0f", TargetPawn->GetShield());

                ImGui::Spacing();

                ImGui::Text("Configure Player:");
                SmallSeparator(Width);

                if (ImGui::Button("Copy Player's Location", ImVec2(Width, Height)))
                {
                    auto Location = TargetPawn->K2_GetActorLocation();

                    Memcury::Util::CopyToClipboard(std::to_string(Location.X) + " " + std::to_string(Location.Y) + " " + std::to_string(Location.Z));
				}

                if (ImGui::Button("Teleport All Players", ImVec2(Width, Height)))
                    AFortPlayerControllerAthena::TeleportAllPlayersTo(TargetPC);

                if (ImGui::Button("Regenerate Health & Shield", ImVec2(Width, Height)))
                {
                    TargetPawn->SetHealth(TargetPawn->GetMaxHealth());
                    TargetPawn->SetShield(TargetPawn->GetMaxShield());

                    auto Handle = TargetPS->AbilitySystemComponent->MakeEffectContext();
                    FGameplayTag Tag;
                    static auto Cue = FName(L"GameplayCue.Shield.PotionConsumed");
                    Tag.TagName = Cue;
                    auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
                    memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());
                    TargetPS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(Tag, *PredictionKey, Handle);
                    TargetPS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(Tag, *PredictionKey, Handle);
                    free(PredictionKey);
                }

                if (ImGui::Button("Rift Player", ImVec2(Width, Height)))
                {
					auto Loc = TargetPawn->K2_GetActorLocation();

                    static auto RiftClass = FindObject<UClass>(L"/Game/Athena/Items/Consumables/RiftItem/BGA_RiftPortal_Item_Athena.BGA_RiftPortal_Item_Athena_C");

                    auto Rift = UWorld::SpawnActor<UClass>(RiftClass, Loc, {});

                    auto Actor = (AActor*)Rift;
                    Actor->ForceNetUpdate();

                    Actor->K2_DestroyActor();
				}

                auto& Health = TargetPC->MyFortPawn->HealthSet->Health;
                float MinValue = 100.f;

                bool bIsGodded = false;

                if (VersionInfo.FortniteVersion >= 21)
                {
                    if (TargetPC->Pawn)
                        bIsGodded = (TargetPC->Pawn->bCanBeDamaged == false);
                }
                else
                {
                    bIsGodded = (Health.Minimum == MinValue);
                }

                if (bIsGodded)
                {
                    if (ImGui::Button("Ungod Player", ImVec2(Width, Height)))
                    {
                        if (VersionInfo.FortniteVersion >= 21)
                        {
                            TargetPC->Pawn->bCanBeDamaged = true;
                        }
                        else
                        {
                            Health.Minimum = 0.f;
                            TargetPC->MyFortPawn->HealthSet->OnRep_Health(Health);
                        }
                    }
                }
                else
                {
                    if (ImGui::Button("God Player", ImVec2(Width, Height)))
                    {
                        if (VersionInfo.FortniteVersion >= 21)
                        {
                            TargetPC->Pawn->bCanBeDamaged = false;

                            float MaxHealth = TargetPawn->GetMaxHealth();
                            float MaxShield = TargetPawn->GetMaxShield();

                            TargetPawn->SetHealth(MaxHealth);
                            TargetPawn->SetShield(MaxShield);

                            auto Handle = TargetPS->AbilitySystemComponent->MakeEffectContext();
                            FGameplayTag Tag;
                            static auto Cue = FName(L"GameplayCue.Shield.PotionConsumed");
                            Tag.TagName = Cue;
                            auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
                            memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());
                            TargetPS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(Tag, *PredictionKey, Handle);
                            TargetPS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(Tag, *PredictionKey, Handle);
                            free(PredictionKey);
                        }
                        else
                        {
                            if (Health.Minimum != MinValue)
                            {
                                Health.Minimum = MinValue;
                                TargetPC->MyFortPawn->HealthSet->OnRep_Health(Health);

                                float MaxHealth = TargetPawn->GetMaxHealth();
                                float MaxShield = TargetPawn->GetMaxShield();

                                TargetPawn->SetHealth(MaxHealth);
                                TargetPawn->SetShield(MaxShield);

                                auto Handle = TargetPS->AbilitySystemComponent->MakeEffectContext();
                                FGameplayTag Tag;
                                static auto Cue = FName(L"GameplayCue.Shield.PotionConsumed");
                                Tag.TagName = Cue;
                                auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
                                memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());
                                TargetPS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(Tag, *PredictionKey, Handle);
                                TargetPS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(Tag, *PredictionKey, Handle);
                                free(PredictionKey);
                            }
                            else
                            {
                                Health.Minimum = 0.f;
                                TargetPC->MyFortPawn->HealthSet->OnRep_Health(Health);
                            }
                        }
                    }
                }

                if (ImGui::Button("Eliminate Player", ImVec2(Width, Height)))
                {
                    TargetPC->ServerSuicide();
                    bIsInspecting = false;
                }

                if (ImGui::Button("Kick Player", ImVec2(Width, Height)))
                {
                    TargetPC->ServerReturnToMainMenu("You have been kicked from the game by the host.");
					bIsInspecting = false;
                }

                ImGui::Spacing();
                ImGui::Spacing();

                static float LaunchX = 0.f;
                static float LaunchY = 0.f;
                static float LaunchZ = 0.f;

                ImGui::SetNextItemWidth(Width);
                ImGui::InputFloat("Launch X", &LaunchX);

                ImGui::SetNextItemWidth(Width);
                ImGui::InputFloat("Launch Y", &LaunchY);

                ImGui::SetNextItemWidth(Width);
                ImGui::InputFloat("Launch Z", &LaunchZ);

                if (ImGui::Button("Launch Player", ImVec2(Width, Height)))
                {
                    FVector LaunchVelocity = FVector(LaunchX, LaunchY, LaunchZ);
                    TargetPawn->LaunchCharacterJump(LaunchVelocity, false, nullptr, true);
                }

                ImGui::Spacing();
                ImGui::Spacing();

                static char WID[256] = {};
				static int Amount = 1.f;

                ImGui::SetNextItemWidth(Width);
                ImGui::InputText("Item To Give", WID, IM_ARRAYSIZE(WID));

                ImGui::SetNextItemWidth(Width);
                ImGui::InputInt("Amount To Give", &Amount);

                if (ImGui::Button("Give Item To Player", ImVec2(Width, Height)))
                {
                    if (WID[0] != '\0')
                    {
                        std::string ItemID = WID;
                        auto ItemDefinition = FindObject<UFortItemDefinition>(UEAllocatedWString(ItemID.begin(), ItemID.end()));

                        if (!ItemDefinition)
                            ItemDefinition = TUObjectArray::FindObject<UFortItemDefinition>(ItemID.c_str());

                        int32 Count = Amount;

                        if (Count <= 0)
							Count = ItemDefinition->GetMaxStackSize();

                        FVector FinalLoc = TargetPawn ? TargetPawn->K2_GetActorLocation() : FVector();

                        FVector ForwardVector = TargetPawn ? TargetPawn->GetActorForwardVector() : FVector();
                        ForwardVector.Z = 0.0f;
                        ForwardVector.Normalize();

                        const float RandomAngleVariation = ((float)rand() * 0.00109866634f) - 18.f;
                        const float FinalAngle = RandomAngleVariation * 0.017453292519943295f;

                        FinalLoc.X += cos(FinalAngle) * 100.f;
                        FinalLoc.Y += sin(FinalAngle) * 100.f;

                        auto Pickup = AFortInventory::SpawnPickup(FinalLoc, ItemDefinition, Count, 0, EFortPickupSourceTypeFlag::GetOther(), EFortPickupSpawnSource::GetUnset(), TargetPawn);

                        if (TargetPawn && Pickup)
                            TargetPawn->ServerHandlePickup(Pickup, Pickup->PickupLocationData.FlyTime, FVector(), true);
                    }
				}

                ImGui::Spacing();
                ImGui::Spacing();

                static std::string nameStr;
                ImGui::SetNextItemWidth(Width);
                ImGui::InputText("New Name", &nameStr);

                if (ImGui::Button("Change Player's Name", ImVec2(Width, Height)))
                {
                    if (!nameStr.empty())
                    {
                        std::wstring nameW(nameStr.begin(), nameStr.end());
                        FString NewName = FString(nameW.c_str());

                        if (!TargetPC)
                            return;

                        TargetPC->ServerChangeName(NewName);
                        TargetPS->OnRep_PlayerName();
                        //nameStr.clear();
                    }
                }
            }

            break;
        }
        case 3:
        {
            ImGui::Text("Lategame Options:");
            SmallSeparator(Width);

            if (gsStatus < StartedMatch)
            {
                bool bIsCustomMap = SelectedPlaylist == static_cast<int>(Playlist::Gav)
                    || SelectedPlaylist == static_cast<int>(Playlist::Retrac1v1)
                    || SelectedPlaylist == static_cast<int>(Playlist::RetracTurtle)
                    || SelectedPlaylist == static_cast<int>(Playlist::RetracWater)
                    || SelectedPlaylist == static_cast<int>(Playlist::TiltedZW)
                    || SelectedPlaylist == static_cast<int>(Playlist::OnlyUp)
                    || SelectedPlaylist == static_cast<int>(Playlist::Twine1v1)
                    || SelectedPlaylist == static_cast<int>(Playlist::Boxfight)
                    || SelectedPlaylist == static_cast<int>(Playlist::Backrooms);

                if (bIsCustomMap)
                    FConfiguration::bLateGame = false;

                ImGui::BeginDisabled(bIsCustomMap);
                ImGui::Checkbox("Late Game", &FConfiguration::bLateGame);
                ImGui::EndDisabled();

                if (FConfiguration::bLateGame)
                {
                    if (VersionInfo.FortniteVersion > 2.50)
                        ImGui::Checkbox("Use Moving Bus", &FConfiguration::bMovingBus);
                    ImGui::Checkbox("Use Long Zone", &FConfiguration::bLateGameLongZone);
                    ImGui::Checkbox("Use Versionized Lategame Loadouts", &FConfiguration::bUseVersionizedLoadout);
                    ImGui::Checkbox("Use Custom Lategame Loadout", &FConfiguration::bUseCustomLoadout);

                    ImGui::PushItemWidth(Width);
                    ImGui::SliderInt("Starting Zone", &FConfiguration::LateGameZone, 1, 7);
                    ImGui::PopItemWidth();
                }
            }

            if (gsStatus < StartedMatch && FConfiguration::bLateGame)
            {
            static char PrimaryWeaponBuffer[256] = { 0 };
            static char SecondaryWeaponBuffer[256] = { 0 };
            static char TertiaryWeaponBuffer[256] = { 0 };
            static char QuaternaryWeaponBuffer[256] = { 0 };
            static char QuinaryWeaponBuffer[256] = { 0 };
            static char TrapsBuffer[256] = { 0 };

            static int PrimaryAmountBuffer = 1;
            static int SecondaryAmountBuffer = 1;
            static int TertiaryAmountBuffer = 1;
            static int QuaternaryAmountBuffer = 1;
            static int QuinaryAmountBuffer = 1;
            static int TrapsAmountBuffer = 6;

            static bool bBuffersInitialized = false;
            static std::string LoadoutStatusMessage;
            static std::chrono::high_resolution_clock::time_point StatusMessageTime;
            static std::string ApplyLoadoutStatusMessage;
            static std::chrono::high_resolution_clock::time_point ApplyStatusMessageTime;

            if (FConfiguration::bUseCustomLoadout)
            {
                if (!bBuffersInitialized)
                {
                    strcpy_s(PrimaryWeaponBuffer, TCHAR_TO_UTF8(*FConfiguration::Primary));
                    strcpy_s(SecondaryWeaponBuffer, TCHAR_TO_UTF8(*FConfiguration::Secondary));
                    strcpy_s(TertiaryWeaponBuffer, TCHAR_TO_UTF8(*FConfiguration::Tertiary));
                    strcpy_s(QuaternaryWeaponBuffer, TCHAR_TO_UTF8(*FConfiguration::Quaternary));
                    strcpy_s(QuinaryWeaponBuffer, TCHAR_TO_UTF8(*FConfiguration::Quinary));
                    strcpy_s(TrapsBuffer, TCHAR_TO_UTF8(*FConfiguration::Traps));

                    PrimaryAmountBuffer = FConfiguration::PrimaryAmount;
                    SecondaryAmountBuffer = FConfiguration::SecondaryAmount;
                    TertiaryAmountBuffer = FConfiguration::TertiaryAmount;
                    QuaternaryAmountBuffer = FConfiguration::QuaternaryAmount;
                    QuinaryAmountBuffer = FConfiguration::QuinaryAmount;
                    TrapsAmountBuffer = FConfiguration::TrapsAmount;

                    bBuffersInitialized = true;
                }

                ImGui::NewLine();

                ImGui::Text("Custom Loadout Slots:");
                SmallSeparator(Width);

                ImGui::PushItemWidth(Width);
                ImGui::InputText("Slot 1", PrimaryWeaponBuffer, sizeof(PrimaryWeaponBuffer));
                ImGui::InputInt("Slot 1 Amount", &PrimaryAmountBuffer);

                ImGui::InputText("Slot 2", SecondaryWeaponBuffer, sizeof(SecondaryWeaponBuffer));
                ImGui::InputInt("Slot 2 Amount", &SecondaryAmountBuffer);

                ImGui::InputText("Slot 3", TertiaryWeaponBuffer, sizeof(TertiaryWeaponBuffer));
                ImGui::InputInt("Slot 3 Amount", &TertiaryAmountBuffer);

                ImGui::InputText("Slot 4", QuaternaryWeaponBuffer, sizeof(QuaternaryWeaponBuffer));
                ImGui::InputInt("Slot 4 Amount", &QuaternaryAmountBuffer);

                ImGui::InputText("Slot 5", QuinaryWeaponBuffer, sizeof(QuinaryWeaponBuffer));
                ImGui::InputInt("Slot 5 Amount", &QuinaryAmountBuffer);

                ImGui::InputText("Trap", TrapsBuffer, sizeof(TrapsBuffer));
                ImGui::InputInt("Trap Amount", &TrapsAmountBuffer);
                ImGui::PopItemWidth();

                if (ImGui::Button("Apply Loadout", ImVec2(Width, Height)))
                {
                    FConfiguration::Primary = FString(PrimaryWeaponBuffer);
                    FConfiguration::Secondary = FString(SecondaryWeaponBuffer);
                    FConfiguration::Tertiary = FString(TertiaryWeaponBuffer);
                    FConfiguration::Quaternary = FString(QuaternaryWeaponBuffer);
                    FConfiguration::Quinary = FString(QuinaryWeaponBuffer);
                    FConfiguration::Traps = FString(TrapsBuffer);

                    FConfiguration::PrimaryAmount = PrimaryAmountBuffer;
                    FConfiguration::SecondaryAmount = SecondaryAmountBuffer;
                    FConfiguration::TertiaryAmount = TertiaryAmountBuffer;
                    FConfiguration::QuaternaryAmount = QuaternaryAmountBuffer;
                    FConfiguration::QuinaryAmount = QuinaryAmountBuffer;
                    FConfiguration::TrapsAmount = TrapsAmountBuffer;

                    printf("Saved current loadout.\n");
                    ApplyLoadoutStatusMessage = "Loadout saved successfully!";
                    ApplyStatusMessageTime = std::chrono::high_resolution_clock::now();

                    if (!ApplyLoadoutStatusMessage.empty())
                    {
                        auto Elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - StatusMessageTime).count();

                        if (Elapsed < 5)
                        {
                            ImVec4 StatusColor = (ApplyLoadoutStatusMessage.find("Failed.") != std::string::npos) ? ImVec4(1.0f, 0.0f, 0.0f, 1.0f) : ImVec4(0.0f, 1.0f, 0.0f, 1.0f);

                            ImGui::TextColored(StatusColor, "%s", LoadoutStatusMessage.c_str());
                        }
                        else
                        {
                            ApplyLoadoutStatusMessage.clear();
                        }
                    }
                }

                ImGui::Spacing();

                ImGui::Text("Save/Load Loadout:");
                SmallSeparator(Width);

                if (ImGui::Button("Save Loadout to File", ImVec2(Width, Height)))
                {
                    if (LoadoutManager::SaveLoadout(PrimaryWeaponBuffer, PrimaryAmountBuffer, SecondaryWeaponBuffer, SecondaryAmountBuffer, TertiaryWeaponBuffer, TertiaryAmountBuffer, QuaternaryWeaponBuffer, QuaternaryAmountBuffer, QuinaryWeaponBuffer, QuinaryAmountBuffer, TrapsBuffer, TrapsAmountBuffer))
                    {
                        LoadoutStatusMessage = "Loadout saved successfully!";
                        printf("Loadout saved to: %s\n", LoadoutManager::GetLoadoutFilePath().c_str());
                    }
                    else
                    {
                        LoadoutStatusMessage = "Failed to save loadout!";
                    }

                    StatusMessageTime = std::chrono::high_resolution_clock::now();
                }

                if (ImGui::Button("Load Loadout from File", ImVec2(Width, Height)))
                {
                    if (LoadoutManager::LoadLoadout(PrimaryWeaponBuffer, PrimaryAmountBuffer, SecondaryWeaponBuffer, SecondaryAmountBuffer, TertiaryWeaponBuffer, TertiaryAmountBuffer, QuaternaryWeaponBuffer, QuaternaryAmountBuffer, QuinaryWeaponBuffer, QuinaryAmountBuffer, TrapsBuffer, TrapsAmountBuffer))
                    {
                        LoadoutStatusMessage = "Loadout loaded successfully!";
                        printf("Loadout loaded from: %s\n", LoadoutManager::GetLoadoutFilePath().c_str());
                    }
                    else
                    {
                        LoadoutStatusMessage = "Failed to load loadout! File may not exist.";
                    }

                    StatusMessageTime = std::chrono::high_resolution_clock::now();
                }

                if (!LoadoutStatusMessage.empty())
                {
                    auto Elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - StatusMessageTime).count();

                    if (Elapsed < 5)
                    {
                        ImVec4 StatusColor = (LoadoutStatusMessage.find("Failed") != std::string::npos) ? ImVec4(1.0f, 0.0f, 0.0f, 1.0f) : ImVec4(0.0f, 1.0f, 0.0f, 1.0f);

                        ImGui::TextColored(StatusColor, "%s", LoadoutStatusMessage.c_str());
                    }
                    else
                    {
                        LoadoutStatusMessage.clear();
                    }
                }
            }
            } // gsStatus < StartedMatch

            break;
        }
        case 4:
        {
            ImGui::Text("Customize Player Bot:");
            SmallSeparator(Width);

            ImGui::PushItemWidth(Width);
            ImGui::InputInt("Bot Health", &FConfiguration::BotHealth);
            ImGui::InputInt("Bot Shield", &FConfiguration::BotShield);
            ImGui::PopItemWidth();

            ImGui::Spacing();

            ImGui::Checkbox("Use Custom Bot Names", &FConfiguration::UseCustomBotNames);

            if (FConfiguration::UseCustomBotNames)
            {
                static char BotNameBuffer[64] = {};

                if (BotNameBuffer[0] == '\0' && !FConfiguration::BotName.empty())
                    strncpy_s(BotNameBuffer, sizeof(BotNameBuffer), FConfiguration::BotName.c_str(), _TRUNCATE);

                ImGui::PushItemWidth(Width);
                ImGui::InputText("Bot Name", BotNameBuffer, sizeof(BotNameBuffer));
                ImGui::PopItemWidth();

                if (ImGui::Button("Apply Bot Name", ImVec2(Width, Height)))
                {
                    FConfiguration::BotName = BotNameBuffer;
                }
            }

            break;
        }
        case 5:
        {
            ImGui::Text("Use Custom Plot:");
            SmallSeparator(Width);

            ImGui::RadioButton("Temperate Island", &SelectedPlot, (int)Plot::Temperate);
            ImGui::RadioButton("Meadow Island", &SelectedPlot, (int)Plot::Meadow);
            ImGui::RadioButton("Arctic Island", &SelectedPlot, (int)Plot::Arctic);
            ImGui::RadioButton("Frosty Fortress", &SelectedPlot, (int)Plot::Fortress);
            ImGui::RadioButton("Ice Lake Island", &SelectedPlot, (int)Plot::IceLake);
            ImGui::RadioButton("Canyon Island", &SelectedPlot, (int)Plot::Canyon);
            ImGui::RadioButton("Arid Island", &SelectedPlot, (int)Plot::Arid);
            ImGui::RadioButton("Wasteland Island", &SelectedPlot, (int)Plot::Wasteland);
            ImGui::RadioButton("Tropical Island", &SelectedPlot, (int)Plot::Tropical);
            ImGui::RadioButton("River Edge Island", &SelectedPlot, (int)Plot::RiverEdge);
            ImGui::RadioButton("Volcano Island", &SelectedPlot, (int)Plot::Volcano);
            ImGui::RadioButton("Sandbar Island", &SelectedPlot, (int)Plot::Sandbar);
            ImGui::RadioButton("Caldera Island", &SelectedPlot, (int)Plot::Caldera);
            ImGui::RadioButton("Kevin Floating Islands", &SelectedPlot, (int)Plot::Kevin);
            ImGui::RadioButton("Black Glass Island", &SelectedPlot, (int)Plot::BlackGlass);
            ImGui::RadioButton("Grid Island", &SelectedPlot, (int)Plot::Grid);
            ImGui::RadioButton("The Block", &SelectedPlot, (int)Plot::Block);
            ImGui::RadioButton("Grassy Hill Island", &SelectedPlot, (int)Plot::GrassyHill);
            ImGui::RadioButton("Shoreline Island", &SelectedPlot, (int)Plot::Shoreline);
            ImGui::RadioButton("Archipelago Island", &SelectedPlot, (int)Plot::Archipelago);
            ImGui::RadioButton("Horseshoe Island", &SelectedPlot, (int)Plot::Horseshoe);
            ImGui::RadioButton("The Shark", &SelectedPlot, (int)Plot::Shark);
            ImGui::RadioButton("Floating Island Hub", &SelectedPlot, (int)Plot::FloatingHub);
            ImGui::RadioButton("Fortilla Island", &SelectedPlot, (int)Plot::Fortilla);
            ImGui::RadioButton("Debris Island", &SelectedPlot, (int)Plot::Debris);
            ImGui::RadioButton("Custom", &SelectedPlot, (int)Plot::Custom);

            switch (SelectedPlot)
            {
            case (int)Plot::Temperate:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Temperate_Medium.Temperate_Medium";
                break;
            }
            case (int)Plot::Meadow:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/FlatGrass_Large.FlatGrass_Large";
                break;
            }
            case (int)Plot::Arctic:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Arctic_Medium.Arctic_Medium";
                break;
            }
            case (int)Plot::Fortress:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Arctic_Competitive_Medium1.Arctic_Competitive_Medium1";
                break;
            }
            case (int)Plot::IceLake:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/IceLake_Large.IceLake_Large";
                break;
            }
            case (int)Plot::Canyon:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Desert_Large.Desert_Large";
                break;
            }
            case (int)Plot::Arid:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Pandora_Large.Pandora_Large";
                break;
            }
            case (int)Plot::Wasteland:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Desert_Large_02.Desert_Large_02";
                break;
            }
            case (int)Plot::Tropical:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Desert_Large_02.Desert_Large_02";
                break;
            }
            case (int)Plot::RiverEdge:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Military_Medium.Military_Medium";
                break;
            }
            case (int)Plot::Volcano:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Volcano_Large.Volcano_Large";
                break;
            }
            case (int)Plot::Sandbar:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Sandbar_Large.Sandbar_Large";
                break;
            }
            case (int)Plot::Caldera:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Volcano_Large_02.Volcano_Large_02";
                break;
            }
            case (int)Plot::Kevin:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Kevin_Large.Kevin_Large";
                break;
            }
            case (int)Plot::BlackGlass:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/BlackGlass_Medium.BlackGlass_Medium";
                break;
            }
            case (int)Plot::Grid:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/FlatGrid_Large.FlatGrid_Large";
                break;
            }
            case (int)Plot::Block:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/TheBlock_Season7.TheBlock_Season7";
                break;
            }
            case (int)Plot::GrassyHill:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/TheHub_01.TheHub_01";
                break;
            }
            case (int)Plot::Shoreline:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/FlatGrass_LargeV2.FlatGrass_LargeV2";
                break;
            }
            case (int)Plot::Archipelago:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/SandBar_LargeV2.SandBar_LargeV2";
                break;
            }
            case (int)Plot::Horseshoe:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Escape_Large.Escape_Large";
                break;
            }
            case (int)Plot::Shark:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Shark_Large.Shark_Large";
                break;
            }
            case (int)Plot::Fortilla:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Arena_Large_01.Arena_Large_01";
                break;
            }
            case (int)Plot::Debris:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Arena_Large_02.Arena_Large_02";
                break;
            }
            case (int)Plot::Custom:
            {
                break;
            }
            default:
            {
                break;
            }
            }

            break;
        }
        case 6:
        {
            ImGui::Text("Custom Map Configuration:");
            SmallSeparator(Width);

			ImGui::Checkbox("One Kill Ends Game", &FConfiguration::AutoEndGame);

            ImGui::Spacing();
			ImGui::Spacing();

            ImGui::Text("Use Custom Map (VERY EXPERIMENTAL):");
            SmallSeparator(Width);

            ImGui::RadioButton("Athena Faceoff", &SelectedMap, (int)Map::Faceoff);
            ImGui::RadioButton("Papaya (Party Royale)", &SelectedMap, (int)Map::Papaya);
            ImGui::RadioButton("The Combine", &SelectedMap, (int)Map::Crucible);
            ImGui::RadioButton("Flat Grid", &SelectedMap, (int)Map::FlatGrid);
            ImGui::RadioButton("Prop Hunt", &SelectedMap, (int)Map::PropHunt);

            switch (SelectedMap)
            {
            case (int)Map::Papaya:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Apollo/Maps/Special/Papaya/Apollo_Papaya";
                break;
            }
            case (int)Map::Crucible:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Maps/Crucible/Athena_Crucible";
                break;
            }
            case (int)Map::TutorialMap:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Maps/Tutorial/Athena_Tutorial_Map_A";
                break;
            }
            case (int)Map::EmptyTest:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Maps/Athena_EmptyTest";
                break;
            }
            case (int)Map::Faceoff:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Maps/Athena_Faceoff";
                break;
            }
            case (int)Map::Playground:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Maps/Athena_Playground";
                break;
            }
            case (int)Map::DADBRO:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Playlists/DADBRO/Athena_DADBRO_Apollo_Island";
                break;
            }
            case (int)Map::Kevin:
            {
                FConfiguration::CustomMap = L"open /Game/Creative/Maps/Islands/105x105/Kevin/Kevin_Floating_Island_105x105_Mesh";
                break;
            }
            case (int)Map::FlatGrid:
            {
                FConfiguration::CustomMap = L"open /Game/Creative/Maps/Islands/105x105/FlatGrid/FlatGrid_Island_105x105";
                break;
            }
            case (int)Map::PropHunt:
            {
                FConfiguration::CustomMap = L"open /Game/Creative/Maps/Islands/105x105/Loki/Loki_Island_105x105_M";
                break;
            }
            default:
            {
                break;
            }
            }

            break;
        }
        case 7:
        {
            ImGui::Text("Trickshot Customization:");
            SmallSeparator(Width);

            if (gsStatus < Joinable)
            {
                ImGui::Checkbox("Toggle Swag Lines", &FConfiguration::bUseWinLines);

                if (VersionInfo.FortniteVersion <= 23.50)
                    ImGui::Checkbox("Toggle Infinite Render", &FConfiguration::bInfiniteRender);

                if ((IsArenaPlaylist() || IsTournamentPlaylist()) && FConfiguration::bLateGame && !FConfiguration::bForceRespawns)
                    ImGui::Checkbox("Randomize Arena Points", &FConfiguration::RandomizeArenaPoints);

                if (FConfiguration::bLateGame)
                    ImGui::Checkbox("Randomize Kills", &FConfiguration::RandomizeKills);

                ImGui::Checkbox("Randomize Levels", &FConfiguration::RandomizeLevels);

                ImGui::Checkbox("Disable Jump Fatigue", &FConfiguration::bDisableJumpFatigue);
            }

            //ImGui::Checkbox("Make Projectiles Rideable (WIP)", &FConfiguration::bRideableProjectiles);

            if (VersionInfo.FortniteVersion >= 19.00)
                ImGui::Checkbox("Toggle Crown Slomo", &FConfiguration::bCrownSlomo);

            if (VersionInfo.FortniteVersion >= 23.20 && VersionInfo.FortniteVersion < 25.20)
                ImGui::Checkbox("Negate Velocity on Win", &FConfiguration::bCancelVelocityOnWin);

            //ImGui::Checkbox("Down But Not Out (DBNO)", &FConfiguration::bEnableDBNO);

            ImGui::Checkbox("Auto Pause TODM", &FConfiguration::bAutoPauseTODM);

            if (FConfiguration::bAutoPauseTODM)
            {
                ImGui::PushItemWidth(Width);
                ImGui::SliderInt("Time Of Day", &FConfiguration::TODMTime, 1, 24);
                ImGui::PopItemWidth();
            }

            if (FConfiguration::bRideableProjectiles)
            {
                static char ProjClassBuffer[512] = {};

                ImGui::InputText("Class", ProjClassBuffer, IM_ARRAYSIZE(ProjClassBuffer));

                if (ImGui::Button("Apply", ImVec2(Width, Height)))
                {
                    if (ProjClassBuffer[0] != '\0')
                    {
                        std::string ProjClass = ProjClassBuffer;
                        UClass* ProjectileClass = (UClass*)SDK::StaticLoadObject(UEAllocatedWString(ProjClass.begin(), ProjClass.end()).c_str(), SDK::UClass::StaticClass());

                        if (ProjectileClass)
                        {
                            static auto ProjectileBaseClass = FindClass("FortProjectileBase");

                            if (ProjectileClass->IsA(ProjectileBaseClass))
                            {
                                auto DefaultObject = ProjectileClass->GetDefaultObj();

                                if (DefaultObject)
                                {
                                    static auto CapsuleComponentOffset = ProjectileClass->GetOffset("CapsuleComponent");

                                    if (CapsuleComponentOffset != -1)
                                    {
                                        auto CapsuleComponent = GetFromOffset<UPrimitiveComponent*>(DefaultObject, CapsuleComponentOffset);

                                        if (CapsuleComponent)
                                        {
                                            static auto CanCharacterStepUpOnOffset = CapsuleComponent->GetOffset("bCanCharacterStepUpOn");

                                            if (CanCharacterStepUpOnOffset != -1)
                                            {
                                                GetFromOffset<bool>(CapsuleComponent, CanCharacterStepUpOnOffset) = true;
                                            }

                                            static auto WalkableSlopeOverrideOffset = CapsuleComponent->GetOffset("WalkableSlopeOverride");

                                            if (WalkableSlopeOverrideOffset != -1)
                                            {
                                                auto& SlopeOverride = GetFromOffset<FWalkableSlopeOverride>(CapsuleComponent, WalkableSlopeOverrideOffset);
                                                SlopeOverride.WalkableSlopeBehavior = EWalkableSlopeBehavior::WalkableSlope_Increase;
                                                SlopeOverride.WalkableSlopeAngle = 90.0f;
                                            }

                                            static auto CollisionEnabledOffset = CapsuleComponent->GetOffset("CollisionEnabled");

                                            if (CollisionEnabledOffset != -1)
                                            {
                                                GetFromOffset<ECollisionEnabled>(CapsuleComponent, CollisionEnabledOffset) = ECollisionEnabled::QueryOnly;
                                            }

                                            static auto SetCollisionEnabledFunc = CapsuleComponent->Class->GetFunction("SetCollisionEnabled");

                                            if (SetCollisionEnabledFunc)
                                            {
                                                struct { ECollisionEnabled NewType; } Params;
                                                Params.NewType = ECollisionEnabled::QueryOnly;
                                                CapsuleComponent->ProcessEvent(SetCollisionEnabledFunc, &Params);
                                            }

                                            static auto SetCollisionResponseToChannelFunc = CapsuleComponent->Class->GetFunction("SetCollisionResponseToChannel");

                                            if (SetCollisionResponseToChannelFunc)
                                            {
                                                struct { uint8 Channel; ECollisionResponse NewResponse; } Params;
                                                Params.Channel = 1;
                                                Params.NewResponse = ECollisionResponse::ECR_Block;
                                                CapsuleComponent->ProcessEvent(SetCollisionResponseToChannelFunc, &Params);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            break;
        }
        case 8:
        {
            auto rule = []()
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x;
                dl->AddLine(ImVec2(p.x, p.y + 1.f), ImVec2(p.x + w, p.y + 1.f), ImGui::GetColorU32(Accent(0.30f)), 1.f);
                ImGui::Dummy(ImVec2(0.f, 9.f));
            };
            auto credit = [](const char* name, const char* role, const char* url)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, Accent(0.95f));
                ImGui::TextUnformatted(name);
                ImGui::PopStyleColor();
                if (url && url[0])
                {
                    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ImGui::IsItemClicked()) ShellExecuteA(0, "open", url, 0, 0, SW_SHOWNORMAL);
                }
                ImGui::SameLine(0.f, 8.f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.60f, 0.66f, 1.f));
                ImGui::Text("- %s", role);
                ImGui::PopStyleColor();
                ImGui::Spacing();
            };

            // Header: logo + title
            if (g_LogoTexture)
            {
                ImGui::Image((void*)g_LogoTexture, ImVec2(44.f, 44.f));
                ImGui::SameLine(0.f, 12.f);
            }
            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_Text, Accent());
            ImGui::TextUnformatted("MAGNESIUM");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.54f, 0.56f, 0.62f, 1.f));
            ImGui::TextUnformatted("Gameserver  -  v2.0.0");
            ImGui::PopStyleColor();
            ImGui::EndGroup();

            ImGui::Dummy(ImVec2(0.f, 14.f));

            ImGui::PushStyleColor(ImGuiCol_Text, Accent(0.85f));
            ImGui::TextUnformatted("CORE");
            ImGui::PopStyleColor();
            rule();
            credit("Erbium", "Base of the project", "https://github.com/plooshi/Erbium");

            const bool anyContrib = FConfiguration::bInfiniteRender
                || SelectedPlaylist == static_cast<int>(Playlist::Gav)
                || SelectedPlaylist == static_cast<int>(Playlist::Retrac1v1)
                || SelectedPlaylist == static_cast<int>(Playlist::RetracTurtle)
                || SelectedPlaylist == static_cast<int>(Playlist::OnlyUp)
                || SelectedPlaylist == static_cast<int>(Playlist::TiltedZW);

            if (anyContrib)
            {
                ImGui::Dummy(ImVec2(0.f, 10.f));
                ImGui::PushStyleColor(ImGuiCol_Text, Accent(0.85f));
                ImGui::TextUnformatted("CONTRIBUTORS");
                ImGui::PopStyleColor();
                rule();

                if (FConfiguration::bInfiniteRender)
                    credit("Sweefy / Milxnor", "Infinite Render research", "https://x.com/Sweefyyy");
                if (SelectedPlaylist == static_cast<int>(Playlist::Gav))
                    credit("Gav", "Maker of the 27.11 1v1 map", "https://github.com/gavbowersdomain/27.11-Mods/tree/main/Mods/1v1");
                if (SelectedPlaylist == static_cast<int>(Playlist::Retrac1v1) || SelectedPlaylist == static_cast<int>(Playlist::RetracTurtle))
                    credit("Retrac", "Creator of the 1v1 & Turtle Fight maps", "https://discord.gg/retrac");
                if (SelectedPlaylist == static_cast<int>(Playlist::OnlyUp) || SelectedPlaylist == static_cast<int>(Playlist::TiltedZW))
                    credit("Jett", "Maker of the Only Up & Tilted FFA maps", "https://discord.com/channels/1469866169635962884/1473850399994806362/1473850399994806362");
            }

            ImGui::Dummy(ImVec2(0.f, 18.f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.44f, 0.50f, 1.f));
            ImGui::TextUnformatted("Thank you for using Magnesium.");
            ImGui::PopStyleColor();

            break;
        }
        default:
        {
            break;
        }
        }

        ImGui::Dummy(ImVec2(0.0f, 40.0f));
        ImGui::EndChild();      // content panel
        ImGui::PopStyleVar();   // content WindowPadding
        ImGui::PopStyleColor(); // content ChildBg
        ImGui::End();           // window


        ImGui::Render();
        const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (g_EmbedTexture)
        g_EmbedTexture->Release();

    g_pSwapChain->Release();
    g_pd3dDeviceContext->Release();
    g_pd3dDevice->Release();
    DestroyWindow(hWnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    TerminateProcess(GetCurrentProcess(), 0);
}
