#pragma once

namespace SDK
{
	class UWorld;
}

class UNetDriver;

namespace HotfixReload
{
	// Called from the authoritative NetDriver game-thread tick. Fortnite's
	// native hotfix manager owns the actual title-file fetch and table update.
	void Tick(SDK::UWorld* World, UNetDriver* Driver);
}
