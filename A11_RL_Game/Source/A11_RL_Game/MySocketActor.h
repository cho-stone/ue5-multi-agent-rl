#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "Kismet/GameplayStatics.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Networking.h"
#include "Components/CapsuleComponent.h"
#include "MySocketActor.generated.h"
UCLASS()
class A11_RL_GAME_API AMySocketActor : public AActor
{
    GENERATED_BODY()
public:
    AMySocketActor();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaTime) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    void SendFullState();
    void ResetAllNPCs();
    void ReceiveAndProcessCommands();
    void ProcessCommand(const FString& Command);
    void ApplyMovementToNPCs(float DeltaTime);
    void DestroyAllBullets();
    void ResetAllNPCStates();
    UPROPERTY(EditAnywhere, Category = "Firing")
    TSubclassOf<AActor> BulletClass;
    void FireGunForNPC(ACharacter* NPC);

private:
    FSocket* Socket;  //TCP socket used for communication with Python
    bool bHasSentNPCPositions; //Tracks if NPC positions have been sent at least once
    TMap<FString, float> ActiveNPCCommands; //Map of active NPC movement commands
    float CommandTimeout;
    TSet<FString> ActiveFiringNPCs; //Set of NPCs that need to fire their weapon this tick
    FString ReceivedBuffer; //Buffer to accumulate partial/streamed incoming socket data

    //NPC state
    struct FNPCState
    {
        FString Name;
        FVector Location;
        FRotator Rotation;
        int32 HP;
        bool bIsAlive;
    };

    bool bPendingReset = false;
    float ResetDelay = 0.15f;
    float ResetElapsed = 0.0f;
};