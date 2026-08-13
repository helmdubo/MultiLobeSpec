#pragma once

#include "SceneViewExtension.h"

/**
 * Publishes the MLS runtime diagnostic selector through three reserved bits in
 * the stock per-view PostVolumeUserFlags uniform. This keeps the selector
 * plugin-owned and avoids changing the Engine View uniform ABI.
 */
class FMLSViewExtension final : public FSceneViewExtensionBase
{
public:
	explicit FMLSViewExtension(const FAutoRegister& AutoRegister);

	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;

	void SetDebugView(int32 InDebugView);
	int32 GetDebugView() const;

	static int32 EncodeDebugView(int32 OriginalUserFlags, int32 DebugView);
	static int32 DecodeDebugView(int32 UserFlags);

	static constexpr uint32 DebugViewShift = 28u;
	static constexpr uint32 DebugViewMask = 0x7u << DebugViewShift;

private:
	TAtomic<int32> DebugView { 0 };
};
