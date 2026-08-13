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
	if (Preset != EMLSBakePreset::Custom) Settings = MLS_MakeBakePreset(Preset);
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

FText SMLSBakerTab::GetReconstructionText() const
{
	switch (Settings.Reconstruction)
	{
	case EMLSHeightReconstruction::PeriodicFFT: return NSLOCTEXT("MLS", "SolverFFT", "Periodic FFT");
	case EMLSHeightReconstruction::Multigrid: return NSLOCTEXT("MLS", "SolverMG", "Multigrid");
	default: return NSLOCTEXT("MLS", "SolverAuto", "Auto");
	}
}

FText SMLSBakerTab::GetBoundaryText() const
{
	switch (Settings.Boundary)
	{
	case EMLSBoundaryMode::Clamp: return NSLOCTEXT("MLS", "BoundaryClamp", "Clamp");
	case EMLSBoundaryMode::Mirror: return NSLOCTEXT("MLS", "BoundaryMirror", "Mirror");
	default: return NSLOCTEXT("MLS", "BoundaryWrap", "Wrap");
	}
}
