#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "NavMesh/RecastNavMesh.h"
#include "ProceduralMeshComponent.h"
#include "NavMeshSerializer.generated.h"

class AObservableRecastNavMesh;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnNavMeshSaveComplete);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnNavMeshLoadComplete);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnNavMeshOperationFailed);

UCLASS()
class SAVELOADNAVIMESHFILE_API ANavMeshSerializer : public AActor
{
    GENERATED_BODY()

public:
    ANavMeshSerializer();

    //----------------------------------------------------------------------//
    // 公開API（全エージェント一括操作）
    //----------------------------------------------------------------------//

    /** 全エージェントのNavMeshを一括保存（ビルド完了待ちあり） */
    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    void SaveNavMeshWhenReady(const FString& StageID);

    /** 全エージェントのNavMeshを一括ロード・適用 */
    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    void LoadAndApplyNavMesh(const FString& StageID, FVector StageOffset = FVector::ZeroVector);

    /** 指定StageIDの保存データが（1つ以上）存在するか */
    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    bool HasSavedNavMesh(const FString& StageID) const;

    /** 指定エージェントのNavMesh可視化を切り替える */
    UFUNCTION(BlueprintCallable, Category = "NavMesh|Debug")
    void SetNavMeshVisualizationEnabled(bool bEnabled, FName AgentName);

    /** 指定エージェントのNavMesh可視化が有効か */
    UFUNCTION(BlueprintCallable, Category = "NavMesh|Debug")
    bool IsNavMeshVisualizationEnabled(FName AgentName) const;

    /** 指定エージェントのNavMeshだけ動的リビルドを有効化 */
    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    void EnableNavMeshDynamicRebuild(FName AgentName);

    /** 全エージェントの動的リビルドを有効化 */
    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    void EnableNavMeshDynamicRebuildAll();

    /** 指定エージェントのNavMeshを、指定範囲内のタイルだけリビルドする。
     *  NavMeshBoundsVolume全体ではなく、地形がある範囲だけを対象にすることで
     *  空タイルの計算を回避し、ビルド時間を短縮できる。
     *  BoundsMinとBoundsMaxでワールド空間のAABBを指定する。 */
    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    void RebuildNavMeshInBoundsForAgent(FVector BoundsMin, FVector BoundsMax, FName AgentName);

    /** 全エージェントのNavMeshを、指定範囲内のタイルだけリビルドする。 */
    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    void RebuildNavMeshInBoundsAll(FVector BoundsMin, FVector BoundsMax);

    /** 指定サブレベル内の全Actorのバウンズを自動計算し、
     *  その範囲内のタイルだけ指定エージェントをリビルドする。
     *  SubLevelName はサブレベルのアセット名（例: "SubLevel_Forest"）。 */
    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    void RebuildNavMeshInSubLevelForAgent(const FString& SubLevelName, FName AgentName);

    /** 指定サブレベル内の全Actorのバウンズを自動計算し、
     *  その範囲内のタイルだけ全エージェントをリビルドする。 */
    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    void RebuildNavMeshInSubLevelAll(const FString& SubLevelName);

    //----------------------------------------------------------------------//
    // ビルド進捗モニタリング
    //----------------------------------------------------------------------//

    /** ビルド進捗の画面表示を開始する。
     *  各エージェントの進捗がPrintStringで画面左上に表示される。
     *  残りビルドタスク数ベースで進捗率を算出。
     *  全エージェントのビルドが完了すると自動停止。 */
    UFUNCTION(BlueprintCallable, Category = "NavMesh|Debug")
    void StartBuildProgressMonitor();

    /** ビルド進捗の画面表示を停止する */
    UFUNCTION(BlueprintCallable, Category = "NavMesh|Debug")
    void StopBuildProgressMonitor();

    //----------------------------------------------------------------------//
    // タイル操作（部分削除・部分追加）
    //----------------------------------------------------------------------//

    /** 指定エージェントのみ、指定位置を中心に半径内のNavMeshタイルを削除する */
    UFUNCTION(BlueprintCallable, Category = "NavMesh|Tiles")
    void RemoveNavMeshInRadiusForAgent(FVector Center, float Radius, FName AgentName);

    /** 指定エージェントのみ、指定位置を中心に半径内のNavMeshタイルを再構築する */
    UFUNCTION(BlueprintCallable, Category = "NavMesh|Tiles")
    void RebuildNavMeshInRadiusForAgent(FVector Center, float Radius, FName AgentName);

    /** 指定エージェントのNavMeshタイルを全削除する。可視化中であればそちらもクリアする */
    UFUNCTION(BlueprintCallable, Category = "NavMesh|Tiles")
    void RemoveAllNavMeshForAgent(FName AgentName);

    /** 指定エージェントのNavMeshビルドを中断する。
     *  既にビルド済みのタイルはNavMesh上に残る。
     *  再開するにはEnableNavMeshDynamicRebuildを再度呼ぶ（未ビルドのタイルから再開される）。*/
    UFUNCTION(BlueprintCallable, Category = "NavMesh|Build")
    void CancelNavMeshBuildForAgent(FName AgentName);

    //----------------------------------------------------------------------//
    // デリゲート
    //----------------------------------------------------------------------//

    UPROPERTY(BlueprintAssignable, Category = "NavMesh")
    FOnNavMeshSaveComplete OnSaveComplete;

    UPROPERTY(BlueprintAssignable, Category = "NavMesh")
    FOnNavMeshLoadComplete OnLoadComplete;

    UPROPERTY(BlueprintAssignable, Category = "NavMesh")
    FOnNavMeshOperationFailed OnOperationFailed;

    //----------------------------------------------------------------------//
    // BP設定
    //----------------------------------------------------------------------//

    /** エージェント名ごとの可視化マテリアル */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NavMesh|Debug")
    TMap<FName, TObjectPtr<UMaterialInterface>> AgentVisMaterials;

    /** エージェント名ごとの可視化メッシュZ高さオフセット。
     *  複数エージェントの可視化が重なる場合にずらして見やすくする。
     *  未設定のエージェントは0として扱う。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NavMesh|Debug")
    TMap<FName, float> AgentVisZOffsets;

    /** 非同期ビルド時の同時タイル生成ジョブ数上限。
     *  大きくするほどCPUコアを多く使いビルドが高速化する。
     *  0の場合はNavMeshのデフォルト値をそのまま使用。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NavMesh|Build", meta = (ClampMin = "0", UIMin = "0"))
    int32 MaxTileJobsCount = 0;

private:
    //----------------------------------------------------------------------//
    // 内部ヘルパー
    //----------------------------------------------------------------------//

    TArray<ARecastNavMesh*> GetAllRecastNavMeshes() const;
    ARecastNavMesh* FindNavMeshByAgentName(FName AgentName) const;
    static FName GetAgentNameFromNavMesh(const ARecastNavMesh* NavMesh);
    FString GetNavDataFilePath(const FString& StageID, const FString& AgentName) const;
    void DisableNavMeshAutoRebuild() const;

    /** 中心座標と半径からFBoxを生成 */
    static FBox MakeBoundsFromRadius(FVector Center, float Radius);

    /** NavMesh上で指定Bounds内に含まれるタイル座標一覧を取得 */
    static TArray<FIntPoint> GetTileCoordinatesInBounds(ARecastNavMesh* NavMesh, const FBox& Bounds);

    /** 指定サブレベル内の全Actorのバウンズを合算して返す。見つからなければ無効なFBoxを返す */
    bool GetSubLevelBounds(const FString& SubLevelName, FBox& OutBounds) const;

    //----------------------------------------------------------------------//
    // 保存
    //----------------------------------------------------------------------//

    void SaveAllNavMeshes(const FString& StageID);
    bool SaveSingleNavMesh(ARecastNavMesh* NavMesh, const FString& FilePath);

    UFUNCTION()
    void OnNavBuildFinished(ANavigationData* NavData);

    FString PendingSaveStageID;

    //----------------------------------------------------------------------//
    // ロード
    //----------------------------------------------------------------------//

    void LoadAndApplyAllNavMeshes(const FString& StageID, FVector StageOffset);
    bool LoadSingleNavMesh(ARecastNavMesh* NavMesh, const FString& FilePath, FVector StageOffset);

    FTimerHandle RegistrationWaitHandle;
    void OnNavDataRegistrationComplete();
    int32 PendingRegistrationCount = 0;

    //----------------------------------------------------------------------//
    // 可視化
    //----------------------------------------------------------------------//

    /** NavMesh全体を INDEX_NONE で一括取得して可視化を構築する */
    void BuildNavMeshVisualizationForAgent(ARecastNavMesh* NavMesh);

    void ClearNavMeshVisualizationForAgent(FName AgentName);

    /** ObservableRecastNavMesh のデリゲートにバインド */
    void BindTileUpdateDelegate(ARecastNavMesh* NavMesh);
    void UnbindTileUpdateDelegates();

    /** タイル更新コールバック — ダーティフラグを立てるだけ */
    void OnTilesChanged(ARecastNavMesh* NavMesh, const TArray<int32>& ChangedTileIndices);

    UPROPERTY()
    TMap<FName, TObjectPtr<UProceduralMeshComponent>> NavMeshVisComponents;

    TSet<FName> EnabledVisAgents;

    /** バインド済みデリゲートハンドル（Unbind用） */
    TMap<FName, FDelegateHandle> TileUpdateDelegateHandles;

    /** 可視化再構築が必要なエージェント（ダーティフラグ） */
    TSet<FName> DirtyVisAgents;

    /** 可視化更新のスロットル間隔（秒） */
    float VisualizationUpdateInterval = 0.2f;

    /** 前回の可視化更新からの経過時間 */
    float TimeSinceLastVisUpdate = 0.0f;

    /** ビルド完了後に可視化を最終フラッシュするためのタイマー */
    FTimerHandle VisFinalFlushHandle;
    void FlushVisualizationForAllEnabled();

    //----------------------------------------------------------------------//
    // ビルド進捗モニタリング
    //----------------------------------------------------------------------//

    virtual void Tick(float DeltaTime) override;

    /** 進捗モニタ用: エージェントごとの初期タスク数（モニタ開始時にスナップショット） */
    TMap<FName, int32> InitialBuildTaskCounts;

    /** 進捗モニタ用: エージェントごとのPrintString用Key */
    TMap<FName, int32> AgentDebugMsgKeys;

    bool bMonitoringBuildProgress = false;

    /** 初期タスク数が確定するまでの待ちフレーム（RebuildAll直後は0の場合がある） */
    int32 MonitorWarmupFrames = 0;

    int32 NextDebugMsgKey = 100;

    /** ビルド開始時の実時間（FPlatformTime::Seconds） */
    double BuildStartRealTime = 0.0;

    /** エージェントごとの完了時の経過実時間（秒）。完了していなければ含まれない */
    TMap<FName, double> AgentCompletionTimes;

    /** キャンセルされたエージェント（進捗とキャンセル時刻を保持） */
    TSet<FName> CancelledAgents;
    TMap<FName, double> AgentCancelledProgress;
    TMap<FName, double> AgentCancelledTimes;
};