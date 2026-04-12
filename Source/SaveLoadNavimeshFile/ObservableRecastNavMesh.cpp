// ObservableRecastNavMesh.cpp
#include "ObservableRecastNavMesh.h"
#include "Detour/DetourNavMesh.h"   // dtNavMesh, decodePolyIdTile

void AObservableRecastNavMesh::OnNavMeshTilesUpdated(const TArray<FNavTileRef>& ChangedTiles)
{
    // 親クラスの処理を必ず先に実行（パス無効化等）
    Super::OnNavMeshTilesUpdated(ChangedTiles);

    if (!OnTilesChanged.IsBound())
    {
        return;
    }

    // FNavTileRef (uint64) → dtTileRef → TileIndex に変換
    const dtNavMesh* DetourMesh = GetRecastMesh();
    if (!DetourMesh)
    {
        return;
    }

    TArray<int32> ChangedIndices;
    ChangedIndices.Reserve(ChangedTiles.Num());

    for (const FNavTileRef& TileRef : ChangedTiles)
    {
        const dtTileRef DtRef = static_cast<dtTileRef>(static_cast<uint64>(TileRef));
        if (DtRef == 0)
        {
            continue;
        }

        // decodePolyIdTile は dtTileRef からタイルインデックスを抽出する
        // dtNavMesh::getTileByRef でもよいが、インデックスだけ欲しいので decode が軽い
        unsigned int Salt, TileIdx, Poly;
        DetourMesh->decodePolyId(DtRef, Salt, TileIdx, Poly);

        ChangedIndices.AddUnique(static_cast<int32>(TileIdx));
    }

    if (ChangedIndices.Num() > 0)
    {
        OnTilesChanged.Broadcast(this, ChangedIndices);
    }
}