bool FMLSRawMaterialVisibilityOverlay::PatchBasePassGuard(
	const FString& OverlayDir,
	FString& OutError)
{
	const FString Path = OverlayDir / TEXT("Private/BasePassPixelShader.usf");
	FString Source;
	if (!FFileHelper::LoadFileToString(Source, *Path))
	{
		OutError = FString::Printf(TEXT("Unable to load BasePassPixelShader.usf: %s"), *Path);
		return false;
	}
	NormalizeLineEndings(Source);
	EnsureConfigInclude(Source);

	const FString OldGuard = TEXT("#if GBUFFER_HAS_DIFFUSE_SAMPLE_OCCLUSION || ALLOW_STATIC_LIGHTING || SUBSTRATE_ENABLED");
	const FString NewGuard = TEXT("#if ALLOW_STATIC_LIGHTING || SUBSTRATE_ENABLED // MLS_SAMPLE_OCCLUSION_TRANSPORT_SUPPORTED");

	if (Source.Contains(TransportMarker))
	{
		if (Source.Contains(OldGuard))
		{
			if (CountExact(Source, OldGuard) != 1)
			{
				OutError = TEXT("Existing BasePass transport guard is ambiguous.");
				return false;
			}
			Source.ReplaceInline(*OldGuard, *NewGuard, ESearchCase::CaseSensitive);
		}
		return SaveAtomic(Path, Source, OutError);
	}

	const FString StoreAnchor = TEXT("GBuffer.GBufferAO = AOMultiBounce( Luminance( GBuffer.SpecularColor ), ShadingOcclusion.SpecOcclusion ).g;");
	if (CountExact(Source, StoreAnchor) != 1)
	{
		OutError = TEXT("BasePass raw visibility store exact-count failure.");
		return false;
	}

	const FString Replacement =
		FString(TEXT("#if MLS_ENABLED && MLS_RAW_MATERIAL_VISIBILITY_TRANSPORT\n"))
		+ TEXT("\t#if ALLOW_STATIC_LIGHTING || SUBSTRATE_ENABLED\n")
		+ TEXT("\t\t#error MultiLobeSpec direct micro-shadowing requires legacy deferred without static-lighting AO packing\n")
		+ TEXT("\t#endif\n")
		+ TEXT("\tGBuffer.GBufferAO = MaterialAO; // MLS_RAW_MATERIAL_VISIBILITY_TRANSPORT\n")
		+ TEXT("#else\n\t")
		+ StoreAnchor
		+ TEXT("\n#endif");
	Source.ReplaceInline(*StoreAnchor, *Replacement, ESearchCase::CaseSensitive);
	return SaveAtomic(Path, Source, OutError);
}
