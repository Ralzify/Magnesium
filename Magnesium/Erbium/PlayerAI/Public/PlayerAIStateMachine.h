#pragma once
// ============================================================================
// Magnesium PlayerAI - PlayerAIStateMachine
//
// Tiny state container with transition logging. Kept intentionally simple:
// the behavior modules decide *when* to transition; the state machine only
// stores the state, the entry time and logs the change for debugging.
// ============================================================================
#include "PlayerAITypes.h"

class PlayerAIStateMachine
{
public:
    EPlayerAIState GetState() const { return State; }
    float GetStateEnterTime() const { return StateEnterTime; }

    // Transition with logging; OwnerName/Reason are used for debug output.
    void Transition(EPlayerAIState NewState, float Now, const char* OwnerName, const char* Reason);

    bool IsInState(EPlayerAIState Query) const { return State == Query; }

private:
    EPlayerAIState State = EPlayerAIState::PreMatchIdle;
    float StateEnterTime = 0.f;
};
