#pragma once

#include "CoreMinimal.h"

class FRDGBuilder;
class FSceneView;
struct FPostProcessingInputs;

/** Optional current-frame HDR screen scattering, before DOF/post processing.
 * The caller owns active FogMS Box/view eligibility. Native FSSS is not enabled.
 * Filters the full HDR scene including volumetric light, blending by current
 * fog coverage. Screen-space approximation; not a new transport/MS solver.
 * Returns true if passes were added. Off/Amount=0/missing native fog is a no-op.
 */
FOGMSRENDER_API bool FogMS_AddScreenScattering(
	FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs);
