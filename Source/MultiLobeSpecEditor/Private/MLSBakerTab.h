#pragma once

#include "CoreMinimal.h"
#include "MLSBakerCore.h"
#include "Widgets/SCompoundWidget.h"

class UMaterialInstanceConstant;
class UTexture2D;
class STextBlock;

/** P2 baker UI: RG normal -> Poisson height -> orthographic GTAO, then deterministic AO assignment. */
class SMLSBakerTab : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMLSBakerTab) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FMLSBakeSettings Settings;
	FString MasterFilter = TEXT("");
	FString NormalSuffix = TEXT("_tex_n");
	FString AOSuffix = TEXT("_tex_ao");
	FString NormalParamNames = TEXT(""); // Empty = discover and sort normal parameters.
	FString AOParamNames = TEXT("AO1,AO2,AO3");

	TArray<TWeakObjectPtr<UTexture2D>> FoundNormals;
	TArray<TWeakObjectPtr<UMaterialInstanceConstant>> FoundInstances;
	TSharedPtr<STextBlock> ListTextWidget;
	FString StatusText;

	FReply OnFindFromSelection();
	FReply OnBake();
	FReply OnAssign();
	FReply OnAssignFromCurrentSelection();
	FReply OnCycleReconstruction();
	FReply OnCycleBoundary();

	void GatherFromSelection();
	int32 AssignBakedAOToGathered(FString& OutLog);
	bool MatchesMasterFilter(class UMaterialInterface* Mat) const;
	void RefreshFoundNormalList();

	FText GetReconstructionText() const;
	FText GetBoundaryText() const;
};
