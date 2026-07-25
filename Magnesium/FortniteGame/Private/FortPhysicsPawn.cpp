#include "pch.h"
#include "../Public/FortPhysicsPawn.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortWeapon.h"
#include "../Public/BattleRoyaleGamePhaseLogic.h"
#include "../Public/FortVehicleMods.h"

struct FReplicatedPhysicsPawnState
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FReplicatedPhysicsPawnState);

    DEFINE_STRUCT_PROP(Rotation, FQuat);
    DEFINE_STRUCT_PROP(Translation, FVector);
    DEFINE_STRUCT_PROP(LinearVelocity, FVector);
    DEFINE_STRUCT_PROP(AngularVelocity, FVector);
};

struct FReplicatedAthenaVehiclePhysicsState
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FReplicatedAthenaVehiclePhysicsState);

    DEFINE_STRUCT_PROP(Rotation, FQuat);
    DEFINE_STRUCT_PROP(Translation, FVector);
    DEFINE_STRUCT_PROP(LinearVelocity, FVector);
    DEFINE_STRUCT_PROP(AngularVelocity, FVector);
};

void AFortPhysicsPawn::ServerMove(UObject* Context, FFrame& Stack)
{
    FQuat Rotation;
    FVector Translation;
    FVector LinearVelocity;
    FVector AngularVelocity;

    static auto StateStruct = FReplicatedPhysicsPawnState::StaticStruct();
    if (StateStruct)
    {
        auto State = (FReplicatedPhysicsPawnState*)malloc(FReplicatedPhysicsPawnState::Size());
        Stack.StepCompiledIn(State);
        Stack.IncrementCode();

        Rotation = State->Rotation;
        Translation = State->Translation;
        LinearVelocity = State->LinearVelocity;
        AngularVelocity = State->AngularVelocity;

        free(State);
    }
    else
    {
        auto State = (FReplicatedAthenaVehiclePhysicsState*)malloc(FReplicatedAthenaVehiclePhysicsState::Size());
        Stack.StepCompiledIn(State);
        Stack.IncrementCode();

        Rotation = State->Rotation;
        Translation = State->Translation;
        LinearVelocity = State->LinearVelocity;
        AngularVelocity = State->AngularVelocity;
        
        free(State);
    }
    auto Pawn = (AFortPhysicsPawn*)Context;


    if (VersionInfo.EngineVersion < 4.26)
    {
        Rotation.X -= 2.5f;
        Rotation.Y /= 0.3f;
        Rotation.Z -= -2.0f;
        Rotation.W /= -1.2f;
    }
    else
    {
        Rotation.X -= 0.3f;
        Rotation.Y /= -0.75f;
        Rotation.Z += 0.15f;
        Rotation.W /= 1.1f;
    }

    UPrimitiveComponent* RootComponent = Pawn->RootComponent->Cast<UPrimitiveComponent>();

    if (RootComponent)
    {
        RootComponent->K2_SetWorldTransform(FTransform(Translation, Rotation), false, nullptr, true);
        RootComponent->SetPhysicsLinearVelocity(LinearVelocity, 0, FName(0));
        RootComponent->SetPhysicsAngularVelocityInRadians(AngularVelocity, 0, FName(0));
    }
}

void AFortPlayerPawnAthena::ServerNotifyPawnHit(UObject* Context, FFrame& Stack) // 28.00+ func for registering hits, this will be a lot of work
{
    /*FHitResult Hit;
    FVector ProjectileOriginPosition;
    float ProjectileStartTimestamp;
    TArray<uint8> ArrayContext;
    FVector LocalSpaceImpactPoint;
    FVector LocalSpaceImpactNormal;
    bool bWasTargetingWhenProjectileFired;

    Stack.StepCompiledIn(&Hit);
    Stack.StepCompiledIn(&ProjectileOriginPosition);
    Stack.StepCompiledIn(&ProjectileStartTimestamp);
    Stack.StepCompiledIn(&ArrayContext);
    Stack.StepCompiledIn(&LocalSpaceImpactPoint);
    Stack.StepCompiledIn(&LocalSpaceImpactNormal);
    Stack.StepCompiledIn(&bWasTargetingWhenProjectileFired);
    Stack.IncrementCode();

    UFortWeaponItemDefinition* Weapon = (UFortWeaponItemDefinition*)Context;

    if (!Weapon)
        return;

    AFortPlayerControllerAthena* PlayerController = (AFortPlayerControllerAthena*)Weapon->PlayerController;

    auto Component = Hit.Component.Get();

    if (!Component)
        return;

    auto Actor = Component->GetOwner();

    if (!Actor)
        return;

    auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());

    if (!GamePhaseLogic)
        return;

    if (Weapon->WeaponStatHandle.DataTable)
    {
        auto StatHandle = Weapon->WeaponStatHandle;

        if (!StatHandle.DataTable)
            return;

        auto Equals = [](const FName& LeftKey, const FName& RightKey) -> bool
            {
                return LeftKey == RightKey;
            };

        auto Index = StatHandle.DataTable->RowMap.Find(StatHandle.RowName, Equals).GetIndex();
        auto WeaponStats = (FFortRangedWeaponStats*)StatHandle.DataTable->RowMap[Index].Second;

        if (WeaponStats)
        {
            if (Actor->bCanBeDamaged == 1 && PlayerController->MyFortPawn)
            {
                if (Actor->IsA(AFortPlayerPawn::StaticClass()))
                {
                    float Multiplier = 1;
                    if (Hit.BoneName.ToString() == "Head")
                        Multiplier = WeaponStats->DamageZone_Critical;

                    float Damage = WeaponStats->DmgPB * Multiplier;

                    FAthenaBatchedDamageGameplayCues_Shared SharedCue{};
                    SharedCue.Location = (FVector_NetQuantize10)Hit.ImpactPoint;
                    SharedCue.Normal = Hit.ImpactNormal;
                    SharedCue.bIsCritical = Hit.BoneName.ToString() == "Head";
                    SharedCue.Magnitude = Damage;
                    SharedCue.bWeaponActivate = true;
                    SharedCue.bIsFatal = false;
                    SharedCue.bIsShield = false;
                    SharedCue.bIsShieldDestroyed = false;
                    SharedCue.bIsShieldApplied = false;
                    SharedCue.bIsBallistic = false;
                    SharedCue.bIsBeam = false;
                    SharedCue.bIsValid = true;

                    FAthenaBatchedDamageGameplayCues_NonShared NonSharedCue{};
                    NonSharedCue.HitActor = Actor;
                    NonSharedCue.NonPlayerHitActor = Actor;

                    AFortPlayerPawnAthena* Pawn = (AFortPlayerPawnAthena*)Actor;
                    if (Pawn->Controller && ((AFortGameStateAthena*)UWorld::GetWorld()->GameState)->CurrentPlaylistInfo.BasePlaylist->MaxSquadSize > 1)
                    {
                        AFortPlayerStateAthena* EnemyPlayerState = (AFortPlayerStateAthena*)Pawn->Controller->PlayerState;
                        AFortPlayerStateAthena* PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;

                        if (EnemyPlayerState && PlayerState)
                        {
                            if (EnemyPlayerState->TeamIndex == PlayerState->TeamIndex)
                                return;
                        }
                    }

                    if (GamePhaseLogic->GamePhase != EAthenaGamePhase::Warmup && Pawn) 
                    {
                        auto PlayerShield = Pawn->GetShield();
                        auto PlayerHealth = Pawn->GetHealth();
                        float RemainingDamage = SharedCue.Magnitude;

                        if (PlayerShield > 0.f)
                        {
                            SharedCue.bIsShield = true;

                            if (PlayerShield <= RemainingDamage)
                            {
                                SharedCue.bIsShieldDestroyed = true;

                                RemainingDamage -= PlayerShield;
                                Pawn->SetShield(0.f);
                            }
                            else
                            {
                                Pawn->SetShield(PlayerShield - RemainingDamage);
                                RemainingDamage = 0.f;
                            }
                        }

                        if (RemainingDamage > 0.f)
                        {
                            if (PlayerHealth <= RemainingDamage)
                            {
                                Pawn->ForceKill(FGameplayTag(), PlayerController, Weapon);
                                return;
                            }
                            else
                            {
                                Pawn->SetHealth(PlayerHealth - RemainingDamage);
                            }
                        }

                        Pawn->ForceNetUpdate();
                    }

                    PlayerController->MyFortPawn->NetMulticast_Athena_BatchedDamageCues(SharedCue, NonSharedCue);
                }
                else
                {
                    FAthenaBatchedDamageGameplayCues_Shared SharedCue{};

                    SharedCue.Location = (FVector_NetQuantize10)Hit.ImpactPoint;
                    SharedCue.Normal = Hit.ImpactNormal;
                    SharedCue.Magnitude = WeaponStats->EnvDmgPB;
                    SharedCue.bWeaponActivate = true;
                    SharedCue.bIsFatal = false;
                    SharedCue.bIsCritical = false;
                    SharedCue.bIsBallistic = true;
                    SharedCue.bIsBeam = false;
                    SharedCue.bIsValid = true;

                    FAthenaBatchedDamageGameplayCues_NonShared NonSharedCue{};
                    NonSharedCue.HitActor = Actor;
                    NonSharedCue.NonPlayerHitActor = Actor;

                    PlayerController->MyFortPawn->NetMulticast_Athena_BatchedDamageCues(SharedCue, NonSharedCue);
                    if (Actor->IsA(ABuildingSMActor::StaticClass()))
                    {
                        ABuildingSMActor* BuildingActor = (ABuildingSMActor*)Actor;
                        if (BuildingActor)
                        {
                            float RemainingHealth = BuildingActor->GetHealth() - SharedCue.Magnitude;
                            BuildingActor->SetHealth(RemainingHealth);
                            BuildingActor->ForceNetUpdate();

                            if (BuildingActor->GetHealth() <= 0)
                                BuildingActor->K2_DestroyActor();
                        }
                    }
                    else if (Actor->IsA(AAthenaSuperDingo::StaticClass()))
                    {
                        AAthenaSuperDingo* SuperDingo = (AAthenaSuperDingo*)Actor;
                        if (SuperDingo)
                        {
                            float RemainingHealth = SuperDingo->GetHealth() - SharedCue.Magnitude;
                            SuperDingo->SetHealth(RemainingHealth);
                            SuperDingo->ForceNetUpdate();

                            if (SuperDingo->GetHealth() <= 0)
                                SuperDingo->K2_DestroyActor();
                        }
                    }
                    else if (Actor->IsA(AFortAthenaVehicle::StaticClass()))
                    {
                        AFortAthenaVehicle* Vehicle = (AFortAthenaVehicle*)Actor;
                        if (Vehicle)
                        {
                            Vehicle->HealthSet->Health.CurrentValue -= WeaponStats->EnvDmgPB;
                            Vehicle->OnRep_HealthSet();

                            if (Vehicle->HealthSet->Health.CurrentValue <= 0)
                                Vehicle->DestroyVehicle();
                        }
                    }
                    else if (Actor->IsA(ABuildingGameplayActorCrashpad::StaticClass()))
                    {
                        ABuildingGameplayActorCrashpad* CrashPad = (ABuildingGameplayActorCrashpad*)Actor;
                        if (CrashPad)
                        {
                            float RemainingHealth = CrashPad->GetHealth() - SharedCue.Magnitude;
                            CrashPad->SetHealth(RemainingHealth);
                            CrashPad->ForceNetUpdate();

                            if (CrashPad->GetHealth() <= 0)
                                CrashPad->K2_DestroyActor();
                        }
                    }
                    else if (Actor->IsA(ABuildingGameplayActorBalloon::StaticClass()))
                    {
                        ABuildingGameplayActorBalloon* Balloon = (ABuildingGameplayActorBalloon*)Actor;
                        if (Balloon)
                        {
                            float RemainingHealth = Balloon->GetHealth() - SharedCue.Magnitude;
                            Balloon->SetHealth(RemainingHealth);
                            Balloon->ForceNetUpdate();

                            if (Balloon->GetHealth() <= 0)
                                Balloon->K2_DestroyActor();
                        }
                    }
                }
            }
        }
    }

    return;*/
}

void AFortOctopusVehicle::ServerUpdateTowhook(UObject* Context, FFrame& Stack)
{
    FVector InNetTowhookAimDir;

    Stack.StepCompiledIn(&InNetTowhookAimDir);
    Stack.IncrementCode();
    auto Vehicle = (AFortOctopusVehicle*)Context;

    printf("ServerUpdateTowhook\n");
    Vehicle->NetTowhookAimDir = InNetTowhookAimDir;
    Vehicle->OnRep_NetTowhookAimDir();
}

void AFortSpaghettiVehicle::ServerUpdateTowhook(UObject* Context, FFrame& Stack)
{
    FVector InNetTowhookAimDir;

    Stack.StepCompiledIn(&InNetTowhookAimDir);
    Stack.IncrementCode();
    auto Vehicle = (AFortSpaghettiVehicle*)Context;

    Vehicle->NetTowhookAimDir = InNetTowhookAimDir;
    Vehicle->OnRep_NetTowhookAimDir();
}

struct FHitResult
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FHitResult);

    DEFINE_STRUCT_PROP(Location, FVector);
    DEFINE_STRUCT_PROP(ImpactPoint, FVector);
    DEFINE_STRUCT_PROP(ImpactNormal, FVector);
    DEFINE_STRUCT_PROP(Normal, FVector);
    DEFINE_STRUCT_PROP(Component, TWeakObjectPtr<UActorComponent>);
};

struct FAttachedInfo
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FAttachedInfo);

    DEFINE_STRUCT_PROP(Hit, FHitResult);
};

class AFortOctopusTowhookAttachableProjectile : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortOctopusTowhookAttachableProjectile);

    DEFINE_PROP(AttachedInfo, FAttachedInfo);
    DEFINE_PROP(OwningVehicle, AFortOctopusVehicle*);

    DEFINE_FUNC(Kill, void);
};

uint64_t CanGrappleToComponent_ = 0;
void (*OnRep_ReplicatedAttachedInfoOG)(AFortOctopusTowhookAttachableProjectile* _this);
void OnRep_ReplicatedAttachedInfo(AFortOctopusTowhookAttachableProjectile* _this)
{
    OnRep_ReplicatedAttachedInfoOG(_this);

    auto OwningVehicle = _this->OwningVehicle;
    if (!OwningVehicle)
        return;

    auto Comp = _this->AttachedInfo.Hit.Component.Get();
    /*if (!Comp)
    {
        _this->Kill();
        return;
    }*/

    auto CanGrappleToComponent = (bool(*)(AFortOctopusVehicle*, UActorComponent*))CanGrappleToComponent_;

    if (!CanGrappleToComponent(OwningVehicle, Comp))
    {
        // game automatically kills for us in OG
        return;
    }

    // the old projectile is supposed to die too, i dont know why it doesn't. i'll have to look at this more
    OwningVehicle->ReplicatedAttachState.Component = Comp;
    OwningVehicle->ReplicatedAttachState.LocalLocation = UKismetMathLibrary::InverseTransformLocation(Comp->K2_GetComponentToWorld(), _this->AttachedInfo.Hit.Location);
    OwningVehicle->ReplicatedAttachState.LocalNormal = UKismetMathLibrary::InverseTransformDirection(Comp->K2_GetComponentToWorld(), _this->AttachedInfo.Hit.Normal);
    OwningVehicle->OnRep_ReplicatedAttachState();

    printf("[Ballers] Comp: %s, Owner %s\n", Comp->Name.ToString().c_str(), Comp->GetOwner()->Name.ToString().c_str());
    printf("[Ballers] Location: %f %f %f [World] -> ", _this->AttachedInfo.Hit.Location.X, _this->AttachedInfo.Hit.Location.Y, _this->AttachedInfo.Hit.Location.Z);
    printf("%f %f %f [Local]\n", OwningVehicle->ReplicatedAttachState.LocalLocation.X, OwningVehicle->ReplicatedAttachState.LocalLocation.Y, OwningVehicle->ReplicatedAttachState.LocalLocation.Z);
    printf("[Ballers] Normal: %f %f %f [World] -> ", _this->AttachedInfo.Hit.Normal.X, _this->AttachedInfo.Hit.Normal.Y, _this->AttachedInfo.Hit.Normal.Z);
    printf("%f %f %f [Local]\n", OwningVehicle->ReplicatedAttachState.LocalNormal.X, OwningVehicle->ReplicatedAttachState.LocalNormal.Y, OwningVehicle->ReplicatedAttachState.LocalNormal.Z);
    //printf("CALLED!!!!\n");
}

void AFortWeaponRangedMountedCannon::ServerFireActorInCannon(UObject* Context, FFrame& Stack)
{
    FVector LaunchDir;

    Stack.StepCompiledIn(&LaunchDir);
    Stack.IncrementCode();

    auto Vehicle = (AFortWeaponRangedMountedCannon*)Context;

    if (!Vehicle)
        return;

    auto PushCannon = Vehicle->Cast<AFortAthenaSKPushCannon>();

    if (!PushCannon)
        return;

    if (auto TargetPawn = PushCannon->GetPawnAtSeat(1))
    {
        PushCannon->OnPreLaunchPawn(TargetPawn, LaunchDir);

        TargetPawn->ServerOnExitVehicle();

        PushCannon->OnLaunchPawn(TargetPawn, LaunchDir);
        PushCannon->MultiCastPushCannonLaunchedPlayer();
    }
}

void AFortPhysicsPawn::Hook()
{
    FortVehicleMods::InstallHooks();

    auto DefaultPhysPawn = GetDefaultObj();
    if (DefaultPhysPawn)
    {
        auto ServerMoveFn = DefaultPhysPawn->GetFunction("ServerMove");

        if (ServerMoveFn)
        {
            Utils::ExecHook(ServerMoveFn, ServerMove);
            Utils::ExecHook(DefaultPhysPawn->GetFunction("ServerMoveReliable"), ServerMove);
        }
        else
        {
            auto ServerUpdatePhysicsParamsFn = DefaultPhysPawn->GetFunction("ServerUpdatePhysicsParams");

            if (ServerUpdatePhysicsParamsFn)
                Utils::ExecHook(ServerUpdatePhysicsParamsFn, ServerMove);
        }
    }
    else
    {
        auto DefaultVehicle = DefaultObjImpl("FortAthenaVehicle");

        if (DefaultVehicle)
            Utils::ExecHook(DefaultVehicle->GetFunction("ServerUpdatePhysicsParams"), ServerMove);
    }

    auto DefaultOctopusVehicle = AFortOctopusVehicle::GetDefaultObj();

    if (DefaultOctopusVehicle)
    {
        Utils::ExecHook(DefaultOctopusVehicle->GetFunction("ServerUpdateTowhook"), AFortOctopusVehicle::ServerUpdateTowhook);
    }

    auto DefaultSpaghettiVehicle = AFortSpaghettiVehicle::GetDefaultObj();

    if (DefaultSpaghettiVehicle)
        Utils::ExecHook(DefaultSpaghettiVehicle->GetFunction("ServerUpdateTowhook"), AFortSpaghettiVehicle::ServerUpdateTowhook);

    if (AFortOctopusTowhookAttachableProjectile::StaticClass())
    {
        auto OnRep_ReplicatedAttachedInfoIdx = AFortOctopusTowhookAttachableProjectile::GetDefaultObj()->GetFunction("OnRep_ReplicatedAttachedInfo")->GetVTableIndex();

        auto OnRep_ReplicatedAttachedInfo__Impl = AFortOctopusTowhookAttachableProjectile::GetDefaultObj()->Vft[OnRep_ReplicatedAttachedInfoIdx];
        auto CanGrappleToComponent = Memcury::Scanner(OnRep_ReplicatedAttachedInfo__Impl).ScanFor({ 0xFF, 0x90 }).Get();
        
        for (int i = 0; i < 2000; i++)
        {
            auto Ptr = (uint8_t*)(CanGrappleToComponent - i);

            if (*Ptr == 0x48 && *(Ptr + 1) == 0x8B)
            {
                if (*(Ptr + 3) == 0xE8)
                {
                    CanGrappleToComponent_ = Memcury::Scanner(Ptr).RelativeOffset(4).Get();
                    break;
                }
                else if (*(Ptr + 3) == 0xFF && (*(Ptr + 4) & 0xF0) == 0x90)
                {
                    CanGrappleToComponent_ = uint64_t(AFortOctopusTowhookAttachableProjectile::GetDefaultObj()->Vft[*(uint32_t*)(Ptr + 5) / 8]);
                    break;
                }
            }
        }
        //.ScanFor({ 0x48, 0x8B, 0xFF, 0xE8 }, false, 0, 1, 2048, true).RelativeOffset(4).Get();

        Utils::Hook<AFortOctopusTowhookAttachableProjectile>(OnRep_ReplicatedAttachedInfoIdx, OnRep_ReplicatedAttachedInfo, OnRep_ReplicatedAttachedInfoOG);
    }

    auto MountedCannonVehicle = AFortWeaponRangedMountedCannon::GetDefaultObj();

    if (MountedCannonVehicle)
    {
        Utils::ExecHook(MountedCannonVehicle->GetFunction("ServerFireActorInCannon"), AFortWeaponRangedMountedCannon::ServerFireActorInCannon);
	}
}
