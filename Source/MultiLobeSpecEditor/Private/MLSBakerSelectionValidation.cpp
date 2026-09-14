#include "MLSBakerTab.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialShared.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMLSBakerCompositeSelectionTest,
	"MultiLobeSpec.Editor.Baker.CompositeSelectionAndAssign",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FMLSBakerCompositeSelectionTest::RunTest(const FString& Parameters)
{
	// Optional integration test: MLS itself still builds/runs without Mimir.
	UClass* CompositeClass = FindObject<UClass>(nullptr, TEXT("/Script/MimirCompositeEditor.MHCompositeActor"));
	if (!CompositeClass)
	{
		AddWarning(TEXT("NOT RUN: enable MimirComposite to test pooled composite selection."));
		return true;
	}
	FArrayProperty* Leaves = FindFProperty<FArrayProperty>(CompositeClass, TEXT("LeafPlacementComponents"));
	FObjectPropertyBase* Leaf = Leaves ? CastField<FObjectPropertyBase>(Leaves->Inner) : nullptr;
	if (!TestNotNull(TEXT("Reflected leaf component view"), Leaf) || !GEditor) return false;

	TArray<AActor*> PreviousSelection;
	for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
	{
		if (AActor* Actor = Cast<AActor>(*It)) PreviousSelection.Add(Actor);
	}
	const UWorld::InitializationValues WorldSettings = UWorld::InitializationValues().AllowAudioPlayback(false)
		.CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false, NAME_None, nullptr,
		true, ERHIFeatureLevel::Num, &WorldSettings);
	const FString Folder = TEXT("/Game/__MLSBakerTest_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	ON_SCOPE_EXIT
	{
		GEditor->SelectNone(false, true, false);
		for (AActor* Actor : PreviousSelection) GEditor->SelectActor(Actor, true, false);
		World->DestroyWorld(false);
		// Delete dependants first so force-delete does not reparent live test MIs
		// to the engine default material and compile its unrelated permutations.
		for (const TCHAR* Name : { TEXT("SelectedMI"), TEXT("OtherMI"), TEXT("rendinst_perlin_layered_Test"),
			TEXT("Stone_tex_n"), TEXT("Stone_tex_ao") })
		{
			if (UEditorAssetLibrary::DoesAssetExist(Folder / Name)) UEditorAssetLibrary::DeleteAsset(Folder / Name);
		}
		UEditorAssetLibrary::DeleteDirectory(Folder);
	};

	const auto MakeAsset = [&Folder](UClass* Class, const TCHAR* Name) -> UObject*
	{
		UObject* Asset = NewObject<UObject>(CreatePackage(*(Folder / Name)), Class, FName(Name), RF_Public | RF_Standalone);
		FAssetRegistryModule::AssetCreated(Asset);
		return Asset;
	};
	UTexture2D* Normal = CastChecked<UTexture2D>(MakeAsset(UTexture2D::StaticClass(), TEXT("Stone_tex_n")));
	TArray<FColor> Pixels;
	Pixels.Init(FColor(128, 128, 255, 255), 64);
	Normal->Source.Init(8, 8, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
	Normal->CompressionSettings = TC_Normalmap;
	Normal->SRGB = false;
	Normal->PostEditChange();
	UEditorAssetLibrary::SaveLoadedAsset(Normal, false);

	// Pre-existing AO also checks rebake + replacement, without a missing-asset log.
	UTexture2D* AO = CastChecked<UTexture2D>(MakeAsset(UTexture2D::StaticClass(), TEXT("Stone_tex_ao")));
	TArray<uint16> AOPixels;
	AOPixels.Init(65535, 64);
	AO->Source.Init(8, 8, 1, 1, TSF_G16, reinterpret_cast<const uint8*>(AOPixels.GetData()));
	AO->CompressionSettings = TC_Grayscale;
	AO->SRGB = false;
	AO->PostEditChange();
	UEditorAssetLibrary::SaveLoadedAsset(AO, false);

	UMaterial* Master = CastChecked<UMaterial>(MakeAsset(UMaterial::StaticClass(), TEXT("rendinst_perlin_layered_Test")));
	UMaterialExpressionTextureSampleParameter2D* NormalParam = NewObject<UMaterialExpressionTextureSampleParameter2D>(Master);
	NormalParam->ParameterName = TEXT("Normal1");
	NormalParam->Texture = Normal;
	NormalParam->SamplerType = SAMPLERTYPE_Normal;
	Master->GetExpressionCollection().AddExpression(NormalParam);
	Master->GetEditorOnlyData()->Normal.Connect(0, NormalParam);
	UMaterialExpressionTextureSampleParameter2D* AOParam = NewObject<UMaterialExpressionTextureSampleParameter2D>(Master);
	AOParam->ParameterName = TEXT("AO1");
	AOParam->Texture = AO;
	AOParam->SamplerType = SAMPLERTYPE_LinearGrayscale;
	Master->GetExpressionCollection().AddExpression(AOParam);
	Master->GetEditorOnlyData()->AmbientOcclusion.Connect(1, AOParam);
	UMaterialExpressionTextureSampleParameter2D* DagorNormal = NewObject<UMaterialExpressionTextureSampleParameter2D>(Master);
	DagorNormal->ParameterName = TEXT("tex2");
	DagorNormal->Texture = Normal;
	DagorNormal->SamplerType = SAMPLERTYPE_Normal;
	Master->GetExpressionCollection().AddExpression(DagorNormal);
	UMaterialExpressionAdd* CombinedNormals = NewObject<UMaterialExpressionAdd>(Master);
	CombinedNormals->A.Connect(0, NormalParam);
	CombinedNormals->B.Connect(0, DagorNormal);
	Master->GetExpressionCollection().AddExpression(CombinedNormals);
	Master->GetEditorOnlyData()->Normal.Connect(0, CombinedNormals);
	UMaterialExpressionTextureSampleParameter2D* DagorAO = NewObject<UMaterialExpressionTextureSampleParameter2D>(Master);
	DagorAO->ParameterName = TEXT("tex2_ao");
	DagorAO->Texture = AO;
	DagorAO->SamplerType = SAMPLERTYPE_LinearGrayscale;
	Master->GetExpressionCollection().AddExpression(DagorAO);
	UMaterialExpressionMultiply* CombinedAO = NewObject<UMaterialExpressionMultiply>(Master);
	CombinedAO->A.Connect(1, AOParam);
	CombinedAO->B.Connect(1, DagorAO);
	Master->GetExpressionCollection().AddExpression(CombinedAO);
	Master->GetEditorOnlyData()->AmbientOcclusion.Connect(0, CombinedAO);
	Master->PostEditChange();
	UEditorAssetLibrary::SaveLoadedAsset(Master, false);
	UMaterialInstanceConstant* SelectedMI = CastChecked<UMaterialInstanceConstant>(MakeAsset(UMaterialInstanceConstant::StaticClass(), TEXT("SelectedMI")));
	UMaterialInstanceConstant* OtherMI = CastChecked<UMaterialInstanceConstant>(MakeAsset(UMaterialInstanceConstant::StaticClass(), TEXT("OtherMI")));
	{
		FMaterialUpdateContext Context;
		for (UMaterialInstanceConstant* MI : { SelectedMI, OtherMI })
		{
			Context.AddMaterialInstance(MI);
			MI->SetParentEditorOnly(Master);
			MI->PostEditChange();
			UEditorAssetLibrary::SaveLoadedAsset(MI, false);
		}
	}

	AActor* Composite = World->SpawnActor<AActor>(CompositeClass);
	AActor* PoolOwner = World->SpawnActor<AActor>();
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Fixture mesh"), Mesh)) return false;
	UInstancedStaticMeshComponent* SelectedBucket = NewObject<UInstancedStaticMeshComponent>(PoolOwner);
	SelectedBucket->SetStaticMesh(Mesh);
	SelectedBucket->SetMaterial(0, SelectedMI);
	PoolOwner->AddInstanceComponent(SelectedBucket);
	SelectedBucket->AddInstance(FTransform::Identity);
	SelectedBucket->RegisterComponentWithWorld(World);
	UInstancedStaticMeshComponent* OtherBucket = NewObject<UInstancedStaticMeshComponent>(PoolOwner);
	OtherBucket->SetStaticMesh(Mesh);
	OtherBucket->SetMaterial(0, OtherMI);
	PoolOwner->AddInstanceComponent(OtherBucket);
	OtherBucket->AddInstance(FTransform(FVector(200, 0, 0)));
	OtherBucket->RegisterComponentWithWorld(World);
	FScriptArrayHelper View(Leaves, Leaves->ContainerPtrToValuePtr<void>(Composite));
	View.Resize(3);
	Leaf->SetObjectPropertyValue(View.GetRawPtr(0), SelectedBucket);
	Leaf->SetObjectPropertyValue(View.GetRawPtr(1), SelectedBucket); // repeated placements
	Leaf->SetObjectPropertyValue(View.GetRawPtr(2), nullptr); // empty leaf
	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(Composite, true, false);

	TSharedRef<SMLSBakerTab> Tab = SNew(SMLSBakerTab);
	Tab->OnGather();
	TestEqual(TEXT("One deduplicated normal from foreign-owned ISM"), Tab->FoundNormals.Num(), 1);
	TestEqual(TEXT("One deduplicated selected MI"), Tab->FoundInstances.Num(), 1);
	TestTrue(TEXT("Selected MI is an assignment target"), Tab->FoundInstances.Contains(SelectedMI));
	TestFalse(TEXT("Other pool bucket excluded"), Tab->FoundInstances.Contains(OtherMI));
	if (HasAnyErrors()) return false;

	// Empty allowed set must not assign an existing AO after a failed/cancelled bake.
	TSet<UTexture2D*> NoSuccessfulNormals;
	int32 Assigned = 0, Skipped = 0;
	FString Details;
	Tab->AssignBakedAOToGathered(&NoSuccessfulNormals, Assigned, Skipped, Details);
	TestEqual(TEXT("No assignment for unbaked normals"), Assigned, 0);
	Tab->Physical.QualitySamplesPerTexel = 64;
	Tab->OnBake();
	TestTrue(TEXT("Bake completed"), Tab->bHasDiagnostics);
	const auto HasAOOverride = [AO](UMaterialInstanceConstant* MI)
	{
		return MI->TextureParameterValues.ContainsByPredicate([AO](const FTextureParameterValue& Value)
		{
			return Value.ParameterInfo.Name == TEXT("AO1") && Value.ParameterValue == AO;
		});
	};
	TestTrue(TEXT("Bake + Assign writes selected MI AO override"), HasAOOverride(SelectedMI));
	TestTrue(TEXT("Auto assignment resolves tex2 to tex2_ao"), SelectedMI->TextureParameterValues.ContainsByPredicate(
		[AO](const FTextureParameterValue& Value) { return Value.ParameterInfo.Name == TEXT("tex2_ao") && Value.ParameterValue == AO; }));
	TestFalse(TEXT("Unselected material remains unchanged"), HasAOOverride(OtherMI));
	TestFalse(TEXT("Assigned MI saved"), SelectedMI->GetOutermost()->IsDirty());

	Tab->MasterFilter = TEXT("DoesNotMatch");
	Tab->OnGather();
	TestEqual(TEXT("Master filter still applies"), Tab->FoundInstances.Num(), 0);
	TArray<TWeakObjectPtr<UTexture2D>> FilteredNormals;
	TArray<TWeakObjectPtr<UMaterialInstanceConstant>> FilteredInstances;
	FString FilterSummary;
	Tab->GatherFromSelection(FilteredNormals, FilteredInstances, FilterSummary);
	TestTrue(TEXT("Filter rejection is explained"), FilterSummary.Contains(TEXT("Master filter excluded 1")));
	Tab->MasterFilter.Reset();
	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(PoolOwner, true, false);
	Tab->OnGather();
	TestEqual(TEXT("Ordinary actor component selection still works"), Tab->FoundInstances.Num(), 2);
	return !HasAnyErrors();
}
#endif
