#include "sierrachart.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

SCDLLName("YMU YM E-mini Dow Delta Divergence Studies")

// -----------------------------------------------------------------------------
// YMU / YM E-mini Dow Confirmed Delta Divergence
//
// Regular bullish divergence:
//   Price makes a lower confirmed swing low while the selected delta measure
//   makes a higher low.
//
// Regular bearish divergence:
//   Price makes a higher confirmed swing high while the selected delta measure
//   makes a lower high.
//
// The study can use trading-day cumulative delta, continuous cumulative delta,
// a rolling sum of bar delta, or the individual bar delta. Bar delta is defined
// as Ask Volume minus Bid Volume.
//
// Swing pivots require Left Strength bars before the pivot and Right Strength
// bars after the pivot. Consequently, a marker on a pivot bar becomes known
// only after the required right-side bars are available. Optional confirmation
// markers show the bar on which that determination became available.
//
// This is an analytical study, not an automated trading system and not a claim
// that divergence necessarily predicts a reversal.
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

    enum ConfirmationMode
    {
        CONFIRM_PIVOT_ONLY = 0,
        CONFIRM_CLOSE_BEYOND_MIDPOINT = 1,
        CONFIRM_CLOSE_BEYOND_PIVOT_EXTREME = 2
    };

    enum MarkerPlacementMode
    {
        MARK_PIVOT_ONLY = 0,
        MARK_CONFIRMATION_ONLY = 1,
        MARK_BOTH = 2
    };

    bool IsYMEminiDowSymbol(const SCString& Symbol)
    {
        const char* Text = Symbol.GetChars();
        if (Text == NULL || Text[0] == '\0')
            return false;

        const size_t Length = std::strlen(Text);

        // Accept common forms such as YMU26-CME, YMZ26-CME, YM?##-CME,
        // CBOT.YMU26, and @YM. Explicitly avoid accepting MYM.
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

            // Prevent the YM substring in MYM from being accepted.
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

    bool IsConfirmedPivotLow(
        SCStudyInterfaceRef sc,
        const int PivotIndex,
        const int LeftStrength,
        const int RightStrength,
        const bool AllowEqualPivots)
    {
        if (PivotIndex - LeftStrength < 0
            || PivotIndex + RightStrength >= sc.ArraySize)
        {
            return false;
        }

        const float PivotLow = sc.Low[PivotIndex];

        // With equal pivots enabled, equality is allowed on the left side but
        // not the right side. This selects the last bar in an equal-low cluster.
        for (int Offset = 1; Offset <= LeftStrength; ++Offset)
        {
            const float OtherLow = sc.Low[PivotIndex - Offset];
            if (AllowEqualPivots)
            {
                if (PivotLow > OtherLow)
                    return false;
            }
            else if (PivotLow >= OtherLow)
            {
                return false;
            }
        }

        for (int Offset = 1; Offset <= RightStrength; ++Offset)
        {
            const float OtherLow = sc.Low[PivotIndex + Offset];
            if (PivotLow >= OtherLow)
                return false;
        }

        return true;
    }

    bool IsConfirmedPivotHigh(
        SCStudyInterfaceRef sc,
        const int PivotIndex,
        const int LeftStrength,
        const int RightStrength,
        const bool AllowEqualPivots)
    {
        if (PivotIndex - LeftStrength < 0
            || PivotIndex + RightStrength >= sc.ArraySize)
        {
            return false;
        }

        const float PivotHigh = sc.High[PivotIndex];

        // With equal pivots enabled, equality is allowed on the left side but
        // not the right side. This selects the last bar in an equal-high cluster.
        for (int Offset = 1; Offset <= LeftStrength; ++Offset)
        {
            const float OtherHigh = sc.High[PivotIndex - Offset];
            if (AllowEqualPivots)
            {
                if (PivotHigh < OtherHigh)
                    return false;
            }
            else if (PivotHigh <= OtherHigh)
            {
                return false;
            }
        }

        for (int Offset = 1; Offset <= RightStrength; ++Offset)
        {
            const float OtherHigh = sc.High[PivotIndex + Offset];
            if (PivotHigh <= OtherHigh)
                return false;
        }

        return true;
    }

    int FindPreviousPivotLow(
        SCStudyInterfaceRef sc,
        const int CurrentPivotIndex,
        const int LeftStrength,
        const int RightStrength,
        const int MinimumBarsBetweenPivots,
        const int MaximumBarsBetweenPivots,
        const bool AllowEqualPivots)
    {
        const int FirstCandidate =
            CurrentPivotIndex - MinimumBarsBetweenPivots;
        const int LastCandidate = std::max(
            LeftStrength,
            CurrentPivotIndex - MaximumBarsBetweenPivots);

        for (int Candidate = FirstCandidate;
             Candidate >= LastCandidate;
             --Candidate)
        {
            if (IsConfirmedPivotLow(
                    sc,
                    Candidate,
                    LeftStrength,
                    RightStrength,
                    AllowEqualPivots))
            {
                return Candidate;
            }
        }

        return -1;
    }

    int FindPreviousPivotHigh(
        SCStudyInterfaceRef sc,
        const int CurrentPivotIndex,
        const int LeftStrength,
        const int RightStrength,
        const int MinimumBarsBetweenPivots,
        const int MaximumBarsBetweenPivots,
        const bool AllowEqualPivots)
    {
        const int FirstCandidate =
            CurrentPivotIndex - MinimumBarsBetweenPivots;
        const int LastCandidate = std::max(
            LeftStrength,
            CurrentPivotIndex - MaximumBarsBetweenPivots);

        for (int Candidate = FirstCandidate;
             Candidate >= LastCandidate;
             --Candidate)
        {
            if (IsConfirmedPivotHigh(
                    sc,
                    Candidate,
                    LeftStrength,
                    RightStrength,
                    AllowEqualPivots))
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
        const int StartIndex = std::max(0, EndingIndex - Length + 1);
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
        for (int Index = StartIndex; Index <= EndIndex; ++Index)
        {
            if (Status[Index] <= 0.0f)
                return false;
        }

        return true;
    }

    bool BullishConfirmationPasses(
        SCStudyInterfaceRef sc,
        const int PivotIndex,
        const int ConfirmationIndex,
        const int Mode,
        const double MinimumMovePoints)
    {
        const double Close = sc.Close[ConfirmationIndex];
        const double PivotLow = sc.Low[PivotIndex];
        const double PivotHigh = sc.High[PivotIndex];

        if (Close - PivotLow < MinimumMovePoints)
            return false;

        if (Mode == CONFIRM_CLOSE_BEYOND_MIDPOINT)
            return Close > (PivotHigh + PivotLow) * 0.5;

        if (Mode == CONFIRM_CLOSE_BEYOND_PIVOT_EXTREME)
            return Close > PivotHigh;

        return true;
    }

    bool BearishConfirmationPasses(
        SCStudyInterfaceRef sc,
        const int PivotIndex,
        const int ConfirmationIndex,
        const int Mode,
        const double MinimumMovePoints)
    {
        const double Close = sc.Close[ConfirmationIndex];
        const double PivotLow = sc.Low[PivotIndex];
        const double PivotHigh = sc.High[PivotIndex];

        if (PivotHigh - Close < MinimumMovePoints)
            return false;

        if (Mode == CONFIRM_CLOSE_BEYOND_MIDPOINT)
            return Close < (PivotHigh + PivotLow) * 0.5;

        if (Mode == CONFIRM_CLOSE_BEYOND_PIVOT_EXTREME)
            return Close < PivotLow;

        return true;
    }
}

SCSFExport scsf_YMYMUConfirmedDeltaDivergence(SCStudyInterfaceRef sc)
{
    SCSubgraphRef BullishPivotMarker = sc.Subgraph[0];
    SCSubgraphRef BearishPivotMarker = sc.Subgraph[1];
    SCSubgraphRef BullishConfirmationMarker = sc.Subgraph[2];
    SCSubgraphRef BearishConfirmationMarker = sc.Subgraph[3];
    SCSubgraphRef BarDelta = sc.Subgraph[4];
    SCSubgraphRef TradingDayCumulativeDelta = sc.Subgraph[5];
    SCSubgraphRef ContinuousCumulativeDelta = sc.Subgraph[6];
    SCSubgraphRef RollingDelta = sc.Subgraph[7];
    SCSubgraphRef SelectedDeltaRaw = sc.Subgraph[8];
    SCSubgraphRef SelectedDelta = sc.Subgraph[9];
    SCSubgraphRef BullishPriceExtension = sc.Subgraph[10];
    SCSubgraphRef BullishDeltaImprovement = sc.Subgraph[11];
    SCSubgraphRef BearishPriceExtension = sc.Subgraph[12];
    SCSubgraphRef BearishDeltaImprovement = sc.Subgraph[13];
    SCSubgraphRef RequiredDeltaImprovement = sc.Subgraph[14];
    SCSubgraphRef ChartAndDataStatus = sc.Subgraph[15];

    SCInputRef DeltaSourceInput = sc.Input[0];
    SCInputRef RollingDeltaLengthInput = sc.Input[1];
    SCInputRef ResetRollingAtTradingDayInput = sc.Input[2];
    SCInputRef DeltaSmoothingLengthInput = sc.Input[3];
    SCInputRef LeftStrengthInput = sc.Input[4];
    SCInputRef RightStrengthInput = sc.Input[5];
    SCInputRef MinimumBarsBetweenPivotsInput = sc.Input[6];
    SCInputRef MaximumBarsBetweenPivotsInput = sc.Input[7];
    SCInputRef AllowEqualPivotsInput = sc.Input[8];
    SCInputRef MinimumPriceExtensionPointsInput = sc.Input[9];
    SCInputRef MinimumDeltaImprovementContractsInput = sc.Input[10];
    SCInputRef MinimumDeltaImprovementMultipleInput = sc.Input[11];
    SCInputRef AverageAbsoluteDeltaLookbackInput = sc.Input[12];
    SCInputRef RequireSameTradingDayInput = sc.Input[13];
    SCInputRef ConfirmationModeInput = sc.Input[14];
    SCInputRef MinimumConfirmationMovePointsInput = sc.Input[15];
    SCInputRef RequireConfirmationBarDeltaDirectionInput = sc.Input[16];
    SCInputRef MarkerPlacementInput = sc.Input[17];
    SCInputRef ClosedBarsOnlyInput = sc.Input[18];
    SCInputRef PivotArrowOffsetPointsInput = sc.Input[19];
    SCInputRef ConfirmationArrowOffsetPointsInput = sc.Input[20];
    SCInputRef AlertSoundNumberInput = sc.Input[21];
    SCInputRef RestrictToYMSymbolInput = sc.Input[22];
    SCInputRef RequireOnePointTickInput = sc.Input[23];

    if (sc.SetDefaults)
    {
        sc.GraphName = "YMU/YM Confirmed Delta Divergence";
        sc.StudyDescription =
            "Marks regular bullish delta divergence when YMU/YM price makes "
            "a lower confirmed swing low while the selected delta measure "
            "makes a higher low. Marks regular bearish delta divergence when "
            "price makes a higher confirmed swing high while delta makes a "
            "lower high. The default delta source is cumulative Ask Volume "
            "minus Bid Volume reset at the start of the chart trading day.";

        sc.AutoLoop = 1;
        sc.GraphRegion = 0;
        sc.ValueFormat = sc.BaseGraphValueFormat;
        sc.MaintainAdditionalChartDataArrays = 1;
        sc.AlertOnlyOncePerBar = 1;
        sc.ResetAlertOnNewBar = 1;

        BullishPivotMarker.Name = "Bullish Delta Divergence - Pivot";
        BullishPivotMarker.DrawStyle = DRAWSTYLE_ARROW_UP;
        BullishPivotMarker.PrimaryColor = RGB(0, 190, 0);
        BullishPivotMarker.LineWidth = 4;
        BullishPivotMarker.DrawZeros = false;

        BearishPivotMarker.Name = "Bearish Delta Divergence - Pivot";
        BearishPivotMarker.DrawStyle = DRAWSTYLE_ARROW_DOWN;
        BearishPivotMarker.PrimaryColor = RGB(220, 35, 35);
        BearishPivotMarker.LineWidth = 4;
        BearishPivotMarker.DrawZeros = false;

        BullishConfirmationMarker.Name =
            "Bullish Delta Divergence - Confirmation Bar";
        BullishConfirmationMarker.DrawStyle = DRAWSTYLE_ARROW_UP;
        BullishConfirmationMarker.PrimaryColor = RGB(0, 130, 0);
        BullishConfirmationMarker.LineWidth = 2;
        BullishConfirmationMarker.DrawZeros = false;

        BearishConfirmationMarker.Name =
            "Bearish Delta Divergence - Confirmation Bar";
        BearishConfirmationMarker.DrawStyle = DRAWSTYLE_ARROW_DOWN;
        BearishConfirmationMarker.PrimaryColor = RGB(170, 0, 0);
        BearishConfirmationMarker.LineWidth = 2;
        BearishConfirmationMarker.DrawZeros = false;

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
            "Bullish Divergence Price Extension (Points)";
        BullishPriceExtension.DrawStyle = DRAWSTYLE_IGNORE;
        BullishPriceExtension.DrawZeros = false;

        BullishDeltaImprovement.Name =
            "Bullish Divergence Delta Improvement";
        BullishDeltaImprovement.DrawStyle = DRAWSTYLE_IGNORE;
        BullishDeltaImprovement.DrawZeros = false;

        BearishPriceExtension.Name =
            "Bearish Divergence Price Extension (Points)";
        BearishPriceExtension.DrawStyle = DRAWSTYLE_IGNORE;
        BearishPriceExtension.DrawZeros = false;

        BearishDeltaImprovement.Name =
            "Bearish Divergence Delta Improvement";
        BearishDeltaImprovement.DrawStyle = DRAWSTYLE_IGNORE;
        BearishDeltaImprovement.DrawZeros = false;

        RequiredDeltaImprovement.Name =
            "Required Delta Improvement at Candidate Pivot";
        RequiredDeltaImprovement.DrawStyle = DRAWSTYLE_IGNORE;
        RequiredDeltaImprovement.DrawZeros = false;

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

        LeftStrengthInput.Name = "Swing Pivot Left Strength (Bars)";
        LeftStrengthInput.SetInt(3);
        LeftStrengthInput.SetIntLimits(1, 50);

        RightStrengthInput.Name =
            "Swing Pivot Right Strength / Confirmation Delay (Bars)";
        RightStrengthInput.SetInt(3);
        RightStrengthInput.SetIntLimits(1, 50);

        MinimumBarsBetweenPivotsInput.Name =
            "Minimum Bars Between Compared Pivots";
        MinimumBarsBetweenPivotsInput.SetInt(5);
        MinimumBarsBetweenPivotsInput.SetIntLimits(1, 1000);

        MaximumBarsBetweenPivotsInput.Name =
            "Maximum Bars Between Compared Pivots";
        MaximumBarsBetweenPivotsInput.SetInt(80);
        MaximumBarsBetweenPivotsInput.SetIntLimits(2, 10000);

        AllowEqualPivotsInput.Name =
            "Allow Equal High/Low Within Left Side of Pivot";
        AllowEqualPivotsInput.SetYesNo(0);

        MinimumPriceExtensionPointsInput.Name =
            "Minimum Price Extension Beyond Prior Pivot (YM Points)";
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
            "Require Both Pivots in Same Trading Day";
        RequireSameTradingDayInput.SetYesNo(1);

        ConfirmationModeInput.Name = "Additional Price Confirmation";
        ConfirmationModeInput.SetCustomInputStrings(
            "Confirmed Pivot Only;"
            "Confirmation Close Beyond Pivot-Bar Midpoint;"
            "Confirmation Close Beyond Pivot-Bar High/Low");
        ConfirmationModeInput.SetCustomInputIndex(CONFIRM_PIVOT_ONLY);

        MinimumConfirmationMovePointsInput.Name =
            "Minimum Confirmation Close Move Away from Pivot (YM Points)";
        MinimumConfirmationMovePointsInput.SetFloat(0.0f);
        MinimumConfirmationMovePointsInput.SetFloatLimits(
            0.0f,
            100000.0f);

        RequireConfirmationBarDeltaDirectionInput.Name =
            "Require Confirmation-Bar Delta in Divergence Direction";
        RequireConfirmationBarDeltaDirectionInput.SetYesNo(0);

        MarkerPlacementInput.Name = "Marker Placement";
        MarkerPlacementInput.SetCustomInputStrings(
            "Pivot Bar Only;Confirmation Bar Only;Both");
        MarkerPlacementInput.SetCustomInputIndex(MARK_BOTH);

        ClosedBarsOnlyInput.Name = "Use Closed Bars Only";
        ClosedBarsOnlyInput.SetYesNo(1);

        PivotArrowOffsetPointsInput.Name =
            "Pivot Arrow Offset (YM Points)";
        PivotArrowOffsetPointsInput.SetFloat(5.0f);
        PivotArrowOffsetPointsInput.SetFloatLimits(0.0f, 10000.0f);

        ConfirmationArrowOffsetPointsInput.Name =
            "Confirmation Arrow Offset (YM Points)";
        ConfirmationArrowOffsetPointsInput.SetFloat(2.0f);
        ConfirmationArrowOffsetPointsInput.SetFloatLimits(0.0f, 10000.0f);

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

    int& LastBullishAlertConfirmationIndex = sc.GetPersistentInt(1);
    int& LastBearishAlertConfirmationIndex = sc.GetPersistentInt(2);

    if (sc.IsFullRecalculation && sc.Index == 0)
    {
        LastBullishAlertConfirmationIndex = -1;
        LastBearishAlertConfirmationIndex = -1;
    }

    BullishPivotMarker[sc.Index] = 0.0f;
    BearishPivotMarker[sc.Index] = 0.0f;
    BullishConfirmationMarker[sc.Index] = 0.0f;
    BearishConfirmationMarker[sc.Index] = 0.0f;
    BullishPriceExtension[sc.Index] = 0.0f;
    BullishDeltaImprovement[sc.Index] = 0.0f;
    BearishPriceExtension[sc.Index] = 0.0f;
    BearishDeltaImprovement[sc.Index] = 0.0f;
    RequiredDeltaImprovement[sc.Index] = 0.0f;

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
                sc.GetTradingDayDate(sc.BaseDateTimeIn[RemovedIndex])
                == sc.GetTradingDayDate(sc.BaseDateTimeIn[sc.Index]);

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
        std::max(0, sc.Index - SmoothingLength + 1);

    // Do not blend a reset-based delta series with values from the prior
    // trading day when smoothing is enabled.
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

    const int LeftStrength = LeftStrengthInput.GetInt();
    const int RightStrength = RightStrengthInput.GetInt();
    const int MinimumBarsBetweenPivots =
        MinimumBarsBetweenPivotsInput.GetInt();
    const int MaximumBarsBetweenPivots = std::max(
        MaximumBarsBetweenPivotsInput.GetInt(),
        MinimumBarsBetweenPivots + 1);

    sc.DataStartIndex =
        LeftStrength + RightStrength + MinimumBarsBetweenPivots;

    int ConfirmationIndex = sc.Index;

    if (ClosedBarsOnlyInput.GetYesNo() != 0
        && sc.GetBarHasClosedStatus(sc.Index)
            == BHCS_BAR_HAS_NOT_CLOSED)
    {
        ConfirmationIndex = sc.Index - 1;
    }

    if (ConfirmationIndex < 0)
        return;

    const int PivotIndex = ConfirmationIndex - RightStrength;
    if (PivotIndex - LeftStrength < 0)
        return;

    // Re-evaluation of the latest completed bar is intentional. Clear the exact
    // candidate locations before recalculating so partial updates remain clean.
    BullishPivotMarker[PivotIndex] = 0.0f;
    BearishPivotMarker[PivotIndex] = 0.0f;
    BullishConfirmationMarker[ConfirmationIndex] = 0.0f;
    BearishConfirmationMarker[ConfirmationIndex] = 0.0f;
    BullishPriceExtension[PivotIndex] = 0.0f;
    BullishDeltaImprovement[PivotIndex] = 0.0f;
    BearishPriceExtension[PivotIndex] = 0.0f;
    BearishDeltaImprovement[PivotIndex] = 0.0f;
    RequiredDeltaImprovement[PivotIndex] = 0.0f;

    const bool AllowEqualPivots =
        AllowEqualPivotsInput.GetYesNo() != 0;
    const double MinimumPriceExtensionPoints =
        static_cast<double>(MinimumPriceExtensionPointsInput.GetFloat());
    const double MinimumAbsoluteDeltaImprovement =
        static_cast<double>(
            MinimumDeltaImprovementContractsInput.GetFloat());
    const double MinimumDeltaImprovementMultiple =
        static_cast<double>(
            MinimumDeltaImprovementMultipleInput.GetFloat());
    const int AverageAbsoluteDeltaLookback =
        AverageAbsoluteDeltaLookbackInput.GetInt();
    const bool RequireSameTradingDay =
        RequireSameTradingDayInput.GetYesNo() != 0;
    const int ConfirmationMode = ConfirmationModeInput.GetIndex();
    const double MinimumConfirmationMovePoints =
        static_cast<double>(
            MinimumConfirmationMovePointsInput.GetFloat());
    const bool RequireConfirmationDeltaDirection =
        RequireConfirmationBarDeltaDirectionInput.GetYesNo() != 0;
    const int MarkerPlacement = MarkerPlacementInput.GetIndex();
    const double PivotArrowOffset =
        static_cast<double>(PivotArrowOffsetPointsInput.GetFloat())
        * static_cast<double>(sc.TickSize);
    const double ConfirmationArrowOffset =
        static_cast<double>(ConfirmationArrowOffsetPointsInput.GetFloat())
        * static_cast<double>(sc.TickSize);

    const double AverageAbsoluteDelta = AverageAbsoluteBarDelta(
        BarDelta,
        PivotIndex,
        AverageAbsoluteDeltaLookback);
    const double RelativeRequiredDeltaImprovement =
        AverageAbsoluteDelta * MinimumDeltaImprovementMultiple;
    const double RequiredDelta = std::max(
        MinimumAbsoluteDeltaImprovement,
        RelativeRequiredDeltaImprovement);

    RequiredDeltaImprovement[PivotIndex] =
        static_cast<float>(RequiredDelta);

    bool BullishSignal = false;
    bool BearishSignal = false;

    if (IsConfirmedPivotLow(
            sc,
            PivotIndex,
            LeftStrength,
            RightStrength,
            AllowEqualPivots))
    {
        const int PreviousPivotIndex = FindPreviousPivotLow(
            sc,
            PivotIndex,
            LeftStrength,
            RightStrength,
            MinimumBarsBetweenPivots,
            MaximumBarsBetweenPivots,
            AllowEqualPivots);

        const int DataValidationStart =
            std::max(0, PreviousPivotIndex - SmoothingLength + 1);

        if (PreviousPivotIndex >= 0
            && RangeHasValidBidAskData(
                ChartAndDataStatus,
                DataValidationStart,
                PivotIndex))
        {
            const bool SameTradingDay =
                sc.GetTradingDayDate(
                    sc.BaseDateTimeIn[PreviousPivotIndex])
                == sc.GetTradingDayDate(
                    sc.BaseDateTimeIn[PivotIndex]);

            const double PriceExtension =
                static_cast<double>(sc.Low[PreviousPivotIndex])
                - static_cast<double>(sc.Low[PivotIndex]);
            const double DeltaImprovement =
                static_cast<double>(SelectedDelta[PivotIndex])
                - static_cast<double>(SelectedDelta[PreviousPivotIndex]);

            BullishPriceExtension[PivotIndex] =
                static_cast<float>(PriceExtension);
            BullishDeltaImprovement[PivotIndex] =
                static_cast<float>(DeltaImprovement);

            const bool PriceCondition =
                PriceExtension > 0.0
                && PriceExtension >= MinimumPriceExtensionPoints;
            const bool DeltaCondition =
                DeltaImprovement > 0.0
                && DeltaImprovement >= RequiredDelta;
            const bool DayCondition =
                !RequireSameTradingDay || SameTradingDay;
            const bool PriceConfirmationCondition =
                BullishConfirmationPasses(
                    sc,
                    PivotIndex,
                    ConfirmationIndex,
                    ConfirmationMode,
                    MinimumConfirmationMovePoints);
            const bool DeltaConfirmationCondition =
                !RequireConfirmationDeltaDirection
                || BarDelta[ConfirmationIndex] > 0.0f;

            BullishSignal =
                PriceCondition
                && DeltaCondition
                && DayCondition
                && PriceConfirmationCondition
                && DeltaConfirmationCondition;
        }
    }

    if (IsConfirmedPivotHigh(
            sc,
            PivotIndex,
            LeftStrength,
            RightStrength,
            AllowEqualPivots))
    {
        const int PreviousPivotIndex = FindPreviousPivotHigh(
            sc,
            PivotIndex,
            LeftStrength,
            RightStrength,
            MinimumBarsBetweenPivots,
            MaximumBarsBetweenPivots,
            AllowEqualPivots);

        const int DataValidationStart =
            std::max(0, PreviousPivotIndex - SmoothingLength + 1);

        if (PreviousPivotIndex >= 0
            && RangeHasValidBidAskData(
                ChartAndDataStatus,
                DataValidationStart,
                PivotIndex))
        {
            const bool SameTradingDay =
                sc.GetTradingDayDate(
                    sc.BaseDateTimeIn[PreviousPivotIndex])
                == sc.GetTradingDayDate(
                    sc.BaseDateTimeIn[PivotIndex]);

            const double PriceExtension =
                static_cast<double>(sc.High[PivotIndex])
                - static_cast<double>(sc.High[PreviousPivotIndex]);
            const double DeltaImprovement =
                static_cast<double>(SelectedDelta[PreviousPivotIndex])
                - static_cast<double>(SelectedDelta[PivotIndex]);

            BearishPriceExtension[PivotIndex] =
                static_cast<float>(PriceExtension);
            BearishDeltaImprovement[PivotIndex] =
                static_cast<float>(DeltaImprovement);

            const bool PriceCondition =
                PriceExtension > 0.0
                && PriceExtension >= MinimumPriceExtensionPoints;
            const bool DeltaCondition =
                DeltaImprovement > 0.0
                && DeltaImprovement >= RequiredDelta;
            const bool DayCondition =
                !RequireSameTradingDay || SameTradingDay;
            const bool PriceConfirmationCondition =
                BearishConfirmationPasses(
                    sc,
                    PivotIndex,
                    ConfirmationIndex,
                    ConfirmationMode,
                    MinimumConfirmationMovePoints);
            const bool DeltaConfirmationCondition =
                !RequireConfirmationDeltaDirection
                || BarDelta[ConfirmationIndex] < 0.0f;

            BearishSignal =
                PriceCondition
                && DeltaCondition
                && DayCondition
                && PriceConfirmationCondition
                && DeltaConfirmationCondition;
        }
    }

    if (BullishSignal)
    {
        if (MarkerPlacement == MARK_PIVOT_ONLY
            || MarkerPlacement == MARK_BOTH)
        {
            BullishPivotMarker[PivotIndex] = static_cast<float>(
                static_cast<double>(sc.Low[PivotIndex])
                - PivotArrowOffset);
        }

        if (MarkerPlacement == MARK_CONFIRMATION_ONLY
            || MarkerPlacement == MARK_BOTH)
        {
            BullishConfirmationMarker[ConfirmationIndex] =
                static_cast<float>(
                    static_cast<double>(sc.Low[ConfirmationIndex])
                    - ConfirmationArrowOffset);
        }
    }

    if (BearishSignal)
    {
        if (MarkerPlacement == MARK_PIVOT_ONLY
            || MarkerPlacement == MARK_BOTH)
        {
            BearishPivotMarker[PivotIndex] = static_cast<float>(
                static_cast<double>(sc.High[PivotIndex])
                + PivotArrowOffset);
        }

        if (MarkerPlacement == MARK_CONFIRMATION_ONLY
            || MarkerPlacement == MARK_BOTH)
        {
            BearishConfirmationMarker[ConfirmationIndex] =
                static_cast<float>(
                    static_cast<double>(sc.High[ConfirmationIndex])
                    + ConfirmationArrowOffset);
        }
    }

    const int AlertSoundNumber = AlertSoundNumberInput.GetInt();
    const bool IsRecentLiveConfirmation =
        !sc.IsFullRecalculation
        && ConfirmationIndex >= sc.ArraySize - 2;

    if (AlertSoundNumber > 0 && IsRecentLiveConfirmation)
    {
        if (BullishSignal
            && LastBullishAlertConfirmationIndex != ConfirmationIndex)
        {
            sc.SetAlert(
                AlertSoundNumber,
                "YMU/YM bullish delta divergence confirmed");
            LastBullishAlertConfirmationIndex = ConfirmationIndex;
        }

        if (BearishSignal
            && LastBearishAlertConfirmationIndex != ConfirmationIndex)
        {
            sc.SetAlert(
                AlertSoundNumber,
                "YMU/YM bearish delta divergence confirmed");
            LastBearishAlertConfirmationIndex = ConfirmationIndex;
        }
    }
}
