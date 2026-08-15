#include "MLSBakerCore.h"

#include "Algo/Sort.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/ParallelFor.h"
#include "EditorAssetLibrary.h"
#include "Engine/Texture2D.h"
#include "Math/Float16.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include <algorithm>
#include <cfloat>

namespace
{
	constexpr float MLS_PI = 3.14159265358979323846f;
	constexpr float MLS_TWO_PI = 6.28318530717958647692f;
	constexpr float MLS_EPS = 1.0e-8f;

	struct FComplex32
	{
		float R = 0.0f;
		float I = 0.0f;
	};

	struct FMultigridLevel
	{
		int32 W = 0;
		int32 H = 0;
		float Spacing = 1.0f;
		TArray<float> U;
		TArray<float> B;
		TArray<float> Residual;
		TArray<float> Scratch;
	};

	FORCEINLINE bool IsPowerOfTwoInt(const int32 Value)
	{
		return Value > 0 && (Value & (Value - 1)) == 0;
	}

	FORCEINLINE int32 WrapCoordinate(int32 C, const int32 Size)
	{
		if (Size <= 1)
		{
			return 0;
		}
		C %= Size;
		return C < 0 ? C + Size : C;
	}

	FORCEINLINE int32 MirrorCoordinate(int32 C, const int32 Size)
	{
		if (Size <= 1)
		{
			return 0;
		}
		const int32 Period = 2 * (Size - 1);
		C %= Period;
		if (C < 0)
		{
			C += Period;
		}
		return C < Size ? C : Period - C;
	}

	FORCEINLINE int32 ResolveCoordinate(const int32 C, const int32 Size, const EMLSBoundaryMode Boundary)
	{
		switch (Boundary)
		{
		case EMLSBoundaryMode::Wrap:
			return WrapCoordinate(C, Size);
		case EMLSBoundaryMode::Mirror:
			return MirrorCoordinate(C, Size);
		case EMLSBoundaryMode::Clamp:
		default:
			return FMath::Clamp(C, 0, FMath::Max(0, Size - 1));
		}
	}

	FORCEINLINE int32 ResolveIndex(
		const int32 X,
		const int32 Y,
		const int32 W,
		const int32 H,
		const EMLSBoundaryMode Boundary)
	{
		return ResolveCoordinate(Y, H, Boundary) * W + ResolveCoordinate(X, W, Boundary);
	}

	float BilinearSample(
		const TArray<float>& Image,
		const int32 W,
		const int32 H,
		const float X,
		const float Y,
		const EMLSBoundaryMode Boundary)
	{
		const int32 X0 = FMath::FloorToInt(X);
		const int32 Y0 = FMath::FloorToInt(Y);
		const float TX = X - static_cast<float>(X0);
		const float TY = Y - static_cast<float>(Y0);

		const float A = Image[ResolveIndex(X0,     Y0,     W, H, Boundary)];
		const float B = Image[ResolveIndex(X0 + 1, Y0,     W, H, Boundary)];
		const float C = Image[ResolveIndex(X0,     Y0 + 1, W, H, Boundary)];
		const float D = Image[ResolveIndex(X0 + 1, Y0 + 1, W, H, Boundary)];
		return FMath::Lerp(FMath::Lerp(A, B, TX), FMath::Lerp(C, D, TX), TY);
	}

	void SubtractMean(TArray<float>& Values)
	{
		if (Values.IsEmpty())
		{
			return;
		}

		double Sum = 0.0;
		for (const float Value : Values)
		{
			Sum += static_cast<double>(Value);
		}
		const float Mean = static_cast<float>(Sum / static_cast<double>(Values.Num()));
		ParallelFor(Values.Num(), [&Values, Mean](const int32 Index)
		{
			Values[Index] -= Mean;
		});
	}

	void FFT1D(FComplex32* Data, const int32 Count, const bool bInverse)
	{
		for (int32 I = 1, J = 0; I < Count; ++I)
		{
			int32 Bit = Count >> 1;
			for (; J & Bit; Bit >>= 1)
			{
				J ^= Bit;
			}
			J ^= Bit;
			if (I < J)
			{
				Swap(Data[I], Data[J]);
			}
		}

		for (int32 Length = 2; Length <= Count; Length <<= 1)
		{
			const float Angle = (bInverse ? MLS_TWO_PI : -MLS_TWO_PI) / static_cast<float>(Length);
			float SinAngle = 0.0f;
			float CosAngle = 1.0f;
			FMath::SinCos(&SinAngle, &CosAngle, Angle);

			for (int32 Base = 0; Base < Count; Base += Length)
			{
				FComplex32 W{1.0f, 0.0f};
				for (int32 J = 0; J < Length / 2; ++J)
				{
					const FComplex32 U = Data[Base + J];
					const FComplex32 Q = Data[Base + J + Length / 2];
					const FComplex32 V{
						Q.R * W.R - Q.I * W.I,
						Q.R * W.I + Q.I * W.R};

					Data[Base + J] = {U.R + V.R, U.I + V.I};
					Data[Base + J + Length / 2] = {U.R - V.R, U.I - V.I};

					const float NextWR = W.R * CosAngle - W.I * SinAngle;
					W.I = W.R * SinAngle + W.I * CosAngle;
					W.R = NextWR;
				}
			}
		}

		if (bInverse)
		{
			const float InvCount = 1.0f / static_cast<float>(Count);
			for (int32 I = 0; I < Count; ++I)
			{
				Data[I].R *= InvCount;
				Data[I].I *= InvCount;
			}
		}
	}

	bool SolvePoissonPeriodicFFT(
		const TArray<float>& RHS,
		const int32 W,
		const int32 H,
		TArray<float>& OutHeight)
	{
		if (!IsPowerOfTwoInt(W) || !IsPowerOfTwoInt(H) || W < 2 || H < 2)
		{
			return false;
		}

		const int32 PixelCount = W * H;
		TArray<FComplex32> Spectrum;
		Spectrum.SetNumUninitialized(PixelCount);
		ParallelFor(PixelCount, [&Spectrum, &RHS](const int32 Index)
		{
			Spectrum[Index] = {RHS[Index], 0.0f};
		});

		ParallelFor(H, [&Spectrum, W](const int32 Y)
		{
			FFT1D(Spectrum.GetData() + Y * W, W, false);
		});

		TArray<FComplex32> Transposed;
		Transposed.SetNumUninitialized(PixelCount);
		ParallelFor(H, [&Spectrum, &Transposed, W, H](const int32 Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				Transposed[X * H + Y] = Spectrum[Y * W + X];
			}
		});
		Spectrum.Empty();

		ParallelFor(W, [&Transposed, H](const int32 X)
		{
			FFT1D(Transposed.GetData() + X * H, H, false);
		});

		ParallelFor(W, [&Transposed, W, H](const int32 KX)
		{
			const float AngleX = MLS_TWO_PI * static_cast<float>(KX) / static_cast<float>(W);
			const float LambdaX = 2.0f - 2.0f * FMath::Cos(AngleX);
			for (int32 KY = 0; KY < H; ++KY)
			{
				FComplex32& Value = Transposed[KX * H + KY];
				if (KX == 0 && KY == 0)
				{
					Value = {0.0f, 0.0f};
					continue;
				}
				const float AngleY = MLS_TWO_PI * static_cast<float>(KY) / static_cast<float>(H);
				const float Lambda = LambdaX + 2.0f - 2.0f * FMath::Cos(AngleY);
				const float InvLambda = 1.0f / FMath::Max(Lambda, MLS_EPS);
				Value.R *= InvLambda;
				Value.I *= InvLambda;
			}
		});

		ParallelFor(W, [&Transposed, H](const int32 X)
		{
			FFT1D(Transposed.GetData() + X * H, H, true);
		});

		Spectrum.SetNumUninitialized(PixelCount);
		ParallelFor(W, [&Spectrum, &Transposed, W, H](const int32 X)
		{
			for (int32 Y = 0; Y < H; ++Y)
			{
				Spectrum[Y * W + X] = Transposed[X * H + Y];
			}
		});
		Transposed.Empty();

		ParallelFor(H, [&Spectrum, W](const int32 Y)
		{
			FFT1D(Spectrum.GetData() + Y * W, W, true);
		});

		OutHeight.SetNumUninitialized(PixelCount);
		ParallelFor(PixelCount, [&OutHeight, &Spectrum](const int32 Index)
		{
			OutHeight[Index] = Spectrum[Index].R;
		});
		SubtractMean(OutHeight);
		return true;
	}

	void GetStencil(
		const int32 X,
		const int32 Y,
		const int32 W,
		const int32 H,
		const EMLSBoundaryMode Boundary,
		int32 (&OutNeighbours)[4],
		int32& OutDiagonal,
		float& OutNeighbourSum,
		const TArray<float>& Values)
	{
		const int32 Self = Y * W + X;
		OutNeighbours[0] = ResolveIndex(X - 1, Y, W, H, Boundary);
		OutNeighbours[1] = ResolveIndex(X + 1, Y, W, H, Boundary);
		OutNeighbours[2] = ResolveIndex(X, Y - 1, W, H, Boundary);
		OutNeighbours[3] = ResolveIndex(X, Y + 1, W, H, Boundary);

		OutDiagonal = 4;
		OutNeighbourSum = 0.0f;
		for (const int32 Neighbour : OutNeighbours)
		{
			if (Neighbour == Self)
			{
				--OutDiagonal;
			}
			else
			{
				OutNeighbourSum += Values[Neighbour];
			}
		}
	}

	void SmoothWeightedJacobi(
		FMultigridLevel& Level,
		const EMLSBoundaryMode Boundary,
		const int32 Iterations,
		const float Omega = 0.8f)
	{
		const int32 W = Level.W;
		const int32 H = Level.H;
		const float H2 = Level.Spacing * Level.Spacing;
		for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
		{
			ParallelFor(H, [&Level, Boundary, W, H, H2, Omega](const int32 Y)
			{
				for (int32 X = 0; X < W; ++X)
				{
					const int32 Index = Y * W + X;
					int32 Neighbours[4];
					int32 Diagonal = 0;
					float NeighbourSum = 0.0f;
					GetStencil(X, Y, W, H, Boundary, Neighbours, Diagonal, NeighbourSum, Level.U);
					if (Diagonal <= 0)
					{
						Level.Scratch[Index] = 0.0f;
						continue;
					}
					const float Candidate = (NeighbourSum + Level.B[Index] * H2) / static_cast<float>(Diagonal);
					Level.Scratch[Index] = FMath::Lerp(Level.U[Index], Candidate, Omega);
				}
			});
			Swap(Level.U, Level.Scratch);
		}
	}

	void ComputeResidual(FMultigridLevel& Level, const EMLSBoundaryMode Boundary)
	{
		const int32 W = Level.W;
		const int32 H = Level.H;
		const float InvH2 = 1.0f / FMath::Max(Level.Spacing * Level.Spacing, MLS_EPS);
		ParallelFor(H, [&Level, Boundary, W, H, InvH2](const int32 Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const int32 Index = Y * W + X;
				int32 Neighbours[4];
				int32 Diagonal = 0;
				float NeighbourSum = 0.0f;
				GetStencil(X, Y, W, H, Boundary, Neighbours, Diagonal, NeighbourSum, Level.U);
				const float AU = (static_cast<float>(Diagonal) * Level.U[Index] - NeighbourSum) * InvH2;
				Level.Residual[Index] = Level.B[Index] - AU;
			}
		});
	}

	void RestrictFullWeighting(
		const FMultigridLevel& Fine,
		FMultigridLevel& Coarse,
		const EMLSBoundaryMode Boundary)
	{
		ParallelFor(Coarse.H, [&Fine, &Coarse, Boundary](const int32 CY)
		{
			for (int32 CX = 0; CX < Coarse.W; ++CX)
			{
				const int32 FX = CX * 2;
				const int32 FY = CY * 2;
				const float Center = Fine.Residual[ResolveIndex(FX, FY, Fine.W, Fine.H, Boundary)] * 4.0f;
				const float Axial =
					Fine.Residual[ResolveIndex(FX - 1, FY, Fine.W, Fine.H, Boundary)] * 2.0f +
					Fine.Residual[ResolveIndex(FX + 1, FY, Fine.W, Fine.H, Boundary)] * 2.0f +
					Fine.Residual[ResolveIndex(FX, FY - 1, Fine.W, Fine.H, Boundary)] * 2.0f +
					Fine.Residual[ResolveIndex(FX, FY + 1, Fine.W, Fine.H, Boundary)] * 2.0f;
				const float Diagonal =
					Fine.Residual[ResolveIndex(FX - 1, FY - 1, Fine.W, Fine.H, Boundary)] +
					Fine.Residual[ResolveIndex(FX + 1, FY - 1, Fine.W, Fine.H, Boundary)] +
					Fine.Residual[ResolveIndex(FX - 1, FY + 1, Fine.W, Fine.H, Boundary)] +
					Fine.Residual[ResolveIndex(FX + 1, FY + 1, Fine.W, Fine.H, Boundary)];
				Coarse.B[CY * Coarse.W + CX] = (Center + Axial + Diagonal) * (1.0f / 16.0f);
			}
		});
		SubtractMean(Coarse.B);
	}

	void ProlongateAndAdd(
		const FMultigridLevel& Coarse,
		FMultigridLevel& Fine,
		const EMLSBoundaryMode Boundary)
	{
		ParallelFor(Fine.H, [&Fine, &Coarse, Boundary](const int32 Y)
		{
			const float FY = (static_cast<float>(Y) + 0.5f) * static_cast<float>(Coarse.H) / static_cast<float>(Fine.H) - 0.5f;
			for (int32 X = 0; X < Fine.W; ++X)
			{
				const float FX = (static_cast<float>(X) + 0.5f) * static_cast<float>(Coarse.W) / static_cast<float>(Fine.W) - 0.5f;
				Fine.U[Y * Fine.W + X] += BilinearSample(Coarse.U, Coarse.W, Coarse.H, FX, FY, Boundary);
			}
		});
	}

	void MultigridVCycle(
		TArray<FMultigridLevel>& Levels,
		const int32 LevelIndex,
		const FMLSDerivedBakeSettings& Settings)
	{
		FMultigridLevel& Level = Levels[LevelIndex];
		const bool bCoarsest = LevelIndex == Levels.Num() - 1;
		if (bCoarsest)
		{
			SmoothWeightedJacobi(Level, Settings.Boundary, Settings.MultigridCoarseIterations, 0.85f);
			SubtractMean(Level.U);
			return;
		}

		SmoothWeightedJacobi(Level, Settings.Boundary, Settings.MultigridPreSmooth);
		ComputeResidual(Level, Settings.Boundary);

		FMultigridLevel& Coarse = Levels[LevelIndex + 1];
		RestrictFullWeighting(Level, Coarse, Settings.Boundary);
		Coarse.U.Init(0.0f, Coarse.W * Coarse.H);
		MultigridVCycle(Levels, LevelIndex + 1, Settings);
		ProlongateAndAdd(Coarse, Level, Settings.Boundary);
		SmoothWeightedJacobi(Level, Settings.Boundary, Settings.MultigridPostSmooth);
	}

	bool SolvePoissonMultigrid(
		const TArray<float>& RHS,
		const int32 W,
		const int32 H,
		const FMLSDerivedBakeSettings& Settings,
		TArray<float>& OutHeight)
	{
		TArray<FMultigridLevel> Levels;
		FMultigridLevel Finest;
		Finest.W = W;
		Finest.H = H;
		Finest.Spacing = 1.0f;
		Finest.B = RHS;
		Finest.U.Init(0.0f, W * H);
		Finest.Residual.SetNumUninitialized(W * H);
		Finest.Scratch.SetNumUninitialized(W * H);
		Levels.Add(MoveTemp(Finest));

		while (FMath::Min(Levels.Last().W, Levels.Last().H) > Settings.MultigridCoarsestSize)
		{
			const FMultigridLevel& Previous = Levels.Last();
			FMultigridLevel Next;
			Next.W = FMath::Max(1, (Previous.W + 1) / 2);
			Next.H = FMath::Max(1, (Previous.H + 1) / 2);
			Next.Spacing = Previous.Spacing * 2.0f;
			const int32 Count = Next.W * Next.H;
			Next.U.Init(0.0f, Count);
			Next.B.Init(0.0f, Count);
			Next.Residual.SetNumUninitialized(Count);
			Next.Scratch.SetNumUninitialized(Count);
			Levels.Add(MoveTemp(Next));
			if (Levels.Last().W == 1 && Levels.Last().H == 1)
			{
				break;
			}
		}

		for (int32 Cycle = 0; Cycle < Settings.MultigridVCycles; ++Cycle)
		{
			MultigridVCycle(Levels, 0, Settings);
			SubtractMean(Levels[0].U);
		}

		OutHeight = MoveTemp(Levels[0].U);
		return OutHeight.Num() == W * H;
	}

	void BuildPoissonRHS(
		const TArray<float>& DX,
		const TArray<float>& DY,
		const int32 W,
		const int32 H,
		const EMLSBoundaryMode Boundary,
		TArray<float>& OutRHS)
	{
		OutRHS.SetNumUninitialized(W * H);
		ParallelFor(H, [&DX, &DY, &OutRHS, W, H, Boundary](const int32 Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const int32 Index = Y * W + X;
				float RHS = 0.0f;

				// Minimise the same edge-gradient energy as the original relaxation:
				//   (H[x+1]-H[x]-DX[x])^2 + (H[y+1]-H[y]-DY[y])^2.
				// D^T D is the standard positive five-point -Laplacian used by both
				// FFT and multigrid below.  A central-difference divergence paired with
				// that Laplacian attenuates/phase-shifts high frequencies.
				if (Boundary == EMLSBoundaryMode::Wrap)
				{
					RHS += DX[ResolveIndex(X - 1, Y, W, H, Boundary)] - DX[Index];
					RHS += DY[ResolveIndex(X, Y - 1, W, H, Boundary)] - DY[Index];
				}
				else
				{
					// Homogeneous Neumann boundary: there is no gradient edge leaving
					// the image. Clamp and Mirror therefore share the same Poisson RHS;
					// they still differ when GTAO samples beyond the border.
					if (X > 0)     { RHS += DX[Index - 1]; }
					if (X < W - 1) { RHS -= DX[Index]; }
					if (Y > 0)     { RHS += DY[Index - W]; }
					if (Y < H - 1) { RHS -= DY[Index]; }
				}

				OutRHS[Index] = RHS;
			}
		});
		SubtractMean(OutRHS);
	}

	// Decodes UNIT slopes: no relief multiplier and no slope clamp here. The
	// physical calibration pass scales and winsorizes AFTER the unit Poisson
	// solve, so local normal, reconstructed height and horizon sampling stay
	// consistent under the derived relief multiplier.
	bool DecodeRGNormalToSlopes(
		UTexture2D* NormalTex,
		const FMLSDerivedBakeSettings& Settings,
		TArray<float>& OutDX,
		TArray<float>& OutDY,
		int32& OutW,
		int32& OutH,
		FString& OutError)
	{
		FTextureSource& Source = NormalTex->Source;
		OutW = Source.GetSizeX();
		OutH = Source.GetSizeY();
		if (OutW <= 0 || OutH <= 0)
		{
			OutError = TEXT("no Source Art");
			return false;
		}

		TArray64<uint8> Raw;
		if (!Source.GetMipData(Raw, 0))
		{
			OutError = TEXT("GetMipData failed");
			return false;
		}

		const ETextureSourceFormat Format = Source.GetFormat();
		const int32 PixelCount = OutW * OutH;
		OutDX.SetNumUninitialized(PixelCount);
		OutDY.SetNumUninitialized(PixelCount);

		auto StoreSlope = [&Settings, &OutDX, &OutDY](const int32 Index, float NX, float NY, float NZ)
		{
			// Convert the imported convention to image-space Y-down. For a DirectX normal
			// map the decoded RG already reconstructs N = (-dh/dx,-dh/dy,+1).
			// Z is ALWAYS reconstructed from RG on the unit sphere: the blue channel
			// of compressed normal maps is unreliable and the RG-only path is the
			// project convention for this baker.
			const float ScreenNY = Settings.bDirectXNormal ? NY : -NY;
			float XY2 = NX * NX + ScreenNY * ScreenNY;
			if (XY2 >= 0.999f * 0.999f)
			{
				const float Scale = 0.999f * FMath::InvSqrt(FMath::Max(XY2, MLS_EPS));
				NX *= Scale;
				NY = ScreenNY * Scale;
				XY2 = NX * NX + NY * NY;
			}
			else
			{
				NY = ScreenNY;
			}
			NZ = FMath::Sqrt(FMath::Max(1.0f - XY2, 0.0f));

			NZ = FMath::Max(NZ, 1.0e-4f);
			OutDX[Index] = -NX / NZ;
			OutDY[Index] = -NY / NZ;
		};

		if (Format == TSF_BGRA8)
		{
			const uint8* Pixels = Raw.GetData();
			ParallelFor(PixelCount, [Pixels, &StoreSlope](const int32 Index)
			{
				const float NX = Pixels[Index * 4 + 2] * (2.0f / 255.0f) - 1.0f;
				const float NY = Pixels[Index * 4 + 1] * (2.0f / 255.0f) - 1.0f;
				const float NZ = Pixels[Index * 4 + 0] * (2.0f / 255.0f) - 1.0f;
				StoreSlope(Index, NX, NY, NZ);
			});
		}
		else if (Format == TSF_RGBA16)
		{
			const uint16* Pixels = reinterpret_cast<const uint16*>(Raw.GetData());
			ParallelFor(PixelCount, [Pixels, &StoreSlope](const int32 Index)
			{
				const float NX = Pixels[Index * 4 + 0] * (2.0f / 65535.0f) - 1.0f;
				const float NY = Pixels[Index * 4 + 1] * (2.0f / 65535.0f) - 1.0f;
				const float NZ = Pixels[Index * 4 + 2] * (2.0f / 65535.0f) - 1.0f;
				StoreSlope(Index, NX, NY, NZ);
			});
		}
		else if (Format == TSF_RGBA16F)
		{
			const FFloat16* Pixels = reinterpret_cast<const FFloat16*>(Raw.GetData());
			ParallelFor(PixelCount, [Pixels, &StoreSlope](const int32 Index)
			{
				// Normal-map source art is encoded in [0,1] even when the storage
				// type is floating point. RG are always decoded; B is ignored for
				// the default RG-only path and reconstructed from the unit sphere.
				StoreSlope(
					Index,
					Pixels[Index * 4 + 0].GetFloat() * 2.0f - 1.0f,
					Pixels[Index * 4 + 1].GetFloat() * 2.0f - 1.0f,
					Pixels[Index * 4 + 2].GetFloat() * 2.0f - 1.0f);
			});
		}
		else if (Format == TSF_RGBA32F)
		{
			const float* Pixels = reinterpret_cast<const float*>(Raw.GetData());
			ParallelFor(PixelCount, [Pixels, &StoreSlope](const int32 Index)
			{
				StoreSlope(
					Index,
					Pixels[Index * 4 + 0] * 2.0f - 1.0f,
					Pixels[Index * 4 + 1] * 2.0f - 1.0f,
					Pixels[Index * 4 + 2] * 2.0f - 1.0f);
			});
		}
		else
		{
			OutError = FString::Printf(TEXT("unsupported source format (%d)"), static_cast<int32>(Format));
			return false;
		}

		if (Settings.bRemoveMeanSlope && Settings.Boundary == EMLSBoundaryMode::Wrap)
		{
			SubtractMean(OutDX);
			SubtractMean(OutDY);
		}
		return true;
	}

	float ComputeGradientResidualRMS(
		const TArray<float>& Height,
		const TArray<float>& DX,
		const TArray<float>& DY,
		const int32 W,
		const int32 H,
		const EMLSBoundaryMode Boundary,
		TArray<float>* OutPerPixel)
	{
		if (OutPerPixel)
		{
			OutPerPixel->SetNumZeroed(W * H);
		}

		double SumSquared = 0.0;
		int64 ConstraintCount = 0;
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const int32 Index = Y * W + X;
				float ErrorSquared = 0.0f;
				int32 LocalConstraintCount = 0;

				if (Boundary == EMLSBoundaryMode::Wrap || X < W - 1)
				{
					const int32 NextX = Boundary == EMLSBoundaryMode::Wrap
						? ResolveIndex(X + 1, Y, W, H, Boundary)
						: Index + 1;
					const float ErrorP = (Height[NextX] - Height[Index]) - DX[Index];
					ErrorSquared += ErrorP * ErrorP;
					++LocalConstraintCount;
				}
				if (Boundary == EMLSBoundaryMode::Wrap || Y < H - 1)
				{
					const int32 NextY = Boundary == EMLSBoundaryMode::Wrap
						? ResolveIndex(X, Y + 1, W, H, Boundary)
						: Index + W;
					const float ErrorQ = (Height[NextY] - Height[Index]) - DY[Index];
					ErrorSquared += ErrorQ * ErrorQ;
					++LocalConstraintCount;
				}

				SumSquared += static_cast<double>(ErrorSquared);
				ConstraintCount += LocalConstraintCount;
				if (OutPerPixel)
				{
					(*OutPerPixel)[Index] = LocalConstraintCount > 0
						? FMath::Sqrt(ErrorSquared / static_cast<float>(LocalConstraintCount))
						: 0.0f;
				}
			}
		}
		return FMath::Sqrt(static_cast<float>(SumSquared / FMath::Max<int64>(1, ConstraintCount)));
	}

	FORCEINLINE float IntegratedCosineArc(
		const float H,
		const float HorizonCos,
		const float SinN,
		const float CosN)
	{
		const float HorizonSinMagnitude = FMath::Sqrt(FMath::Max(1.0f - HorizonCos * HorizonCos, 0.0f));
		const float HorizonSin = H < 0.0f ? -HorizonSinMagnitude : HorizonSinMagnitude;
		const float Cos2H = 2.0f * HorizonCos * HorizonCos - 1.0f;
		const float Sin2H = 2.0f * HorizonSin * HorizonCos;
		const float Cos2HMinusN = Cos2H * CosN + Sin2H * SinN;
		return 0.25f * (CosN + 2.0f * H * SinN - Cos2HMinusN);
	}

	void ComputeOrthographicGTAO(
		const TArray<float>& Height,
		const TArray<float>& DX,
		const TArray<float>& DY,
		const int32 W,
		const int32 H,
		const FMLSDerivedBakeSettings& Settings,
		TArray<float>& OutVisibility)
	{
		const int32 SliceCount = FMath::Clamp(Settings.Slices, 1, 64);
		const int32 StepCount = FMath::Clamp(Settings.StepsPerSide, 1, 64);
		const float Radius = static_cast<float>(FMath::Max(Settings.RadiusPx, 1));
		const float DistributionPower = FMath::Max(Settings.SampleDistributionPower, 0.1f);
		// Fixed calibrated falloff kernel: full contribution up to 0.6R, smooth
		// fade to zero at R. Material differences are expressed through the
		// physical Height/Radius, not through a per-material falloff shape.
		const float FalloffBegin = 0.6f * Radius;

		TArray<FVector2f> Directions;
		Directions.SetNumUninitialized(SliceCount);
		for (int32 Slice = 0; Slice < SliceCount; ++Slice)
		{
			const float Phi = MLS_PI * static_cast<float>(Slice) / static_cast<float>(SliceCount);
			float SinPhi = 0.0f;
			float CosPhi = 1.0f;
			FMath::SinCos(&SinPhi, &CosPhi, Phi);
			Directions[Slice] = FVector2f(CosPhi, SinPhi);
		}

		TArray<float> Distances;
		Distances.SetNumUninitialized(StepCount);
		for (int32 Step = 0; Step < StepCount; ++Step)
		{
			// Centre samples in their strata so the final step is not wasted at
			// exactly Radius (where the GTAO falloff is zero).
			const float T = (static_cast<float>(Step) + 0.5f) / static_cast<float>(StepCount);
			Distances[Step] = 1.0f + (Radius - 1.0f) * FMath::Pow(T, DistributionPower);
		}

		OutVisibility.SetNumUninitialized(W * H);
		ParallelFor(H, [&Height, &DX, &DY, &OutVisibility, &Directions, &Distances, W, H, SliceCount, StepCount, Radius, FalloffBegin, &Settings](const int32 Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const int32 Index = Y * W + X;
				const float P = DX[Index];
				const float Q = DY[Index];

				const float InvNormalLength = FMath::InvSqrt(FMath::Max(1.0f + P * P + Q * Q, MLS_EPS));
				const float NX = -P * InvNormalLength;
				const float NY = -Q * InvNormalLength;
				const float NZ = InvNormalLength;
				const float CenterHeight = Height[Index];
				float VisibilitySum = 0.0f;

				for (int32 Slice = 0; Slice < SliceCount; ++Slice)
				{
					const FVector2f Direction = Directions[Slice];
					const float NormalAlongSlice = NX * Direction.X + NY * Direction.Y;
					const float ProjectedNormalLength = FMath::Sqrt(
						NormalAlongSlice * NormalAlongSlice + NZ * NZ);
					if (ProjectedNormalLength <= MLS_EPS)
					{
						continue;
					}

					const float SinN = FMath::Clamp(NormalAlongSlice / ProjectedNormalLength, -1.0f, 1.0f);
					const float CosN = FMath::Clamp(NZ / ProjectedNormalLength, -1.0f, 1.0f);
					// Unoccluded horizon of the local tangent plane for each side.
					float HorizonCosPositive = -SinN;
					float HorizonCosNegative = SinN;

					for (int32 Step = 0; Step < StepCount; ++Step)
					{
						const float Distance = Distances[Step];
						const float OffsetX = Direction.X * Distance;
						const float OffsetY = Direction.Y * Distance;

						const float PositiveHeight = BilinearSample(
							Height, W, H,
							static_cast<float>(X) + OffsetX,
							static_cast<float>(Y) + OffsetY,
							Settings.Boundary);
						const float NegativeHeight = BilinearSample(
							Height, W, H,
							static_cast<float>(X) - OffsetX,
							static_cast<float>(Y) - OffsetY,
							Settings.Boundary);

						const float DZPositive = PositiveHeight - CenterHeight;
						const float DZNegative = NegativeHeight - CenterHeight;
						const float SampleDistancePositive = FMath::Sqrt(
							FMath::Max(Distance * Distance + DZPositive * DZPositive, MLS_EPS));
						const float SampleDistanceNegative = FMath::Sqrt(
							FMath::Max(Distance * Distance + DZNegative * DZNegative, MLS_EPS));
						const float SampleCosPositive = DZPositive / SampleDistancePositive;
						const float SampleCosNegative = DZNegative / SampleDistanceNegative;
						const float WeightPositive =
							1.0f - FMath::SmoothStep(FalloffBegin, Radius, SampleDistancePositive);
						const float WeightNegative =
							1.0f - FMath::SmoothStep(FalloffBegin, Radius, SampleDistanceNegative);

						const float WeightedPositive = FMath::Lerp(-SinN, SampleCosPositive, WeightPositive);
						const float WeightedNegative = FMath::Lerp( SinN, SampleCosNegative, WeightNegative);
						HorizonCosPositive = FMath::Max(HorizonCosPositive, WeightedPositive);
						HorizonCosNegative = FMath::Max(HorizonCosNegative, WeightedNegative);
					}

					HorizonCosPositive = FMath::Clamp(HorizonCosPositive, -1.0f, 1.0f);
					HorizonCosNegative = FMath::Clamp(HorizonCosNegative, -1.0f, 1.0f);
					// XeGTAO uses a small projection-length relaxation to avoid a
					// systematic over-darkening on very steep local normals.
					const float CorrectedProjectedNormalLength = FMath::Lerp(ProjectedNormalLength, 1.0f, 0.05f);
					const float H0 = -FMath::Acos(HorizonCosNegative);
					const float H1 =  FMath::Acos(HorizonCosPositive);
					const float Arc0 = IntegratedCosineArc(H0, HorizonCosNegative, SinN, CosN);
					const float Arc1 = IntegratedCosineArc(H1, HorizonCosPositive, SinN, CosN);
					VisibilitySum += CorrectedProjectedNormalLength * (Arc0 + Arc1);
				}

				// No Strength/OutputPower remap: the asset is canonical cosine-weighted
				// visibility (cos(theta) = sqrt(1 - V)); art remaps would break that.
				OutVisibility[Index] = FMath::Clamp(VisibilitySum / static_cast<float>(SliceCount), 0.0f, 1.0f);
			}
		});
	}

	void ConvertUnitFloatToG16(const TArray<float>& Values, TArray<uint16>& OutPixels)
	{
		OutPixels.SetNumUninitialized(Values.Num());
		ParallelFor(Values.Num(), [&Values, &OutPixels](const int32 Index)
		{
			OutPixels[Index] = static_cast<uint16>(FMath::RoundToInt(
				FMath::Clamp(Values[Index], 0.0f, 1.0f) * 65535.0f));
		});
	}

	bool CreateOrUpdateGrayscaleTexture(
		const FString& PackageFolder,
		const FString& AssetName,
		const int32 W,
		const int32 H,
		const TArray<uint16>& Pixels,
		const EMLSBoundaryMode Boundary,
		FString& OutError)
	{
		const FString PackageName = PackageFolder / AssetName;
		const FString ObjectPath = PackageName + TEXT(".") + AssetName;
		UTexture2D* Texture = Cast<UTexture2D>(UEditorAssetLibrary::LoadAsset(ObjectPath));
		bool bNewAsset = false;

		if (!Texture)
		{
			if (UEditorAssetLibrary::DoesAssetExist(ObjectPath))
			{
				OutError = FString::Printf(TEXT("%s exists but is not a Texture2D"), *ObjectPath);
				return false;
			}
			UPackage* Package = CreatePackage(*PackageName);
			if (!Package)
			{
				OutError = FString::Printf(TEXT("CreatePackage failed: %s"), *PackageName);
				return false;
			}
			Texture = NewObject<UTexture2D>(Package, FName(*AssetName), RF_Public | RF_Standalone);
			if (!Texture)
			{
				OutError = FString::Printf(TEXT("NewObject failed: %s"), *ObjectPath);
				return false;
			}
			FAssetRegistryModule::AssetCreated(Texture);
			bNewAsset = true;
		}

		Texture->Modify();
		Texture->PreEditChange(nullptr);
		Texture->Source.Init(
			W,
			H,
			1,
			1,
			TSF_G16,
			reinterpret_cast<const uint8*>(Pixels.GetData()));
		Texture->CompressionSettings = TC_Grayscale;
		Texture->SRGB = false;
		Texture->MipGenSettings = TMGS_SimpleAverage;
		Texture->AddressX = Boundary == EMLSBoundaryMode::Wrap ? TA_Wrap : TA_Clamp;
		Texture->AddressY = Boundary == EMLSBoundaryMode::Wrap ? TA_Wrap : TA_Clamp;
		Texture->PostEditChange();
		Texture->UpdateResource();
		Texture->MarkPackageDirty();
		if (!UEditorAssetLibrary::SaveLoadedAsset(Texture, false))
		{
			OutError = FString::Printf(TEXT("SaveLoadedAsset failed: %s"), *ObjectPath);
			return false;
		}

		if (bNewAsset)
		{
			Texture->GetOutermost()->MarkPackageDirty();
		}
		return true;
	}

	void BuildHeightDebug(const TArray<float>& Height, TArray<float>& OutDebug)
	{
		double SumSquared = 0.0;
		for (const float Value : Height)
		{
			SumSquared += static_cast<double>(Value * Value);
		}
		const float RMS = FMath::Sqrt(static_cast<float>(SumSquared / FMath::Max(1, Height.Num())));
		const float Scale = FMath::Max(3.0f * RMS, 1.0e-4f);
		OutDebug.SetNumUninitialized(Height.Num());
		ParallelFor(Height.Num(), [&Height, &OutDebug, Scale](const int32 Index)
		{
			OutDebug[Index] = FMath::Clamp(0.5f + 0.5f * Height[Index] / Scale, 0.0f, 1.0f);
		});
	}

	void BuildResidualDebug(const TArray<float>& Error, const float RMS, TArray<float>& OutDebug)
	{
		const float Scale = FMath::Max(3.0f * RMS, 1.0e-4f);
		OutDebug.SetNumUninitialized(Error.Num());
		ParallelFor(Error.Num(), [&Error, &OutDebug, Scale](const int32 Index)
		{
			OutDebug[Index] = 1.0f - FMath::Exp(-Error[Index] / Scale);
		});
	}

	float PercentileInPlace(TArray<float>& Scratch, const float Fraction)
	{
		const int32 Count = Scratch.Num();
		if (Count <= 0)
		{
			return 0.0f;
		}
		const int32 Index = FMath::Clamp(
			static_cast<int32>(Fraction * static_cast<float>(Count - 1) + 0.5f), 0, Count - 1);
		std::nth_element(Scratch.GetData(), Scratch.GetData() + Index, Scratch.GetData() + Count);
		return Scratch[Index];
	}

	void WriteBakeMetadataSidecar(
		const FString& AOPackageName,
		const FString& NormalPath,
		const FMLSPhysicalBakeSettings& Physical,
		const int32 W,
		const int32 H,
		FMLSBakeDiagnostics& Diagnostics)
	{
		FString MetadataPath;
		if (!FPackageName::TryConvertLongPackageNameToFilename(AOPackageName, MetadataPath, TEXT(".mlsbake.json")))
		{
			Diagnostics.Warnings.Add(TEXT("bake metadata sidecar path could not be resolved"));
			return;
		}

		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), TEXT("MLSBakeMetadataV1"));
		Root->SetStringField(TEXT("normalTexture"), NormalPath);
		Root->SetNumberField(TEXT("surfaceSizeCm"), Physical.SurfaceSizeCm);
		Root->SetNumberField(TEXT("reliefHeightCm"), Physical.ReliefHeightCm);
		Root->SetNumberField(TEXT("radiusCm"), Physical.OcclusionRadiusCm);
		Root->SetNumberField(TEXT("samplesPerTexelRequested"), Physical.QualitySamplesPerTexel);
		Root->SetNumberField(TEXT("samplesPerTexelActual"), Diagnostics.ActualSamplesPerTexel);
		Root->SetNumberField(TEXT("derivedReliefScale"), Diagnostics.AppliedReliefScale);
		Root->SetNumberField(TEXT("normalImpliedHeightCm"), Diagnostics.NormalImpliedHeightCm);
		Root->SetNumberField(TEXT("cmPerTexel"), Diagnostics.CmPerTexel);
		Root->SetNumberField(TEXT("radiusTexels"), Diagnostics.RadiusTexels);
		Root->SetNumberField(TEXT("slices"), Diagnostics.Slices);
		Root->SetNumberField(TEXT("stepsPerSide"), Diagnostics.StepsPerSide);
		Root->SetNumberField(TEXT("distributionPower"), 2.35);
		Root->SetStringField(TEXT("falloffKernel"), TEXT("1 - smoothstep(0.6R, R, d)"));
		Root->SetNumberField(TEXT("strength"), 1.0);
		Root->SetNumberField(TEXT("outputPower"), 1.0);
		Root->SetNumberField(TEXT("clampedSlopeFraction"), Diagnostics.ClampedSlopeFraction);
		Root->SetStringField(TEXT("solver"), Diagnostics.SolverName);
		Root->SetStringField(TEXT("boundary"), Diagnostics.BoundaryName);
		Root->SetNumberField(TEXT("width"), W);
		Root->SetNumberField(TEXT("height"), H);

		FString Output;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Output);
		if (!FJsonSerializer::Serialize(Root, Writer)
			|| !FFileHelper::SaveStringToFile(Output + TEXT("\n"), *MetadataPath))
		{
			Diagnostics.Warnings.Add(FString::Printf(TEXT("bake metadata sidecar write failed: %s"), *MetadataPath));
		}
	}
}

bool FMLSBakerCore::BakeAOForNormal(
	UTexture2D* NormalTex,
	const FMLSPhysicalBakeSettings& Physical,
	const FString& NormalSuffix,
	const FString& AOSuffix,
	FString& OutMessage,
	FMLSBakeDiagnostics* OutDiagnostics)
{
	if (!NormalTex)
	{
		OutMessage = TEXT("null texture");
		return false;
	}

	FMLSBakeDiagnostics Diagnostics;

	// ---- Derived configuration: everything below is computed, never authored ----
	FMLSDerivedBakeSettings Derived;
	Derived.bDirectXNormal = Physical.bDirectXNormal;
	Derived.bWriteDebugTextures = Physical.bWriteDebugTextures;
	Derived.Reconstruction = EMLSHeightReconstruction::Auto;
	// Boundary follows the texture's own addressing; periodic tiles also drop the
	// mean slope so the wrap reconstruction stays well-posed.
	const bool bWrap = NormalTex->AddressX == TA_Wrap && NormalTex->AddressY == TA_Wrap;
	Derived.Boundary = bWrap ? EMLSBoundaryMode::Wrap : EMLSBoundaryMode::Clamp;
	Derived.bRemoveMeanSlope = bWrap;
	MLS_QualityToSampling(Physical.QualitySamplesPerTexel, Derived.Slices, Derived.StepsPerSide);
	Diagnostics.Slices = Derived.Slices;
	Diagnostics.StepsPerSide = Derived.StepsPerSide;
	Diagnostics.ActualSamplesPerTexel = 2 * Derived.Slices * Derived.StepsPerSide;
	Diagnostics.BoundaryName = bWrap ? TEXT("Wrap (Auto)") : TEXT("Clamp (Auto)");

	// ---- Unit decode + unit Poisson solve --------------------------------------
	int32 W = 0;
	int32 H = 0;
	TArray<float> DX;
	TArray<float> DY;
	FString Error;
	if (!DecodeRGNormalToSlopes(NormalTex, Derived, DX, DY, W, H, Error))
	{
		OutMessage = FString::Printf(TEXT("%s: %s"), *NormalTex->GetName(), *Error);
		return false;
	}
	const int32 PixelCount = W * H;
	const float SurfaceSizeCm = FMath::Max(Physical.SurfaceSizeCm, 1.0f);
	const float CmPerTexel = SurfaceSizeCm / static_cast<float>(W);
	Diagnostics.CmPerTexel = CmPerTexel;

	TArray<float> RHS;
	BuildPoissonRHS(DX, DY, W, H, Derived.Boundary, RHS);

	TArray<float> Height;
	const bool bFFTCompatible =
		Derived.Boundary == EMLSBoundaryMode::Wrap &&
		IsPowerOfTwoInt(W) &&
		IsPowerOfTwoInt(H);
	bool bSolved = false;
	if (bFFTCompatible)
	{
		bSolved = SolvePoissonPeriodicFFT(RHS, W, H, Height);
		Diagnostics.SolverName = TEXT("FFT (Auto)");
	}
	if (!bSolved)
	{
		bSolved = SolvePoissonMultigrid(RHS, W, H, Derived, Height);
		Diagnostics.SolverName = TEXT("Multigrid (Auto)");
	}
	if (!bSolved)
	{
		OutMessage = FString::Printf(TEXT("%s: Poisson solver failed"), *NormalTex->GetName());
		return false;
	}
	SubtractMean(Height);

	// ---- Physical calibration ---------------------------------------------------
	// Robust peak-to-valley span (P1..P99 of unit reconstructed height, in
	// texel-height units); min/max would let a single damaged texel rescale the
	// whole map. The unit solve is linear, so the requested physical height is a
	// single multiplier on height AND slopes -- no second solve.
	{
		TArray<float> Scratch = Height;
		const float P1 = PercentileInPlace(Scratch, 0.01f);
		const float P99 = PercentileInPlace(Scratch, 0.99f);
		Diagnostics.NormalImpliedHeightCm = FMath::Max(P99 - P1, 0.0f) * CmPerTexel;
	}
	const float ReliefMultiplier =
		FMath::Max(Physical.ReliefHeightCm, 0.001f)
		/ FMath::Max(Diagnostics.NormalImpliedHeightCm, 1.0e-4f);
	Diagnostics.AppliedReliefScale = ReliefMultiplier;

	ParallelFor(PixelCount, [&Height, &DX, &DY, ReliefMultiplier](const int32 Index)
	{
		Height[Index] *= ReliefMultiplier;
		DX[Index] *= ReliefMultiplier;
		DY[Index] *= ReliefMultiplier;
	});

	// Two-stage slope protection instead of an authored MaxSlope: winsorize the
	// extreme tail above P99.95, then apply the absolute tan(85 deg) limit. The
	// clamped fraction is reported, not silently absorbed.
	{
		TArray<float> Magnitudes;
		Magnitudes.SetNumUninitialized(PixelCount);
		ParallelFor(PixelCount, [&Magnitudes, &DX, &DY](const int32 Index)
		{
			Magnitudes[Index] = FMath::Sqrt(DX[Index] * DX[Index] + DY[Index] * DY[Index]);
		});
		TArray<float> Scratch = Magnitudes;
		const float WinsorLimit = PercentileInPlace(Scratch, 0.9995f);
		constexpr float AbsoluteSlopeLimit = 11.43f; // tan(85 deg)
		const float SlopeLimit = FMath::Clamp(WinsorLimit, 1.0e-3f, AbsoluteSlopeLimit);

		int64 ClampedCount = 0;
		for (int32 Index = 0; Index < PixelCount; ++Index)
		{
			if (Magnitudes[Index] > SlopeLimit)
			{
				const float Scale = SlopeLimit / Magnitudes[Index];
				DX[Index] *= Scale;
				DY[Index] *= Scale;
				++ClampedCount;
			}
		}
		Diagnostics.ClampedSlopeFraction =
			static_cast<float>(ClampedCount) / static_cast<float>(FMath::Max(PixelCount, 1));
		if (Diagnostics.ClampedSlopeFraction > 0.001f)
		{
			Diagnostics.Warnings.Add(FString::Printf(
				TEXT("requested height cannot be reproduced without clamping %.2f%% of normal slopes"),
				Diagnostics.ClampedSlopeFraction * 100.0f));
		}
	}

	// ---- Radius: physical -> texels, with tile-sanity warnings ------------------
	Diagnostics.RadiusTexels = Physical.OcclusionRadiusCm / CmPerTexel;
	Derived.RadiusPx = FMath::Max(FMath::RoundToInt(Diagnostics.RadiusTexels), 1);
	if (Diagnostics.RadiusTexels < 2.0f)
	{
		Diagnostics.Warnings.Add(FString::Printf(
			TEXT("occlusion radius is %.2f texels at this density; AO is under-resolved below 2 texels"),
			Diagnostics.RadiusTexels));
	}
	const float TileMinCm = SurfaceSizeCm * FMath::Min(1.0f, static_cast<float>(H) / static_cast<float>(W));
	if (bWrap && Physical.OcclusionRadiusCm > 0.45f * TileMinCm)
	{
		Diagnostics.Warnings.Add(FString::Printf(
			TEXT("occlusion radius %.1f cm exceeds 45%% of the %.1f cm tile; occluders repeat through the wrap"),
			Physical.OcclusionRadiusCm, TileMinCm));
	}

	TArray<float> ResidualError;
	const float GradientResidualRMS = ComputeGradientResidualRMS(
		Height, DX, DY, W, H, Derived.Boundary,
		Derived.bWriteDebugTextures ? &ResidualError : nullptr);

	TArray<float> Visibility;
	ComputeOrthographicGTAO(Height, DX, DY, W, H, Derived, Visibility);

	FString BaseName = NormalTex->GetName();
	if (!NormalSuffix.IsEmpty() && BaseName.EndsWith(NormalSuffix, ESearchCase::IgnoreCase))
	{
		BaseName.LeftChopInline(NormalSuffix.Len());
	}
	const FString PackageFolder = FPackageName::GetLongPackagePath(NormalTex->GetOutermost()->GetName());
	const FString AOAssetName = BaseName + AOSuffix;

	TArray<uint16> AO16;
	ConvertUnitFloatToG16(Visibility, AO16);
	if (!CreateOrUpdateGrayscaleTexture(
		PackageFolder, AOAssetName, W, H, AO16, Derived.Boundary, Error))
	{
		OutMessage = FString::Printf(TEXT("%s: %s"), *NormalTex->GetName(), *Error);
		return false;
	}

	// Physical calibration receipt next to the baked asset: re-bakes and the
	// assignment tool can detect an incompatible SurfaceSize/Height/Radius setup.
	WriteBakeMetadataSidecar(
		PackageFolder / AOAssetName,
		NormalTex->GetPathName(),
		Physical,
		W,
		H,
		Diagnostics);

	if (Derived.bWriteDebugTextures)
	{
		TArray<float> HeightDebug;
		TArray<float> ResidualDebug;
		TArray<uint16> Debug16;

		BuildHeightDebug(Height, HeightDebug);
		ConvertUnitFloatToG16(HeightDebug, Debug16);
		if (!CreateOrUpdateGrayscaleTexture(
			PackageFolder, BaseName + TEXT("_tex_height"), W, H, Debug16, Derived.Boundary, Error))
		{
			OutMessage = FString::Printf(TEXT("%s: AO saved, height debug failed: %s"), *NormalTex->GetName(), *Error);
			return false;
		}

		BuildResidualDebug(ResidualError, GradientResidualRMS, ResidualDebug);
		ConvertUnitFloatToG16(ResidualDebug, Debug16);
		if (!CreateOrUpdateGrayscaleTexture(
			PackageFolder, BaseName + TEXT("_tex_graderr"), W, H, Debug16, Derived.Boundary, Error))
		{
			OutMessage = FString::Printf(TEXT("%s: AO saved, residual debug failed: %s"), *NormalTex->GetName(), *Error);
			return false;
		}
	}

	float MinAO = 1.0f;
	float MeanAO = 0.0f;
	for (const float Value : Visibility)
	{
		MinAO = FMath::Min(MinAO, Value);
		MeanAO += Value;
	}
	MeanAO /= FMath::Max(1, Visibility.Num());

	FString WarningSummary;
	for (const FString& Warning : Diagnostics.Warnings)
	{
		WarningSummary += FString::Printf(TEXT(" | WARN: %s"), *Warning);
	}

	OutMessage = FString::Printf(
		TEXT("%s -> %s | %s | %dx%d @ %.4f cm/texel | implied H %.2f cm, applied x%.2f | radius %.1f tex | %dx%d = %d spp | AO min %.3f mean %.3f | grad RMS %.4f%s"),
		*NormalTex->GetName(),
		*AOAssetName,
		*Diagnostics.SolverName,
		W,
		H,
		CmPerTexel,
		Diagnostics.NormalImpliedHeightCm,
		Diagnostics.AppliedReliefScale,
		Diagnostics.RadiusTexels,
		Diagnostics.Slices,
		Diagnostics.StepsPerSide,
		Diagnostics.ActualSamplesPerTexel,
		MinAO,
		MeanAO,
		GradientResidualRMS,
		*WarningSummary);

	if (OutDiagnostics)
	{
		*OutDiagnostics = MoveTemp(Diagnostics);
	}
	return true;
}
