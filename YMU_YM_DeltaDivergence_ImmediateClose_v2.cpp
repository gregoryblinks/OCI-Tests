#include "sierrachart.h"

// Sierra Chart defines max/min as macros. Remove them before including C++
// standard headers and use the local MaximumInt/MaximumDouble helpers below.
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include <cctype>
#include <cmath>
#include <cstring>

SCDLLName("YMU YM Immediate Close Delta Divergence Studies")

// -----------------------------------------------------------------------------
// YMU / YM Immediate-Close Delta Divergence
//
// Bullish divergence:
//   The just-closed YMU/YM bar makes a lower low than a prior swing-low anchor,
//   while the selected delta measure is higher than it was at that anchor.
//
// Bearish divergence:
//   The just-closed YMU/YM bar makes a higher high than a prior swing-high
//   anchor, while the selected delta measure is lower than it was at that
//   anchor.
//
// IMPORTANT REAL-TIME BEHAVIOR
// ----------------------------
// The CURRENT signal bar uses only bars to its left. It does not wait for one
// or more future bars to confirm the current high/low. With "Use Closed Bars
// Only" enabled, the marker is calculated as soon as Sierra Chart recognizes
// that bar as closed (normally on the first update of the next bar).
//
// The previous anchor can require right-side confirmation because all of those
// bars are already historical by the time the current signal is evaluated.
// The "Previous Anchor Right Strength" input therefore improves anchor quality
// without delaying the current divergence marker. Set it to 0 for a completely
// left-side-only anchor.
//
// Bar delta is Ask Volume minus Bid Volume. This is an analytical study, not an
// automated trading system or a guarantee that price will reverse.
// -----------------------------------------------------------------------------

namespace
{
    enum DeltaSourceMode
    {
        DELTA_TRADING_DAY_CUMULATIVE = 0,
        DELTA_CONTINUOUS_CUMULATIVE = 1,
        DELTA_ROLLING_SUM = 2,
        DELTA_BAR = 3
    };

    enum SignalBarCloseFilterMode
    {
        CLOSE_FILTER_NONE = 0,
        CLOSE_FILTER_BEYOND_MIDPOINT = 1,
        CLOSE_FILTER_CANDLE_DIRECTION = 2
    };

    enum MarkerStyleMode
    {
        MARK_LARGE_ARROW = 0,
        MARK_SMALL_ARROW = 1,
        MARK_BOTH_ARROWS = 2
    };

    int MaximumInt(const int A, const int B)
    {
        return A > B ? A : B;
    }

    double MaximumDouble(const double A, const double B)
    {
        return A > B ? A : B;
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

            // Prevent the YM substring within MYM (or another alphanumeric
            // prefix) from being accepted as the full-size YM product.
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

    bool IsSameTradingDay(
        SCStudyInterfaceRef sc,
        const int FirstIndex,
        const int SecondIndex)
    {
        return sc.GetTradingDayDate(sc.BaseDateTimeIn[FirstIndex])
            == sc.GetTradingDayDate(sc.BaseDateTimeIn[SecondIndex]);
    }

    // Current-bar test. It deliberately uses only bars to the LEFT so the
    // current divergence can be known when that bar closes.
    bool IsCurrentLeftSideLow(
        SCStudyInterfaceRef sc,
        const int BarIndex,
        const int LeftStrength,
        const bool AllowEqual)
    {
        if (BarIndex - LeftStrength < 0)
            return false;

        const float Candidate = sc.Low[BarIndex];

        for (int Offset = 1; Offset <= LeftStrength; ++Offset)
        {
            const float Other = sc.Low[BarIndex - Offset];

            if (AllowEqual)
            {
                if (Candidate > Other)
                    return false;
            }
            else if (Candidate >= Other)
            {
                return false;
            }
        }

        return true;
    }

    bool IsCurrentLeftSideHigh(
        SCStudyInterfaceRef sc,
        const int BarIndex,
        const int LeftStrength,
        const bool AllowEqual)
    {
        if (BarIndex - LeftStrength < 0)
            return false;

        const float Candidate = sc.High[BarIndex];

        for (int Offset = 1; Offset <= LeftStrength; ++Offset)
        {
            const float Other = sc.High[BarIndex - Offset];

            if (AllowEqual)
            {
                if (Candidate < Other)
                    return false;
            }
            else if (Candidate <= Other)
            {
                return false;
            }
        }

        return true;
    }

    // Historical anchor test. LastKnownIndex prevents a historical
    // recalculation from using bars that were not yet available when the
    // current divergence bar closed.
    bool IsHistoricalPivotLow(
        SCStudyInterfaceRef sc,
        const int PivotIndex,
        const int LeftStrength,
        const int RightStrength,
        const int LastKnownIndex,
        const bool AllowEqual)
    {
        if (PivotIndex - LeftStrength < 0
            || PivotIndex + RightStrength > LastKnownIndex)
        {
            return false;
        }

        const float Candidate = sc.Low[PivotIndex];

        for (int Offset = 1; Offset <= LeftStrength; ++Offset)
        {
            const float Other = sc.Low[PivotIndex - Offset];

            if (AllowEqual)
            {
                if (Candidate > Other)
                    return false;
            }
            else if (Candidate >= Other)
            {
                return false;
            }
        }

        // Equality is not allowed on the right. This selects the last bar in
        // an equal-low cluster when equal values are permitted on the left.
        for (int Offset = 1; Offset <= RightStrength; ++Offset)
        {
            if (Candidate >= sc.Low[PivotIndex + Offset])
                return false;
        }

        return true;
    }

    bool IsHistoricalPivotHigh(
        SCStudyInterfaceRef sc,
        const int PivotIndex,
        const int LeftStrength,
        const int RightStrength,
        const int LastKnownIndex,
        const bool AllowEqual)
    {
        if (PivotIndex - LeftStrength < 0
            || PivotIndex + RightStrength > LastKnownIndex)
        {
            return false;
        }

        const float Candidate = sc.High[PivotIndex];

        for (int Offset = 1; Offset <= LeftStrength; ++Offset)
        {
            const float Other = sc.High[PivotIndex - Offset];

            if (AllowEqual)
            {
                if (Candidate < Other)
                    return false;
            }
            else if (Candidate <= Other)
            {
                return false;
            }
        }

        for (int Offset = 1; Offset <= RightStrength; ++Offset)
        {
            if (Candidate <= sc.High[PivotIndex + Offset])
                return false;
        }

        return true;
    }

    int FindPreviousPivotLow(
        SCStudyInterfaceRef sc,
        const int CurrentIndex,
        const int LeftStrength,
        const int AnchorRightStrength,
        const int MinimumBarsBetween,
        const int MaximumBarsBetween,
        const bool AllowEqual,
        const bool RequireSameTradingDay)
    {
        const int FirstCandidate = CurrentIndex - MinimumBarsBetween;
        const int LastCandidate = MaximumInt(
            LeftStrength,
            CurrentIndex - MaximumBarsBetween);
        const int LastKnownIndex = CurrentIndex - 1;

        if (FirstCandidate < LastCandidate || LastKnownIndex < 0)
            return -1;

        for (int Candidate = FirstCandidate;
             Candidate >= LastCandidate;
             --Candidate)
        {
            if (RequireSameTradingDay
                && !IsSameTradingDay(sc, Candidate, CurrentIndex))
            {
                // Scanning backward has crossed into the prior trading day.
                break;
            }

            if (IsHistoricalPivotLow(
                    sc,
                    Candidate,
                    LeftStrength,
                    AnchorRightStrength,
                    LastKnownIndex,
                    AllowEqual))
            {
                return Candidate;
            }
        }

        return -1;
    }

    int FindPreviousPivotHigh(
        SCStudyInterfaceRef sc,
        const int CurrentIndex,
        const int LeftStrength,
        const int AnchorRightStrength,
        const int MinimumBarsBetween,
        const int MaximumBarsBetween,
        const bool AllowEqual,
        const bool RequireSameTradingDay)
    {
        const int FirstCandidate = CurrentIndex - MinimumBarsBetween;
        const int LastCandidate = MaximumInt(
            LeftStrength,
            CurrentIndex - MaximumBarsBetween);
        const int LastKnownIndex = CurrentIndex - 1;

        if (FirstCandidate < LastCandidate || LastKnownIndex < 0)
            return -1;

        for (int Candidate = FirstCandidate;
             Candidate >= LastCandidate;
             --Candidate)
        {
            if (RequireSameTradingDay
                && !IsSameTradingDay(sc, Candidate, CurrentIndex))
            {
                break;
            }

            if (IsHistoricalPivotHigh(
                    sc,
                    Candidate,
                    LeftStrength,
                    AnchorRightStrength,
                    LastKnownIndex,
                    AllowEqual))
            {
                return Candidate;
            }
        }

        return -1;
    }

    double AverageAbsoluteBarDelta(
        SCSubgraphRef BarDelta,
        const int EndingIndex,
        const int Length)
    {
        const int StartIndex = MaximumInt(0, EndingIndex - Length + 1);
        double Sum = 0.0;
        int Count = 0;

        for (int Index = StartIndex; Index <= EndingIndex; ++Index)
        {
            Sum += std::fabs(static_cast<double>(BarDelta[Index]));
            ++Count;
        }

        return Count > 0 ? Sum / static_cast<double>(Count) : 0.0;
    }

    bool RangeHasValidBidAskData(
        SCSubgraphRef Status,
        const int StartIndex,
        const int EndIndex)
    {
        if (StartIndex < 0 || EndIndex < StartIndex)
            return false;

        for (int Index = StartIndex; Index <= EndIndex; ++Index)
        {
            if (Status[Index] <= 0.0f)
                return false;
        }

        return true;
    }

    bool BullishSignalBarCloseFilterPasses(
        SCStudyInterfaceRef sc,
        const int BarIndex,
        const int Mode,
        const double MinimumCloseMovePoints)
    {
        const double BarLow = static_cast<double>(sc.Low[BarIndex]);
        const double BarHigh = static_cast<double>(sc.High[BarIndex]);
        const double BarOpen = static_cast<double>(sc.Open[BarIndex]);
        const double BarClose = static_cast<double>(sc.Close[BarIndex]);

        if (BarClose - BarLow < MinimumCloseMovePoints)
            return false;

        if (Mode == CLOSE_FILTER_BEYOND_MIDPOINT)
            return BarClose > (BarHigh + BarLow) * 0.5;

        if (Mode == CLOSE_FILTER_CANDLE_DIRECTION)
            return BarClose > BarOpen;

        return true;
    }

    bool BearishSignalBarCloseFilterPasses(
        SCStudyInterfaceRef sc,
        const int BarIndex,
        const int Mode,
        const double MinimumCloseMovePoints)
    {
        const double BarLow = static_cast<double>(sc.Low[BarIndex]);
        const double BarHigh = static_cast<double>(sc.High[BarIndex]);
        const double BarOpen = static_cast<double>(sc.Open[BarIndex]);
        const double BarClose = static_cast<double>(sc.Close[BarIndex]);

        if (BarHigh - BarClose < MinimumCloseMovePoints)
            return false;

        if (Mode == CLOSE_FILTER_BEYOND_MIDPOINT)
            return BarClose < (BarHigh + BarLow) * 0.5;

        if (Mode == CLOSE_FILTER_CANDLE_DIRECTION)
            return BarClose < BarOpen;

        return true;
    }
}

SCSFExport scsf_YMYMImmediateCloseDeltaDivergence(SCStudyInterfaceRef sc)
{
    SCSubgraphRef BullishMarker = sc.Subgraph[0];
    SCSubgraphRef BearishMarker = sc.Subgraph[1];
    SCSubgraphRef BarDelta = sc.Subgraph[2];
    SCSubgraphRef TradingDayCumulativeDelta = sc.Subgraph[3];
    SCSubgraphRef ContinuousCumulativeDelta = sc.Subgraph[4];
    SCSubgraphRef RollingDelta = sc.Subgraph[5];
    SCSubgraphRef SelectedDeltaRaw = sc.Subgraph[6];
    SCSubgraphRef SelectedDelta = sc.Subgraph[7];
    SCSubgraphRef BullishPriceExtension = sc.Subgraph[8];
    SCSubgraphRef BullishDeltaImprovement = sc.Subgraph[9];
    SCSubgraphRef BearishPriceExtension = sc.Subgraph[10];
    SCSubgraphRef BearishDeltaImprovement = sc.Subgraph[11];
    SCSubgraphRef RequiredDeltaImprovement = sc.Subgraph[12];
    SCSubgraphRef BullishAnchorIndex = sc.Subgraph[13];
    SCSubgraphRef BearishAnchorIndex = sc.Subgraph[14];
    SCSubgraphRef ChartAndDataStatus = sc.Subgraph[15];
    SCSubgraphRef BullishSmallMarker = sc.Subgraph[16];
    SCSubgraphRef BearishSmallMarker = sc.Subgraph[17];

    SCInputRef DeltaSourceInput = sc.Input[0];
    SCInputRef RollingDeltaLengthInput = sc.Input[1];
    SCInputRef ResetRollingAtTradingDayInput = sc.Input[2];
    SCInputRef DeltaSmoothingLengthInput = sc.Input[3];
    SCInputRef CurrentLeftStrengthInput = sc.Input[4];
    SCInputRef PreviousAnchorRightStrengthInput = sc.Input[5];
    SCInputRef MinimumBarsBetweenPivotsInput = sc.Input[6];
    SCInputRef MaximumBarsBetweenPivotsInput = sc.Input[7];
    SCInputRef AllowEqualPivotsInput = sc.Input[8];
    SCInputRef MinimumPriceExtensionPointsInput = sc.Input[9];
    SCInputRef MinimumDeltaImprovementContractsInput = sc.Input[10];
    SCInputRef MinimumDeltaImprovementMultipleInput = sc.Input[11];
    SCInputRef AverageAbsoluteDeltaLookbackInput = sc.Input[12];
    SCInputRef RequireSameTradingDayInput = sc.Input[13];
    SCInputRef SignalBarCloseFilterInput = sc.Input[14];
    SCInputRef MinimumSignalBarCloseMovePointsInput = sc.Input[15];
    SCInputRef RequireSignalBarDeltaDirectionInput = sc.Input[16];
    SCInputRef MarkerStyleInput = sc.Input[17];
    SCInputRef ClosedBarsOnlyInput = sc.Input[18];
    SCInputRef LargeArrowOffsetPointsInput = sc.Input[19];
    SCInputRef SmallArrowOffsetPointsInput = sc.Input[20];
    SCInputRef AlertSoundNumberInput = sc.Input[21];
    SCInputRef RestrictToYMSymbolInput = sc.Input[22];
    SCInputRef RequireOnePointTickInput = sc.Input[23];

    if (sc.SetDefaults)
    {
        sc.GraphName = "YMU/YM Immediate Close Delta Divergence v2";
        sc.StudyDescription =
            "Marks bullish or bearish YMU/YM delta divergence on the current "
            "signal candle as soon as that candle is closed. The current "
            "signal candle uses only prior bars and therefore has no right-"
            "side pivot delay. A prior anchor can use already-known right-"
            "side confirmation without delaying the current marker. Delta is "
            "Ask Volume minus Bid Volume.";

        sc.AutoLoop = 1;
        sc.GraphRegion = 0;
        sc.ValueFormat = sc.BaseGraphValueFormat;
        sc.MaintainAdditionalChartDataArrays = 1;
        sc.AlertOnlyOncePerBar = 1;
        sc.ResetAlertOnNewBar = 1;

        BullishMarker.Name =
            "Bullish Delta Divergence - Immediate Closed Bar";
        BullishMarker.DrawStyle = DRAWSTYLE_ARROW_UP;
        BullishMarker.PrimaryColor = RGB(0, 190, 0);
        BullishMarker.LineWidth = 4;
        BullishMarker.DrawZeros = false;

        BearishMarker.Name =
            "Bearish Delta Divergence - Immediate Closed Bar";
        BearishMarker.DrawStyle = DRAWSTYLE_ARROW_DOWN;
        BearishMarker.PrimaryColor = RGB(220, 35, 35);
        BearishMarker.LineWidth = 4;
        BearishMarker.DrawZeros = false;

        BullishSmallMarker.Name =
            "Bullish Delta Divergence - Small Immediate Marker";
        BullishSmallMarker.DrawStyle = DRAWSTYLE_ARROW_UP;
        BullishSmallMarker.PrimaryColor = RGB(0, 130, 0);
        BullishSmallMarker.LineWidth = 2;
        BullishSmallMarker.DrawZeros = false;

        BearishSmallMarker.Name =
            "Bearish Delta Divergence - Small Immediate Marker";
        BearishSmallMarker.DrawStyle = DRAWSTYLE_ARROW_DOWN;
        BearishSmallMarker.PrimaryColor = RGB(170, 0, 0);
        BearishSmallMarker.LineWidth = 2;
        BearishSmallMarker.DrawZeros = false;

        BarDelta.Name = "Bar Delta (Ask Volume - Bid Volume)";
        BarDelta.DrawStyle = DRAWSTYLE_IGNORE;
        BarDelta.DrawZeros = false;

        TradingDayCumulativeDelta.Name = "Trading-Day Cumulative Delta";
        TradingDayCumulativeDelta.DrawStyle = DRAWSTYLE_IGNORE;
        TradingDayCumulativeDelta.DrawZeros = false;

        ContinuousCumulativeDelta.Name = "Continuous Cumulative Delta";
        ContinuousCumulativeDelta.DrawStyle = DRAWSTYLE_IGNORE;
        ContinuousCumulativeDelta.DrawZeros = false;

        RollingDelta.Name = "Rolling Delta Sum";
        RollingDelta.DrawStyle = DRAWSTYLE_IGNORE;
        RollingDelta.DrawZeros = false;

        SelectedDeltaRaw.Name = "Selected Delta - Raw";
        SelectedDeltaRaw.DrawStyle = DRAWSTYLE_IGNORE;
        SelectedDeltaRaw.DrawZeros = false;

        SelectedDelta.Name = "Selected Delta - Smoothed";
        SelectedDelta.DrawStyle = DRAWSTYLE_IGNORE;
        SelectedDelta.DrawZeros = false;

        BullishPriceExtension.Name =
            "Bullish Price Extension Beyond Prior Anchor (YM Points)";
        BullishPriceExtension.DrawStyle = DRAWSTYLE_IGNORE;
        BullishPriceExtension.DrawZeros = false;

        BullishDeltaImprovement.Name =
            "Bullish Delta Improvement Versus Prior Anchor";
        BullishDeltaImprovement.DrawStyle = DRAWSTYLE_IGNORE;
        BullishDeltaImprovement.DrawZeros = false;

        BearishPriceExtension.Name =
            "Bearish Price Extension Beyond Prior Anchor (YM Points)";
        BearishPriceExtension.DrawStyle = DRAWSTYLE_IGNORE;
        BearishPriceExtension.DrawZeros = false;

        BearishDeltaImprovement.Name =
            "Bearish Delta Improvement Versus Prior Anchor";
        BearishDeltaImprovement.DrawStyle = DRAWSTYLE_IGNORE;
        BearishDeltaImprovement.DrawZeros = false;

        RequiredDeltaImprovement.Name =
            "Required Delta Improvement at Signal Bar";
        RequiredDeltaImprovement.DrawStyle = DRAWSTYLE_IGNORE;
        RequiredDeltaImprovement.DrawZeros = false;

        BullishAnchorIndex.Name = "Bullish Prior Anchor Bar Index";
        BullishAnchorIndex.DrawStyle = DRAWSTYLE_IGNORE;
        BullishAnchorIndex.DrawZeros = false;

        BearishAnchorIndex.Name = "Bearish Prior Anchor Bar Index";
        BearishAnchorIndex.DrawStyle = DRAWSTYLE_IGNORE;
        BearishAnchorIndex.DrawZeros = false;

        ChartAndDataStatus.Name =
            "Status (1 = Valid, 0 = Missing Bid/Ask Data, -1 = Wrong Chart)";
        ChartAndDataStatus.DrawStyle = DRAWSTYLE_IGNORE;
        ChartAndDataStatus.DrawZeros = false;

        DeltaSourceInput.Name = "Delta Source";
        DeltaSourceInput.SetCustomInputStrings(
            "Trading-Day Cumulative Delta;"
            "Continuous Cumulative Delta;"
            "Rolling Delta Sum;"
            "Bar Delta");
        DeltaSourceInput.SetCustomInputIndex(DELTA_TRADING_DAY_CUMULATIVE);

        RollingDeltaLengthInput.Name =
            "Rolling Delta Length (Bars; Used Only for Rolling Source)";
        RollingDeltaLengthInput.SetInt(20);
        RollingDeltaLengthInput.SetIntLimits(1, 5000);

        ResetRollingAtTradingDayInput.Name =
            "Reset Rolling Delta at Start of Trading Day";
        ResetRollingAtTradingDayInput.SetYesNo(1);

        DeltaSmoothingLengthInput.Name =
            "Delta Smoothing Length (1 = None)";
        DeltaSmoothingLengthInput.SetInt(1);
        DeltaSmoothingLengthInput.SetIntLimits(1, 500);

        CurrentLeftStrengthInput.Name =
            "Current Signal Extreme Left Strength (Prior Bars Only)";
        CurrentLeftStrengthInput.SetInt(3);
        CurrentLeftStrengthInput.SetIntLimits(1, 50);

        PreviousAnchorRightStrengthInput.Name =
            "Previous Anchor Right Strength (Does Not Delay Current Signal)";
        PreviousAnchorRightStrengthInput.SetInt(2);
        PreviousAnchorRightStrengthInput.SetIntLimits(0, 50);

        MinimumBarsBetweenPivotsInput.Name =
            "Minimum Bars Between Current Signal and Prior Anchor";
        MinimumBarsBetweenPivotsInput.SetInt(5);
        MinimumBarsBetweenPivotsInput.SetIntLimits(1, 1000);

        MaximumBarsBetweenPivotsInput.Name =
            "Maximum Bars Between Current Signal and Prior Anchor";
        MaximumBarsBetweenPivotsInput.SetInt(80);
        MaximumBarsBetweenPivotsInput.SetIntLimits(2, 10000);

        AllowEqualPivotsInput.Name =
            "Allow Equal High/Low in Left-Side Extreme Tests";
        AllowEqualPivotsInput.SetYesNo(0);

        MinimumPriceExtensionPointsInput.Name =
            "Minimum Price Extension Beyond Prior Anchor (YM Points)";
        MinimumPriceExtensionPointsInput.SetFloat(1.0f);
        MinimumPriceExtensionPointsInput.SetFloatLimits(0.0f, 100000.0f);

        MinimumDeltaImprovementContractsInput.Name =
            "Minimum Absolute Delta Improvement (Contracts; 0 = Disabled)";
        MinimumDeltaImprovementContractsInput.SetFloat(0.0f);
        MinimumDeltaImprovementContractsInput.SetFloatLimits(
            0.0f,
            1000000000.0f);

        MinimumDeltaImprovementMultipleInput.Name =
            "Minimum Delta Improvement as Multiple of Average Absolute Bar Delta";
        MinimumDeltaImprovementMultipleInput.SetFloat(0.50f);
        MinimumDeltaImprovementMultipleInput.SetFloatLimits(0.0f, 1000.0f);

        AverageAbsoluteDeltaLookbackInput.Name =
            "Average Absolute Bar Delta Lookback (Bars)";
        AverageAbsoluteDeltaLookbackInput.SetInt(20);
        AverageAbsoluteDeltaLookbackInput.SetIntLimits(1, 5000);

        RequireSameTradingDayInput.Name =
            "Require Current Signal and Prior Anchor in Same Trading Day";
        RequireSameTradingDayInput.SetYesNo(1);

        SignalBarCloseFilterInput.Name =
            "Additional Signal-Bar Close Filter";
        SignalBarCloseFilterInput.SetCustomInputStrings(
            "None;"
            "Close Beyond Signal-Bar Midpoint;"
            "Close in Divergence Direction (Green/Red Candle)");
        SignalBarCloseFilterInput.SetCustomInputIndex(CLOSE_FILTER_NONE);

        MinimumSignalBarCloseMovePointsInput.Name =
            "Minimum Signal-Bar Close Distance from Extreme (YM Points)";
        MinimumSignalBarCloseMovePointsInput.SetFloat(0.0f);
        MinimumSignalBarCloseMovePointsInput.SetFloatLimits(
            0.0f,
            100000.0f);

        RequireSignalBarDeltaDirectionInput.Name =
            "Require Signal-Bar Delta in Reversal Direction";
        RequireSignalBarDeltaDirectionInput.SetYesNo(0);

        MarkerStyleInput.Name = "Marker Style on Immediate Signal Bar";
        MarkerStyleInput.SetCustomInputStrings(
            "Large Arrow;Small Arrow;Both Arrows");
        MarkerStyleInput.SetCustomInputIndex(MARK_LARGE_ARROW);

        ClosedBarsOnlyInput.Name =
            "Use Closed Bars Only (Recommended)";
        ClosedBarsOnlyInput.SetYesNo(1);

        LargeArrowOffsetPointsInput.Name =
            "Large Arrow Offset (YM Points)";
        LargeArrowOffsetPointsInput.SetFloat(5.0f);
        LargeArrowOffsetPointsInput.SetFloatLimits(0.0f, 10000.0f);

        SmallArrowOffsetPointsInput.Name =
            "Small Arrow Offset (YM Points)";
        SmallArrowOffsetPointsInput.SetFloat(2.0f);
        SmallArrowOffsetPointsInput.SetFloatLimits(0.0f, 10000.0f);

        AlertSoundNumberInput.Name = "Alert Sound Number (0 = Disabled)";
        AlertSoundNumberInput.SetInt(0);
        AlertSoundNumberInput.SetIntLimits(0, 150);

        RestrictToYMSymbolInput.Name =
            "Restrict Study to E-mini Dow YM/YMU Symbols";
        RestrictToYMSymbolInput.SetYesNo(1);

        RequireOnePointTickInput.Name = "Require YM 1-Point Tick Size";
        RequireOnePointTickInput.SetYesNo(1);

        return;
    }

    int& LastBullishAlertIndex = sc.GetPersistentInt(1);
    int& LastBearishAlertIndex = sc.GetPersistentInt(2);

    if (sc.IsFullRecalculation && sc.Index == 0)
    {
        LastBullishAlertIndex = -1;
        LastBearishAlertIndex = -1;
    }

    // Clear the arrays at the index currently being processed. The actual
    // just-closed signal index is cleared again below before evaluation.
    BullishMarker[sc.Index] = 0.0f;
    BearishMarker[sc.Index] = 0.0f;
    BullishSmallMarker[sc.Index] = 0.0f;
    BearishSmallMarker[sc.Index] = 0.0f;
    BullishPriceExtension[sc.Index] = 0.0f;
    BullishDeltaImprovement[sc.Index] = 0.0f;
    BearishPriceExtension[sc.Index] = 0.0f;
    BearishDeltaImprovement[sc.Index] = 0.0f;
    RequiredDeltaImprovement[sc.Index] = 0.0f;
    BullishAnchorIndex[sc.Index] = 0.0f;
    BearishAnchorIndex[sc.Index] = 0.0f;

    const bool SymbolIsValid =
        RestrictToYMSymbolInput.GetYesNo() == 0
        || IsYMEminiDowSymbol(sc.Symbol);

    const bool TickSizeIsValid =
        RequireOnePointTickInput.GetYesNo() == 0
        || IsYMOnePointTickSize(sc.TickSize);

    const double AskVolume =
        static_cast<double>(sc.AskVolume[sc.Index]);
    const double BidVolume =
        static_cast<double>(sc.BidVolume[sc.Index]);
    const double TotalVolume =
        static_cast<double>(sc.Volume[sc.Index]);

    const bool BidAskDataAvailable =
        TotalVolume <= 0.0 || AskVolume + BidVolume > 0.0;

    if (!SymbolIsValid || !TickSizeIsValid)
        ChartAndDataStatus[sc.Index] = -1.0f;
    else if (!BidAskDataAvailable)
        ChartAndDataStatus[sc.Index] = 0.0f;
    else
        ChartAndDataStatus[sc.Index] = 1.0f;

    const double CurrentBarDelta = AskVolume - BidVolume;
    BarDelta[sc.Index] = static_cast<float>(CurrentBarDelta);

    const bool IsNewTradingDay =
        sc.Index == 0
        || sc.GetTradingDayDate(sc.BaseDateTimeIn[sc.Index])
            != sc.GetTradingDayDate(sc.BaseDateTimeIn[sc.Index - 1]);

    if (sc.Index == 0 || IsNewTradingDay)
    {
        TradingDayCumulativeDelta[sc.Index] =
            static_cast<float>(CurrentBarDelta);
    }
    else
    {
        TradingDayCumulativeDelta[sc.Index] =
            TradingDayCumulativeDelta[sc.Index - 1]
            + static_cast<float>(CurrentBarDelta);
    }

    if (sc.Index == 0)
    {
        ContinuousCumulativeDelta[sc.Index] =
            static_cast<float>(CurrentBarDelta);
    }
    else
    {
        ContinuousCumulativeDelta[sc.Index] =
            ContinuousCumulativeDelta[sc.Index - 1]
            + static_cast<float>(CurrentBarDelta);
    }

    const int RollingLength = RollingDeltaLengthInput.GetInt();
    const bool ResetRollingAtTradingDay =
        ResetRollingAtTradingDayInput.GetYesNo() != 0;

    if (sc.Index == 0 || (ResetRollingAtTradingDay && IsNewTradingDay))
    {
        RollingDelta[sc.Index] = static_cast<float>(CurrentBarDelta);
    }
    else
    {
        double RollingValue =
            static_cast<double>(RollingDelta[sc.Index - 1])
            + CurrentBarDelta;

        const int RemovedIndex = sc.Index - RollingLength;
        if (RemovedIndex >= 0)
        {
            const bool RemovedBarIsInCurrentTradingDay =
                IsSameTradingDay(sc, RemovedIndex, sc.Index);

            if (!ResetRollingAtTradingDay
                || RemovedBarIsInCurrentTradingDay)
            {
                RollingValue -=
                    static_cast<double>(BarDelta[RemovedIndex]);
            }
        }

        RollingDelta[sc.Index] = static_cast<float>(RollingValue);
    }

    const int DeltaSource = DeltaSourceInput.GetIndex();

    if (DeltaSource == DELTA_CONTINUOUS_CUMULATIVE)
        SelectedDeltaRaw[sc.Index] = ContinuousCumulativeDelta[sc.Index];
    else if (DeltaSource == DELTA_ROLLING_SUM)
        SelectedDeltaRaw[sc.Index] = RollingDelta[sc.Index];
    else if (DeltaSource == DELTA_BAR)
        SelectedDeltaRaw[sc.Index] = BarDelta[sc.Index];
    else
        SelectedDeltaRaw[sc.Index] = TradingDayCumulativeDelta[sc.Index];

    const int SmoothingLength = DeltaSmoothingLengthInput.GetInt();
    int SmoothingStart =
        MaximumInt(0, sc.Index - SmoothingLength + 1);

    // Do not blend reset-based delta values from different trading days.
    if (DeltaSource == DELTA_TRADING_DAY_CUMULATIVE
        || (DeltaSource == DELTA_ROLLING_SUM
            && ResetRollingAtTradingDay))
    {
        const int CurrentTradingDay =
            sc.GetTradingDayDate(sc.BaseDateTimeIn[sc.Index]);

        while (SmoothingStart < sc.Index
            && sc.GetTradingDayDate(
                sc.BaseDateTimeIn[SmoothingStart]) != CurrentTradingDay)
        {
            ++SmoothingStart;
        }
    }

    double SmoothedDeltaSum = 0.0;
    int SmoothedDeltaCount = 0;

    for (int Index = SmoothingStart; Index <= sc.Index; ++Index)
    {
        SmoothedDeltaSum +=
            static_cast<double>(SelectedDeltaRaw[Index]);
        ++SmoothedDeltaCount;
    }

    SelectedDelta[sc.Index] =
        SmoothedDeltaCount > 0
        ? static_cast<float>(
            SmoothedDeltaSum
            / static_cast<double>(SmoothedDeltaCount))
        : SelectedDeltaRaw[sc.Index];

    if (!SymbolIsValid || !TickSizeIsValid || !BidAskDataAvailable)
        return;

    const int CurrentLeftStrength = CurrentLeftStrengthInput.GetInt();
    const int PreviousAnchorRightStrength =
        PreviousAnchorRightStrengthInput.GetInt();
    const int MinimumBarsBetween =
        MinimumBarsBetweenPivotsInput.GetInt();
    const int MaximumBarsBetween = MaximumInt(
        MaximumBarsBetweenPivotsInput.GetInt(),
        MinimumBarsBetween + 1);

    sc.DataStartIndex =
        CurrentLeftStrength
        + MaximumInt(
            MinimumBarsBetween,
            PreviousAnchorRightStrength + 1);

    int SignalIndex = sc.Index;

    if (ClosedBarsOnlyInput.GetYesNo() != 0
        && sc.GetBarHasClosedStatus(sc.Index)
            == BHCS_BAR_HAS_NOT_CLOSED)
    {
        SignalIndex = sc.Index - 1;
    }

    if (SignalIndex < CurrentLeftStrength)
        return;

    // Clear and recalculate the exact signal bar. During real-time updating,
    // this is normally the bar at sc.ArraySize - 2.
    BullishMarker[SignalIndex] = 0.0f;
    BearishMarker[SignalIndex] = 0.0f;
    BullishSmallMarker[SignalIndex] = 0.0f;
    BearishSmallMarker[SignalIndex] = 0.0f;
    BullishPriceExtension[SignalIndex] = 0.0f;
    BullishDeltaImprovement[SignalIndex] = 0.0f;
    BearishPriceExtension[SignalIndex] = 0.0f;
    BearishDeltaImprovement[SignalIndex] = 0.0f;
    RequiredDeltaImprovement[SignalIndex] = 0.0f;
    BullishAnchorIndex[SignalIndex] = 0.0f;
    BearishAnchorIndex[SignalIndex] = 0.0f;

    const bool AllowEqual =
        AllowEqualPivotsInput.GetYesNo() != 0;
    const bool RequireSameTradingDay =
        RequireSameTradingDayInput.GetYesNo() != 0;
    const double MinimumPriceExtension =
        static_cast<double>(MinimumPriceExtensionPointsInput.GetFloat());
    const double MinimumAbsoluteDeltaImprovement =
        static_cast<double>(
            MinimumDeltaImprovementContractsInput.GetFloat());
    const double MinimumDeltaImprovementMultiple =
        static_cast<double>(
            MinimumDeltaImprovementMultipleInput.GetFloat());
    const int AverageAbsoluteDeltaLookback =
        AverageAbsoluteDeltaLookbackInput.GetInt();
    const int SignalBarCloseFilter =
        SignalBarCloseFilterInput.GetIndex();
    const double MinimumSignalBarCloseMovePoints =
        static_cast<double>(
            MinimumSignalBarCloseMovePointsInput.GetFloat());
    const bool RequireSignalBarDeltaDirection =
        RequireSignalBarDeltaDirectionInput.GetYesNo() != 0;
    const int MarkerStyle = MarkerStyleInput.GetIndex();
    const double LargeArrowOffset =
        static_cast<double>(LargeArrowOffsetPointsInput.GetFloat())
        * static_cast<double>(sc.TickSize);
    const double SmallArrowOffset =
        static_cast<double>(SmallArrowOffsetPointsInput.GetFloat())
        * static_cast<double>(sc.TickSize);

    const double AverageAbsoluteDelta = AverageAbsoluteBarDelta(
        BarDelta,
        SignalIndex,
        AverageAbsoluteDeltaLookback);
    const double RelativeRequiredDelta =
        AverageAbsoluteDelta * MinimumDeltaImprovementMultiple;
    const double RequiredDelta = MaximumDouble(
        MinimumAbsoluteDeltaImprovement,
        RelativeRequiredDelta);

    RequiredDeltaImprovement[SignalIndex] =
        static_cast<float>(RequiredDelta);

    bool BullishSignal = false;
    bool BearishSignal = false;

    if (IsCurrentLeftSideLow(
            sc,
            SignalIndex,
            CurrentLeftStrength,
            AllowEqual))
    {
        const int PreviousAnchor = FindPreviousPivotLow(
            sc,
            SignalIndex,
            CurrentLeftStrength,
            PreviousAnchorRightStrength,
            MinimumBarsBetween,
            MaximumBarsBetween,
            AllowEqual,
            RequireSameTradingDay);

        if (PreviousAnchor >= 0)
        {
            const int DataValidationStart = MaximumInt(
                0,
                PreviousAnchor - SmoothingLength + 1);

            if (RangeHasValidBidAskData(
                    ChartAndDataStatus,
                    DataValidationStart,
                    SignalIndex))
            {
                const double PriceExtension =
                    static_cast<double>(sc.Low[PreviousAnchor])
                    - static_cast<double>(sc.Low[SignalIndex]);
                const double DeltaImprovement =
                    static_cast<double>(SelectedDelta[SignalIndex])
                    - static_cast<double>(SelectedDelta[PreviousAnchor]);

                BullishPriceExtension[SignalIndex] =
                    static_cast<float>(PriceExtension);
                BullishDeltaImprovement[SignalIndex] =
                    static_cast<float>(DeltaImprovement);
                BullishAnchorIndex[SignalIndex] =
                    static_cast<float>(PreviousAnchor);

                const bool PriceCondition =
                    PriceExtension > 0.0
                    && PriceExtension >= MinimumPriceExtension;
                const bool DeltaCondition =
                    DeltaImprovement > 0.0
                    && DeltaImprovement >= RequiredDelta;
                const bool CloseFilterCondition =
                    BullishSignalBarCloseFilterPasses(
                        sc,
                        SignalIndex,
                        SignalBarCloseFilter,
                        MinimumSignalBarCloseMovePoints);
                const bool SignalBarDeltaCondition =
                    !RequireSignalBarDeltaDirection
                    || BarDelta[SignalIndex] > 0.0f;

                BullishSignal =
                    PriceCondition
                    && DeltaCondition
                    && CloseFilterCondition
                    && SignalBarDeltaCondition;
            }
        }
    }

    if (IsCurrentLeftSideHigh(
            sc,
            SignalIndex,
            CurrentLeftStrength,
            AllowEqual))
    {
        const int PreviousAnchor = FindPreviousPivotHigh(
            sc,
            SignalIndex,
            CurrentLeftStrength,
            PreviousAnchorRightStrength,
            MinimumBarsBetween,
            MaximumBarsBetween,
            AllowEqual,
            RequireSameTradingDay);

        if (PreviousAnchor >= 0)
        {
            const int DataValidationStart = MaximumInt(
                0,
                PreviousAnchor - SmoothingLength + 1);

            if (RangeHasValidBidAskData(
                    ChartAndDataStatus,
                    DataValidationStart,
                    SignalIndex))
            {
                const double PriceExtension =
                    static_cast<double>(sc.High[SignalIndex])
                    - static_cast<double>(sc.High[PreviousAnchor]);
                const double DeltaImprovement =
                    static_cast<double>(SelectedDelta[PreviousAnchor])
                    - static_cast<double>(SelectedDelta[SignalIndex]);

                BearishPriceExtension[SignalIndex] =
                    static_cast<float>(PriceExtension);
                BearishDeltaImprovement[SignalIndex] =
                    static_cast<float>(DeltaImprovement);
                BearishAnchorIndex[SignalIndex] =
                    static_cast<float>(PreviousAnchor);

                const bool PriceCondition =
                    PriceExtension > 0.0
                    && PriceExtension >= MinimumPriceExtension;
                const bool DeltaCondition =
                    DeltaImprovement > 0.0
                    && DeltaImprovement >= RequiredDelta;
                const bool CloseFilterCondition =
                    BearishSignalBarCloseFilterPasses(
                        sc,
                        SignalIndex,
                        SignalBarCloseFilter,
                        MinimumSignalBarCloseMovePoints);
                const bool SignalBarDeltaCondition =
                    !RequireSignalBarDeltaDirection
                    || BarDelta[SignalIndex] < 0.0f;

                BearishSignal =
                    PriceCondition
                    && DeltaCondition
                    && CloseFilterCondition
                    && SignalBarDeltaCondition;
            }
        }
    }

    if (BullishSignal)
    {
        if (MarkerStyle == MARK_LARGE_ARROW
            || MarkerStyle == MARK_BOTH_ARROWS)
        {
            BullishMarker[SignalIndex] = static_cast<float>(
                static_cast<double>(sc.Low[SignalIndex])
                - LargeArrowOffset);
        }

        if (MarkerStyle == MARK_SMALL_ARROW
            || MarkerStyle == MARK_BOTH_ARROWS)
        {
            BullishSmallMarker[SignalIndex] = static_cast<float>(
                static_cast<double>(sc.Low[SignalIndex])
                - SmallArrowOffset);
        }
    }

    if (BearishSignal)
    {
        if (MarkerStyle == MARK_LARGE_ARROW
            || MarkerStyle == MARK_BOTH_ARROWS)
        {
            BearishMarker[SignalIndex] = static_cast<float>(
                static_cast<double>(sc.High[SignalIndex])
                + LargeArrowOffset);
        }

        if (MarkerStyle == MARK_SMALL_ARROW
            || MarkerStyle == MARK_BOTH_ARROWS)
        {
            BearishSmallMarker[SignalIndex] = static_cast<float>(
                static_cast<double>(sc.High[SignalIndex])
                + SmallArrowOffset);
        }
    }

    const int AlertSoundNumber = AlertSoundNumberInput.GetInt();
    const bool IsRecentLiveSignal =
        !sc.IsFullRecalculation
        && SignalIndex >= sc.ArraySize - 2;

    if (AlertSoundNumber > 0 && IsRecentLiveSignal)
    {
        if (BullishSignal && LastBullishAlertIndex != SignalIndex)
        {
            SCString AlertMessage;
            AlertMessage =
                "YMU/YM bullish delta divergence on closed bar";
            sc.SetAlert(
                AlertSoundNumber,
                SignalIndex,
                AlertMessage);
            LastBullishAlertIndex = SignalIndex;
        }

        if (BearishSignal && LastBearishAlertIndex != SignalIndex)
        {
            SCString AlertMessage;
            AlertMessage =
                "YMU/YM bearish delta divergence on closed bar";
            sc.SetAlert(
                AlertSoundNumber,
                SignalIndex,
                AlertMessage);
            LastBearishAlertIndex = SignalIndex;
        }
    }
}
