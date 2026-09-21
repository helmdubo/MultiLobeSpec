#pragma once

#include "CoreMinimal.h"

/** A1 only. Independent of MLS BRDF/tonemap policies; all lengths are centimetres. */
struct FFogMSConfig
{
	bool bEnabled = false;
	int32 Steps = 16;
	float MarchDistance = 0.0f; // 0 = current fog grid far depth
	float MaxDistance = 2000000.0f;
	bool bExcludeGlobalLayer = false;
	bool bDebugViews = true;
	float GlobalExtinctionScale = 1.0f; // Snapshot of the component at Apply.
	int32 BoxMode = 0; // 0 global A1; 1 live Box. Transform is never compiled.
	bool bIndirectPreview = false; // Session-only, explicitly enabled by the actor button.
	uint32 BoxDescriptorIndex = 0;
	FString Error;
};

/** Only writes the existing MLS overlay. Never creates a second /Engine mapping. */
class FFogMSShaderPatcher
{
public:
	static FFogMSConfig ReadConfig();
	static FString GetIdentity(const FFogMSConfig& Config);
	static bool PatchOverlay(const FString& OverlayDir, const FFogMSConfig& Config, FString& OutError);
};
