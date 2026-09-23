#pragma once

#include "CoreMinimal.h"

#include "RHIFwd.h"

class FRHICommandListImmediate;
class UVolumeTexture;

/** Plugin-owned, resident BGRA8 atlas. Producers bind the texture directly (any bindless configuration);
 * overlay consumers read it through its bindless heap descriptor (BindlessAll only).
 * BindlessAll: native D3D12 committed resource (never evicted, hidden heap readers) with a heap index.
 * Otherwise (injection-only): a plain RHI texture, same format/layout/bytes, no heap index (MAX_uint32).
 */
class FFogMSDensityAtlas
{
public:
	struct FUpload
	{
		// X is contiguous; atlas row = Y + Z * SizeY. No channel conversion or gamma transform.
		TArray64<uint8> Bytes;
		int32 SizeX = 0;
		int32 SizeY = 0;
		int32 SizeZ = 0;
		FGuid SourceId;
		uint64 Revision = 0;
		FString TexturePath;
	};
	using FUploadPtr = TSharedPtr<const FUpload, ESPMode::ThreadSafe>;

	FFogMSDensityAtlas();
	~FFogMSDensityAtlas();
	FFogMSDensityAtlas(const FFogMSDensityAtlas&) = delete;
	FFogMSDensityAtlas& operator=(const FFogMSDensityAtlas&) = delete;

	/** Game thread. Caches up to four textures by weak object, SourceId and resource identity.
	 * Returns null with an explicit error for unsupported or unavailable CPU source data.
	 */
	FUploadPtr Prepare(UVolumeTexture* Texture, FString& OutError);

	/** Render thread. Capture the returned Prepare snapshot in the same command as the
	 * corresponding packet upload, then call this BEFORE uploading that packet.
	 * OutTexture (optional): the uploaded atlas, left in SRV state, or null on failure (then OutError is set).
	 * Returns the bindless heap index, or MAX_uint32 when there is none: always on failure, and also for an
	 * uploaded atlas whose SRV has no bindless handle or that has no heap readers (no BindlessAll); no error in
	 * either case: only the packet consumers need the index.
	 * No UObject access, blocking waits or renderer-private dependencies.
	 */
	uint32 EnsureAndGetDescriptor(FRHICommandListImmediate& RHICmdList, const FUploadPtr& Upload, FString& OutError,
		FTextureRHIRef* OutTexture = nullptr);

private:
	struct FGameThreadState;
	struct FRenderThreadState;
	// These two states are accessed exclusively from their named threads.
	TUniquePtr<FGameThreadState> GameThread;
	TUniquePtr<FRenderThreadState> RenderThread;
};
