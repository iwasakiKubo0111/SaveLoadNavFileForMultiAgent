// ObservableRecastNavMesh.h
#pragma once

#include "CoreMinimal.h"
#include "NavMesh/RecastNavMesh.h"
#include "ObservableRecastNavMesh.generated.h"

/**
 * タイル更新時に変更タイルインデックスのリストを通知するデリゲート。
 * @param NavMesh       更新されたNavMesh
 * @param ChangedTileIndices 変更されたタイルのインデックス一覧
 */
DECLARE_MULTICAST_DELEGATE_TwoParams(
    FOnNavMeshTilesChanged,
    ARecastNavMesh* /*NavMesh*/,
    const TArray<int32>& /*ChangedTileIndices*/
);

/**
 * ARecastNavMesh のサブクラス。
 * OnNavMeshTilesUpdated を override し、変更タイルを
 * FOnNavMeshTilesChanged デリゲートで外部に通知する。
 *
 * 使い方:
 *   Project Settings > Navigation System > Supported Agents で
 *   NavDataClass を AObservableRecastNavMesh に変更する。
 */
UCLASS()
class SAVELOADNAVIMESHFILE_API AObservableRecastNavMesh : public ARecastNavMesh
{
    GENERATED_BODY()

public:
    /** タイル更新通知デリゲート（外部からバインドして使用） */
    FOnNavMeshTilesChanged OnTilesChanged;

protected:
    //~ Begin ARecastNavMesh Interface
    virtual void OnNavMeshTilesUpdated(const TArray<FNavTileRef>& ChangedTiles) override;
    //~ End ARecastNavMesh Interface
};