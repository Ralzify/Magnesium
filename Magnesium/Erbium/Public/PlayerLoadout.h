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

namespace PlayerLoadout
{
    enum class EPreviewTextureLoadState : std::uint8_t
    {
        Unavailable, Pending, Resident
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

    bool HandleBridgeMessage(AFortPlayerControllerAthena* PlayerController,
        const std::string& Message) noexcept;

    // Game-thread only. Owner must be the live UObject that owns SoftReference.
    FPreviewTextureLoadResult ResolveOrRequestPreviewTexture(const void* Owner,
        const void* SoftReference, std::uint32_t SoftReferenceSize) noexcept;

    FSoftObjectLoadResult ResolveOrRequestSoftObject(const void* Owner, const void* SoftReference,
        std::uint32_t SoftReferenceSize, const SDK::UClass* ExpectedClass) noexcept;

    void GameThreadTick();
    void Render(AFortPlayerControllerAthena* PlayerController, float Width, ID3D11Device* Device);
    void RenderPicker();
    void ShutdownRenderer();
}

namespace GameTextureBridge
{
    bool ExtractToRGBA(const UTexture2D* Texture, std::vector<unsigned char>& RGBA, int& Width,
        int& Height);
}
