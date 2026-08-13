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
	if (AOParameterNames.IsEmpty())
	{
		OutDetails = TEXT("No AO parameter names configured.\n");
		return false;
	}

	for (const TWeakObjectPtr<UMaterialInstanceConstant>& WeakInstance : FoundInstances)
	{
		UMaterialInstanceConstant* Instance = WeakInstance.Get();
		if (!Instance || !PassesMasterFilter(Instance)) continue;

		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		Instance->GetAllTextureParameterInfo(Infos, Ids);
		int32 FallbackIndex = 0;
		bool bChanged = false;

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
			if (!AOParameterNames.IsValidIndex(AONameIndex))
			{
				++OutSkipped;
				continue;
			}

			FString BaseName = NormalTexture->GetName();
			BaseName.LeftChopInline(NormalSuffix.Len());
			const FString Folder = FPackageName::GetLongPackagePath(NormalTexture->GetOutermost()->GetName());
			const FString AOName = BaseName + AOSuffix;
			const FString AOPath = (Folder / AOName) + TEXT(".") + AOName;
			UTexture2D* AOTexture = Cast<UTexture2D>(UEditorAssetLibrary::LoadAsset(AOPath));
			FMaterialParameterInfo AOInfo;
			if (!AOTexture || !FindParameterInfoByNamePreferContext(
				Infos, AOParameterNames[AONameIndex], NormalInfo, AOInfo))
			{
				++OutSkipped;
				OutDetails += FString::Printf(TEXT("%s: skipped %s -> %s.\n"),
					*Instance->GetName(), *NormalInfo.Name.ToString(), *AOPath);
				continue;
			}

			if (!bChanged)
			{
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
