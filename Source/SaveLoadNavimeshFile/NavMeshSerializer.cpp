#include "NavMeshSerializer.h"
#include "ObservableRecastNavMesh.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "AI/NavDataGenerator.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Compression.h"
#include "HAL/PlatformFileManager.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/LevelStreaming.h"
#include "Engine/Level.h"

ANavMeshSerializer::ANavMeshSerializer()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
}

//======================================================================//
// ユーティリティ
//======================================================================//

FName ANavMeshSerializer::GetAgentNameFromNavMesh(const ARecastNavMesh* NavMesh)
{
    if (!NavMesh)
    {
        return NAME_None;
    }
    const FNavDataConfig& Config = NavMesh->GetConfig();
    return Config.Name;
}

TArray<ARecastNavMesh*> ANavMeshSerializer::GetAllRecastNavMeshes() const
{
    TArray<ARecastNavMesh*> Result;

    UWorld* World = GetWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: World is null"));
        return Result;
    }

    for (TActorIterator<ARecastNavMesh> It(World); It; ++It)
    {
        ARecastNavMesh* NavMesh = *It;
        if (NavMesh && !NavMesh->IsPendingKillPending())
        {
            Result.Add(NavMesh);
        }
    }

    if (Result.Num() == 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("NavMeshSerializer: No RecastNavMesh found in world"));
    }

    return Result;
}

ARecastNavMesh* ANavMeshSerializer::FindNavMeshByAgentName(FName AgentName) const
{
    UWorld* World = GetWorld();
    if (!World) return nullptr;

    for (TActorIterator<ARecastNavMesh> It(World); It; ++It)
    {
        ARecastNavMesh* NavMesh = *It;
        if (NavMesh && !NavMesh->IsPendingKillPending())
        {
            if (GetAgentNameFromNavMesh(NavMesh) == AgentName)
            {
                return NavMesh;
            }
        }
    }

    UE_LOG(LogTemp, Warning,
        TEXT("NavMeshSerializer: NavMesh for agent [%s] not found"),
        *AgentName.ToString());
    return nullptr;
}

FString ANavMeshSerializer::GetNavDataFilePath(const FString& StageID, const FString& AgentName) const
{
    return FPaths::ProjectSavedDir() / TEXT("Stages") / (StageID + TEXT("_") + AgentName + TEXT(".navdata"));
}

bool ANavMeshSerializer::HasSavedNavMesh(const FString& StageID) const
{
    const FString SearchDir = FPaths::ProjectSavedDir() / TEXT("Stages");
    const FString Pattern = StageID + TEXT("_*.navdata");

    TArray<FString> FoundFiles;
    IFileManager::Get().FindFiles(FoundFiles, *(SearchDir / Pattern), true, false);

    return FoundFiles.Num() > 0;
}

void ANavMeshSerializer::DisableNavMeshAutoRebuild() const
{
    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (NavSys)
    {
        NavSys->SetNavigationOctreeLock(true);
    }
}

void ANavMeshSerializer::EnableNavMeshDynamicRebuild(FName AgentName)
{
    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!NavSys)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: NavigationSystem not found"));
        return;
    }

    ARecastNavMesh* NavMesh = FindNavMeshByAgentName(AgentName);
    if (!NavMesh)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Cannot enable rebuild — agent [%s] not found"),
            *AgentName.ToString());
        return;
    }

    NavSys->SetNavigationOctreeLock(false);

    NavMesh->ConditionalConstructGenerator();

    // 同時タイル生成ジョブ数を設定（0ならデフォルトのまま）
    if (MaxTileJobsCount > 0)
    {
        NavMesh->SetMaxSimultaneousTileGenerationJobsCount(MaxTileJobsCount);
        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: Set MaxSimultaneousTileGenerationJobsCount=%d for agent [%s]"),
            MaxTileJobsCount, *AgentName.ToString());
    }

    NavMesh->RebuildAll();

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Dynamic rebuild triggered for agent [%s] (async tile-based)"),
        *AgentName.ToString());
}

void ANavMeshSerializer::EnableNavMeshDynamicRebuildAll()
{
    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!NavSys)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: NavigationSystem not found"));
        return;
    }

    NavSys->SetNavigationOctreeLock(false);

    TArray<ARecastNavMesh*> AllNavMeshes = GetAllRecastNavMeshes();
    for (ARecastNavMesh* NavMesh : AllNavMeshes)
    {
        NavMesh->ConditionalConstructGenerator();

        if (MaxTileJobsCount > 0)
        {
            NavMesh->SetMaxSimultaneousTileGenerationJobsCount(MaxTileJobsCount);
        }

        NavMesh->RebuildAll();
    }

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Dynamic rebuild triggered for all %d agent(s) (async tile-based)"),
        AllNavMeshes.Num());
}

void ANavMeshSerializer::RebuildNavMeshInBoundsForAgent(FVector BoundsMin, FVector BoundsMax, FName AgentName)
{
    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!NavSys)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: NavigationSystem not found"));
        return;
    }

    ARecastNavMesh* NavMesh = FindNavMeshByAgentName(AgentName);
    if (!NavMesh)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Cannot rebuild in bounds — agent [%s] not found"),
            *AgentName.ToString());
        return;
    }

    NavSys->SetNavigationOctreeLock(false);
    NavMesh->ConditionalConstructGenerator();

    if (MaxTileJobsCount > 0)
    {
        NavMesh->SetMaxSimultaneousTileGenerationJobsCount(MaxTileJobsCount);
    }

    const FBox Bounds(BoundsMin, BoundsMax);
    TArray<FIntPoint> TileCoords = GetTileCoordinatesInBounds(NavMesh, Bounds);

    if (TileCoords.Num() > 0)
    {
        NavMesh->RebuildTile(TileCoords);

        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: Bounds rebuild queued for %d tile(s) for agent [%s] (Bounds=%s to %s)"),
            TileCoords.Num(), *AgentName.ToString(),
            *BoundsMin.ToString(), *BoundsMax.ToString());
    }
    else
    {
        UE_LOG(LogTemp, Warning,
            TEXT("NavMeshSerializer: No tiles found in bounds for agent [%s]"),
            *AgentName.ToString());
    }
}

void ANavMeshSerializer::RebuildNavMeshInBoundsAll(FVector BoundsMin, FVector BoundsMax)
{
    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!NavSys)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: NavigationSystem not found"));
        return;
    }

    NavSys->SetNavigationOctreeLock(false);

    TArray<ARecastNavMesh*> AllNavMeshes = GetAllRecastNavMeshes();
    const FBox Bounds(BoundsMin, BoundsMax);

    for (ARecastNavMesh* NavMesh : AllNavMeshes)
    {
        NavMesh->ConditionalConstructGenerator();

        if (MaxTileJobsCount > 0)
        {
            NavMesh->SetMaxSimultaneousTileGenerationJobsCount(MaxTileJobsCount);
        }

        TArray<FIntPoint> TileCoords = GetTileCoordinatesInBounds(NavMesh, Bounds);
        if (TileCoords.Num() > 0)
        {
            NavMesh->RebuildTile(TileCoords);

            const FName AgentName = GetAgentNameFromNavMesh(NavMesh);
            UE_LOG(LogTemp, Log,
                TEXT("NavMeshSerializer: Bounds rebuild queued for %d tile(s) for agent [%s]"),
                TileCoords.Num(), *AgentName.ToString());
        }
    }

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Bounds rebuild triggered for all %d agent(s) (Bounds=%s to %s)"),
        AllNavMeshes.Num(), *BoundsMin.ToString(), *BoundsMax.ToString());
}

bool ANavMeshSerializer::GetSubLevelBounds(const FString& SubLevelName, FBox& OutBounds) const
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return false;
    }

    OutBounds.Init();

    bool bFoundLevel = false;

    // ストリーミングレベル（サブレベル）を検索
    const TArray<ULevelStreaming*>& StreamingLevels = World->GetStreamingLevels();
    for (const ULevelStreaming* StreamingLevel : StreamingLevels)
    {
        if (!StreamingLevel) continue;

        // パッケージ名の末尾がサブレベル名と一致するか確認
        const FString PackageName = StreamingLevel->GetWorldAssetPackageName();
        if (!PackageName.EndsWith(SubLevelName))
        {
            continue;
        }

        ULevel* Level = StreamingLevel->GetLoadedLevel();
        if (!Level)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("NavMeshSerializer: SubLevel [%s] found but not loaded"),
                *SubLevelName);
            return false;
        }

        bFoundLevel = true;

        // レベル内の全Actorのバウンズを合算
        for (AActor* Actor : Level->Actors)
        {
            if (!Actor) continue;

            FVector Origin, Extent;
            Actor->GetActorBounds(false, Origin, Extent);

            // バウンズが有効な（サイズがある）Actorのみ合算
            if (!Extent.IsNearlyZero())
            {
                const FBox ActorBox(Origin - Extent, Origin + Extent);
                OutBounds += ActorBox;
            }
        }

        break;
    }

    if (!bFoundLevel)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: SubLevel [%s] not found in streaming levels"),
            *SubLevelName);
        return false;
    }

    if (!OutBounds.IsValid)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("NavMeshSerializer: SubLevel [%s] has no actors with valid bounds"),
            *SubLevelName);
        return false;
    }

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: SubLevel [%s] bounds calculated: Min=%s Max=%s"),
        *SubLevelName, *OutBounds.Min.ToString(), *OutBounds.Max.ToString());

    return true;
}

void ANavMeshSerializer::RebuildNavMeshInSubLevelForAgent(const FString& SubLevelName, FName AgentName)
{
    FBox SubLevelBounds;
    if (!GetSubLevelBounds(SubLevelName, SubLevelBounds))
    {
        OnOperationFailed.Broadcast();
        return;
    }

    RebuildNavMeshInBoundsForAgent(SubLevelBounds.Min, SubLevelBounds.Max, AgentName);
}

void ANavMeshSerializer::RebuildNavMeshInSubLevelAll(const FString& SubLevelName)
{
    FBox SubLevelBounds;
    if (!GetSubLevelBounds(SubLevelName, SubLevelBounds))
    {
        OnOperationFailed.Broadcast();
        return;
    }

    RebuildNavMeshInBoundsAll(SubLevelBounds.Min, SubLevelBounds.Max);
}

//======================================================================//
// ビルド進捗モニタリング
//======================================================================//

void ANavMeshSerializer::StartBuildProgressMonitor()
{
    TArray<ARecastNavMesh*> AllNavMeshes = GetAllRecastNavMeshes();
    if (AllNavMeshes.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("NavMeshSerializer: No NavMesh to monitor"));
        return;
    }

    InitialBuildTaskCounts.Empty();
    AgentDebugMsgKeys.Empty();

    for (ARecastNavMesh* NavMesh : AllNavMeshes)
    {
        const FName AgentName = GetAgentNameFromNavMesh(NavMesh);
        AgentDebugMsgKeys.Add(AgentName, NextDebugMsgKey++);

        // 初期タスク数は0で仮登録。
        // RebuildAll直後はGeneratorがまだタスクをキューに積み終わっていない場合があるため、
        // Tick内のウォームアップフェーズで最大値を捕捉する。
        InitialBuildTaskCounts.Add(AgentName, 0);
    }

    bMonitoringBuildProgress = true;
    MonitorWarmupFrames = 0;
    BuildStartRealTime = FPlatformTime::Seconds();
    AgentCompletionTimes.Empty();
    CancelledAgents.Empty();
    AgentCancelledProgress.Empty();
    AgentCancelledTimes.Empty();
    SetActorTickEnabled(true);

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Build progress monitor started for %d agent(s)"),
        AllNavMeshes.Num());
}

void ANavMeshSerializer::StopBuildProgressMonitor()
{
    bMonitoringBuildProgress = false;
    SetActorTickEnabled(false);

    if (GEngine)
    {
        for (auto& Pair : AgentDebugMsgKeys)
        {
            GEngine->AddOnScreenDebugMessage(Pair.Value, 0.0f, FColor::Transparent, TEXT(""));
        }
    }

    UE_LOG(LogTemp, Log, TEXT("NavMeshSerializer: Build progress monitor stopped"));
}

void ANavMeshSerializer::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // --- ダーティフラグによる可視化の差分更新 ---
    if (DirtyVisAgents.Num() > 0)
    {
        TimeSinceLastVisUpdate += DeltaTime;
        if (TimeSinceLastVisUpdate >= VisualizationUpdateInterval)
        {
            TimeSinceLastVisUpdate = 0.0f;

            for (const FName& AgentName : DirtyVisAgents)
            {
                ARecastNavMesh* NavMesh = FindNavMeshByAgentName(AgentName);
                if (NavMesh)
                {
                    BuildNavMeshVisualizationForAgent(NavMesh);
                }
            }
            DirtyVisAgents.Empty();
        }
    }

    // --- ビルド進捗モニタリング ---
    if (!bMonitoringBuildProgress || !GEngine)
    {
        return;
    }

    // ウォームアップ: 最初の数十フレームはタスク数が増加していく可能性があるため
    // 各エージェントのGetNumRemaningBuildTasksの最大値を初期値として捕捉し続ける
    constexpr int32 WarmupDuration = 30; // 約0.5秒（60fps想定）
    const bool bInWarmup = (MonitorWarmupFrames < WarmupDuration);
    if (bInWarmup)
    {
        MonitorWarmupFrames++;
    }

    // 実時間ベースで経過時間を算出（DeltaTimeはクランプされるためFPS低下時に不正確）
    const double BuildElapsedTime = FPlatformTime::Seconds() - BuildStartRealTime;

    bool bAllComplete = true;

    for (auto& Pair : InitialBuildTaskCounts)
    {
        const FName AgentName = Pair.Key;
        int32& InitialTasks = Pair.Value;

        ARecastNavMesh* NavMesh = FindNavMeshByAgentName(AgentName);
        if (!NavMesh)
        {
            continue;
        }

        // GetGenerator() → FNavDataGenerator* → GetNumRemaningBuildTasks()
        const FNavDataGenerator* Generator = NavMesh->GetGenerator();
        const int32 RemainingTasks = Generator ? Generator->GetNumRemaningBuildTasks() : 0;
        const int32 RunningTasks = Generator ? Generator->GetNumRunningBuildTasks() : 0;

        // ウォームアップ中は観測された最大値で初期タスク数を更新
        if (bInWarmup && RemainingTasks > InitialTasks)
        {
            InitialTasks = RemainingTasks;
        }

        // 進捗率を計算
        double Progress = 100.0;
        if (InitialTasks > 0)
        {
            const int32 CompletedTasks = InitialTasks - RemainingTasks;
            Progress = FMath::Clamp(
                static_cast<double>(CompletedTasks) / static_cast<double>(InitialTasks) * 100.0,
                0.0,
                100.0
            );
        }

        const bool bIsBuilding = (RemainingTasks > 0);
        const bool bIsCancelled = CancelledAgents.Contains(AgentName);

        if (bIsBuilding && !bIsCancelled)
        {
            bAllComplete = false;
        }
        else if (!bIsBuilding && !bIsCancelled && !AgentCompletionTimes.Contains(AgentName) && !bInWarmup && InitialTasks > 0)
        {
            // このエージェントが今フレームで完了した
            AgentCompletionTimes.Add(AgentName, BuildElapsedTime);
        }

        // 表示内容を決定
        FString ProgressStr;
        FColor DisplayColor;

        if (bIsCancelled)
        {
            // キャンセル済み: キャンセル時の進捗と時間を表示
            const double CancelledProgress = AgentCancelledProgress.FindRef(AgentName);
            const double CancelledTime = AgentCancelledTimes.FindRef(AgentName);
            ProgressStr = FString::Printf(
                TEXT("%s : %.4f%%  (%d/%d tasks)  %.2fs [Cancelled]"),
                *AgentName.ToString(),
                CancelledProgress,
                (InitialTasks - RemainingTasks),
                InitialTasks,
                CancelledTime
            );
            DisplayColor = FColor::Orange;
        }
        else if (bInWarmup && InitialTasks == 0)
        {
            const double DisplayTime = BuildElapsedTime;
            ProgressStr = FString::Printf(
                TEXT("%s : Waiting...  Running: %d  %.2fs"),
                *AgentName.ToString(),
                RunningTasks,
                DisplayTime
            );
            DisplayColor = FColor::Yellow;
        }
        else if (bIsBuilding)
        {
            const double DisplayTime = BuildElapsedTime;
            ProgressStr = FString::Printf(
                TEXT("%s : %.4f%%  (%d/%d tasks)  Running: %d  %.2fs"),
                *AgentName.ToString(),
                Progress,
                (InitialTasks - RemainingTasks),
                InitialTasks,
                RunningTasks,
                DisplayTime
            );
            DisplayColor = FColor::Yellow;
        }
        else
        {
            // 完了済み
            const double DisplayTime = AgentCompletionTimes.Contains(AgentName)
                ? AgentCompletionTimes[AgentName]
                : BuildElapsedTime;
            ProgressStr = FString::Printf(
                TEXT("%s : %.4f%%  (%d/%d tasks)  %.2fs"),
                *AgentName.ToString(),
                Progress,
                (InitialTasks - RemainingTasks),
                InitialTasks,
                DisplayTime
            );
            DisplayColor = FColor::Green;
        }

        const int32* MsgKeyPtr = AgentDebugMsgKeys.Find(AgentName);
        if (MsgKeyPtr)
        {
            GEngine->AddOnScreenDebugMessage(
                *MsgKeyPtr,
                0.0f,
                DisplayColor,
                ProgressStr
            );
        }
    }

    // 全エージェントが完了（ウォームアップ完了後のみ判定）
    if (bAllComplete && !bInWarmup)
    {
        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: All agents build complete. Stopping monitor."));

        for (auto& Pair : AgentDebugMsgKeys)
        {
            const FName AgentName = Pair.Key;
            const int32 InitialTasks = InitialBuildTaskCounts.FindRef(AgentName);

            FString CompleteStr;
            FColor CompleteColor;

            if (CancelledAgents.Contains(AgentName))
            {
                const double CancelledProgress = AgentCancelledProgress.FindRef(AgentName);
                const double CancelledTime = AgentCancelledTimes.FindRef(AgentName);
                CompleteStr = FString::Printf(
                    TEXT("%s : %.4f%%  (%d/%d tasks)  %.2fs [Cancelled]"),
                    *AgentName.ToString(),
                    CancelledProgress,
                    InitialTasks, // キャンセル後はRemainingが0なのでInitialTasks表示
                    InitialTasks,
                    CancelledTime
                );
                CompleteColor = FColor::Orange;
            }
            else
            {
                const double CompletionTime = AgentCompletionTimes.Contains(AgentName)
                    ? AgentCompletionTimes[AgentName]
                    : BuildElapsedTime;
                CompleteStr = FString::Printf(
                    TEXT("%s : 100.0000%%  (%d/%d tasks)  %.2fs [Complete]"),
                    *AgentName.ToString(),
                    InitialTasks,
                    InitialTasks,
                    CompletionTime
                );
                CompleteColor = FColor::Green;
            }

            GEngine->AddOnScreenDebugMessage(
                Pair.Value,
                5.0f,
                CompleteColor,
                CompleteStr
            );
        }

        bMonitoringBuildProgress = false;

        // 遅延フラッシュ: 完了判定後にまだタイル通知が遅れて届く場合に備え、
        // 少し待ってから有効な全エージェントの可視化を最終更新する
        if (EnabledVisAgents.Num() > 0)
        {
            GetWorldTimerManager().SetTimer(
                VisFinalFlushHandle,
                this,
                &ANavMeshSerializer::FlushVisualizationForAllEnabled,
                1.0f,  // 1秒後に最終フラッシュ
                false
            );
        }

        // 可視化が有効なエージェントがあればTickは継続
        if (EnabledVisAgents.Num() == 0)
        {
            SetActorTickEnabled(false);
        }
    }
}

//======================================================================//
// タイル操作（部分削除・部分追加）
//======================================================================//

FBox ANavMeshSerializer::MakeBoundsFromRadius(FVector Center, float Radius)
{
    const FVector Extent(Radius, Radius, Radius);
    return FBox(Center - Extent, Center + Extent);
}

TArray<FIntPoint> ANavMeshSerializer::GetTileCoordinatesInBounds(ARecastNavMesh* NavMesh, const FBox& Bounds)
{
    TArray<FIntPoint> TileCoords;
    if (!NavMesh)
    {
        return TileCoords;
    }

    // Boundsの四隅からタイル座標範囲を特定
    int32 MinTileX, MinTileY, MaxTileX, MaxTileY;
    if (!NavMesh->GetNavMeshTileXY(Bounds.Min, MinTileX, MinTileY) ||
        !NavMesh->GetNavMeshTileXY(Bounds.Max, MaxTileX, MaxTileY))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("NavMeshSerializer: Could not determine tile coordinates for bounds %s"),
            *Bounds.ToString());
        return TileCoords;
    }

    // Min/Maxが逆転している場合に対応
    if (MinTileX > MaxTileX) Swap(MinTileX, MaxTileX);
    if (MinTileY > MaxTileY) Swap(MinTileY, MaxTileY);

    for (int32 TileY = MinTileY; TileY <= MaxTileY; ++TileY)
    {
        for (int32 TileX = MinTileX; TileX <= MaxTileX; ++TileX)
        {
            TileCoords.Add(FIntPoint(TileX, TileY));
        }
    }

    return TileCoords;
}

void ANavMeshSerializer::RemoveNavMeshInRadiusForAgent(FVector Center, float Radius, FName AgentName)
{
    ARecastNavMesh* NavMesh = FindNavMeshByAgentName(AgentName);
    if (!NavMesh)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Cannot remove tiles — agent [%s] not found"),
            *AgentName.ToString());
        return;
    }

    const FBox Bounds = MakeBoundsFromRadius(Center, Radius);
    TArray<FIntPoint> TileCoords = GetTileCoordinatesInBounds(NavMesh, Bounds);
    if (TileCoords.Num() > 0)
    {
        NavMesh->RemoveTiles(TileCoords);

        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: Removed %d tile(s) for agent [%s] at %s (Radius=%.1f)"),
            TileCoords.Num(), *AgentName.ToString(),
            *Center.ToString(), Radius);
    }
}

void ANavMeshSerializer::RebuildNavMeshInRadiusForAgent(FVector Center, float Radius, FName AgentName)
{
    ARecastNavMesh* NavMesh = FindNavMeshByAgentName(AgentName);
    if (!NavMesh)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Cannot rebuild tiles — agent [%s] not found"),
            *AgentName.ToString());
        return;
    }

    const FBox Bounds = MakeBoundsFromRadius(Center, Radius);
    TArray<FIntPoint> TileCoords = GetTileCoordinatesInBounds(NavMesh, Bounds);
    if (TileCoords.Num() > 0)
    {
        NavMesh->RebuildTile(TileCoords);

        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: Rebuild queued for %d tile(s) for agent [%s] at %s (Radius=%.1f)"),
            TileCoords.Num(), *AgentName.ToString(),
            *Center.ToString(), Radius);
    }
}

void ANavMeshSerializer::RemoveAllNavMeshForAgent(FName AgentName)
{
    ARecastNavMesh* NavMesh = FindNavMeshByAgentName(AgentName);
    if (!NavMesh)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Cannot remove all tiles — agent [%s] not found"),
            *AgentName.ToString());
        return;
    }

    const int32 TileCount = NavMesh->GetNavMeshTilesCount();
    if (TileCount == 0)
    {
        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: Agent [%s] has no tiles to remove"),
            *AgentName.ToString());
        return;
    }

    // 全タイルの座標を収集（レイヤー違いの重複を排除）
    TSet<FIntPoint> UniqueCoords;
    for (int32 TileIdx = 0; TileIdx < TileCount; ++TileIdx)
    {
        int32 TileX, TileY, Layer;
        if (NavMesh->GetNavMeshTileXY(TileIdx, TileX, TileY, Layer))
        {
            UniqueCoords.Add(FIntPoint(TileX, TileY));
        }
    }

    if (UniqueCoords.Num() > 0)
    {
        TArray<FIntPoint> TileCoords = UniqueCoords.Array();
        NavMesh->RemoveTiles(TileCoords);

        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: Removed %d tile coord(s) for agent [%s]"),
            TileCoords.Num(), *AgentName.ToString());
    }

    // 可視化中であればクリア
    if (EnabledVisAgents.Contains(AgentName))
    {
        ClearNavMeshVisualizationForAgent(AgentName);
    }
}

void ANavMeshSerializer::CancelNavMeshBuildForAgent(FName AgentName)
{
    ARecastNavMesh* NavMesh = FindNavMeshByAgentName(AgentName);
    if (!NavMesh)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Cannot cancel build — agent [%s] not found"),
            *AgentName.ToString());
        return;
    }

    FNavDataGenerator* Generator = NavMesh->GetGenerator();
    if (!Generator)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("NavMeshSerializer: Agent [%s] has no generator (build may not be in progress)"),
            *AgentName.ToString());
        return;
    }

    // キャンセル時の進捗と時間を記録
    if (bMonitoringBuildProgress)
    {
        const int32* InitialTasksPtr = InitialBuildTaskCounts.Find(AgentName);
        const int32 InitialTasks = InitialTasksPtr ? *InitialTasksPtr : 0;
        const int32 RemainingTasks = Generator->GetNumRemaningBuildTasks();

        double Progress = 0.0;
        if (InitialTasks > 0)
        {
            const int32 CompletedTasks = InitialTasks - RemainingTasks;
            Progress = FMath::Clamp(
                static_cast<double>(CompletedTasks) / static_cast<double>(InitialTasks) * 100.0,
                0.0, 100.0);
        }

        const double ElapsedTime = FPlatformTime::Seconds() - BuildStartRealTime;

        CancelledAgents.Add(AgentName);
        AgentCancelledProgress.Add(AgentName, Progress);
        AgentCancelledTimes.Add(AgentName, ElapsedTime);
    }

    Generator->CancelBuild();

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Build cancelled for agent [%s]. "
            "Already-built tiles remain. Call EnableNavMeshDynamicRebuild to restart."),
        *AgentName.ToString());
}

void ANavMeshSerializer::SetAgentRadiusForAgent(FName AgentName, float NewRadius)
{
    ARecastNavMesh* NavMesh = FindNavMeshByAgentName(AgentName);
    if (!NavMesh)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Cannot set AgentRadius — agent [%s] not found"),
            *AgentName.ToString());
        return;
    }

    NavMesh->AgentRadius = FMath::Max(0.0f, NewRadius);

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: AgentRadius set to %.2f for agent [%s]. Rebuild to apply."),
        NavMesh->AgentRadius, *AgentName.ToString());
}

void ANavMeshSerializer::SetAgentMaxSlopeForAgent(FName AgentName, float NewMaxSlope)
{
    ARecastNavMesh* NavMesh = FindNavMeshByAgentName(AgentName);
    if (!NavMesh)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Cannot set AgentMaxSlope — agent [%s] not found"),
            *AgentName.ToString());
        return;
    }

    NavMesh->AgentMaxSlope = FMath::Clamp(NewMaxSlope, 0.0f, 89.0f);

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: AgentMaxSlope set to %.2f for agent [%s]. Rebuild to apply."),
        NavMesh->AgentMaxSlope, *AgentName.ToString());
}

void ANavMeshSerializer::SetAgentMaxStepHeightForAgent(FName AgentName, float NewMaxStepHeight)
{
    ARecastNavMesh* NavMesh = FindNavMeshByAgentName(AgentName);
    if (!NavMesh)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Cannot set AgentMaxStepHeight — agent [%s] not found"),
            *AgentName.ToString());
        return;
    }

    const float ClampedValue = FMath::Max(0.0f, NewMaxStepHeight);

    // 全Resolution（Default, Low, High等）に一括適用
    for (uint8 i = 0; i < static_cast<uint8>(ENavigationDataResolution::MAX); ++i)
    {
        NavMesh->SetAgentMaxStepHeight(static_cast<ENavigationDataResolution>(i), ClampedValue);
    }

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: AgentMaxStepHeight set to %.2f for agent [%s] (all resolutions). Rebuild to apply."),
        ClampedValue, *AgentName.ToString());
}

//======================================================================//
// 可視化
//======================================================================//

void ANavMeshSerializer::SetNavMeshVisualizationEnabled(bool bEnabled, FName AgentName)
{
    if (bEnabled)
    {
        ARecastNavMesh* NavMesh = FindNavMeshByAgentName(AgentName);
        if (!NavMesh)
        {
            UE_LOG(LogTemp, Error,
                TEXT("NavMeshSerializer: Cannot enable visualization — agent [%s] not found"),
                *AgentName.ToString());
            return;
        }

        EnabledVisAgents.Add(AgentName);

        // 初回は全体を構築
        BuildNavMeshVisualizationForAgent(NavMesh);

        // タイル更新デリゲートをバインド
        BindTileUpdateDelegate(NavMesh);

        // Tick を有効化（ダーティフラグ処理のため）
        SetActorTickEnabled(true);
    }
    else
    {
        EnabledVisAgents.Remove(AgentName);
        DirtyVisAgents.Remove(AgentName);
        ClearNavMeshVisualizationForAgent(AgentName);

        // デリゲートをアンバインド
        if (FDelegateHandle* Handle = TileUpdateDelegateHandles.Find(AgentName))
        {
            ARecastNavMesh* NavMesh = FindNavMeshByAgentName(AgentName);
            if (AObservableRecastNavMesh* Observable = Cast<AObservableRecastNavMesh>(NavMesh))
            {
                Observable->OnTilesChanged.Remove(*Handle);
            }
            TileUpdateDelegateHandles.Remove(AgentName);
        }

        // 可視化もモニタリングも不要なら Tick を止める
        if (EnabledVisAgents.Num() == 0 && !bMonitoringBuildProgress)
        {
            SetActorTickEnabled(false);
        }
    }
}

bool ANavMeshSerializer::IsNavMeshVisualizationEnabled(FName AgentName) const
{
    return EnabledVisAgents.Contains(AgentName);
}

void ANavMeshSerializer::BindTileUpdateDelegate(ARecastNavMesh* NavMesh)
{
    AObservableRecastNavMesh* Observable = Cast<AObservableRecastNavMesh>(NavMesh);
    if (!Observable)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("NavMeshSerializer: NavMesh for agent [%s] is not AObservableRecastNavMesh. "
                "Incremental visualization updates will not work. "
                "Please set NavDataClass to ObservableRecastNavMesh in Project Settings > Navigation System."),
            *GetAgentNameFromNavMesh(NavMesh).ToString());
        return;
    }

    const FName AgentName = GetAgentNameFromNavMesh(NavMesh);

    // 二重バインド防止
    if (TileUpdateDelegateHandles.Contains(AgentName))
    {
        Observable->OnTilesChanged.Remove(TileUpdateDelegateHandles[AgentName]);
    }

    FDelegateHandle Handle = Observable->OnTilesChanged.AddUObject(
        this, &ANavMeshSerializer::OnTilesChanged);

    TileUpdateDelegateHandles.Add(AgentName, Handle);

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Tile update delegate bound for agent [%s]"),
        *AgentName.ToString());
}

void ANavMeshSerializer::UnbindTileUpdateDelegates()
{
    for (auto& Pair : TileUpdateDelegateHandles)
    {
        ARecastNavMesh* NavMesh = FindNavMeshByAgentName(Pair.Key);
        if (AObservableRecastNavMesh* Observable = Cast<AObservableRecastNavMesh>(NavMesh))
        {
            Observable->OnTilesChanged.Remove(Pair.Value);
        }
    }
    TileUpdateDelegateHandles.Empty();
}

void ANavMeshSerializer::FlushVisualizationForAllEnabled()
{
    for (const FName& AgentName : EnabledVisAgents)
    {
        ARecastNavMesh* NavMesh = FindNavMeshByAgentName(AgentName);
        if (NavMesh)
        {
            BuildNavMeshVisualizationForAgent(NavMesh);
        }
    }
    DirtyVisAgents.Empty();
    TimeSinceLastVisUpdate = 0.0f;

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Final visualization flush completed for %d agent(s)"),
        EnabledVisAgents.Num());
}

void ANavMeshSerializer::OnTilesChanged(ARecastNavMesh* NavMesh, const TArray<int32>& ChangedTileIndices)
{
    if (!NavMesh) return;

    const FName AgentName = GetAgentNameFromNavMesh(NavMesh);

    if (EnabledVisAgents.Contains(AgentName))
    {
        DirtyVisAgents.Add(AgentName);
    }
}

void ANavMeshSerializer::BuildNavMeshVisualizationForAgent(ARecastNavMesh* NavMesh)
{
    if (!NavMesh) return;

    const FName AgentName = GetAgentNameFromNavMesh(NavMesh);

    TObjectPtr<UProceduralMeshComponent>* FoundComp = NavMeshVisComponents.Find(AgentName);
    UProceduralMeshComponent* VisMesh = nullptr;

    if (FoundComp && *FoundComp)
    {
        VisMesh = *FoundComp;
    }
    else
    {
        VisMesh = NewObject<UProceduralMeshComponent>(this);
        VisMesh->RegisterComponent();
        VisMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        VisMesh->bUseComplexAsSimpleCollision = false;
        NavMeshVisComponents.Add(AgentName, VisMesh);
    }

    VisMesh->ClearAllMeshSections();

    // INDEX_NONE で全タイルのジオメトリを一括取得
    FRecastDebugGeometry DebugGeom;
    NavMesh->GetDebugGeometryForTile(DebugGeom, INDEX_NONE);

    if (DebugGeom.MeshVerts.IsEmpty())
    {
        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: DebugGeom has no verts for agent [%s] — NavMesh may still be building"),
            *AgentName.ToString());
        return;
    }

    TArray<FVector>  Vertices = DebugGeom.MeshVerts;
    TArray<int32>    Indices;
    TArray<FVector>  Normals;
    TArray<FVector2D> UV0;
    TArray<FColor>   Colors;
    TArray<FProcMeshTangent> Tangents;

    // Zオフセットを適用
    const float* ZOffsetPtr = AgentVisZOffsets.Find(AgentName);
    if (ZOffsetPtr && !FMath::IsNearlyZero(*ZOffsetPtr))
    {
        const float ZOffset = *ZOffsetPtr;
        for (FVector& Vert : Vertices)
        {
            Vert.Z += ZOffset;
        }
    }

    Normals.Init(FVector::UpVector, Vertices.Num());

    for (int32 AreaIdx = 0; AreaIdx < RECAST_MAX_AREAS; ++AreaIdx)
    {
        Indices.Append(DebugGeom.AreaIndices[AreaIdx]);
    }

    Colors.Init(FColor::White, Vertices.Num());
    UV0.Init(FVector2D::ZeroVector, Vertices.Num());

    VisMesh->CreateMeshSection(
        0, Vertices, Indices, Normals, UV0, Colors, Tangents, false
    );

    if (TObjectPtr<UMaterialInterface>* MatPtr = AgentVisMaterials.Find(AgentName))
    {
        if (*MatPtr)
        {
            VisMesh->SetMaterial(0, *MatPtr);
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning,
            TEXT("NavMeshSerializer: No material set for agent [%s] in AgentVisMaterials map."),
            *AgentName.ToString());
    }

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Visualization built for agent [%s] (Verts=%d Indices=%d)"),
        *AgentName.ToString(), Vertices.Num(), Indices.Num());
}

void ANavMeshSerializer::ClearNavMeshVisualizationForAgent(FName AgentName)
{
    if (TObjectPtr<UProceduralMeshComponent>* FoundComp = NavMeshVisComponents.Find(AgentName))
    {
        if (*FoundComp)
        {
            (*FoundComp)->ClearAllMeshSections();
        }
    }
}

//======================================================================//
// 保存
//======================================================================//

void ANavMeshSerializer::SaveNavMeshWhenReady(const FString& StageID)
{
    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!NavSys)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: NavigationSystem not found"));
        OnOperationFailed.Broadcast();
        return;
    }

    if (!NavSys->IsNavigationBuildInProgress())
    {
        SaveAllNavMeshes(StageID);
        return;
    }

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: NavMesh building in progress. "
            "Will save automatically when build completes."));

    PendingSaveStageID = StageID;
    NavSys->OnNavigationGenerationFinishedDelegate.AddDynamic(
        this, &ANavMeshSerializer::OnNavBuildFinished);
}

void ANavMeshSerializer::OnNavBuildFinished(ANavigationData* NavData)
{
    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (NavSys)
    {
        NavSys->OnNavigationGenerationFinishedDelegate.RemoveDynamic(
            this, &ANavMeshSerializer::OnNavBuildFinished);
    }

    SaveAllNavMeshes(PendingSaveStageID);
    PendingSaveStageID.Empty();
}

void ANavMeshSerializer::SaveAllNavMeshes(const FString& StageID)
{
    TArray<ARecastNavMesh*> AllNavMeshes = GetAllRecastNavMeshes();
    if (AllNavMeshes.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: No NavMesh to save"));
        OnOperationFailed.Broadcast();
        return;
    }

    const FString SaveDir = FPaths::ProjectSavedDir() / TEXT("Stages");
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.DirectoryExists(*SaveDir))
    {
        PlatformFile.CreateDirectoryTree(*SaveDir);
    }

    int32 SuccessCount = 0;
    for (ARecastNavMesh* NavMesh : AllNavMeshes)
    {
        const FName AgentName = GetAgentNameFromNavMesh(NavMesh);
        const FString FilePath = GetNavDataFilePath(StageID, AgentName.ToString());

        if (SaveSingleNavMesh(NavMesh, FilePath))
        {
            UE_LOG(LogTemp, Log,
                TEXT("NavMeshSerializer: Saved agent [%s] (Actor=[%s]) -> [%s]"),
                *AgentName.ToString(), *NavMesh->GetName(), *FilePath);
            ++SuccessCount;
        }
    }

    if (SuccessCount == AllNavMeshes.Num())
    {
        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: All %d NavMesh(es) saved successfully for StageID [%s]"),
            SuccessCount, *StageID);
        OnSaveComplete.Broadcast();
    }
    else
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Only %d/%d NavMesh(es) saved for StageID [%s]"),
            SuccessCount, AllNavMeshes.Num(), *StageID);
        OnOperationFailed.Broadcast();
    }
}

bool ANavMeshSerializer::SaveSingleNavMesh(ARecastNavMesh* NavMesh, const FString& FilePath)
{
    if (!NavMesh) return false;

    // 1. シリアライズ
    TArray<uint8> NavMeshData;
    FMemoryWriter MemWriter(NavMeshData, true);
    FObjectAndNameAsStringProxyArchive Writer(MemWriter, false);
    NavMesh->Serialize(Writer);

    if (NavMeshData.Num() == 0)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Serialized data is empty for agent [%s]."),
            *GetAgentNameFromNavMesh(NavMesh).ToString());
        return false;
    }

    const int32 UncompressedSize = NavMeshData.Num();

    // 2. 圧縮

    // UncompressedSizeを圧縮する前に、最悪でも必要なメモリ(バッファ)サイズを取得
    // MEMO : 圧縮後サイズは事前に正確には分からないので最悪数必要なメモリを確保する必要がある
    int32 CompressedSize = FCompression::CompressMemoryBound(NAME_Zlib, UncompressedSize);
    TArray<uint8> CompressedData;

    // メモリ(バッファ)を確保 この後すぐに圧縮データで上書きするため初期化は必要なし(UnInitialize)
    CompressedData.SetNumUninitialized(CompressedSize);

    // 圧縮を実施
    if (!FCompression::CompressMemory(
        NAME_Zlib,                      // 使用する圧縮アルゴリズム
        CompressedData.GetData(),       // 圧縮後のデータを書き込む先頭アドレス
        CompressedSize,                 // 入出力兼用 : 関数を呼ぶ前は何バイトまで書き込んで良いか 呼んだ後は実際に書き込まれた圧縮後のサイズとなる
        NavMeshData.GetData(),          // 圧縮したい元データの先頭アドレス(このデータを圧縮してねという入力元)
        UncompressedSize))              // 元データのサイズ 第4引数の先頭から何バイトを読み取るか教える
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Compression failed for agent [%s]"),
            *GetAgentNameFromNavMesh(NavMesh).ToString());
        return false;
    }

    // 実際に書き込まれた圧縮後のサイズとする(SetNumUninitializedでは最大サイズ確保したので実際に必要だった分まで切り詰める)
    CompressedData.SetNum(CompressedSize);

    // 3. ヘッダー(非圧縮サイズ)+圧縮データを書き出し
    TArray<uint8> FileData;
    FileData.SetNumUninitialized(sizeof(int32) + CompressedSize);// 先頭32ビットに展開時に必要なメモリ(バッファ)量値を格納
    FMemory::Memcpy(FileData.GetData(), &UncompressedSize, sizeof(int32));
    FMemory::Memcpy(FileData.GetData() + sizeof(int32), CompressedData.GetData(), CompressedSize);

    if (FFileHelper::SaveArrayToFile(FileData, *FilePath))
    {
        const float Ratio = (UncompressedSize > 0)
            ? static_cast<float>(CompressedSize) / static_cast<float>(UncompressedSize) * 100.0f
            : 0.0f;
        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: Saved agent [%s] compressed: %d -> %d bytes (%.1f%%)"),
            *GetAgentNameFromNavMesh(NavMesh).ToString(),
            UncompressedSize, CompressedSize, Ratio);
        return true;
    }

    UE_LOG(LogTemp, Error,
        TEXT("NavMeshSerializer: Failed to write file [%s]"), *FilePath);
    return false;
}

//======================================================================//
// ロード・適用
//======================================================================//

void ANavMeshSerializer::LoadAndApplyNavMesh(const FString& StageID, FVector StageOffset)
{
    LoadAndApplyAllNavMeshes(StageID, StageOffset);
}

void ANavMeshSerializer::LoadAndApplyAllNavMeshes(const FString& StageID, FVector StageOffset)
{
    TArray<ARecastNavMesh*> AllNavMeshes = GetAllRecastNavMeshes();
    if (AllNavMeshes.Num() == 0)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: No NavMesh actors in world to restore into"));
        OnOperationFailed.Broadcast();
        return;
    }

    DisableNavMeshAutoRebuild();

    int32 SuccessCount = 0;

    for (ARecastNavMesh* NavMesh : AllNavMeshes)
    {
        const FName AgentName = GetAgentNameFromNavMesh(NavMesh);
        const FString FilePath = GetNavDataFilePath(StageID, AgentName.ToString());

        if (FPaths::FileExists(FilePath))
        {
            if (LoadSingleNavMesh(NavMesh, FilePath, StageOffset))
            {
                UE_LOG(LogTemp, Log,
                    TEXT("NavMeshSerializer: Loaded agent [%s] (Actor=[%s]) <- [%s]"),
                    *AgentName.ToString(), *NavMesh->GetName(), *FilePath);
                ++SuccessCount;
            }
        }
        else
        {
            UE_LOG(LogTemp, Warning,
                TEXT("NavMeshSerializer: No saved data found for agent [%s] at [%s]"),
                *AgentName.ToString(), *FilePath);
        }
    }

    if (SuccessCount == 0)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: No NavMesh data loaded for StageID [%s]"), *StageID);
        OnOperationFailed.Broadcast();
        return;
    }

    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (NavSys)
    {
        PendingRegistrationCount = 0;
        for (ARecastNavMesh* NavMesh : AllNavMeshes)
        {
            NavSys->UnregisterNavData(NavMesh);
            NavSys->RequestRegistrationDeferred(*NavMesh);
            ++PendingRegistrationCount;
        }

        GetWorldTimerManager().SetTimer(
            RegistrationWaitHandle,
            this,
            &ANavMeshSerializer::OnNavDataRegistrationComplete,
            0.1f,
            false
        );
    }

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Loaded %d/%d NavMesh(es) for StageID [%s]"),
        SuccessCount, AllNavMeshes.Num(), *StageID);

    OnLoadComplete.Broadcast();
}

bool ANavMeshSerializer::LoadSingleNavMesh(ARecastNavMesh* NavMesh, const FString& FilePath, FVector StageOffset)
{
    if (!NavMesh) return false;

    // 1. ファイル読み込み
    TArray<uint8> FileData;
    if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Failed to read file [%s]"), *FilePath);
        return false;
    }

    if (FileData.Num() <= static_cast<int32>(sizeof(int32)))
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: File too small to contain valid data [%s]"), *FilePath);
        return false;
    }

    // 2. ヘッダーから非圧縮サイズを読み取り
    int32 UncompressedSize = 0;
    FMemory::Memcpy(&UncompressedSize, FileData.GetData(), sizeof(int32));// 先頭32ビットに展開時に必要なメモリ(非圧縮サイズ)が記載されているのでコピー

    if (UncompressedSize <= 0)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Invalid uncompressed size in file [%s]"), *FilePath);
        return false;
    }

    // 3. 解凍
    const int32 CompressedSize = FileData.Num() - sizeof(int32);// 先頭32ビットに展開時に必要なメモリが記載されているがこの時は必要ないので引く(つまり実データサイズとなる)
    TArray<uint8> NavMeshData;
    NavMeshData.SetNumUninitialized(UncompressedSize);

    if (!FCompression::UncompressMemory(
        NAME_Zlib,
        NavMeshData.GetData(),              // どこ(のアドレスから)に入れるか
        UncompressedSize,                   // どれくらいメモリバッファを空けるか(非圧縮サイズ分空けなくてはいけない)
        FileData.GetData() + sizeof(int32), // 先頭32ビットを飛ばしたアドレスから読み取りを指示(圧縮データのアドレス)
        CompressedSize))                    // どれくらい読み取るか
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Decompression failed for file [%s]"), *FilePath);
        return false;
    }

    // 4. デシリアライズ
    FMemoryReader MemReader(NavMeshData, true);
    FObjectAndNameAsStringProxyArchive Reader(MemReader, true);
    NavMesh->Serialize(Reader);

    if (!StageOffset.IsNearlyZero())
    {
        NavMesh->ApplyWorldOffset(StageOffset, false);
        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: Applied offset %s to agent [%s]"),
            *StageOffset.ToString(), *GetAgentNameFromNavMesh(NavMesh).ToString());
    }

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Loaded agent [%s] decompressed: %d -> %d bytes"),
        *GetAgentNameFromNavMesh(NavMesh).ToString(),
        CompressedSize, UncompressedSize);

    return true;
}

void ANavMeshSerializer::OnNavDataRegistrationComplete()
{
    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: NavData registration complete for all agents."));
    OnLoadComplete.Broadcast();
}