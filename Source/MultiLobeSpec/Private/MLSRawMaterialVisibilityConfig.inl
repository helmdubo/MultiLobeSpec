bool PatchConfigDefine(const FString& OverlayDir, FString& OutError)
{
	const FString Path = OverlayDir / TEXT("Private/MultiLobeSpecConfig.ush");
	FString Source;
	if (!FFileHelper::LoadFileToString(Source, *Path))
	{
		OutError = TEXT("Unable to load generated MultiLobeSpecConfig.ush.");
		return false;
	}
	NormalizeLineEndings(Source);

	const FString Enabled = TEXT("#define MLS_RAW_MATERIAL_VISIBILITY_TRANSPORT 1");
	if (Source.Contains(Enabled))
	{
		return true;
	}

	const FString Disabled = TEXT("#define MLS_RAW_MATERIAL_VISIBILITY_TRANSPORT 0");
	if (CountExact(Source, Disabled) != 1)
	{
		OutError = TEXT("Raw visibility config define exact-count failure.");
		return false;
	}
	Source.ReplaceInline(*Disabled, *Enabled, ESearchCase::CaseSensitive);
	return SaveAtomic(Path, Source, OutError);
}
