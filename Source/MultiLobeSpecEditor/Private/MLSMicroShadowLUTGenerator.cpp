#include "MLSMicroShadowLUTGenerator.h"

#include "Async/ParallelFor.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	constexpr double MLSInvUint32 = 1.0 / 4294967296.0;
	constexpr double MLSTwoPi = 6.283185307179586476925286766559;

	FVector3d SafeNormal(const FVector3d& Value, const FVector3d& Fallback)
	{
		const double LengthSquared = Value.SquaredLength();
		return LengthSquared > 1.0e-24 ? Value / FMath::Sqrt(LengthSquared) : Fallback;
	}

	bool IsUnitInput(double Value)
	{
		return FMath::IsFinite(Value) && Value >= 0.0 && Value <= 1.0;
	}

	double AxisNode(int32 Index, int32 Size, EMLSMicroShadowAxisLayout Layout, int32 Axis)
	{
		const double T = Size > 1 ? static_cast<double>(Index) / static_cast<double>(Size - 1) : 0.0;
		if (Layout == EMLSMicroShadowAxisLayout::Linear)
		{
			return T;
		}
		if (Layout == EMLSMicroShadowAxisLayout::BoundaryAware && Axis == 0)
		{
			const double A = T * T;
			const double B = (1.0 - T) * (1.0 - T);
			return A / FMath::Max(A + B, 1.0e-12);
		}

		// Axis 0 = Visibility, 1 = NoL, 2 = NoV.
		return Axis == 1 ? 1.0 - (1.0 - T) * (1.0 - T) : T * T;
	}

	double NoLNode(int32 Index, int32 Size, EMLSMicroShadowAxisLayout Layout, double Visibility)
	{
		if (Layout != EMLSMicroShadowAxisLayout::BoundaryAware)
		{
			return AxisNode(Index, Size, Layout, 1);
		}
		const double T = Size > 1 ? static_cast<double>(Index) / static_cast<double>(Size - 1) : 0.5;
		const double Signed = 2.0 * T - 1.0;
		const double Threshold = FMath::Sqrt(FMath::Max(1.0 - Visibility, 0.0));
		return Signed < 0.0
			? Threshold * (1.0 - Signed * Signed)
			: Threshold + (1.0 - Threshold) * Signed * Signed;
	}

	double RuntimeAxisT(double Value, EMLSMicroShadowAxisLayout Layout, int32 Axis)
	{
		Value = FMath::Clamp(Value, 0.0, 1.0);
		if (Layout == EMLSMicroShadowAxisLayout::Linear)
		{
			return Value;
		}
		if (Layout == EMLSMicroShadowAxisLayout::BoundaryAware && Axis == 0)
		{
			const double A = FMath::Sqrt(Value);
			const double B = FMath::Sqrt(FMath::Max(1.0 - Value, 0.0));
			return A / FMath::Max(A + B, 1.0e-12);
		}
		return Axis == 1
			? 1.0 - FMath::Sqrt(FMath::Max(1.0 - Value, 0.0))
			: FMath::Sqrt(Value);
	}

	double RuntimeNoLAxisT(double NoL, double Visibility, EMLSMicroShadowAxisLayout Layout)
	{
		if (Layout != EMLSMicroShadowAxisLayout::BoundaryAware)
		{
			return RuntimeAxisT(NoL, Layout, 1);
		}
		NoL = FMath::Clamp(NoL, 0.0, 1.0);
		const double Threshold = FMath::Sqrt(FMath::Max(1.0 - FMath::Clamp(Visibility, 0.0, 1.0), 0.0));
		double Signed = 0.0;
		if (NoL < Threshold)
		{
			Signed = -FMath::Sqrt(FMath::Clamp((Threshold - NoL) / FMath::Max(Threshold, 1.0e-12), 0.0, 1.0));
		}
		else
		{
			Signed = FMath::Sqrt(FMath::Clamp((NoL - Threshold) / FMath::Max(1.0 - Threshold, 1.0e-12), 0.0, 1.0));
		}
		return 0.5 + 0.5 * Signed;
	}

	int32 TexelIndex(const FMLSMicroShadowLUTSettings& Settings, int32 VisibilityIndex, int32 NoLIndex, int32 NoVIndex)
	{
		return (NoVIndex * Settings.NoLSize + NoLIndex) * Settings.VisibilitySize + VisibilityIndex;
	}

	float VectorChannel(const FVector4f& Value, int32 Channel)
	{
		switch (Channel)
		{
		case 0: return Value.X;
		case 1: return Value.Y;
		case 2: return Value.Z;
		default: return Value.W;
		}
	}

	uint8 PackedChannel(uint32 Value, int32 Channel)
	{
		return static_cast<uint8>((Value >> (Channel * 8)) & 0xffu);
	}

	template <typename FFetch>
	double SampleLUT(const FMLSMicroShadowLUTData& Data, double Visibility, double Roughness, double NoL, double NoV, FFetch&& Fetch)
	{
		if (Visibility <= 0.0)
		{
			return 0.0;
		}
		if (Visibility >= 1.0)
		{
			return 1.0;
		}
		NoL = FMath::Clamp(NoL, 0.0, 1.0);
		if (2.0 * Visibility + NoL < 1.0)
		{
			return 0.0;
		}

		const FMLSMicroShadowLUTSettings& Settings = Data.Settings;
		const double AxisT[3] =
		{
			RuntimeAxisT(Visibility, Settings.AxisLayout, 0),
			RuntimeNoLAxisT(NoL, Visibility, Settings.AxisLayout),
			RuntimeAxisT(NoV, Settings.AxisLayout, 2)
		};
		const int32 AxisSize[3] = { Settings.VisibilitySize, Settings.NoLSize, Settings.NoVSize };
		int32 Axis0[3];
		int32 Axis1[3];
		double AxisFraction[3];
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double Position = AxisT[Axis] * static_cast<double>(AxisSize[Axis] - 1);
			Axis0[Axis] = FMath::Clamp(FMath::FloorToInt(Position), 0, AxisSize[Axis] - 1);
			Axis1[Axis] = FMath::Min(Axis0[Axis] + 1, AxisSize[Axis] - 1);
			AxisFraction[Axis] = FMath::Clamp(Position - static_cast<double>(Axis0[Axis]), 0.0, 1.0);
		}

		Roughness = FMath::Clamp(Roughness, 0.1, 1.0);
		const int32 RoughnessBank =
			Settings.RoughnessBankCount() == 2 && Roughness > Settings.RoughnessNodes[3] ? 1 : 0;
		const int32 BankNodeOffset = RoughnessBank * 3;

		auto SampleChannel = [&](int32 Channel)
		{
			double Corners[2][2][2];
			for (int32 Z = 0; Z < 2; ++Z)
			{
				for (int32 Y = 0; Y < 2; ++Y)
				{
					for (int32 X = 0; X < 2; ++X)
					{
						Corners[Z][Y][X] = Fetch(RoughnessBank * Settings.NumTexels() + TexelIndex(
							Settings,
							X == 0 ? Axis0[0] : Axis1[0],
							Y == 0 ? Axis0[1] : Axis1[1],
							Z == 0 ? Axis0[2] : Axis1[2]), Channel);
					}
				}
			}

			const double X00 = FMath::Lerp(Corners[0][0][0], Corners[0][0][1], AxisFraction[0]);
			const double X01 = FMath::Lerp(Corners[0][1][0], Corners[0][1][1], AxisFraction[0]);
			const double X10 = FMath::Lerp(Corners[1][0][0], Corners[1][0][1], AxisFraction[0]);
			const double X11 = FMath::Lerp(Corners[1][1][0], Corners[1][1][1], AxisFraction[0]);
			const double Y0 = FMath::Lerp(X00, X01, AxisFraction[1]);
			const double Y1 = FMath::Lerp(X10, X11, AxisFraction[1]);
			return FMath::Lerp(Y0, Y1, AxisFraction[2]);
		};

		int32 Roughness0 = BankNodeOffset;
		int32 Roughness1 = BankNodeOffset;
		for (int32 Channel = 0; Channel + 1 < 4; ++Channel)
		{
			const int32 NodeIndex = BankNodeOffset + Channel;
			if (Roughness >= Settings.RoughnessNodes[NodeIndex])
			{
				Roughness0 = NodeIndex;
				Roughness1 = NodeIndex + 1;
			}
		}
		const int32 BankLastNode = BankNodeOffset + 3;
		if (Roughness >= Settings.RoughnessNodes[BankLastNode])
		{
			Roughness0 = Roughness1 = BankLastNode;
		}

		const double Node0 = Settings.RoughnessNodes[Roughness0];
		const double Node1 = Settings.RoughnessNodes[Roughness1];
		double RoughnessFraction = 0.0;
		if (Roughness1 != Roughness0)
		{
			if (Settings.RoughnessInterpolation == EMLSMicroShadowRoughnessInterpolation::Alpha)
			{
				RoughnessFraction = (Roughness * Roughness - Node0 * Node0) / (Node1 * Node1 - Node0 * Node0);
			}
			else
			{
				RoughnessFraction = (Roughness - Node0) / (Node1 - Node0);
			}
		}

		return FMath::Clamp(FMath::Lerp(
			SampleChannel(Roughness0 - BankNodeOffset),
			SampleChannel(Roughness1 - BankNodeOffset),
			RoughnessFraction), 0.0, 1.0);
	}

	class FMLSSHA256
	{
	public:
		FMLSSHA256()
		{
			State[0] = 0x6a09e667u; State[1] = 0xbb67ae85u;
			State[2] = 0x3c6ef372u; State[3] = 0xa54ff53au;
			State[4] = 0x510e527fu; State[5] = 0x9b05688cu;
			State[6] = 0x1f83d9abu; State[7] = 0x5be0cd19u;
		}

		void Update(const uint8* Data, int64 NumBytes)
		{
			for (int64 Index = 0; Index < NumBytes; ++Index)
			{
				Buffer[BufferLength++] = Data[Index];
				if (BufferLength == 64)
				{
					Transform(Buffer);
					BitCount += 512;
					BufferLength = 0;
				}
			}
		}

		FString FinalHex()
		{
			BitCount += static_cast<uint64>(BufferLength) * 8u;
			Buffer[BufferLength++] = 0x80u;
			if (BufferLength > 56)
			{
				while (BufferLength < 64) Buffer[BufferLength++] = 0u;
				Transform(Buffer);
				BufferLength = 0;
			}
			while (BufferLength < 56) Buffer[BufferLength++] = 0u;
			for (int32 Shift = 56; Shift >= 0; Shift -= 8)
			{
				Buffer[BufferLength++] = static_cast<uint8>((BitCount >> Shift) & 0xffu);
			}
			Transform(Buffer);

			FString Result;
			Result.Reserve(64);
			for (uint32 Word : State)
			{
				for (int32 Shift = 24; Shift >= 0; Shift -= 8)
				{
					Result += FString::Printf(TEXT("%02x"), static_cast<uint8>((Word >> Shift) & 0xffu));
				}
			}
			return Result;
		}

	private:
		static uint32 RotateRight(uint32 Value, uint32 Count)
		{
			return (Value >> Count) | (Value << (32u - Count));
		}

		void Transform(const uint8 Block[64])
		{
			static constexpr uint32 K[64] =
			{
				0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
				0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
				0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
				0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
				0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
				0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
				0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
				0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
			};

			uint32 W[64];
			for (int32 Index = 0; Index < 16; ++Index)
			{
				const int32 Offset = Index * 4;
				W[Index] = (static_cast<uint32>(Block[Offset]) << 24)
					| (static_cast<uint32>(Block[Offset + 1]) << 16)
					| (static_cast<uint32>(Block[Offset + 2]) << 8)
					| static_cast<uint32>(Block[Offset + 3]);
			}
			for (int32 Index = 16; Index < 64; ++Index)
			{
				const uint32 S0 = RotateRight(W[Index - 15], 7) ^ RotateRight(W[Index - 15], 18) ^ (W[Index - 15] >> 3);
				const uint32 S1 = RotateRight(W[Index - 2], 17) ^ RotateRight(W[Index - 2], 19) ^ (W[Index - 2] >> 10);
				W[Index] = W[Index - 16] + S0 + W[Index - 7] + S1;
			}

			uint32 A = State[0], B = State[1], C = State[2], D = State[3];
			uint32 E = State[4], F = State[5], G = State[6], H = State[7];
			for (int32 Index = 0; Index < 64; ++Index)
			{
				const uint32 S1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
				const uint32 Choice = (E & F) ^ ((~E) & G);
				const uint32 T1 = H + S1 + Choice + K[Index] + W[Index];
				const uint32 S0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
				const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
				const uint32 T2 = S0 + Majority;
				H = G; G = F; F = E; E = D + T1;
				D = C; C = B; B = A; A = T1 + T2;
			}
			State[0] += A; State[1] += B; State[2] += C; State[3] += D;
			State[4] += E; State[5] += F; State[6] += G; State[7] += H;
		}

		uint32 State[8]{};
		uint64 BitCount = 0;
		uint8 Buffer[64]{};
		int32 BufferLength = 0;
	};

	FString HashPackedData(const TArray<uint32>& PackedData)
	{
		FMLSSHA256 Hasher;
		for (uint32 Packed : PackedData)
		{
			const uint8 Bytes[4] =
			{
				static_cast<uint8>(Packed & 0xffu),
				static_cast<uint8>((Packed >> 8) & 0xffu),
				static_cast<uint8>((Packed >> 16) & 0xffu),
				static_cast<uint8>((Packed >> 24) & 0xffu)
			};
			Hasher.Update(Bytes, 4);
		}
		return Hasher.FinalHex();
	}

	bool SaveStringAtomic(const FString& Path, const FString& Contents, FString& OutError)
	{
		IFileManager& FileManager = IFileManager::Get();
		if (!FileManager.MakeDirectory(*FPaths::GetPath(Path), true))
		{
			OutError = FString::Printf(TEXT("Unable to create artifact directory: %s"), *FPaths::GetPath(Path));
			return false;
		}

		const FString Temporary = Path + TEXT(".tmp");
		FileManager.Delete(*Temporary, false, true, true);
		if (!FFileHelper::SaveStringToFile(Contents, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Unable to write temporary artifact: %s"), *Temporary);
			return false;
		}
		if (!FileManager.Move(*Path, *Temporary, true, true, false, true))
		{
			FileManager.Delete(*Temporary, false, true, true);
			OutError = FString::Printf(TEXT("Unable to commit artifact: %s"), *Path);
			return false;
		}
		return true;
	}
}

bool FMLSMicroShadowLUTSettings::Validate(FString& OutError) const
{
	if (VisibilitySize < 2 || NoLSize < 2 || NoVSize < 2)
	{
		OutError = TEXT("All LUT dimensions must be at least two.");
		return false;
	}
	if (AxisLayout == EMLSMicroShadowAxisLayout::BoundaryAware && (NoLSize % 2) == 0)
	{
		OutError = TEXT("Boundary-aware NoL dimension must be odd so the smooth-limit boundary has an exact center node.");
		return false;
	}
	if (RoughnessNodes.Num() != 4 && RoughnessNodes.Num() != 7)
	{
		OutError = TEXT("Packed RGBA8 requires four nodes or seven nodes in two overlapping RGBA banks.");
		return false;
	}
	for (int32 Index = 0; Index < RoughnessNodes.Num(); ++Index)
	{
		if (!FMath::IsFinite(RoughnessNodes[Index]) || RoughnessNodes[Index] < 0.1 || RoughnessNodes[Index] > 1.0)
		{
			OutError = TEXT("Roughness nodes must be finite and inside [0.1, 1].");
			return false;
		}
		if (Index > 0 && RoughnessNodes[Index] <= RoughnessNodes[Index - 1])
		{
			OutError = TEXT("Roughness nodes must be strictly increasing.");
			return false;
		}
	}
	if (Sequence.SampleCount <= 0 || !FMath::IsFinite(RelativeAzimuthRadians))
	{
		OutError = TEXT("Sample count must be positive and relative azimuth finite.");
		return false;
	}
	return true;
}

uint32 FMLSMicroShadowLUTGenerator::ReverseBits32(uint32 Bits)
{
	Bits = (Bits << 16u) | (Bits >> 16u);
	Bits = ((Bits & 0x00ff00ffu) << 8u) | ((Bits & 0xff00ff00u) >> 8u);
	Bits = ((Bits & 0x0f0f0f0fu) << 4u) | ((Bits & 0xf0f0f0f0u) >> 4u);
	Bits = ((Bits & 0x33333333u) << 2u) | ((Bits & 0xccccccccu) >> 2u);
	Bits = ((Bits & 0x55555555u) << 1u) | ((Bits & 0xaaaaaaaau) >> 1u);
	return Bits;
}

FVector2d FMLSMicroShadowLUTGenerator::Hammersley(uint32 Index, uint32 Count, uint32 Seed)
{
	if (Count == 0u)
	{
		return FVector2d::ZeroVector;
	}

	// SequenceVersion 1: stratum center on X, digitally scrambled base-2
	// radical inverse on Y. Neither endpoint is sampled.
	const double X = (static_cast<double>(Index) + 0.5) / static_cast<double>(Count);
	const uint32 Scrambled = ReverseBits32(Index) ^ Seed;
	const double Y = (static_cast<double>(Scrambled) + 0.5) * MLSInvUint32;
	return FVector2d(X, Y);
}

FVector3d FMLSMicroShadowLUTGenerator::SampleGGXVNDFIsotropic(
	const FVector3d& ViewDirection,
	double Alpha,
	const FVector2d& Xi)
{
	Alpha = FMath::Max(Alpha, 0.0);
	if (Alpha <= 1.0e-12)
	{
		return FVector3d(0.0, 0.0, 1.0);
	}

	const FVector3d ViewStandard = SafeNormal(
		FVector3d(ViewDirection.X * Alpha, ViewDirection.Y * Alpha, ViewDirection.Z),
		FVector3d(0.0, 0.0, 1.0));
	const double Phi = MLSTwoPi * FMath::Frac(Xi.X);
	const double UY = FMath::Clamp(Xi.Y, 0.0, 1.0);
	const double Z = (1.0 - UY) * (1.0 + ViewStandard.Z) - ViewStandard.Z;
	const double SinTheta = FMath::Sqrt(FMath::Clamp(1.0 - Z * Z, 0.0, 1.0));
	const FVector3d CapDirection(SinTheta * FMath::Cos(Phi), SinTheta * FMath::Sin(Phi), Z);
	const FVector3d MicroNormalStandard = CapDirection + ViewStandard;
	return SafeNormal(
		FVector3d(MicroNormalStandard.X * Alpha, MicroNormalStandard.Y * Alpha, MicroNormalStandard.Z),
		FVector3d(0.0, 0.0, 1.0));
}

double FMLSMicroShadowLUTGenerator::LambdaGGX(double NoX, double Alpha)
{
	NoX = FMath::Max(NoX, 1.0e-6);
	const double SinSquared = FMath::Max(1.0 - NoX * NoX, 0.0);
	const double TanSquared = SinSquared / (NoX * NoX);
	return 0.5 * (FMath::Sqrt(1.0 + Alpha * Alpha * TanSquared) - 1.0);
}

double FMLSMicroShadowLUTGenerator::SmithHeightCorrelatedWeight(double NoV, double NoL, double Alpha)
{
	const double LambdaV = LambdaGGX(NoV, Alpha);
	const double LambdaL = LambdaGGX(NoL, Alpha);
	return (1.0 + LambdaV) / FMath::Max(1.0 + LambdaV + LambdaL, 1.0e-6);
}

bool FMLSMicroShadowLUTGenerator::ValidateReferenceInput(
	const FMLSMicroShadowReferenceInput& Input,
	const FMLSMicroShadowSequenceSettings& Sequence,
	FString& OutError)
{
	if (!IsUnitInput(Input.Visibility) || !IsUnitInput(Input.Roughness)
		|| !IsUnitInput(Input.NoL) || !IsUnitInput(Input.NoV))
	{
		OutError = TEXT("Visibility, Roughness, NoL and NoV must be finite values in [0,1].");
		return false;
	}
	if (!FMath::IsFinite(Input.RelativeAzimuthRadians))
	{
		OutError = TEXT("Relative azimuth must be finite.");
		return false;
	}
	if (Sequence.SampleCount <= 0)
	{
		OutError = TEXT("Reference sample count must be positive.");
		return false;
	}
	return true;
}

bool FMLSMicroShadowLUTGenerator::IntegrateReferenceChecked(
	const FMLSMicroShadowReferenceInput& Input,
	const FMLSMicroShadowSequenceSettings& Sequence,
	FMLSMicroShadowIntegralResult& OutResult,
	FString& OutError)
{
	OutResult = {};
	if (!ValidateReferenceInput(Input, Sequence, OutError))
	{
		return false;
	}
	if (Input.Visibility <= 0.0)
	{
		return true;
	}
	if (Input.Visibility >= 1.0)
	{
		OutResult.Value = 1.0;
		return true;
	}

	const double Alpha = Input.Roughness * Input.Roughness;
	const double ViewSin = FMath::Sqrt(FMath::Max(1.0 - Input.NoV * Input.NoV, 0.0));
	const double LightSin = FMath::Sqrt(FMath::Max(1.0 - Input.NoL * Input.NoL, 0.0));
	const FVector3d ViewDirection(ViewSin, 0.0, Input.NoV);
	const FVector3d LightDirection(
		LightSin * FMath::Cos(Input.RelativeAzimuthRadians),
		LightSin * FMath::Sin(Input.RelativeAzimuthRadians),
		Input.NoL);
	const double CosTheta = FMath::Sqrt(FMath::Max(1.0 - Input.Visibility, 0.0));
	const double Weight = SmithHeightCorrelatedWeight(Input.NoV, Input.NoL, Alpha);

	for (int32 SampleIndex = 0; SampleIndex < Sequence.SampleCount; ++SampleIndex)
	{
		const FVector2d Xi = Hammersley(
			static_cast<uint32>(SampleIndex),
			static_cast<uint32>(Sequence.SampleCount),
			Sequence.Seed);
		const FVector3d MicroNormal = SampleGGXVNDFIsotropic(ViewDirection, Alpha, Xi);
		const double NdotM = FMath::Clamp(MicroNormal.Z, 0.0, 1.0);
		const double MdotL = FMath::Clamp(FVector3d::DotProduct(MicroNormal, LightDirection), 0.0, 1.0);
		const double Hemisphere = StepPositive(NdotM) * StepPositive(MdotL) * Weight;
		const double Cone = StepPositive(NdotM - CosTheta) * StepPositive(MdotL - CosTheta) * Weight;
		OutResult.HemisphereSum += Hemisphere;
		OutResult.ConeSum += Cone;
		OutResult.PositiveHemisphereSamples += Hemisphere > 0.0 ? 1 : 0;
		OutResult.PositiveConeSamples += Cone > 0.0 ? 1 : 0;
	}

	OutResult.Value = OutResult.HemisphereSum <= 1.0e-8
		? 0.0
		: FMath::Clamp(OutResult.ConeSum / OutResult.HemisphereSum, 0.0, 1.0);
	return true;
}

FMLSMicroShadowIntegralResult FMLSMicroShadowLUTGenerator::IntegrateReference(
	const FMLSMicroShadowReferenceInput& Input,
	const FMLSMicroShadowSequenceSettings& Sequence)
{
	FMLSMicroShadowIntegralResult Result;
	FString IgnoredError;
	IntegrateReferenceChecked(Input, Sequence, Result, IgnoredError);
	return Result;
}

bool FMLSMicroShadowLUTGenerator::Generate(
	const FMLSMicroShadowLUTSettings& Settings,
	FMLSMicroShadowLUTData& OutData,
	FString& OutError)
{
	if (!Settings.Validate(OutError))
	{
		return false;
	}

	OutData = {};
	OutData.Settings = Settings;
	OutData.FloatTexels.SetNumUninitialized(Settings.PackedTexelCount());
	OutData.PackedRGBA8.SetNumUninitialized(Settings.PackedTexelCount());

	// Every LUT cell uses the same fixed QMC stream. Precompute it once, then
	// reuse each sampled micro-normal across every Visibility x NoL cell for a
	// fixed (NoV, roughness) group. This preserves the exact per-cell sample
	// order while avoiding ~134 million duplicate spherical-cap samplings.
	TArray<FVector2d> SequencePoints;
	SequencePoints.SetNumUninitialized(Settings.Sequence.SampleCount);
	for (int32 SampleIndex = 0; SampleIndex < Settings.Sequence.SampleCount; ++SampleIndex)
	{
		SequencePoints[SampleIndex] = Hammersley(
			static_cast<uint32>(SampleIndex),
			static_cast<uint32>(Settings.Sequence.SampleCount),
			Settings.Sequence.Seed);
	}

	TArray<float> FloatChannels;
	FloatChannels.SetNumUninitialized(Settings.PackedTexelCount() * 4);
	const int32 BankCount = Settings.RoughnessBankCount();
	const int32 GroupCount = Settings.NoVSize * BankCount * 4;
	ParallelFor(TEXT("MLS Generic VNDF LUT bake"), GroupCount, 1, [&](int32 GroupIndex)
	{
		const int32 Channel = GroupIndex % 4;
		const int32 BankAndNoV = GroupIndex / 4;
		const int32 BankIndex = BankAndNoV % BankCount;
		const int32 NoVIndex = BankAndNoV / BankCount;
		const double Roughness = Settings.RoughnessNodes[Settings.RoughnessNodeIndex(BankIndex, Channel)];
		const double Alpha = Roughness * Roughness;
		const double NoV = AxisNode(NoVIndex, Settings.NoVSize, Settings.AxisLayout, 2);
		const double ViewSin = FMath::Sqrt(FMath::Max(1.0 - NoV * NoV, 0.0));
		const FVector3d ViewDirection(ViewSin, 0.0, NoV);

		if (Settings.AxisLayout == EMLSMicroShadowAxisLayout::BoundaryAware)
		{
			const int32 CellCount = Settings.VisibilitySize * Settings.NoLSize;
			TArray<FVector3d> LightDirections;
			TArray<double> Weights;
			TArray<double> Thresholds;
			TArray<double> HemisphereSums;
			TArray<double> ConeSums;
			LightDirections.SetNumUninitialized(CellCount);
			Weights.SetNumUninitialized(CellCount);
			Thresholds.SetNumUninitialized(Settings.VisibilitySize);
			HemisphereSums.Init(0.0, CellCount);
			ConeSums.Init(0.0, CellCount);

			for (int32 VisibilityIndex = 0; VisibilityIndex < Settings.VisibilitySize; ++VisibilityIndex)
			{
				const double Visibility = AxisNode(VisibilityIndex, Settings.VisibilitySize, Settings.AxisLayout, 0);
				Thresholds[VisibilityIndex] = FMath::Sqrt(FMath::Max(1.0 - Visibility, 0.0));
				for (int32 NoLIndex = 0; NoLIndex < Settings.NoLSize; ++NoLIndex)
				{
					const int32 CellIndex = VisibilityIndex * Settings.NoLSize + NoLIndex;
					const double NoL = NoLNode(NoLIndex, Settings.NoLSize, Settings.AxisLayout, Visibility);
					const double LightSin = FMath::Sqrt(FMath::Max(1.0 - NoL * NoL, 0.0));
					LightDirections[CellIndex] = FVector3d(
						LightSin * FMath::Cos(Settings.RelativeAzimuthRadians),
						LightSin * FMath::Sin(Settings.RelativeAzimuthRadians),
						NoL);
					Weights[CellIndex] = SmithHeightCorrelatedWeight(NoV, NoL, Alpha);
				}
			}

			for (const FVector2d& Xi : SequencePoints)
			{
				const FVector3d MicroNormal = SampleGGXVNDFIsotropic(ViewDirection, Alpha, Xi);
				const double NdotM = FMath::Clamp(MicroNormal.Z, 0.0, 1.0);
				if (StepPositive(NdotM) == 0.0)
				{
					continue;
				}
				for (int32 VisibilityIndex = 1; VisibilityIndex + 1 < Settings.VisibilitySize; ++VisibilityIndex)
				{
					const double Threshold = Thresholds[VisibilityIndex];
					for (int32 NoLIndex = 0; NoLIndex < Settings.NoLSize; ++NoLIndex)
					{
						const int32 CellIndex = VisibilityIndex * Settings.NoLSize + NoLIndex;
						const double MdotL = FMath::Clamp(
							FVector3d::DotProduct(MicroNormal, LightDirections[CellIndex]), 0.0, 1.0);
						if (StepPositive(MdotL) == 0.0)
						{
							continue;
						}
						const double Weight = Weights[CellIndex];
						HemisphereSums[CellIndex] += Weight;
						if (StepPositive(NdotM - Threshold) > 0.0 && StepPositive(MdotL - Threshold) > 0.0)
						{
							ConeSums[CellIndex] += Weight;
						}
					}
				}
			}

			for (int32 VisibilityIndex = 0; VisibilityIndex < Settings.VisibilitySize; ++VisibilityIndex)
			{
				for (int32 NoLIndex = 0; NoLIndex < Settings.NoLSize; ++NoLIndex)
				{
					const int32 CellIndex = VisibilityIndex * Settings.NoLSize + NoLIndex;
					double Value = 0.0;
					if (VisibilityIndex + 1 == Settings.VisibilitySize)
					{
						Value = 1.0;
					}
					else if (VisibilityIndex > 0 && HemisphereSums[CellIndex] > 1.0e-8)
					{
						Value = FMath::Clamp(ConeSums[CellIndex] / HemisphereSums[CellIndex], 0.0, 1.0);
					}
					const int32 LinearIndex = BankIndex * Settings.NumTexels()
						+ TexelIndex(Settings, VisibilityIndex, NoLIndex, NoVIndex);
					FloatChannels[LinearIndex * 4 + Channel] = static_cast<float>(Value);
				}
			}
			return;
		}

		TArray<FVector3d, TInlineAllocator<64>> LightDirections;
		TArray<double, TInlineAllocator<64>> Weights;
		LightDirections.SetNumUninitialized(Settings.NoLSize);
		Weights.SetNumUninitialized(Settings.NoLSize);
		for (int32 NoLIndex = 0; NoLIndex < Settings.NoLSize; ++NoLIndex)
		{
			const double NoL = AxisNode(NoLIndex, Settings.NoLSize, Settings.AxisLayout, 1);
			const double LightSin = FMath::Sqrt(FMath::Max(1.0 - NoL * NoL, 0.0));
			LightDirections[NoLIndex] = FVector3d(
				LightSin * FMath::Cos(Settings.RelativeAzimuthRadians),
				LightSin * FMath::Sin(Settings.RelativeAzimuthRadians),
				NoL);
			Weights[NoLIndex] = SmithHeightCorrelatedWeight(NoV, NoL, Alpha);
		}

		TArray<double, TInlineAllocator<64>> HemisphereSums;
		TArray<double> ConeSums;
		TArray<double, TInlineAllocator<64>> CosTheta;
		HemisphereSums.Init(0.0, Settings.NoLSize);
		ConeSums.Init(0.0, Settings.NoLSize * Settings.VisibilitySize);
		CosTheta.SetNumUninitialized(Settings.VisibilitySize);
		for (int32 VisibilityIndex = 0; VisibilityIndex < Settings.VisibilitySize; ++VisibilityIndex)
		{
			const double Visibility = AxisNode(VisibilityIndex, Settings.VisibilitySize, Settings.AxisLayout, 0);
			CosTheta[VisibilityIndex] = FMath::Sqrt(FMath::Max(1.0 - Visibility, 0.0));
		}

		for (const FVector2d& Xi : SequencePoints)
		{
			const FVector3d MicroNormal = SampleGGXVNDFIsotropic(ViewDirection, Alpha, Xi);
			const double NdotM = FMath::Clamp(MicroNormal.Z, 0.0, 1.0);
			if (StepPositive(NdotM) == 0.0)
			{
				continue;
			}

			for (int32 NoLIndex = 0; NoLIndex < Settings.NoLSize; ++NoLIndex)
			{
				const double MdotL = FMath::Clamp(
					FVector3d::DotProduct(MicroNormal, LightDirections[NoLIndex]), 0.0, 1.0);
				if (StepPositive(MdotL) == 0.0)
				{
					continue;
				}

				const double Weight = Weights[NoLIndex];
				HemisphereSums[NoLIndex] += Weight;
				for (int32 VisibilityIndex = 1; VisibilityIndex + 1 < Settings.VisibilitySize; ++VisibilityIndex)
				{
					const double Threshold = CosTheta[VisibilityIndex];
					if (StepPositive(NdotM - Threshold) > 0.0 && StepPositive(MdotL - Threshold) > 0.0)
					{
						ConeSums[NoLIndex * Settings.VisibilitySize + VisibilityIndex] += Weight;
					}
				}
			}
		}

		for (int32 NoLIndex = 0; NoLIndex < Settings.NoLSize; ++NoLIndex)
		{
			for (int32 VisibilityIndex = 0; VisibilityIndex < Settings.VisibilitySize; ++VisibilityIndex)
			{
				double Value = 0.0;
				if (VisibilityIndex + 1 == Settings.VisibilitySize)
				{
					Value = 1.0;
				}
				else if (VisibilityIndex > 0 && HemisphereSums[NoLIndex] > 1.0e-8)
				{
					Value = FMath::Clamp(
						ConeSums[NoLIndex * Settings.VisibilitySize + VisibilityIndex] / HemisphereSums[NoLIndex],
						0.0,
						1.0);
				}
				const int32 LinearIndex = BankIndex * Settings.NumTexels()
					+ TexelIndex(Settings, VisibilityIndex, NoLIndex, NoVIndex);
				FloatChannels[LinearIndex * 4 + Channel] = static_cast<float>(Value);
			}
		}
	}, EParallelForFlags::Unbalanced);

	for (int32 LinearIndex = 0; LinearIndex < Settings.PackedTexelCount(); ++LinearIndex)
	{
		const float* Channels = &FloatChannels[LinearIndex * 4];
		OutData.FloatTexels[LinearIndex] = FVector4f(Channels[0], Channels[1], Channels[2], Channels[3]);
		uint32 Packed = 0u;
		for (int32 Channel = 0; Channel < 4; ++Channel)
		{
			const uint32 Quantized = static_cast<uint32>(FMath::Clamp(FMath::RoundToInt(Channels[Channel] * 255.0f), 0, 255));
			Packed |= Quantized << (Channel * 8);
		}
		OutData.PackedRGBA8[LinearIndex] = Packed;
	}

	OutData.DataSHA256 = ComputePackedDataSHA256(OutData.PackedRGBA8);
	return true;
}

FString FMLSMicroShadowLUTGenerator::ComputePackedDataSHA256(const TArray<uint32>& PackedData)
{
	return HashPackedData(PackedData);
}

double FMLSMicroShadowLUTGenerator::SampleFloatLUT(
	const FMLSMicroShadowLUTData& Data,
	double Visibility,
	double Roughness,
	double NoL,
	double NoV)
{
	return SampleLUT(Data, Visibility, Roughness, NoL, NoV,
		[&](int32 Index, int32 Channel) { return static_cast<double>(VectorChannel(Data.FloatTexels[Index], Channel)); });
}

double FMLSMicroShadowLUTGenerator::SamplePackedLUT(
	const FMLSMicroShadowLUTData& Data,
	double Visibility,
	double Roughness,
	double NoL,
	double NoV)
{
	return SampleLUT(Data, Visibility, Roughness, NoL, NoV,
		[&](int32 Index, int32 Channel) { return static_cast<double>(PackedChannel(Data.PackedRGBA8[Index], Channel)) / 255.0; });
}

FString FMLSMicroShadowLUTGenerator::AxisLayoutName(EMLSMicroShadowAxisLayout Layout)
{
	switch (Layout)
	{
	case EMLSMicroShadowAxisLayout::Warped: return TEXT("Warped");
	case EMLSMicroShadowAxisLayout::BoundaryAware: return TEXT("BoundaryAware");
	default: return TEXT("Linear");
	}
}

FString FMLSMicroShadowLUTGenerator::RoughnessInterpolationName(EMLSMicroShadowRoughnessInterpolation Interpolation)
{
	return Interpolation == EMLSMicroShadowRoughnessInterpolation::Alpha ? TEXT("Alpha") : TEXT("PerceptualRoughness");
}

bool FMLSMicroShadowLUTGenerator::WriteArtifacts(
	const FMLSMicroShadowLUTData& Data,
	const FMLSMicroShadowArtifactPaths& Paths,
	FString& OutError)
{
	FString ValidationError;
	if (!Data.Settings.Validate(ValidationError)
		|| Data.FloatTexels.Num() != Data.Settings.PackedTexelCount()
		|| Data.PackedRGBA8.Num() != Data.Settings.PackedTexelCount()
		|| Data.DataSHA256.Len() != 64)
	{
		OutError = ValidationError.IsEmpty() ? TEXT("LUT data is incomplete or inconsistent.") : ValidationError;
		return false;
	}

	FString Shader;
	Shader.Reserve(Data.PackedRGBA8.Num() * 14 + 2048);
	Shader += TEXT("// Generated by MultiLobeSpec Generic VNDF LUT Generator. Do not edit.\n");
	Shader += FString::Printf(TEXT("// generatorVersion=%u sequenceVersion=%u dataSHA256=%s\n"), GeneratorVersion, SequenceVersion, *Data.DataSHA256);
	Shader += TEXT("#ifndef MLS_MICROSHADOW_LUT_GENERATED\n#define MLS_MICROSHADOW_LUT_GENERATED 1\n");
	Shader += FString::Printf(TEXT("#define MLS_VNDF_LUT_VISIBILITY_SIZE %d\n"), Data.Settings.VisibilitySize);
	Shader += FString::Printf(TEXT("#define MLS_VNDF_LUT_NOL_SIZE %d\n"), Data.Settings.NoLSize);
	Shader += FString::Printf(TEXT("#define MLS_VNDF_LUT_NOV_SIZE %d\n"), Data.Settings.NoVSize);
	Shader += FString::Printf(TEXT("#define MLS_VNDF_LUT_TEXELS_PER_BANK %d\n"), Data.Settings.NumTexels());
	Shader += FString::Printf(TEXT("#define MLS_VNDF_LUT_ROUGHNESS_BANK_COUNT %d\n"), Data.Settings.RoughnessBankCount());
	Shader += FString::Printf(TEXT("#define MLS_VNDF_LUT_TEXEL_COUNT %d\n"), Data.PackedRGBA8.Num());
	Shader += FString::Printf(TEXT("#define MLS_VNDF_LUT_AXIS_WARPED %d\n"), Data.Settings.AxisLayout == EMLSMicroShadowAxisLayout::Warped ? 1 : 0);
	Shader += FString::Printf(TEXT("#define MLS_VNDF_LUT_AXIS_BOUNDARY_AWARE %d\n"), Data.Settings.AxisLayout == EMLSMicroShadowAxisLayout::BoundaryAware ? 1 : 0);
	Shader += FString::Printf(TEXT("#define MLS_VNDF_LUT_ROUGHNESS_ALPHA_INTERPOLATION %d\n"), Data.Settings.RoughnessInterpolation == EMLSMicroShadowRoughnessInterpolation::Alpha ? 1 : 0);
	Shader += FString::Printf(TEXT("static const float4 MLS_VNDF_LUT_ROUGHNESS_NODES = float4(%.9g, %.9g, %.9g, %.9g);\n"),
		Data.Settings.RoughnessNodes[0], Data.Settings.RoughnessNodes[1], Data.Settings.RoughnessNodes[2], Data.Settings.RoughnessNodes[3]);
	if (Data.Settings.RoughnessBankCount() == 2)
	{
		Shader += FString::Printf(TEXT("static const float4 MLS_VNDF_LUT_ROUGHNESS_NODES_HIGH = float4(%.9g, %.9g, %.9g, %.9g);\n"),
			Data.Settings.RoughnessNodes[3], Data.Settings.RoughnessNodes[4], Data.Settings.RoughnessNodes[5], Data.Settings.RoughnessNodes[6]);
		Shader += FString::Printf(TEXT("#define MLS_VNDF_LUT_ROUGHNESS_BANK_SPLIT %.9g\n"), Data.Settings.RoughnessNodes[3]);
	}
	Shader += FString::Printf(TEXT("static const uint MLS_VNDF_LUT_PACKED[%d] =\n{\n"), Data.PackedRGBA8.Num());
	for (int32 Index = 0; Index < Data.PackedRGBA8.Num(); ++Index)
	{
		Shader += FString::Printf(TEXT("0x%08xu%s"), Data.PackedRGBA8[Index], Index + 1 == Data.PackedRGBA8.Num() ? TEXT("") : TEXT(","));
		Shader += (Index % 8 == 7 || Index + 1 == Data.PackedRGBA8.Num()) ? TEXT("\n") : TEXT(" ");
	}
	Shader += TEXT("};\n#endif // MLS_MICROSHADOW_LUT_GENERATED\n");

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("algorithm"), TEXT("GenericVNDFBasedMicroShadowing"));
	Root->SetStringField(TEXT("distribution"), TEXT("IsotropicGGX"));
	Root->SetStringField(TEXT("vndfSampler"), TEXT("DupuyBenyoub2023"));
	Root->SetStringField(TEXT("smithCorrelation"), TEXT("HeightCorrelated"));
	Root->SetStringField(TEXT("visibilityWeighting"), TEXT("CosineWeightedProjectedSolidAngle"));
	TArray<TSharedPtr<FJsonValue>> Dimensions;
	Dimensions.Add(MakeShared<FJsonValueNumber>(Data.Settings.VisibilitySize));
	Dimensions.Add(MakeShared<FJsonValueNumber>(Data.Settings.NoLSize));
	Dimensions.Add(MakeShared<FJsonValueNumber>(Data.Settings.NoVSize));
	Root->SetArrayField(TEXT("dimensions"), Dimensions);
	TArray<TSharedPtr<FJsonValue>> RoughnessNodes;
	for (double Node : Data.Settings.RoughnessNodes) RoughnessNodes.Add(MakeShared<FJsonValueNumber>(Node));
	Root->SetArrayField(TEXT("roughnessNodes"), RoughnessNodes);
	if (Data.Settings.RoughnessBankCount() == 2)
	{
		TArray<TSharedPtr<FJsonValue>> Banks;
		for (int32 BankIndex = 0; BankIndex < 2; ++BankIndex)
		{
			TArray<TSharedPtr<FJsonValue>> Bank;
			for (int32 Channel = 0; Channel < 4; ++Channel)
			{
				Bank.Add(MakeShared<FJsonValueNumber>(Data.Settings.RoughnessNodeIndex(BankIndex, Channel)));
			}
			Banks.Add(MakeShared<FJsonValueArray>(Bank));
		}
		Root->SetArrayField(TEXT("roughnessBanks"), Banks);
		Root->SetNumberField(TEXT("roughnessBankSplit"), Data.Settings.RoughnessNodes[3]);
		Root->SetStringField(TEXT("roughnessBankSelection"), TEXT("SingleBankPiecewise"));
		Root->SetStringField(TEXT("packedBankOrder"), TEXT("LowRGBAThenHighRGBA"));
	}
	TSharedRef<FJsonObject> AxisWarp = MakeShared<FJsonObject>();
	if (Data.Settings.AxisLayout == EMLSMicroShadowAxisLayout::BoundaryAware)
	{
		AxisWarp->SetStringField(TEXT("visibility"), TEXT("symmetricRatioSquare"));
		AxisWarp->SetStringField(TEXT("NoL"), TEXT("smoothLimitBoundarySignedSquare"));
		AxisWarp->SetStringField(TEXT("NoV"), TEXT("square"));
	}
	else if (Data.Settings.AxisLayout == EMLSMicroShadowAxisLayout::Warped)
	{
		AxisWarp->SetStringField(TEXT("visibility"), TEXT("square"));
		AxisWarp->SetStringField(TEXT("NoL"), TEXT("oneMinusSquare"));
		AxisWarp->SetStringField(TEXT("NoV"), TEXT("square"));
	}
	else
	{
		AxisWarp->SetStringField(TEXT("visibility"), TEXT("linear"));
		AxisWarp->SetStringField(TEXT("NoL"), TEXT("linear"));
		AxisWarp->SetStringField(TEXT("NoV"), TEXT("linear"));
	}
	Root->SetObjectField(TEXT("axisWarp"), AxisWarp);
	Root->SetStringField(TEXT("roughnessInterpolation"), RoughnessInterpolationName(Data.Settings.RoughnessInterpolation));
	Root->SetStringField(TEXT("relativeAzimuth"), TEXT("coplanar_same_azimuth"));
	Root->SetNumberField(TEXT("relativeAzimuthRadians"), Data.Settings.RelativeAzimuthRadians);
	Root->SetNumberField(TEXT("samplesPerCell"), Data.Settings.Sequence.SampleCount);
	Root->SetStringField(TEXT("sequence"), TEXT("DigitallyScrambledHammersley"));
	Root->SetNumberField(TEXT("sequenceVersion"), SequenceVersion);
	Root->SetNumberField(TEXT("seed"), Data.Settings.Sequence.Seed);
	Root->SetStringField(TEXT("quantization"), TEXT("RGBA8_UNORM"));
	TArray<TSharedPtr<FJsonValue>> LinearIndexOrder;
	LinearIndexOrder.Add(MakeShared<FJsonValueString>(TEXT("Visibility")));
	LinearIndexOrder.Add(MakeShared<FJsonValueString>(TEXT("NoL")));
	LinearIndexOrder.Add(MakeShared<FJsonValueString>(TEXT("NoV")));
	Root->SetArrayField(TEXT("linearIndexOrder"), LinearIndexOrder);
	Root->SetStringField(TEXT("channelPacking"), TEXT("RGBA_LittleEndian"));
	Root->SetNumberField(TEXT("generatorVersion"), GeneratorVersion);
	Root->SetStringField(TEXT("dataSHA256"), Data.DataSHA256);

	FString Manifest;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Manifest);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		OutError = TEXT("Unable to serialize LUT manifest.");
		return false;
	}
	Manifest += TEXT("\n");

	if (!SaveStringAtomic(Paths.ShaderInclude, Shader, OutError))
	{
		return false;
	}
	if (!SaveStringAtomic(Paths.Manifest, Manifest, OutError))
	{
		return false;
	}
	return true;
}

bool FMLSMicroShadowLUTGenerator::GetCanonicalArtifactPaths(
	FMLSMicroShadowArtifactPaths& OutPaths,
	FString& OutError)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MultiLobeSpec"));
	if (!Plugin.IsValid())
	{
		OutError = TEXT("MultiLobeSpec plugin base directory is unavailable.");
		return false;
	}

	const FString GeneratedDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Generated"));
	OutPaths.ShaderInclude = FPaths::Combine(GeneratedDir, TEXT("MLS_MicroShadowLUT.ush"));
	OutPaths.Manifest = FPaths::Combine(GeneratedDir, TEXT("MLS_MicroShadowLUT.manifest.json"));
	OutPaths.ValidationJson = FPaths::Combine(GeneratedDir, TEXT("MLS_MicroShadowLUT.validation.json"));
	OutPaths.ValidationCsv = FPaths::Combine(GeneratedDir, TEXT("MLS_MicroShadowLUT.validation.csv"));
	return true;
}
