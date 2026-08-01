#pragma once

namespace AutoHosting
{
    // Loads the profile for the running Fortnite version. Corrupt or incomplete
    // profiles fail closed and never arm automatic startup.
    void Initialize();

    // True only when a complete enabled profile was restored during startup.
    bool HasRestoredPreferences();

    // Countdown controls are driven by the GUI thread so the final preference
    // snapshot is published before bReadyToStart releases the server thread.
    void ArmCountdown();
    void CancelCountdown();
    bool IsCountdownActive();
    int GetRemainingSeconds();
    void TickCountdown();

    // The authoritative server lifecycle arms this once when a hosted match
    // reaches its terminal state. Auto Host runs close the full process ten
    // seconds later; manual hosts never arm this deadline.
    void OnAuthoritativeMatchEnded();
    void TickPostMatchShutdown();

    // Persist toggle/delay unconditionally. Full launch preferences are
    // auto-saved while Auto Host is enabled.
    void SaveIfChanged();
    void SaveNow(bool ForcePreferenceSnapshot = false);

    // Restores Magnesium defaults for the running Fortnite-version profile.
    // Before server start this also refreshes the live launcher controls.
    void ResetPreferences();
}
