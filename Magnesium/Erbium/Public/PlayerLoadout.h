#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ID3D11Device;
class AFortPlayerControllerAthena;
class UTexture2D;

// Cross-thread player inventory editor used by the Players tab. Unreal object
// access stays in GameThreadTick(); Render() consumes plain cached snapshots.
namespace PlayerLoadout
{
    // Consumes only the optional fixed-format ATLAS slot telemetry messages.
    // Returns false for every ordinary ServerCheat command.
    bool HandleBridgeMessage(
        AFortPlayerControllerAthena* PlayerController,
        const std::string& Message) noexcept;
    void GameThreadTick();
    void Render(
        AFortPlayerControllerAthena* PlayerController,
        float Width,
        ID3D11Device* Device);
    // Must be called once every ImGui frame so an open modal can dismiss itself
    // if the inspected player disappears or the operator changes tabs.
    void RenderPicker();
    void ShutdownRenderer();
}

// Implemented beside the existing Safe Zone texture bridge in GUI.cpp. Texture
// extraction must only be requested from PlayerLoadout::GameThreadTick().
namespace GameTextureBridge
{
    bool ExtractToRGBA(
        const UTexture2D* Texture,
        std::vector<unsigned char>& RGBA,
        int& Width,
        int& Height);
}
