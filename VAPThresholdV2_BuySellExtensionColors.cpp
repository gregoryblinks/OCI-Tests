// Custom Sierra Chart ACSIL study.
// Derived from the Volume At Price Threshold Alert V2 implementation in the
// user-provided Studies8.cpp. The built-in study is not modified.

#include "sierrachart.h"

SCDLLName("VAP Threshold V2 Buy Sell Extension Colors")

SCSFExport scsf_VolumeAtPriceThresholdAlertV2BuySellExtensionColors(SCStudyInterfaceRef sc)
{
	SCSubgraphRef Subgraph_ExtensionLineProperties = sc.Subgraph[SC_SUBGRAPHS_AVAILABLE - 1];
	SCSubgraphRef Subgraph_CountOfAlerts = sc.Subgraph[SC_SUBGRAPHS_AVAILABLE - 2];

	int AdjacentAlertsHighlightSubgraphStartingNumber = SC_SUBGRAPHS_AVAILABLE - 12;

	SCInputRef Input_ComparisonMethod = sc.Input[0];
	SCInputRef Input_VolumeThreshold = sc.Input[1];
	SCInputRef Input_AlertNumber = sc.Input[2];
	SCInputRef Input_DrawExtensionLines = sc.Input[3];
	SCInputRef Input_DrawExtensionLinesWithTransparentRange = sc.Input[4];
	SCInputRef Input_PercentageThreshold = sc.Input[5];
	SCInputRef Input_AdditionalVolumeThreshold = sc.Input[6];
	SCInputRef Input_AllowZeroValueCompares = sc.Input[7];
	SCInputRef Input_DivideByZeroAction = sc.Input[8];
	SCInputRef Input_HighlightAdjacentAlertsGroupSize = sc.Input[9];
	SCInputRef Input_DrawExtensionLinesUntilEndOfChart = sc.Input[10];
	SCInputRef Input_NumberOfDaysToCalculate = sc.Input[11];
	SCInputRef Input_Version = sc.Input[12];
	SCInputRef Input_MinimumVolumeValueForRatioComparisons = sc.Input[13];

	if (sc.SetDefaults)
	{
		// Set the configuration and defaults
		sc.GraphName = "Volume At Price Threshold Alert V2 - Buy/Sell Extension Colors";
		sc.StudyDescription = "Based on Volume At Price Threshold Alert V2. Extension lines use the SG60 primary color when Ask Volume is greater than or equal to Bid Volume, and the SG60 secondary color when Bid Volume is greater. Adjacent-alert groups use summed Ask and Bid Volume for the full group.";

		sc.GraphRegion = 0;		
		sc.AutoLoop = 0;//Manual looping
		sc.ValueFormat = sc.BaseGraphValueFormat;		

		sc.MaintainVolumeAtPriceData = 1;  // true
	
		for (int SubgraphIndex = 0; SubgraphIndex < SC_SUBGRAPHS_AVAILABLE - 13; ++SubgraphIndex)
		{
			SCString SubgraphName;
			SubgraphName.Format("Trigger %d", SubgraphIndex);

			sc.Subgraph[SubgraphIndex].Name = SubgraphName;
			sc.Subgraph[SubgraphIndex].PrimaryColor = RGB(255, 128, 0);
			sc.Subgraph[SubgraphIndex].DrawStyle = DRAWSTYLE_SQUARE_OFFSET_LEFT_FOR_CANDLESTICK;
			sc.Subgraph[SubgraphIndex].LineWidth = 8;
			sc.Subgraph[SubgraphIndex].DrawZeros = 0;
			sc.Subgraph[SubgraphIndex].DisplayNameValueInWindowsFlags = 0;
		}

		Subgraph_CountOfAlerts.Name = "Count of Alerts";
		Subgraph_CountOfAlerts.DrawStyle = DRAWSTYLE_IGNORE;
		Subgraph_CountOfAlerts.PrimaryColor = RGB(0, 255, 0);
		Subgraph_CountOfAlerts.DrawZeros = 1;

		for (int SubgraphIndex = 0; SubgraphIndex < 10; ++SubgraphIndex)
		{
			SCString SubgraphName;
			SubgraphDrawStyles AdjacentAlertsDrawStyle;
			if (SubgraphIndex % 2 == 0)
			{
				SubgraphName.Format("Adjacent Alert Highlight Bottom %d", static_cast<int>(SubgraphIndex / 2) + 1);
				AdjacentAlertsDrawStyle = DRAWSTYLE_LEFT_OFFSET_BOX_TOP_FOR_CANDLESTICK;
			}
			else
			{
				SubgraphName.Format("Adjacent Alert Highlight Top %d", static_cast<int>(SubgraphIndex / 2) + 1);
				AdjacentAlertsDrawStyle = DRAWSTYLE_LEFT_OFFSET_BOX_BOTTOM_FOR_CANDLESTICK;
			}

			sc.Subgraph[AdjacentAlertsHighlightSubgraphStartingNumber + SubgraphIndex].Name = SubgraphName;
			sc.Subgraph[AdjacentAlertsHighlightSubgraphStartingNumber + SubgraphIndex].PrimaryColor = RGB(255, 255, 0);
			sc.Subgraph[AdjacentAlertsHighlightSubgraphStartingNumber + SubgraphIndex].DrawStyle = AdjacentAlertsDrawStyle;
			sc.Subgraph[AdjacentAlertsHighlightSubgraphStartingNumber + SubgraphIndex].LineWidth = 8;
			sc.Subgraph[AdjacentAlertsHighlightSubgraphStartingNumber + SubgraphIndex].DisplayNameValueInWindowsFlags = 0;
		}

		Subgraph_ExtensionLineProperties.Name = "Extension Line Properties";
		Subgraph_ExtensionLineProperties.DrawStyle = DRAWSTYLE_SUBGRAPH_NAME_AND_VALUE_LABELS_ONLY;
		Subgraph_ExtensionLineProperties.LineWidth = 1;
		Subgraph_ExtensionLineProperties.PrimaryColor = RGB(0, 255, 0); // Buy-dominant or tied
		Subgraph_ExtensionLineProperties.SecondaryColor = RGB(255, 0, 0); // Sell-dominant
		Subgraph_ExtensionLineProperties.SecondaryColorUsed = true;
		Subgraph_ExtensionLineProperties.DrawZeros = false;
		Subgraph_ExtensionLineProperties.DisplayNameValueInWindowsFlags = 0;

		int DisplayOrder = 1;

		Input_ComparisonMethod.Name = "Comparison Method";
		Input_ComparisonMethod.SetCustomInputStrings("Bid Volume;Ask Volume;Total Volume;Number of Trades;Ask Volume Bid Volume Difference;Ask Volume Bid Volume Diagonal Difference;Ask Volume Bid Volume Ratio;Ask Volume Bid Volume Diagonal Ratio;Bid Volume and Ask Volume Separately");
		Input_ComparisonMethod.SetCustomInputIndex(2);
		Input_ComparisonMethod.DisplayOrder = DisplayOrder++;

		Input_VolumeThreshold.Name = "Volume Threshold";
		Input_VolumeThreshold.SetInt(100);
		Input_VolumeThreshold.DisplayOrder = DisplayOrder++;

		Input_AlertNumber.Name = "Volume Alert Number";
		Input_AlertNumber.SetAlertSoundNumber(0);
		Input_AlertNumber.DisplayOrder = DisplayOrder++;

		Input_DrawExtensionLines.Name = "Draw Extension Lines";
		Input_DrawExtensionLines.SetCustomInputStrings("None;All Alerts;Lowest Price in Adjacent Alerts;Highest Price in Adjacent Alerts;All Prices in Adjacent Alerts");
		Input_DrawExtensionLines.SetCustomInputIndex(0);
		Input_DrawExtensionLines.DisplayOrder = DisplayOrder++;

		Input_DrawExtensionLinesWithTransparentRange.Name = "Draw Extension Lines With Transparent Range";
		Input_DrawExtensionLinesWithTransparentRange.SetYesNo(false);
		Input_DrawExtensionLinesWithTransparentRange.DisplayOrder = DisplayOrder++;

		Input_PercentageThreshold.Name = "Percentage Threshold";
		Input_PercentageThreshold.SetInt(150);
		Input_PercentageThreshold.DisplayOrder = DisplayOrder++;

		Input_MinimumVolumeValueForRatioComparisons.Name = "Minimum Volume Value for Ratio Comparisons";
		Input_MinimumVolumeValueForRatioComparisons.SetInt(0);
		Input_MinimumVolumeValueForRatioComparisons.DisplayOrder = DisplayOrder++;
		
		Input_AdditionalVolumeThreshold.Name = "Additional Volume Threshold";
		Input_AdditionalVolumeThreshold.SetInt(100);
		Input_AdditionalVolumeThreshold.DisplayOrder = DisplayOrder++;

		Input_AllowZeroValueCompares.Name = "Enable Zero Bid/Ask Compares";
		Input_AllowZeroValueCompares.SetYesNo(0);
		Input_AllowZeroValueCompares.DisplayOrder = DisplayOrder++;

		Input_DivideByZeroAction.Name = "Zero Value Compare Action";
		Input_DivideByZeroAction.SetCustomInputStrings("Set 0 to 1;Set Percentage to +/- 1000%");
		Input_DivideByZeroAction.SetCustomInputIndex(0);
		Input_DivideByZeroAction.DisplayOrder = DisplayOrder++;

		Input_HighlightAdjacentAlertsGroupSize.Name = "Highlight Adjacent Alerts Minimum Group Size";
		Input_HighlightAdjacentAlertsGroupSize.SetInt(0);
		Input_HighlightAdjacentAlertsGroupSize.DisplayOrder = DisplayOrder++;

		Input_DrawExtensionLinesUntilEndOfChart.Name = "Draw Extension Lines until End of Chart";
		Input_DrawExtensionLinesUntilEndOfChart.SetYesNo(false);
		Input_DrawExtensionLinesUntilEndOfChart.DisplayOrder++;

		Input_NumberOfDaysToCalculate.Name = "Number of Days to Calculate";
		Input_NumberOfDaysToCalculate.SetInt(30);
		Input_NumberOfDaysToCalculate.SetIntLimits(1, 10000);
		Input_NumberOfDaysToCalculate.DisplayOrder = DisplayOrder++;

		Input_Version.SetInt(2);

		sc.ValueFormat = VALUEFORMAT_INHERITED;

		return;
	}

	const int TransparencyLevel = sc.GetChartStudyTransparencyLevel(sc.ChartNumber, sc.StudyGraphInstanceID);

	if (Input_Version.GetInt() < 1)
	{
		Input_Version.SetInt(1);
		Input_NumberOfDaysToCalculate.SetInt(30);
	}

	if (Input_Version.GetInt() < 2)
	{
		Input_Version.SetInt(2);
		if (Input_DrawExtensionLines.GetYesNo() == false)
			Input_DrawExtensionLines.SetCustomInputIndex(0);
		else
			Input_DrawExtensionLines.SetCustomInputIndex(1);
	}
			
	//This is an indication that the volume at price data does not exist
	if (static_cast<int>(sc.VolumeAtPriceForBars->GetNumberOfBars()) < sc.ArraySize)
		return;

	//The Subgraph display properties need to be the same for all Subgraphs. If the properties at Subgraph index 1 are different than at subgraph index 0, then apply Subgraph 0 properties to the rest.

	if (sc.Subgraph[1].PrimaryColor != sc.Subgraph[0].PrimaryColor
		|| sc.Subgraph[1].DrawStyle != sc.Subgraph[0].DrawStyle
		|| sc.Subgraph[1].LineWidth != sc.Subgraph[0].LineWidth
	)
	{
		for (int SubgraphIndex = 1; SubgraphIndex < SC_SUBGRAPHS_AVAILABLE - 13; ++SubgraphIndex)
		{
			sc.Subgraph[SubgraphIndex].PrimaryColor = sc.Subgraph[0].PrimaryColor;
			sc.Subgraph[SubgraphIndex].DrawStyle = sc.Subgraph[0].DrawStyle;
			sc.Subgraph[SubgraphIndex].LineWidth = sc.Subgraph[0].LineWidth;
		}
	}

	bool EnableAlerts = sc.IsFullRecalculation == 0 && !sc.ChartIsDownloadingHistoricalData(sc.ChartNumber);

	if (Input_HighlightAdjacentAlertsGroupSize.GetInt() == 0)
	{
		for (int SubgraphIndex = 0; SubgraphIndex < 10; ++SubgraphIndex)
		{
			sc.Subgraph[AdjacentAlertsHighlightSubgraphStartingNumber + SubgraphIndex].DrawStyle = DRAWSTYLE_IGNORE;
		}

		if (Input_DrawExtensionLines.GetIndex() > 1)
		{
			Input_DrawExtensionLines.SetCustomInputIndex(1);
		}
	}
	else if (Input_HighlightAdjacentAlertsGroupSize.GetInt() != 0)
	{
		bool AllSetToIgnore = true;
		for (int SubgraphIndex = 0; SubgraphIndex < 10; ++SubgraphIndex)
		{
			if (sc.Subgraph[AdjacentAlertsHighlightSubgraphStartingNumber + SubgraphIndex].DrawStyle != DRAWSTYLE_IGNORE)
			{
				AllSetToIgnore = false;
				break;
			}
		}

		if (AllSetToIgnore)
		{
			for (int SubgraphIndex = 0; SubgraphIndex < 10; ++SubgraphIndex)
			{
				if (SubgraphIndex % 2 == 0)
				{
					sc.Subgraph[AdjacentAlertsHighlightSubgraphStartingNumber + SubgraphIndex].DrawStyle = DRAWSTYLE_LEFT_OFFSET_BOX_TOP_FOR_CANDLESTICK;
				}
				else
				{
					sc.Subgraph[AdjacentAlertsHighlightSubgraphStartingNumber + SubgraphIndex].DrawStyle = DRAWSTYLE_LEFT_OFFSET_BOX_BOTTOM_FOR_CANDLESTICK;
				}
			}
		}
	}

	const int HighlightAdjacentAlertsGroupSizeValue = Input_HighlightAdjacentAlertsGroupSize.GetInt();

	SCDateTimeMS StartDateTimeForCalculations = sc.BaseDateTimeIn[sc.ArraySize - 1];
	StartDateTimeForCalculations.SubtractDays(Input_NumberOfDaysToCalculate.GetInt());
	StartDateTimeForCalculations = sc.GetStartOfPeriodForDateTime(StartDateTimeForCalculations, TIME_PERIOD_LENGTH_UNIT_DAYS , 1, 0);

	int LineID = 0;

	for (int BarIndex = sc.UpdateStartIndex; BarIndex < sc.ArraySize; BarIndex++)
	{
		if (sc.BaseDateTimeIn[BarIndex] < StartDateTimeForCalculations)
		{
			continue;
		}

		int AvailableSubgraphIndex = 0;

		Subgraph_CountOfAlerts[BarIndex] = 0;

		bool AdjacentAlertsHaveBottomHighlightAlertPrice = false;
		int AdjacentAlertsHighlightBottomPriceIndex = 0;
		bool AdjacentAlertsGotGroup = false;
		int AdjacentAlertsGroupNumber = -1;
		int AdjacentAlertsCountInGroup = 0;

		//Reset all subgraph values
		for (int SubgraphIndex = 0; SubgraphIndex < SC_SUBGRAPHS_AVAILABLE - 2; ++SubgraphIndex)
			sc.Subgraph[SubgraphIndex].Data[BarIndex] = 0;

		bool GotExtensionLineForGroup = false;
		float AdjacentAlertsHighlightBottomPrice = 0.0f;
		double AdjacentAlertsGroupAskVolume = 0.0;
		double AdjacentAlertsGroupBidVolume = 0.0;
		int AdjacentAlertsGroupFirstLineID = -1;
		int AdjacentAlertsGroupLineCount = 0;

		int NumberOfPricesAtBarIndex = sc.VolumeAtPriceForBars->GetSizeAtBarIndex(BarIndex);

		for (int PriceIndex = 0; PriceIndex < NumberOfPricesAtBarIndex; ++PriceIndex)
		{
			s_VolumeAtPriceV2 *p_VolumeAtPrice = nullptr;

			if (!sc.VolumeAtPriceForBars->GetVAPElementAtIndex(BarIndex, PriceIndex, &p_VolumeAtPrice))
				continue;

			s_VolumeAtPriceV2 *p_NextVolumeAtPrice = nullptr;

			if (PriceIndex < NumberOfPricesAtBarIndex - 1)
			{
				sc.VolumeAtPriceForBars->GetVAPElementAtIndex(BarIndex, PriceIndex + 1, &p_NextVolumeAtPrice);
			}

			float Price = p_VolumeAtPrice->PriceInTicks * sc.TickSize;
			float PriceForExtensionLine = Price;
			s_VolumeAtPriceV2* p_ExtensionLineVolumeAtPrice = p_VolumeAtPrice;
			s_VolumeAtPriceV2* p_GroupVolumeAtPrice = p_VolumeAtPrice;

			// Check if condition has been met
			int ComparisonMethodIndex = Input_ComparisonMethod.GetIndex();
			bool ConditionMet = false;
			unsigned int VolumeThresholdValue = Input_VolumeThreshold.GetInt();
			unsigned int AdditionalVolumeThresholdValue = Input_AdditionalVolumeThreshold.GetInt();
			unsigned int MinimumVolumeValueForRatioComparisons = Input_MinimumVolumeValueForRatioComparisons.GetInt();

			if (ComparisonMethodIndex == 0)//Bid Volume
			{
				if ((VolumeThresholdValue > 0
					&& p_VolumeAtPrice->GetBidVolume() >= VolumeThresholdValue)
					|| (VolumeThresholdValue == 0 && p_VolumeAtPrice->GetBidVolume() == 0)
					)
					ConditionMet = true;

			}
			else if (ComparisonMethodIndex == 1)//Ask Volume
			{
				if (( VolumeThresholdValue > 0
					&& p_VolumeAtPrice->GetAskVolume() >= VolumeThresholdValue)
					|| (VolumeThresholdValue == 0 && p_VolumeAtPrice->GetAskVolume() == 0)
					)
					ConditionMet = true;
			}
			else if (ComparisonMethodIndex == 2)//Total Volume
			{
				if ((VolumeThresholdValue > 0
					&& p_VolumeAtPrice->GetVolume() >= VolumeThresholdValue)
					|| (VolumeThresholdValue == 0 && p_VolumeAtPrice->GetVolume() == 0)
					)
					ConditionMet = true;
			}
			else if (ComparisonMethodIndex == 3)//Number of Trades
			{
				if ((VolumeThresholdValue > 0
					&& p_VolumeAtPrice->NumberOfTrades >= VolumeThresholdValue)
					|| (VolumeThresholdValue == 0 && p_VolumeAtPrice->NumberOfTrades == 0)
					)
					ConditionMet = true;
			}
			else if (ComparisonMethodIndex == 4)//Ask Volume Bid Volume Difference
			{
				double AskVolumeBidVolumeDifference = p_VolumeAtPrice->GetAskVolume() - p_VolumeAtPrice->GetBidVolume();

				int VolumeThresholdSigned = Input_VolumeThreshold.GetInt();

				if (AskVolumeBidVolumeDifference > 0 && VolumeThresholdSigned > 0 && AskVolumeBidVolumeDifference >= VolumeThresholdSigned)
					ConditionMet = true;
				else if (AskVolumeBidVolumeDifference < 0 && VolumeThresholdSigned < 0 && AskVolumeBidVolumeDifference <= VolumeThresholdSigned)
					ConditionMet = true;

			}
			else if (ComparisonMethodIndex == 5)//Ask Volume Bid Volume Diagonal Difference
			{
				double AskVolumeBidVolumeDifference = 0;

				if (p_NextVolumeAtPrice != nullptr)
				{
					AskVolumeBidVolumeDifference = p_NextVolumeAtPrice->GetAskVolume() - p_VolumeAtPrice->GetBidVolume();
					if (p_NextVolumeAtPrice->GetAskVolume() > p_VolumeAtPrice->GetBidVolume())
					{
						PriceForExtensionLine = p_NextVolumeAtPrice->PriceInTicks * sc.TickSize;
						p_ExtensionLineVolumeAtPrice = p_NextVolumeAtPrice;
					}
				}

				int VolumeThresholdSigned = Input_VolumeThreshold.GetInt();

				if (AskVolumeBidVolumeDifference > 0 && VolumeThresholdSigned > 0 && AskVolumeBidVolumeDifference >= VolumeThresholdSigned)
					ConditionMet = true;
				else if (AskVolumeBidVolumeDifference < 0 && VolumeThresholdSigned < 0 && AskVolumeBidVolumeDifference <= VolumeThresholdSigned)
					ConditionMet = true;

			}
			else if (ComparisonMethodIndex == 6)//Ask Volume Bid Volume Ratio
			{
				bool AllowZeroValueComparesSetting = Input_AllowZeroValueCompares.GetYesNo();
				unsigned int DivideByZeroActionIndex = Input_DivideByZeroAction.GetIndex();
				int AskVolumeBidVolumeRatioPercent = 0;

				if (((p_VolumeAtPrice->GetAskVolume() > 0 && p_VolumeAtPrice->GetBidVolume() > 0) || AllowZeroValueComparesSetting) && p_VolumeAtPrice->GetVolume() >= MinimumVolumeValueForRatioComparisons)
				{
					if (p_VolumeAtPrice->GetAskVolume() >= p_VolumeAtPrice->GetBidVolume())
					{
						if (p_VolumeAtPrice->GetBidVolume() == 0 && DivideByZeroActionIndex == 0)
							AskVolumeBidVolumeRatioPercent = static_cast<int>((p_VolumeAtPrice->GetAskVolume() / 1) * 100);
						else if (p_VolumeAtPrice->GetBidVolume() == 0 && DivideByZeroActionIndex == 1)
							AskVolumeBidVolumeRatioPercent = 1000;
						else
							AskVolumeBidVolumeRatioPercent = sc.Round(static_cast<float>(p_VolumeAtPrice->GetAskVolume() / p_VolumeAtPrice->GetBidVolume()) * 100);
					}
					else
					{
						if (p_VolumeAtPrice->GetAskVolume() == 0 && DivideByZeroActionIndex == 0)
							AskVolumeBidVolumeRatioPercent = static_cast<int>((p_VolumeAtPrice->GetBidVolume() / 1) * -100);
						else if (p_VolumeAtPrice->GetAskVolume() == 0 && DivideByZeroActionIndex == 1)
							AskVolumeBidVolumeRatioPercent = -1000;
						else
							AskVolumeBidVolumeRatioPercent = sc.Round(static_cast<float>(p_VolumeAtPrice->GetBidVolume() / p_VolumeAtPrice->GetAskVolume()) * -100);
					}
				}

				int PercentThresholdSigned = Input_PercentageThreshold.GetInt();

				if (AskVolumeBidVolumeRatioPercent > 0 && PercentThresholdSigned > 0 && AskVolumeBidVolumeRatioPercent >= PercentThresholdSigned)
					ConditionMet = true;
				else if (AskVolumeBidVolumeRatioPercent < 0 && PercentThresholdSigned < 0 && AskVolumeBidVolumeRatioPercent <= PercentThresholdSigned)
					ConditionMet = true;
			}
			else if (ComparisonMethodIndex == 7)//Ask Volume Bid Volume Diagonal Ratio
			{
				bool AllowZeroValueComparesSetting = Input_AllowZeroValueCompares.GetYesNo();
				unsigned int DivideByZeroActionIndex = Input_DivideByZeroAction.GetIndex();
				int AskVolumeBidVolumeRatioPercent = 0;

				if (p_NextVolumeAtPrice != nullptr)
				{
					if ((p_NextVolumeAtPrice->GetAskVolume() >= p_VolumeAtPrice->GetBidVolume()) && (p_VolumeAtPrice->GetBidVolume() > 0 || AllowZeroValueComparesSetting)
						&& p_NextVolumeAtPrice->GetVolume() >= MinimumVolumeValueForRatioComparisons && p_VolumeAtPrice->GetVolume() >= MinimumVolumeValueForRatioComparisons)
					{
						if (p_VolumeAtPrice->GetBidVolume() == 0 && DivideByZeroActionIndex == 0)
							AskVolumeBidVolumeRatioPercent = static_cast<int>((p_NextVolumeAtPrice->GetAskVolume() / 1) * 100);
						else if (p_VolumeAtPrice->GetBidVolume() == 0 && DivideByZeroActionIndex == 1)
							AskVolumeBidVolumeRatioPercent = 1000;
						else
							AskVolumeBidVolumeRatioPercent = sc.Round(static_cast<float>(p_NextVolumeAtPrice->GetAskVolume() / p_VolumeAtPrice->GetBidVolume()) * 100);

						Price = p_NextVolumeAtPrice->PriceInTicks * sc.TickSize;
						PriceForExtensionLine = p_NextVolumeAtPrice->PriceInTicks * sc.TickSize;
						p_ExtensionLineVolumeAtPrice = p_NextVolumeAtPrice;
						p_GroupVolumeAtPrice = p_NextVolumeAtPrice;
					}
					else if (p_VolumeAtPrice->GetBidVolume() > p_NextVolumeAtPrice->GetAskVolume() && (p_NextVolumeAtPrice->GetAskVolume() > 0 || AllowZeroValueComparesSetting)
						&& p_VolumeAtPrice->GetVolume() >= MinimumVolumeValueForRatioComparisons && p_NextVolumeAtPrice->GetVolume() >= MinimumVolumeValueForRatioComparisons)
					{
						if (p_NextVolumeAtPrice->GetAskVolume() == 0 && DivideByZeroActionIndex == 0)
							AskVolumeBidVolumeRatioPercent = static_cast<int>((p_VolumeAtPrice->GetBidVolume() / 1) * -100);
						else if (p_NextVolumeAtPrice->GetAskVolume() == 0 && DivideByZeroActionIndex == 1)
							AskVolumeBidVolumeRatioPercent = -1000;
						else
							AskVolumeBidVolumeRatioPercent = sc.Round(static_cast<float>(p_VolumeAtPrice->GetBidVolume() / p_NextVolumeAtPrice->GetAskVolume()) * -100);
					}
				}

				int PercentThresholdSigned = Input_PercentageThreshold.GetInt();

				if (AskVolumeBidVolumeRatioPercent > 0 && PercentThresholdSigned > 0 && AskVolumeBidVolumeRatioPercent >= PercentThresholdSigned)
					ConditionMet = true;
				else if (AskVolumeBidVolumeRatioPercent < 0 && PercentThresholdSigned < 0 && AskVolumeBidVolumeRatioPercent <= PercentThresholdSigned)
					ConditionMet = true;
			}
			else if (ComparisonMethodIndex == 8)//Bid Volume and Ask Volume Separately
			{
				if (
					((VolumeThresholdValue > 0
						&& p_VolumeAtPrice->GetBidVolume() >= VolumeThresholdValue)
						|| (VolumeThresholdValue == 0 && p_VolumeAtPrice->GetBidVolume() == 0))
					&&
					((AdditionalVolumeThresholdValue > 0
						&& p_VolumeAtPrice->GetAskVolume() >= AdditionalVolumeThresholdValue)
						|| (AdditionalVolumeThresholdValue == 0 && p_VolumeAtPrice->GetAskVolume() == 0))
					)
					ConditionMet = true;
			}

			if (ConditionMet)
			{
				Subgraph_CountOfAlerts[BarIndex]++;

				const double SingleLineAskVolume = p_ExtensionLineVolumeAtPrice->GetAskVolume();
				const double SingleLineBidVolume = p_ExtensionLineVolumeAtPrice->GetBidVolume();
				const COLORREF SingleExtensionLineColor =
					SingleLineAskVolume >= SingleLineBidVolume
					? Subgraph_ExtensionLineProperties.PrimaryColor
					: Subgraph_ExtensionLineProperties.SecondaryColor;

				const double CurrentAlertGroupAskVolume = p_GroupVolumeAtPrice->GetAskVolume();
				const double CurrentAlertGroupBidVolume = p_GroupVolumeAtPrice->GetBidVolume();
				
				// Adjacent Alerts Highlight
				if (HighlightAdjacentAlertsGroupSizeValue > 0 && AdjacentAlertsGroupNumber < 5)
				{
					if (AdjacentAlertsHaveBottomHighlightAlertPrice && PriceIndex != AdjacentAlertsHighlightBottomPriceIndex + AdjacentAlertsCountInGroup)
					{
						AdjacentAlertsHaveBottomHighlightAlertPrice = false;
						AdjacentAlertsCountInGroup = 0;
						if (!AdjacentAlertsGotGroup)
						{
							AdjacentAlertsGroupNumber--;
						}
						AdjacentAlertsGotGroup = false;
						GotExtensionLineForGroup = false;
						AdjacentAlertsGroupAskVolume = 0.0;
						AdjacentAlertsGroupBidVolume = 0.0;
						AdjacentAlertsGroupFirstLineID = -1;
						AdjacentAlertsGroupLineCount = 0;
					}

					if (AdjacentAlertsHaveBottomHighlightAlertPrice == false)
					{
						AdjacentAlertsGroupNumber++;
						sc.Subgraph[AdjacentAlertsHighlightSubgraphStartingNumber + AdjacentAlertsGroupNumber * 2].Data[BarIndex] = Price;
						AdjacentAlertsHaveBottomHighlightAlertPrice = true;
						AdjacentAlertsCountInGroup++;
						AdjacentAlertsGroupAskVolume = CurrentAlertGroupAskVolume;
						AdjacentAlertsGroupBidVolume = CurrentAlertGroupBidVolume;
						AdjacentAlertsHighlightBottomPriceIndex = PriceIndex;
						AdjacentAlertsHighlightBottomPrice = Price; // Needed for extension lines
						if (AdjacentAlertsCountInGroup >= HighlightAdjacentAlertsGroupSizeValue)
						{
							sc.Subgraph[AdjacentAlertsHighlightSubgraphStartingNumber + AdjacentAlertsGroupNumber * 2 + 1].Data[BarIndex] = Price;
							AdjacentAlertsGotGroup = true;
						}

					}
					else if (AdjacentAlertsHaveBottomHighlightAlertPrice && PriceIndex == AdjacentAlertsHighlightBottomPriceIndex + AdjacentAlertsCountInGroup)
					{
						AdjacentAlertsCountInGroup++;
						AdjacentAlertsGroupAskVolume += CurrentAlertGroupAskVolume;
						AdjacentAlertsGroupBidVolume += CurrentAlertGroupBidVolume;
						if (AdjacentAlertsCountInGroup >= HighlightAdjacentAlertsGroupSizeValue)
						{
							sc.Subgraph[AdjacentAlertsHighlightSubgraphStartingNumber + AdjacentAlertsGroupNumber * 2 + 1].Data[BarIndex] = Price;
							AdjacentAlertsGotGroup = true;
						}
					}
					else if (AdjacentAlertsHaveBottomHighlightAlertPrice && !AdjacentAlertsGotGroup)
					{
						AdjacentAlertsCountInGroup = 0;
						AdjacentAlertsHaveBottomHighlightAlertPrice = false;
						AdjacentAlertsGroupAskVolume = 0.0;
						AdjacentAlertsGroupBidVolume = 0.0;
						AdjacentAlertsGroupFirstLineID = -1;
						AdjacentAlertsGroupLineCount = 0;
						sc.Subgraph[AdjacentAlertsHighlightSubgraphStartingNumber + AdjacentAlertsGroupNumber * 2].Data[BarIndex] = 0.0;
						AdjacentAlertsGroupNumber--;
					}
				}

				sc.Subgraph[AvailableSubgraphIndex][BarIndex] = Price;

				// Extension Lines
				const COLORREF AdjacentGroupExtensionLineColor =
					AdjacentAlertsGroupAskVolume >= AdjacentAlertsGroupBidVolume
					? Subgraph_ExtensionLineProperties.PrimaryColor
					: Subgraph_ExtensionLineProperties.SecondaryColor;

				if (Input_DrawExtensionLines.GetIndex() == 1 // All Alerts
					&& sc.GetBarHasClosedStatus(BarIndex) == BHCS_BAR_HAS_CLOSED)
				{
					n_ACSIL::s_LineUntilFutureIntersection LineUntilFutureIntersection;
					LineUntilFutureIntersection.StartBarIndex = BarIndex;
					LineUntilFutureIntersection.LineIDForBar = LineID;

					if (Input_DrawExtensionLinesWithTransparentRange.GetYesNo())
					{
						LineUntilFutureIntersection.LineValue = PriceForExtensionLine + sc.TickSize * 0.5f;
						LineUntilFutureIntersection.UseLineValue2 = true;
						LineUntilFutureIntersection.LineValue2ForRange = PriceForExtensionLine - sc.TickSize * 0.5f;
						LineUntilFutureIntersection.TransparencyLevel = TransparencyLevel;
					}
					else
					{
						LineUntilFutureIntersection.LineValue = PriceForExtensionLine;
					}

					LineUntilFutureIntersection.LineColor = SingleExtensionLineColor;
					LineUntilFutureIntersection.LineWidth = Subgraph_ExtensionLineProperties.LineWidth;
					LineUntilFutureIntersection.LineStyle = Subgraph_ExtensionLineProperties.LineStyle;

					if (Input_DrawExtensionLinesUntilEndOfChart.GetYesNo())
						LineUntilFutureIntersection.AlwaysExtendToEndOfChart = true;

					sc.AddLineUntilFutureIntersectionEx(LineUntilFutureIntersection);
					LineID++;
				}
				else if (Input_DrawExtensionLines.GetIndex() == 2 // Lowest Price in Adjacent Alerts
					&& AdjacentAlertsGotGroup
					&& sc.GetBarHasClosedStatus(BarIndex) == BHCS_BAR_HAS_CLOSED)
				{
					PriceForExtensionLine = AdjacentAlertsHighlightBottomPrice;

					if (!GotExtensionLineForGroup)
					{
						GotExtensionLineForGroup = true;
						AdjacentAlertsGroupFirstLineID = LineID;
						AdjacentAlertsGroupLineCount = 1;
						LineID++;
					}

					n_ACSIL::s_LineUntilFutureIntersection LineUntilFutureIntersection;
					LineUntilFutureIntersection.StartBarIndex = BarIndex;
					LineUntilFutureIntersection.LineIDForBar = AdjacentAlertsGroupFirstLineID;

					if (Input_DrawExtensionLinesWithTransparentRange.GetYesNo())
					{
						LineUntilFutureIntersection.LineValue = PriceForExtensionLine + sc.TickSize * 0.5f;
						LineUntilFutureIntersection.UseLineValue2 = true;
						LineUntilFutureIntersection.LineValue2ForRange = PriceForExtensionLine - sc.TickSize * 0.5f;
						LineUntilFutureIntersection.TransparencyLevel = TransparencyLevel;
					}
					else
					{
						LineUntilFutureIntersection.LineValue = PriceForExtensionLine;
					}

					LineUntilFutureIntersection.LineColor = AdjacentGroupExtensionLineColor;
					LineUntilFutureIntersection.LineWidth = Subgraph_ExtensionLineProperties.LineWidth;
					LineUntilFutureIntersection.LineStyle = Subgraph_ExtensionLineProperties.LineStyle;

					if (Input_DrawExtensionLinesUntilEndOfChart.GetYesNo())
						LineUntilFutureIntersection.AlwaysExtendToEndOfChart = true;

					// Reusing the same bar/line ID updates the color if later members
					// cause the aggregate Ask/Bid dominance to change.
					sc.AddLineUntilFutureIntersectionEx(LineUntilFutureIntersection);
				}
				else if (Input_DrawExtensionLines.GetIndex() == 3 // Highest Price in Adjacent Alerts
					&& AdjacentAlertsGotGroup
					&& sc.GetBarHasClosedStatus(BarIndex) == BHCS_BAR_HAS_CLOSED)
				{
					if (!GotExtensionLineForGroup)
					{
						GotExtensionLineForGroup = true;
					}
					else
					{
						LineID--;
						sc.DeleteLineUntilFutureIntersection(BarIndex, LineID);
					}

					PriceForExtensionLine = Price;

					n_ACSIL::s_LineUntilFutureIntersection LineUntilFutureIntersection;
					LineUntilFutureIntersection.StartBarIndex = BarIndex;
					LineUntilFutureIntersection.LineIDForBar = LineID;

					if (Input_DrawExtensionLinesWithTransparentRange.GetYesNo())
					{
						LineUntilFutureIntersection.LineValue = PriceForExtensionLine + sc.TickSize * 0.5f;
						LineUntilFutureIntersection.UseLineValue2 = true;
						LineUntilFutureIntersection.LineValue2ForRange = PriceForExtensionLine - sc.TickSize * 0.5f;
						LineUntilFutureIntersection.TransparencyLevel = TransparencyLevel;
					}
					else
					{
						LineUntilFutureIntersection.LineValue = PriceForExtensionLine;
					}

					LineUntilFutureIntersection.LineColor = AdjacentGroupExtensionLineColor;
					LineUntilFutureIntersection.LineWidth = Subgraph_ExtensionLineProperties.LineWidth;
					LineUntilFutureIntersection.LineStyle = Subgraph_ExtensionLineProperties.LineStyle;

					if (Input_DrawExtensionLinesUntilEndOfChart.GetYesNo())
						LineUntilFutureIntersection.AlwaysExtendToEndOfChart = true;

					sc.AddLineUntilFutureIntersectionEx(LineUntilFutureIntersection);
					LineID++;
				}
				else if (Input_DrawExtensionLines.GetIndex() == 4 // All Prices in Adjacent Alerts
					&& AdjacentAlertsGotGroup
					&& sc.GetBarHasClosedStatus(BarIndex) == BHCS_BAR_HAS_CLOSED)
				{
					if (!GotExtensionLineForGroup)
					{
						GotExtensionLineForGroup = true;
						AdjacentAlertsGroupFirstLineID = LineID;
					}

					while (AdjacentAlertsGroupLineCount < AdjacentAlertsCountInGroup)
					{
						AdjacentAlertsGroupLineCount++;
						LineID++;
					}

					// Add any newly required lines and update all existing lines in the
					// group so every line reflects the final aggregate dominance.
					for (int GroupLineIndex = 0; GroupLineIndex < AdjacentAlertsGroupLineCount; GroupLineIndex++)
					{
						PriceForExtensionLine = AdjacentAlertsHighlightBottomPrice
							+ GroupLineIndex * sc.TickSize * sc.VolumeAtPriceMultiplier;

						n_ACSIL::s_LineUntilFutureIntersection LineUntilFutureIntersection;
						LineUntilFutureIntersection.StartBarIndex = BarIndex;
						LineUntilFutureIntersection.LineIDForBar = AdjacentAlertsGroupFirstLineID + GroupLineIndex;

						if (Input_DrawExtensionLinesWithTransparentRange.GetYesNo())
						{
							LineUntilFutureIntersection.LineValue = PriceForExtensionLine + sc.TickSize * 0.5f;
							LineUntilFutureIntersection.UseLineValue2 = true;
							LineUntilFutureIntersection.LineValue2ForRange = PriceForExtensionLine - sc.TickSize * 0.5f;
							LineUntilFutureIntersection.TransparencyLevel = TransparencyLevel;
						}
						else
						{
							LineUntilFutureIntersection.LineValue = PriceForExtensionLine;
						}

						LineUntilFutureIntersection.LineColor = AdjacentGroupExtensionLineColor;
						LineUntilFutureIntersection.LineWidth = Subgraph_ExtensionLineProperties.LineWidth;
						LineUntilFutureIntersection.LineStyle = Subgraph_ExtensionLineProperties.LineStyle;

						if (Input_DrawExtensionLinesUntilEndOfChart.GetYesNo())
							LineUntilFutureIntersection.AlwaysExtendToEndOfChart = true;

						sc.AddLineUntilFutureIntersectionEx(LineUntilFutureIntersection);
					}
				}

				if (AvailableSubgraphIndex < SC_SUBGRAPHS_AVAILABLE - 13)
				{
					++AvailableSubgraphIndex;
				}

				if (EnableAlerts && Input_AlertNumber.GetAlertSoundNumber() > 0  && BarIndex == sc.ArraySize - 1)
				{
					sc.SetAlert(Input_AlertNumber.GetAlertSoundNumber() - 1 , "Volume threshold triggered");
				}
			}
		}
	}
}
