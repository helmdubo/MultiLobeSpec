#pragma once

#include "CoreMinimal.h"

class FRHICommandListImmediate;
class UVolumeTexture;

/** Plugin-owned, resident BGRA8 atlas for the unbound FogMS shader descriptor. */
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
	 * Returns MAX_uint32 on failure; the caller must disable that packet's density path.
	 * No UObject access, blocking waits or renderer-private dependencies.
	 */
	uint32 EnsureAndGetDescriptor(FRHICommandListImmediate& RHICmdList, const FUploadPtr& Upload, FString& OutError);

private:
	struct FGameThreadState;
	struct FRenderThreadState;
	// These two states are accessed exclusively from their named threads.
	TUniquePtr<FGameThreadState> GameThread;
	TUniquePtr<FRenderThreadState> RenderThread;
};
