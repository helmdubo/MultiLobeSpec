#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"

/** Value-only weather snapshot. RHI references are resolved on the render thread by the
 * game-thread producer's enqueued command; no UObject/FTexture is retained here.
 * Domain/wind/layers match the weather material. Inactive weather is exact identity.
 */
struct FFogMSWeatherLighting
{
	bool bActive = false;
	FVector3d Origin = FVector3d::ZeroVector;
	FVector3d PlanetCenter = FVector3d(0, 0, -636000000.0);
	double PlanetRadius = 636000000.0;
	FVector4f Domain = FVector4f(0, 0, 0, 0);
	FVector4f Wind = FVector4f(0, 0, 0, 0);
	FVector4f L0 = FVector4f(0, 0, 0, 0);
	FVector4f L1 = FVector4f(0, 0, 0, 0);
	FVector3f DirectionToSun = FVector3f::ZeroVector;
	uint64 Revision = 0;
	FTextureRHIRef Map, SunMap, TypeLUT, Pattern, Curl;

	bool Equals(const FFogMSWeatherLighting& Other) const
	{
		return bActive == Other.bActive && Origin == Other.Origin && PlanetCenter == Other.PlanetCenter
			&& PlanetRadius == Other.PlanetRadius && Domain == Other.Domain && Wind == Other.Wind
			&& L0 == Other.L0 && L1 == Other.L1 && DirectionToSun == Other.DirectionToSun && Revision == Other.Revision
			&& Map == Other.Map && SunMap == Other.SunMap && TypeLUT == Other.TypeLUT && Pattern == Other.Pattern && Curl == Other.Curl;
	}
};
