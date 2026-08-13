#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

MULTILOBESPEC_API DECLARE_LOG_CATEGORY_EXTERN(LogMultiLobeSpec, Log, All);

class FMLSViewExtension;

/**
 * MultiLobeSpec — dual-lobe GGX and material micro-visibility for UE 5.7 legacy deferred shading.
 *
 * Mechanism (ShaderShift-style, non-destructive):
 *   1. Copy <Engine>/Shaders into <Project>/Saved/MultiLobeSpec/Shaders (overlay).
 *   2. Patch exact UE 5.7 call sites for per-lobe direct BRDF/micro-shadow,
 *      raw MaterialAO transport, indirect diffuse visibility and paired
 *      capture/skylight radiance x cone-aware response.
 *   3. Re-point the "/Engine" virtual shader directory mapping at the overlay.
 *   4. FlushShaderFileCache() + "RecompileShaders Changed" -> shaders rebuild
 *      on the fly. Each preset hashes differently, so once compiled it lives
 *      in the DDC and switching between presets becomes fast.
 *
 * Fail-safe: requested features have exact anchor counts; any mismatch aborts
 * the new immutable overlay before remap and leaves the previous mapping intact.
 */
class MULTILOBESPEC_API FMultiLobeSpecModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FMultiLobeSpecModule& Get()
	{
		return FModuleManager::GetModuleChecked<FMultiLobeSpecModule>("MultiLobeSpec");
	}

	/** Build/refresh overlay from current settings, remap, flush, recompile. */
	void ApplyAndRecompile();

	/** Decide from settings whether anything (BRDF preset OR tonemapper) needs the overlay, and apply/disable accordingly. */
	void ApplyFromSettings();

	/** Restore vanilla engine shader mapping, flush, recompile. */
	void DisableAndRecompile();

	/** Print the fail-closed capability manifest for the active/selected overlay. */
	void LogCapabilities() const;

	/** One-shot health report: overlay state, /Engine mapping, effective config, CVar contract, patch markers. */
	void LogStatus() const;

	/** Change the plugin-owned per-view debug selector without rebuilding shaders. */
	bool SetRuntimeDebugView(int32 DebugView, bool bPersistSetting = true);

	bool IsOverlayActive() const { return bOverlayActive; }

private:
	bool ApplyInternal(FString& OutError);
	bool RemapEngineShaders(const FString& NewDir);
	void TriggerRecompile();

	FString OriginalEngineShaderDir;
	/** Value of the "/Engine" mapping right before our first remap (may belong to another tool). */
	FString PreviousEngineMapping;
	/** Immutable content-addressed overlay currently mapped by this module. */
	FString ActiveOverlayDir;
	bool bOverlayActive = false;
	TSharedPtr<FMLSViewExtension, ESPMode::ThreadSafe> RuntimeViewExtension;

};
