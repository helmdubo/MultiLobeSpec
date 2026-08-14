void SMLSBakerTab::Construct(const FArguments& Args)
{
	ApplyBakePreset(EMLSBakePreset::StoneBrickMedium);
	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(6)
			[
				SNew(STextBlock).AutoWrapText(true)
				.Text(NSLOCTEXT("MLS", "BakerTitleV16", "Material Visibility Baker: physically calibrated RG Normal -> Poisson Height -> Orthographic GTAO"))
			]
			MLS_ROW(TEXT("Master filter"),
				SNew(SEditableTextBox).Text_Lambda([this] { return FText::FromString(MasterFilter); })
				.OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type) { MasterFilter = T.ToString(); }))
			MLS_ROW(TEXT("Bake preset"),
				SNew(SButton).Text(this, &SMLSBakerTab::GetBakePresetText).OnClicked(this, &SMLSBakerTab::OnCycleBakePreset))
			MLS_ROW(TEXT("Surface Size (cm, one UV tile)"),
				SNew(SSpinBox<float>).MinValue(10.f).MaxValue(2000.f)
				.Value_Lambda([this] { return Physical.SurfaceSizeCm; })
				.OnValueChanged_Lambda([this](float V) { Physical.SurfaceSizeCm = V; }))
			MLS_ROW(TEXT("Relief Height (cm, P1..P99)"),
				SNew(SSpinBox<float>).MinValue(0.05f).MaxValue(50.f)
				.Value_Lambda([this] { return Physical.ReliefHeightCm; })
				.OnValueChanged_Lambda([this](float V) { MarkBakePresetCustom(); Physical.ReliefHeightCm = V; }))
			MLS_ROW(TEXT("Occlusion Radius (cm)"),
				SNew(SSpinBox<float>).MinValue(0.1f).MaxValue(100.f)
				.Value_Lambda([this] { return Physical.OcclusionRadiusCm; })
				.OnValueChanged_Lambda([this](float V) { MarkBakePresetCustom(); Physical.OcclusionRadiusCm = V; }))
			MLS_ROW(TEXT("Quality (samples / texel)"),
				SNew(SSpinBox<int32>).MinValue(64).MaxValue(1024)
				.Value_Lambda([this] { return Physical.QualitySamplesPerTexel; })
				.OnValueChanged_Lambda([this](int32 V) { MarkBakePresetCustom(); Physical.QualitySamplesPerTexel = V; }))
			MLS_ROW(TEXT("DirectX normal (green down)"),
				SNew(SCheckBox)
				.IsChecked_Lambda([this] { return Physical.bDirectXNormal ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { Physical.bDirectXNormal = S == ECheckBoxState::Checked; }))
			MLS_ROW(TEXT("Normal / AO suffix"),
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(.5f)[ SNew(SEditableTextBox).Text_Lambda([this] { return FText::FromString(NormalSuffix); }).OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type) { NormalSuffix = T.ToString(); }) ]
				+ SHorizontalBox::Slot().FillWidth(.5f)[ SNew(SEditableTextBox).Text_Lambda([this] { return FText::FromString(AOSuffix); }).OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type) { AOSuffix = T.ToString(); }) ])
			MLS_ROW(TEXT("AO parameter names"),
				SNew(SEditableTextBox).Text_Lambda([this] { return FText::FromString(AOParamNames); }).OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type) { AOParamNames = T.ToString(); }))
			MLS_ROW(TEXT("Assign immediately after bake"),
				SNew(SCheckBox).IsChecked_Lambda([this] { return bAssignAfterBake ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }).OnCheckStateChanged_Lambda([this](ECheckBoxState S) { bAssignAfterBake = S == ECheckBoxState::Checked; }))
			MLS_ROW(TEXT("Write height / gradient-error debug"),
				SNew(SCheckBox).IsChecked_Lambda([this] { return Physical.bWriteDebugTextures ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }).OnCheckStateChanged_Lambda([this](ECheckBoxState S) { Physical.bWriteDebugTextures = S == ECheckBoxState::Checked; }))
			+ SVerticalBox::Slot().AutoHeight().Padding(4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(.45f)[ SNew(SButton).Text(NSLOCTEXT("MLS", "Gather", "Find From Selection")).OnClicked(this, &SMLSBakerTab::OnGather) ]
				+ SHorizontalBox::Slot().FillWidth(.55f)[ SNew(SButton).IsEnabled_Lambda([this] { return !FoundNormals.IsEmpty(); }).Text_Lambda([this] { return FText::FromString(FString::Printf(TEXT("Bake%s (%d)"), bAssignAfterBake ? TEXT(" + Assign") : TEXT(""), FoundNormals.Num())); }).OnClicked(this, &SMLSBakerTab::OnBake) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(6, 2)
			[
				// Read-only calibration report of the last baked texture; never an input.
				SNew(STextBlock).AutoWrapText(true).Text(this, &SMLSBakerTab::GetDiagnosticsText)
			]
			+ SVerticalBox::Slot().AutoHeight().MaxHeight(240.f).Padding(4)
			[
				SNew(SBorder).Padding(4)[ SNew(SScrollBox) + SScrollBox::Slot()[ SAssignNew(ListRowsWidget, SVerticalBox) ] ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(6)
			[
				SAssignNew(StatusText, STextBlock).AutoWrapText(true).Text(NSLOCTEXT("MLS", "BakerReady", "Ready."))
			]
		]
	];
	RefreshFoundNormalList();
}
