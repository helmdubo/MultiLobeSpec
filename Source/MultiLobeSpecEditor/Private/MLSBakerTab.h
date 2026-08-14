#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "MLSBakerCore.h"

class STextBlock;
class SVerticalBox;
class UMaterialInstanceConstant;
class UTexture2D;

class SMLSBakerTab : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMLSBakerTab) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& Args);

private:
	FReply OnGather();
	FReply OnBake();
	FReply OnCycleBakePreset();
	void ApplyBakePreset(EMLSBakePreset Preset);
	void MarkBakePresetCustom();
	FText GetBakePresetText() const;
	FText GetDiagnosticsText() const;
	void RefreshFoundNormalList();
	void RemoveNormalFromBakeList(TWeakObjectPtr<UTexture2D> Normal);
	void GatherFromSelection(
		TArray<TWeakObjectPtr<UTexture2D>>& OutNormals,
		TArray<TWeakObjectPtr<UMaterialInstanceConstant>>& OutInstances,
		FString& OutSummary) const;
	bool AssignBakedAOToGathered(
		const TSet<UTexture2D*>* AllowedNormals,
		int32& OutAssignments,
		int32& OutSkipped,
		FString& OutDetails) const;
	bool PassesMasterFilter(const UMaterialInstanceConstant* Instance) const;

	FMLSPhysicalBakeSettings Physical;
	FMLSBakeDiagnostics LastDiagnostics;
	bool bHasDiagnostics = false;
	EMLSBakePreset BakePreset = EMLSBakePreset::StoneBrickMedium;
	TArray<TWeakObjectPtr<UTexture2D>> FoundNormals;
	TArray<TWeakObjectPtr<UMaterialInstanceConstant>> FoundInstances;
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<SVerticalBox> ListRowsWidget;

	FString MasterFilter = TEXT("rendinst_vcolor_layered");
	FString NormalParamNames;
	FString AOParamNames = TEXT("AO1,AO2,AO3");
	FString NormalSuffix = TEXT("_tex_n");
	FString AOSuffix = TEXT("_tex_ao");
	bool bAssignAfterBake = true;
};
