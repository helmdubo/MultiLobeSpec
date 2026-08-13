bool FMLSRawMaterialVisibilityOverlay::PatchGBufferHelpers(
	const FString& OverlayDir,
	FString& OutError)
{
	const FString Path = OverlayDir / TEXT("Private/GBufferHelpers.ush");
	FString Source;
	if (!FFileHelper::LoadFileToString(Source, *Path))
	{
		OutError = FString::Printf(TEXT("Unable to load GBufferHelpers.ush: %s"), *Path);
		return false;
	}
	NormalizeLineEndings(Source);
	if (Source.Contains(GBufferMarker))
	{
		return true;
	}
	EnsureConfigInclude(Source);

	const FString EncodeAnchor =
		TEXT("\t\tGBuffer.GenericAO = float(GBuffer.DiffuseIndirectSampleOcclusion) * (1.0f / 255.0f);");
	const FString EncodeReplacement =
		FString(TEXT("#if MLS_ENABLED && MLS_RAW_MATERIAL_VISIBILITY_TRANSPORT\n"))
		+ TEXT("\t\tGBuffer.GenericAO = GBuffer.GBufferAO; // MLS_GBUFFER_RAW_VISIBILITY_TRANSPORT\n")
		+ TEXT("#else\n") + EncodeAnchor + TEXT("\n#endif");

	const FString DecodeAnchor =
		TEXT("\tRet.DiffuseIndirectSampleOcclusion = 255 * Ret.GenericAO;\n")
		TEXT("\tRet.GBufferAO = saturate(1.0 - float(countbits(Ret.DiffuseIndirectSampleOcclusion)) * rcp(float(INDIRECT_SAMPLE_COUNT)));\n")
		TEXT("\tRet.IndirectIrradiance = 1;");
	const FString DecodeReplacement =
		FString(TEXT("#if MLS_ENABLED && MLS_RAW_MATERIAL_VISIBILITY_TRANSPORT\n"))
		+ TEXT("\tRet.DiffuseIndirectSampleOcclusion = 0x0;\n")
		+ TEXT("\tRet.GBufferAO = Ret.GenericAO;\n")
		+ TEXT("\tRet.IndirectIrradiance = 1;\n")
		+ TEXT("#else\n") + DecodeAnchor + TEXT("\n#endif");

	const int32 EncodeCount = CountExact(Source, EncodeAnchor);
	const int32 DecodeCount = CountExact(Source, DecodeAnchor);
	if (EncodeCount != 1 || DecodeCount != 1)
	{
		OutError = FString::Printf(
			TEXT("GBuffer raw-visibility exact-count failure: encode %d, decode %d."),
			EncodeCount,
			DecodeCount);
		return false;
	}

	Source.ReplaceInline(*EncodeAnchor, *EncodeReplacement, ESearchCase::CaseSensitive);
	Source.ReplaceInline(*DecodeAnchor, *DecodeReplacement, ESearchCase::CaseSensitive);
	return SaveAtomic(Path, Source, OutError);
}
