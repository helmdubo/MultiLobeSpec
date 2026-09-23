#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"
#include "RenderGraphFwd.h"

class FSceneView;
class FFogMSShadowCacheState;

namespace FogMSRender
{
	constexpr uint32 BoxRowCount = 32; // Box packet float4 rows; rows 24..31 are reserved for authored density (zero until used).
	using FShadowCacheStatePtr = TSharedPtr<FFogMSShadowCacheState, ESPMode::ThreadSafe>;

	struct FShadowCacheResult
	{
		FTextureRHIRef Texture;
		FRDGTextureRef GraphTexture = nullptr;
		uint32 DescriptorIndex = MAX_uint32;
		FVector3f AxisX = FVector3f::ZeroVector;
		FVector3f AxisY = FVector3f::ZeroVector;
		FVector3f AxisZ = FVector3f::ZeroVector; // Toward the sun; boundary zero is +HalfExtent.Z.
		FVector3f HalfExtent = FVector3f::ZeroVector;
		FIntVector GridSize = FIntVector(128, 128, 64); // XY texels; Z intervals, with 65 stored boundaries.
		FString Error;
	};

	/** One owner per world/overlay. Keep alive through all consumers; unregister the view
	 * extension and drain rendering before release. Its bindless allocation is never resized.
	 */
	FOGMSRENDER_API FShadowCacheStatePtr CreateShadowCacheState();

	/** Render thread, inside the current RDG graph, before deferred-light consumers.
	 * BoxRows is an immutable snapshot with a ready resident authored-density atlas in row 7.z.
	 * DirectionToSun is the matching game-thread light snapshot, not a UObject read on this thread.
	 * FilterSigmaCm is a world-space Gaussian standard deviation; zero preserves the raw field.
	 * Adds prefix, optional XY filter, copy and external-SRV transition in this graph. On success
	 * the caller may add its ordered metadata/packet upload. A false result must disable lookup.
	 * Atlas address: (x, y + z * GridSize.Y); z=0 faces the sun, z=GridSize.Z is the far boundary.
	 * Exact density/light/filter matches reuse the resident result without compute or copy passes.
	 * Pass the prepared density atlas revision to permit reuse across graphs/frames; revision zero
	 * permits only same-graph/frame reuse, since a recycled descriptor alone is not texture identity.
	 * The caller owns per-world/family scheduling and executes its RDG graphs in render order.
	 */
	FOGMSRENDER_API bool BuildShadow(FRDGBuilder& GraphBuilder, const FSceneView& View,
		const FShadowCacheStatePtr& State, const FVector4f (&BoxRows)[BoxRowCount],
		FVector3f DirectionToSun, float FilterSigmaCm, FShadowCacheResult& OutResult, uint64 DensityAtlasRevision = 0);
}
