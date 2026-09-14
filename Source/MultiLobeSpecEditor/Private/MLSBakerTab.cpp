#include "MLSBakerTab.h"

#include "Algo/Sort.h"
#include "Components/MeshComponent.h"
#include "Components/SceneComponent.h"
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
#include "MaterialShared.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "UObject/UnrealType.h"

#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define MLS_ROW(Label, Widget) \
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
		return Start == Name.Len() ? INDEX_NONE : FCString::Atoi(*Name.Mid(Start));
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

#include "MLSBakerTabUI.inl"
#include "MLSBakerTabGather.inl"

bool SMLSBakerTab::AssignBakedAOToGathered(
	const TSet<UTexture2D*>* AllowedNormals,
	int32& OutAssignments,
	int32& OutSkipped,
	FString& OutDetails) const
{
	OutAssignments = 0;
	OutSkipped = 0;
	OutDetails.Reset();

	TArray<FString> ExplicitNormalNames;
	TArray<FString> AOParameterNames;
	ParseNames(NormalParamNames, ExplicitNormalNames);
	ParseNames(AOParamNames, AOParameterNames);
	const bool bAutoAOParameters = AOParameterNames.IsEmpty();
	if (bAutoAOParameters) ParseNames(TEXT("AO1,AO2,AO3"), AOParameterNames);

	for (const TWeakObjectPtr<UMaterialInstanceConstant>& WeakInstance : FoundInstances)
	{
		UMaterialInstanceConstant* Instance = WeakInstance.Get();
		if (!Instance || !PassesMasterFilter(Instance)) continue;

		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		Instance->GetAllTextureParameterInfo(Infos, Ids);
		int32 FallbackIndex = 0;
		bool bChanged = false;
		TUniquePtr<FMaterialUpdateContext> UpdateContext;

		for (const FMaterialParameterInfo& NormalInfo : Infos)
		{
			if (!ExplicitNormalNames.IsEmpty()
				&& !ExplicitNormalNames.ContainsByPredicate([&](const FString& Name)
					{ return NormalInfo.Name.ToString().Equals(Name, ESearchCase::IgnoreCase); }))
			{
				continue;
			}

			UTexture* Value = nullptr;
			if (!Instance->GetTextureParameterValue(NormalInfo, Value)) continue;
			UTexture2D* NormalTexture = Cast<UTexture2D>(Value);
			if (!NormalTexture || !NormalTexture->GetName().EndsWith(NormalSuffix, ESearchCase::IgnoreCase)) continue;
			if (AllowedNormals && !AllowedNormals->Contains(NormalTexture)) continue;

			const int32 AONameIndex = ChooseAOParameterIndex(NormalInfo, AOParameterNames, FallbackIndex++);
			FMaterialParameterInfo AOInfo;
			bool bHasAOParameter = bAutoAOParameters && FindParameterInfoByNamePreferContext(
				Infos, NormalInfo.Name.ToString() + TEXT("_ao"), NormalInfo, AOInfo);
			if (!bHasAOParameter && AOParameterNames.IsValidIndex(AONameIndex))
			{
				bHasAOParameter = FindParameterInfoByNamePreferContext(
					Infos, AOParameterNames[AONameIndex], NormalInfo, AOInfo);
			}
			if (!bHasAOParameter)
			{
				++OutSkipped;
				OutDetails += FString::Printf(TEXT("%s: no AO parameter for %s.\n"),
					*Instance->GetName(), *NormalInfo.Name.ToString());
				continue;
			}

			FString BaseName = NormalTexture->GetName();
			BaseName.LeftChopInline(NormalSuffix.Len());
			const FString Folder = FPackageName::GetLongPackagePath(NormalTexture->GetOutermost()->GetName());
			const FString AOName = BaseName + AOSuffix;
			const FString AOPath = (Folder / AOName) + TEXT(".") + AOName;
			UTexture2D* AOTexture = Cast<UTexture2D>(UEditorAssetLibrary::LoadAsset(AOPath));
			if (!AOTexture)
			{
				++OutSkipped;
				OutDetails += FString::Printf(TEXT("%s: skipped %s -> %s.\n"),
					*Instance->GetName(), *NormalInfo.Name.ToString(), *AOPath);
				continue;
			}

			if (!bChanged)
			{
				// Detach live mesh/ISM render states before changing their material.
				UpdateContext = MakeUnique<FMaterialUpdateContext>();
				UpdateContext->AddMaterialInstance(Instance);
				Instance->Modify();
				bChanged = true;
			}
			Instance->SetTextureParameterValueEditorOnly(AOInfo, AOTexture);
			++OutAssignments;
		}

		if (bChanged)
		{
			Instance->PostEditChange();
			Instance->MarkPackageDirty();
			UEditorAssetLibrary::SaveLoadedAsset(Instance, false);
		}
	}
	return OutAssignments > 0;
}

#undef MLS_ROW
