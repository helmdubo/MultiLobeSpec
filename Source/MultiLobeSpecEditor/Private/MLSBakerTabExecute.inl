FReply SMLSBakerTab::OnBake()
{
	if (FoundNormals.IsEmpty()) return FReply::Handled();

	FScopedSlowTask Task(FoundNormals.Num(), NSLOCTEXT("MLS", "BakingP2V15", "Generating material visibility..."));
	Task.MakeDialog(true);

	int32 SuccessCount = 0;
	TSet<UTexture2D*> SuccessfulNormals;
	FString Details;
	for (const TWeakObjectPtr<UTexture2D>& WeakTexture : FoundNormals)
	{
		if (Task.ShouldCancel()) break;
		UTexture2D* Texture = WeakTexture.Get();
		Task.EnterProgressFrame(1.0f, FText::FromString(Texture ? Texture->GetName() : TEXT("<invalid>")));
		FString Message;
		if (Texture && FMLSBakerCore::BakeAOForNormal(Texture, Settings, NormalSuffix, AOSuffix, Message))
		{
			++SuccessCount;
			SuccessfulNormals.Add(Texture);
		}
		Details += Message + LINE_TERMINATOR;
	}

	int32 AssignmentCount = 0;
	int32 AssignmentSkipped = 0;
	if (bAssignAfterBake && !SuccessfulNormals.IsEmpty())
	{
		FString AssignmentDetails;
		AssignBakedAOToGathered(&SuccessfulNormals, AssignmentCount, AssignmentSkipped, AssignmentDetails);
		Details += AssignmentDetails;
	}

	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::FromString(FString::Printf(
			TEXT("Preset: %s\nGenerated: %d/%d | Applied: %d | Skipped: %d\n%s"),
			MLS_BakePresetName(BakePreset), SuccessCount, FoundNormals.Num(), AssignmentCount, AssignmentSkipped, *Details)));
	}
	return FReply::Handled();
}
