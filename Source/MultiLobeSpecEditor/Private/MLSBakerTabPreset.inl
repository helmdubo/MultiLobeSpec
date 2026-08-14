FReply SMLSBakerTab::OnCycleBakePreset()
{
	switch (BakePreset)
	{
	case EMLSBakePreset::DebrisDeep: ApplyBakePreset(EMLSBakePreset::StoneBrickMedium); break;
	case EMLSBakePreset::StoneBrickMedium: ApplyBakePreset(EMLSBakePreset::SandEarthShallow); break;
	case EMLSBakePreset::SandEarthShallow: ApplyBakePreset(EMLSBakePreset::Custom); break;
	default: ApplyBakePreset(EMLSBakePreset::DebrisDeep); break;
	}
	return FReply::Handled();
}

void SMLSBakerTab::ApplyBakePreset(EMLSBakePreset Preset)
{
	// Presets set only the physical material values (Height/Radius/Quality).
	// Surface Size stays the user's: it belongs to the asset's real texel
	// density and UV tiling, not to a material category.
	MLS_ApplyPhysicalBakePreset(Preset, Physical);
	BakePreset = Preset;
}

void SMLSBakerTab::MarkBakePresetCustom()
{
	BakePreset = EMLSBakePreset::Custom;
}

FText SMLSBakerTab::GetBakePresetText() const
{
	return FText::FromString(MLS_BakePresetName(BakePreset));
}

FText SMLSBakerTab::GetDiagnosticsText() const
{
	if (!bHasDiagnostics)
	{
		return NSLOCTEXT("MLS", "BakerDiagEmpty", "Diagnostics: bake to see the calibration report.");
	}
	FString Text = FString::Printf(
		TEXT("Diagnostics (last bake):\n")
		TEXT("  Normal-implied height: %.2f cm | Applied scale: %.2fx\n")
		TEXT("  Texture pitch: %.4f cm/texel | Radius: %.1f texels\n")
		TEXT("  Sampling: %d slices x %d steps = %d samples/texel\n")
		TEXT("  Solver: %s | Boundary: %s | Clamped slopes: %.2f%%"),
		LastDiagnostics.NormalImpliedHeightCm,
		LastDiagnostics.AppliedReliefScale,
		LastDiagnostics.CmPerTexel,
		LastDiagnostics.RadiusTexels,
		LastDiagnostics.Slices,
		LastDiagnostics.StepsPerSide,
		LastDiagnostics.ActualSamplesPerTexel,
		*LastDiagnostics.SolverName,
		*LastDiagnostics.BoundaryName,
		LastDiagnostics.ClampedSlopeFraction * 100.0f);
	for (const FString& Warning : LastDiagnostics.Warnings)
	{
		Text += FString::Printf(TEXT("\n  WARN: %s"), *Warning);
	}
	return FText::FromString(Text);
}
