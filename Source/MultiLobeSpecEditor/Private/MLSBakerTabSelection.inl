bool SMLSBakerTab::PassesMasterFilter(const UMaterialInstanceConstant* Instance) const
{
	if (MasterFilter.TrimStartAndEnd().IsEmpty()) return true;
	// UMaterialInterface::GetBaseMaterial() is not const-qualified in UE 5.7, so a
	// const instance pointer cannot call it directly. Both call sites own a
	// non-const instance; the cast only restores the caller's actual constness.
	const UMaterial* Base = Instance
		? const_cast<UMaterialInstanceConstant*>(Instance)->GetBaseMaterial()
		: nullptr;
	if (!Base) return false;
	TArray<FString> Filters;
	MasterFilter.ParseIntoArray(Filters, TEXT(","), true);
	for (FString& Filter : Filters)
	{
		Filter.TrimStartAndEndInline();
		if (!Filter.IsEmpty() && Base->GetName().Contains(Filter)) return true;
	}
	return false;
}

void SMLSBakerTab::GatherFromSelection(
	TArray<TWeakObjectPtr<UTexture2D>>& OutNormals,
	TArray<TWeakObjectPtr<UMaterialInstanceConstant>>& OutInstances,
	FString& OutSummary) const
{
	OutNormals.Reset();
	OutInstances.Reset();
	TArray<UMaterialInterface*> Materials;

	for (UObject* Object : UEditorUtilityLibrary::GetSelectedAssets())
	{
		if (UTexture2D* Texture = Cast<UTexture2D>(Object))
		{
			if (Texture->GetName().EndsWith(NormalSuffix, ESearchCase::IgnoreCase)) OutNormals.AddUnique(Texture);
		}
		else if (UStaticMesh* Mesh = Cast<UStaticMesh>(Object))
		{
			for (const FStaticMaterial& Entry : Mesh->GetStaticMaterials())
			{
				if (Entry.MaterialInterface) Materials.AddUnique(Entry.MaterialInterface);
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
					for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
					{
						if (UMaterialInterface* Material = Component->GetMaterial(Slot)) Materials.AddUnique(Material);
					}
				}
			}
		}
	}

	for (UMaterialInterface* Material : Materials)
	{
		UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Material);
		if (!Instance || !PassesMasterFilter(Instance)) continue;
		OutInstances.AddUnique(Instance);
		TArray<UTexture*> Textures;
		Material->GetUsedTextures(Textures);
		for (UTexture* Texture : Textures)
		{
			UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
			if (Texture2D && Texture2D->GetName().EndsWith(NormalSuffix, ESearchCase::IgnoreCase)) OutNormals.AddUnique(Texture2D);
		}
	}

	Algo::Sort(OutNormals, [](const TWeakObjectPtr<UTexture2D>& A, const TWeakObjectPtr<UTexture2D>& B)
	{
		return A.IsValid() && B.IsValid() ? A->GetPathName() < B->GetPathName() : A.IsValid();
	});
	Algo::Sort(OutInstances, [](const TWeakObjectPtr<UMaterialInstanceConstant>& A, const TWeakObjectPtr<UMaterialInstanceConstant>& B)
	{
		return A.IsValid() && B.IsValid() ? A->GetPathName() < B->GetPathName() : A.IsValid();
	});
	OutSummary = FString::Printf(TEXT("Found %d normal map(s) and %d material instance(s)."), OutNormals.Num(), OutInstances.Num());
}

FReply SMLSBakerTab::OnGather()
{
	FString Summary;
	GatherFromSelection(FoundNormals, FoundInstances, Summary);
	RefreshFoundNormalList();
	if (StatusText.IsValid()) StatusText->SetText(FText::FromString(Summary));
	return FReply::Handled();
}

void SMLSBakerTab::RemoveNormalFromBakeList(TWeakObjectPtr<UTexture2D> Normal)
{
	FoundNormals.RemoveAll([Normal](const TWeakObjectPtr<UTexture2D>& Item) { return Item == Normal; });
	RefreshFoundNormalList();
}

void SMLSBakerTab::RefreshFoundNormalList()
{
	if (!ListRowsWidget.IsValid()) return;
	ListRowsWidget->ClearChildren();
	if (FoundNormals.IsEmpty())
	{
		ListRowsWidget->AddSlot().AutoHeight()[SNew(STextBlock).Text(NSLOCTEXT("MLS", "NoNormals", "No normal maps found."))];
		return;
	}
	for (const TWeakObjectPtr<UTexture2D> WeakTexture : FoundNormals)
	{
		const FString Label = WeakTexture.IsValid() ? WeakTexture->GetPathName() : TEXT("<invalid texture>");
		ListRowsWidget->AddSlot().AutoHeight().Padding(1)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[SNew(STextBlock).Text(FText::FromString(Label)).ToolTipText(FText::FromString(Label))]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
			[
				SNew(SButton).Text(NSLOCTEXT("MLS", "RemoveBakeItem", "Remove"))
				.OnClicked_Lambda([this, WeakTexture] { RemoveNormalFromBakeList(WeakTexture); return FReply::Handled(); })
			]
		];
	}
}
