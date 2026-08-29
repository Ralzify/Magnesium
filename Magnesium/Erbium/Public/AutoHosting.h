#pragma once

namespace AutoHosting
{
    void Initialize();

    bool HasRestoredPreferences();

    void ArmCountdown();
    void CancelCountdown();
    bool IsCountdownActive();
    int GetRemainingSeconds();
    void TickCountdown();

    void OnAuthoritativeMatchEnded();
    void TickPostMatchShutdown();

    void SaveIfChanged();
    void SaveNow(bool ForcePreferenceSnapshot = false);

    void RequestCustomSafeZonePreferenceRefresh();

    void ResetPreferences();
}
