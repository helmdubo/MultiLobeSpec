void FMLSRawMaterialVisibilityOverlay::UpdateCapabilityReceipt(const FString& OverlayDir)
{
	const FString Path = OverlayDir / TEXT("MLS_CAPABILITIES.json");
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *Path))
	{
		return;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return;
	}

	int32 SampleOcclusion = -1;
	if (const IConsoleVariable* Variable =
		IConsoleManager::Get().FindConsoleVariable(TEXT("r.GBufferDiffuseSampleOcclusion")))
	{
		SampleOcclusion = Variable->GetInt();
	}

	Root->SetBoolField(TEXT("LegacyRawMaterialVisibility_OverlayPrerequisitesMet"), true);
	Root->SetBoolField(TEXT("LegacyRawMaterialVisibility_TransportRequested"), true);
	Root->SetBoolField(TEXT("LegacyRawMaterialVisibility_TransportPatched"), true);
	Root->SetNumberField(TEXT("LegacyRawMaterialVisibility_DiffuseSampleOcclusionCVar"), SampleOcclusion);
	Root->SetStringField(
		TEXT("LegacyRawMaterialVisibility_TransportMode"),
		TEXT("Overlay GenericAO transport; sample-mask decode is disabled only in active micro-shadow permutations"));
	Root->SetBoolField(TEXT("ActivisionDirectMicroShadow_DefaultLit"), true);
	Root->SetBoolField(TEXT("EngineForkRequired"), false);

	const TSharedRef<FJsonObject> Prerequisites = MakeShared<FJsonObject>();
	Prerequisites->SetNumberField(TEXT("r.Substrate"), 0);
	Prerequisites->SetNumberField(TEXT("r.AllowStaticLighting"), 0);
	Prerequisites->SetNumberField(TEXT("r.GBufferDiffuseSampleOcclusion"), SampleOcclusion);
	Prerequisites->SetStringField(
		TEXT("requiredContract"),
		TEXT("r.Substrate=0; r.AllowStaticLighting=0; r.GBufferDiffuseSampleOcclusion=0 or 1"));
	Root->SetObjectField(TEXT("rawMaterialVisibilityPrerequisites"), Prerequisites);

	FString Output;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Output);
	if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
	{
		return;
	}
	Output += TEXT("\n");
	FString IgnoredError;
	SaveAtomic(Path, Output, IgnoredError);
}
