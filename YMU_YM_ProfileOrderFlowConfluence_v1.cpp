#include "sierrachart.h"

// Some Sierra Chart headers define max/min as macros. Remove them before the
// standard library headers are included. This source deliberately avoids
// std::max and std::min as an additional Remote Build safeguard.
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

SCDLLName("YMU YM Profile Volume Order Flow Confluence Signals")

// -----------------------------------------------------------------------------
// YMU / YM Volume Profile + Volume + Order-Flow Confluence Signals
//
// This study is a decision-support indicator, not a guaranteed predictor.
// Reference levels provide context only. A BUY or SELL signal cannot be created
// by a level touch alone. Every signal must also pass:
//   1. Candle-behavior gate
//   2. Volume gate (same-session relative volume and/or a concentrated
//      high-volume price aligned with the active reference)
//   3. Current-bar order-flow gate
//   4. Configurable confluence-score threshold
//
// Internally calculated reference data:
//   - Current trading-day high and low (RTH or full trading day)
//   - Previous trading-day high and low (RTH or full trading day)
//   - Current trading day's overnight VAH / VPOC / VAL
//   - Previous completed RTH VAH / VPOC / VAL
//   - Developing current-day primary HVN (VPOC) and two secondary HVNs
//
// Signal types:
//   +1 = Bullish rejection / responsive BUY
//   +2 = Bullish breakout / acceptance BUY
//   -1 = Bearish rejection / responsive SELL
//   -2 = Bearish breakdown / acceptance SELL
//
// The study uses closed bars only. It does not wait for future bars. The signal
// is placed on the candle that just closed on the first update of the next bar.
// All session times are interpreted in the chart time zone.
// -----------------------------------------------------------------------------

namespace
{
    enum SessionKind
    {
        SESSION_OUTSIDE = 0,
        SESSION_OVERNIGHT = 1,
        SESSION_RTH = 2
    };

    enum SignalSessionFilter
    {
        SIGNAL_RTH_ONLY = 0,
        SIGNAL_ALL_INCLUDED_SESSIONS = 1,
        SIGNAL_OVERNIGHT_ONLY = 2
    };

    enum DayHighLowBasis
    {
        DAY_HIGH_LOW_RTH = 0,
        DAY_HIGH_LOW_FULL_TRADING_DAY = 1
    };

    enum CurrentHVNBasis
    {
        HVN_RTH = 0,
        HVN_FULL_TRADING_DAY = 1
    };

    enum LevelRole
    {
        LEVEL_SUPPORT = 0,
        LEVEL_RESISTANCE = 1,
        LEVEL_TWO_SIDED = 2
    };

    enum ContextCategory
    {
        CATEGORY_CURRENT_DAY_EXTREME = 0,
        CATEGORY_PREVIOUS_DAY_EXTREME = 1,
        CATEGORY_OVERNIGHT_PROFILE = 2,
        CATEGORY_PREVIOUS_RTH_PROFILE = 3,
        CATEGORY_CURRENT_DAY_HVN = 4
    };

    struct PriceVolumeData
    {
        uint64_t TotalVolume;
        uint64_t BidVolume;
        uint64_t AskVolume;

        PriceVolumeData()
            : TotalVolume(0)
            , BidVolume(0)
            , AskVolume(0)
        {
        }
    };

    typedef std::map<int, PriceVolumeData> ProfileMap;

    struct ProfileLevels
    {
        bool Valid;
        int PointOfControlTicks;
        int ValueAreaHighTicks;
        int ValueAreaLowTicks;
        int SecondaryHVN1Ticks;
        int SecondaryHVN2Ticks;
        bool SecondaryHVN1Valid;
        bool SecondaryHVN2Valid;
        uint64_t TotalVolume;
        uint64_t PointOfControlVolume;

        ProfileLevels()
            : Valid(false)
            , PointOfControlTicks(0)
            , ValueAreaHighTicks(0)
            , ValueAreaLowTicks(0)
            , SecondaryHVN1Ticks(0)
            , SecondaryHVN2Ticks(0)
            , SecondaryHVN1Valid(false)
            , SecondaryHVN2Valid(false)
            , TotalVolume(0)
            , PointOfControlVolume(0)
        {
        }
    };

    struct BarLevel
    {
        int PriceInTicks;
        uint32_t TotalVolume;
        uint32_t BidVolume;
        uint32_t AskVolume;
    };

    struct BarOrderFlowStats
    {
        bool Valid;
        uint64_t TotalVolume;
        uint64_t ClassifiedVolume;
        uint64_t BidVolume;
        uint64_t AskVolume;
        int64_t Delta;
        double DeltaPercent;
        double ClassifiedPercent;
        int LevelCount;
        int BarPOCTicks;
        uint64_t BarPOCVolume;
        double BarPOCMultiple;
        int BullishStackedImbalance;
        int BearishStackedImbalance;
        bool SellAbsorptionRaw;
        bool BuyAbsorptionRaw;
        double LowBidEdgeMultiple;
        double HighAskEdgeMultiple;

        BarOrderFlowStats()
            : Valid(false)
            , TotalVolume(0)
            , ClassifiedVolume(0)
            , BidVolume(0)
            , AskVolume(0)
            , Delta(0)
            , DeltaPercent(0.0)
            , ClassifiedPercent(0.0)
            , LevelCount(0)
            , BarPOCTicks(0)
            , BarPOCVolume(0)
            , BarPOCMultiple(0.0)
            , BullishStackedImbalance(0)
            , BearishStackedImbalance(0)
            , SellAbsorptionRaw(false)
            , BuyAbsorptionRaw(false)
            , LowBidEdgeMultiple(0.0)
            , HighAskEdgeMultiple(0.0)
        {
        }
    };

    struct ReferenceLevel
    {
        double Price;
        LevelRole Role;
        int Category;

        ReferenceLevel(const double InPrice, const LevelRole InRole, const int InCategory)
            : Price(InPrice)
            , Role(InRole)
            , Category(InCategory)
        {
        }
    };

    struct ContextScores
    {
        unsigned int LongRejectionMask;
        unsigned int ShortRejectionMask;
        unsigned int LongBreakoutMask;
        unsigned int ShortBreakdownMask;

        // A separate mask records whether the candle's high-volume price
        // (bar VPOC) is concentrated close to the specific reference level
        // responsible for the setup. This prevents a high-volume print at the
        // opposite end of a wide candle from receiving the profile-volume
        // confluence point.
        unsigned int LongRejectionHighVolumeMask;
        unsigned int ShortRejectionHighVolumeMask;
        unsigned int LongBreakoutHighVolumeMask;
        unsigned int ShortBreakdownHighVolumeMask;

        ContextScores()
            : LongRejectionMask(0)
            , ShortRejectionMask(0)
            , LongBreakoutMask(0)
            , ShortBreakdownMask(0)
            , LongRejectionHighVolumeMask(0)
            , ShortRejectionHighVolumeMask(0)
            , LongBreakoutHighVolumeMask(0)
            , ShortBreakdownHighVolumeMask(0)
        {
        }
    };

    struct EngineState
    {
        int LastProcessedIndex;
        int CurrentTradingDate;
        bool HasTradingDay;

        float CurrentFullHigh;
        float CurrentFullLow;
        bool CurrentFullValid;
        float PreviousFullHigh;
        float PreviousFullLow;
        bool PreviousFullValid;

        float CurrentRTHHigh;
        float CurrentRTHLow;
        bool CurrentRTHValid;
        float PreviousRTHHigh;
        float PreviousRTHLow;
        bool PreviousRTHValid;

        ProfileMap CurrentFullProfile;
        ProfileMap CurrentRTHProfile;
        ProfileMap CurrentOvernightProfile;

        ProfileLevels CurrentFullLevels;
        ProfileLevels CurrentRTHLevels;
        ProfileLevels OvernightLevels;
        ProfileLevels PreviousRTHLevels;

        int CurrentFullProfileBars;
        int CurrentRTHProfileBars;
        int CurrentOvernightProfileBars;

        // Keep RTH and overnight baselines separate. Mixing overnight bars
        // into the RTH average makes the cash-session open look artificially
        // high-volume and can distort rolling delta confirmation.
        std::deque<double> RTHVolumeHistory;
        double RTHVolumeHistorySum;
        std::deque<double> OvernightVolumeHistory;
        double OvernightVolumeHistorySum;
        std::deque<double> RTHDeltaHistory;
        std::deque<double> OvernightDeltaHistory;

        int LastSignalIndex;
        int LastAlertIndex;

        EngineState()
            : LastProcessedIndex(-1)
            , CurrentTradingDate(0)
            , HasTradingDay(false)
            , CurrentFullHigh(0.0f)
            , CurrentFullLow(0.0f)
            , CurrentFullValid(false)
            , PreviousFullHigh(0.0f)
            , PreviousFullLow(0.0f)
            , PreviousFullValid(false)
            , CurrentRTHHigh(0.0f)
            , CurrentRTHLow(0.0f)
            , CurrentRTHValid(false)
            , PreviousRTHHigh(0.0f)
            , PreviousRTHLow(0.0f)
            , PreviousRTHValid(false)
            , CurrentFullProfileBars(0)
            , CurrentRTHProfileBars(0)
            , CurrentOvernightProfileBars(0)
            , RTHVolumeHistorySum(0.0)
            , OvernightVolumeHistorySum(0.0)
            , LastSignalIndex(-1000000000)
            , LastAlertIndex(-1)
        {
        }
    };

    int MaximumInt(const int A, const int B)
    {
        return A > B ? A : B;
    }

    int MinimumInt(const int A, const int B)
    {
        return A < B ? A : B;
    }

    double MaximumDouble(const double A, const double B)
    {
        return A > B ? A : B;
    }

    double MinimumDouble(const double A, const double B)
    {
        return A < B ? A : B;
    }

    double ClampDouble(const double Value, const double Minimum, const double Maximum)
    {
        if (Value < Minimum)
            return Minimum;
        if (Value > Maximum)
            return Maximum;
        return Value;
    }

    int CountSetBits(unsigned int Value)
    {
        int Count = 0;
        while (Value != 0)
        {
            Count += static_cast<int>(Value & 1U);
            Value >>= 1U;
        }
        return Count;
    }

    bool IsYMEminiDowSymbol(const SCString& Symbol)
    {
        const char* Text = Symbol.GetChars();
        if (Text == NULL || Text[0] == '\0')
            return false;

        const size_t Length = std::strlen(Text);

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

            // Reject the YM substring inside MYM and other alphanumeric names.
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

    bool IsTimeWithinRange(
        const int TimeValue,
        const int StartTime,
        const int EndTime)
    {
        if (StartTime <= EndTime)
            return TimeValue >= StartTime && TimeValue <= EndTime;

        return TimeValue >= StartTime || TimeValue <= EndTime;
    }

    SessionKind GetSessionKind(
        const int TimeValue,
        const int RTHStart,
        const int RTHEnd,
        const int OvernightStart)
    {
        if (IsTimeWithinRange(TimeValue, RTHStart, RTHEnd))
            return SESSION_RTH;

        if (OvernightStart > RTHStart)
        {
            if (TimeValue >= OvernightStart || TimeValue < RTHStart)
                return SESSION_OVERNIGHT;
        }
        else if (IsTimeWithinRange(TimeValue, OvernightStart, RTHStart - 1))
        {
            return SESSION_OVERNIGHT;
        }

        return SESSION_OUTSIDE;
    }

    int GetTradingDate(
        const SCDateTime& DateTime,
        const int OvernightStart)
    {
        const int CalendarDate = DateTime.GetDate();
        const int TimeValue = DateTime.GetTime();

        if (TimeValue >= OvernightStart)
            return CalendarDate + 1;

        return CalendarDate;
    }

    double TicksToPrice(const int PriceInTicks, const float TickSize)
    {
        return static_cast<double>(PriceInTicks)
            * static_cast<double>(TickSize);
    }

    bool RatioAtLeast(
        const uint64_t Numerator,
        const uint64_t Denominator,
        const double RequiredRatio,
        const uint64_t MinimumNumerator)
    {
        if (Numerator < MinimumNumerator)
            return false;

        if (Denominator == 0)
            return Numerator > 0;

        return static_cast<double>(Numerator)
            / static_cast<double>(Denominator)
            >= RequiredRatio;
    }

    bool LoadBarLevels(
        SCStudyInterfaceRef sc,
        const int BarIndex,
        std::vector<BarLevel>& Levels)
    {
        Levels.clear();

        if (sc.VolumeAtPriceForBars == NULL)
            return false;

        const int Count =
            sc.VolumeAtPriceForBars->GetSizeAtBarIndex(BarIndex);

        if (Count <= 0)
            return false;

        Levels.reserve(static_cast<size_t>(Count));

        for (int VAPIndex = 0; VAPIndex < Count; ++VAPIndex)
        {
            const s_VolumeAtPriceV2* Element = NULL;
            if (!sc.VolumeAtPriceForBars->GetVAPElementAtIndex(
                    BarIndex,
                    VAPIndex,
                    &Element)
                || Element == NULL)
            {
                return false;
            }

            BarLevel Level;
            Level.PriceInTicks = Element->PriceInTicks;
            Level.TotalVolume = Element->Volume;
            Level.BidVolume = Element->BidVolume;
            Level.AskVolume = Element->AskVolume;
            Levels.push_back(Level);
        }

        return !Levels.empty();
    }

    void AddLevelsToProfile(
        const std::vector<BarLevel>& Levels,
        ProfileMap& Profile)
    {
        for (size_t Index = 0; Index < Levels.size(); ++Index)
        {
            const BarLevel& Level = Levels[Index];
            PriceVolumeData& Destination = Profile[Level.PriceInTicks];
            Destination.TotalVolume +=
                static_cast<uint64_t>(Level.TotalVolume);
            Destination.BidVolume +=
                static_cast<uint64_t>(Level.BidVolume);
            Destination.AskVolume +=
                static_cast<uint64_t>(Level.AskVolume);
        }
    }

    struct HVNCandidate
    {
        int PriceInTicks;
        uint64_t Volume;
    };

    bool HVNCandidateVolumeSort(
        const HVNCandidate& A,
        const HVNCandidate& B)
    {
        if (A.Volume != B.Volume)
            return A.Volume > B.Volume;
        return A.PriceInTicks < B.PriceInTicks;
    }

    ProfileLevels CalculateProfileLevels(
        const ProfileMap& Profile,
        const double ValueAreaPercent,
        const double MinimumHVNPercentOfPOC,
        const int MinimumHVNSeparationTicks)
    {
        ProfileLevels Result;

        if (Profile.empty())
            return Result;

        const int LowTicks = Profile.begin()->first;
        const int HighTicks = Profile.rbegin()->first;
        const int DenseSize = HighTicks - LowTicks + 1;

        // Protect against an invalid symbol/tick size or corrupt price range.
        if (DenseSize <= 0 || DenseSize > 200000)
            return Result;

        std::vector<uint64_t> Volumes(
            static_cast<size_t>(DenseSize),
            static_cast<uint64_t>(0));

        uint64_t TotalVolume = 0;
        for (ProfileMap::const_iterator Iterator = Profile.begin();
             Iterator != Profile.end();
             ++Iterator)
        {
            const int DenseIndex = Iterator->first - LowTicks;
            if (DenseIndex < 0 || DenseIndex >= DenseSize)
                continue;

            Volumes[static_cast<size_t>(DenseIndex)] =
                Iterator->second.TotalVolume;
            TotalVolume += Iterator->second.TotalVolume;
        }

        if (TotalVolume == 0)
            return Result;

        const double ProfileMidpoint =
            (static_cast<double>(LowTicks)
                + static_cast<double>(HighTicks))
            * 0.5;

        int POCIndex = 0;
        uint64_t POCVolume = Volumes[0];

        for (int Index = 1; Index < DenseSize; ++Index)
        {
            const uint64_t CandidateVolume =
                Volumes[static_cast<size_t>(Index)];

            if (CandidateVolume > POCVolume)
            {
                POCVolume = CandidateVolume;
                POCIndex = Index;
                continue;
            }

            if (CandidateVolume == POCVolume)
            {
                const double CurrentDistance = std::fabs(
                    static_cast<double>(LowTicks + POCIndex)
                    - ProfileMidpoint);
                const double CandidateDistance = std::fabs(
                    static_cast<double>(LowTicks + Index)
                    - ProfileMidpoint);

                if (CandidateDistance < CurrentDistance
                    || (CandidateDistance == CurrentDistance
                        && Index < POCIndex))
                {
                    POCIndex = Index;
                }
            }
        }

        const double ClampedValueArea =
            ClampDouble(ValueAreaPercent, 1.0, 100.0);
        const uint64_t TargetVolume = static_cast<uint64_t>(
            std::ceil(
                static_cast<double>(TotalVolume)
                * ClampedValueArea
                / 100.0));

        int ValueAreaLowIndex = POCIndex;
        int ValueAreaHighIndex = POCIndex;
        int NextLowIndex = POCIndex - 1;
        int NextHighIndex = POCIndex + 1;
        uint64_t IncludedVolume = POCVolume;

        while (IncludedVolume < TargetVolume
            && (NextLowIndex >= 0 || NextHighIndex < DenseSize))
        {
            if (NextLowIndex < 0)
            {
                IncludedVolume +=
                    Volumes[static_cast<size_t>(NextHighIndex)];
                ValueAreaHighIndex = NextHighIndex;
                ++NextHighIndex;
                continue;
            }

            if (NextHighIndex >= DenseSize)
            {
                IncludedVolume +=
                    Volumes[static_cast<size_t>(NextLowIndex)];
                ValueAreaLowIndex = NextLowIndex;
                --NextLowIndex;
                continue;
            }

            const uint64_t LowerVolume =
                Volumes[static_cast<size_t>(NextLowIndex)];
            const uint64_t HigherVolume =
                Volumes[static_cast<size_t>(NextHighIndex)];

            if (HigherVolume > LowerVolume)
            {
                IncludedVolume += HigherVolume;
                ValueAreaHighIndex = NextHighIndex;
                ++NextHighIndex;
            }
            else if (LowerVolume > HigherVolume)
            {
                IncludedVolume += LowerVolume;
                ValueAreaLowIndex = NextLowIndex;
                --NextLowIndex;
            }
            else
            {
                IncludedVolume += LowerVolume;
                IncludedVolume += HigherVolume;
                ValueAreaLowIndex = NextLowIndex;
                ValueAreaHighIndex = NextHighIndex;
                --NextLowIndex;
                ++NextHighIndex;
            }
        }

        Result.Valid = true;
        Result.PointOfControlTicks = LowTicks + POCIndex;
        Result.ValueAreaHighTicks = LowTicks + ValueAreaHighIndex;
        Result.ValueAreaLowTicks = LowTicks + ValueAreaLowIndex;
        Result.TotalVolume = TotalVolume;
        Result.PointOfControlVolume = POCVolume;

        // Primary HVN is the VPOC. Find two additional local high-volume nodes.
        std::vector<HVNCandidate> Candidates;
        const double MinimumCandidateVolume =
            static_cast<double>(POCVolume)
            * ClampDouble(MinimumHVNPercentOfPOC, 0.0, 100.0)
            / 100.0;

        for (int Index = 0; Index < DenseSize; ++Index)
        {
            if (Index == POCIndex)
                continue;

            const uint64_t CurrentVolume =
                Volumes[static_cast<size_t>(Index)];

            if (static_cast<double>(CurrentVolume)
                < MinimumCandidateVolume)
            {
                continue;
            }

            const uint64_t LeftVolume = Index > 0
                ? Volumes[static_cast<size_t>(Index - 1)]
                : static_cast<uint64_t>(0);
            const uint64_t RightVolume = Index + 1 < DenseSize
                ? Volumes[static_cast<size_t>(Index + 1)]
                : static_cast<uint64_t>(0);

            const bool IsLocalMaximum =
                CurrentVolume >= LeftVolume
                && CurrentVolume >= RightVolume
                && (CurrentVolume > LeftVolume
                    || CurrentVolume > RightVolume);

            if (!IsLocalMaximum)
                continue;

            HVNCandidate Candidate;
            Candidate.PriceInTicks = LowTicks + Index;
            Candidate.Volume = CurrentVolume;
            Candidates.push_back(Candidate);
        }

        std::sort(
            Candidates.begin(),
            Candidates.end(),
            HVNCandidateVolumeSort);

        const int SeparationTicks =
            MaximumInt(1, MinimumHVNSeparationTicks);

        std::vector<int> SelectedPrices;
        SelectedPrices.push_back(Result.PointOfControlTicks);

        for (size_t Index = 0;
             Index < Candidates.size() && SelectedPrices.size() < 3;
             ++Index)
        {
            bool TooClose = false;
            for (size_t SelectedIndex = 0;
                 SelectedIndex < SelectedPrices.size();
                 ++SelectedIndex)
            {
                if (std::abs(
                        Candidates[Index].PriceInTicks
                        - SelectedPrices[SelectedIndex])
                    < SeparationTicks)
                {
                    TooClose = true;
                    break;
                }
            }

            if (!TooClose)
                SelectedPrices.push_back(Candidates[Index].PriceInTicks);
        }

        if (SelectedPrices.size() >= 2)
        {
            Result.SecondaryHVN1Ticks = SelectedPrices[1];
            Result.SecondaryHVN1Valid = true;
        }

        if (SelectedPrices.size() >= 3)
        {
            Result.SecondaryHVN2Ticks = SelectedPrices[2];
            Result.SecondaryHVN2Valid = true;
        }

        return Result;
    }

    BarOrderFlowStats CalculateBarOrderFlowStats(
        const std::vector<BarLevel>& Levels,
        const double DiagonalImbalanceRatio,
        const uint64_t MinimumImbalanceVolume,
        const int AbsorptionEdgeLevels,
        const int AbsorptionMinimumDominantLevels,
        const double AbsorptionRatio,
        const double AbsorptionEdgeVolumeMultiple)
    {
        BarOrderFlowStats Result;

        if (Levels.empty())
            return Result;

        Result.LevelCount = static_cast<int>(Levels.size());

        uint64_t HighestLevelVolume = 0;
        int HighestLevelTicks = Levels[0].PriceInTicks;

        for (size_t Index = 0; Index < Levels.size(); ++Index)
        {
            Result.TotalVolume +=
                static_cast<uint64_t>(Levels[Index].TotalVolume);
            Result.BidVolume +=
                static_cast<uint64_t>(Levels[Index].BidVolume);
            Result.AskVolume +=
                static_cast<uint64_t>(Levels[Index].AskVolume);

            if (Levels[Index].TotalVolume > HighestLevelVolume)
            {
                HighestLevelVolume = Levels[Index].TotalVolume;
                HighestLevelTicks = Levels[Index].PriceInTicks;
            }
        }

        Result.ClassifiedVolume =
            Result.BidVolume + Result.AskVolume;
        Result.Delta = static_cast<int64_t>(Result.AskVolume)
            - static_cast<int64_t>(Result.BidVolume);

        if (Result.ClassifiedVolume > 0)
        {
            Result.DeltaPercent =
                static_cast<double>(Result.Delta)
                / static_cast<double>(Result.ClassifiedVolume)
                * 100.0;
        }

        if (Result.TotalVolume > 0)
        {
            Result.ClassifiedPercent =
                static_cast<double>(Result.ClassifiedVolume)
                / static_cast<double>(Result.TotalVolume)
                * 100.0;
        }

        Result.BarPOCTicks = HighestLevelTicks;
        Result.BarPOCVolume = HighestLevelVolume;

        if (Result.TotalVolume > 0 && Result.LevelCount > 0)
        {
            const double AverageVolumePerLevel =
                static_cast<double>(Result.TotalVolume)
                / static_cast<double>(Result.LevelCount);

            if (AverageVolumePerLevel > 0.0)
            {
                Result.BarPOCMultiple =
                    static_cast<double>(HighestLevelVolume)
                    / AverageVolumePerLevel;
            }
        }

        int CurrentBullishStack = 0;
        int CurrentBearishStack = 0;

        for (size_t Index = 1; Index < Levels.size(); ++Index)
        {
            const bool Adjacent =
                Levels[Index].PriceInTicks
                == Levels[Index - 1].PriceInTicks + 1;

            if (Adjacent
                && RatioAtLeast(
                    static_cast<uint64_t>(Levels[Index].AskVolume),
                    static_cast<uint64_t>(Levels[Index - 1].BidVolume),
                    DiagonalImbalanceRatio,
                    MinimumImbalanceVolume))
            {
                ++CurrentBullishStack;
                if (CurrentBullishStack > Result.BullishStackedImbalance)
                    Result.BullishStackedImbalance = CurrentBullishStack;
            }
            else
            {
                CurrentBullishStack = 0;
            }

            if (Adjacent
                && RatioAtLeast(
                    static_cast<uint64_t>(Levels[Index - 1].BidVolume),
                    static_cast<uint64_t>(Levels[Index].AskVolume),
                    DiagonalImbalanceRatio,
                    MinimumImbalanceVolume))
            {
                ++CurrentBearishStack;
                if (CurrentBearishStack > Result.BearishStackedImbalance)
                    Result.BearishStackedImbalance = CurrentBearishStack;
            }
            else
            {
                CurrentBearishStack = 0;
            }
        }

        const int EdgeCount = MinimumInt(
            MaximumInt(1, AbsorptionEdgeLevels),
            Result.LevelCount);

        int SellDominantCount = 0;
        uint64_t SellEdgeBid = 0;
        uint64_t SellEdgeOpposingAsk = 0;

        for (int EdgeIndex = 0; EdgeIndex < EdgeCount; ++EdgeIndex)
        {
            SellEdgeBid +=
                static_cast<uint64_t>(Levels[static_cast<size_t>(EdgeIndex)].BidVolume);

            if (EdgeIndex + 1 >= Result.LevelCount)
                continue;

            const BarLevel& Current =
                Levels[static_cast<size_t>(EdgeIndex)];
            const BarLevel& Above =
                Levels[static_cast<size_t>(EdgeIndex + 1)];

            if (Above.PriceInTicks != Current.PriceInTicks + 1)
                continue;

            SellEdgeOpposingAsk +=
                static_cast<uint64_t>(Above.AskVolume);

            if (RatioAtLeast(
                    static_cast<uint64_t>(Current.BidVolume),
                    static_cast<uint64_t>(Above.AskVolume),
                    AbsorptionRatio,
                    MinimumImbalanceVolume))
            {
                ++SellDominantCount;
            }
        }

        int BuyDominantCount = 0;
        uint64_t BuyEdgeAsk = 0;
        uint64_t BuyEdgeOpposingBid = 0;

        for (int Offset = 0; Offset < EdgeCount; ++Offset)
        {
            const int EdgeIndex = Result.LevelCount - 1 - Offset;
            BuyEdgeAsk +=
                static_cast<uint64_t>(Levels[static_cast<size_t>(EdgeIndex)].AskVolume);

            if (EdgeIndex - 1 < 0)
                continue;

            const BarLevel& Current =
                Levels[static_cast<size_t>(EdgeIndex)];
            const BarLevel& Below =
                Levels[static_cast<size_t>(EdgeIndex - 1)];

            if (Current.PriceInTicks != Below.PriceInTicks + 1)
                continue;

            BuyEdgeOpposingBid +=
                static_cast<uint64_t>(Below.BidVolume);

            if (RatioAtLeast(
                    static_cast<uint64_t>(Current.AskVolume),
                    static_cast<uint64_t>(Below.BidVolume),
                    AbsorptionRatio,
                    MinimumImbalanceVolume))
            {
                ++BuyDominantCount;
            }
        }

        const double AverageBidPerLevel = Result.LevelCount > 0
            ? static_cast<double>(Result.BidVolume)
                / static_cast<double>(Result.LevelCount)
            : 0.0;
        const double AverageAskPerLevel = Result.LevelCount > 0
            ? static_cast<double>(Result.AskVolume)
                / static_cast<double>(Result.LevelCount)
            : 0.0;

        const double AverageLowEdgeBid = EdgeCount > 0
            ? static_cast<double>(SellEdgeBid)
                / static_cast<double>(EdgeCount)
            : 0.0;
        const double AverageHighEdgeAsk = EdgeCount > 0
            ? static_cast<double>(BuyEdgeAsk)
                / static_cast<double>(EdgeCount)
            : 0.0;

        if (AverageBidPerLevel > 0.0)
            Result.LowBidEdgeMultiple =
                AverageLowEdgeBid / AverageBidPerLevel;

        if (AverageAskPerLevel > 0.0)
            Result.HighAskEdgeMultiple =
                AverageHighEdgeAsk / AverageAskPerLevel;

        const int RequiredDominantLevels = MinimumInt(
            MaximumInt(1, AbsorptionMinimumDominantLevels),
            EdgeCount);

        const bool SellAggregateRatio = RatioAtLeast(
            SellEdgeBid,
            SellEdgeOpposingAsk,
            AbsorptionRatio,
            MinimumImbalanceVolume);

        const bool BuyAggregateRatio = RatioAtLeast(
            BuyEdgeAsk,
            BuyEdgeOpposingBid,
            AbsorptionRatio,
            MinimumImbalanceVolume);

        Result.SellAbsorptionRaw =
            SellDominantCount >= RequiredDominantLevels
            && SellAggregateRatio
            && Result.LowBidEdgeMultiple >= AbsorptionEdgeVolumeMultiple;

        Result.BuyAbsorptionRaw =
            BuyDominantCount >= RequiredDominantLevels
            && BuyAggregateRatio
            && Result.HighAskEdgeMultiple >= AbsorptionEdgeVolumeMultiple;

        Result.Valid = Result.TotalVolume > 0
            && Result.ClassifiedVolume > 0;

        return Result;
    }

    void AddCategory(unsigned int& Mask, const int Category)
    {
        if (Category < 0 || Category >= 31)
            return;
        Mask |= (1U << static_cast<unsigned int>(Category));
    }

    void EvaluateReferenceLevel(
        const ReferenceLevel& Level,
        const double PreviousClose,
        const double BarOpen,
        const double BarHigh,
        const double BarLow,
        const double BarClose,
        const double Proximity,
        const double BreakoutBuffer,
        const bool HasConcentratedBarPOC,
        const double BarPOCPrice,
        const double HighVolumePriceProximity,
        ContextScores& Scores)
    {
        const bool CanActAsSupport =
            Level.Role == LEVEL_SUPPORT
            || Level.Role == LEVEL_TWO_SIDED;
        const bool CanActAsResistance =
            Level.Role == LEVEL_RESISTANCE
            || Level.Role == LEVEL_TWO_SIDED;

        const bool BarNearLevel =
            BarLow <= Level.Price + Proximity
            && BarHigh >= Level.Price - Proximity;
        const bool HighVolumePriceNearLevel =
            HasConcentratedBarPOC
            && std::fabs(BarPOCPrice - Level.Price)
                <= HighVolumePriceProximity;

        if (CanActAsSupport)
        {
            const bool ApproachedFromAbove =
                PreviousClose >= Level.Price - Proximity * 2.0
                || BarOpen >= Level.Price - Proximity;

            if (BarNearLevel
                && ApproachedFromAbove
                && BarClose >= Level.Price)
            {
                AddCategory(
                    Scores.LongRejectionMask,
                    Level.Category);
                if (HighVolumePriceNearLevel)
                {
                    AddCategory(
                        Scores.LongRejectionHighVolumeMask,
                        Level.Category);
                }
            }

            const bool BrokeDown =
                PreviousClose >= Level.Price - Proximity
                && BarLow < Level.Price
                && BarClose <= Level.Price - BreakoutBuffer;

            if (BrokeDown)
            {
                AddCategory(
                    Scores.ShortBreakdownMask,
                    Level.Category);
                if (HighVolumePriceNearLevel)
                {
                    AddCategory(
                        Scores.ShortBreakdownHighVolumeMask,
                        Level.Category);
                }
            }
        }

        if (CanActAsResistance)
        {
            const bool ApproachedFromBelow =
                PreviousClose <= Level.Price + Proximity * 2.0
                || BarOpen <= Level.Price + Proximity;

            if (BarNearLevel
                && ApproachedFromBelow
                && BarClose <= Level.Price)
            {
                AddCategory(
                    Scores.ShortRejectionMask,
                    Level.Category);
                if (HighVolumePriceNearLevel)
                {
                    AddCategory(
                        Scores.ShortRejectionHighVolumeMask,
                        Level.Category);
                }
            }

            const bool BrokeOut =
                PreviousClose <= Level.Price + Proximity
                && BarHigh > Level.Price
                && BarClose >= Level.Price + BreakoutBuffer;

            if (BrokeOut)
            {
                AddCategory(
                    Scores.LongBreakoutMask,
                    Level.Category);
                if (HighVolumePriceNearLevel)
                {
                    AddCategory(
                        Scores.LongBreakoutHighVolumeMask,
                        Level.Category);
                }
            }
        }
    }

    int CalculateCandidateScore(
        const int ContextPoints,
        const bool CandleGate,
        const bool VolumeGate,
        const bool VolumeRatioPoint,
        const bool BarPOCPoint,
        const bool CurrentDeltaPoint,
        const bool StackedImbalancePoint,
        const bool AbsorptionPoint,
        const bool RollingDeltaPoint,
        const int MinimumContextCategories)
    {
        const bool CurrentOrderFlowGate =
            CurrentDeltaPoint
            || StackedImbalancePoint
            || AbsorptionPoint;

        if (ContextPoints < MinimumContextCategories
            || !CandleGate
            || !VolumeGate
            || !CurrentOrderFlowGate)
        {
            return 0;
        }

        int Score = MinimumInt(ContextPoints, 3);
        Score += 2; // Candle behavior is mandatory and receives two points.

        if (VolumeRatioPoint)
            ++Score;
        if (BarPOCPoint)
            ++Score;

        int OrderFlowPoints = 0;
        if (CurrentDeltaPoint)
            ++OrderFlowPoints;
        if (StackedImbalancePoint)
            ++OrderFlowPoints;
        if (AbsorptionPoint)
            ++OrderFlowPoints;
        if (RollingDeltaPoint)
            ++OrderFlowPoints;

        Score += MinimumInt(OrderFlowPoints, 3);
        return MinimumInt(Score, 10);
    }

    void UpdateHighLow(
        const float BarHigh,
        const float BarLow,
        float& High,
        float& Low,
        bool& Valid)
    {
        if (!Valid)
        {
            High = BarHigh;
            Low = BarLow;
            Valid = true;
            return;
        }

        if (BarHigh > High)
            High = BarHigh;
        if (BarLow < Low)
            Low = BarLow;
    }

    void BeginNewTradingDay(
        EngineState& State,
        const int NewTradingDate,
        const double ValueAreaPercent,
        const double MinimumHVNPercentOfPOC,
        const int MinimumHVNSeparationTicks)
    {
        if (State.HasTradingDay)
        {
            State.PreviousFullHigh = State.CurrentFullHigh;
            State.PreviousFullLow = State.CurrentFullLow;
            State.PreviousFullValid = State.CurrentFullValid;

            State.PreviousRTHHigh = State.CurrentRTHHigh;
            State.PreviousRTHLow = State.CurrentRTHLow;
            State.PreviousRTHValid = State.CurrentRTHValid;

            State.PreviousRTHLevels = CalculateProfileLevels(
                State.CurrentRTHProfile,
                ValueAreaPercent,
                MinimumHVNPercentOfPOC,
                MinimumHVNSeparationTicks);
        }

        State.CurrentTradingDate = NewTradingDate;
        State.HasTradingDay = true;

        State.CurrentFullHigh = 0.0f;
        State.CurrentFullLow = 0.0f;
        State.CurrentFullValid = false;
        State.CurrentRTHHigh = 0.0f;
        State.CurrentRTHLow = 0.0f;
        State.CurrentRTHValid = false;

        State.CurrentFullProfile.clear();
        State.CurrentRTHProfile.clear();
        State.CurrentOvernightProfile.clear();

        State.CurrentFullLevels = ProfileLevels();
        State.CurrentRTHLevels = ProfileLevels();
        State.OvernightLevels = ProfileLevels();

        State.CurrentFullProfileBars = 0;
        State.CurrentRTHProfileBars = 0;
        State.CurrentOvernightProfileBars = 0;
    }

    bool GetSelectedCurrentHighLow(
        const EngineState& State,
        const int Basis,
        double& High,
        double& Low)
    {
        if (Basis == DAY_HIGH_LOW_FULL_TRADING_DAY)
        {
            if (!State.CurrentFullValid)
                return false;
            High = State.CurrentFullHigh;
            Low = State.CurrentFullLow;
            return true;
        }

        if (!State.CurrentRTHValid)
            return false;
        High = State.CurrentRTHHigh;
        Low = State.CurrentRTHLow;
        return true;
    }

    bool GetSelectedPreviousHighLow(
        const EngineState& State,
        const int Basis,
        double& High,
        double& Low)
    {
        if (Basis == DAY_HIGH_LOW_FULL_TRADING_DAY)
        {
            if (!State.PreviousFullValid)
                return false;
            High = State.PreviousFullHigh;
            Low = State.PreviousFullLow;
            return true;
        }

        if (!State.PreviousRTHValid)
            return false;
        High = State.PreviousRTHHigh;
        Low = State.PreviousRTHLow;
        return true;
    }

    const ProfileLevels& GetSelectedCurrentProfileLevels(
        const EngineState& State,
        const int Basis)
    {
        if (Basis == HVN_FULL_TRADING_DAY)
            return State.CurrentFullLevels;
        return State.CurrentRTHLevels;
    }

    int GetSelectedCurrentProfileBars(
        const EngineState& State,
        const int Basis)
    {
        if (Basis == HVN_FULL_TRADING_DAY)
            return State.CurrentFullProfileBars;
        return State.CurrentRTHProfileBars;
    }

    bool SessionIsSignalEligible(
        const SessionKind Session,
        const int Filter)
    {
        if (Filter == SIGNAL_ALL_INCLUDED_SESSIONS)
            return Session == SESSION_RTH || Session == SESSION_OVERNIGHT;
        if (Filter == SIGNAL_OVERNIGHT_ONLY)
            return Session == SESSION_OVERNIGHT;
        return Session == SESSION_RTH;
    }

    bool RTHTimeWindowAllowsSignal(
        const int TimeValue,
        const int RTHStart,
        const int RTHEnd,
        const int MinimumMinutesAfterOpen,
        const int StopMinutesBeforeEnd)
    {
        const int SecondsAfterOpen = TimeValue - RTHStart;
        const int SecondsBeforeEnd = RTHEnd - TimeValue;

        if (SecondsAfterOpen
            < MaximumInt(0, MinimumMinutesAfterOpen) * 60)
        {
            return false;
        }

        if (SecondsBeforeEnd
            < MaximumInt(0, StopMinutesBeforeEnd) * 60)
        {
            return false;
        }

        return true;
    }

    void PushVolumeHistory(
        std::deque<double>& History,
        double& HistorySum,
        const double Volume,
        const int MaximumLength)
    {
        History.push_back(Volume);
        HistorySum += Volume;

        const int Length = MaximumInt(1, MaximumLength);
        while (static_cast<int>(History.size()) > Length)
        {
            HistorySum -= History.front();
            History.pop_front();
        }
    }

    void PushDeltaHistory(
        std::deque<double>& History,
        const double Delta,
        const int MaximumLength)
    {
        History.push_back(Delta);
        const int Length = MaximumInt(1, MaximumLength);
        while (static_cast<int>(History.size()) > Length)
            History.pop_front();
    }

    double RollingDeltaIncludingCurrent(
        const std::deque<double>& History,
        const double CurrentDelta,
        const int Length)
    {
        const int RequiredLength = MaximumInt(1, Length);
        double Sum = CurrentDelta;
        int Used = 1;

        for (std::deque<double>::const_reverse_iterator Iterator =
                 History.rbegin();
             Iterator != History.rend()
                 && Used < RequiredLength;
             ++Iterator)
        {
            Sum += *Iterator;
            ++Used;
        }

        return Sum;
    }
}

SCSFExport scsf_YMYMProfileOrderFlowConfluenceSignals(
    SCStudyInterfaceRef sc)
{
    SCSubgraphRef BuyArrow = sc.Subgraph[0];
    SCSubgraphRef SellArrow = sc.Subgraph[1];
    SCSubgraphRef BuyScoreLabel = sc.Subgraph[2];
    SCSubgraphRef SellScoreLabel = sc.Subgraph[3];
    SCSubgraphRef LongScore = sc.Subgraph[4];
    SCSubgraphRef ShortScore = sc.Subgraph[5];
    SCSubgraphRef SignalType = sc.Subgraph[6];

    SCSubgraphRef CurrentDayHigh = sc.Subgraph[7];
    SCSubgraphRef CurrentDayLow = sc.Subgraph[8];
    SCSubgraphRef PreviousDayHigh = sc.Subgraph[9];
    SCSubgraphRef PreviousDayLow = sc.Subgraph[10];
    SCSubgraphRef OvernightVAH = sc.Subgraph[11];
    SCSubgraphRef OvernightVPOC = sc.Subgraph[12];
    SCSubgraphRef OvernightVAL = sc.Subgraph[13];
    SCSubgraphRef PreviousRTHVAH = sc.Subgraph[14];
    SCSubgraphRef PreviousRTHVPOC = sc.Subgraph[15];
    SCSubgraphRef PreviousRTHVAL = sc.Subgraph[16];
    SCSubgraphRef CurrentPrimaryHVN = sc.Subgraph[17];
    SCSubgraphRef CurrentSecondaryHVN1 = sc.Subgraph[18];
    SCSubgraphRef CurrentSecondaryHVN2 = sc.Subgraph[19];

    SCSubgraphRef DeltaPercent = sc.Subgraph[20];
    SCSubgraphRef VolumeRatio = sc.Subgraph[21];
    SCSubgraphRef BarPOCMultiple = sc.Subgraph[22];
    SCSubgraphRef BullishStackCount = sc.Subgraph[23];
    SCSubgraphRef BearishStackCount = sc.Subgraph[24];
    SCSubgraphRef SellAbsorption = sc.Subgraph[25];
    SCSubgraphRef BuyAbsorption = sc.Subgraph[26];
    SCSubgraphRef LongContextPoints = sc.Subgraph[27];
    SCSubgraphRef ShortContextPoints = sc.Subgraph[28];
    SCSubgraphRef LongRejectionScore = sc.Subgraph[29];
    SCSubgraphRef LongBreakoutScore = sc.Subgraph[30];
    SCSubgraphRef ShortRejectionScore = sc.Subgraph[31];
    SCSubgraphRef ShortBreakdownScore = sc.Subgraph[32];
    SCSubgraphRef DataStatus = sc.Subgraph[33];
    SCSubgraphRef LongHighVolumeContext = sc.Subgraph[34];
    SCSubgraphRef ShortHighVolumeContext = sc.Subgraph[35];

    SCInputRef RTHStartTimeInput = sc.Input[0];
    SCInputRef RTHEndTimeInput = sc.Input[1];
    SCInputRef OvernightStartTimeInput = sc.Input[2];
    SCInputRef SignalSessionFilterInput = sc.Input[3];
    SCInputRef DayHighLowBasisInput = sc.Input[4];
    SCInputRef CurrentHVNBasisInput = sc.Input[5];
    SCInputRef ValueAreaPercentInput = sc.Input[6];
    SCInputRef LevelProximityPointsInput = sc.Input[7];
    SCInputRef BreakoutBufferPointsInput = sc.Input[8];
    SCInputRef MinimumContextCategoriesInput = sc.Input[9];
    SCInputRef SignalScoreThresholdInput = sc.Input[10];
    SCInputRef MinimumScoreLeadInput = sc.Input[11];
    SCInputRef MinimumBarsBetweenSignalsInput = sc.Input[12];
    SCInputRef VolumeAverageLookbackInput = sc.Input[13];
    SCInputRef MinimumVolumeAverageBarsInput = sc.Input[14];
    SCInputRef MinimumReversalVolumeRatioInput = sc.Input[15];
    SCInputRef MinimumBreakoutVolumeRatioInput = sc.Input[16];
    SCInputRef MinimumBarPOCMultipleInput = sc.Input[17];
    SCInputRef MinimumClassifiedVolumePercentInput = sc.Input[18];
    SCInputRef MinimumAbsoluteBarVolumeInput = sc.Input[19];
    SCInputRef MinimumDeltaPercentInput = sc.Input[20];
    SCInputRef DiagonalImbalanceRatioInput = sc.Input[21];
    SCInputRef MinimumImbalanceVolumeInput = sc.Input[22];
    SCInputRef MinimumStackedLevelsInput = sc.Input[23];
    SCInputRef AbsorptionEdgeLevelsInput = sc.Input[24];
    SCInputRef AbsorptionMinimumDominantLevelsInput = sc.Input[25];
    SCInputRef AbsorptionRatioInput = sc.Input[26];
    SCInputRef AbsorptionEdgeVolumeMultipleInput = sc.Input[27];
    SCInputRef RollingDeltaBarsInput = sc.Input[28];
    SCInputRef MinimumRejectionCloseLocationInput = sc.Input[29];
    SCInputRef MinimumRejectionWickPercentInput = sc.Input[30];
    SCInputRef MinimumMomentumCloseLocationInput = sc.Input[31];
    SCInputRef MinimumMomentumBodyPercentInput = sc.Input[32];
    SCInputRef RequireCandleDirectionInput = sc.Input[33];
    SCInputRef MinimumSignalBarRangeInput = sc.Input[34];
    SCInputRef MaximumSignalBarRangeInput = sc.Input[35];
    SCInputRef MinimumHVNPercentOfPOCInput = sc.Input[36];
    SCInputRef MinimumHVNSeparationPointsInput = sc.Input[37];
    SCInputRef MinimumProfileBarsForHVNInput = sc.Input[38];
    SCInputRef EnableReversalSignalsInput = sc.Input[39];
    SCInputRef EnableBreakoutSignalsInput = sc.Input[40];
    SCInputRef MinimumMinutesAfterRTHOpenInput = sc.Input[41];
    SCInputRef StopMinutesBeforeRTHEndInput = sc.Input[42];
    SCInputRef ShowContextLinesInput = sc.Input[43];
    SCInputRef ShowScoreLabelsInput = sc.Input[44];
    SCInputRef ArrowOffsetPointsInput = sc.Input[45];
    SCInputRef ScoreLabelOffsetPointsInput = sc.Input[46];
    SCInputRef AlertSoundNumberInput = sc.Input[47];
    SCInputRef MaximumHistoricalBarsInput = sc.Input[48];
    SCInputRef RestrictToYMSymbolInput = sc.Input[49];
    SCInputRef RequireOnePointTickInput = sc.Input[50];
    SCInputRef HighVolumePriceProximityInput = sc.Input[51];

    if (sc.SetDefaults)
    {
        sc.GraphName =
            "YMU/YM Volume Profile Order Flow Confluence Signals v1";
        sc.StudyDescription =
            "Closed-bar YMU/YM BUY/SELL confluence engine. It internally "
            "calculates current and previous day highs/lows, current "
            "overnight value area, previous RTH value area, and developing "
            "current-day HVNs. A reference-level touch cannot create a "
            "signal by itself: candle behavior, same-session volume or "
            "context-aligned high-volume concentration, current-bar order "
            "flow, and a score threshold are mandatory.";

        sc.AutoLoop = 0;
        sc.GraphRegion = 0;
        sc.ScaleRangeType = SCALE_SAMEASREGION;
        sc.ValueFormat = 0;
        sc.MaintainVolumeAtPriceData = 1;
        sc.AlertOnlyOncePerBar = 1;
        sc.ResetAlertOnNewBar = 1;

        BuyArrow.Name = "BUY Signal";
        BuyArrow.DrawStyle = DRAWSTYLE_ARROW_UP;
        BuyArrow.PrimaryColor = RGB(0, 200, 0);
        BuyArrow.LineWidth = 5;
        BuyArrow.DrawZeros = false;

        SellArrow.Name = "SELL Signal";
        SellArrow.DrawStyle = DRAWSTYLE_ARROW_DOWN;
        SellArrow.PrimaryColor = RGB(225, 35, 35);
        SellArrow.LineWidth = 5;
        SellArrow.DrawZeros = false;

        BuyScoreLabel.Name = "BUY Score Label";
        BuyScoreLabel.DrawStyle =
            DRAWSTYLE_TRANSPARENT_CUSTOM_VALUE_AT_Y;
        BuyScoreLabel.PrimaryColor = RGB(0, 200, 0);
        BuyScoreLabel.LineWidth = 8;
        BuyScoreLabel.DrawZeros = false;

        SellScoreLabel.Name = "SELL Score Label";
        SellScoreLabel.DrawStyle =
            DRAWSTYLE_TRANSPARENT_CUSTOM_VALUE_AT_Y;
        SellScoreLabel.PrimaryColor = RGB(225, 35, 35);
        SellScoreLabel.LineWidth = 8;
        SellScoreLabel.DrawZeros = false;

        LongScore.Name = "Long Score 0-10";
        LongScore.DrawStyle = DRAWSTYLE_IGNORE;
        LongScore.DrawZeros = false;

        ShortScore.Name = "Short Score 0-10";
        ShortScore.DrawStyle = DRAWSTYLE_IGNORE;
        ShortScore.DrawZeros = false;

        SignalType.Name =
            "Signal Type (+1 Long Rejection, +2 Long Breakout, -1 Short Rejection, -2 Short Breakdown)";
        SignalType.DrawStyle = DRAWSTYLE_IGNORE;
        SignalType.DrawZeros = false;

        CurrentDayHigh.Name = "Current Day High";
        CurrentDayHigh.DrawStyle = DRAWSTYLE_LINE;
        CurrentDayHigh.PrimaryColor = RGB(0, 200, 210);
        CurrentDayHigh.LineWidth = 1;
        CurrentDayHigh.DrawZeros = false;

        CurrentDayLow.Name = "Current Day Low";
        CurrentDayLow.DrawStyle = DRAWSTYLE_LINE;
        CurrentDayLow.PrimaryColor = RGB(0, 200, 210);
        CurrentDayLow.LineWidth = 1;
        CurrentDayLow.DrawZeros = false;

        PreviousDayHigh.Name = "Previous Day High";
        PreviousDayHigh.DrawStyle = DRAWSTYLE_LINE;
        PreviousDayHigh.PrimaryColor = RGB(230, 200, 0);
        PreviousDayHigh.LineWidth = 2;
        PreviousDayHigh.DrawZeros = false;

        PreviousDayLow.Name = "Previous Day Low";
        PreviousDayLow.DrawStyle = DRAWSTYLE_LINE;
        PreviousDayLow.PrimaryColor = RGB(230, 200, 0);
        PreviousDayLow.LineWidth = 2;
        PreviousDayLow.DrawZeros = false;

        OvernightVAH.Name = "Current Overnight VAH";
        OvernightVAH.DrawStyle = DRAWSTYLE_LINE;
        OvernightVAH.PrimaryColor = RGB(70, 130, 230);
        OvernightVAH.LineWidth = 1;
        OvernightVAH.DrawZeros = false;

        OvernightVPOC.Name = "Current Overnight VPOC";
        OvernightVPOC.DrawStyle = DRAWSTYLE_LINE;
        OvernightVPOC.PrimaryColor = RGB(30, 90, 210);
        OvernightVPOC.LineWidth = 2;
        OvernightVPOC.DrawZeros = false;

        OvernightVAL.Name = "Current Overnight VAL";
        OvernightVAL.DrawStyle = DRAWSTYLE_LINE;
        OvernightVAL.PrimaryColor = RGB(70, 130, 230);
        OvernightVAL.LineWidth = 1;
        OvernightVAL.DrawZeros = false;

        PreviousRTHVAH.Name = "Previous RTH VAH";
        PreviousRTHVAH.DrawStyle = DRAWSTYLE_LINE;
        PreviousRTHVAH.PrimaryColor = RGB(235, 140, 20);
        PreviousRTHVAH.LineWidth = 1;
        PreviousRTHVAH.DrawZeros = false;

        PreviousRTHVPOC.Name = "Previous RTH VPOC";
        PreviousRTHVPOC.DrawStyle = DRAWSTYLE_LINE;
        PreviousRTHVPOC.PrimaryColor = RGB(220, 100, 0);
        PreviousRTHVPOC.LineWidth = 2;
        PreviousRTHVPOC.DrawZeros = false;

        PreviousRTHVAL.Name = "Previous RTH VAL";
        PreviousRTHVAL.DrawStyle = DRAWSTYLE_LINE;
        PreviousRTHVAL.PrimaryColor = RGB(235, 140, 20);
        PreviousRTHVAL.LineWidth = 1;
        PreviousRTHVAL.DrawZeros = false;

        CurrentPrimaryHVN.Name = "Current Day Primary HVN / VPOC";
        CurrentPrimaryHVN.DrawStyle = DRAWSTYLE_LINE;
        CurrentPrimaryHVN.PrimaryColor = RGB(170, 70, 210);
        CurrentPrimaryHVN.LineWidth = 3;
        CurrentPrimaryHVN.DrawZeros = false;

        CurrentSecondaryHVN1.Name = "Current Day Secondary HVN 1";
        CurrentSecondaryHVN1.DrawStyle = DRAWSTYLE_LINE;
        CurrentSecondaryHVN1.PrimaryColor = RGB(190, 110, 220);
        CurrentSecondaryHVN1.LineWidth = 1;
        CurrentSecondaryHVN1.DrawZeros = false;

        CurrentSecondaryHVN2.Name = "Current Day Secondary HVN 2";
        CurrentSecondaryHVN2.DrawStyle = DRAWSTYLE_LINE;
        CurrentSecondaryHVN2.PrimaryColor = RGB(190, 110, 220);
        CurrentSecondaryHVN2.LineWidth = 1;
        CurrentSecondaryHVN2.DrawZeros = false;

        DeltaPercent.Name = "Bar Delta Percent";
        DeltaPercent.DrawStyle = DRAWSTYLE_IGNORE;
        DeltaPercent.DrawZeros = false;

        VolumeRatio.Name = "Bar Volume / Prior Average";
        VolumeRatio.DrawStyle = DRAWSTYLE_IGNORE;
        VolumeRatio.DrawZeros = false;

        BarPOCMultiple.Name = "Bar POC Volume / Average Volume Per Price";
        BarPOCMultiple.DrawStyle = DRAWSTYLE_IGNORE;
        BarPOCMultiple.DrawZeros = false;

        BullishStackCount.Name = "Maximum Bullish Stacked Imbalance Count";
        BullishStackCount.DrawStyle = DRAWSTYLE_IGNORE;
        BullishStackCount.DrawZeros = false;

        BearishStackCount.Name = "Maximum Bearish Stacked Imbalance Count";
        BearishStackCount.DrawStyle = DRAWSTYLE_IGNORE;
        BearishStackCount.DrawZeros = false;

        SellAbsorption.Name = "Sell Absorption at Low (1/0)";
        SellAbsorption.DrawStyle = DRAWSTYLE_IGNORE;
        SellAbsorption.DrawZeros = false;

        BuyAbsorption.Name = "Buy Absorption at High (1/0)";
        BuyAbsorption.DrawStyle = DRAWSTYLE_IGNORE;
        BuyAbsorption.DrawZeros = false;

        LongContextPoints.Name = "Long Context Categories";
        LongContextPoints.DrawStyle = DRAWSTYLE_IGNORE;
        LongContextPoints.DrawZeros = false;

        ShortContextPoints.Name = "Short Context Categories";
        ShortContextPoints.DrawStyle = DRAWSTYLE_IGNORE;
        ShortContextPoints.DrawZeros = false;

        LongRejectionScore.Name = "Long Rejection Score";
        LongRejectionScore.DrawStyle = DRAWSTYLE_IGNORE;
        LongRejectionScore.DrawZeros = false;

        LongBreakoutScore.Name = "Long Breakout Score";
        LongBreakoutScore.DrawStyle = DRAWSTYLE_IGNORE;
        LongBreakoutScore.DrawZeros = false;

        ShortRejectionScore.Name = "Short Rejection Score";
        ShortRejectionScore.DrawStyle = DRAWSTYLE_IGNORE;
        ShortRejectionScore.DrawZeros = false;

        ShortBreakdownScore.Name = "Short Breakdown Score";
        ShortBreakdownScore.DrawStyle = DRAWSTYLE_IGNORE;
        ShortBreakdownScore.DrawZeros = false;

        DataStatus.Name =
            "Status (1 Valid, 0 Missing VAP, -1 Wrong Chart, -2 Poor Classification, -3 Relative-Volume Warmup)";
        DataStatus.DrawStyle = DRAWSTYLE_IGNORE;
        DataStatus.DrawZeros = false;

        LongHighVolumeContext.Name =
            "Long Context Categories with Concentrated Bar VPOC";
        LongHighVolumeContext.DrawStyle = DRAWSTYLE_IGNORE;
        LongHighVolumeContext.DrawZeros = false;

        ShortHighVolumeContext.Name =
            "Short Context Categories with Concentrated Bar VPOC";
        ShortHighVolumeContext.DrawStyle = DRAWSTYLE_IGNORE;
        ShortHighVolumeContext.DrawZeros = false;

        RTHStartTimeInput.Name =
            "RTH Start Time (Chart Time Zone)";
        RTHStartTimeInput.SetTime(HMS_TIME(9, 30, 0));

        RTHEndTimeInput.Name =
            "RTH End Time (Chart Time Zone)";
        RTHEndTimeInput.SetTime(HMS_TIME(15, 59, 59));

        OvernightStartTimeInput.Name =
            "Overnight Start Time (Chart Time Zone)";
        OvernightStartTimeInput.SetTime(HMS_TIME(18, 0, 0));

        SignalSessionFilterInput.Name = "Signal Session";
        SignalSessionFilterInput.SetCustomInputStrings(
            "RTH Only;RTH and Overnight;Overnight Only");
        SignalSessionFilterInput.SetCustomInputIndex(SIGNAL_RTH_ONLY);

        DayHighLowBasisInput.Name =
            "Current/Previous Day High-Low Basis";
        DayHighLowBasisInput.SetCustomInputStrings(
            "RTH Only;Full Trading Day (Overnight + RTH)");
        DayHighLowBasisInput.SetCustomInputIndex(DAY_HIGH_LOW_RTH);

        CurrentHVNBasisInput.Name =
            "Developing Current-Day HVN Profile Basis";
        CurrentHVNBasisInput.SetCustomInputStrings(
            "RTH Only;Full Trading Day (Overnight + RTH)");
        CurrentHVNBasisInput.SetCustomInputIndex(HVN_RTH);

        ValueAreaPercentInput.Name = "Volume Value Area Percentage";
        ValueAreaPercentInput.SetFloat(70.0f);
        ValueAreaPercentInput.SetFloatLimits(1.0f, 100.0f);

        LevelProximityPointsInput.Name =
            "Reference-Level Proximity in YM Points";
        LevelProximityPointsInput.SetFloat(10.0f);
        LevelProximityPointsInput.SetFloatLimits(0.0f, 1000.0f);

        BreakoutBufferPointsInput.Name =
            "Breakout/Breakdown Close Buffer in YM Points";
        BreakoutBufferPointsInput.SetFloat(2.0f);
        BreakoutBufferPointsInput.SetFloatLimits(0.0f, 1000.0f);

        MinimumContextCategoriesInput.Name =
            "Minimum Independent Context Categories";
        MinimumContextCategoriesInput.SetInt(1);
        MinimumContextCategoriesInput.SetIntLimits(1, 5);

        SignalScoreThresholdInput.Name =
            "BUY/SELL Signal Score Threshold (0-10)";
        SignalScoreThresholdInput.SetInt(7);
        SignalScoreThresholdInput.SetIntLimits(1, 10);

        MinimumScoreLeadInput.Name =
            "Minimum Score Lead Over Opposite Direction";
        MinimumScoreLeadInput.SetInt(1);
        MinimumScoreLeadInput.SetIntLimits(0, 10);

        MinimumBarsBetweenSignalsInput.Name =
            "Minimum Bars Between Any Signals";
        MinimumBarsBetweenSignalsInput.SetInt(3);
        MinimumBarsBetweenSignalsInput.SetIntLimits(0, 10000);

        VolumeAverageLookbackInput.Name =
            "Prior-Bar Volume Average Lookback";
        VolumeAverageLookbackInput.SetInt(20);
        VolumeAverageLookbackInput.SetIntLimits(1, 10000);

        MinimumVolumeAverageBarsInput.Name =
            "Minimum Prior Bars Before Volume-Ratio Signals";
        MinimumVolumeAverageBarsInput.SetInt(10);
        MinimumVolumeAverageBarsInput.SetIntLimits(1, 10000);

        MinimumReversalVolumeRatioInput.Name =
            "Minimum Volume Ratio for Rejection Signal";
        MinimumReversalVolumeRatioInput.SetFloat(1.20f);
        MinimumReversalVolumeRatioInput.SetFloatLimits(0.0f, 100.0f);

        MinimumBreakoutVolumeRatioInput.Name =
            "Minimum Volume Ratio for Breakout Signal";
        MinimumBreakoutVolumeRatioInput.SetFloat(1.40f);
        MinimumBreakoutVolumeRatioInput.SetFloatLimits(0.0f, 100.0f);

        MinimumBarPOCMultipleInput.Name =
            "High-Volume Price Concentration: Bar POC Multiple";
        MinimumBarPOCMultipleInput.SetFloat(1.80f);
        MinimumBarPOCMultipleInput.SetFloatLimits(0.0f, 100.0f);

        MinimumClassifiedVolumePercentInput.Name =
            "Minimum Bid+Ask Classified Volume Percentage";
        MinimumClassifiedVolumePercentInput.SetFloat(80.0f);
        MinimumClassifiedVolumePercentInput.SetFloatLimits(0.0f, 100.0f);

        MinimumAbsoluteBarVolumeInput.Name =
            "Minimum Bar Volume (0 = Disabled)";
        MinimumAbsoluteBarVolumeInput.SetInt(0);
        MinimumAbsoluteBarVolumeInput.SetIntLimits(0, 2000000000);

        MinimumDeltaPercentInput.Name =
            "Minimum Directional Bar Delta Percentage";
        MinimumDeltaPercentInput.SetFloat(10.0f);
        MinimumDeltaPercentInput.SetFloatLimits(0.0f, 100.0f);

        DiagonalImbalanceRatioInput.Name =
            "Diagonal Bid/Ask Imbalance Ratio";
        DiagonalImbalanceRatioInput.SetFloat(3.0f);
        DiagonalImbalanceRatioInput.SetFloatLimits(1.0f, 1000.0f);

        MinimumImbalanceVolumeInput.Name =
            "Minimum Aggressive Volume at Imbalance Price";
        MinimumImbalanceVolumeInput.SetInt(20);
        MinimumImbalanceVolumeInput.SetIntLimits(0, 2000000000);

        MinimumStackedLevelsInput.Name =
            "Minimum Consecutive Stacked Imbalance Levels";
        MinimumStackedLevelsInput.SetInt(2);
        MinimumStackedLevelsInput.SetIntLimits(1, 1000);

        AbsorptionEdgeLevelsInput.Name =
            "Absorption Edge Price Levels";
        AbsorptionEdgeLevelsInput.SetInt(3);
        AbsorptionEdgeLevelsInput.SetIntLimits(1, 1000);

        AbsorptionMinimumDominantLevelsInput.Name =
            "Absorption Minimum Dominant Edge Levels";
        AbsorptionMinimumDominantLevelsInput.SetInt(2);
        AbsorptionMinimumDominantLevelsInput.SetIntLimits(1, 1000);

        AbsorptionRatioInput.Name =
            "Absorption Diagonal Dominance Ratio";
        AbsorptionRatioInput.SetFloat(3.0f);
        AbsorptionRatioInput.SetFloatLimits(1.0f, 1000.0f);

        AbsorptionEdgeVolumeMultipleInput.Name =
            "Absorption Edge Volume / Bar Per-Level Average";
        AbsorptionEdgeVolumeMultipleInput.SetFloat(1.50f);
        AbsorptionEdgeVolumeMultipleInput.SetFloatLimits(0.0f, 100.0f);

        RollingDeltaBarsInput.Name =
            "Rolling Delta Confirmation Bars";
        RollingDeltaBarsInput.SetInt(3);
        RollingDeltaBarsInput.SetIntLimits(1, 1000);

        MinimumRejectionCloseLocationInput.Name =
            "Rejection Candle Minimum Close Location % Away from Extreme";
        MinimumRejectionCloseLocationInput.SetFloat(65.0f);
        MinimumRejectionCloseLocationInput.SetFloatLimits(0.0f, 100.0f);

        MinimumRejectionWickPercentInput.Name =
            "Rejection Candle Minimum Wick % of Range";
        MinimumRejectionWickPercentInput.SetFloat(20.0f);
        MinimumRejectionWickPercentInput.SetFloatLimits(0.0f, 100.0f);

        MinimumMomentumCloseLocationInput.Name =
            "Momentum Candle Minimum Close Location %";
        MinimumMomentumCloseLocationInput.SetFloat(75.0f);
        MinimumMomentumCloseLocationInput.SetFloatLimits(0.0f, 100.0f);

        MinimumMomentumBodyPercentInput.Name =
            "Momentum Candle Minimum Body % of Range";
        MinimumMomentumBodyPercentInput.SetFloat(50.0f);
        MinimumMomentumBodyPercentInput.SetFloatLimits(0.0f, 100.0f);

        RequireCandleDirectionInput.Name =
            "Require Bullish BUY Candle / Bearish SELL Candle";
        RequireCandleDirectionInput.SetYesNo(1);

        MinimumSignalBarRangeInput.Name =
            "Minimum Signal Candle Range in YM Points";
        MinimumSignalBarRangeInput.SetFloat(4.0f);
        MinimumSignalBarRangeInput.SetFloatLimits(0.0f, 100000.0f);

        MaximumSignalBarRangeInput.Name =
            "Maximum Signal Candle Range in YM Points (0 = Disabled)";
        MaximumSignalBarRangeInput.SetFloat(0.0f);
        MaximumSignalBarRangeInput.SetFloatLimits(0.0f, 100000.0f);

        MinimumHVNPercentOfPOCInput.Name =
            "Secondary HVN Minimum % of Current VPOC Volume";
        MinimumHVNPercentOfPOCInput.SetFloat(65.0f);
        MinimumHVNPercentOfPOCInput.SetFloatLimits(0.0f, 100.0f);

        MinimumHVNSeparationPointsInput.Name =
            "Minimum Separation Between Current HVNs in YM Points";
        MinimumHVNSeparationPointsInput.SetFloat(8.0f);
        MinimumHVNSeparationPointsInput.SetFloatLimits(1.0f, 10000.0f);

        MinimumProfileBarsForHVNInput.Name =
            "Minimum Current-Profile Bars Before HVNs Can Trigger";
        MinimumProfileBarsForHVNInput.SetInt(10);
        MinimumProfileBarsForHVNInput.SetIntLimits(1, 1000000);

        EnableReversalSignalsInput.Name =
            "Enable Rejection / Responsive Signals";
        EnableReversalSignalsInput.SetYesNo(1);

        EnableBreakoutSignalsInput.Name =
            "Enable Breakout / Acceptance Signals";
        EnableBreakoutSignalsInput.SetYesNo(1);

        MinimumMinutesAfterRTHOpenInput.Name =
            "Do Not Signal Until N Minutes After RTH Open";
        MinimumMinutesAfterRTHOpenInput.SetInt(5);
        MinimumMinutesAfterRTHOpenInput.SetIntLimits(0, 1440);

        StopMinutesBeforeRTHEndInput.Name =
            "Stop RTH Signals N Minutes Before RTH End";
        StopMinutesBeforeRTHEndInput.SetInt(5);
        StopMinutesBeforeRTHEndInput.SetIntLimits(0, 1440);

        ShowContextLinesInput.Name = "Show Internally Calculated Context Lines";
        ShowContextLinesInput.SetYesNo(1);

        ShowScoreLabelsInput.Name = "Show Numeric Score on Signal Candle";
        ShowScoreLabelsInput.SetYesNo(1);

        ArrowOffsetPointsInput.Name = "Signal Arrow Offset in YM Points";
        ArrowOffsetPointsInput.SetFloat(8.0f);
        ArrowOffsetPointsInput.SetFloatLimits(0.0f, 10000.0f);

        ScoreLabelOffsetPointsInput.Name =
            "Score Label Offset Beyond Arrow in YM Points";
        ScoreLabelOffsetPointsInput.SetFloat(6.0f);
        ScoreLabelOffsetPointsInput.SetFloatLimits(0.0f, 10000.0f);

        AlertSoundNumberInput.Name = "Alert Sound Number (0 = Disabled)";
        AlertSoundNumberInput.SetInt(0);
        AlertSoundNumberInput.SetIntLimits(0, 150);

        MaximumHistoricalBarsInput.Name =
            "Maximum Historical Bars to Calculate (0 = All Loaded Bars)";
        MaximumHistoricalBarsInput.SetInt(30000);
        MaximumHistoricalBarsInput.SetIntLimits(0, 2000000000);

        RestrictToYMSymbolInput.Name =
            "Restrict Study to E-mini Dow YM/YMU Symbols";
        RestrictToYMSymbolInput.SetYesNo(1);

        RequireOnePointTickInput.Name =
            "Require YM 1-Point Tick Size";
        RequireOnePointTickInput.SetYesNo(1);

        HighVolumePriceProximityInput.Name =
            "Bar High-Volume Price Proximity to Active Reference (YM Points)";
        HighVolumePriceProximityInput.SetFloat(8.0f);
        HighVolumePriceProximityInput.SetFloatLimits(0.0f, 10000.0f);

        return;
    }

    void*& PersistentPointer = sc.GetPersistentPointer(1);
    EngineState* State =
        static_cast<EngineState*>(PersistentPointer);

    if (sc.LastCallToFunction)
    {
        if (State != NULL)
        {
            delete State;
            PersistentPointer = NULL;
        }
        return;
    }

    const bool SymbolIsValid =
        RestrictToYMSymbolInput.GetYesNo() == 0
        || IsYMEminiDowSymbol(sc.Symbol);
    const bool TickSizeIsValid =
        RequireOnePointTickInput.GetYesNo() == 0
        || IsYMOnePointTickSize(sc.TickSize);

    if (!SymbolIsValid || !TickSizeIsValid)
    {
        const int LastIndex = sc.ArraySize > 0 ? sc.ArraySize - 1 : 0;
        DataStatus[LastIndex] = -1.0f;
        return;
    }

    if (sc.ArraySize <= 0 || sc.VolumeAtPriceForBars == NULL)
        return;

    int LastClosedIndex = sc.ArraySize - 1;
    if (sc.GetBarHasClosedStatus(LastClosedIndex)
        == BHCS_BAR_HAS_NOT_CLOSED)
    {
        --LastClosedIndex;
    }

    if (LastClosedIndex < 0)
        return;

    const int MaximumHistoricalBars =
        MaximumHistoricalBarsInput.GetInt();
    int HistoricalStartIndex = 0;
    if (MaximumHistoricalBars > 0)
    {
        HistoricalStartIndex = MaximumInt(
            0,
            LastClosedIndex - MaximumHistoricalBars + 1);
    }

    bool ResetState = sc.IsFullRecalculation != 0
        || State == NULL
        || State->LastProcessedIndex > LastClosedIndex;

    if (ResetState)
    {
        if (State != NULL)
            delete State;

        State = new EngineState();
        PersistentPointer = State;
        State->LastProcessedIndex = HistoricalStartIndex - 1;

        for (int SubgraphIndex = 0; SubgraphIndex <= 35; ++SubgraphIndex)
        {
            for (int BarIndex = 0; BarIndex < sc.ArraySize; ++BarIndex)
                sc.Subgraph[SubgraphIndex][BarIndex] = 0.0f;
        }
    }

    int StartIndex = State->LastProcessedIndex + 1;
    if (StartIndex < HistoricalStartIndex)
        StartIndex = HistoricalStartIndex;

    const int RTHStart = RTHStartTimeInput.GetTime();
    const int RTHEnd = RTHEndTimeInput.GetTime();
    const int OvernightStart = OvernightStartTimeInput.GetTime();
    const int SignalFilter = SignalSessionFilterInput.GetIndex();
    const int HighLowBasis = DayHighLowBasisInput.GetIndex();
    const int HVNBasis = CurrentHVNBasisInput.GetIndex();

    const double ValueAreaPercent = ValueAreaPercentInput.GetFloat();
    const double LevelProximity = LevelProximityPointsInput.GetFloat();
    const double BreakoutBuffer = BreakoutBufferPointsInput.GetFloat();
    const double HighVolumePriceProximity =
        HighVolumePriceProximityInput.GetFloat();
    const int MinimumContextCategories =
        MaximumInt(1, MinimumContextCategoriesInput.GetInt());
    const int RequiredScore = SignalScoreThresholdInput.GetInt();
    const int MinimumScoreLead = MinimumScoreLeadInput.GetInt();
    const int CooldownBars = MinimumBarsBetweenSignalsInput.GetInt();

    const int VolumeLookback =
        MaximumInt(1, VolumeAverageLookbackInput.GetInt());
    const int MinimumVolumeHistoryBars =
        MaximumInt(1, MinimumVolumeAverageBarsInput.GetInt());
    const double MinimumReversalVolumeRatio =
        MinimumReversalVolumeRatioInput.GetFloat();
    const double MinimumBreakoutVolumeRatio =
        MinimumBreakoutVolumeRatioInput.GetFloat();
    const double MinimumBarPOCMultiple =
        MinimumBarPOCMultipleInput.GetFloat();
    const double MinimumClassifiedPercent =
        MinimumClassifiedVolumePercentInput.GetFloat();
    const uint64_t MinimumAbsoluteBarVolume =
        static_cast<uint64_t>(MaximumInt(
            0,
            MinimumAbsoluteBarVolumeInput.GetInt()));

    const double MinimumDeltaPercent =
        MinimumDeltaPercentInput.GetFloat();
    const double DiagonalImbalanceRatio =
        DiagonalImbalanceRatioInput.GetFloat();
    const uint64_t MinimumImbalanceVolume =
        static_cast<uint64_t>(MaximumInt(
            0,
            MinimumImbalanceVolumeInput.GetInt()));
    const int MinimumStackedLevels =
        MaximumInt(1, MinimumStackedLevelsInput.GetInt());
    const int AbsorptionEdgeLevels =
        MaximumInt(1, AbsorptionEdgeLevelsInput.GetInt());
    const int AbsorptionMinimumDominantLevels =
        MaximumInt(1, AbsorptionMinimumDominantLevelsInput.GetInt());
    const double AbsorptionRatio =
        AbsorptionRatioInput.GetFloat();
    const double AbsorptionEdgeVolumeMultiple =
        AbsorptionEdgeVolumeMultipleInput.GetFloat();
    const int RollingDeltaBars =
        MaximumInt(1, RollingDeltaBarsInput.GetInt());

    const double MinimumRejectionCloseLocation =
        MinimumRejectionCloseLocationInput.GetFloat() / 100.0;
    const double MinimumRejectionWick =
        MinimumRejectionWickPercentInput.GetFloat() / 100.0;
    const double MinimumMomentumCloseLocation =
        MinimumMomentumCloseLocationInput.GetFloat() / 100.0;
    const double MinimumMomentumBody =
        MinimumMomentumBodyPercentInput.GetFloat() / 100.0;
    const bool RequireCandleDirection =
        RequireCandleDirectionInput.GetYesNo() != 0;
    const double MinimumBarRange =
        MinimumSignalBarRangeInput.GetFloat();
    const double MaximumBarRange =
        MaximumSignalBarRangeInput.GetFloat();

    const double MinimumHVNPercentOfPOC =
        MinimumHVNPercentOfPOCInput.GetFloat();
    const int MinimumHVNSeparationTicks = MaximumInt(
        1,
        static_cast<int>(std::floor(
            MinimumHVNSeparationPointsInput.GetFloat()
                / sc.TickSize
            + 0.5)));
    const int MinimumProfileBarsForHVN =
        MaximumInt(1, MinimumProfileBarsForHVNInput.GetInt());

    const bool EnableReversal =
        EnableReversalSignalsInput.GetYesNo() != 0;
    const bool EnableBreakout =
        EnableBreakoutSignalsInput.GetYesNo() != 0;
    const int MinimumMinutesAfterOpen =
        MinimumMinutesAfterRTHOpenInput.GetInt();
    const int StopMinutesBeforeEnd =
        StopMinutesBeforeRTHEndInput.GetInt();
    const bool ShowLines =
        ShowContextLinesInput.GetYesNo() != 0;
    const bool ShowScoreLabels =
        ShowScoreLabelsInput.GetYesNo() != 0;
    const double ArrowOffset = ArrowOffsetPointsInput.GetFloat();
    const double ScoreLabelOffset =
        ScoreLabelOffsetPointsInput.GetFloat();
    const int AlertSoundNumber = AlertSoundNumberInput.GetInt();

    std::vector<BarLevel> Levels;

    for (int BarIndex = StartIndex;
         BarIndex <= LastClosedIndex;
         ++BarIndex)
    {
        for (int SubgraphIndex = 0; SubgraphIndex <= 35; ++SubgraphIndex)
            sc.Subgraph[SubgraphIndex][BarIndex] = 0.0f;
        BuyScoreLabel.Arrays[0][BarIndex] = 0.0f;
        SellScoreLabel.Arrays[0][BarIndex] = 0.0f;

        const SCDateTime& BarDateTime = sc.BaseDateTimeIn[BarIndex];
        const int TimeValue = BarDateTime.GetTime();
        const SessionKind Session = GetSessionKind(
            TimeValue,
            RTHStart,
            RTHEnd,
            OvernightStart);
        const int TradingDate = GetTradingDate(
            BarDateTime,
            OvernightStart);

        if (!State->HasTradingDay
            || TradingDate != State->CurrentTradingDate)
        {
            BeginNewTradingDay(
                *State,
                TradingDate,
                ValueAreaPercent,
                MinimumHVNPercentOfPOC,
                MinimumHVNSeparationTicks);
        }

        // Snapshot all context before the current candle is added. This avoids
        // using the signal candle to manufacture its own current-day level.
        double PreCurrentDayHigh = 0.0;
        double PreCurrentDayLow = 0.0;
        const bool PreCurrentDayHighLowValid =
            GetSelectedCurrentHighLow(
                *State,
                HighLowBasis,
                PreCurrentDayHigh,
                PreCurrentDayLow);

        double PreviousDayHighValue = 0.0;
        double PreviousDayLowValue = 0.0;
        const bool PreviousDayHighLowValid =
            GetSelectedPreviousHighLow(
                *State,
                HighLowBasis,
                PreviousDayHighValue,
                PreviousDayLowValue);

        const ProfileLevels PreCurrentHVNLevels =
            GetSelectedCurrentProfileLevels(*State, HVNBasis);
        const int PreCurrentProfileBars =
            GetSelectedCurrentProfileBars(*State, HVNBasis);

        const ProfileLevels OvernightProfileLevels =
            State->OvernightLevels;
        const ProfileLevels PreviousRTHProfileLevels =
            State->PreviousRTHLevels;

        const float BarOpen = sc.Open[BarIndex];
        const float BarHigh = sc.High[BarIndex];
        const float BarLow = sc.Low[BarIndex];
        const float BarClose = sc.Close[BarIndex];
        const double BarRange =
            static_cast<double>(BarHigh) - static_cast<double>(BarLow);
        const double PreviousClose = BarIndex > 0
            ? static_cast<double>(sc.Close[BarIndex - 1])
            : static_cast<double>(BarOpen);

        const bool VAPLoaded = LoadBarLevels(
            sc,
            BarIndex,
            Levels);

        BarOrderFlowStats OrderFlow;
        if (VAPLoaded)
        {
            OrderFlow = CalculateBarOrderFlowStats(
                Levels,
                DiagonalImbalanceRatio,
                MinimumImbalanceVolume,
                AbsorptionEdgeLevels,
                AbsorptionMinimumDominantLevels,
                AbsorptionRatio,
                AbsorptionEdgeVolumeMultiple);
        }

        // Build reference-level context from levels that existed before this bar.
        std::vector<ReferenceLevel> References;
        References.reserve(16);

        if (PreCurrentDayHighLowValid)
        {
            References.push_back(ReferenceLevel(
                PreCurrentDayHigh,
                LEVEL_RESISTANCE,
                CATEGORY_CURRENT_DAY_EXTREME));
            References.push_back(ReferenceLevel(
                PreCurrentDayLow,
                LEVEL_SUPPORT,
                CATEGORY_CURRENT_DAY_EXTREME));
        }

        if (PreviousDayHighLowValid)
        {
            References.push_back(ReferenceLevel(
                PreviousDayHighValue,
                LEVEL_RESISTANCE,
                CATEGORY_PREVIOUS_DAY_EXTREME));
            References.push_back(ReferenceLevel(
                PreviousDayLowValue,
                LEVEL_SUPPORT,
                CATEGORY_PREVIOUS_DAY_EXTREME));
        }

        if (OvernightProfileLevels.Valid)
        {
            References.push_back(ReferenceLevel(
                TicksToPrice(
                    OvernightProfileLevels.ValueAreaHighTicks,
                    sc.TickSize),
                LEVEL_RESISTANCE,
                CATEGORY_OVERNIGHT_PROFILE));
            References.push_back(ReferenceLevel(
                TicksToPrice(
                    OvernightProfileLevels.PointOfControlTicks,
                    sc.TickSize),
                LEVEL_TWO_SIDED,
                CATEGORY_OVERNIGHT_PROFILE));
            References.push_back(ReferenceLevel(
                TicksToPrice(
                    OvernightProfileLevels.ValueAreaLowTicks,
                    sc.TickSize),
                LEVEL_SUPPORT,
                CATEGORY_OVERNIGHT_PROFILE));
        }

        if (PreviousRTHProfileLevels.Valid)
        {
            References.push_back(ReferenceLevel(
                TicksToPrice(
                    PreviousRTHProfileLevels.ValueAreaHighTicks,
                    sc.TickSize),
                LEVEL_RESISTANCE,
                CATEGORY_PREVIOUS_RTH_PROFILE));
            References.push_back(ReferenceLevel(
                TicksToPrice(
                    PreviousRTHProfileLevels.PointOfControlTicks,
                    sc.TickSize),
                LEVEL_TWO_SIDED,
                CATEGORY_PREVIOUS_RTH_PROFILE));
            References.push_back(ReferenceLevel(
                TicksToPrice(
                    PreviousRTHProfileLevels.ValueAreaLowTicks,
                    sc.TickSize),
                LEVEL_SUPPORT,
                CATEGORY_PREVIOUS_RTH_PROFILE));
        }

        if (PreCurrentHVNLevels.Valid
            && PreCurrentProfileBars >= MinimumProfileBarsForHVN)
        {
            References.push_back(ReferenceLevel(
                TicksToPrice(
                    PreCurrentHVNLevels.PointOfControlTicks,
                    sc.TickSize),
                LEVEL_TWO_SIDED,
                CATEGORY_CURRENT_DAY_HVN));

            if (PreCurrentHVNLevels.SecondaryHVN1Valid)
            {
                References.push_back(ReferenceLevel(
                    TicksToPrice(
                        PreCurrentHVNLevels.SecondaryHVN1Ticks,
                        sc.TickSize),
                    LEVEL_TWO_SIDED,
                    CATEGORY_CURRENT_DAY_HVN));
            }

            if (PreCurrentHVNLevels.SecondaryHVN2Valid)
            {
                References.push_back(ReferenceLevel(
                    TicksToPrice(
                        PreCurrentHVNLevels.SecondaryHVN2Ticks,
                        sc.TickSize),
                    LEVEL_TWO_SIDED,
                    CATEGORY_CURRENT_DAY_HVN));
            }
        }

        ContextScores Context;
        for (size_t LevelIndex = 0;
             LevelIndex < References.size();
             ++LevelIndex)
        {
            EvaluateReferenceLevel(
                References[LevelIndex],
                PreviousClose,
                BarOpen,
                BarHigh,
                BarLow,
                BarClose,
                LevelProximity,
                BreakoutBuffer,
                OrderFlow.Valid
                    && OrderFlow.BarPOCMultiple
                        >= MinimumBarPOCMultiple,
                TicksToPrice(OrderFlow.BarPOCTicks, sc.TickSize),
                HighVolumePriceProximity,
                Context);
        }

        const int LongRejectionContext =
            CountSetBits(Context.LongRejectionMask);
        const int ShortRejectionContext =
            CountSetBits(Context.ShortRejectionMask);
        const int LongBreakoutContext =
            CountSetBits(Context.LongBreakoutMask);
        const int ShortBreakdownContext =
            CountSetBits(Context.ShortBreakdownMask);

        const int LongRejectionHighVolumeContextCount =
            CountSetBits(Context.LongRejectionHighVolumeMask);
        const int ShortRejectionHighVolumeContextCount =
            CountSetBits(Context.ShortRejectionHighVolumeMask);
        const int LongBreakoutHighVolumeContextCount =
            CountSetBits(Context.LongBreakoutHighVolumeMask);
        const int ShortBreakdownHighVolumeContextCount =
            CountSetBits(Context.ShortBreakdownHighVolumeMask);

        const bool LongRejectionHighVolumeContext =
            LongRejectionHighVolumeContextCount > 0;
        const bool ShortRejectionHighVolumeContext =
            ShortRejectionHighVolumeContextCount > 0;
        const bool LongBreakoutHighVolumeContext =
            LongBreakoutHighVolumeContextCount > 0;
        const bool ShortBreakdownHighVolumeContext =
            ShortBreakdownHighVolumeContextCount > 0;

        const double CloseLocationFromLow = BarRange > 0.0
            ? (static_cast<double>(BarClose)
                - static_cast<double>(BarLow))
                / BarRange
            : 0.5;
        const double CloseLocationFromHigh =
            1.0 - CloseLocationFromLow;
        const double LowerWick = BarRange > 0.0
            ? (MinimumDouble(
                    static_cast<double>(BarOpen),
                    static_cast<double>(BarClose))
                - static_cast<double>(BarLow))
                / BarRange
            : 0.0;
        const double UpperWick = BarRange > 0.0
            ? (static_cast<double>(BarHigh)
                - MaximumDouble(
                    static_cast<double>(BarOpen),
                    static_cast<double>(BarClose)))
                / BarRange
            : 0.0;
        const double BodyPercent = BarRange > 0.0
            ? std::fabs(
                    static_cast<double>(BarClose)
                    - static_cast<double>(BarOpen))
                / BarRange
            : 0.0;

        const bool BullishDirection = BarClose > BarOpen;
        const bool BearishDirection = BarClose < BarOpen;
        const bool LongDirectionPass =
            !RequireCandleDirection || BullishDirection;
        const bool ShortDirectionPass =
            !RequireCandleDirection || BearishDirection;

        const bool LongRejectionCandle =
            CloseLocationFromLow >= MinimumRejectionCloseLocation
            && LowerWick >= MinimumRejectionWick
            && LongDirectionPass;
        const bool ShortRejectionCandle =
            CloseLocationFromHigh >= MinimumRejectionCloseLocation
            && UpperWick >= MinimumRejectionWick
            && ShortDirectionPass;

        const bool LongMomentumCandle =
            CloseLocationFromLow >= MinimumMomentumCloseLocation
            && BodyPercent >= MinimumMomentumBody
            && BullishDirection
            && BarClose > PreviousClose;
        const bool ShortMomentumCandle =
            CloseLocationFromHigh >= MinimumMomentumCloseLocation
            && BodyPercent >= MinimumMomentumBody
            && BearishDirection
            && BarClose < PreviousClose;

        const bool BarRangePass =
            BarRange >= MinimumBarRange
            && (MaximumBarRange <= 0.0
                || BarRange <= MaximumBarRange);

        const std::deque<double>* ActiveVolumeHistory = NULL;
        const std::deque<double>* ActiveDeltaHistory = NULL;
        double ActiveVolumeHistorySum = 0.0;

        if (Session == SESSION_RTH)
        {
            ActiveVolumeHistory = &State->RTHVolumeHistory;
            ActiveDeltaHistory = &State->RTHDeltaHistory;
            ActiveVolumeHistorySum = State->RTHVolumeHistorySum;
        }
        else if (Session == SESSION_OVERNIGHT)
        {
            ActiveVolumeHistory = &State->OvernightVolumeHistory;
            ActiveDeltaHistory = &State->OvernightDeltaHistory;
            ActiveVolumeHistorySum = State->OvernightVolumeHistorySum;
        }

        const bool HasVolumeAverage =
            ActiveVolumeHistory != NULL
            && static_cast<int>(ActiveVolumeHistory->size())
                >= MinimumVolumeHistoryBars
            && ActiveVolumeHistorySum > 0.0;
        const double PriorAverageVolume = HasVolumeAverage
            ? ActiveVolumeHistorySum
                / static_cast<double>(ActiveVolumeHistory->size())
            : 0.0;
        const double CurrentVolumeRatio =
            HasVolumeAverage && PriorAverageVolume > 0.0
            ? static_cast<double>(OrderFlow.TotalVolume)
                / PriorAverageVolume
            : 0.0;

        const bool AbsoluteVolumePass =
            OrderFlow.TotalVolume >= MinimumAbsoluteBarVolume;
        const bool ClassificationPass =
            OrderFlow.ClassifiedPercent >= MinimumClassifiedPercent;
        const bool ReversalVolumeRatioPass =
            HasVolumeAverage
            && CurrentVolumeRatio >= MinimumReversalVolumeRatio;
        const bool BreakoutVolumeRatioPass =
            HasVolumeAverage
            && CurrentVolumeRatio >= MinimumBreakoutVolumeRatio;
        const bool PositiveDeltaPass =
            OrderFlow.DeltaPercent >= MinimumDeltaPercent;
        const bool NegativeDeltaPass =
            OrderFlow.DeltaPercent <= -MinimumDeltaPercent;
        const bool BullishStackPass =
            OrderFlow.BullishStackedImbalance >= MinimumStackedLevels;
        const bool BearishStackPass =
            OrderFlow.BearishStackedImbalance >= MinimumStackedLevels;

        const bool SellAbsorptionConfirmed =
            OrderFlow.SellAbsorptionRaw
            && LongRejectionCandle;
        const bool BuyAbsorptionConfirmed =
            OrderFlow.BuyAbsorptionRaw
            && ShortRejectionCandle;

        const double RollingDelta = ActiveDeltaHistory != NULL
            ? RollingDeltaIncludingCurrent(
                *ActiveDeltaHistory,
                static_cast<double>(OrderFlow.Delta),
                RollingDeltaBars)
            : static_cast<double>(OrderFlow.Delta);
        const bool RollingDeltaPositive = RollingDelta > 0.0;
        const bool RollingDeltaNegative = RollingDelta < 0.0;

        const bool SignalSessionEligible =
            SessionIsSignalEligible(Session, SignalFilter);
        bool TimeWindowEligible = true;
        if (Session == SESSION_RTH)
        {
            TimeWindowEligible = RTHTimeWindowAllowsSignal(
                TimeValue,
                RTHStart,
                RTHEnd,
                MinimumMinutesAfterOpen,
                StopMinutesBeforeEnd);
        }

        const bool BaseDataGate =
            VAPLoaded
            && OrderFlow.Valid
            && ClassificationPass
            && AbsoluteVolumePass
            && BarRangePass
            && SignalSessionEligible
            && TimeWindowEligible;

        int LongRejectionCandidateScore = 0;
        int LongBreakoutCandidateScore = 0;
        int ShortRejectionCandidateScore = 0;
        int ShortBreakdownCandidateScore = 0;

        if (BaseDataGate && EnableReversal)
        {
            LongRejectionCandidateScore = CalculateCandidateScore(
                LongRejectionContext,
                LongRejectionCandle,
                ReversalVolumeRatioPass
                    || LongRejectionHighVolumeContext,
                ReversalVolumeRatioPass,
                LongRejectionHighVolumeContext,
                PositiveDeltaPass,
                BullishStackPass,
                SellAbsorptionConfirmed,
                RollingDeltaPositive,
                MinimumContextCategories);

            ShortRejectionCandidateScore = CalculateCandidateScore(
                ShortRejectionContext,
                ShortRejectionCandle,
                ReversalVolumeRatioPass
                    || ShortRejectionHighVolumeContext,
                ReversalVolumeRatioPass,
                ShortRejectionHighVolumeContext,
                NegativeDeltaPass,
                BearishStackPass,
                BuyAbsorptionConfirmed,
                RollingDeltaNegative,
                MinimumContextCategories);
        }

        if (BaseDataGate && EnableBreakout)
        {
            // Breakout/acceptance requires actual above-average total volume;
            // a concentrated but low-volume candle is not sufficient.
            LongBreakoutCandidateScore = CalculateCandidateScore(
                LongBreakoutContext,
                LongMomentumCandle,
                BreakoutVolumeRatioPass,
                BreakoutVolumeRatioPass,
                LongBreakoutHighVolumeContext,
                PositiveDeltaPass,
                BullishStackPass,
                false,
                RollingDeltaPositive,
                MinimumContextCategories);

            ShortBreakdownCandidateScore = CalculateCandidateScore(
                ShortBreakdownContext,
                ShortMomentumCandle,
                BreakoutVolumeRatioPass,
                BreakoutVolumeRatioPass,
                ShortBreakdownHighVolumeContext,
                NegativeDeltaPass,
                BearishStackPass,
                false,
                RollingDeltaNegative,
                MinimumContextCategories);
        }

        int BestLongScore = LongRejectionCandidateScore;
        int BestLongType = 1;
        if (LongBreakoutCandidateScore > BestLongScore)
        {
            BestLongScore = LongBreakoutCandidateScore;
            BestLongType = 2;
        }

        int BestShortScore = ShortRejectionCandidateScore;
        int BestShortType = -1;
        if (ShortBreakdownCandidateScore > BestShortScore)
        {
            BestShortScore = ShortBreakdownCandidateScore;
            BestShortType = -2;
        }

        LongScore[BarIndex] = static_cast<float>(BestLongScore);
        ShortScore[BarIndex] = static_cast<float>(BestShortScore);
        LongRejectionScore[BarIndex] =
            static_cast<float>(LongRejectionCandidateScore);
        LongBreakoutScore[BarIndex] =
            static_cast<float>(LongBreakoutCandidateScore);
        ShortRejectionScore[BarIndex] =
            static_cast<float>(ShortRejectionCandidateScore);
        ShortBreakdownScore[BarIndex] =
            static_cast<float>(ShortBreakdownCandidateScore);

        DeltaPercent[BarIndex] =
            static_cast<float>(OrderFlow.DeltaPercent);
        VolumeRatio[BarIndex] =
            static_cast<float>(CurrentVolumeRatio);
        BarPOCMultiple[BarIndex] =
            static_cast<float>(OrderFlow.BarPOCMultiple);
        BullishStackCount[BarIndex] =
            static_cast<float>(OrderFlow.BullishStackedImbalance);
        BearishStackCount[BarIndex] =
            static_cast<float>(OrderFlow.BearishStackedImbalance);
        SellAbsorption[BarIndex] =
            SellAbsorptionConfirmed ? 1.0f : 0.0f;
        BuyAbsorption[BarIndex] =
            BuyAbsorptionConfirmed ? 1.0f : 0.0f;
        LongContextPoints[BarIndex] = static_cast<float>(
            MaximumInt(LongRejectionContext, LongBreakoutContext));
        ShortContextPoints[BarIndex] = static_cast<float>(
            MaximumInt(ShortRejectionContext, ShortBreakdownContext));
        LongHighVolumeContext[BarIndex] = static_cast<float>(
            MaximumInt(
                LongRejectionHighVolumeContextCount,
                LongBreakoutHighVolumeContextCount));
        ShortHighVolumeContext[BarIndex] = static_cast<float>(
            MaximumInt(
                ShortRejectionHighVolumeContextCount,
                ShortBreakdownHighVolumeContextCount));

        if (!VAPLoaded || !OrderFlow.Valid)
            DataStatus[BarIndex] = 0.0f;
        else if (!ClassificationPass)
            DataStatus[BarIndex] = -2.0f;
        else if (!HasVolumeAverage)
            DataStatus[BarIndex] = -3.0f;
        else
            DataStatus[BarIndex] = 1.0f;

        const bool CooldownPass =
            BarIndex - State->LastSignalIndex >= CooldownBars;

        int FinalSignalType = 0;
        int FinalSignalScore = 0;

        if (CooldownPass
            && BestLongScore >= RequiredScore
            && BestLongScore - BestShortScore >= MinimumScoreLead)
        {
            FinalSignalType = BestLongType;
            FinalSignalScore = BestLongScore;
        }
        else if (CooldownPass
            && BestShortScore >= RequiredScore
            && BestShortScore - BestLongScore >= MinimumScoreLead)
        {
            FinalSignalType = BestShortType;
            FinalSignalScore = BestShortScore;
        }

        if (FinalSignalType > 0)
        {
            BuyArrow[BarIndex] = static_cast<float>(
                static_cast<double>(BarLow) - ArrowOffset);
            SignalType[BarIndex] =
                static_cast<float>(FinalSignalType);

            if (ShowScoreLabels)
            {
                BuyScoreLabel[BarIndex] =
                    static_cast<float>(FinalSignalScore);
                BuyScoreLabel.Arrays[0][BarIndex] = static_cast<float>(
                    static_cast<double>(BarLow)
                    - ArrowOffset
                    - ScoreLabelOffset);
            }

            State->LastSignalIndex = BarIndex;

            if (!sc.IsFullRecalculation
                && AlertSoundNumber > 0
                && State->LastAlertIndex != BarIndex)
            {
                SCString AlertMessage;
                AlertMessage.Format(
                    "YMU/YM BUY signal: type %d, score %d/10",
                    FinalSignalType,
                    FinalSignalScore);
                sc.SetAlert(
                    AlertSoundNumber,
                    BarIndex,
                    AlertMessage);
                State->LastAlertIndex = BarIndex;
            }
        }
        else if (FinalSignalType < 0)
        {
            SellArrow[BarIndex] = static_cast<float>(
                static_cast<double>(BarHigh) + ArrowOffset);
            SignalType[BarIndex] =
                static_cast<float>(FinalSignalType);

            if (ShowScoreLabels)
            {
                SellScoreLabel[BarIndex] =
                    static_cast<float>(FinalSignalScore);
                SellScoreLabel.Arrays[0][BarIndex] = static_cast<float>(
                    static_cast<double>(BarHigh)
                    + ArrowOffset
                    + ScoreLabelOffset);
            }

            State->LastSignalIndex = BarIndex;

            if (!sc.IsFullRecalculation
                && AlertSoundNumber > 0
                && State->LastAlertIndex != BarIndex)
            {
                SCString AlertMessage;
                AlertMessage.Format(
                    "YMU/YM SELL signal: type %d, score %d/10",
                    FinalSignalType,
                    FinalSignalScore);
                sc.SetAlert(
                    AlertSoundNumber,
                    BarIndex,
                    AlertMessage);
                State->LastAlertIndex = BarIndex;
            }
        }

        // Add the completed candle to the profiles after signal evaluation.
        if (Session == SESSION_RTH || Session == SESSION_OVERNIGHT)
        {
            UpdateHighLow(
                BarHigh,
                BarLow,
                State->CurrentFullHigh,
                State->CurrentFullLow,
                State->CurrentFullValid);

            if (VAPLoaded)
            {
                AddLevelsToProfile(Levels, State->CurrentFullProfile);
                ++State->CurrentFullProfileBars;
            }
        }

        if (Session == SESSION_RTH)
        {
            UpdateHighLow(
                BarHigh,
                BarLow,
                State->CurrentRTHHigh,
                State->CurrentRTHLow,
                State->CurrentRTHValid);

            if (VAPLoaded)
            {
                AddLevelsToProfile(Levels, State->CurrentRTHProfile);
                ++State->CurrentRTHProfileBars;
            }
        }
        else if (Session == SESSION_OVERNIGHT && VAPLoaded)
        {
            AddLevelsToProfile(Levels, State->CurrentOvernightProfile);
            ++State->CurrentOvernightProfileBars;
        }

        State->CurrentFullLevels = CalculateProfileLevels(
            State->CurrentFullProfile,
            ValueAreaPercent,
            MinimumHVNPercentOfPOC,
            MinimumHVNSeparationTicks);
        State->CurrentRTHLevels = CalculateProfileLevels(
            State->CurrentRTHProfile,
            ValueAreaPercent,
            MinimumHVNPercentOfPOC,
            MinimumHVNSeparationTicks);
        State->OvernightLevels = CalculateProfileLevels(
            State->CurrentOvernightProfile,
            ValueAreaPercent,
            MinimumHVNPercentOfPOC,
            MinimumHVNSeparationTicks);

        if (VAPLoaded && OrderFlow.TotalVolume > 0)
        {
            if (Session == SESSION_RTH)
            {
                PushVolumeHistory(
                    State->RTHVolumeHistory,
                    State->RTHVolumeHistorySum,
                    static_cast<double>(OrderFlow.TotalVolume),
                    VolumeLookback);
                PushDeltaHistory(
                    State->RTHDeltaHistory,
                    static_cast<double>(OrderFlow.Delta),
                    RollingDeltaBars);
            }
            else if (Session == SESSION_OVERNIGHT)
            {
                PushVolumeHistory(
                    State->OvernightVolumeHistory,
                    State->OvernightVolumeHistorySum,
                    static_cast<double>(OrderFlow.TotalVolume),
                    VolumeLookback);
                PushDeltaHistory(
                    State->OvernightDeltaHistory,
                    static_cast<double>(OrderFlow.Delta),
                    RollingDeltaBars);
            }
        }

        if (ShowLines)
        {
            double SelectedCurrentHigh = 0.0;
            double SelectedCurrentLow = 0.0;
            if (GetSelectedCurrentHighLow(
                    *State,
                    HighLowBasis,
                    SelectedCurrentHigh,
                    SelectedCurrentLow))
            {
                CurrentDayHigh[BarIndex] =
                    static_cast<float>(SelectedCurrentHigh);
                CurrentDayLow[BarIndex] =
                    static_cast<float>(SelectedCurrentLow);
            }

            double SelectedPreviousHigh = 0.0;
            double SelectedPreviousLow = 0.0;
            if (GetSelectedPreviousHighLow(
                    *State,
                    HighLowBasis,
                    SelectedPreviousHigh,
                    SelectedPreviousLow))
            {
                PreviousDayHigh[BarIndex] =
                    static_cast<float>(SelectedPreviousHigh);
                PreviousDayLow[BarIndex] =
                    static_cast<float>(SelectedPreviousLow);
            }

            if (State->OvernightLevels.Valid)
            {
                OvernightVAH[BarIndex] = static_cast<float>(TicksToPrice(
                    State->OvernightLevels.ValueAreaHighTicks,
                    sc.TickSize));
                OvernightVPOC[BarIndex] = static_cast<float>(TicksToPrice(
                    State->OvernightLevels.PointOfControlTicks,
                    sc.TickSize));
                OvernightVAL[BarIndex] = static_cast<float>(TicksToPrice(
                    State->OvernightLevels.ValueAreaLowTicks,
                    sc.TickSize));
            }

            if (State->PreviousRTHLevels.Valid)
            {
                PreviousRTHVAH[BarIndex] = static_cast<float>(TicksToPrice(
                    State->PreviousRTHLevels.ValueAreaHighTicks,
                    sc.TickSize));
                PreviousRTHVPOC[BarIndex] = static_cast<float>(TicksToPrice(
                    State->PreviousRTHLevels.PointOfControlTicks,
                    sc.TickSize));
                PreviousRTHVAL[BarIndex] = static_cast<float>(TicksToPrice(
                    State->PreviousRTHLevels.ValueAreaLowTicks,
                    sc.TickSize));
            }

            const ProfileLevels& CurrentLevels =
                GetSelectedCurrentProfileLevels(*State, HVNBasis);
            if (CurrentLevels.Valid)
            {
                CurrentPrimaryHVN[BarIndex] = static_cast<float>(TicksToPrice(
                    CurrentLevels.PointOfControlTicks,
                    sc.TickSize));

                if (CurrentLevels.SecondaryHVN1Valid)
                {
                    CurrentSecondaryHVN1[BarIndex] =
                        static_cast<float>(TicksToPrice(
                            CurrentLevels.SecondaryHVN1Ticks,
                            sc.TickSize));
                }

                if (CurrentLevels.SecondaryHVN2Valid)
                {
                    CurrentSecondaryHVN2[BarIndex] =
                        static_cast<float>(TicksToPrice(
                            CurrentLevels.SecondaryHVN2Ticks,
                            sc.TickSize));
                }
            }
        }

        State->LastProcessedIndex = BarIndex;
    }

    // Extend the last completed context values into the currently forming bar.
    const int FormingBarIndex = LastClosedIndex + 1;
    if (FormingBarIndex < sc.ArraySize && LastClosedIndex >= 0)
    {
        for (int SubgraphIndex = 7; SubgraphIndex <= 19; ++SubgraphIndex)
        {
            sc.Subgraph[SubgraphIndex][FormingBarIndex] =
                sc.Subgraph[SubgraphIndex][LastClosedIndex];
        }

        BuyArrow[FormingBarIndex] = 0.0f;
        SellArrow[FormingBarIndex] = 0.0f;
        BuyScoreLabel[FormingBarIndex] = 0.0f;
        SellScoreLabel[FormingBarIndex] = 0.0f;
        BuyScoreLabel.Arrays[0][FormingBarIndex] = 0.0f;
        SellScoreLabel.Arrays[0][FormingBarIndex] = 0.0f;
        LongScore[FormingBarIndex] = 0.0f;
        ShortScore[FormingBarIndex] = 0.0f;
        SignalType[FormingBarIndex] = 0.0f;
    }

    sc.DataStartIndex = HistoricalStartIndex;
    sc.EarliestUpdateSubgraphDataArrayIndex = StartIndex;
}
