#pragma once

namespace AutoHosting
{
    // Loads Auto Host controls for the running Fortnite version. Optional full
    // preferences are restored only when Save Settings is enabled and valid.
    void Initialize();

    // True only when Save Settings restored a complete preference snapshot.
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

    // Persist Auto Host, delay, and Save Settings unconditionally. Full launch
    // preferences are saved only while Save Settings is enabled.
    void SaveIfChanged();
    void SaveNow(bool ForcePreferenceSnapshot = false);

    // Restores Magnesium defaults and removes every saved version profile.
    // Before server start this also refreshes the live launcher controls.
    void ResetPreferences();
}
