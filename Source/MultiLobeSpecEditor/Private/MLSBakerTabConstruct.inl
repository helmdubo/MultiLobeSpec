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
				.Text(NSLOCTEXT("MLS", "BakerTitleV15", "Material Micro-Visibility: RG Normal -> Poisson Height -> Orthographic GTAO -> Bake + Assign"))
			]
			MLS_ROW(TEXT("Master filter"),
				SNew(SEditableTextBox).Text_Lambda([this] { return FText::FromString(MasterFilter); })
				.OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type) { MasterFilter = T.ToString(); }))
			MLS_ROW(TEXT("Bake preset"),
				SNew(SButton).Text(this, &SMLSBakerTab::GetBakePresetText).OnClicked(this, &SMLSBakerTab::OnCycleBakePreset))
			MLS_ROW(TEXT("Relief scale / max slope"),
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(.5f)
				[ SNew(SSpinBox<float>).MinValue(.05f).MaxValue(8.f).Value_Lambda([this] { return Settings.ReliefScale; }).OnValueChanged_Lambda([this](float V) { MarkBakePresetCustom(); Settings.ReliefScale = V; }) ]
				+ SHorizontalBox::Slot().FillWidth(.5f)
				[ SNew(SSpinBox<float>).MinValue(.5f).MaxValue(32.f).Value_Lambda([this] { return Settings.MaxSlope; }).OnValueChanged_Lambda([this](float V) { MarkBakePresetCustom(); Settings.MaxSlope = V; }) ])
			MLS_ROW(TEXT("Radius / slices / steps"),
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(.34f)
				[ SNew(SSpinBox<int32>).MinValue(2).MaxValue(128).Value_Lambda([this] { return Settings.RadiusPx; }).OnValueChanged_Lambda([this](int32 V) { MarkBakePresetCustom(); Settings.RadiusPx = V; }) ]
				+ SHorizontalBox::Slot().FillWidth(.33f)
				[ SNew(SSpinBox<int32>).MinValue(2).MaxValue(32).Value_Lambda([this] { return Settings.Slices; }).OnValueChanged_Lambda([this](int32 V) { MarkBakePresetCustom(); Settings.Slices = V; }) ]
				+ SHorizontalBox::Slot().FillWidth(.33f)
				[ SNew(SSpinBox<int32>).MinValue(2).MaxValue(24).Value_Lambda([this] { return Settings.StepsPerSide; }).OnValueChanged_Lambda([this](int32 V) { MarkBakePresetCustom(); Settings.StepsPerSide = V; }) ])
			MLS_ROW(TEXT("Normal / AO suffix"),
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(.5f)[ SNew(SEditableTextBox).Text_Lambda([this] { return FText::FromString(NormalSuffix); }).OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type) { NormalSuffix = T.ToString(); }) ]
				+ SHorizontalBox::Slot().FillWidth(.5f)[ SNew(SEditableTextBox).Text_Lambda([this] { return FText::FromString(AOSuffix); }).OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type) { AOSuffix = T.ToString(); }) ])
			MLS_ROW(TEXT("AO parameter names"),
				SNew(SEditableTextBox).Text_Lambda([this] { return FText::FromString(AOParamNames); }).OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type) { AOParamNames = T.ToString(); }))
			MLS_ROW(TEXT("Assign immediately after bake"),
				SNew(SCheckBox).IsChecked_Lambda([this] { return bAssignAfterBake ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }).OnCheckStateChanged_Lambda([this](ECheckBoxState S) { bAssignAfterBake = S == ECheckBoxState::Checked; }))
			+ SVerticalBox::Slot().AutoHeight().Padding(4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(.45f)[ SNew(SButton).Text(NSLOCTEXT("MLS", "Gather", "Find From Selection")).OnClicked(this, &SMLSBakerTab::OnGather) ]
				+ SHorizontalBox::Slot().FillWidth(.55f)[ SNew(SButton).IsEnabled_Lambda([this] { return !FoundNormals.IsEmpty(); }).Text_Lambda([this] { return FText::FromString(FString::Printf(TEXT("Bake%s (%d)"), bAssignAfterBake ? TEXT(" + Assign") : TEXT(""), FoundNormals.Num())); }).OnClicked(this, &SMLSBakerTab::OnBake) ]
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
