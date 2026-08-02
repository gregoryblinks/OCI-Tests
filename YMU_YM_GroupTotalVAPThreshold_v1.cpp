#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "sierrachart.h"

SCDLLName("YMU YM Group Total Volume At Price Threshold Studies v1")

// -----------------------------------------------------------------------------
// YMU / YM Group Total Volume-at-Price Threshold Extension Lines
//
// This study is intentionally similar in workflow to Sierra Chart's
// Volume at Price Threshold Alert V2 study, with one important change:
//
//   1. Individual Volume-at-Price levels are tested against a threshold.
//   2. Consecutive qualifying price levels are combined into a group.
//   3. If the group meets the Minimum Adjacent Group Size, Ask Volume and
//      Bid Volume are summed across every qualifying price in that group.
//   4. The extension line is colored and labelled according to the larger
//      group total:
//          Ask Volume > Bid Volume  -> buyer-dominant group
//          Bid Volume > Ask Volume  -> seller-dominant group
//          otherwise                -> neutral / near-tie group
//
// Ask Volume represents trades executed at the Ask (aggressive buyers).
// Bid Volume represents trades executed at the Bid (aggressive sellers).
// The result describes executed order flow at the qualifying prices. It does
// not identify the intentions or identity of market participants.
// -----------------------------------------------------------------------------

namespace
{
    enum ComparisonMethod
    {
        COMPARE_BID_VOLUME = 0,
        COMPARE_ASK_VOLUME = 1,
        COMPARE_TOTAL_VOLUME = 2,
        COMPARE_NUMBER_OF_TRADES = 3,
        COMPARE_ABSOLUTE_SAME_PRICE_DIFFERENCE = 4,
        COMPARE_ABSOLUTE_DIAGONAL_DIFFERENCE = 5,
        COMPARE_EITHER_SIDE_SAME_PRICE_RATIO = 6,
        COMPARE_EITHER_SIDE_DIAGONAL_RATIO = 7,
        COMPARE_BID_AND_ASK_SEPARATELY = 8
    };

    enum GroupSide
    {
        GROUP_NEUTRAL = 0,
        GROUP_BUYERS = 1,
        GROUP_SELLERS = -1
    };

    enum NearTieAction
    {
        NEAR_TIE_SKIP = 0,
        NEAR_TIE_DRAW_NEUTRAL = 1
    };

    enum ExtensionLineMode
    {
        LINE_AT_DOMINANT_SIDE_EDGE = 0,
        LINE_AT_LOWEST_GROUP_PRICE = 1,
        LINE_AT_HIGHEST_GROUP_PRICE = 2,
        LINE_AT_CENTER_GROUP_PRICE = 3,
        LINE_AT_VOLUME_WEIGHTED_GROUP_PRICE = 4,
        LINE_AT_ALL_GROUP_PRICES = 5,
        LINE_AS_TRANSPARENT_GROUP_ZONE = 6
    };

    enum LabelDetailMode
    {
        LABEL_SIDE_ONLY = 0,
        LABEL_SIDE_AND_GROUP_SIZE = 1,
        LABEL_SIDE_AND_TOTALS = 2,
        LABEL_FULL_DETAILS = 3
    };

    struct LevelData
    {
        int PriceInTicks = 0;
        uint64_t BidVolume = 0;
        uint64_t AskVolume = 0;
        uint64_t TotalVolume = 0;
        uint64_t NumberOfTrades = 0;
        bool Qualifies = false;
    };

    const int GROUP_LINE_ID_BASE = 100000;
    const int PRICE_LINE_ID_BASE = 200000;

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

    bool MeetsUnsignedThreshold(const uint64_t Value, const int Threshold)
    {
        if (Threshold > 0)
            return Value >= static_cast<uint64_t>(Threshold);

        // This follows the useful zero-print behavior of the built-in study:
        // a threshold of zero means an equality comparison with zero.
        return Value == 0;
    }

    double RatioPercent(
        const uint64_t Numerator,
        const uint64_t Denominator,
        const bool EnableZeroCompares,
        const int ZeroCompareAction)
    {
        if (Denominator > 0)
        {
            return static_cast<double>(Numerator)
                / static_cast<double>(Denominator)
                * 100.0;
        }

        if (!EnableZeroCompares || Numerator == 0)
            return 0.0;

        if (ZeroCompareAction == 1)
            return 1000.0;

        // Treat zero denominator as one for the calculation.
        return static_cast<double>(Numerator) * 100.0;
    }

    double GroupDominancePercent(
        const uint64_t DominantVolume,
        const uint64_t OpposingVolume)
    {
        if (OpposingVolume > 0)
        {
            return static_cast<double>(DominantVolume)
                / static_cast<double>(OpposingVolume)
                * 100.0;
        }

        return DominantVolume > 0 ? 1000000.0 : 100.0;
    }

    int DetermineGroupSide(
        const uint64_t AskVolume,
        const uint64_t BidVolume,
        const uint64_t MinimumNetDifference,
        const double MinimumDominancePercent,
        double& DominancePercent)
    {
        DominancePercent = 100.0;

        if (AskVolume > BidVolume)
        {
            const uint64_t Difference = AskVolume - BidVolume;
            DominancePercent = GroupDominancePercent(AskVolume, BidVolume);

            if (Difference >= MinimumNetDifference
                && DominancePercent >= MinimumDominancePercent)
            {
                return GROUP_BUYERS;
            }
        }
        else if (BidVolume > AskVolume)
        {
            const uint64_t Difference = BidVolume - AskVolume;
            DominancePercent = GroupDominancePercent(BidVolume, AskVolume);

            if (Difference >= MinimumNetDifference
                && DominancePercent >= MinimumDominancePercent)
            {
                return GROUP_SELLERS;
            }
        }

        return GROUP_NEUTRAL;
    }

    SCString BuildGroupLabel(
        const int Side,
        const int GroupSize,
        const uint64_t AskVolume,
        const uint64_t BidVolume,
        const int64_t Delta,
        const double DominancePercent,
        const int DetailMode)
    {
        const char* SideText = "NEUTRAL";
        if (Side == GROUP_BUYERS)
            SideText = "BUYERS";
        else if (Side == GROUP_SELLERS)
            SideText = "SELLERS";

        SCString Label;

        if (DetailMode == LABEL_SIDE_ONLY)
        {
            Label.Format("%s", SideText);
        }
        else if (DetailMode == LABEL_SIDE_AND_GROUP_SIZE)
        {
            Label.Format("%s G=%d", SideText, GroupSize);
        }
        else if (DetailMode == LABEL_SIDE_AND_TOTALS)
        {
            Label.Format(
                "%s Ask=%llu Bid=%llu",
                SideText,
                static_cast<unsigned long long>(AskVolume),
                static_cast<unsigned long long>(BidVolume));
        }
        else
        {
            Label.Format(
                "%s G=%d Ask=%llu Bid=%llu D=%+lld R=%.0f%%",
                SideText,
                GroupSize,
                static_cast<unsigned long long>(AskVolume),
                static_cast<unsigned long long>(BidVolume),
                static_cast<long long>(Delta),
                DominancePercent);
        }

        return Label;
    }

    int GetAnchorIndex(
        const std::vector<LevelData>& Levels,
        const int GroupStart,
        const int GroupEnd,
        const int Side,
        const int LineMode)
    {
        if (LineMode == LINE_AT_LOWEST_GROUP_PRICE)
            return GroupStart;

        if (LineMode == LINE_AT_HIGHEST_GROUP_PRICE)
            return GroupEnd;

        if (LineMode == LINE_AT_CENTER_GROUP_PRICE)
            return GroupStart + (GroupEnd - GroupStart) / 2;

        if (LineMode == LINE_AT_DOMINANT_SIDE_EDGE)
        {
            if (Side == GROUP_BUYERS)
                return GroupStart;
            if (Side == GROUP_SELLERS)
                return GroupEnd;

            return GroupStart + (GroupEnd - GroupStart) / 2;
        }

        if (LineMode == LINE_AT_VOLUME_WEIGHTED_GROUP_PRICE)
        {
            long double WeightedTicks = 0.0;
            uint64_t WeightTotal = 0;

            for (int Index = GroupStart; Index <= GroupEnd; ++Index)
            {
                const uint64_t Weight =
                    Levels[Index].AskVolume + Levels[Index].BidVolume;

                WeightedTicks +=
                    static_cast<long double>(Levels[Index].PriceInTicks)
                    * static_cast<long double>(Weight);
                WeightTotal += Weight;
            }

            if (WeightTotal == 0)
                return GroupStart + (GroupEnd - GroupStart) / 2;

            const int TargetTicks = static_cast<int>(std::llround(
                WeightedTicks / static_cast<long double>(WeightTotal)));

            int BestIndex = GroupStart;
            int BestDistance = std::abs(
                Levels[GroupStart].PriceInTicks - TargetTicks);

            for (int Index = GroupStart + 1; Index <= GroupEnd; ++Index)
            {
                const int Distance =
                    std::abs(Levels[Index].PriceInTicks - TargetTicks);

                if (Distance < BestDistance)
                {
                    BestDistance = Distance;
                    BestIndex = Index;
                }
            }

            return BestIndex;
        }

        return GroupStart + (GroupEnd - GroupStart) / 2;
    }

    void AddExtensionLine(
        SCStudyInterfaceRef sc,
        const int BarIndex,
        const int LineID,
        const float LineValue,
        const bool UseRange,
        const float LineValue2,
        const uint32_t LineColor,
        const int LineWidth,
        const int LineStyle,
        const bool DrawValueLabel,
        const bool DrawNameLabel,
        const SCString& NameLabel,
        const bool AlwaysExtendToEndOfChart,
        const bool PerformCloseCrossoverComparison,
        const int TransparencyLevel)
    {
        n_ACSIL::s_LineUntilFutureIntersection Line;
        Line.StartBarIndex = BarIndex;
        Line.LineIDForBar = LineID;
        Line.LineValue = LineValue;
        Line.LineColor = LineColor;
        Line.LineWidth = static_cast<unsigned short>(
            std::max(1, LineWidth));
        Line.LineStyle = static_cast<unsigned short>(LineStyle);
        Line.DrawValueLabel = DrawValueLabel ? 1 : 0;
        Line.DrawNameLabel = DrawNameLabel ? 1 : 0;
        Line.NameLabel = NameLabel;
        Line.AlwaysExtendToEndOfChart =
            AlwaysExtendToEndOfChart ? 1 : 0;
        Line.PerformCloseCrossoverComparison =
            PerformCloseCrossoverComparison ? 1 : 0;

        if (UseRange)
        {
            Line.UseLineValue2 = 1;
            Line.LineValue2ForRange = LineValue2;
            Line.TransparencyLevel = std::max(
                0,
                std::min(100, TransparencyLevel));
        }

        sc.AddLineUntilFutureIntersectionEx(Line);
    }

    void DeletePossibleLinesForBar(
        SCStudyInterfaceRef sc,
        const int BarIndex,
        const int NumberOfPriceLevels)
    {
        for (int Index = 0; Index < NumberOfPriceLevels; ++Index)
        {
            sc.DeleteLineUntilFutureIntersection(
                BarIndex,
                GROUP_LINE_ID_BASE + Index);
            sc.DeleteLineUntilFutureIntersection(
                BarIndex,
                PRICE_LINE_ID_BASE + Index);
        }
    }

    void MarkQualifyingLevels(
        std::vector<LevelData>& Levels,
        const int ComparisonMethodIndex,
        const int VolumeOrDifferenceThreshold,
        const int PercentageThreshold,
        const int AdditionalAskVolumeThreshold,
        const int MinimumTotalVolumeForRatios,
        const bool EnableZeroCompares,
        const int ZeroCompareAction)
    {
        const int LevelCount = static_cast<int>(Levels.size());

        if (ComparisonMethodIndex == COMPARE_BID_VOLUME
            || ComparisonMethodIndex == COMPARE_ASK_VOLUME
            || ComparisonMethodIndex == COMPARE_TOTAL_VOLUME
            || ComparisonMethodIndex == COMPARE_NUMBER_OF_TRADES
            || ComparisonMethodIndex
                == COMPARE_ABSOLUTE_SAME_PRICE_DIFFERENCE
            || ComparisonMethodIndex
                == COMPARE_EITHER_SIDE_SAME_PRICE_RATIO
            || ComparisonMethodIndex == COMPARE_BID_AND_ASK_SEPARATELY)
        {
            for (int Index = 0; Index < LevelCount; ++Index)
            {
                LevelData& Level = Levels[Index];

                if (ComparisonMethodIndex == COMPARE_BID_VOLUME)
                {
                    Level.Qualifies = MeetsUnsignedThreshold(
                        Level.BidVolume,
                        VolumeOrDifferenceThreshold);
                }
                else if (ComparisonMethodIndex == COMPARE_ASK_VOLUME)
                {
                    Level.Qualifies = MeetsUnsignedThreshold(
                        Level.AskVolume,
                        VolumeOrDifferenceThreshold);
                }
                else if (ComparisonMethodIndex == COMPARE_TOTAL_VOLUME)
                {
                    Level.Qualifies = MeetsUnsignedThreshold(
                        Level.TotalVolume,
                        VolumeOrDifferenceThreshold);
                }
                else if (ComparisonMethodIndex == COMPARE_NUMBER_OF_TRADES)
                {
                    Level.Qualifies = MeetsUnsignedThreshold(
                        Level.NumberOfTrades,
                        VolumeOrDifferenceThreshold);
                }
                else if (ComparisonMethodIndex
                    == COMPARE_ABSOLUTE_SAME_PRICE_DIFFERENCE)
                {
                    const uint64_t Difference =
                        Level.AskVolume >= Level.BidVolume
                        ? Level.AskVolume - Level.BidVolume
                        : Level.BidVolume - Level.AskVolume;

                    if (VolumeOrDifferenceThreshold > 0)
                    {
                        Level.Qualifies = Difference
                            >= static_cast<uint64_t>(
                                VolumeOrDifferenceThreshold);
                    }
                    else
                    {
                        Level.Qualifies = Difference == 0;
                    }
                }
                else if (ComparisonMethodIndex
                    == COMPARE_EITHER_SIDE_SAME_PRICE_RATIO)
                {
                    if (Level.TotalVolume
                        < static_cast<uint64_t>(
                            std::max(0, MinimumTotalVolumeForRatios)))
                    {
                        continue;
                    }

                    const double AskRatio = RatioPercent(
                        Level.AskVolume,
                        Level.BidVolume,
                        EnableZeroCompares,
                        ZeroCompareAction);
                    const double BidRatio = RatioPercent(
                        Level.BidVolume,
                        Level.AskVolume,
                        EnableZeroCompares,
                        ZeroCompareAction);

                    Level.Qualifies =
                        AskRatio >= PercentageThreshold
                        || BidRatio >= PercentageThreshold;
                }
                else if (ComparisonMethodIndex
                    == COMPARE_BID_AND_ASK_SEPARATELY)
                {
                    Level.Qualifies =
                        MeetsUnsignedThreshold(
                            Level.BidVolume,
                            VolumeOrDifferenceThreshold)
                        && MeetsUnsignedThreshold(
                            Level.AskVolume,
                            AdditionalAskVolumeThreshold);
                }
            }

            return;
        }

        // Diagonal comparisons use Bid Volume at the lower price and Ask
        // Volume at the next higher price. Gaps in PriceInTicks are not treated
        // as adjacent price levels.
        for (int LowerIndex = 0; LowerIndex + 1 < LevelCount; ++LowerIndex)
        {
            const int UpperIndex = LowerIndex + 1;
            LevelData& LowerLevel = Levels[LowerIndex];
            LevelData& UpperLevel = Levels[UpperIndex];

            if (UpperLevel.PriceInTicks != LowerLevel.PriceInTicks + 1)
                continue;

            if (ComparisonMethodIndex
                == COMPARE_ABSOLUTE_DIAGONAL_DIFFERENCE)
            {
                const int64_t Difference =
                    static_cast<int64_t>(UpperLevel.AskVolume)
                    - static_cast<int64_t>(LowerLevel.BidVolume);

                if (VolumeOrDifferenceThreshold > 0)
                {
                    if (Difference
                        >= static_cast<int64_t>(
                            VolumeOrDifferenceThreshold))
                    {
                        UpperLevel.Qualifies = true;
                    }
                    else if (-Difference
                        >= static_cast<int64_t>(
                            VolumeOrDifferenceThreshold))
                    {
                        LowerLevel.Qualifies = true;
                    }
                }
                else if (Difference == 0)
                {
                    // An equality comparison at zero has no directional side.
                    // Mark the lower price only to keep the result deterministic.
                    LowerLevel.Qualifies = true;
                }
            }
            else if (ComparisonMethodIndex
                == COMPARE_EITHER_SIDE_DIAGONAL_RATIO)
            {
                const uint64_t MinimumTotal = static_cast<uint64_t>(
                    std::max(0, MinimumTotalVolumeForRatios));

                if (LowerLevel.TotalVolume < MinimumTotal
                    || UpperLevel.TotalVolume < MinimumTotal)
                {
                    continue;
                }

                const double AskDominanceRatio = RatioPercent(
                    UpperLevel.AskVolume,
                    LowerLevel.BidVolume,
                    EnableZeroCompares,
                    ZeroCompareAction);
                const double BidDominanceRatio = RatioPercent(
                    LowerLevel.BidVolume,
                    UpperLevel.AskVolume,
                    EnableZeroCompares,
                    ZeroCompareAction);

                if (AskDominanceRatio >= PercentageThreshold)
                    UpperLevel.Qualifies = true;

                if (BidDominanceRatio >= PercentageThreshold)
                    LowerLevel.Qualifies = true;
            }
        }
    }
}

SCSFExport scsf_YMUGroupTotalVAPThresholdExtensionLines(
    SCStudyInterfaceRef sc)
{
    SCSubgraphRef BuyerLineProperties = sc.Subgraph[0];
    SCSubgraphRef SellerLineProperties = sc.Subgraph[1];
    SCSubgraphRef NeutralLineProperties = sc.Subgraph[2];
    SCSubgraphRef BuyerGroupCount = sc.Subgraph[3];
    SCSubgraphRef SellerGroupCount = sc.Subgraph[4];
    SCSubgraphRef NeutralGroupCount = sc.Subgraph[5];
    SCSubgraphRef StrongestSignedGroupDelta = sc.Subgraph[6];
    SCSubgraphRef StrongestGroupDominancePercent = sc.Subgraph[7];
    SCSubgraphRef QualifyingGroupLevelCount = sc.Subgraph[8];
    SCSubgraphRef ChartCompatibility = sc.Subgraph[9];

    SCInputRef ComparisonMethodInput = sc.Input[0];
    SCInputRef VolumeOrDifferenceThresholdInput = sc.Input[1];
    SCInputRef PercentageThresholdInput = sc.Input[2];
    SCInputRef AdditionalAskVolumeThresholdInput = sc.Input[3];
    SCInputRef MinimumTotalVolumeForRatiosInput = sc.Input[4];
    SCInputRef EnableZeroComparesInput = sc.Input[5];
    SCInputRef ZeroCompareActionInput = sc.Input[6];
    SCInputRef MinimumAdjacentGroupSizeInput = sc.Input[7];
    SCInputRef MinimumClassifiedGroupVolumeInput = sc.Input[8];
    SCInputRef MinimumGroupNetDifferenceInput = sc.Input[9];
    SCInputRef MinimumGroupDominancePercentInput = sc.Input[10];
    SCInputRef NearTieActionInput = sc.Input[11];
    SCInputRef ExtensionLineModeInput = sc.Input[12];
    SCInputRef ExtendToEndOfChartInput = sc.Input[13];
    SCInputRef CloseCrossoverForIntersectionInput = sc.Input[14];
    SCInputRef DrawNameLabelInput = sc.Input[15];
    SCInputRef DrawValueLabelInput = sc.Input[16];
    SCInputRef LabelDetailInput = sc.Input[17];
    SCInputRef ZoneTransparencyInput = sc.Input[18];
    SCInputRef ClosedBarsOnlyInput = sc.Input[19];
    SCInputRef NumberOfDaysToCalculateInput = sc.Input[20];
    SCInputRef BuyerAlertSoundInput = sc.Input[21];
    SCInputRef SellerAlertSoundInput = sc.Input[22];
    SCInputRef RestrictToYMSymbolInput = sc.Input[23];
    SCInputRef RequireOnePointTickInput = sc.Input[24];

    if (sc.SetDefaults)
    {
        sc.GraphName =
            "YMU/YM Group Total VAP Threshold Extension Lines v1";
        sc.StudyDescription =
            "Groups adjacent Volume-at-Price threshold alerts, sums Ask and "
            "Bid Volume across each complete qualifying group, and colors the "
            "extension line according to whether aggressive buyers (Ask "
            "Volume) or aggressive sellers (Bid Volume) have the larger "
            "group total. Designed for E-mini Dow YM charts, including YMU.";

        sc.GraphRegion = 0;
        sc.AutoLoop = 0;
        sc.MaintainVolumeAtPriceData = 1;
        sc.ValueFormat = VALUEFORMAT_INHERITED;

        BuyerLineProperties.Name =
            "Buyer-Dominant Group Line Properties";
        BuyerLineProperties.DrawStyle = DRAWSTYLE_IGNORE;
        BuyerLineProperties.PrimaryColor = RGB(0, 128, 255);
        BuyerLineProperties.LineWidth = 2;
        BuyerLineProperties.LineStyle = LINESTYLE_SOLID;
        BuyerLineProperties.DrawZeros = false;

        SellerLineProperties.Name =
            "Seller-Dominant Group Line Properties";
        SellerLineProperties.DrawStyle = DRAWSTYLE_IGNORE;
        SellerLineProperties.PrimaryColor = RGB(220, 40, 40);
        SellerLineProperties.LineWidth = 2;
        SellerLineProperties.LineStyle = LINESTYLE_SOLID;
        SellerLineProperties.DrawZeros = false;

        NeutralLineProperties.Name =
            "Neutral Group Line Properties";
        NeutralLineProperties.DrawStyle = DRAWSTYLE_IGNORE;
        NeutralLineProperties.PrimaryColor = RGB(140, 140, 140);
        NeutralLineProperties.LineWidth = 1;
        NeutralLineProperties.LineStyle = LINESTYLE_DASH;
        NeutralLineProperties.DrawZeros = false;

        BuyerGroupCount.Name = "Buyer-Dominant Group Count";
        BuyerGroupCount.DrawStyle = DRAWSTYLE_IGNORE;
        BuyerGroupCount.DrawZeros = false;

        SellerGroupCount.Name = "Seller-Dominant Group Count";
        SellerGroupCount.DrawStyle = DRAWSTYLE_IGNORE;
        SellerGroupCount.DrawZeros = false;

        NeutralGroupCount.Name = "Neutral Group Count";
        NeutralGroupCount.DrawStyle = DRAWSTYLE_IGNORE;
        NeutralGroupCount.DrawZeros = false;

        StrongestSignedGroupDelta.Name =
            "Strongest Signed Group Delta (Ask-Bid)";
        StrongestSignedGroupDelta.DrawStyle = DRAWSTYLE_IGNORE;
        StrongestSignedGroupDelta.DrawZeros = false;

        StrongestGroupDominancePercent.Name =
            "Strongest Group Dominance Percentage";
        StrongestGroupDominancePercent.DrawStyle = DRAWSTYLE_IGNORE;
        StrongestGroupDominancePercent.DrawZeros = false;

        QualifyingGroupLevelCount.Name =
            "Price Levels in Qualifying Groups";
        QualifyingGroupLevelCount.DrawStyle = DRAWSTYLE_IGNORE;
        QualifyingGroupLevelCount.DrawZeros = false;

        ChartCompatibility.Name =
            "Chart/Data Compatibility (1 Valid, 0 Missing VAP, -1 Wrong YM)";
        ChartCompatibility.DrawStyle = DRAWSTYLE_IGNORE;
        ChartCompatibility.DrawZeros = false;

        ComparisonMethodInput.Name = "Comparison Method";
        ComparisonMethodInput.SetCustomInputStrings(
            "Bid Volume;"
            "Ask Volume;"
            "Total Volume;"
            "Number of Trades;"
            "Absolute Ask-Bid Difference;"
            "Absolute Diagonal Ask-Bid Difference;"
            "Either-Side Ask/Bid Ratio;"
            "Either-Side Diagonal Ask/Bid Ratio;"
            "Bid Volume and Ask Volume Separately");
        ComparisonMethodInput.SetCustomInputIndex(
            COMPARE_EITHER_SIDE_DIAGONAL_RATIO);

        VolumeOrDifferenceThresholdInput.Name =
            "Volume or Difference Threshold";
        VolumeOrDifferenceThresholdInput.SetInt(100);
        VolumeOrDifferenceThresholdInput.SetIntLimits(0, 2000000000);

        PercentageThresholdInput.Name =
            "Percentage Threshold for Ratio Comparisons";
        PercentageThresholdInput.SetInt(300);
        PercentageThresholdInput.SetIntLimits(100, 1000000);

        AdditionalAskVolumeThresholdInput.Name =
            "Additional Ask Volume Threshold";
        AdditionalAskVolumeThresholdInput.SetInt(100);
        AdditionalAskVolumeThresholdInput.SetIntLimits(0, 2000000000);

        MinimumTotalVolumeForRatiosInput.Name =
            "Minimum Total Volume at Each Compared Price for Ratios";
        MinimumTotalVolumeForRatiosInput.SetInt(10);
        MinimumTotalVolumeForRatiosInput.SetIntLimits(0, 2000000000);

        EnableZeroComparesInput.Name =
            "Enable Zero Bid/Ask Ratio Comparisons";
        EnableZeroComparesInput.SetYesNo(0);

        ZeroCompareActionInput.Name = "Zero Value Compare Action";
        ZeroCompareActionInput.SetCustomInputStrings(
            "Set Denominator 0 to 1;"
            "Set Percentage to 1000 Percent");
        ZeroCompareActionInput.SetCustomInputIndex(0);

        MinimumAdjacentGroupSizeInput.Name =
            "Minimum Adjacent Group Size";
        MinimumAdjacentGroupSizeInput.SetInt(3);
        MinimumAdjacentGroupSizeInput.SetIntLimits(1, 1000);

        MinimumClassifiedGroupVolumeInput.Name =
            "Minimum Classified Group Volume (Ask + Bid)";
        MinimumClassifiedGroupVolumeInput.SetInt(0);
        MinimumClassifiedGroupVolumeInput.SetIntLimits(0, 2000000000);

        MinimumGroupNetDifferenceInput.Name =
            "Minimum Group Net Difference (Contracts)";
        MinimumGroupNetDifferenceInput.SetInt(1);
        MinimumGroupNetDifferenceInput.SetIntLimits(0, 2000000000);

        MinimumGroupDominancePercentInput.Name =
            "Minimum Group Dominance Percentage";
        MinimumGroupDominancePercentInput.SetFloat(100.0f);
        MinimumGroupDominancePercentInput.SetFloatLimits(
            100.0f,
            1000000.0f);

        NearTieActionInput.Name =
            "Group Below Dominance Filters / Exact Tie Action";
        NearTieActionInput.SetCustomInputStrings(
            "Skip Group;Draw Neutral Line");
        NearTieActionInput.SetCustomInputIndex(NEAR_TIE_SKIP);

        ExtensionLineModeInput.Name = "Extension Line Draw Mode";
        ExtensionLineModeInput.SetCustomInputStrings(
            "Dominant-Side Edge;"
            "Lowest Group Price;"
            "Highest Group Price;"
            "Center Group Price;"
            "Volume-Weighted Group Price;"
            "All Group Prices;"
            "Transparent Group Zone");
        ExtensionLineModeInput.SetCustomInputIndex(
            LINE_AT_DOMINANT_SIDE_EDGE);

        ExtendToEndOfChartInput.Name =
            "Extend Lines Until End of Chart";
        ExtendToEndOfChartInput.SetYesNo(0);

        CloseCrossoverForIntersectionInput.Name =
            "Use Close Crossover for Future Intersection";
        CloseCrossoverForIntersectionInput.SetYesNo(0);

        DrawNameLabelInput.Name =
            "Draw Group Side and Volume Name Label";
        DrawNameLabelInput.SetYesNo(1);

        DrawValueLabelInput.Name = "Draw Price Value Label";
        DrawValueLabelInput.SetYesNo(0);

        LabelDetailInput.Name = "Name Label Detail";
        LabelDetailInput.SetCustomInputStrings(
            "Side Only;"
            "Side and Group Size;"
            "Side and Ask/Bid Totals;"
            "Full: Side, Size, Totals, Delta, Ratio");
        LabelDetailInput.SetCustomInputIndex(LABEL_FULL_DETAILS);

        ZoneTransparencyInput.Name =
            "Transparent Group Zone Transparency Percent";
        ZoneTransparencyInput.SetInt(80);
        ZoneTransparencyInput.SetIntLimits(0, 100);

        ClosedBarsOnlyInput.Name = "Closed Bars Only";
        ClosedBarsOnlyInput.SetYesNo(1);

        NumberOfDaysToCalculateInput.Name = "Number of Days to Calculate";
        NumberOfDaysToCalculateInput.SetInt(30);
        NumberOfDaysToCalculateInput.SetIntLimits(1, 10000);

        BuyerAlertSoundInput.Name = "Buyer-Dominant Group Alert Sound";
        BuyerAlertSoundInput.SetAlertSoundNumber(0);

        SellerAlertSoundInput.Name = "Seller-Dominant Group Alert Sound";
        SellerAlertSoundInput.SetAlertSoundNumber(0);

        RestrictToYMSymbolInput.Name =
            "Restrict Study to E-mini Dow YM/YMU Symbols";
        RestrictToYMSymbolInput.SetYesNo(1);

        RequireOnePointTickInput.Name = "Require YM 1-Point Tick Size";
        RequireOnePointTickInput.SetYesNo(1);

        return;
    }

    sc.EarliestUpdateSubgraphDataArrayIndex = sc.UpdateStartIndex;

    if (sc.ArraySize <= 0)
        return;

    int& LastBuyerAlertBar = sc.GetPersistentInt(1);
    int& LastSellerAlertBar = sc.GetPersistentInt(2);

    if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0)
    {
        LastBuyerAlertBar = -1;
        LastSellerAlertBar = -1;
    }

    const bool SymbolIsValid =
        !RestrictToYMSymbolInput.GetYesNo()
        || IsYMEminiDowSymbol(sc.Symbol);

    const bool TickSizeIsValid =
        !RequireOnePointTickInput.GetYesNo()
        || IsYMOnePointTickSize(sc.TickSize);

    if (!SymbolIsValid || !TickSizeIsValid)
    {
        for (int BarIndex = sc.UpdateStartIndex;
             BarIndex < sc.ArraySize;
             ++BarIndex)
        {
            BuyerGroupCount[BarIndex] = 0.0f;
            SellerGroupCount[BarIndex] = 0.0f;
            NeutralGroupCount[BarIndex] = 0.0f;
            StrongestSignedGroupDelta[BarIndex] = 0.0f;
            StrongestGroupDominancePercent[BarIndex] = 0.0f;
            QualifyingGroupLevelCount[BarIndex] = 0.0f;
            ChartCompatibility[BarIndex] = -1.0f;
        }

        return;
    }

    if (sc.VolumeAtPriceForBars == NULL)
    {
        for (int BarIndex = sc.UpdateStartIndex;
             BarIndex < sc.ArraySize;
             ++BarIndex)
        {
            ChartCompatibility[BarIndex] = 0.0f;
        }

        return;
    }

    // Wait until Volume-at-Price data exists for all chart bars.
    if (static_cast<int>(
            sc.VolumeAtPriceForBars->GetNumberOfBars()) < sc.ArraySize)
    {
        for (int BarIndex = sc.UpdateStartIndex;
             BarIndex < sc.ArraySize;
             ++BarIndex)
        {
            ChartCompatibility[BarIndex] = 0.0f;
        }

        return;
    }

    SCDateTimeMS StartDateTimeForCalculations =
        sc.BaseDateTimeIn[sc.ArraySize - 1];
    StartDateTimeForCalculations.SubtractDays(
        NumberOfDaysToCalculateInput.GetInt());

    const bool EnableAlerts =
        sc.IsFullRecalculation == 0
        && !sc.ChartIsDownloadingHistoricalData(sc.ChartNumber);

    for (int BarIndex = sc.UpdateStartIndex;
         BarIndex < sc.ArraySize;
         ++BarIndex)
    {
        BuyerGroupCount[BarIndex] = 0.0f;
        SellerGroupCount[BarIndex] = 0.0f;
        NeutralGroupCount[BarIndex] = 0.0f;
        StrongestSignedGroupDelta[BarIndex] = 0.0f;
        StrongestGroupDominancePercent[BarIndex] = 0.0f;
        QualifyingGroupLevelCount[BarIndex] = 0.0f;
        ChartCompatibility[BarIndex] = 1.0f;

        if (sc.BaseDateTimeIn[BarIndex] < StartDateTimeForCalculations)
            continue;

        if (ClosedBarsOnlyInput.GetYesNo()
            && sc.GetBarHasClosedStatus(BarIndex)
                == BHCS_BAR_HAS_NOT_CLOSED)
        {
            continue;
        }

        const int VAPCount =
            sc.VolumeAtPriceForBars->GetSizeAtBarIndex(BarIndex);

        if (VAPCount <= 0)
            continue;

        // Lines added by the study are automatically removed during a full
        // recalculation. For an ordinary update, remove deterministic line IDs
        // for the changed bar before redrawing, which also supports intrabar
        // mode without leaving stale lines behind.
        if (!sc.IsFullRecalculation)
            DeletePossibleLinesForBar(sc, BarIndex, VAPCount);

        std::vector<LevelData> Levels;
        Levels.reserve(static_cast<size_t>(VAPCount));
        uint64_t BarTotalVolume = 0;
        uint64_t BarClassifiedVolume = 0;

        for (int VAPIndex = 0; VAPIndex < VAPCount; ++VAPIndex)
        {
            const s_VolumeAtPriceV2* Element = NULL;
            if (!sc.VolumeAtPriceForBars->GetVAPElementAtIndex(
                    BarIndex,
                    VAPIndex,
                    &Element))
            {
                continue;
            }

            LevelData Level;
            Level.PriceInTicks = Element->PriceInTicks;
            Level.BidVolume = Element->BidVolume;
            Level.AskVolume = Element->AskVolume;
            Level.TotalVolume = Element->Volume;
            Level.NumberOfTrades = Element->NumberOfTrades;
            BarTotalVolume += Level.TotalVolume;
            BarClassifiedVolume += Level.BidVolume + Level.AskVolume;
            Levels.push_back(Level);
        }

        if (Levels.empty())
            continue;

        // The buyer/seller classification requires historical Bid Volume and
        // Ask Volume. A bar with traded volume but no classified Bid/Ask
        // volume is marked incompatible rather than producing neutral groups.
        if (BarTotalVolume > 0 && BarClassifiedVolume == 0)
        {
            ChartCompatibility[BarIndex] = 0.0f;
            continue;
        }

        std::sort(
            Levels.begin(),
            Levels.end(),
            [](const LevelData& Left, const LevelData& Right)
            {
                return Left.PriceInTicks < Right.PriceInTicks;
            });

        MarkQualifyingLevels(
            Levels,
            ComparisonMethodInput.GetIndex(),
            VolumeOrDifferenceThresholdInput.GetInt(),
            PercentageThresholdInput.GetInt(),
            AdditionalAskVolumeThresholdInput.GetInt(),
            MinimumTotalVolumeForRatiosInput.GetInt(),
            EnableZeroComparesInput.GetYesNo() != 0,
            ZeroCompareActionInput.GetIndex());

        const int MinimumGroupSize =
            std::max(1, MinimumAdjacentGroupSizeInput.GetInt());
        const uint64_t MinimumClassifiedGroupVolume =
            static_cast<uint64_t>(std::max(
                0,
                MinimumClassifiedGroupVolumeInput.GetInt()));
        const uint64_t MinimumGroupNetDifference =
            static_cast<uint64_t>(std::max(
                0,
                MinimumGroupNetDifferenceInput.GetInt()));
        const double MinimumGroupDominancePercent =
            std::max(
                100.0,
                static_cast<double>(
                    MinimumGroupDominancePercentInput.GetFloat()));

        bool BuyerGroupFound = false;
        bool SellerGroupFound = false;
        int64_t StrongestDelta = 0;
        double StrongestRatio = 0.0;

        const int LevelCount = static_cast<int>(Levels.size());
        int Index = 0;

        while (Index < LevelCount)
        {
            if (!Levels[Index].Qualifies)
            {
                ++Index;
                continue;
            }

            const int GroupStart = Index;
            int GroupEnd = Index;

            while (GroupEnd + 1 < LevelCount
                && Levels[GroupEnd + 1].Qualifies
                && Levels[GroupEnd + 1].PriceInTicks
                    == Levels[GroupEnd].PriceInTicks + 1)
            {
                ++GroupEnd;
            }

            Index = GroupEnd + 1;

            const int GroupSize = GroupEnd - GroupStart + 1;
            if (GroupSize < MinimumGroupSize)
                continue;

            uint64_t GroupAskVolume = 0;
            uint64_t GroupBidVolume = 0;

            for (int GroupIndex = GroupStart;
                 GroupIndex <= GroupEnd;
                 ++GroupIndex)
            {
                GroupAskVolume += Levels[GroupIndex].AskVolume;
                GroupBidVolume += Levels[GroupIndex].BidVolume;
            }

            const uint64_t ClassifiedGroupVolume =
                GroupAskVolume + GroupBidVolume;

            if (ClassifiedGroupVolume < MinimumClassifiedGroupVolume)
                continue;

            const int64_t GroupDelta =
                static_cast<int64_t>(GroupAskVolume)
                - static_cast<int64_t>(GroupBidVolume);

            double DominancePercent = 100.0;
            const int Side = DetermineGroupSide(
                GroupAskVolume,
                GroupBidVolume,
                MinimumGroupNetDifference,
                MinimumGroupDominancePercent,
                DominancePercent);

            QualifyingGroupLevelCount[BarIndex] +=
                static_cast<float>(GroupSize);

            if (Side == GROUP_BUYERS)
            {
                BuyerGroupCount[BarIndex] += 1.0f;
                BuyerGroupFound = true;
            }
            else if (Side == GROUP_SELLERS)
            {
                SellerGroupCount[BarIndex] += 1.0f;
                SellerGroupFound = true;
            }
            else
            {
                NeutralGroupCount[BarIndex] += 1.0f;

                if (NearTieActionInput.GetIndex() == NEAR_TIE_SKIP)
                    continue;
            }

            const int64_t AbsoluteGroupDelta =
                GroupDelta >= 0 ? GroupDelta : -GroupDelta;
            const int64_t AbsoluteStrongestDelta =
                StrongestDelta >= 0 ? StrongestDelta : -StrongestDelta;

            if (AbsoluteGroupDelta > AbsoluteStrongestDelta)
            {
                StrongestDelta = GroupDelta;
                StrongestRatio = DominancePercent;
            }

            SCSubgraphRef LineProperties =
                Side == GROUP_BUYERS
                ? BuyerLineProperties
                : (Side == GROUP_SELLERS
                    ? SellerLineProperties
                    : NeutralLineProperties);

            const SCString GroupLabel = BuildGroupLabel(
                Side,
                GroupSize,
                GroupAskVolume,
                GroupBidVolume,
                GroupDelta,
                DominancePercent,
                LabelDetailInput.GetIndex());

            const int LineMode = ExtensionLineModeInput.GetIndex();
            const int LabelAnchorIndex = GetAnchorIndex(
                Levels,
                GroupStart,
                GroupEnd,
                Side,
                LINE_AT_DOMINANT_SIDE_EDGE);

            if (LineMode == LINE_AS_TRANSPARENT_GROUP_ZONE)
            {
                const float UpperValue =
                    Levels[GroupEnd].PriceInTicks * sc.TickSize
                    + sc.TickSize * 0.5f;
                const float LowerValue =
                    Levels[GroupStart].PriceInTicks * sc.TickSize
                    - sc.TickSize * 0.5f;

                AddExtensionLine(
                    sc,
                    BarIndex,
                    GROUP_LINE_ID_BASE + GroupStart,
                    UpperValue,
                    true,
                    LowerValue,
                    LineProperties.PrimaryColor,
                    LineProperties.LineWidth,
                    LineProperties.LineStyle,
                    DrawValueLabelInput.GetYesNo() != 0,
                    DrawNameLabelInput.GetYesNo() != 0,
                    GroupLabel,
                    ExtendToEndOfChartInput.GetYesNo() != 0,
                    CloseCrossoverForIntersectionInput.GetYesNo() != 0,
                    ZoneTransparencyInput.GetInt());
            }
            else if (LineMode == LINE_AT_ALL_GROUP_PRICES)
            {
                for (int GroupIndex = GroupStart;
                     GroupIndex <= GroupEnd;
                     ++GroupIndex)
                {
                    const float Price =
                        Levels[GroupIndex].PriceInTicks * sc.TickSize;
                    const bool DrawThisNameLabel =
                        DrawNameLabelInput.GetYesNo() != 0
                        && GroupIndex == LabelAnchorIndex;

                    AddExtensionLine(
                        sc,
                        BarIndex,
                        PRICE_LINE_ID_BASE + GroupIndex,
                        Price,
                        false,
                        0.0f,
                        LineProperties.PrimaryColor,
                        LineProperties.LineWidth,
                        LineProperties.LineStyle,
                        DrawValueLabelInput.GetYesNo() != 0,
                        DrawThisNameLabel,
                        GroupLabel,
                        ExtendToEndOfChartInput.GetYesNo() != 0,
                        CloseCrossoverForIntersectionInput.GetYesNo() != 0,
                        0);
                }
            }
            else
            {
                const int AnchorIndex = GetAnchorIndex(
                    Levels,
                    GroupStart,
                    GroupEnd,
                    Side,
                    LineMode);
                const float Price =
                    Levels[AnchorIndex].PriceInTicks * sc.TickSize;

                AddExtensionLine(
                    sc,
                    BarIndex,
                    GROUP_LINE_ID_BASE + GroupStart,
                    Price,
                    false,
                    0.0f,
                    LineProperties.PrimaryColor,
                    LineProperties.LineWidth,
                    LineProperties.LineStyle,
                    DrawValueLabelInput.GetYesNo() != 0,
                    DrawNameLabelInput.GetYesNo() != 0,
                    GroupLabel,
                    ExtendToEndOfChartInput.GetYesNo() != 0,
                    CloseCrossoverForIntersectionInput.GetYesNo() != 0,
                    0);
            }
        }

        StrongestSignedGroupDelta[BarIndex] =
            static_cast<float>(StrongestDelta);
        StrongestGroupDominancePercent[BarIndex] =
            static_cast<float>(StrongestRatio);

        // A newly completed bar can be ArraySize - 2 after Sierra Chart has
        // already created the next bar. This check supports both completed-bar
        // and intrabar operation while avoiding alerts on older recalculated
        // history.
        const bool RecentBar = BarIndex >= sc.ArraySize - 2;

        if (EnableAlerts && RecentBar)
        {
            if (BuyerGroupFound
                && BuyerAlertSoundInput.GetAlertSoundNumber() > 0
                && LastBuyerAlertBar != BarIndex)
            {
                SCString Message;
                Message.Format(
                    "YMU/YM VAP group total: buyers exceed sellers on bar %d",
                    BarIndex);
                sc.SetAlert(
                    BuyerAlertSoundInput.GetAlertSoundNumber() - 1,
                    Message.GetChars());
                LastBuyerAlertBar = BarIndex;
            }

            if (SellerGroupFound
                && SellerAlertSoundInput.GetAlertSoundNumber() > 0
                && LastSellerAlertBar != BarIndex)
            {
                SCString Message;
                Message.Format(
                    "YMU/YM VAP group total: sellers exceed buyers on bar %d",
                    BarIndex);
                sc.SetAlert(
                    SellerAlertSoundInput.GetAlertSoundNumber() - 1,
                    Message.GetChars());
                LastSellerAlertBar = BarIndex;
            }
        }
    }
}
