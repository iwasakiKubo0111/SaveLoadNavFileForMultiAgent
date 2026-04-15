// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RHIGPUReadback.h"
#include "TextureReadBackTest.generated.h"

// ------------------------------------------------------------
// 非同期Readbackラッパー
// ------------------------------------------------------------
class FHogeAsyncReadback : public TSharedFromThis<FHogeAsyncReadback>
{
public:
	FHogeAsyncReadback(const FString& InName)
		: Readback(FRHIGPUTextureReadback(*InName))
	{
	}

	void Request(FTextureRenderTargetResource* RTResource);
	bool IsReady();

	FRHIGPUTextureReadback& GetReadback() { return Readback; }

private:
	FRHIGPUTextureReadback Readback;
};

// ------------------------------------------------------------
// キューに積むエントリ（Readback + 対応するRenderTarget参照）
// ------------------------------------------------------------
struct FReadbackEntry
{
	TSharedPtr<FHogeAsyncReadback> Readback;

	// RenderTargetの寿命をReadback完了まで保証するための参照
	// (UPROPERTYではないがOwnerのTArrayがUPROPERTYなので参照は保持される)
	UTextureRenderTarget2D* RenderTarget = nullptr;
};

// ------------------------------------------------------------
// レンダーコマンド可視化用カウンタ（グローバル）
// ------------------------------------------------------------
extern std::atomic<int32> GHogeRenderCommandsInFlight;

// ------------------------------------------------------------
// Actor
// ------------------------------------------------------------
UCLASS()
class MYPROJECT_API ATextureReadBackTest : public AActor
{
	GENERATED_BODY()

public:
	ATextureReadBackTest();

protected:
	virtual void BeginPlay() override;

public:
	virtual void Tick(float DeltaTime) override;

	UFUNCTION(BlueprintCallable)
	void CreateAsyncTexture();

	// マテリアル
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Render")
	UMaterialInterface* SnapShotMaterial;

	// 画面デバッグ表示の ON/OFF
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bShowDebugStats = true;

private:
	// インフライト制限（同時に走るReadbackの最大数）
	static constexpr int32 MaxInflightReadbacks = 2;

	// FIFO キュー（先頭から順に処理）
	TArray<FReadbackEntry> PendingQueue;

	// RenderTarget の GC防止用（UPROPERTY で参照保持）
	UPROPERTY()
	TArray<TObjectPtr<UTextureRenderTarget2D>> ActiveRenderTargets;

	// Readback完了後の保存処理（必要に応じて実装を差し替えてください）
	void OnReadbackComplete(FReadbackEntry& Entry);

	//参考
	//https://claude.ai/share/0388401c-8f72-4676-a161-c9ee72dbce35
};