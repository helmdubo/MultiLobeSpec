#include "FogMS_DensityAtlas.h"

#include "DynamicRHI.h"
#include "Engine/Texture.h"
#include "Engine/VolumeTexture.h"
#include "HAL/IConsoleManager.h"
#if PLATFORM_WINDOWS
#include "ID3D12DynamicRHI.h"
#endif
#include "MultiGPU.h"
#include "RHICommandList.h"
#include "RHIShaderPlatform.h"
#include "RenderingThread.h"
#include "ShaderPlatformConfig.h"
#include "Streaming/StreamableRenderResourceState.h"
#include "TextureResource.h"
#include "UObject/WeakObjectPtrTemplates.h"

namespace
{
	constexpr int32 FogMS_MaxDensityDimension = 256;
	constexpr int32 FogMS_MaxAtlasHeight = 16384;
	constexpr int32 FogMS_MaxCachedDensityTextures = 4;

	// Same predicate as FogMS_BoxRuntime.cpp FogMS_IsBindlessAll(): only BindlessAll has overlay consumers, the hidden
	// heap readers that need the native never-evicted allocation and its descriptor. Fixed for the process lifetime.
	bool FogMS_DensityAtlasNeedsHeapReaders()
	{
		return FShaderPlatformConfig::IsValid(GMaxRHIShaderPlatform)
			&& FShaderPlatformConfig::GetBindlessConfiguration(GMaxRHIShaderPlatform) == ERHIBindlessConfiguration::All;
	}

	bool FogMS_AreAtlasDimensionsValid(int64 SizeX, int64 SizeY, int64 SizeZ)
	{
		return SizeX > 0 && SizeX <= FogMS_MaxDensityDimension
			&& SizeY > 0 && SizeY <= FogMS_MaxDensityDimension
			&& SizeZ > 0 && SizeZ <= FogMS_MaxDensityDimension
			&& SizeY * SizeZ <= FogMS_MaxAtlasHeight;
	}

	TAutoConsoleVariable<int32> CVarFogMSDensityAtlasForceGPUCopy(
		TEXT("r.FogMS.DensityAtlas.ForceGPUCopy"), 0,
		TEXT("Build the FogMS density atlas from the Volume Texture's resident GPU mip 0 even when the editor CPU source exists ")
		TEXT("(the cooked-game path; injection-only, i.e. without -BindlessAll). 0 = CPU source when available (default). ")
		TEXT("For A/B checks of the GPU copy in the editor or -game; changing it rebuilds the atlas."),
		ECVF_Default);

	// Renewed on every Prepare call until the render thread has copied mip 0.
	constexpr float FogMS_GpuSourceResidencySeconds = 5.0f;

	// Game thread, GPU-copy uploads only: true once mip 0 was copied, or while the full-resolution mip 0 is resident.
	// Until the copy, every mip of the texture is requested resident (the Box may be too small on screen for the
	// streamer to want mip 0). The atlas does not depend on the texture after the copy, so later streaming is ignored.
	bool FogMS_IsGpuSourceReady(UVolumeTexture& Texture, const FFogMSDensityAtlas::FUpload& Upload, FString& OutError)
	{
		if (Upload.bGpuCopied.load(std::memory_order_acquire)) return true;
		Texture.SetForceMipLevelsToBeResident(FogMS_GpuSourceResidencySeconds);
		const FStreamableRenderResourceState& State = Texture.GetStreamableResourceState();
		if (State.IsValid() && State.AssetLODBias != 0)
		{
			OutError = FString::Printf(TEXT("FogMS density atlas rejects %s: its render resource starts at mip %d (texture group/LOD bias or cooked mip drop), but the atlas needs the full-resolution mip 0."),
				*Upload.TexturePath, static_cast<int32>(State.AssetLODBias));
			return false;
		}
		// An invalid state is a non-streamable resource, which holds all mips.
		if (Texture.HasPendingInitOrStreaming() || (State.IsValid() && State.NumResidentLODs < State.MaxNumLODs))
		{
			OutError = FString::Printf(TEXT("FogMS density atlas is waiting for %s mip 0 to become resident (requested); no uniform-density fallback is used."),
				*Upload.TexturePath);
			return false;
		}
		return true;
	}

	// Render thread, GPU-copy uploads only: the source resource's current RHI texture when its mip 0 is the
	// full-resolution BGRA8 volume of this upload, else null (streamed out or replaced since Prepare: retry).
	FRHITexture* FogMS_GetGpuSourceTexture(const FFogMSDensityAtlas::FUpload& Upload)
	{
		FRHITexture* Texture = Upload.GpuSource ? Upload.GpuSource->GetTexture3DRHI() : nullptr;
		if (!Texture) return nullptr;
		const FRHITextureDesc& Desc = Texture->GetDesc();
		return Desc.Format == PF_B8G8R8A8 && Desc.Extent == FIntPoint(Upload.SizeX, Upload.SizeY) && Desc.Depth == Upload.SizeZ
			? Texture : nullptr;
	}
}

// Implemented in FogMSRender.cpp (the global shader needs that PostConfigInit module). Declared here, not in a
// FogMSRender public header, to keep this change within the reviewed file set. Records a standalone RDG graph on
// RHICmdList: compute copy of mip 0 of Volume (linear PF_B8G8R8A8 3D) into Atlas (PF_B8G8R8A8 2D with UAV,
// X by Y*Z); Atlas ends in SRVMask. Returns false with OutError (nothing recorded) on unsupported inputs.
FOGMSRENDER_API bool FogMS_CopyVolumeToDensityAtlas(FRHICommandListImmediate& RHICmdList, FRHITexture* Volume, FRHITexture* Atlas,
	FString& OutError);

struct FFogMSDensityAtlas::FGameThreadState
{
	struct FEntry
	{
		FGuid SourceId;
		const FTextureResource* Resource = nullptr;
		// Built from the GPU resource (no CPU source, or ForceGPUCopy); keyed on Resource alone.
		bool bGpu = false;
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
#if WITH_EDITOR
	if (Texture->IsCompiling())
	{
		OutError = FString::Printf(TEXT("FogMS density atlas is waiting for %s to finish texture compilation; no uniform-density fallback is used."), *Texture->GetPathName());
		return nullptr;
	}
#endif
	// The editor CPU source stays the primary path (unchanged bytes). Without it (cooked game, source-stripped texture)
	// or with r.FogMS.DensityAtlas.ForceGPUCopy 1, the render thread copies the texture's resident GPU mip 0 instead.
	bool bFromSource = false;
#if WITH_EDITORONLY_DATA
	bFromSource = Texture->Source.IsValid() && CVarFogMSDensityAtlasForceGPUCopy.GetValueOnGameThread() == 0;
#endif
	int64 SizeX = 0;
	int64 SizeY = 0;
	int32 SizeZ = 0;
	FGuid SourceId;
	if (bFromSource)
	{
#if WITH_EDITORONLY_DATA
		if (Texture->Source.GetFormat() != TSF_BGRA8
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
		SizeX = Texture->Source.GetSizeX();
		SizeY = Texture->Source.GetSizeY();
		SizeZ = Texture->Source.GetNumSlices();
		SourceId = Texture->Source.GetId();
#endif
	}
	else
	{
		// Same runtime acceptance as the CPU path: linear and VectorDisplacementmap, i.e. uncompressed PF_B8G8R8A8
		// (checked with the platform data below). Build-time color adjustments are already baked into the platform
		// mips, which are exactly what the Box Volume material samples, so the GPU copy needs no adjustment check.
		if (Texture->SRGB || Texture->CompressionSettings != TC_VectorDisplacementmap)
		{
			OutError = FString::Printf(TEXT("FogMS density atlas rejects %s: require a Volume Texture with sRGB off and VectorDisplacementmap compression (PF_B8G8R8A8); other formats are not converted."), *Texture->GetPathName());
			return nullptr;
		}
		SizeX = Texture->GetSizeX();
		SizeY = Texture->GetSizeY();
		SizeZ = Texture->GetSizeZ();
	}
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
		OutError = bFromSource
			? FString::Printf(TEXT("FogMS density atlas rejects %s: require a ready PF_B8G8R8A8 platform mip 0 with the same dimensions as the CPU source."), *Texture->GetPathName())
			: FString::Printf(TEXT("FogMS density atlas rejects %s: require a render resource and a PF_B8G8R8A8 platform mip 0 with the texture dimensions."), *Texture->GetPathName());
		return nullptr;
	}

	for (auto It = GameThread->Entries.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid()) It.RemoveCurrent();
	}
	const TWeakObjectPtr<UVolumeTexture> Key(Texture);
	const uint64 AccessSerial = ++GameThread->AccessSerial;
	if (FGameThreadState::FEntry* Existing = GameThread->Entries.Find(Key))
	{
		Existing->LastUse = AccessSerial;
		// CPU path: source id + resource. GPU path: the render resource (a new one after UpdateResource/recreation).
		if (Existing->bGpu == !bFromSource && Existing->SourceId == SourceId && Existing->Resource == Resource)
		{
			if (Existing->Upload.IsValid() && Existing->Upload->GpuSource && !FogMS_IsGpuSourceReady(*Texture, *Existing->Upload, OutError))
				return nullptr;
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
	Entry.bGpu = !bFromSource;
	Entry.LastUse = AccessSerial;
	TSharedRef<FUpload, ESPMode::ThreadSafe> NewUpload = MakeShared<FUpload, ESPMode::ThreadSafe>();
	NewUpload->SizeX = static_cast<int32>(SizeX);
	NewUpload->SizeY = static_cast<int32>(SizeY);
	NewUpload->SizeZ = SizeZ;
	NewUpload->SourceId = SourceId;
	NewUpload->TexturePath = Texture->GetPathName();
	if (bFromSource)
	{
#if WITH_EDITORONLY_DATA
		const int64 ExpectedBytes = SizeX * SizeY * SizeZ * 4;
		if (!Texture->Source.GetMipData(NewUpload->Bytes, 0) || NewUpload->Bytes.Num() != ExpectedBytes)
		{
			Entry.Error = FString::Printf(TEXT("FogMS density atlas cannot read %s BGRA8 source mip 0: expected %lld bytes, received %lld. Reimport/update the texture to retry."),
				*NewUpload->TexturePath, ExpectedBytes, NewUpload->Bytes.Num());
			OutError = Entry.Error;
			return nullptr;
		}
		// Source voxels are already [Z][Y][X] BGRA8; X by (Y*Z) needs no repacking.
#endif
	}
	else
	{
		// No bytes: the render thread copies mip 0 of this resource's current RHI texture (same layout, same bytes).
		NewUpload->GpuSource = Resource;
	}
	NewUpload->Revision = ++GameThread->Revision;
	Entry.Upload = NewUpload;
	if (NewUpload->GpuSource && !FogMS_IsGpuSourceReady(*Texture, *NewUpload, OutError)) return nullptr;
	return Entry.Upload;
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
	const bool bGpuCopy = Upload.IsValid() && Upload->GpuSource != nullptr;
	if (!Upload.IsValid() || Upload->Revision == 0
		|| !FogMS_AreAtlasDimensionsValid(Upload->SizeX, Upload->SizeY, Upload->SizeZ)
		|| (bGpuCopy ? !Upload->Bytes.IsEmpty()
			: Upload->Bytes.Num() != static_cast<int64>(Upload->SizeX) * Upload->SizeY * Upload->SizeZ * 4))
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
	// GPU copy (no CPU source): the resource may have streamed mip 0 out, or swapped its RHI texture, since Prepare.
	// Report "waiting" without caching this revision, so the next call retries (fail closed meanwhile).
	FRHITexture* GpuVolume = nullptr;
	if (bGpuCopy && !FogMS_DensityAtlasNeedsHeapReaders())
	{
		GpuVolume = FogMS_GetGpuSourceTexture(*Upload);
		if (!GpuVolume)
		{
			OutError = FString::Printf(TEXT("FogMS density atlas is waiting for %s: its GPU texture is not the full-resolution PF_B8G8R8A8 mip 0 yet."), *Upload->TexturePath);
			return MAX_uint32;
		}
	}

	TUniquePtr<FRenderThreadState::FAtlas> Atlas = MakeUnique<FRenderThreadState::FAtlas>();
	Atlas->Upload = Upload;
	if (bGpuCopy && FogMS_DensityAtlasNeedsHeapReaders())
	{
		// The native BindlessAll atlas (hidden heap readers) keeps the CPU upload: it is never a UAV target.
		Atlas->Error = FString::Printf(TEXT("FogMS density atlas for %s has no editor CPU source (cooked, or ForceGPUCopy): the GPU copy is injection-only; -BindlessAll overlay consumers require the editor source. Textured density remains disabled."), *Upload->TexturePath);
	}
	else if (!FogMS_DensityAtlasNeedsHeapReaders())
	{
		// Injection-only (no BindlessAll): the only reader is producer pass 0, which binds this texture as an ordinary
		// SRV (FFogMSWorldRequest::DensityAtlas), so the RHI tracks its residency. A plain RHI texture suffices: same
		// BGRA8 format, X by Y+Z*SizeY layout and bytes as the native path below; left in SRV state. No SRV object or
		// heap index is created (nothing reads the heap without BindlessAll), so DescriptorIndex stays MAX_uint32.
		// GPU copy: the same texture plus UAV, written by one compute pass instead of UpdateTexture2D.
		Atlas->Texture = RHICmdList.CreateTexture(FRHITextureCreateDesc::Create2D(TEXT("FogMS.DensityAtlas"),
			FIntPoint(Upload->SizeX, Upload->SizeY * Upload->SizeZ), PF_B8G8R8A8)
			.SetFlags(ETextureCreateFlags::ShaderResource | (GpuVolume ? ETextureCreateFlags::UAV : ETextureCreateFlags::None))
			.SetInitialState(ERHIAccess::SRVMask));
		if (!Atlas->Texture.IsValid())
		{
			Atlas->Error = FString::Printf(TEXT("FogMS density atlas could not create a BGRA8 texture for %s; textured density remains disabled."), *Upload->TexturePath);
		}
		else if (GpuVolume)
		{
			FString CopyError;
			if (FogMS_CopyVolumeToDensityAtlas(RHICmdList, GpuVolume, Atlas->Texture, CopyError))
			{
				Atlas->bUploaded = true;
				Upload->bGpuCopied.store(true, std::memory_order_release);
			}
			else
			{
				Atlas->Error = FString::Printf(TEXT("%s (%s); textured density remains disabled."), *CopyError, *Upload->TexturePath);
			}
		}
		else
		{
			RHICmdList.UpdateTexture2D(Atlas->Texture, 0,
				FUpdateTextureRegion2D(0, 0, 0, 0, Upload->SizeX, Upload->SizeY * Upload->SizeZ),
				Upload->SizeX * 4, Upload->Bytes.GetData());
			Atlas->bUploaded = true;
		}
	}
	else
	{
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
	}
	OutError = Atlas->Error;
	if (OutTexture && Atlas->bUploaded) *OutTexture = Atlas->Texture;
	const uint32 DescriptorIndex = Atlas->DescriptorIndex;
	// Cache failures as well: repeated frames report the same error without reallocating.
	RenderThread->Atlases.Add(Upload->Revision, MoveTemp(Atlas));
	return DescriptorIndex;
}
