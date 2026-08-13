void FMLSRawMaterialVisibilityOverlay::UpdateCapabilityReceipt(const FString& OverlayDir)
{
	const FString Path = OverlayDir / TEXT("MLS_CAPABILITIES.json");
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *Path))
	{
		return;
	}

	Json.ReplaceInline(
		TEXT("\"LegacyRawMaterialVisibility_OverlayPrerequisitesMet\": false"),
		TEXT("\"LegacyRawMaterialVisibility_OverlayPrerequisitesMet\": true"),
		ESearchCase::CaseSensitive);
	Json.ReplaceInline(
		TEXT("\"LegacyRawMaterialVisibility_TransportPatched\": false"),
		TEXT("\"LegacyRawMaterialVisibility_TransportPatched\": true"),
		ESearchCase::CaseSensitive);

	FString IgnoredError;
	SaveAtomic(Path, Json, IgnoredError);
}
