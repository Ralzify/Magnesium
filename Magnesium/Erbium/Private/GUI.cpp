#include "pch.h"
#include "../Public/GUI.h"
#include <d3d11.h>
#include "../../ImGui/imgui.h"
#include "../../ImGui/imgui_impl_win32.h"
#include "../../ImGui/imgui_impl_dx11.h"
#include "../Public/Configuration.h"
#include "../Public/Events.h"
#include "../../FortniteGame/Public/BattleRoyaleGamePhaseLogic.h"
#include "../../FortniteGame/Public/BuildingSMActor.h"
#include "../../Engine/Public/NetDriver.h"
#include <sstream>
#include <fstream>
#include <string>
#include <Windows.h>
#include <chrono>
#pragma comment(lib, "d3d11.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

UINT g_ResizeWidth = 0, g_ResizeHeight = 0;

void SmallSeparator(float Width, float Thickness = 1.0f)
{
    ImVec2 Pos = ImGui::GetCursorScreenPos();
    auto* Draw = ImGui::GetWindowDrawList();

    Draw->AddLine(Pos, ImVec2(Pos.x + Width, Pos.y), ImGui::GetColorU32(ImGuiCol_Separator), Thickness);

    ImGui::Dummy(ImVec2(Width, Thickness + 4));
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

inline FString* GetRequestURL(UObject* Connection)
{
    if (VersionInfo.EngineVersion <= 4.20)
        return (FString*)(__int64(Connection) + 432);
    if (std::floor(VersionInfo.FortniteVersion) >= 5 && VersionInfo.EngineVersion < 4.24)
        return (FString*)(__int64(Connection) + 424);
    else if (VersionInfo.EngineVersion >= 4.24)
        return (FString*)(__int64(Connection) + 440);

    return nullptr;
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
    style.Colors[ImGuiCol_Text] = ImVec4(0.85f, 0.95f, 0.90f, 0.80f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.85f, 0.95f, 0.90f, 0.30f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.29f, 0.29f, 0.29f, 1.00f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.20f, 0.22f, 0.27f, 0.75f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.29f, 0.29f, 0.29f, 1.00f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.29f, 0.29f, 0.29f, 1.00f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.43f, 0.43f, 0.43f, 0.85f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.46f, 0.46f, 0.46f, 1.00f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.20f, 0.22f, 0.27f, 0.47f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.67f, 0.67f, 0.67f, 1.00f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.20f, 0.20f, 0.20f, 0.67f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.59f, 0.59f, 0.59f, 1.00f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.25f, 0.25f, 0.25f, 0.75f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.43f, 0.43f, 0.43f, 0.85f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.46f, 0.46f, 0.46f, 1.00f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.92f, 0.18f, 0.29f, 0.76f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.92f, 0.18f, 0.29f, 0.86f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.92f, 0.18f, 0.29f, 1.00f);
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.92f, 0.18f, 0.29f, 0.43f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.20f, 0.22f, 0.27f, 0.9f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.29f, 0.29f, 0.29f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.32f, 0.32f, 0.32f, 1.0f);
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

        main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(WindowWidth * main_scale, WindowHeight * main_scale), ImGuiCond_Always);

        ImGui::Begin("Magnesium", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);
        ImGui::BeginChild("MainScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

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
        if (ImGui::BeginTabBar(""))
        {
            if (ImGui::BeginTabItem("Match"))
            {
                SelectedUI = 0;
                ImGui::EndTabItem();
            }

            if (!FConfiguration::bReadyToStart)
            {
                if (ImGui::BeginTabItem("Playlist"))
                {
                    SelectedUI = 1;
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Player Bot"))
                {
                    SelectedUI = 4;
                    ImGui::EndTabItem();
                }
            }

            /*if (gsStatus >= Joinable)
            {
                if (ImGui::BeginTabItem("Players"))
                {
                    SelectedUI = 2;
                    ImGui::EndTabItem();
                }
            }*/

            if (ImGui::BeginTabItem("Dump"))
            {
                SelectedUI = 3;
                ImGui::EndTabItem();
            }

            if (SelectedPlaylist == static_cast<int>(Playlist::Creative) && !FConfiguration::bReadyToStart)
            {
                if (ImGui::BeginTabItem("Creative"))
                {
                    SelectedUI = 5;
                    ImGui::EndTabItem();
                }
            }

            ImGui::EndTabBar();
        }

        float Width = 260.0f;
        float Height = 0.0f;

        static char commandBuffer[1024] = { 0 };
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

            ImGui::Text("- Server Port: %d", FConfiguration::Port);
            // ImGui::Text("- MaxTickRate: %.1f", FConfiguration::MaxTickRate);

            if (gsStatus >= Joinable)
            {
                auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;

                auto Playlist = VersionInfo.FortniteVersion >= 3.5 && GameMode->HasWarmupRequiredPlayerCount() ? (GameMode->GameState->HasCurrentPlaylistInfo() ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : GameMode->GameState->CurrentPlaylistData) : nullptr;

                if (Playlist)
                {
                    FString Name = UKismetTextLibrary::Conv_TextToString(Playlist->UIDisplayName);
                    ImGui::Text((UEAllocatedString("- Playlist: ") + Name.ToString()).c_str());
                }

                int AliveCount = 0;

                if (GameMode)
                    AliveCount = GameMode->AlivePlayers.Num();

                ImGui::Text("- Players: %d", AliveCount);

                ImGui::Text((std::string("- Uptime: ") + std::to_string((int)floor(UGameplayStatics::GetTimeSeconds(GameMode))) + "s").c_str());

                /*static std::string LastElimStatusMessage;
                static std::chrono::high_resolution_clock::time_point AddMessageTime;

                if (!FConfiguration::ElimStatusMessage.empty() && FConfiguration::ElimStatusMessage != LastElimStatusMessage)
                {
                    LastElimStatusMessage = FConfiguration::ElimStatusMessage;
                    AddMessageTime = std::chrono::high_resolution_clock::now();
                }

                if (!LastElimStatusMessage.empty() && duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - AddMessageTime).count() < 15)
                {
                    ImVec4 KillerColor = ImVec4(0x4e / 255.f, 0x86 / 255.f, 0xa5 / 255.f, 1.0f); // #4e86a5
                    ImVec4 EliminatedColor = ImVec4(0xa5 / 255.f, 0x56 / 255.f, 0x4c / 255.f, 1.0f); // #a5564c

                    ImGui::TextUnformatted("- ");
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextColored(KillerColor, "%s", FConfiguration::Elim_KillerName.c_str());
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextUnformatted(" eliminated ");
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextColored(EliminatedColor, "%s", FConfiguration::Elim_EliminatedName.c_str());
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextUnformatted(" from ");
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::Text("%sm!", FConfiguration::Elim_Distance.c_str());
                }
                else
                {
                    LastElimStatusMessage.clear();
                    FConfiguration::ElimStatusMessage.clear();
                }

                ImGui::NewLine();*/
            }

            if (gsStatus <= Joinable)
            {
                ImGui::Spacing();
                ImGui::Spacing();

                auto GavMap = wcsstr(FConfiguration::Playlist, L"/Game/Gav/Levels/GM_1v1/Playlist_Arena_DefaultSolo_Respawn.Playlist_Arena_DefaultSolo_Respawn");

                ImGui::Text("Pre-Game Configuration:");
                SmallSeparator(Width);

                if (GavMap)
                {
                    ImGui::Text("- Playing Gav 1v1 Map.");
				}
                else
                {
                    ImGui::Checkbox("Auto Bus Start", &FConfiguration::bAutoBusStart);

                    if (!FConfiguration::bReadyToStart)
                    {
                        if (VersionInfo.FortniteVersion <= 23.50)
                            ImGui::Checkbox("Toggle Infinite Render", &FConfiguration::bInfiniteRender);
                    }

                    if (gsStatus <= Joinable)
                    {
                        static bool bInitializedZone = false;

                        if (!bInitializedZone)
                        {
                            FConfiguration::LateGameZone = FConfiguration::IsS27() ? 3 : 4;
                            bInitializedZone = true;
                        }

                        ImGui::Checkbox("Lategame", &FConfiguration::bLateGame);

                        if (FConfiguration::bLateGame)
                        {
                            ImGui::Checkbox("Infinite Respawns (Requires Console DLL)", &FConfiguration::bForceRespawns);
                            ImGui::Checkbox("Use Long Zone", &FConfiguration::bLateGameLongZone);

                            ImGui::PushItemWidth(Width);
                            ImGui::SliderInt("Starting Zone", &FConfiguration::LateGameZone, 1, 7);
                            ImGui::PopItemWidth();
                        }
                    }

                    ImGui::Spacing();

                    if (gsStatus == Joinable && ImGui::Button("Start Bus Early", ImVec2(Width, Height)))
                    {
                        if (UFortGameStateComponent_BattleRoyaleGamePhaseLogic::GetDefaultObj())
                        {
                            UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bStartAircraft = true;
                            //auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get();

                            //GamePhaseLogic->StartAircraftPhase();
                        }
                        else
                            UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"startaircraft"), nullptr);
                    }
                }
            }

            if (gsStatus >= Joinable)
            {
                ImGui::Spacing();
                ImGui::Spacing();

                ImGui::Text("Match Settings:");
                SmallSeparator(Width);

                ImGui::Checkbox("Infinite Materials", &FConfiguration::bInfiniteMats);
                ImGui::Checkbox("Infinite Ammo", &FConfiguration::bInfiniteAmmo);
                ImGui::Checkbox("Toggle Cheat Commands", &FConfiguration::bEnableCheats);
                ImGui::Checkbox("Siphon", &FConfiguration::bSiphon);

                if (FConfiguration::bSiphon)
                {
                    ImGui::SetNextItemWidth(260.0f);
                    ImGui::InputInt("Siphon Amount", &FConfiguration::SiphonAmount);
                }

                if (ImGui::Button("Reset Builds", ImVec2(Width, Height)))
                {
                    TArray<ABuildingSMActor*> Builds;
                    Utils::GetAll<ABuildingSMActor>(Builds);

                    for (auto& Build : Builds) // this
                    {
                        if (Build && Build->bPlayerPlaced)
                            Build->K2_DestroyActor();
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

                if (ImGui::Button("Pause Safe Zone", ImVec2(Width, Height)))
                {
                    UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bPausedZone = true;
                    auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
                    if (GameMode->HasbSafeZonePaused())
                        GameMode->bSafeZonePaused = true;
                    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"pausesafezone"), nullptr);
                }

                if (ImGui::Button("Resume Safe Zone", ImVec2(Width, Height)))
                {
                    UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bPausedZone = false;
                    auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
                    if (GameMode->HasbSafeZonePaused())
                        GameMode->bSafeZonePaused = false;
                    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"startsafezone"), nullptr);
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

            if (gsStatus == StartedMatch)
            {
                if (hasEvent == 2)
                {
                    ImGui::Spacing();
                    ImGui::Spacing();

                    ImGui::Text("Event:");
                    SmallSeparator(Width);

                    if (ImGui::Button("Start Event", ImVec2(Width, Height)))
                        Events::StartEvent();
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

                if (ImGui::Button("Execute"))
                {
                    std::string str = commandBuffer;
                    auto wstr = std::wstring(str.begin(), str.end());

                    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(wstr.c_str()), nullptr);
                }
            }

            if (gsStatus == NotReady)
            {
                ImGui::Spacing();
                ImGui::Spacing();
                ImGui::Spacing();

                if (!FConfiguration::bReadyToStart)
                {
                    if (ImGui::Button("Start the Server", ImVec2(Width, 30)))
                    {
                        FConfiguration::bReadyToStart = true;
                    }
                }
            }

            break;
        }
        case 1:
        {
            ImGui::Text("Gamemodes:");
            SmallSeparator(Width);

            ImGui::RadioButton("Solos", &SelectedPlaylist, (int)Playlist::Solos);
            ImGui::RadioButton("Duos", &SelectedPlaylist, (int)Playlist::Duos);
            ImGui::RadioButton("Trios", &SelectedPlaylist, (int)Playlist::Trios);
            ImGui::RadioButton("Squads", &SelectedPlaylist, (int)Playlist::Squads);
            ImGui::RadioButton("One Shot Solos", &SelectedPlaylist, (int)Playlist::OneShotSolos);
            ImGui::RadioButton("One Shot Duos", &SelectedPlaylist, (int)Playlist::OneShotDuos);
            ImGui::RadioButton("One Shot Squads", &SelectedPlaylist, (int)Playlist::OneShotSquads);
            ImGui::RadioButton("Siphon Solos", &SelectedPlaylist, (int)Playlist::SiphonSolos);
            ImGui::RadioButton("Siphon Duos", &SelectedPlaylist, (int)Playlist::SiphonDuos);
            ImGui::RadioButton("Siphon Squads", &SelectedPlaylist, (int)Playlist::SiphonSquads);
            ImGui::RadioButton("Slide Solos", &SelectedPlaylist, (int)Playlist::SlideSolos);
            ImGui::RadioButton("Slide Duos", &SelectedPlaylist, (int)Playlist::SlideDuos);
            ImGui::RadioButton("Arena Solos", &SelectedPlaylist, (int)Playlist::TournamentSolos);
            ImGui::RadioButton("Arena Duos", &SelectedPlaylist, (int)Playlist::TournamentDuos);
            ImGui::RadioButton("Arena Trios", &SelectedPlaylist, (int)Playlist::TournamentTrios);
            ImGui::RadioButton("Arena Squads", &SelectedPlaylist, (int)Playlist::TournamentSquads);
            ImGui::RadioButton("Creative ", &SelectedPlaylist, (int)Playlist::Creative);
            ImGui::RadioButton("Custom", &SelectedPlaylist, (int)Playlist::Custom);

            if (VersionInfo.FortniteVersion == 14.40 || VersionInfo.FortniteVersion == 27.11)
            {
                ImGui::Spacing();

                ImGui::Text("Custom Maps (require additional paks):");
                SmallSeparator(Width);

                if (VersionInfo.FortniteVersion == 27.11)
                    ImGui::RadioButton("Gav 1v1 Map", &SelectedPlaylist, (int)Playlist::Gav);

                if (VersionInfo.FortniteVersion == 14.40)
                {
                    ImGui::RadioButton("Retrac 1v1 Map", &SelectedPlaylist, (int)Playlist::Retrac1v1);
                    ImGui::RadioButton("Retrac Turtle Fights", &SelectedPlaylist, (int)Playlist::RetracTurtle);
                }
            }

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
            case (int)Playlist::SlideSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
                break;
            }
            case (int)Playlist::SlideDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Slide/Playlist_Slide_Duos.Playlist_Slide_Duos";
                break;
            }
            case (int)Playlist::TournamentSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Solo.Playlist_ShowdownAlt_Solo";
                break;
            }
            case (int)Playlist::TournamentDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Duos.Playlist_ShowdownAlt_Duos";
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
            case (int)Playlist::Custom:
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
        case 2:
        {
            auto World = UWorld::GetWorld();

            if (World)
            {
                UObject* NetDriver = World->NetDriver;

                if (!NetDriver)
                    return;

                UNetDriver* Driver = static_cast<UNetDriver*>(NetDriver);

                auto& ClientConnections = Driver->ClientConnections;

                for (int i = 0; i < ClientConnections.Num(); i++)
                {
                    auto Connection = ClientConnections[i];

                    if (!Connection)
                        continue;

                    auto CurrentController = Connection->PlayerController;

                    if (CurrentController)
                    {
                        auto FindAllControllers = std::find_if(AllControllers.begin(), AllControllers.end(), [CurrentController, Connection](const auto& pair)
                            {
                                return pair.first == CurrentController && pair.second == Connection;
                            });

                        if (FindAllControllers == AllControllers.end())
                            AllControllers.push_back(std::make_pair(CurrentController, Connection));
                    }
                }

                ImGui::Text(("Players Connected: " + std::to_string(AllControllers.size())).c_str());

                for (int i = 0; i < AllControllers.size(); i++)
                {
                    auto& CurrentPair = AllControllers[i];
                    auto PlayerController = CurrentPair.first;
                    auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;

                    if (!PlayerController || !PlayerState)
                        continue;

                    auto PlayerName = PlayerState->GetPlayerName();

                    std::string DisplayName = PlayerName.c_str();

                    if (DisplayName.empty())
                        DisplayName = "Player";

                    ImGui::PushID(i);

                    if (ImGui::Button(DisplayName.c_str()))
                    {
                        SelectedUI = i;
                    }

                    ImGui::PopID();
                }
            }

            break;
        }
        case 3:
        {
            static auto PlaylistClass = UFortPlaylistAthena::StaticClass();

            if (ImGui::Button("Dump Items"))
            {
                std::stringstream ss;

                ss << "Generated by Erbium (https://github.com/plooshi/Erbium)\n";
                char version[6];

                sprintf_s(version, VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "%.2f" : "%.1f", VersionInfo.FortniteVersion);
                ss << "Fortnite Version: " << version << "\n\n";

                auto RarityEnum = EFortRarity::StaticEnum();
                for (int i = 0; i < TUObjectArray::Num(); i++)
                {
                    auto Object = TUObjectArray::GetObjectByIndex(i);
                    if (!Object || !Object->Class || Object->IsDefaultObject() || !Object->IsA<UFortWorldItemDefinition>())
                        continue;
                    auto Item = (UFortWorldItemDefinition*)Object;

                    FString Name = UKismetTextLibrary::Conv_TextToString(Item->HasDisplayName() ? Item->DisplayName : Item->ItemName);

                    ss << "- " << UKismetSystemLibrary::GetPathName(Item).ToString() << "\n";
                    ss << "-     Name: " << (Name.GetData() ? Name.ToString() : "None") << "\n";

                    auto Names = *(TArray<TPair<FName, int64>>*)(__int64(RarityEnum) + 0x40);

                    for (int i = 0; i < Names.Num(); i++)
                    {
                        auto& Pair = Names[i];
                        auto& Name = Pair.Key();
                        auto& Value = Pair.Value();

                        if (Value == Item->Rarity)
                        {
                            auto str = Name.ToString();
                            auto colcolIdx = str.find_last_of("::");

                            auto RealName = colcolIdx == -1 ? str : str.substr(colcolIdx + 1);

                            ss << "-     Rarity: " << RealName << "\n";
                        }
                    }
                }

                std::ofstream of("DumpedItems.txt", std::ios::trunc);

                of << ss.str();
                of.close();
            }
            else if (PlaylistClass && ImGui::Button("Dump Playlists"))
            {
                std::stringstream ss;

                ss << "Generated by Erbium (https://github.com/plooshi/Erbium)\n";
                char version[6];

                sprintf_s(version, VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "%.2f" : "%.1f", VersionInfo.FortniteVersion);
                ss << "Fortnite Version: " << version << "\n\n";

                auto RarityEnum = EFortRarity::StaticEnum();
                for (int i = 0; i < TUObjectArray::Num(); i++)
                {
                    auto Object = TUObjectArray::GetObjectByIndex(i);
                    if (!Object || !Object->Class || Object->IsDefaultObject() || !Object->IsA<UFortPlaylistAthena>())
                        continue;
                    auto Playlist = (UFortPlaylistAthena*)Object;

                    FString Name = UKismetTextLibrary::Conv_TextToString(Playlist->UIDisplayName);

                    ss << "- " << UKismetSystemLibrary::GetPathName(Playlist).ToString() << "\n";
                    ss << "-     Name: " << (Name.GetData() ? Name.ToString() : "None") << "\n";
                    if (Playlist->HasMaxPlayers())
                        ss << "-     Max Players: " << std::to_string(Playlist->MaxPlayers) << "\n";
                    if (Playlist->HasMaxSquadSize())
                        ss << "-     Squad Size: " << std::to_string(Playlist->MaxSquadSize) << "\n";
                }

                std::ofstream of("DumpedPlaylists.txt", std::ios::trunc);

                of << ss.str();
                of.close();
            }

            break;
        }
        case 4:
        {
            ImGui::InputInt("Bot Health", &FConfiguration::BotHealth);
            ImGui::InputInt("Bot Shield", &FConfiguration::BotShield);

            ImGui::Spacing();

            ImGui::Checkbox("Use Custom Bot Names", &FConfiguration::UseCustomBotNames);

            if (FConfiguration::UseCustomBotNames)
            {
                static char BotNameBuffer[64] = {};

                if (BotNameBuffer[0] == '\0' && !FConfiguration::BotName.empty())
                {
                    strncpy_s(
                        BotNameBuffer,
                        sizeof(BotNameBuffer),
                        FConfiguration::BotName.c_str(),
                        _TRUNCATE
                    );
                }

                ImGui::InputText("Bot Name", BotNameBuffer, sizeof(BotNameBuffer));

                if (ImGui::Button("Apply Bot Name"))
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
        }

        ImGui::Dummy(ImVec2(0.0f, 40.0f));
        ImGui::EndChild();
        ImGui::End();


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

    g_pSwapChain->Release();
    g_pd3dDeviceContext->Release();
    g_pd3dDevice->Release();
    DestroyWindow(hWnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    TerminateProcess(GetCurrentProcess(), 0);
}