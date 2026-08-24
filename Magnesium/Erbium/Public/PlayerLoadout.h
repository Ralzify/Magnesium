#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ID3D11Device;
class AFortPlayerControllerAthena;
class UTexture2D;

namespace SDK
{
    class UObject;
    class UClass;
}

// Cross-thread player inventory editor used by the Players tab. Unreal object
// access stays in GameThreadTick(); Render() consumes plain cached snapshots.
namespace PlayerLoadout
{
    enum class EPreviewTextureLoadState : std::uint8_t
    {
        Unavailable,
        Pending,
        Resident
    };

    struct FPreviewTextureLoadResult
    {
        const UTexture2D* Texture;
        EPreviewTextureLoadState State;
        std::uint64_t RetryAfterMs;
    };

    struct FSoftObjectLoadResult
    {
        const SDK::UObject* Object;
        EPreviewTextureLoadState State;
        std::uint64_t RetryAfterMs;
    };

    // Consumes only the optional fixed-format ATLAS slot telemetry messages.
    // Returns false for every ordinary ServerCheat command.
    bool HandleBridgeMessage(
        AFortPlayerControllerAthena* PlayerController,
        const std::string& Message) noexcept;

    // Resolves an already-resident preview texture or schedules its validated
    // soft reference through the guarded, throttled reflected latent LoadAsset
    // path. This is game-thread-only. Owner must be the live UObject that owns
    // SoftReference.
    FPreviewTextureLoadResult ResolveOrRequestPreviewTexture(
        const void* Owner,
        const void* SoftReference,
        std::uint32_t SoftReferenceSize) noexcept;

    // Generic form of the same guarded resident/latent loader for deferred
    // gameplay cosmetics. ExpectedClass and Owner must be live game-thread
    // UObjects; Owner is the stable identity used to serialize that caller's
    // requests and need not contain the temporary soft pointer.
    FSoftObjectLoadResult ResolveOrRequestSoftObject(
        const void* Owner,
        const void* SoftReference,
        std::uint32_t SoftReferenceSize,
        const SDK::UClass* ExpectedClass) noexcept;

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
