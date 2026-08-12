#include "MLSBakerTab.h"

#include "Algo/Sort.h"
#include "Components/MeshComponent.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "EditorUtilityLibrary.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"

#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/STextBlock.h"

#define ROW(Label, Widget) \
	+ SVerticalBox::Slot().AutoHeight().Padding(4, 2) \
	[ SNew(SHorizontalBox) \
	  + SHorizontalBox::Slot().FillWidth(0.47f).VAlign(VAlign_Center)[ SNew(STextBlock).Text(FText::FromString(Label)) ] \
	  + SHorizontalBox::Slot().FillWidth(0.53f)[ Widget ] ]

namespace
{
	void ParseNames(const FString& Source, TArray<FString>& OutNames)
	{
		OutNames.Reset();
		Source.ParseIntoArray(OutNames, TEXT(","), true);
		for (FString& Name : OutNames)
		{
			Name.TrimStartAndEndInline();
		}
		OutNames.RemoveAll([](const FString& Name) { return Name.IsEmpty(); });
	}

	bool FindParameterInfoByName(
		const TArray<FMaterialParameterInfo>& Infos,
		const FString& Name,
		FMaterialParameterInfo& OutInfo)
	{
		for (const FMaterialParameterInfo& Info : Infos)
		{
			if (Info.Name.ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				OutInfo = Info;
				return true;
			}
		}
		return false;
	}

	bool FindParameterInfoByNamePreferContext(
		const TArray<FMaterialParameterInfo>& Infos,
		const FString& Name,
		const FMaterialParameterInfo& ReferenceInfo,
		FMaterialParameterInfo& OutInfo)
	{
		// Material Layers can expose identical parameter names in multiple associations.
		// Prefer the AO parameter in the same layer/blend context as the normal.
		for (const FMaterialParameterInfo& Info : Infos)
		{
			if (Info.Name.ToString().Equals(Name, ESearchCase::IgnoreCase)
				&& Info.Association == ReferenceInfo.Association
				&& Info.Index == ReferenceInfo.Index)
			{
				OutInfo = Info;
				return true;
			}
		}

		return FindParameterInfoByName(Infos, Name, OutInfo);
	}

	int32 ExtractTrailingNumber(const FString& Name)
	{
		int32 Start = Name.Len();
		while (Start > 0 && FChar::IsDigit(Name[Start - 1]))
		{
			--Start;
		}
		if (Start == Name.Len())
		{
			return INDEX_NONE;
		}
		return FCString::Atoi(*Name.Mid(Start));
	}

	int32 ChooseAOParameterIndex(
		const FMaterialParameterInfo& NormalInfo,
		const TArray<FString>& AOParameterNames,
		const int32 FallbackIndex)
	{
		const int32 NormalNumber = ExtractTrailingNumber(NormalInfo.Name.ToString());
		if (NormalNumber != INDEX_NONE)
		{
			for (int32 Index = 0; Index < AOParameterNames.Num(); ++Index)
			{
				if (ExtractTrailingNumber(AOParameterNames[Index]) == NormalNumber)
				{
					return Index;
				}
			}
		}

		return AOParameterNames.IsValidIndex(FallbackIndex) ? FallbackIndex : INDEX_NONE;
	}
}

void SMLSBakerTab::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(6)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(NSLOCTEXT(
					"MLS",
					"Sec1P2",
					"1. P2 Offline Bake: RG Normal -> Poisson Height -> slope-aware Orthographic GTAO"))
			]

			ROW(TEXT("Master material filter (a,b,c; empty = all)"),
				SNew(SEditableTextBox)
				.Text_Lambda([this] { return FText::FromString(MasterFilter); })
				.OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type) { MasterFilter = T.ToString(); }))
			ROW(TEXT("Normal suffix"),
				SNew(SEditableTextBox)
				.Text_Lambda([this] { return FText::FromString(NormalSuffix); })
				.OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type) { NormalSuffix = T.ToString(); }))
			ROW(TEXT("AO suffix"),
				SNew(SEditableTextBox)
				.Text_Lambda([this] { return FText::FromString(AOSuffix); })
				.OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type) { AOSuffix = T.ToString(); }))

			ROW(TEXT("Poisson reconstruction"),
				SNew(SButton)
				.Text(this, &SMLSBakerTab::GetReconstructionText)
				.OnClicked(this, &SMLSBakerTab::OnCycleReconstruction))
			ROW(TEXT("Boundary mode"),
				SNew(SButton)
				.Text(this, &SMLSBakerTab::GetBoundaryText)
				.OnClicked(this, &SMLSBakerTab::OnCycleBoundary))
			ROW(TEXT("Relief scale / max slope"),
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(0.5f)
				[
					SNew(SSpinBox<float>)
					.MinValue(0.05f).MaxValue(8.0f)
					.Value_Lambda([this] { return Settings.ReliefScale; })
					.OnValueChanged_Lambda([this](const float V) { Settings.ReliefScale = V; })
				]
				+ SHorizontalBox::Slot().FillWidth(0.5f)
				[
					SNew(SSpinBox<float>)
					.MinValue(0.5f).MaxValue(32.0f)
					.Value_Lambda([this] { return Settings.MaxSlope; })
					.OnValueChanged_Lambda([this](const float V) { Settings.MaxSlope = V; })
				])
			ROW(TEXT("Multigrid V-cycles / coarse size"),
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(0.5f)
				[
					SNew(SSpinBox<int32>)
					.MinValue(1).MaxValue(24)
					.Value_Lambda([this] { return Settings.MultigridVCycles; })
					.OnValueChanged_Lambda([this](const int32 V) { Settings.MultigridVCycles = V; })
				]
				+ SHorizontalBox::Slot().FillWidth(0.5f)
				[
					SNew(SSpinBox<int32>)
					.MinValue(4).MaxValue(64)
					.Value_Lambda([this] { return Settings.MultigridCoarsestSize; })
					.OnValueChanged_Lambda([this](const int32 V) { Settings.MultigridCoarsestSize = V; })
				])

			ROW(TEXT("AO radius (texels)"),
				SNew(SSpinBox<int32>)
				.MinValue(2).MaxValue(128)
				.Value_Lambda([this] { return Settings.RadiusPx; })
				.OnValueChanged_Lambda([this](const int32 V) { Settings.RadiusPx = V; }))
			ROW(TEXT("GTAO slices / steps per side"),
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(0.5f)
				[
					SNew(SSpinBox<int32>)
					.MinValue(2).MaxValue(32)
					.Value_Lambda([this] { return Settings.Slices; })
					.OnValueChanged_Lambda([this](const int32 V) { Settings.Slices = V; })
				]
				+ SHorizontalBox::Slot().FillWidth(0.5f)
				[
					SNew(SSpinBox<int32>)
					.MinValue(2).MaxValue(24)
					.Value_Lambda([this] { return Settings.StepsPerSide; })
					.OnValueChanged_Lambda([this](const int32 V) { Settings.StepsPerSide = V; })
				])
			ROW(TEXT("Sample power / falloff start"),
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(0.5f)
				[
					SNew(SSpinBox<float>)
					.MinValue(0.5f).MaxValue(4.0f)
					.Value_Lambda([this] { return Settings.SampleDistributionPower; })
					.OnValueChanged_Lambda([this](const float V) { Settings.SampleDistributionPower = V; })
				]
				+ SHorizontalBox::Slot().FillWidth(0.5f)
				[
					SNew(SSpinBox<float>)
					.MinValue(0.0f).MaxValue(0.95f)
					.Value_Lambda([this] { return Settings.FalloffStart; })
					.OnValueChanged_Lambda([this](const float V) { Settings.FalloffStart = V; })
				])
			ROW(TEXT("AO strength / output power"),
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(0.5f)
				[
					SNew(SSpinBox<float>)
					.MinValue(0.0f).MaxValue(4.0f)
					.Value_Lambda([this] { return Settings.Strength; })
					.OnValueChanged_Lambda([this](const float V) { Settings.Strength = V; })
				]
				+ SHorizontalBox::Slot().FillWidth(0.5f)
				[
					SNew(SSpinBox<float>)
					.MinValue(0.1f).MaxValue(4.0f)
					.Value_Lambda([this] { return Settings.OutputPower; })
					.OnValueChanged_Lambda([this](const float V) { Settings.OutputPower = V; })
				])

			ROW(TEXT("DirectX normal convention"),
				SNew(SCheckBox)
				.IsChecked_Lambda([this] { return Settings.bDirectXNormal ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](const ECheckBoxState S) { Settings.bDirectXNormal = S == ECheckBoxState::Checked; }))
			ROW(TEXT("RG source: reconstruct positive Z"),
				SNew(SCheckBox)
				.IsChecked_Lambda([this] { return Settings.bReconstructZ ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](const ECheckBoxState S) { Settings.bReconstructZ = S == ECheckBoxState::Checked; }))
			ROW(TEXT("Remove mean slope for tiling"),
				SNew(SCheckBox)
				.IsChecked_Lambda([this] { return Settings.bRemoveMeanSlope ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](const ECheckBoxState S) { Settings.bRemoveMeanSlope = S == ECheckBoxState::Checked; }))
			ROW(TEXT("AO local normal from source RG"),
				SNew(SCheckBox)
				.IsChecked_Lambda([this] { return Settings.bUseSourceNormalForAO ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](const ECheckBoxState S) { Settings.bUseSourceNormalForAO = S == ECheckBoxState::Checked; }))
			ROW(TEXT("Write height + gradient-error debug maps"),
				SNew(SCheckBox)
				.IsChecked_Lambda([this] { return Settings.bWriteDebugTextures ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](const ECheckBoxState S) { Settings.bWriteDebugTextures = S == ECheckBoxState::Checked; }))

			+ SVerticalBox::Slot().AutoHeight().Padding(4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(0.5f).Padding(2)
				[
					SNew(SButton)
					.Text(NSLOCTEXT("MLS", "Find", "Find From Selection"))
					.OnClicked(this, &SMLSBakerTab::OnFindFromSelection)
				]
				+ SHorizontalBox::Slot().FillWidth(0.5f).Padding(2)
				[
					SNew(SButton)
					.IsEnabled_Lambda([this] { return FoundNormals.Num() > 0; })
					.Text_Lambda([this] { return FText::FromString(FString::Printf(TEXT("Bake P2 AO (%d)"), FoundNormals.Num())); })
					.OnClicked(this, &SMLSBakerTab::OnBake)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().MaxHeight(180.0f).Padding(4)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SAssignNew(ListTextWidget, STextBlock)
					.Text(NSLOCTEXT("MLS", "NoNormals", "No normal maps found."))
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(4)[SNew(SSeparator)]

			+ SVerticalBox::Slot().AutoHeight().Padding(6)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(NSLOCTEXT(
					"MLS",
					"Sec2P2",
					"2. Assign: match normal parameters deterministically, derive baked AO beside each normal, assign to AO parameters"))
			]
			ROW(TEXT("Normal parameter names (optional; empty = auto)"),
				SNew(SEditableTextBox)
				.Text_Lambda([this] { return FText::FromString(NormalParamNames); })
				.OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type) { NormalParamNames = T.ToString(); }))
			ROW(TEXT("AO parameter names (per layer)"),
				SNew(SEditableTextBox)
				.Text_Lambda([this] { return FText::FromString(AOParamNames); })
				.OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type) { AOParamNames = T.ToString(); }))

			+ SVerticalBox::Slot().AutoHeight().Padding(4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(0.45f).Padding(2)
				[
					SNew(SButton)
					.IsEnabled_Lambda([this] { return FoundInstances.Num() > 0; })
					.Text_Lambda([this] { return FText::FromString(FString::Printf(TEXT("Assign From Found (%d)"), FoundInstances.Num())); })
					.ToolTipText(NSLOCTEXT("MLS", "AssignFoundTip", "Assign baked AO textures to the material instances already gathered by Find From Selection."))
					.OnClicked(this, &SMLSBakerTab::OnAssign)
				]
				+ SHorizontalBox::Slot().FillWidth(0.55f).Padding(2)
				[
					SNew(SButton)
					.Text(NSLOCTEXT("MLS", "FindAssign", "Find + Assign AO From Current Selection"))
					.ToolTipText(NSLOCTEXT(
						"MLS",
						"FindAssignTip",
						"One-click assignment for selected actors, Static Mesh assets, or material instances. "
						"For each normal ending in the configured suffix, loads the exact baked AO beside it and assigns it to the corresponding AO slot."))
					.OnClicked(this, &SMLSBakerTab::OnAssignFromCurrentSelection)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(6)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text_Lambda([this] { return FText::FromString(StatusText); })
			]
		]
	];
}

FText SMLSBakerTab::GetReconstructionText() const
{
	switch (Settings.Reconstruction)
	{
	case EMLSHeightReconstruction::PeriodicFFT:
		return NSLOCTEXT("MLS", "SolverFFT", "Periodic FFT (fallback MG)");
	case EMLSHeightReconstruction::Multigrid:
		return NSLOCTEXT("MLS", "SolverMG", "Multigrid");
	case EMLSHeightReconstruction::Auto:
	default:
		return NSLOCTEXT("MLS", "SolverAuto", "Auto: FFT for power-of-two Wrap, else MG");
	}
}

FText SMLSBakerTab::GetBoundaryText() const
{
	switch (Settings.Boundary)
	{
	case EMLSBoundaryMode::Clamp:
		return NSLOCTEXT("MLS", "BoundaryClamp", "Clamp / unique texture");
	case EMLSBoundaryMode::Mirror:
		return NSLOCTEXT("MLS", "BoundaryMirror", "Mirror / unique texture");
	case EMLSBoundaryMode::Wrap:
	default:
		return NSLOCTEXT("MLS", "BoundaryWrap", "Wrap / tileable texture");
	}
}

FReply SMLSBakerTab::OnCycleReconstruction()
{
	switch (Settings.Reconstruction)
	{
	case EMLSHeightReconstruction::Auto:
		Settings.Reconstruction = EMLSHeightReconstruction::PeriodicFFT;
		break;
	case EMLSHeightReconstruction::PeriodicFFT:
		Settings.Reconstruction = EMLSHeightReconstruction::Multigrid;
		break;
	case EMLSHeightReconstruction::Multigrid:
	default:
		Settings.Reconstruction = EMLSHeightReconstruction::Auto;
		break;
	}
	return FReply::Handled();
}

FReply SMLSBakerTab::OnCycleBoundary()
{
	switch (Settings.Boundary)
	{
	case EMLSBoundaryMode::Wrap:
		Settings.Boundary = EMLSBoundaryMode::Clamp;
		break;
	case EMLSBoundaryMode::Clamp:
		Settings.Boundary = EMLSBoundaryMode::Mirror;
		break;
	case EMLSBoundaryMode::Mirror:
	default:
		Settings.Boundary = EMLSBoundaryMode::Wrap;
		break;
	}
	return FReply::Handled();
}

void SMLSBakerTab::RefreshFoundNormalList()
{
	if (!ListTextWidget.IsValid())
	{
		return;
	}

	FString Lines;
	for (const TWeakObjectPtr<UTexture2D>& WeakTexture : FoundNormals)
	{
		if (const UTexture2D* Texture = WeakTexture.Get())
		{
			if (!Lines.IsEmpty())
			{
				Lines += LINE_TERMINATOR;
			}
			Lines += Texture->GetPathName();
		}
	}

	ListTextWidget->SetText(Lines.IsEmpty()
		? NSLOCTEXT("MLS", "NoNormals", "No normal maps found.")
		: FText::FromString(Lines));
}

bool SMLSBakerTab::MatchesMasterFilter(UMaterialInterface* Mat) const
{
	if (MasterFilter.TrimStartAndEnd().IsEmpty())
	{
		return true;
	}
	const UMaterial* Base = Mat ? Mat->GetBaseMaterial() : nullptr;
	if (!Base)
	{
		return false;
	}
	TArray<FString> Names;
	MasterFilter.ParseIntoArray(Names, TEXT(","));
	for (FString& Name : Names)
	{
		if (Base->GetName().Contains(Name.TrimStartAndEnd()))
		{
			return true;
		}
	}
	return false;
}

void SMLSBakerTab::GatherFromSelection()
{
	FoundNormals.Reset();
	FoundInstances.Reset();

	TArray<UMaterialInterface*> Materials;
	for (UObject* Object : UEditorUtilityLibrary::GetSelectedAssets())
	{
		if (UTexture2D* Texture = Cast<UTexture2D>(Object))
		{
			if (Texture->GetName().EndsWith(NormalSuffix, ESearchCase::IgnoreCase))
			{
				FoundNormals.AddUnique(Texture);
			}
		}
		else if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Object))
		{
			for (const FStaticMaterial& StaticMaterial : StaticMesh->GetStaticMaterials())
			{
				if (StaticMaterial.MaterialInterface)
				{
					Materials.AddUnique(StaticMaterial.MaterialInterface);
				}
			}
		}
		else if (UMaterialInterface* Material = Cast<UMaterialInterface>(Object))
		{
			Materials.AddUnique(Material);
		}
	}

	if (GEditor)
	{
		for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
		{
			if (AActor* Actor = Cast<AActor>(*It))
			{
				TInlineComponentArray<UMeshComponent*> Components(Actor);
				for (UMeshComponent* Component : Components)
				{
					for (int32 Index = 0; Index < Component->GetNumMaterials(); ++Index)
					{
						if (UMaterialInterface* Material = Component->GetMaterial(Index))
						{
							Materials.AddUnique(Material);
						}
					}
				}
			}
		}
	}

	for (UMaterialInterface* Material : Materials)
	{
		if (!MatchesMasterFilter(Material))
		{
			continue;
		}
		if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Material))
		{
			FoundInstances.AddUnique(Instance);
		}
		TArray<UTexture*> Textures;
		Material->GetUsedTextures(Textures);
		for (UTexture* Texture : Textures)
		{
			UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
			if (Texture2D && Texture2D->GetName().EndsWith(NormalSuffix, ESearchCase::IgnoreCase))
			{
				FoundNormals.AddUnique(Texture2D);
			}
		}
	}

	RefreshFoundNormalList();
	StatusText = FString::Printf(
		TEXT("Found normals: %d, material instances: %d"),
		FoundNormals.Num(),
		FoundInstances.Num());
}

FReply SMLSBakerTab::OnFindFromSelection()
{
	GatherFromSelection();
	return FReply::Handled();
}

FReply SMLSBakerTab::OnBake()
{
	FScopedSlowTask Task(FoundNormals.Num(), NSLOCTEXT("MLS", "BakingP2", "MLS P2: Poisson + Orthographic GTAO..."));
	Task.MakeDialog(true);
	int32 SuccessCount = 0;
	FString Log;
	for (const TWeakObjectPtr<UTexture2D>& Texture : FoundNormals)
	{
		if (Task.ShouldCancel())
		{
			break;
		}
		Task.EnterProgressFrame(1.0f, FText::FromString(Texture.IsValid() ? Texture->GetName() : TEXT("?")));
		FString Message;
		if (Texture.IsValid() && FMLSBakerCore::BakeAOForNormal(
			Texture.Get(),
			Settings,
			NormalSuffix,
			AOSuffix,
			Message))
		{
			++SuccessCount;
		}
		Log += Message + TEXT("\n");
	}
	StatusText = FString::Printf(TEXT("P2 Bake: %d/%d OK\n%s"), SuccessCount, FoundNormals.Num(), *Log);
	return FReply::Handled();
}

int32 SMLSBakerTab::AssignBakedAOToGathered(FString& OutLog)
{
	TArray<FString> ExplicitNormalNames;
	TArray<FString> AOParameterNames;
	ParseNames(NormalParamNames, ExplicitNormalNames);
	ParseNames(AOParamNames, AOParameterNames);

	if (AOParameterNames.IsEmpty())
	{
		OutLog = TEXT("No AO parameter names configured.");
		return 0;
	}

	int32 Assigned = 0;
	for (const TWeakObjectPtr<UMaterialInstanceConstant>& WeakInstance : FoundInstances)
	{
		UMaterialInstanceConstant* Instance = WeakInstance.Get();
		if (!Instance)
		{
			continue;
		}

		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		Instance->GetAllTextureParameterInfo(Infos, Ids);

		struct FNormalBinding
		{
			FMaterialParameterInfo Info;
			UTexture2D* Texture = nullptr;
		};
		TArray<FNormalBinding> NormalBindings;

		if (!ExplicitNormalNames.IsEmpty())
		{
			for (const FString& ParameterName : ExplicitNormalNames)
			{
				FMaterialParameterInfo Info;
				if (!FindParameterInfoByName(Infos, ParameterName, Info))
				{
					OutLog += FString::Printf(
						TEXT("%s: normal parameter '%s' was not found.\n"),
						*Instance->GetName(),
						*ParameterName);
					continue;
				}

				UTexture* Texture = nullptr;
				if (Instance->GetTextureParameterValue(Info, Texture))
				{
					if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
					{
						if (Texture2D->GetName().EndsWith(NormalSuffix, ESearchCase::IgnoreCase))
						{
							NormalBindings.Add({Info, Texture2D});
						}
					}
				}
			}
		}
		else
		{
			for (const FMaterialParameterInfo& Info : Infos)
			{
				UTexture* Texture = nullptr;
				if (Instance->GetTextureParameterValue(Info, Texture))
				{
					UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
					if (Texture2D && Texture2D->GetName().EndsWith(NormalSuffix, ESearchCase::IgnoreCase))
					{
						NormalBindings.Add({Info, Texture2D});
					}
				}
			}

			Algo::Sort(NormalBindings, [](const FNormalBinding& A, const FNormalBinding& B)
			{
				if (A.Info.Association != B.Info.Association)
				{
					return static_cast<uint8>(A.Info.Association) < static_cast<uint8>(B.Info.Association);
				}
				if (A.Info.Index != B.Info.Index)
				{
					return A.Info.Index < B.Info.Index;
				}
				return A.Info.Name.ToString() < B.Info.Name.ToString();
			});
		}

		bool bInstanceChanged = false;
		for (int32 BindingIndex = 0; BindingIndex < NormalBindings.Num(); ++BindingIndex)
		{
			const FNormalBinding& Binding = NormalBindings[BindingIndex];
			const int32 AONameIndex = ChooseAOParameterIndex(Binding.Info, AOParameterNames, BindingIndex);
			if (!AOParameterNames.IsValidIndex(AONameIndex))
			{
				OutLog += FString::Printf(
					TEXT("%s: no AO slot configured for normal parameter %s.\n"),
					*Instance->GetName(),
					*Binding.Info.Name.ToString());
				continue;
			}

			FString BaseName = Binding.Texture->GetName();
			BaseName.LeftChopInline(NormalSuffix.Len());
			const FString Folder = FPackageName::GetLongPackagePath(Binding.Texture->GetOutermost()->GetName());
			const FString AOAssetName = BaseName + AOSuffix;
			const FString AOPath = (Folder / AOAssetName) + TEXT(".") + AOAssetName;
			UTexture2D* AOTexture = Cast<UTexture2D>(UEditorAssetLibrary::LoadAsset(AOPath));
			if (!AOTexture)
			{
				OutLog += FString::Printf(
					TEXT("%s: missing exact baked AO '%s' beside %s (normal parameter %s).\n"),
					*Instance->GetName(),
					*AOAssetName,
					*Binding.Texture->GetName(),
					*Binding.Info.Name.ToString());
				continue;
			}

			const FString& AOParameterName = AOParameterNames[AONameIndex];
			FMaterialParameterInfo AOInfo;
			if (!FindParameterInfoByNamePreferContext(Infos, AOParameterName, Binding.Info, AOInfo))
			{
				OutLog += FString::Printf(
					TEXT("%s: AO parameter '%s' was not found for normal parameter %s.\n"),
					*Instance->GetName(),
					*AOParameterName,
					*Binding.Info.Name.ToString());
				continue;
			}

			if (!bInstanceChanged)
			{
				Instance->Modify();
				bInstanceChanged = true;
			}

			Instance->SetTextureParameterValueEditorOnly(AOInfo, AOTexture);
			++Assigned;
			OutLog += FString::Printf(
				TEXT("%s: %s=%s -> %s=%s\n"),
				*Instance->GetName(),
				*Binding.Info.Name.ToString(),
				*Binding.Texture->GetName(),
				*AOInfo.Name.ToString(),
				*AOTexture->GetName());
		}

		if (bInstanceChanged)
		{
			Instance->PostEditChange();
			Instance->MarkPackageDirty();
			UEditorAssetLibrary::SaveLoadedAsset(Instance, false);
		}
	}

	return Assigned;
}

FReply SMLSBakerTab::OnAssign()
{
	FString Log;
	const int32 Assigned = AssignBakedAOToGathered(Log);
	StatusText = FString::Printf(TEXT("Assign: %d parameters\n%s"), Assigned, *Log);
	return FReply::Handled();
}

FReply SMLSBakerTab::OnAssignFromCurrentSelection()
{
	GatherFromSelection();

	FString Log;
	const int32 Assigned = AssignBakedAOToGathered(Log);
	StatusText = FString::Printf(
		TEXT("Find + Assign: %d parameters across %d material instances\n%s"),
		Assigned,
		FoundInstances.Num(),
		*Log);
	return FReply::Handled();
}

#undef ROW
