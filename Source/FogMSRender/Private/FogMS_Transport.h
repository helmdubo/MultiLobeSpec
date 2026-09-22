#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"

class FViewInfo;
struct FFogMSWorldRequest;
class FFogMSLumenSourceParameters;
class FFogMSWorldSourcesParameters;

// B2: same-frame, six-ordinate cell-average transport. The four atlas slabs are
// total incident / primary incident + connectivity / coefficients / flux metrics.
FRDGTextureRef FogMS_RenderTransport(FRDGBuilder& GraphBuilder, const FViewInfo& View,
	const FFogMSWorldRequest& Request, const FFogMSLumenSourceParameters& Lumen,
	const FFogMSWorldSourcesParameters& Lights, bool bIndirect);
