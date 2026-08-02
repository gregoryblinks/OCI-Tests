#include "sierrachart.h"

// Sierra Chart headers define max/min macros on some builds. Remove them before
// including standard C++ headers and avoid std::max/std::min in this file.
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>

SCDLLName("YMU YM Extreme Bid Ask Exhaustion Ratios")

// -----------------------------------------------------------------------------
// YMU / YM Extreme Bid-Ask Exhaustion Ratios
//
// Seller exhaustion at the LOW compares:
//   Bid volume at the lowest traded price
//   versus Bid volume one price level above it.
//
// Buyer exhaustion at the HIGH compares:
//   Ask volume at the highest traded price
//   versus Ask volume one price level below it.
//
// Default displayed calculation (Drop-Off %):
//   (AdjacentInsideVolume - ExtremeVolume) / AdjacentInsideVolume * 100
//
// Example: adjacent Bid = 100 and lowest Bid = 70 -> seller exhaustion = 30%.
// A larger positive percentage means a larger reduction in aggressive volume at
// the final price. The default signal threshold is 10%, adjustable separately
// for seller and buyer exhaustion.
//
// The alternate calculation displays Extreme / Adjacent * 100. In that mode a
// smaller percentage means stronger exhaustion and the signal test is <= the
// configured threshold.
//
// The study uses no future bars. With Closed Bars Only enabled, a signal is
// finalized on the candle that just closed, on the first update of the new bar.
// -----------------------------------------------------------------------------

namespace
{
    enum CalculationMode
    {
        CALC_DROP_OFF_PERCENT = 0,
        CALC_REMAINING_PERCENT = 1
    };

    double ClampDouble(const double Value, const double Minimum, const double Maximum)
    {
        if (Value < Minimum)
            return Minimum;
        if (Value > Maximum)
            return Maximum;
        return Value;
    }

    bool IsYMEminiDowSymbol(const SCString& Symbol)
    {
        const char* Text = Symbol.GetChars();
        if (Text == NULL || Text[0] == '\0')
            return false;

        const size_t Length = std::strlen(Text);

        // Accept common forms such as YMU26-CME, YMZ26-CME, YM?##-CME,
        // CBOT.YMU26, F.US.YMU26, and @YM. Explicitly reject MYM.
        for (size_t Index = 0; Index + 1 < Length; ++Index)
        {
            const unsigned char CurrentRaw =
                static_cast<unsigned char>(Text[Index]);
            const unsigned char NextRaw =
                static_cast<unsigned char>(Text[Index + 1]);

            const char Current =
                static_cast<char>(std::toupper(CurrentRaw));
            const char Next =
                static_cast<char>(std::toupper(NextRaw));

            if (Current != 'Y' || Next != 'M')
                continue;

            // Prevent the YM substring within MYM or another alphanumeric
            // prefix from being accepted as the full-size YM product.
            if (Index > 0
                && std::isalnum(
                    static_cast<unsigned char>(Text[Index - 1])) != 0)
            {
                continue;
            }

            if (Index + 2 >= Length)
                return true;

            const unsigned char FollowingRaw =
                static_cast<unsigned char>(Text[Index + 2]);
            const char Following =
                static_cast<char>(std::toupper(FollowingRaw));

            if (Following == 'H'
                || Following == 'M'
                || Following == 'U'
                || Following == 'Z'
                || Following == '?'
                || Following == '#'
                || std::isdigit(FollowingRaw) != 0
                || std::isalnum(FollowingRaw) == 0)
            {
                return true;
            }
        }

        return false;
    }

    bool IsYMOnePointTickSize(const float TickSize)
    {
        return std::fabs(static_cast<double>(TickSize) - 1.0) <= 0.0001;
    }

    bool GetVAPElement(
        SCStudyInterfaceRef sc,
        const int BarIndex,
        const int VAPIndex,
        const s_VolumeAtPriceV2*& Element)
    {
        Element = NULL;

        if (sc.VolumeAtPriceForBars == NULL)
            return false;

        return sc.VolumeAtPriceForBars->GetVAPElementAtIndex(
            BarIndex,
            VAPIndex,
            &Element);
    }

    double DropOffPercent(
        const double AdjacentInsideVolume,
        const double ExtremeVolume)
    {
        if (AdjacentInsideVolume <= 0.0)
            return 0.0;

        return (AdjacentInsideVolume - ExtremeVolume)
            / AdjacentInsideVolume
            * 100.0;
    }

    double RemainingPercent(
        const double AdjacentInsideVolume,
        const double ExtremeVolume)
    {
        if (AdjacentInsideVolume <= 0.0)
            return 0.0;

        return ExtremeVolume / AdjacentInsideVolume * 100.0;
    }

    // DRAWSTYLE_TRANSPARENT_CUSTOM_VALUE_AT_Y does not draw a zero when
    // DrawZeros is false. Return a tiny nonzero value which formats as 0 or 0.0
    // so a valid zero percentage can still be displayed on a candle.
    float VisibleDisplayValue(const double Value)
    {
        if (std::fabs(Value) < 0.00005)
            return 0.0001f;

        return static_cast<float>(Value);
    }

    bool IsNewLowWithinLookback(
        SCStudyInterfaceRef sc,
        const int BarIndex,
        const int Lookback)
    {
        if (Lookback <= 0)
            return true;

        if (BarIndex < Lookback)
            return false;

        const int StartIndex = BarIndex - Lookback;

        const float Candidate = sc.Low[BarIndex];

        for (int Index = StartIndex; Index < BarIndex; ++Index)
        {
            if (Candidate > sc.Low[Index])
                return false;
        }

        return true;
    }

    bool IsNewHighWithinLookback(
        SCStudyInterfaceRef sc,
        const int BarIndex,
        const int Lookback)
    {
        if (Lookback <= 0)
            return true;

        if (BarIndex < Lookback)
            return false;

        const int StartIndex = BarIndex - Lookback;

        const float Candidate = sc.High[BarIndex];

        for (int Index = StartIndex; Index < BarIndex; ++Index)
        {
            if (Candidate < sc.High[Index])
                return false;
        }

        return true;
    }

    void ClearOutputsAtIndex(SCStudyInterfaceRef sc, const int BarIndex)
    {
        if (BarIndex < 0)
            return;

        for (int SubgraphIndex = 0; SubgraphIndex <= 16; ++SubgraphIndex)
            sc.Subgraph[SubgraphIndex][BarIndex] = 0.0f;

        sc.Subgraph[2].Arrays[0][BarIndex] = 0.0f;
        sc.Subgraph[3].Arrays[0][BarIndex] = 0.0f;
    }
}

SCSFExport scsf_YMYMExtremeExhaustionRatios(SCStudyInterfaceRef sc)
{
    SCSubgraphRef SellerExhaustionArrow = sc.Subgraph[0];
    SCSubgraphRef BuyerExhaustionArrow = sc.Subgraph[1];
    SCSubgraphRef SellerPercentLabel = sc.Subgraph[2];
    SCSubgraphRef BuyerPercentLabel = sc.Subgraph[3];
    SCSubgraphRef SellerDropOffPercent = sc.Subgraph[4];
    SCSubgraphRef BuyerDropOffPercent = sc.Subgraph[5];
    SCSubgraphRef SellerRemainingPercent = sc.Subgraph[6];
    SCSubgraphRef BuyerRemainingPercent = sc.Subgraph[7];
    SCSubgraphRef LowExtremeBidVolume = sc.Subgraph[8];
    SCSubgraphRef LowAdjacentBidVolume = sc.Subgraph[9];
    SCSubgraphRef HighExtremeAskVolume = sc.Subgraph[10];
    SCSubgraphRef HighAdjacentAskVolume = sc.Subgraph[11];
    SCSubgraphRef SellerRatioQualified = sc.Subgraph[12];
    SCSubgraphRef BuyerRatioQualified = sc.Subgraph[13];
    SCSubgraphRef SellerFullSignal = sc.Subgraph[14];
    SCSubgraphRef BuyerFullSignal = sc.Subgraph[15];
    SCSubgraphRef DataStatus = sc.Subgraph[16];

    SCInputRef CalculationModeInput = sc.Input[0];
    SCInputRef SellerThresholdPercentInput = sc.Input[1];
    SCInputRef BuyerThresholdPercentInput = sc.Input[2];
    SCInputRef MinimumAdjacentVolumeInput = sc.Input[3];
    SCInputRef MinimumCombinedTwoLevelVolumeInput = sc.Input[4];
    SCInputRef MinimumAbsoluteVolumeReductionInput = sc.Input[5];
    SCInputRef RequireExactOneTickAdjacencyInput = sc.Input[6];
    SCInputRef RequireCloseRejectionInput = sc.Input[7];
    SCInputRef MinimumCloseRejectionPercentInput = sc.Input[8];
    SCInputRef RequireCandleDirectionInput = sc.Input[9];
    SCInputRef RequireFinishedAuctionInput = sc.Input[10];
    SCInputRef NewExtremeLookbackInput = sc.Input[11];
    SCInputRef MinimumBarRangePointsInput = sc.Input[12];
    SCInputRef ClosedBarsOnlyInput = sc.Input[13];
    SCInputRef ShowPercentOnEveryValidCandleInput = sc.Input[14];
    SCInputRef DisplaySignedDropOffInput = sc.Input[15];
    SCInputRef PercentageDecimalPlacesInput = sc.Input[16];
    SCInputRef PercentageLabelOffsetPointsInput = sc.Input[17];
    SCInputRef ShowSignalArrowsInput = sc.Input[18];
    SCInputRef ArrowOffsetPointsInput = sc.Input[19];
    SCInputRef AlertSoundNumberInput = sc.Input[20];
    SCInputRef RestrictToYMSymbolInput = sc.Input[21];
    SCInputRef RequireOnePointTickInput = sc.Input[22];

    if (sc.SetDefaults)
    {
        sc.GraphName = "YMU/YM Extreme Bid-Ask Exhaustion Ratios v1";
        sc.StudyDescription =
            "Calculates the Bid-volume drop-off between the two lowest prices "
            "and the Ask-volume drop-off between the two highest prices of "
            "each YMU/YM candle. Displays the percentages above and below "
            "each candle and optionally marks confirmed seller or buyer "
            "exhaustion. Uses Volume-at-Price data and no future bars.";

        sc.AutoLoop = 1;
        sc.GraphRegion = 0;
        sc.ScaleRangeType = SCALE_SAMEASREGION;
        sc.ValueFormat = 1;
        sc.MaintainVolumeAtPriceData = 1;
        sc.AlertOnlyOncePerBar = 1;
        sc.ResetAlertOnNewBar = 1;

        SellerExhaustionArrow.Name =
            "Seller Exhaustion Signal (Up Arrow)";
        SellerExhaustionArrow.DrawStyle = DRAWSTYLE_ARROW_UP;
        SellerExhaustionArrow.PrimaryColor = RGB(0, 210, 0);
        SellerExhaustionArrow.LineWidth = 4;
        SellerExhaustionArrow.DrawZeros = false;

        BuyerExhaustionArrow.Name =
            "Buyer Exhaustion Signal (Down Arrow)";
        BuyerExhaustionArrow.DrawStyle = DRAWSTYLE_ARROW_DOWN;
        BuyerExhaustionArrow.PrimaryColor = RGB(225, 35, 35);
        BuyerExhaustionArrow.LineWidth = 4;
        BuyerExhaustionArrow.DrawZeros = false;

        SellerPercentLabel.Name =
            "Seller Exhaustion % Below Candle";
        SellerPercentLabel.DrawStyle =
            DRAWSTYLE_TRANSPARENT_CUSTOM_VALUE_AT_Y;
        SellerPercentLabel.PrimaryColor = RGB(0, 210, 0);
        SellerPercentLabel.LineWidth = 8;
        SellerPercentLabel.DrawZeros = false;

        BuyerPercentLabel.Name =
            "Buyer Exhaustion % Above Candle";
        BuyerPercentLabel.DrawStyle =
            DRAWSTYLE_TRANSPARENT_CUSTOM_VALUE_AT_Y;
        BuyerPercentLabel.PrimaryColor = RGB(225, 35, 35);
        BuyerPercentLabel.LineWidth = 8;
        BuyerPercentLabel.DrawZeros = false;

        SellerDropOffPercent.Name =
            "Seller Exhaustion Drop-Off % (Low Bid Pair)";
        SellerDropOffPercent.DrawStyle = DRAWSTYLE_IGNORE;
        SellerDropOffPercent.DrawZeros = false;

        BuyerDropOffPercent.Name =
            "Buyer Exhaustion Drop-Off % (High Ask Pair)";
        BuyerDropOffPercent.DrawStyle = DRAWSTYLE_IGNORE;
        BuyerDropOffPercent.DrawZeros = false;

        SellerRemainingPercent.Name =
            "Low Extreme Bid / Adjacent Bid %";
        SellerRemainingPercent.DrawStyle = DRAWSTYLE_IGNORE;
        SellerRemainingPercent.DrawZeros = false;

        BuyerRemainingPercent.Name =
            "High Extreme Ask / Adjacent Ask %";
        BuyerRemainingPercent.DrawStyle = DRAWSTYLE_IGNORE;
        BuyerRemainingPercent.DrawZeros = false;

        LowExtremeBidVolume.Name = "Bid Volume at Candle Low";
        LowExtremeBidVolume.DrawStyle = DRAWSTYLE_IGNORE;
        LowExtremeBidVolume.DrawZeros = false;

        LowAdjacentBidVolume.Name = "Bid Volume One Tick Above Low";
        LowAdjacentBidVolume.DrawStyle = DRAWSTYLE_IGNORE;
        LowAdjacentBidVolume.DrawZeros = false;

        HighExtremeAskVolume.Name = "Ask Volume at Candle High";
        HighExtremeAskVolume.DrawStyle = DRAWSTYLE_IGNORE;
        HighExtremeAskVolume.DrawZeros = false;

        HighAdjacentAskVolume.Name = "Ask Volume One Tick Below High";
        HighAdjacentAskVolume.DrawStyle = DRAWSTYLE_IGNORE;
        HighAdjacentAskVolume.DrawZeros = false;

        SellerRatioQualified.Name = "Seller Ratio Qualified (1/0)";
        SellerRatioQualified.DrawStyle = DRAWSTYLE_IGNORE;
        SellerRatioQualified.DrawZeros = false;

        BuyerRatioQualified.Name = "Buyer Ratio Qualified (1/0)";
        BuyerRatioQualified.DrawStyle = DRAWSTYLE_IGNORE;
        BuyerRatioQualified.DrawZeros = false;

        SellerFullSignal.Name = "Seller Exhaustion Full Signal (1/0)";
        SellerFullSignal.DrawStyle = DRAWSTYLE_IGNORE;
        SellerFullSignal.DrawZeros = false;

        BuyerFullSignal.Name = "Buyer Exhaustion Full Signal (1/0)";
        BuyerFullSignal.DrawStyle = DRAWSTYLE_IGNORE;
        BuyerFullSignal.DrawZeros = false;

        DataStatus.Name =
            "Status (1 Valid, 0 Missing Data, -1 Wrong Chart, -2 Invalid Pair)";
        DataStatus.DrawStyle = DRAWSTYLE_IGNORE;
        DataStatus.DrawZeros = false;

        CalculationModeInput.Name =
            "Percentage Calculation and Threshold Mode";
        CalculationModeInput.SetCustomInputStrings(
            "Drop-Off %: Signal When >= Threshold;"
            "Extreme / Adjacent %: Signal When <= Threshold");
        CalculationModeInput.SetCustomInputIndex(CALC_DROP_OFF_PERCENT);

        SellerThresholdPercentInput.Name =
            "Seller Exhaustion Threshold % (Low Bid Pair)";
        SellerThresholdPercentInput.SetFloat(10.0f);
        SellerThresholdPercentInput.SetFloatLimits(0.0f, 100.0f);

        BuyerThresholdPercentInput.Name =
            "Buyer Exhaustion Threshold % (High Ask Pair)";
        BuyerThresholdPercentInput.SetFloat(10.0f);
        BuyerThresholdPercentInput.SetFloatLimits(0.0f, 100.0f);

        MinimumAdjacentVolumeInput.Name =
            "Minimum Aggressive Volume at Adjacent Inside Price";
        MinimumAdjacentVolumeInput.SetInt(10);
        MinimumAdjacentVolumeInput.SetIntLimits(0, 1000000000);

        MinimumCombinedTwoLevelVolumeInput.Name =
            "Minimum Combined Aggressive Volume of Two Prices";
        MinimumCombinedTwoLevelVolumeInput.SetInt(20);
        MinimumCombinedTwoLevelVolumeInput.SetIntLimits(0, 1000000000);

        MinimumAbsoluteVolumeReductionInput.Name =
            "Minimum Absolute Volume Reduction in Contracts";
        MinimumAbsoluteVolumeReductionInput.SetInt(1);
        MinimumAbsoluteVolumeReductionInput.SetIntLimits(0, 1000000000);

        RequireExactOneTickAdjacencyInput.Name =
            "Require the Two Prices to Be Exactly 1 Tick Apart";
        RequireExactOneTickAdjacencyInput.SetYesNo(1);

        RequireCloseRejectionInput.Name =
            "Require Candle Close Rejection from Extreme";
        RequireCloseRejectionInput.SetYesNo(1);

        MinimumCloseRejectionPercentInput.Name =
            "Minimum Close Rejection (% of Candle Range)";
        MinimumCloseRejectionPercentInput.SetFloat(20.0f);
        MinimumCloseRejectionPercentInput.SetFloatLimits(0.0f, 100.0f);

        RequireCandleDirectionInput.Name =
            "Require Bullish Candle for Seller / Bearish Candle for Buyer";
        RequireCandleDirectionInput.SetYesNo(0);

        RequireFinishedAuctionInput.Name =
            "Require Finished Auction at Candle Extreme";
        RequireFinishedAuctionInput.SetYesNo(0);

        NewExtremeLookbackInput.Name =
            "Require New High/Low Over Prior N Candles (0 = Disabled)";
        NewExtremeLookbackInput.SetInt(0);
        NewExtremeLookbackInput.SetIntLimits(0, 10000);

        MinimumBarRangePointsInput.Name =
            "Minimum Candle Range in YMU/YM Points";
        MinimumBarRangePointsInput.SetFloat(2.0f);
        MinimumBarRangePointsInput.SetFloatLimits(0.0f, 100000.0f);

        ClosedBarsOnlyInput.Name =
            "Use Closed Candles Only (Immediate on Close)";
        ClosedBarsOnlyInput.SetYesNo(1);

        ShowPercentOnEveryValidCandleInput.Name =
            "Show Percentages on Every Valid Candle";
        ShowPercentOnEveryValidCandleInput.SetYesNo(1);

        DisplaySignedDropOffInput.Name =
            "Display Signed Drop-Off % (Negative = Volume Increased)";
        DisplaySignedDropOffInput.SetYesNo(0);

        PercentageDecimalPlacesInput.Name =
            "Percentage Decimal Places";
        PercentageDecimalPlacesInput.SetInt(1);
        PercentageDecimalPlacesInput.SetIntLimits(0, 3);

        PercentageLabelOffsetPointsInput.Name =
            "Percentage Label Offset in YMU/YM Points";
        PercentageLabelOffsetPointsInput.SetFloat(2.0f);
        PercentageLabelOffsetPointsInput.SetFloatLimits(0.0f, 1000.0f);

        ShowSignalArrowsInput.Name = "Show Exhaustion Signal Arrows";
        ShowSignalArrowsInput.SetYesNo(1);

        ArrowOffsetPointsInput.Name =
            "Signal Arrow Offset in YMU/YM Points";
        ArrowOffsetPointsInput.SetFloat(7.0f);
        ArrowOffsetPointsInput.SetFloatLimits(0.0f, 1000.0f);

        AlertSoundNumberInput.Name =
            "Alert Sound Number (0 = Disabled)";
        AlertSoundNumberInput.SetInt(0);
        AlertSoundNumberInput.SetIntLimits(0, 150);

        RestrictToYMSymbolInput.Name =
            "Restrict Study to E-mini Dow YM/YMU Symbols";
        RestrictToYMSymbolInput.SetYesNo(1);

        RequireOnePointTickInput.Name =
            "Require YM 1-Point Tick Size";
        RequireOnePointTickInput.SetYesNo(1);

        return;
    }

    int PercentageDecimals = PercentageDecimalPlacesInput.GetInt();
    if (PercentageDecimals < 0)
        PercentageDecimals = 0;
    if (PercentageDecimals > 3)
        PercentageDecimals = 3;
    sc.ValueFormat = PercentageDecimals;
    sc.DataStartIndex = 1;

    if (sc.IsFullRecalculation && sc.Index == 0)
    {
        sc.GetPersistentInt(1) = -1;
        sc.GetPersistentInt(2) = -1;
    }

    int SignalIndex = sc.Index;

    if (ClosedBarsOnlyInput.GetYesNo() != 0
        && sc.GetBarHasClosedStatus(sc.Index) == BHCS_BAR_HAS_NOT_CLOSED)
    {
        SignalIndex = sc.Index - 1;
    }

    if (SignalIndex < 0 || SignalIndex >= sc.ArraySize)
        return;

    ClearOutputsAtIndex(sc, SignalIndex);

    const bool SymbolIsValid =
        RestrictToYMSymbolInput.GetYesNo() == 0
        || IsYMEminiDowSymbol(sc.Symbol);

    const bool TickSizeIsValid =
        RequireOnePointTickInput.GetYesNo() == 0
        || IsYMOnePointTickSize(sc.TickSize);

    if (!SymbolIsValid || !TickSizeIsValid)
    {
        DataStatus[SignalIndex] = -1.0f;
        return;
    }

    if (sc.VolumeAtPriceForBars == NULL || sc.TickSize <= 0.0f)
    {
        DataStatus[SignalIndex] = 0.0f;
        return;
    }

    const int VAPCount =
        sc.VolumeAtPriceForBars->GetSizeAtBarIndex(SignalIndex);

    if (VAPCount < 2)
    {
        DataStatus[SignalIndex] = -2.0f;
        return;
    }

    const s_VolumeAtPriceV2* Lowest = NULL;
    const s_VolumeAtPriceV2* SecondLowest = NULL;
    const s_VolumeAtPriceV2* Highest = NULL;
    const s_VolumeAtPriceV2* SecondHighest = NULL;

    if (!GetVAPElement(sc, SignalIndex, 0, Lowest)
        || !GetVAPElement(sc, SignalIndex, 1, SecondLowest)
        || !GetVAPElement(sc, SignalIndex, VAPCount - 1, Highest)
        || !GetVAPElement(sc, SignalIndex, VAPCount - 2, SecondHighest))
    {
        DataStatus[SignalIndex] = 0.0f;
        return;
    }

    const bool RequireExactAdjacency =
        RequireExactOneTickAdjacencyInput.GetYesNo() != 0;

    const bool LowPairAdjacent =
        SecondLowest->PriceInTicks == Lowest->PriceInTicks + 1;

    const bool HighPairAdjacent =
        Highest->PriceInTicks == SecondHighest->PriceInTicks + 1;

    const bool LowPairValid =
        !RequireExactAdjacency || LowPairAdjacent;
    const bool HighPairValid =
        !RequireExactAdjacency || HighPairAdjacent;

    if (!LowPairValid && !HighPairValid)
    {
        DataStatus[SignalIndex] = -2.0f;
        return;
    }

    DataStatus[SignalIndex] = 1.0f;

    const double LowExtremeBid =
        static_cast<double>(Lowest->BidVolume);
    const double LowAdjacentBid =
        static_cast<double>(SecondLowest->BidVolume);
    const double HighExtremeAsk =
        static_cast<double>(Highest->AskVolume);
    const double HighAdjacentAsk =
        static_cast<double>(SecondHighest->AskVolume);

    LowExtremeBidVolume[SignalIndex] =
        static_cast<float>(LowExtremeBid);
    LowAdjacentBidVolume[SignalIndex] =
        static_cast<float>(LowAdjacentBid);
    HighExtremeAskVolume[SignalIndex] =
        static_cast<float>(HighExtremeAsk);
    HighAdjacentAskVolume[SignalIndex] =
        static_cast<float>(HighAdjacentAsk);

    const bool LowDenominatorValid =
        LowPairValid && LowAdjacentBid > 0.0;
    const bool HighDenominatorValid =
        HighPairValid && HighAdjacentAsk > 0.0;

    const double SellerDropOff = LowDenominatorValid
        ? DropOffPercent(LowAdjacentBid, LowExtremeBid)
        : 0.0;
    const double BuyerDropOff = HighDenominatorValid
        ? DropOffPercent(HighAdjacentAsk, HighExtremeAsk)
        : 0.0;

    const double SellerRemaining = LowDenominatorValid
        ? RemainingPercent(LowAdjacentBid, LowExtremeBid)
        : 0.0;
    const double BuyerRemaining = HighDenominatorValid
        ? RemainingPercent(HighAdjacentAsk, HighExtremeAsk)
        : 0.0;

    if (LowDenominatorValid)
    {
        SellerDropOffPercent[SignalIndex] =
            static_cast<float>(SellerDropOff);
        SellerRemainingPercent[SignalIndex] =
            static_cast<float>(SellerRemaining);
    }

    if (HighDenominatorValid)
    {
        BuyerDropOffPercent[SignalIndex] =
            static_cast<float>(BuyerDropOff);
        BuyerRemainingPercent[SignalIndex] =
            static_cast<float>(BuyerRemaining);
    }

    const int CalculationModeIndex = CalculationModeInput.GetIndex();
    const double SellerThreshold =
        static_cast<double>(SellerThresholdPercentInput.GetFloat());
    const double BuyerThreshold =
        static_cast<double>(BuyerThresholdPercentInput.GetFloat());

    bool SellerRatioPass = false;
    bool BuyerRatioPass = false;

    if (CalculationModeIndex == CALC_REMAINING_PERCENT)
    {
        SellerRatioPass =
            LowDenominatorValid && SellerRemaining <= SellerThreshold;
        BuyerRatioPass =
            HighDenominatorValid && BuyerRemaining <= BuyerThreshold;
    }
    else
    {
        SellerRatioPass =
            LowDenominatorValid && SellerDropOff >= SellerThreshold;
        BuyerRatioPass =
            HighDenominatorValid && BuyerDropOff >= BuyerThreshold;
    }

    SellerRatioQualified[SignalIndex] =
        SellerRatioPass ? 1.0f : 0.0f;
    BuyerRatioQualified[SignalIndex] =
        BuyerRatioPass ? 1.0f : 0.0f;

    const double MinimumAdjacentVolume =
        static_cast<double>(MinimumAdjacentVolumeInput.GetInt());
    const double MinimumCombinedVolume =
        static_cast<double>(MinimumCombinedTwoLevelVolumeInput.GetInt());
    const double MinimumAbsoluteReduction =
        static_cast<double>(MinimumAbsoluteVolumeReductionInput.GetInt());

    const bool SellerVolumeFiltersPass =
        LowDenominatorValid
        && LowAdjacentBid >= MinimumAdjacentVolume
        && LowAdjacentBid + LowExtremeBid >= MinimumCombinedVolume
        && LowAdjacentBid - LowExtremeBid >= MinimumAbsoluteReduction;

    const bool BuyerVolumeFiltersPass =
        HighDenominatorValid
        && HighAdjacentAsk >= MinimumAdjacentVolume
        && HighAdjacentAsk + HighExtremeAsk >= MinimumCombinedVolume
        && HighAdjacentAsk - HighExtremeAsk >= MinimumAbsoluteReduction;

    const double BarRange =
        static_cast<double>(sc.High[SignalIndex] - sc.Low[SignalIndex]);
    const double MinimumBarRange =
        static_cast<double>(MinimumBarRangePointsInput.GetFloat());
    const bool BarRangePass = BarRange >= MinimumBarRange;

    const double CloseLocation = BarRange > 0.0
        ? ClampDouble(
            (static_cast<double>(sc.Close[SignalIndex])
                - static_cast<double>(sc.Low[SignalIndex]))
                / BarRange,
            0.0,
            1.0)
        : 0.5;

    const bool RequireRejection =
        RequireCloseRejectionInput.GetYesNo() != 0;
    const double RejectionFraction =
        static_cast<double>(MinimumCloseRejectionPercentInput.GetFloat())
        / 100.0;

    const bool SellerRejectionPass =
        !RequireRejection || CloseLocation >= RejectionFraction;
    const bool BuyerRejectionPass =
        !RequireRejection || CloseLocation <= 1.0 - RejectionFraction;

    const bool RequireDirection =
        RequireCandleDirectionInput.GetYesNo() != 0;

    const bool SellerDirectionPass =
        !RequireDirection
        || sc.Close[SignalIndex] > sc.Open[SignalIndex];
    const bool BuyerDirectionPass =
        !RequireDirection
        || sc.Close[SignalIndex] < sc.Open[SignalIndex];

    const bool RequireFinishedAuction =
        RequireFinishedAuctionInput.GetYesNo() != 0;

    // Common footprint convention:
    //   finished low  = zero Ask volume at the lowest price
    //   finished high = zero Bid volume at the highest price
    const bool SellerFinishedAuctionPass =
        !RequireFinishedAuction || Lowest->AskVolume == 0;
    const bool BuyerFinishedAuctionPass =
        !RequireFinishedAuction || Highest->BidVolume == 0;

    const int NewExtremeLookback = NewExtremeLookbackInput.GetInt();
    const bool SellerNewExtremePass =
        IsNewLowWithinLookback(sc, SignalIndex, NewExtremeLookback);
    const bool BuyerNewExtremePass =
        IsNewHighWithinLookback(sc, SignalIndex, NewExtremeLookback);

    const bool SellerSignal =
        SellerRatioPass
        && SellerVolumeFiltersPass
        && BarRangePass
        && SellerRejectionPass
        && SellerDirectionPass
        && SellerFinishedAuctionPass
        && SellerNewExtremePass;

    const bool BuyerSignal =
        BuyerRatioPass
        && BuyerVolumeFiltersPass
        && BarRangePass
        && BuyerRejectionPass
        && BuyerDirectionPass
        && BuyerFinishedAuctionPass
        && BuyerNewExtremePass;

    SellerFullSignal[SignalIndex] = SellerSignal ? 1.0f : 0.0f;
    BuyerFullSignal[SignalIndex] = BuyerSignal ? 1.0f : 0.0f;

    const bool ShowAllPercentages =
        ShowPercentOnEveryValidCandleInput.GetYesNo() != 0;
    const bool DisplaySignedDropOff =
        DisplaySignedDropOffInput.GetYesNo() != 0;
    const double LabelOffset =
        static_cast<double>(PercentageLabelOffsetPointsInput.GetFloat())
        * static_cast<double>(sc.TickSize);

    if (LowDenominatorValid && (ShowAllPercentages || SellerRatioPass))
    {
        double SellerDisplayValue = SellerRemaining;

        if (CalculationModeIndex == CALC_DROP_OFF_PERCENT)
        {
            SellerDisplayValue = SellerDropOff;
            if (!DisplaySignedDropOff && SellerDisplayValue < 0.0)
                SellerDisplayValue = 0.0;
        }

        SellerPercentLabel[SignalIndex] =
            VisibleDisplayValue(SellerDisplayValue);
        SellerPercentLabel.Arrays[0][SignalIndex] =
            static_cast<float>(
                static_cast<double>(sc.Low[SignalIndex]) - LabelOffset);

        if (SellerSignal)
        {
            SellerPercentLabel.DataColor[SignalIndex] =
                SellerPercentLabel.PrimaryColor;
        }
        else if (SellerRatioPass)
        {
            // Ratio passed, but one or more confirmation filters did not.
            SellerPercentLabel.DataColor[SignalIndex] = RGB(0, 165, 255);
        }
        else
        {
            SellerPercentLabel.DataColor[SignalIndex] = RGB(135, 135, 135);
        }
    }

    if (HighDenominatorValid && (ShowAllPercentages || BuyerRatioPass))
    {
        double BuyerDisplayValue = BuyerRemaining;

        if (CalculationModeIndex == CALC_DROP_OFF_PERCENT)
        {
            BuyerDisplayValue = BuyerDropOff;
            if (!DisplaySignedDropOff && BuyerDisplayValue < 0.0)
                BuyerDisplayValue = 0.0;
        }

        BuyerPercentLabel[SignalIndex] =
            VisibleDisplayValue(BuyerDisplayValue);
        BuyerPercentLabel.Arrays[0][SignalIndex] =
            static_cast<float>(
                static_cast<double>(sc.High[SignalIndex]) + LabelOffset);

        if (BuyerSignal)
        {
            BuyerPercentLabel.DataColor[SignalIndex] =
                BuyerPercentLabel.PrimaryColor;
        }
        else if (BuyerRatioPass)
        {
            // Ratio passed, but one or more confirmation filters did not.
            BuyerPercentLabel.DataColor[SignalIndex] = RGB(255, 155, 0);
        }
        else
        {
            BuyerPercentLabel.DataColor[SignalIndex] = RGB(135, 135, 135);
        }
    }

    if (ShowSignalArrowsInput.GetYesNo() != 0)
    {
        const double ArrowOffset =
            static_cast<double>(ArrowOffsetPointsInput.GetFloat())
            * static_cast<double>(sc.TickSize);

        if (SellerSignal)
        {
            SellerExhaustionArrow[SignalIndex] =
                static_cast<float>(
                    static_cast<double>(sc.Low[SignalIndex]) - ArrowOffset);
        }

        if (BuyerSignal)
        {
            BuyerExhaustionArrow[SignalIndex] =
                static_cast<float>(
                    static_cast<double>(sc.High[SignalIndex]) + ArrowOffset);
        }
    }

    const int AlertSoundNumber = AlertSoundNumberInput.GetInt();
    const bool IsRecentLiveSignal =
        !sc.IsFullRecalculation
        && SignalIndex >= sc.ArraySize - 2;

    int& LastSellerAlertIndex = sc.GetPersistentInt(1);
    int& LastBuyerAlertIndex = sc.GetPersistentInt(2);

    if (AlertSoundNumber > 0 && IsRecentLiveSignal)
    {
        if (SellerSignal && LastSellerAlertIndex != SignalIndex)
        {
            SCString AlertMessage;
            AlertMessage =
                "YMU/YM seller exhaustion at candle low";
            sc.SetAlert(AlertSoundNumber, SignalIndex, AlertMessage);
            LastSellerAlertIndex = SignalIndex;
        }

        if (BuyerSignal && LastBuyerAlertIndex != SignalIndex)
        {
            SCString AlertMessage;
            AlertMessage =
                "YMU/YM buyer exhaustion at candle high";
            sc.SetAlert(AlertSoundNumber, SignalIndex, AlertMessage);
            LastBuyerAlertIndex = SignalIndex;
        }
    }
}
