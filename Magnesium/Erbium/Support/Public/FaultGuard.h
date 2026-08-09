#pragma once
// ============================================================================
// Magnesium - fault guard cooperation flag
//
// The Magnesium crash reporter uses a *vectored* exception handler, which
// runs before any frame based __try/__except. The support layer wraps its
// risky native calls in SEH guards so a faulting engine call degrades a
// feature instead of crashing the gameserver - but that only works if the
// vectored handler lets the exception reach the frame handler.
//
// While this counter is non-zero on a thread, such an SEH guard is active on
// that thread and will contain the fault; the crash reporter checks it and
// returns EXCEPTION_CONTINUE_SEARCH instead of terminating. Everything else
// (all unguarded crashes) is reported exactly as before.
//
// Header is intentionally dependency-free.
// ============================================================================

inline thread_local int GGuardedNativeCallDepth = 0;

