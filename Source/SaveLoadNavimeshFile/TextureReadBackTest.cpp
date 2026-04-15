// Fill out your copyright notice in the Description page of Project Settings.

#include "TextureReadBackTest.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Engine/Engine.h"  // GEngine

// ------------------------------------------------------------
// グローバルカウンタ定義
// 自前でEnqueueしたレンダーコマンドの未消化数を追跡する
// ------------------------------------------------------------
std::atomic<int32> GHogeRenderCommandsInFlight{ 0 };

// ------------------------------------------------------------
// コンストラクタ
// ------------------------------------------------------------
ATextureReadBackTest::ATextureReadBackTest()
{
	PrimaryActorTick.bCanEverTick = true;
}

// ------------------------------------------------------------
// BeginPlay
// ------------------------------------------------------------
void ATextureReadBackTest::BeginPlay()
{
	Super::BeginPlay();
}

// ------------------------------------------------------------
// Tick — FIFO順で完了チェック＋デバッグ表示
// ------------------------------------------------------------
void ATextureReadBackTest::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// --- FIFO順に先頭から処理 ---
	// 先頭が完了していなければ後続も処理しない（順番保証）
	while (PendingQueue.Num() > 0 && PendingQueue[0].Readback->IsReady())
	{
		FReadbackEntry Entry = PendingQueue[0];
		PendingQueue.RemoveAt(0);

		// GC防止リストからも除去
		ActiveRenderTargets.Remove(Entry.RenderTarget);

		// 保存処理
		OnReadbackComplete(Entry);
	}

	// --- 画面デバッグ表示 ---
	if (bShowDebugStats && GEngine)
	{
		const int32 CmdsInFlight = GHogeRenderCommandsInFlight.load();
		const int32 QueueSize = PendingQueue.Num();

		GEngine->AddOnScreenDebugMessage(
			-1, 0.f, FColor::Yellow,
			FString::Printf(TEXT("[Readback] RenderCmds InFlight: %d | Queue: %d / %d"),
				CmdsInFlight, QueueSize, MaxInflightReadbacks));
	}
}

// ------------------------------------------------------------
// CreateAsyncTexture — 非同期Readbackをキューに積む
// ------------------------------------------------------------
void ATextureReadBackTest::CreateAsyncTexture()
{
	// インフライト制限 — キューが一杯ならスキップ
	if (PendingQueue.Num() >= MaxInflightReadbacks)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Readback] Queue full (%d), skipping this frame"), MaxInflightReadbacks);
		return;
	}

	// RenderTarget 作成
	UTextureRenderTarget2D* TextureTarget =
		UKismetRenderingLibrary::CreateRenderTarget2D(
			this,
			512 * 4,
			512 * 4,
			RTF_RGBA8,
			FLinearColor::White,
			true
		);

	if (!TextureTarget)
	{
		UE_LOG(LogTemp, Error, TEXT("[Readback] Failed to create RenderTarget"));
		return;
	}

	// GC から守る
	ActiveRenderTargets.Add(TextureTarget);

	// マテリアルを描画
	UKismetRenderingLibrary::DrawMaterialToRenderTarget(
		GetWorld(),
		TextureTarget,
		SnapShotMaterial
	);

	// リソース取得
	FTextureRenderTargetResource* TextureResource =
		TextureTarget->GameThread_GetRenderTargetResource();

	if (!TextureResource)
	{
		UE_LOG(LogTemp, Error, TEXT("[Readback] RenderTargetResource is null"));
		ActiveRenderTargets.Remove(TextureTarget);
		return;
	}

	// Readback リクエスト発行
	TSharedPtr<FHogeAsyncReadback> AsyncReadback =
		MakeShared<FHogeAsyncReadback>(TEXT("Hoge"));

	AsyncReadback->Request(TextureResource);

	// キューに追加
	FReadbackEntry Entry;
	Entry.Readback = AsyncReadback;
	Entry.RenderTarget = TextureTarget;
	PendingQueue.Add(MoveTemp(Entry));
}

// ------------------------------------------------------------
// Readback完了後の処理（必要に応じて差し替えてください）
// ------------------------------------------------------------
void ATextureReadBackTest::OnReadbackComplete(FReadbackEntry& Entry)
{
	// --- ここでピクセルデータを読み取って保存する ---
	// 例:
	// FRHIGPUTextureReadback& Rb = Entry.Readback->GetReadback();
	// int32 RowPitchInPixels = 0;
	// const FColor* Data = static_cast<const FColor*>(Rb.Lock(RowPitchInPixels));
	// if (Data)
	// {
	//     // Data からピクセルを読み取り、TArray<FColor> にコピーして画像保存
	//     // 保存処理が重い場合は AsyncTask で別スレッドに飛ばす
	//     Rb.Unlock();
	// }

	UE_LOG(LogTemp, Log, TEXT("[Readback] Complete — remaining queue: %d"), PendingQueue.Num());
}

// ------------------------------------------------------------
// FHogeAsyncReadback::Request
// ------------------------------------------------------------
void FHogeAsyncReadback::Request(FTextureRenderTargetResource* RTResource)
{
	if (!RTResource) return;

	// カウンタ加算
	GHogeRenderCommandsInFlight.fetch_add(1);

	ENQUEUE_RENDER_COMMAND(ReadbackCommand)(
		[this, RTResource](FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture2D* RHITexture = RTResource->GetRenderTargetTexture();
			if (RHITexture)
			{
				Readback.EnqueueCopy(RHICmdList, RHITexture);
			}

			// カウンタ減算（コマンド消化済み）
			GHogeRenderCommandsInFlight.fetch_sub(1);
		}
		);
}

// ------------------------------------------------------------
// FHogeAsyncReadback::IsReady
// ------------------------------------------------------------
bool FHogeAsyncReadback::IsReady()
{
	return Readback.IsReady();
}