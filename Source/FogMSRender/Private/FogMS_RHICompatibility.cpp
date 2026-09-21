#include "FogMS_RHICompatibility.h"

#include "DynamicRHI.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DelayedAutoRegister.h"
#include "MultiGPU.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "Runtime/Launch/Resources/Version.h"
#include "ShaderPlatformConfig.h"
#if PLATFORM_WINDOWS
#include "ID3D12DynamicRHI.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogFogMSRHICompatibility, Log, All);

void FFogMSRHICompatibility::Startup()
{
#if PLATFORM_WINDOWS && ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8 && ENGINE_PATCH_VERSION == 2
	// PostConfigInit precedes RHI selection. ShaderTypesReady runs after RHIInit
	// and RenderUtilsInit, before global shaders/Slate/the first scene rendering.
	// The module stays loaded for the engine lifetime (it owns global shaders).
	FDelayedAutoRegisterHelper(EDelayedRegisterRunPhase::ShaderTypesReady, [this]() { Apply(); });
	// Keep a late init check and a sink for subsequent console/config changes.
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FFogMSRHICompatibility::Apply);
	ConsoleSinkHandle = IConsoleManager::Get().RegisterConsoleVariableSink_Handle(
		FConsoleCommandDelegate::CreateRaw(this, &FFogMSRHICompatibility::Apply));
	Apply();
#endif
}

void FFogMSRHICompatibility::Apply()
{
#if PLATFORM_WINDOWS && ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8 && ENGINE_PATCH_VERSION == 2
	if (!GIsRHIInitialized || !GDynamicRHI || FCString::Strcmp(GDynamicRHI->GetName(), TEXT("D3D12")) != 0
		|| GNumExplicitGPUsForRendering != 1
		|| GMaxRHIShaderPlatform != SP_PCD3D_SM6
		|| !FShaderPlatformConfig::IsValid(GMaxRHIShaderPlatform)
		|| FShaderPlatformConfig::GetBindlessConfiguration(GMaxRHIShaderPlatform) != ERHIBindlessConfiguration::All)
	{
		return;
	}

	IConsoleVariable* ParallelTranslate = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RHICmd.ParallelTranslate.Enable"));
	checkf(ParallelTranslate, TEXT("UE 5.8.2 requires r.RHICmd.ParallelTranslate.Enable for the FogMS D3D12 compatibility guard."));
	const bool bWasEnabled = ParallelTranslate->GetBool();
	if (bWasEnabled)
	{
		// In UE 5.8.2, a fresh parallel context can dispatch native RT before a
		// bindless draw/compute initializes bLastSetHeapsBindless. RT cleanup then
		// restores a bindful CurrentViewHeap, which is null under BindlessAll.
		// Translate ordinary graphics lists on the persistent default context.
		// RDG parallel recording, ray tracing and lighting shaders stay enabled.
		ParallelTranslate->SetWithCurrentPriority(false, NAME_None, ECVF_SetByConsole, ECVF_SetByCode);
	}
	checkf(!ParallelTranslate->GetBool(), TEXT("FogMS could not enable the UE 5.8.2 D3D12 compatibility guard."));
	if (!bPrimed)
	{
		bPrimed = true;
		ENQUEUE_RENDER_COMMAND(FogMSPrimeDefaultBindlessContext)([](FRHICommandListImmediate& RHICmdList)
		{
			// Immediate lists always translate on the default context. Public RHI
			// state restoration primes its bindless heaps before any first RT dispatch;
			// no shader, draw, resource allocation or private D3D12 state access.
			FRHICommandListScopedPipeline Pipeline(RHICmdList, ERHIPipeline::Graphics);
			RHICmdList.EnqueueLambda([](FRHICommandListBase& ExecutingCmdList)
			{
				ID3D12DynamicRHI* D3D12 = GetID3D12DynamicRHI();
				ID3D12GraphicsCommandList* NativeList = D3D12->RHIGetGraphicsCommandList(ExecutingCmdList, 0);
				D3D12->RHIFinishExternalComputeWork(ExecutingCmdList, 0, NativeList);
				UE_LOG(LogFogMSRHICompatibility, Log, TEXT("Default graphics context bindless heaps primed through the public D3D12 RHI."));
			});
		});
	}
	if (!bReported || bWasEnabled)
	{
		UE_LOG(LogFogMSRHICompatibility, Warning,
			TEXT("UE 5.8.2 D3D12/SM6 BindlessAll compatibility: r.RHICmd.ParallelTranslate.Enable=0 (CPU RHI translation). Native RT descriptor-heap restore can dereference a null heap on a fresh parallel context. Ray tracing and lighting remain enabled; parallel RDG recording is unchanged. The guard lasts for this editor process, including FogMS Off."));
		bReported = true;
	}
#endif
}

void FFogMSRHICompatibility::Shutdown()
{
#if PLATFORM_WINDOWS && ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8 && ENGINE_PATCH_VERSION == 2
	FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
	IConsoleManager::Get().UnregisterConsoleVariableSink_Handle(ConsoleSinkHandle);
	// Do not restore parallel translation while the affected native RHI is alive.
	// No ini is written; a new process selects its normal defaults/config again.
#endif
}
