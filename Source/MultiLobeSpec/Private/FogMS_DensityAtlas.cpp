#include "FogMS_DensityAtlas.h"

#include "DynamicRHI.h"
#include "Engine/Texture.h"
#include "Engine/VolumeTexture.h"
#if PLATFORM_WINDOWS
#include "ID3D12DynamicRHI.h"
#endif
#include "MultiGPU.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "UObject/WeakObjectPtrTemplates.h"

namespace
{
	constexpr int32 FogMS_MaxDensityDimension = 256;
	constexpr int32 FogMS_MaxAtlasHeight = 16384;
	constexpr int32 FogMS_MaxCachedDensityTextures = 4;

	bool FogMS_AreAtlasDimensionsValid(int64 SizeX, int64 SizeY, int64 SizeZ)
	{
		return SizeX > 0 && SizeX <= FogMS_MaxDensityDimension
			&& SizeY > 0 && SizeY <= FogMS_MaxDensityDimension
			&& SizeZ > 0 && SizeZ <= FogMS_MaxDensityDimension
			&& SizeY * SizeZ <= FogMS_MaxAtlasHeight;
	}
}

struct FFogMSDensityAtlas::FGameThreadState
{
	struct FEntry
	{
		FGuid SourceId;
		const FTextureResource* Resource = nullptr;
		FUploadPtr Upload;
		FString Error;
		uint64 LastUse = 0;
	};
	TMap<TWeakObjectPtr<UVolumeTexture>, FEntry> Entries;
	uint64 AccessSerial = 0;
	uint64 Revision = 0;
};

struct FFogMSDensityAtlas::FRenderThreadState
{
	struct FAtlas
	{
		TWeakPtr<const FUpload, ESPMode::ThreadSafe> Upload;
#if PLATFORM_WINDOWS
		// The RHI external wrapper also holds the native allocation through deferred deletion.
		TRefCountPtr<ID3D12Resource> NativeTexture;
#endif
		FTextureRHIRef Texture;
		FShaderResourceViewRHIRef SRV;
		uint32 DescriptorIndex = MAX_uint32;
		// Texture holds the uploaded bytes (what producers bind); independent of DescriptorIndex.
		bool bUploaded = false;
		FString Error;
	};
	struct FRetired
	{
		TUniquePtr<FAtlas> Atlas;
		FGPUFenceRHIRef Fence;
	};
	TMap<uint64, TUniquePtr<FAtlas>> Atlases;
	TArray<FRetired> Retired;

	void Collect(FRHICommandListImmediate& RHICmdList)
	{
		for (int32 Index = Retired.Num() - 1; Index >= 0; --Index)
		{
			const FGPUFenceRHIRef& Fence = Retired[Index].Fence;
			if (Fence.IsValid() && Fence->NumPendingWriteCommands.GetValue() == 0 && Fence->Poll())
			{
				Retired.RemoveAtSwap(Index, 1, EAllowShrinking::No);
			}
		}
		for (auto It = Atlases.CreateIterator(); It; ++It)
		{
			if (!It.Value()->Upload.IsValid())
			{
				// GT eviction cannot invalidate a snapshot captured by an enqueued command.
				// Once no snapshot survives, retire after all earlier graphics-queue work.
				if (It.Value()->Texture.IsValid())
				{
					FRetired Item;
					Item.Atlas = MoveTemp(It.Value());
					Item.Fence = RHICreateGPUFence(TEXT("FogMS_DensityAtlasRetired"));
					if (Item.Fence.IsValid()) RHICmdList.WriteGPUFence(Item.Fence);
					// If fence creation fails, retain until owner shutdown rather than guess lifetime.
					Retired.Add(MoveTemp(Item));
				}
				It.RemoveCurrent();
			}
		}
	}
};

FFogMSDensityAtlas::FFogMSDensityAtlas()
	: GameThread(MakeUnique<FGameThreadState>()), RenderThread(MakeUnique<FRenderThreadState>())
{
}

// The Box GPU owner must unregister its view extension and drain enqueued render commands
// before destruction, as it already does for its packet. RHI references delete deferred.
FFogMSDensityAtlas::~FFogMSDensityAtlas() = default;

FFogMSDensityAtlas::FUploadPtr FFogMSDensityAtlas::Prepare(UVolumeTexture* Texture, FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	if (!IsValid(Texture))
	{
		OutError = TEXT("FogMS density atlas requires a valid Volume Texture; the textured density path is disabled.");
		return nullptr;
	}
#if WITH_EDITORONLY_DATA
#if WITH_EDITOR
	if (Texture->IsCompiling())
	{
		OutError = FString::Printf(TEXT("FogMS density atlas is waiting for %s to finish texture compilation; no uniform-density fallback is used."), *Texture->GetPathName());
		return nullptr;
	}
#endif
	if (!Texture->Source.IsValid() || Texture->Source.GetFormat() != TSF_BGRA8
		|| Texture->Source.GetNumBlocks() != 1 || Texture->Source.GetNumLayers() != 1
		|| Texture->Source.GetNumMips() < 1 || !Texture->Source.IsVolume()
		|| Texture->SRGB || Texture->CompressionSettings != TC_VectorDisplacementmap)
	{
		OutError = FString::Printf(TEXT("FogMS density atlas rejects %s: require a single BGRA8 Volume Texture source, sRGB off and VectorDisplacementmap compression."), *Texture->GetPathName());
		return nullptr;
	}
	// The atlas copies raw source mip 0. Build-time color adjustments would make it
	// disagree with the actor's existing Volume Texture material even with BGRA8 output.
	if (Texture->AdjustBrightness != 1.0f || Texture->AdjustBrightnessCurve != 1.0f
		|| Texture->AdjustSaturation != 1.0f || Texture->AdjustVibrance != 0.0f
		|| Texture->AdjustRGBCurve != 1.0f || Texture->AdjustHue != 0.0f
		|| Texture->AdjustMinAlpha != 0.0f || Texture->AdjustMaxAlpha != 1.0f
		|| Texture->bFlipGreenChannel)
	{
		OutError = FString::Printf(TEXT("FogMS density atlas rejects %s: reset texture color/alpha adjustments and Flip Green Channel so source mip 0 matches the volume material."), *Texture->GetPathName());
		return nullptr;
	}
	const int64 SizeX = Texture->Source.GetSizeX();
	const int64 SizeY = Texture->Source.GetSizeY();
	const int32 SizeZ = Texture->Source.GetNumSlices();
	if (!FogMS_AreAtlasDimensionsValid(SizeX, SizeY, SizeZ))
	{
		OutError = FString::Printf(TEXT("FogMS density atlas rejects %s (%lld x %lld x %d): each axis must be 1..256 and SizeY*SizeZ must be <=16384."), *Texture->GetPathName(), SizeX, SizeY, SizeZ);
		return nullptr;
	}
	const FTexturePlatformData* PlatformData = Texture->GetPlatformData();
	const FTextureResource* Resource = Texture->GetResource();
	if (!PlatformData || !Resource || PlatformData->PixelFormat != PF_B8G8R8A8
		|| PlatformData->SizeX != SizeX || PlatformData->SizeY != SizeY
		|| PlatformData->GetNumSlices() != SizeZ || PlatformData->Mips.IsEmpty()
		|| PlatformData->Mips[0].SizeX != SizeX || PlatformData->Mips[0].SizeY != SizeY
		|| PlatformData->Mips[0].SizeZ != SizeZ)
	{
		OutError = FString::Printf(TEXT("FogMS density atlas rejects %s: require a ready PF_B8G8R8A8 platform mip 0 with the same dimensions as the CPU source."), *Texture->GetPathName());
		return nullptr;
	}

	for (auto It = GameThread->Entries.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid()) It.RemoveCurrent();
	}
	const TWeakObjectPtr<UVolumeTexture> Key(Texture);
	const FGuid SourceId = Texture->Source.GetId();
	const uint64 AccessSerial = ++GameThread->AccessSerial;
	if (FGameThreadState::FEntry* Existing = GameThread->Entries.Find(Key))
	{
		Existing->LastUse = AccessSerial;
		if (Existing->SourceId == SourceId && Existing->Resource == Resource)
		{
			OutError = Existing->Error;
			return Existing->Upload;
		}
	}
	else if (GameThread->Entries.Num() >= FogMS_MaxCachedDensityTextures)
	{
		TWeakObjectPtr<UVolumeTexture> OldestKey;
		uint64 OldestUse = MAX_uint64;
		for (const auto& Pair : GameThread->Entries)
		{
			if (Pair.Value.LastUse < OldestUse)
			{
				OldestUse = Pair.Value.LastUse;
				OldestKey = Pair.Key;
			}
		}
		GameThread->Entries.Remove(OldestKey);
	}

	FGameThreadState::FEntry& Entry = GameThread->Entries.FindOrAdd(Key);
	Entry = FGameThreadState::FEntry{};
	Entry.SourceId = SourceId;
	Entry.Resource = Resource;
	Entry.LastUse = AccessSerial;
	TSharedRef<FUpload, ESPMode::ThreadSafe> NewUpload = MakeShared<FUpload, ESPMode::ThreadSafe>();
	NewUpload->SizeX = static_cast<int32>(SizeX);
	NewUpload->SizeY = static_cast<int32>(SizeY);
	NewUpload->SizeZ = SizeZ;
	NewUpload->SourceId = SourceId;
	NewUpload->TexturePath = Texture->GetPathName();
	const int64 ExpectedBytes = SizeX * SizeY * SizeZ * 4;
	if (!Texture->Source.GetMipData(NewUpload->Bytes, 0) || NewUpload->Bytes.Num() != ExpectedBytes)
	{
		Entry.Error = FString::Printf(TEXT("FogMS density atlas cannot read %s BGRA8 source mip 0: expected %lld bytes, received %lld. Reimport/update the texture to retry."),
			*NewUpload->TexturePath, ExpectedBytes, NewUpload->Bytes.Num());
		OutError = Entry.Error;
		return nullptr;
	}
	// Source voxels are already [Z][Y][X] BGRA8; X by (Y*Z) needs no repacking.
	NewUpload->Revision = ++GameThread->Revision;
	Entry.Upload = NewUpload;
	return Entry.Upload;
#else
	OutError = TEXT("FogMS density atlas requires editor CPU TextureSource mip 0; cooked/source-stripped Volume Textures are unsupported in A1c.");
	return nullptr;
#endif
}

uint32 FFogMSDensityAtlas::EnsureAndGetDescriptor(FRHICommandListImmediate& RHICmdList, const FUploadPtr& Upload, FString& OutError,
	FTextureRHIRef* OutTexture)
{
	check(IsInRenderingThread());
	OutError.Reset();
	if (OutTexture) OutTexture->SafeRelease();
	if (!GDynamicRHI || FCString::Strcmp(GDynamicRHI->GetName(), TEXT("D3D12")) != 0 || GNumExplicitGPUsForRendering != 1)
	{
		OutError = TEXT("FogMS density atlas requires single-GPU D3D12.");
		return MAX_uint32;
	}
	RenderThread->Collect(RHICmdList);
	if (!Upload.IsValid() || Upload->Revision == 0
		|| !FogMS_AreAtlasDimensionsValid(Upload->SizeX, Upload->SizeY, Upload->SizeZ)
		|| Upload->Bytes.Num() != static_cast<int64>(Upload->SizeX) * Upload->SizeY * Upload->SizeZ * 4)
	{
		OutError = TEXT("FogMS density atlas has no valid prepared upload; textured density must remain disabled.");
		return MAX_uint32;
	}
	if (const TUniquePtr<FRenderThreadState::FAtlas>* Existing = RenderThread->Atlases.Find(Upload->Revision))
	{
		OutError = (*Existing)->Error;
		if (OutTexture && (*Existing)->bUploaded) *OutTexture = (*Existing)->Texture;
		return (*Existing)->DescriptorIndex;
	}

	TUniquePtr<FRenderThreadState::FAtlas> Atlas = MakeUnique<FRenderThreadState::FAtlas>();
	Atlas->Upload = Upload;
#if PLATFORM_WINDOWS
	ID3D12DynamicRHI* D3D12 = GetID3D12DynamicRHI();
	D3D12_HEAP_PROPERTIES Heap{};
	Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	Heap.CreationNodeMask = Heap.VisibleNodeMask = D3D12->RHIGetDeviceNodeMask(0);
	D3D12_RESOURCE_DESC Desc{};
	Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	Desc.Width = Upload->SizeX;
	Desc.Height = Upload->SizeY * Upload->SizeZ;
	Desc.DepthOrArraySize = Desc.MipLevels = Desc.SampleDesc.Count = 1;
	Desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	// No CREATE_NOT_RESIDENT: native fog passes cannot enumerate this unbound descriptor
	// for residency. The external RHI wrapper opts out of Engine eviction, like Box data.
	const HRESULT Result = D3D12->RHIGetDevice(0)->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		nullptr, IID_PPV_ARGS(Atlas->NativeTexture.GetInitReference()));
	if (FAILED(Result))
	{
		Atlas->Error = FString::Printf(TEXT("FogMS density atlas allocation failed for %s: HRESULT 0x%08x. Reimport/update the texture to retry."), *Upload->TexturePath, static_cast<uint32>(Result));
	}
	else
	{
		Atlas->Texture = D3D12->RHICreateTexture2DFromResource(PF_B8G8R8A8,
			ETextureCreateFlags::ShaderResource | ETextureCreateFlags::External, FClearValueBinding::None, Atlas->NativeTexture);
		if (Atlas->Texture.IsValid())
		{
			Atlas->SRV = RHICmdList.CreateShaderResourceView(Atlas->Texture,
				FRHIViewDesc::CreateTextureSRV().SetDimensionFromTexture(Atlas->Texture));
			if (Atlas->SRV.IsValid())
			{
				// Upload no longer depends on a bindless handle: Transport/World producers bind Texture directly
				// (the injection-only runtime has no BindlessAll). Same bytes, same order as before.
				RHICmdList.UpdateTexture2D(Atlas->Texture, 0,
					FUpdateTextureRegion2D(0, 0, 0, 0, Upload->SizeX, Upload->SizeY * Upload->SizeZ),
					Upload->SizeX * 4, Upload->Bytes.GetData());
				Atlas->bUploaded = true;
				// Heap index only for overlay consumers (row 7.z); present whenever the RHI provides a handle.
				const FRHIDescriptorHandle Handle = Atlas->SRV->GetBindlessHandle();
				if (Handle.IsValid()) Atlas->DescriptorIndex = Handle.GetIndex();
			}
		}
		if (!Atlas->bUploaded)
		{
			Atlas->Error = FString::Printf(TEXT("FogMS density atlas could not create a BGRA8 texture/SRV for %s; textured density remains disabled."), *Upload->TexturePath);
		}
	}
#else
	Atlas->Error = TEXT("FogMS density atlas currently supports Windows D3D12 only.");
#endif
	OutError = Atlas->Error;
	if (OutTexture && Atlas->bUploaded) *OutTexture = Atlas->Texture;
	const uint32 DescriptorIndex = Atlas->DescriptorIndex;
	// Cache failures as well: repeated frames report the same error without reallocating.
	RenderThread->Atlases.Add(Upload->Revision, MoveTemp(Atlas));
	return DescriptorIndex;
}
