//A11_Problem_CHO_SEOKJIN

#include "MySocketActor.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include <GameFramework/ProjectileMovementComponent.h>

AMySocketActor::AMySocketActor()
{
    PrimaryActorTick.bCanEverTick = true;
    Socket = nullptr;
    bHasSentNPCPositions = false;
    CommandTimeout = 0.5f;
}

void AMySocketActor::BeginPlay()
{
    Super::BeginPlay();

    //Set up and connect to a TCP socket server (UE -> client)
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    Socket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("default"), false);

    FIPv4Address IP;
    FIPv4Address::Parse(TEXT("127.0.0.1"), IP); //localhost
    int32 Port = 9001;
    TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
    Addr->SetIp(IP.Value);
    Addr->SetPort(Port);

    bool Connected = Socket->Connect(*Addr);

    if (Connected) {
        UE_LOG(LogTemp, Log, TEXT("Socket success"));
    }
    else {
        int32 ErrorCode = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
        FString ErrorMessage = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetSocketError((ESocketErrors)ErrorCode);
        UE_LOG(LogTemp, Warning, TEXT("Socket Fail! [%d] %s"), ErrorCode, *ErrorMessage);
    }
}

void AMySocketActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    SendFullState(); //Send current full state to Python
    ReceiveAndProcessCommands(); //Receive and apply action commands from Python

    //Timeout logic
    TArray<FString> RemoveKeys;
    for (auto& Pair : ActiveNPCCommands)
    {
        Pair.Value += DeltaTime;
        if (Pair.Value > CommandTimeout)
        {
            RemoveKeys.Add(Pair.Key);
        }
    }
    for (const FString& Key : RemoveKeys)
    {
        ActiveNPCCommands.Remove(Key);
    }

    if (bPendingReset)
    {
        ResetElapsed += DeltaTime;
        if (ResetElapsed > ResetDelay)
        {
            ResetAllNPCStates();
            bPendingReset = false;
        }
    }

    ApplyMovementToNPCs(DeltaTime); //Apply valid move/fire commands to NPCs
}

// Send the current state of all NPC's State as JSON to Python
void AMySocketActor::SendFullState()
{
    TArray<AActor*> FoundNPCs;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), ACharacter::StaticClass(), FoundNPCs);

    TArray<FString> NPCStates;
    int AliveCount = 0;

    for (AActor* Actor : FoundNPCs)
    {
        ACharacter* NPC = Cast<ACharacter>(Actor);
        if (!NPC) continue;

#if WITH_EDITOR
        FString Name = NPC->GetActorLabel();
#else
        FString Name = NPC->GetName();
#endif
        FVector Location = NPC->GetActorLocation();
        FRotator Rotation = NPC->GetActorRotation();

        int32 HP = 10;
        bool bAlive = true;
        UClass* NPCClass = NPC->GetClass();
        FProperty* HealthProp = NPCClass->FindPropertyByName(TEXT("Health"));
        FProperty* AliveProp = NPCClass->FindPropertyByName(TEXT("Alive"));
        if (HealthProp)
        {
            void* Ptr = HealthProp->ContainerPtrToValuePtr<void>(NPC);
            HP = *reinterpret_cast<int32*>(Ptr);
        }
        if (AliveProp)
        {
            void* Ptr = AliveProp->ContainerPtrToValuePtr<void>(NPC);
            bAlive = *reinterpret_cast<bool*>(Ptr);
        }
        if (bAlive) AliveCount++; //Count only alive NPCs

        //Build state as JSON string
        FString NPCStr = FString::Printf(
            TEXT("{\"name\":\"%s\",\"pos\":[%.2f,%.2f,%.2f],\"rot\":[%.2f,%.2f,%.2f],\"hp\":%d,\"alive\":%s}"),
            *Name, Location.X, Location.Y, Location.Z, Rotation.Pitch, Rotation.Yaw, Rotation.Roll, HP, bAlive ? TEXT("true") : TEXT("false"));
        NPCStates.Add(NPCStr);
    }

    //Episode done if only 1 or less NPCs are alive
    bool bDone = (AliveCount <= 1);

    //Compose final state message for Python
    FString StateJson = FString::Printf(
        TEXT("{\"npcs\":[%s],\"done\":%s}\n"),
        *FString::Join(NPCStates, TEXT(",")), bDone ? TEXT("true") : TEXT("false"));

    FTCHARToUTF8 Convert(*StateJson);
    int32 BytesSent = 0;
    if (Socket)
        Socket->Send((uint8*)Convert.Get(), Convert.Length(), BytesSent);
}

//Receive command strings from Python, process actions, and update internal command state
void AMySocketActor::ReceiveAndProcessCommands()
{
    if (!Socket) return;
    if (!Socket->GetConnectionState() == SCS_Connected) return; //Check socket connection

    uint32 PendingDataSize = 0;
    if (Socket->HasPendingData(PendingDataSize) && PendingDataSize > 0)
    {
        TArray<uint8> ReceivedData;
        ReceivedData.SetNumUninitialized(PendingDataSize);

        int32 BytesRead = 0;
        bool bRecvOk = Socket->Recv(ReceivedData.GetData(), ReceivedData.Num(), BytesRead);

        //Exit if receive failed or empty data
        if (!bRecvOk || BytesRead <= 0 || ReceivedData.Num() == 0) return;

        //Truncate to received bytes (for safety)
        ReceivedData.SetNum(BytesRead);

        //Null-terminate for string safety
        ReceivedData.Add(0);

        //Convert to FString, handle any exceptions
        FString NewData;
        try
        {
            NewData = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(ReceivedData.GetData())));
        }
        catch (...)
        {
            UE_LOG(LogTemp, Warning, TEXT("Occurred while converting socket data string!"));
            return;
        }

        ReceivedBuffer += NewData;

        //Split by newlines
        TArray<FString> Commands;
        ReceivedBuffer.ParseIntoArray(Commands, TEXT("\n"), true);

        if (Commands.Num() > 0)
        {
            if (ReceivedBuffer.EndsWith(TEXT("\n")))
            {
                ReceivedBuffer.Empty();
            }
            else
            {
                ReceivedBuffer = Commands.Last();
                Commands.RemoveAt(Commands.Num() - 1);
            }
        }

        //Process all complete commands
        for (const FString& Command : Commands)
        {
            ProcessCommand(Command);
        }
    }
}

//Parse and handle a single command (format: NPC:Action, e.g. BP_AI_NPC0:FORWARD)
void AMySocketActor::ProcessCommand(const FString& Command)
{
    FString CleanCommand = Command.Replace(TEXT("\r"), TEXT("")).Replace(TEXT("\n"), TEXT(""));
    if (CleanCommand.IsEmpty()) return;

    //Check for RESET command
    if (CleanCommand.ToUpper() == TEXT("RESET")) {
        UE_LOG(LogTemp, Warning, TEXT("receive RESET command!"));
        ResetAllNPCs();
        return;
    }

    //Split command by comma to support multiple commands in one line
    TArray<FString> CommandList;
    CleanCommand.ParseIntoArray(CommandList, TEXT(","), true);

    for (const FString& Cmd : CommandList)
    {
        FString NPCName, Action;
        if (Cmd.Split(TEXT(":"), &NPCName, &Action))
        {
            NPCName = NPCName.TrimStartAndEnd();
            Action = Action.TrimStartAndEnd().ToUpper(); // FORWARD, RIGHT, STOP etc

            //Movement commands: add to active commands
            if (Action == TEXT("FORWARD") || Action == TEXT("BACKWARD") ||
                Action == TEXT("LEFT") || Action == TEXT("RIGHT"))
            {
                FString Key = NPCName + ":" + Action;
                ActiveNPCCommands.Add(Key, 0.0f);
            }
            //Firing command: mark for firing
            else if (Action == TEXT("FIRE"))
            {
                ActiveFiringNPCs.Add(NPCName);
            }
            //STOP command: clear all active movement commands
            else if (Action == TEXT("STOP"))
            {
                TArray<FString> KeysToRemove;
                for (auto& Pair : ActiveNPCCommands)
                {
                    if (Pair.Key.StartsWith(NPCName + ":"))
                        KeysToRemove.Add(Pair.Key);
                }
                for (const FString& Key : KeysToRemove)
                    ActiveNPCCommands.Remove(Key);
            }
        }
    }
}

//Apply movement and firing commands to each NPC for this tick
void AMySocketActor::ApplyMovementToNPCs(float DeltaTime)
{
    UWorld* World = GetWorld();
    if (!World) return;

    for (TActorIterator<ACharacter> It(World); It; ++It)
    {
        ACharacter* NPC = *It;
        if (!NPC) continue;

#if WITH_EDITOR
        FString Label = NPC->GetActorLabel();
#else
        FString Label = NPC->GetName();
#endif

        UCharacterMovementComponent* MovementComp = NPC->GetCharacterMovement();
        if (!MovementComp) continue;

        //Build movement vector based on active commands
        FVector FinalMove = FVector::ZeroVector;

        TMap<FString, FVector> DirectionMap;
        DirectionMap.Add("FORWARD", NPC->GetActorForwardVector());
        DirectionMap.Add("BACKWARD", -NPC->GetActorForwardVector());
        DirectionMap.Add("RIGHT", NPC->GetActorRightVector());
        DirectionMap.Add("LEFT", -NPC->GetActorRightVector());

        for (const auto& Dir : DirectionMap)
        {
            FString Key = Label + ":" + Dir.Key;
            if (ActiveNPCCommands.Contains(Key))
            {
                FinalMove += Dir.Value;
            }
            //Fire gun if marked for firing
            if (ActiveFiringNPCs.Contains(Label)) {
                FireGunForNPC(NPC);
                ActiveFiringNPCs.Remove(Label);
            }
        }

        if (!FinalMove.IsNearlyZero())
        {
            MovementComp->MaxWalkSpeed = 500.f;
            FinalMove.Z = 0.0f;
            FinalMove.Normalize();
            NPC->AddMovementInput(FinalMove, 1.0f);
        }
    }
}

//Spawns and fires a bullet from the specified NPC's location/rotation
void AMySocketActor::FireGunForNPC(ACharacter* NPC)
{
    if (!NPC || !BulletClass) return;

    USkeletalMeshComponent* MeshComp = NPC->GetMesh();
    if (!MeshComp) return;

    FVector SpawnLocation;
    FRotator SpawnRotation;

    if (MeshComp->DoesSocketExist(TEXT("Muzzle")))
    {
        //Use "Muzzle" socket if available, but always use NPC's forward direction
        SpawnLocation = MeshComp->GetSocketLocation(TEXT("Muzzle"));
        SpawnRotation = NPC->GetActorRotation();

        //Move further away from the muzzle for preventing conflicts
        SpawnLocation += NPC->GetActorForwardVector() * 25.0f;
    }
    else
    {
        SpawnLocation = NPC->GetActorLocation() + (NPC->GetActorForwardVector() * 100.0f) + FVector(0, 0, 50);
        SpawnRotation = NPC->GetActorRotation();
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = NPC;
    SpawnParams.Instigator = NPC;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    //Actually spawn the bullet actor and set its velocity
    AActor* SpawnedBullet = GetWorld()->SpawnActor<AActor>(BulletClass, SpawnLocation, SpawnRotation, SpawnParams);

    if (SpawnedBullet)
    {
        UProjectileMovementComponent* ProjectileComp = SpawnedBullet->FindComponentByClass<UProjectileMovementComponent>();
        if (ProjectileComp)
        {
            ProjectileComp->Velocity = NPC->GetActorForwardVector() * 1000.0f;
        }
    }
}

/*
Resets all NPC's state and clears all active commands
Also destroys all bullets in the level to start fresh
*/
void AMySocketActor::ResetAllNPCs()
{
    DestroyAllBullets(); //Remove all bullets in world
    bPendingReset = true; //Wait before NPC reset
    ResetElapsed = 0.0f;

    //Clear all movement/fire command state
    ActiveNPCCommands.Empty();
    ActiveFiringNPCs.Empty();

    UE_LOG(LogTemp, Warning, TEXT("ResetAllNPCs: Waiting for bullet destruction..."));
}

//Destroy all bullet actors in the world to prevent leftover damage after reset
void AMySocketActor::DestroyAllBullets()
{
    UWorld* World = GetWorld();
    if (!World) return;

    static UClass* BulletBPClass = StaticLoadClass(AActor::StaticClass(), nullptr, TEXT("/Game/TopDown/Blueprints/BP_Bullet"));
    if (BulletBPClass)
    {
        for (TActorIterator<AActor> It(World, BulletBPClass); It; ++It)
        {
            AActor* Bullet = *It;
            if (IsValid(Bullet))
            {
                Bullet->Destroy();
            }
        }
    }
}

void AMySocketActor::ResetAllNPCStates()
{
    UWorld* World = GetWorld();
    if (!World) return;

    TMap<FString, FVector> ResetPositions;
    TMap<FString, FRotator> ResetRotations;

    //Customize the reset locations and rotations per NPC
    ResetPositions.Add(TEXT("BP_AI_NPC0"), FVector(-850.f, -850.f, 88.f));
    ResetPositions.Add(TEXT("BP_AI_NPC1"), FVector(850.f, 850.f, 88.f));
    ResetRotations.Add(TEXT("BP_AI_NPC0"), FRotator(0.f, 0.f, 0.f));
    ResetRotations.Add(TEXT("BP_AI_NPC1"), FRotator(0.f, 230.f, 0.f));

    for (TActorIterator<ACharacter> It(World); It; ++It)
    {
        ACharacter* NPC = *It;
        if (!NPC) continue;

#if WITH_EDITOR
        FString Label = NPC->GetActorLabel();
#else
        FString Label = NPC->GetName();
#endif

        //Reset location and rotation if available in map
        if (ResetPositions.Contains(Label))
            NPC->SetActorLocation(ResetPositions[Label]);
        if (ResetRotations.Contains(Label))
            NPC->SetActorRotation(ResetRotations[Label]);

        //Reset health and alive state using Blueprint variables
        UClass* NPCClass = NPC->GetClass();
        FProperty* HealthProp = NPCClass->FindPropertyByName(TEXT("Health"));
        if (HealthProp)
        {
            void* Ptr = HealthProp->ContainerPtrToValuePtr<void>(NPC);
            *reinterpret_cast<int32*>(Ptr) = 10;
        }
        FProperty* AliveProp = NPCClass->FindPropertyByName(TEXT("Alive"));
        if (AliveProp)
        {
            void* Ptr = AliveProp->ContainerPtrToValuePtr<void>(NPC);
            *reinterpret_cast<bool*>(Ptr) = true;
        }
    }

    //one more HP reset here(For defense)
    for (TActorIterator<ACharacter> It(World); It; ++It)
    {
        ACharacter* NPC = *It;
        if (!NPC) continue;

        UClass* NPCClass = NPC->GetClass();
        FProperty* HealthProp = NPCClass->FindPropertyByName(TEXT("Health"));
        if (HealthProp)
        {
            void* Ptr = HealthProp->ContainerPtrToValuePtr<void>(NPC);
            *reinterpret_cast<int32*>(Ptr) = 10;
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("ResetAllNPCStates: All NPC state reset done!"));
}

//Called when actor is destroyed/game ends
void AMySocketActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (Socket)
    {
        Socket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
        Socket = nullptr;
    }
    Super::EndPlay(EndPlayReason);
}