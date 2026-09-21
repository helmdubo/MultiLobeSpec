#pragma once

#include "CoreMinimal.h"

/** One persistent bindless SRV; only its index is compiled, never the actor transform. */
class FFogMSBoxRuntime
{
public:
	static int32 GetMode();
	static bool Prepare(uint32& OutDescriptorIndex, FString& OutError);
	static void ConfigureIndirectPreview(bool bEnable);
	static bool IsIndirectPreviewEnabled();
	static void Shutdown();
};
