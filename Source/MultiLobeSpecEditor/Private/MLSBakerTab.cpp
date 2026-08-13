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
#include "MLSBakerTabAssign.inl"

#undef MLS_ROW
