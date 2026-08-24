#include "pch.h"
#include "../Public/AbilitySystemComponent.h"
#include "../../Erbium/Public/Finders.h"
#include "../../FortniteGame/Public/FortKismetLibrary.h"
#include "../../FortniteGame/Public/FortWeapon.h"

uint64 ConstructAbilitySpec;
uint64 GiveAbility_;
static uint64 ClearAbilityAddress;

FGameplayAbilitySpecHandle UAbilitySystemComponent::GiveAbility(
    const UObject* Ability, UObject* SourceObject,
    int32 Level, int32 InputID)
{
    if (!this || !Ability || !GiveAbility_)
        return {};
    // printf("GiveAbility[%s]\n", Ability->Name.ToString().c_str());

    auto Spec = (FGameplayAbilitySpec*)malloc(FGameplayAbilitySpec::Size());
    memset(PBYTE(Spec), 0, FGameplayAbilitySpec::Size());

    if (ConstructAbilitySpec)
        ((void (*)(FGameplayAbilitySpec*, const UObject*, int, int, UObject*))ConstructAbilitySpec)(Spec, Ability, Level, InputID, SourceObject);
    else
    {
        Spec->MostRecentArrayReplicationKey = -1;
        Spec->ReplicationID = -1;
        Spec->ReplicationKey = -1;
        Spec->Ability = (UFortGameplayAbility*)Ability;
        Spec->Level = Level;
        Spec->InputID = InputID;
        Spec->Handle.Handle = rand();
        Spec->SourceObject = SourceObject;
    }

    FGameplayAbilitySpecHandle OutHandle{};
    ((FGameplayAbilitySpecHandle * (*)(UAbilitySystemComponent*, FGameplayAbilitySpecHandle*, __int64)) GiveAbility_)(this, &OutHandle, __int64(Spec));
    free(Spec);
    return OutHandle;
}

bool UAbilitySystemComponent::ClearAbility(FGameplayAbilitySpecHandle Handle)
{
    if (!this || !ClearAbilityAddress)
        return false;

    auto ClearAbilityInternal =
        (void(*)(UAbilitySystemComponent*,
            FGameplayAbilitySpecHandle&))ClearAbilityAddress;
    ClearAbilityInternal(this, Handle);
    return true;
}

void UAbilitySystemComponent::GiveAbilitySet(const UFortAbilitySet* Set)
{
    TScriptInterface<class IFortAbilitySystemInterface> ScriptInterface;
    ScriptInterface.ObjectPointer = this->GetOwner();
    ScriptInterface.InterfacePointer = ScriptInterface.ObjectPointer->GetInterface(IFortAbilitySystemInterface::StaticClass());

    if (VersionInfo.EngineVersion >= 4.19 && ScriptInterface.ObjectPointer && ScriptInterface.InterfacePointer)
        UFortKismetLibrary::EquipFortAbilitySet(ScriptInterface, Set, nullptr);
    else if (Set)
    {
        // printf("GiveAbilitySet[%s]\n", Set->Name.ToString().c_str());
        for (auto& GameplayAbility : Set->GameplayAbilities)
            GiveAbility(GameplayAbility->GetDefaultObj());
        if (Set->HasGrantedGameplayEffects())
            for (int i = 0; i < Set->GrantedGameplayEffects.Num(); i++)
            {
                auto& GameplayEffect = Set->GrantedGameplayEffects.Get(i, FGameplayEffectApplicationInfoHard::Size());

                BP_ApplyGameplayEffectToSelf(GameplayEffect.GameplayEffect.Get(), GameplayEffect.Level, MakeEffectContext());
            }
    }
}

struct _Pad_0x10
{
    uint8_t Padding[0x10];
};

struct _Pad_0x18
{
    uint8_t Padding[0x18];
};

uint64_t InternalTryActivateAbility_ = 0;

void UAbilitySystemComponent::InternalServerTryActivateAbility(
    UAbilitySystemComponent* AbilitySystemComponent, FGameplayAbilitySpecHandle Handle, bool InputPressed, FPredictionKey* PredictionKey, void* TriggerEventData)
{
    if (!AbilitySystemComponent || !PredictionKey ||
        !InternalTryActivateAbility_)
        return;

    auto Spec = AbilitySystemComponent->ActivatableAbilities.Items.Search(
        [&](FGameplayAbilitySpec& item)
        {
            return item.Handle.Handle == Handle.Handle;
        },
        FGameplayAbilitySpec::Size());

    if (!Spec)
        return AbilitySystemComponent->ClientActivateAbilityFailed(Handle, PredictionKey->Current);

    // Instant abilities may remove their granting gameplay effect and mutate
    // ActivatableAbilities while InternalTryActivateAbility is unwinding.
    // Keep only stable values across that call and resolve the spec again
    // afterward before marking it dirty.
    UFortGameplayAbility* AbilityToActivate = Spec->Ability;
    Spec->InputPressed = true;
    UObject* AbilitySourceObject =
        Spec->HasSourceObject()
            ? Spec->SourceObject
            : nullptr;
    AFortWeaponRanged::
        NotifyServerAbilityActivationStarted(
            AbilitySourceObject);

    UFortGameplayAbility* InstancedAbility = nullptr;
    auto InternalTryActivateAbility = (bool (*)(UAbilitySystemComponent*, FGameplayAbilitySpecHandle, _Pad_0x10, UFortGameplayAbility**, void*, void*))InternalTryActivateAbility_;
    auto InternalTryActivateAbilityNew
        = (bool (*)(UAbilitySystemComponent*, FGameplayAbilitySpecHandle, _Pad_0x18, UFortGameplayAbility**, void*, void*))InternalTryActivateAbility_;

    const bool Activated =
        FPredictionKey::Size() == 0x18
            ? InternalTryActivateAbilityNew(
                AbilitySystemComponent,
                Handle,
                *(_Pad_0x18*)PredictionKey,
                &InstancedAbility,
                nullptr,
                TriggerEventData)
            : InternalTryActivateAbility(
                AbilitySystemComponent,
                Handle,
                *(_Pad_0x10*)PredictionKey,
                &InstancedAbility,
                nullptr,
                TriggerEventData);
    if (!Activated)
    {
        AFortWeaponRanged::
            NotifyServerAbilityActivationFailed(
                AbilitySourceObject);
        AbilitySystemComponent->ClientActivateAbilityFailed(Handle, PredictionKey->Current);
    }
    else
    {
        AFortWeaponRanged::NotifyServerAbilityActivated(
            AbilitySourceObject);
        AFortInventory::NotifyGhostModeExitAbilityActivated(
            AbilitySystemComponent,
            AbilityToActivate);
    }

    auto PostActivationSpec =
        AbilitySystemComponent->ActivatableAbilities.Items.Search(
            [&](FGameplayAbilitySpec& Item)
            {
                return Item.Handle.Handle == Handle.Handle;
            },
            FGameplayAbilitySpec::Size());
    if (PostActivationSpec)
    {
        if (!Activated)
            PostActivationSpec->InputPressed = false;

        // UE 4.21's native RPC path expects this explicit dirty mark after
        // both successful and failed activation. Without it, instant exit
        // abilities can remain client-only and their state never reaches the
        // authoritative Ghost Mode lifecycle.
        if (VersionInfo.EngineVersion <= 4.21 || !Activated)
        {
            AbilitySystemComponent->ActivatableAbilities
                .MarkItemDirty(*PostActivationSpec);
        }
    }
}

void UFortGameplayAbility::K2_AddGameplayCueWithParams_(UObject* Context, FFrame& Stack)
{
    auto& GameplayCueTag = Stack.StepCompiledInRef<FGameplayTag>();
    auto& GameplayCueParameter = Stack.StepCompiledInRef<FGameplayCueParameters>();
    bool bRemoveOnAbilityEnd;

    Stack.StepCompiledIn(&bRemoveOnAbilityEnd);
    Stack.IncrementCode();

    auto Ability = (UFortGameplayAbility*)Context;
    callOG(Ability, Stack.GetCurrentNativeFunction(), K2_AddGameplayCueWithParams, GameplayCueTag, GameplayCueParameter, bRemoveOnAbilityEnd);

    auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
    memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());

    auto AbilitySystemComponent = (UAbilitySystemComponent*)Ability->GetAbilitySystemComponentFromActorInfo();

    // AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(GameplayCueTag, *PredictionKey, EffectContext);
    if (AbilitySystemComponent)
        AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded_WithParams(GameplayCueTag, *PredictionKey, GameplayCueParameter);

    free(PredictionKey);
}

void UFortGameplayAbility::K2_AddGameplayCue_(UObject* Context, FFrame& Stack)
{
    auto& GameplayCueTag = Stack.StepCompiledInRef<FGameplayTag>();
    auto& EffectContext = Stack.StepCompiledInRef<FGameplayEffectContextHandle>();
    bool bRemoveOnAbilityEnd;

    Stack.StepCompiledIn(&bRemoveOnAbilityEnd);
    Stack.IncrementCode();

    auto Ability = (UFortGameplayAbility*)Context;
    callOG(Ability, Stack.GetCurrentNativeFunction(), K2_AddGameplayCue, GameplayCueTag, EffectContext, bRemoveOnAbilityEnd);

    auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
    memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());

    auto AbilitySystemComponent = (UAbilitySystemComponent*)Ability->GetAbilitySystemComponentFromActorInfo();

    if (AbilitySystemComponent)
        AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(GameplayCueTag, *PredictionKey, EffectContext);

    free(PredictionKey);
}

void UFortGameplayAbility::K2_ExecuteGameplayCue_(UObject* Context, FFrame& Stack)
{
    auto& GameplayCueTag = Stack.StepCompiledInRef<FGameplayTag>();
    auto& EffectContext = Stack.StepCompiledInRef<FGameplayEffectContextHandle>();
    Stack.IncrementCode();

    auto Ability = (UFortGameplayAbility*)Context;
    callOG(Ability, Stack.GetCurrentNativeFunction(), K2_ExecuteGameplayCue, GameplayCueTag, EffectContext);

    auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
    memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());

    auto AbilitySystemComponent = (UAbilitySystemComponent*)Ability->GetAbilitySystemComponentFromActorInfo();

    // AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(GameplayCueTag, *PredictionKey, EffectContext);
    if (AbilitySystemComponent)
        AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(GameplayCueTag, *PredictionKey, EffectContext);

    free(PredictionKey);
}

void UFortGameplayAbility::K2_ExecuteGameplayCueWithParams_(UObject* Context, FFrame& Stack)
{
    auto& GameplayCueTag = Stack.StepCompiledInRef<FGameplayTag>();
    auto& GameplayCueParameter = Stack.StepCompiledInRef<FGameplayCueParameters>();
    Stack.IncrementCode();

    auto Ability = (UFortGameplayAbility*)Context;
    callOG(Ability, Stack.GetCurrentNativeFunction(), K2_ExecuteGameplayCueWithParams, GameplayCueTag, GameplayCueParameter);

    auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
    memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());

    auto AbilitySystemComponent = (UAbilitySystemComponent*)Ability->GetAbilitySystemComponentFromActorInfo();

    // AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(GameplayCueTag, *PredictionKey, EffectContext);
    if (AbilitySystemComponent)
        AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted_WithParams(GameplayCueTag, *PredictionKey, GameplayCueParameter);

    free(PredictionKey);
}

void UAbilitySystemComponent::Hook()
{
    SDK::DbgLog("  [ASC] 1 pre-FindConstructAbilitySpec\n");
    ConstructAbilitySpec = FindConstructAbilitySpec();
    SDK::DbgLog("  [ASC] 2 CAS=%p pre-FindGiveAbility\n", (void*)ConstructAbilitySpec);
    GiveAbility_ = FindGiveAbility();
    SDK::DbgLog("  [ASC] 3 GA=%p pre-FindITAA\n", (void*)GiveAbility_);
    ClearAbilityAddress = FindClearAbility();
    SDK::DbgLog("  [ASC] 3a CA=%p\n", (void*)ClearAbilityAddress);
    InternalTryActivateAbility_ = FindInternalTryActivateAbility();
    SDK::DbgLog("  [ASC] 4 ITAA=%p\n", (void*)InternalTryActivateAbility_);

    uint32 istaIdx = 0;

    if (VersionInfo.EngineVersion > 4.20)
    {
        auto OnRep_ReplicatedAnimMontage = GetDefaultObj()->GetFunction("OnRep_ReplicatedAnimMontage");
        SDK::DbgLog("  [ASC] 5 OnRep=%p pre-GetVTableIndex\n", (void*)OnRep_ReplicatedAnimMontage);
        istaIdx = OnRep_ReplicatedAnimMontage->GetVTableIndex() - 1;
        SDK::DbgLog("  [ASC] 6 istaIdx=0x%X\n", istaIdx);
    }
    else
    {
        auto ServerTryActivateAbilityWithEventData = GetDefaultObj()->GetFunction("ServerTryActivateAbilityWithEventData");
        auto ServerTryActivateAbilityWithEventDataNativeAddr = __int64(GetDefaultObj()->Vft[ServerTryActivateAbilityWithEventData->GetVTableIndex()]);

        for (int i = 0; i < 400; i++)
        {
            if ((*(uint8_t*)(ServerTryActivateAbilityWithEventDataNativeAddr + i) == 0xFF && *(uint8_t*)(ServerTryActivateAbilityWithEventDataNativeAddr + i + 1) == 0x90)
                || (*(uint8_t*)(ServerTryActivateAbilityWithEventDataNativeAddr + i) == 0xFF && *(uint8_t*)(ServerTryActivateAbilityWithEventDataNativeAddr + i + 1) == 0x93))
            {
                istaIdx = *(uint32*)(ServerTryActivateAbilityWithEventDataNativeAddr + i + 2) / 8;
                break;
            }
        }
    }

    SDK::DbgLog("  [ASC] 7 pre-HookEvery(istaIdx=0x%X)\n", istaIdx);
    // A failed GetVTableIndex yields a garbage index; patching a
    // wild vtable slot would corrupt the process. Only install when the index looks sane.
    if (istaIdx != 0 && istaIdx < 0x1000)
        Utils::HookEvery<UAbilitySystemComponent>(istaIdx, InternalServerTryActivateAbility);
    else
        SDK::DbgLog("  [ASC] 7! SKIP HookEvery — istaIdx invalid (0x%X)\n", istaIdx);
    SDK::DbgLog("  [ASC] 8 post-HookEvery\n");

    // 10.40 and 15.30's native K2 gameplay-cue implementations perform the
    // authoritative cue dispatch. Hooking them here calls the native function
    // and then sends a second multicast from the wrappers above. That creates
    // duplicate one-shot cue components, while duplicated added cues do not
    // receive a corresponding second removal. Both are unsafe around pawn
    // replacement. In 10.40 this is especially harmful to Thanos' infinite
    // jump/skydive cues and can starve the later purple beam cue. Leave both
    // builds entirely on their native, balanced cue lifecycle.
    if (VersionInfo.FortniteVersion >= 8 &&
        VersionInfo.FortniteVersion != 10.40 &&
        VersionInfo.FortniteVersion != 15.30)
    {
        Utils::ExecHook(UFortGameplayAbility::GetDefaultObj()->GetFunction("K2_ExecuteGameplayCue"), UFortGameplayAbility::K2_ExecuteGameplayCue_,
            UFortGameplayAbility::K2_ExecuteGameplayCue_OG);
        Utils::ExecHook(UFortGameplayAbility::GetDefaultObj()->GetFunction("K2_ExecuteGameplayCueWithParams"), UFortGameplayAbility::K2_ExecuteGameplayCueWithParams_,
            UFortGameplayAbility::K2_ExecuteGameplayCueWithParams_OG);
        Utils::ExecHook(
            UFortGameplayAbility::GetDefaultObj()->GetFunction("K2_AddGameplayCue"), UFortGameplayAbility::K2_AddGameplayCue_, UFortGameplayAbility::K2_AddGameplayCue_OG);
        Utils::ExecHook(UFortGameplayAbility::GetDefaultObj()->GetFunction("K2_AddGameplayCueWithParams"), UFortGameplayAbility::K2_AddGameplayCueWithParams_,
            UFortGameplayAbility::K2_AddGameplayCueWithParams_OG);
    }
    else if (VersionInfo.FortniteVersion == 10.40 ||
             VersionInfo.FortniteVersion == 15.30)
    {
        SDK::DbgLog(
            "  [ASC] %.2f using native balanced gameplay-cue dispatch\n",
            VersionInfo.FortniteVersion);
    }
    SDK::DbgLog("  [ASC] 9 Hook() complete\n");
}
