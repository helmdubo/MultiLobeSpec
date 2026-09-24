#pragma once

#include "CoreMinimal.h"

/** One persistent bindless SRV; only its index is compiled, never the actor transform. */
class FFogMSBoxRuntime
{
public:
	static int32 GetMode();
	static bool Prepare(uint32& OutDescriptorIndex, FString& OutError);
	/** bA1cSettings (enable only): also set the A1c-only TLV settings (spatial filter 0, temporal jitter 0); false for a Transport Box. */
	static void ConfigureIndirectPreview(bool bEnable, bool bA1cSettings = true);
	static bool IsIndirectPreviewEnabled();
	static void Shutdown();
};
