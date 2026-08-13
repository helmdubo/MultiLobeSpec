#include "MultiLobeSpecViewExtension.h"

#include "SceneView.h"

FMLSViewExtension::FMLSViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

void FMLSViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
	const bool bAllowDebug = !InView.bIsSceneCapture
		&& !InView.bIsReflectionCapture
		&& !InView.bIsPlanarReflection
		&& !InView.bIsVirtualTexture
		&& InView.CustomRenderPass == nullptr;
	const int32 EffectiveDebugView = bAllowDebug ? GetDebugView() : 0;
	InView.FinalPostProcessSettings.UserFlags = EncodeDebugView(
		InView.FinalPostProcessSettings.UserFlags,
		EffectiveDebugView);
}

void FMLSViewExtension::SetDebugView(int32 InDebugView)
{
	DebugView.Store(FMath::Clamp(InDebugView, 0, 5));
}

int32 FMLSViewExtension::GetDebugView() const
{
	return DebugView.Load();
}

int32 FMLSViewExtension::EncodeDebugView(int32 OriginalUserFlags, int32 InDebugView)
{
	const uint32 PreservedFlags = static_cast<uint32>(OriginalUserFlags) & ~DebugViewMask;
	const uint32 EncodedDebugView = static_cast<uint32>(FMath::Clamp(InDebugView, 0, 5)) << DebugViewShift;
	return static_cast<int32>(PreservedFlags | EncodedDebugView);
}

int32 FMLSViewExtension::DecodeDebugView(int32 UserFlags)
{
	return static_cast<int32>((static_cast<uint32>(UserFlags) & DebugViewMask) >> DebugViewShift);
}
