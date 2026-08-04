#include "sierrachart.h"

// Sierra Chart defines min/max as function-like macros in scstructures.h.
// Remove them before using the C++ standard-library overloads and
// std::numeric_limits<T>::max(). This must remain after sierrachart.h.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

SCDLLName("YM Adaptive Order Flow Signals")

/*
    YM Adaptive Order Flow Signals
    Version 1.1.0 - 2026-08-04

    Signal-only ACSIL study. No order-entry functions and no Market by Order dependency.
    Intended decision charts: YM 1 minute, 5 minutes, and 15 minutes.
    A 60-minute chart is supported as a broad regime view.
*/

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <vector>

namespace ymaofs
{

constexpr int kSessionStartSeconds = 9 * 3600 + 30 * 60; // 09:30 New York
constexpr int kSessionEndSeconds = 16 * 3600;              // 16:00 New York
constexpr int kOpeningRangeEndSeconds = 10 * 3600;         // 10:00 New York
constexpr int kSessionBuckets = 13;                         // 30-minute buckets
constexpr int kDepthLevels = 10;
constexpr int kMaxPrimarySignalsPerSession = 14;
constexpr int kMaxReversalSignalsPerSession = 8;
constexpr int kMaxContinuationSignalsPerSession = 10;
constexpr int kMaxEventMarkersPerTypePerSession = 5;
constexpr int kMaxEventMarkersPerSession = 16;
constexpr double kEpsilon = 1e-9;

// Visible signal subgraphs.
enum SubgraphIndex
{
    SG_REVERSAL_UP = 0,
    SG_REVERSAL_DOWN,
    SG_CONTINUATION_UP,
    SG_CONTINUATION_DOWN,

    // Diagnostic subgraphs. These default to hidden/ignored but remain available
    // in the Chart Values window and for spreadsheet/alert references.
    SG_UP_SCORE,
    SG_DOWN_SCORE,
    SG_NO_TRADE_SCORE,
    SG_REVERSAL_UP_SCORE,
    SG_REVERSAL_DOWN_SCORE,
    SG_CONTINUATION_UP_SCORE,
    SG_CONTINUATION_DOWN_SCORE,
    SG_BULL_ABSORPTION,
    SG_BEAR_ABSORPTION,
    SG_BULL_EXHAUSTION,
    SG_BEAR_EXHAUSTION,
    SG_SESSION_VWAP,
    SG_SESSION_POC,
    SG_SESSION_VAH,
    SG_SESSION_VAL,
    SG_TREND_15M,
    SG_TREND_60M,
    SG_REGIME,
    SG_DATA_QUALITY,
    SG_SIGNAL_CODE,

    // Visible context-event markers. These are independent of the final
    // buy/sell/continuation decision, so a developing event is visible even
    // when confirmation is not yet strong enough for a primary arrow.
    SG_BULL_ABSORPTION_MARKER,
    SG_BEAR_ABSORPTION_MARKER,
    SG_SELLER_EXHAUSTION_MARKER,
    SG_BUYER_EXHAUSTION_MARKER,
    SG_COUNT
};

enum class SignalClass : int
{
    ReversalUp = 0,
    ReversalDown = 1,
    ContinuationUp = 2,
    ContinuationDown = 3,
    NoTrade = 4,
    Count = 5
};

enum class EventClass : int
{
    BullAbsorption = 0,
    BearAbsorption = 1,
    SellerExhaustion = 2,
    BuyerExhaustion = 3,
    Count = 4
};

inline double Clamp(const double x, const double lo, const double hi)
{
    return std::max(lo, std::min(hi, x));
}

inline double SafeDivide(const double numerator, const double denominator, const double fallback = 0.0)
{
    return std::fabs(denominator) > kEpsilon ? numerator / denominator : fallback;
}

inline double Logistic(const double x)
{
    const double z = Clamp(x, -12.0, 12.0);
    return 1.0 / (1.0 + std::exp(-z));
}

inline double SmoothStep(const double x, const double center, const double width)
{
    return Logistic(SafeDivide(x - center, std::max(width, 1e-6)));
}

inline double GaussianNear(const double distance, const double scale)
{
    const double s = std::max(scale, 1e-6);
    const double z = distance / s;
    return std::exp(-0.5 * z * z);
}

inline int Sign(const double x)
{
    return (x > 0.0) - (x < 0.0);
}

inline bool IsFinite(const double x)
{
    return std::isfinite(x) != 0;
}

inline int PriceToTicks(const double price, const double tickSize)
{
    const double safeTick = std::max(tickSize, 1e-8);
    return static_cast<int>(std::llround(price / safeTick));
}

inline double TicksToPrice(const int priceTicks, const double tickSize)
{
    return static_cast<double>(priceTicks) * tickSize;
}

inline int BarsForMinutes(const int barSeconds, const int minutes)
{
    const int seconds = std::max(1, barSeconds);
    return std::max(2, static_cast<int>(std::ceil(minutes * 60.0 / seconds)));
}

struct EWStat
{
    double Mean = 0.0;
    double Variance = 1.0;
    std::uint64_t Count = 0;

    double StandardDeviation() const
    {
        return std::sqrt(std::max(Variance, 1e-8));
    }

    double ZScore(const double value) const
    {
        if (Count < 8)
            return 0.0;

        return Clamp((value - Mean) / StandardDeviation(), -4.5, 4.5);
    }

    void Update(const double value, const double alpha)
    {
        if (!IsFinite(value))
            return;

        const double a = Clamp(alpha, 0.002, 0.35);
        if (Count == 0)
        {
            Mean = value;
            const double seed = std::max(std::fabs(value) * 0.10, 1.0);
            Variance = seed * seed;
            Count = 1;
            return;
        }

        const double previousMean = Mean;
        Mean = (1.0 - a) * Mean + a * value;
        const double innovation = value - previousMean;
        Variance = std::max(1e-8, (1.0 - a) * Variance + a * innovation * (value - Mean));
        ++Count;
    }
};

struct ActivityBucket
{
    EWStat Volume;
    EWStat Range;
    EWStat AbsDelta;
    EWStat Trades;
};

struct AdaptiveActivity
{
    std::array<ActivityBucket, kSessionBuckets> Bucket;
    ActivityBucket Global;

    static double AdaptiveZ(const EWStat& local, const EWStat& global, const double value)
    {
        if (local.Count >= 10)
            return local.ZScore(value);
        if (global.Count >= 8)
            return global.ZScore(value);
        return 0.0;
    }

    double VolumeZ(const int bucket, const double value) const
    {
        return AdaptiveZ(Bucket[bucket].Volume, Global.Volume, value);
    }

    double RangeZ(const int bucket, const double value) const
    {
        return AdaptiveZ(Bucket[bucket].Range, Global.Range, value);
    }

    double AbsDeltaZ(const int bucket, const double value) const
    {
        return AdaptiveZ(Bucket[bucket].AbsDelta, Global.AbsDelta, value);
    }

    double TradesZ(const int bucket, const double value) const
    {
        return AdaptiveZ(Bucket[bucket].Trades, Global.Trades, value);
    }

    void Update(const int bucket, const double volume, const double range,
                const double absDelta, const double trades)
    {
        // A slow update protects the baseline from adapting immediately to a
        // short-lived news shock. The global baseline adapts slightly faster.
        constexpr double localAlpha = 0.025;
        constexpr double globalAlpha = 0.035;

        Bucket[bucket].Volume.Update(volume, localAlpha);
        Bucket[bucket].Range.Update(range, localAlpha);
        Bucket[bucket].AbsDelta.Update(absDelta, localAlpha);
        Bucket[bucket].Trades.Update(trades, localAlpha);

        Global.Volume.Update(volume, globalAlpha);
        Global.Range.Update(range, globalAlpha);
        Global.AbsDelta.Update(absDelta, globalAlpha);
        Global.Trades.Update(trades, globalAlpha);
    }
};

struct VAPPoint
{
    int PriceTicks = 0;
    double Volume = 0.0;
    double BidVolume = 0.0;
    double AskVolume = 0.0;
    double Trades = 0.0;
};

struct BarFootprint
{
    std::vector<VAPPoint> Levels;
    bool HasVAP = false;
    double TotalVolume = 0.0;
    double BidVolume = 0.0;
    double AskVolume = 0.0;
    double Trades = 0.0;
    double LowerThirdBid = 0.0;
    double LowerThirdAsk = 0.0;
    double UpperThirdBid = 0.0;
    double UpperThirdAsk = 0.0;
    int BarPOCTicks = 0;
    double BarPOCLocation = 0.5;
    int MaxStackedAskImbalances = 0;
    int MaxStackedBidImbalances = 0;

    double FractionVolumeAbove(const int levelTicks) const
    {
        if (TotalVolume <= 0.0)
            return 0.5;

        double volume = 0.0;
        for (const VAPPoint& point : Levels)
        {
            if (point.PriceTicks > levelTicks)
                volume += point.Volume;
            else if (point.PriceTicks == levelTicks)
                volume += 0.5 * point.Volume;
        }
        return Clamp(volume / TotalVolume, 0.0, 1.0);
    }

    double FractionVolumeBelow(const int levelTicks) const
    {
        return 1.0 - FractionVolumeAbove(levelTicks);
    }
};

struct ProfileNode
{
    double Volume = 0.0;
    double BidVolume = 0.0;
    double AskVolume = 0.0;
    double Trades = 0.0;
};

struct ProfileSnapshot
{
    bool Valid = false;
    int POCTicks = 0;
    int VAHTicks = 0;
    int VALTicks = 0;
    int NearestHVNTicks = 0;
    int NearestLVNTicks = 0;
    double HVNStrength = 0.0;
    double LVNStrength = 0.0;
    double VWAPTicks = 0.0;
    double TotalVolume = 0.0;
};

class SessionProfile
{
public:
    void Reset()
    {
        Levels.clear();
        TotalVolume = 0.0;
        PriceVolume = 0.0;
    }

    bool Empty() const
    {
        return TotalVolume <= 0.0 || Levels.empty();
    }

    void Add(const BarFootprint& footprint)
    {
        for (const VAPPoint& point : footprint.Levels)
        {
            if (point.Volume <= 0.0)
                continue;

            ProfileNode& node = Levels[point.PriceTicks];
            node.Volume += point.Volume;
            node.BidVolume += point.BidVolume;
            node.AskVolume += point.AskVolume;
            node.Trades += point.Trades;

            TotalVolume += point.Volume;
            PriceVolume += static_cast<double>(point.PriceTicks) * point.Volume;
        }
    }

    ProfileSnapshot Snapshot(const int referenceTicks) const
    {
        ProfileSnapshot snapshot;
        if (Empty())
            return snapshot;

        std::vector<int> ticks;
        std::vector<double> volume;
        ticks.reserve(Levels.size());
        volume.reserve(Levels.size());

        int pocIndex = 0;
        double pocVolume = -1.0;
        int index = 0;
        for (const auto& item : Levels)
        {
            ticks.push_back(item.first);
            volume.push_back(item.second.Volume);
            if (item.second.Volume > pocVolume)
            {
                pocVolume = item.second.Volume;
                pocIndex = index;
            }
            ++index;
        }

        // Value area: grow out from POC by selecting the larger adjacent volume
        // until 70% of session volume is enclosed.
        const double targetVolume = TotalVolume * 0.70;
        int lowIndex = pocIndex;
        int highIndex = pocIndex;
        double enclosedVolume = volume[pocIndex];
        while (enclosedVolume < targetVolume && (lowIndex > 0 || highIndex + 1 < static_cast<int>(volume.size())))
        {
            const double below = lowIndex > 0 ? volume[lowIndex - 1] : -1.0;
            const double above = highIndex + 1 < static_cast<int>(volume.size()) ? volume[highIndex + 1] : -1.0;
            if (above >= below)
            {
                ++highIndex;
                enclosedVolume += volume[highIndex];
            }
            else
            {
                --lowIndex;
                enclosedVolume += volume[lowIndex];
            }
        }

        // Three-price smoothing creates stable local profile nodes while still
        // respecting the one-tick YM auction structure.
        std::vector<double> smooth(volume.size(), 0.0);
        for (std::size_t i = 0; i < volume.size(); ++i)
        {
            const double left = i > 0 ? volume[i - 1] : volume[i];
            const double right = i + 1 < volume.size() ? volume[i + 1] : volume[i];
            smooth[i] = 0.25 * left + 0.50 * volume[i] + 0.25 * right;
        }

        std::vector<double> positiveSmooth;
        positiveSmooth.reserve(smooth.size());
        for (const double value : smooth)
        {
            if (value > 0.0)
                positiveSmooth.push_back(value);
        }

        double median = 1.0;
        if (!positiveSmooth.empty())
        {
            const std::size_t middle = positiveSmooth.size() / 2;
            std::nth_element(positiveSmooth.begin(), positiveSmooth.begin() + middle, positiveSmooth.end());
            median = std::max(1.0, positiveSmooth[middle]);
        }

        bool foundHVN = false;
        bool foundLVN = false;
        int nearestHVN = ticks[pocIndex];
        int nearestLVN = ticks[pocIndex];
        int nearestHVNDistance = std::numeric_limits<int>::max();
        int nearestLVNDistance = std::numeric_limits<int>::max();
        double hvnStrength = smooth[pocIndex] / median;
        double lvnStrength = 0.0;

        if (smooth.size() >= 3)
        {
            for (std::size_t i = 1; i + 1 < smooth.size(); ++i)
            {
                const int distance = std::abs(ticks[i] - referenceTicks);
                const bool isPeak = smooth[i] >= smooth[i - 1] && smooth[i] >= smooth[i + 1]
                    && smooth[i] >= 1.25 * median;
                const bool isTrough = smooth[i] <= smooth[i - 1] && smooth[i] <= smooth[i + 1]
                    && smooth[i] <= 0.78 * median
                    && std::max(smooth[i - 1], smooth[i + 1]) >= 0.95 * median;

                if (isPeak && distance < nearestHVNDistance)
                {
                    foundHVN = true;
                    nearestHVNDistance = distance;
                    nearestHVN = ticks[i];
                    hvnStrength = smooth[i] / median;
                }

                if (isTrough && distance < nearestLVNDistance)
                {
                    foundLVN = true;
                    nearestLVNDistance = distance;
                    nearestLVN = ticks[i];
                    lvnStrength = Clamp(1.0 - smooth[i] / median, 0.0, 1.0);
                }
            }
        }

        if (!foundHVN)
        {
            nearestHVN = ticks[pocIndex];
            hvnStrength = smooth[pocIndex] / median;
        }

        if (!foundLVN && smooth.size() >= 3)
        {
            double minimum = std::numeric_limits<double>::max();
            for (std::size_t i = 1; i + 1 < smooth.size(); ++i)
            {
                const double candidate = smooth[i] + 0.02 * median * std::abs(ticks[i] - referenceTicks);
                if (candidate < minimum)
                {
                    minimum = candidate;
                    nearestLVN = ticks[i];
                    lvnStrength = Clamp(1.0 - smooth[i] / median, 0.0, 1.0);
                }
            }
        }

        snapshot.Valid = true;
        snapshot.POCTicks = ticks[pocIndex];
        snapshot.VALTicks = ticks[lowIndex];
        snapshot.VAHTicks = ticks[highIndex];
        snapshot.NearestHVNTicks = nearestHVN;
        snapshot.NearestLVNTicks = nearestLVN;
        snapshot.HVNStrength = Clamp(hvnStrength / 3.0, 0.0, 1.0);
        snapshot.LVNStrength = Clamp(lvnStrength, 0.0, 1.0);
        snapshot.VWAPTicks = PriceVolume / std::max(TotalVolume, 1.0);
        snapshot.TotalVolume = TotalVolume;
        return snapshot;
    }

private:
    std::map<int, ProfileNode> Levels;
    double TotalVolume = 0.0;
    double PriceVolume = 0.0;
};

struct MicroBar
{
    double BuyVolume = 0.0;
    double SellVolume = 0.0;
    int BuyTrades = 0;
    int SellTrades = 0;
    int SweepsUp = 0;
    int SweepsDown = 0;
    double FirstTradePrice = 0.0;
    double LastTradePrice = 0.0;
    double HighTradePrice = -std::numeric_limits<double>::max();
    double LowTradePrice = std::numeric_limits<double>::max();
    int LastTradeSide = 0;
    int ConsecutiveSameSide = 0;
    int MaximumConsecutiveSameSide = 0;
    bool HasTrades = false;

    double TotalVolume() const
    {
        return BuyVolume + SellVolume;
    }

    double Flow() const
    {
        return SafeDivide(BuyVolume - SellVolume, TotalVolume());
    }
};

struct DepthBar
{
    double WeightedBid = 0.0;
    double WeightedAsk = 0.0;
    double BidAdded = 0.0;
    double AskAdded = 0.0;
    double BidPulled = 0.0;
    double AskPulled = 0.0;
    double BestBidReplenished = 0.0;
    double BestAskReplenished = 0.0;
    int Samples = 0;

    double QueueImbalance() const
    {
        return SafeDivide(WeightedBid - WeightedAsk, WeightedBid + WeightedAsk);
    }
};

struct DepthLevelState
{
    int PriceTicks = 0;
    double Quantity = 0.0;
    bool Valid = false;
};

struct WindowMetrics
{
    double SlopeNormalized = 0.0;
    double Efficiency = 0.0;
    double Position = 0.5;
    double NetChangeNormalized = 0.0;
};

struct ScoreSet
{
    std::array<double, static_cast<int>(SignalClass::Count)> Probability{};
    double ReversalUpRaw = 0.0;
    double ReversalDownRaw = 0.0;
    double ContinuationUpRaw = 0.0;
    double ContinuationDownRaw = 0.0;
    double NoTradeRaw = 0.0;

    // Exact model components used both for diagnostics and visible event
    // markers. Keeping these in ScoreSet avoids plotting simplified proxies
    // that can disagree with the actual reversal model.
    double BullAbsorption = 0.0;
    double BearAbsorption = 0.0;
    double BullExhaustion = 0.0;
    double BearExhaustion = 0.0;
};

struct EngineState
{
    int LastProcessedClosedBar = -1;
    std::uint32_t LastTimeAndSalesSequence = 0;
    int CurrentNYDate = 0;
    int SessionBars = 0;
    int SessionSignalCount = 0;
    int SessionReversalSignalCount = 0;
    int SessionContinuationSignalCount = 0;
    int SessionEventMarkerCount = 0;
    int LastSignalBar = -1000000;
    int LastReversalSignalBar = -1000000;
    int LastContinuationSignalBar = -1000000;
    int LastSignalDirection = 0;

    SessionProfile Profile;
    ProfileSnapshot PreviousSessionProfile;
    bool HasPreviousSessionProfile = false;
    double PreviousSessionHigh = 0.0;
    double PreviousSessionLow = 0.0;

    double SessionHigh = -std::numeric_limits<double>::max();
    double SessionLow = std::numeric_limits<double>::max();
    double OpeningRangeHigh = -std::numeric_limits<double>::max();
    double OpeningRangeLow = std::numeric_limits<double>::max();
    bool OpeningRangeComplete = false;

    AdaptiveActivity Activity;

    double ATR = 0.0;
    double PreviousClose = 0.0;
    double PreviousFlow1 = 0.0;
    double PreviousFlow2 = 0.0;
    double PreviousVolumeZ1 = 0.0;
    double PreviousVolumeZ2 = 0.0;
    double PreviousHigh1 = 0.0;
    double PreviousHigh2 = 0.0;
    double PreviousLow1 = 0.0;
    double PreviousLow2 = 0.0;
    int FeatureHistoryBars = 0;
    int LastSessionCloseTicks = 0;
    double PreviousSessionVWAPTicks = 0.0;
    int PreviousPOCTicks = 0;

    double ImpactNumerator = 0.0;
    double ImpactDenominator = 0.50;

    std::array<bool, static_cast<int>(SignalClass::Count)> Armed{{true, true, true, true, true}};
    std::array<bool, static_cast<int>(EventClass::Count)> EventArmed{{true, true, true, true}};
    std::array<int, static_cast<int>(EventClass::Count)> LastEventBar{{-1000000, -1000000, -1000000, -1000000}};
    std::array<int, static_cast<int>(EventClass::Count)> SessionEventCount{{0, 0, 0, 0}};

    std::map<int, MicroBar> MicroByBar;
    std::map<int, DepthBar> DepthByBar;
    std::array<DepthLevelState, kDepthLevels> PreviousBidDepth{};
    std::array<DepthLevelState, kDepthLevels> PreviousAskDepth{};

    void BeginNewSession(const int newDate)
    {
        if (!Profile.Empty())
        {
            PreviousSessionProfile = Profile.Snapshot(LastSessionCloseTicks);
            HasPreviousSessionProfile = PreviousSessionProfile.Valid;
            PreviousSessionHigh = SessionHigh;
            PreviousSessionLow = SessionLow;
        }

        CurrentNYDate = newDate;
        SessionBars = 0;
        SessionSignalCount = 0;
        SessionReversalSignalCount = 0;
        SessionContinuationSignalCount = 0;
        SessionEventMarkerCount = 0;
        LastSignalBar = -1000000;
        LastReversalSignalBar = -1000000;
        LastContinuationSignalBar = -1000000;
        Profile.Reset();
        SessionHigh = -std::numeric_limits<double>::max();
        SessionLow = std::numeric_limits<double>::max();
        OpeningRangeHigh = -std::numeric_limits<double>::max();
        OpeningRangeLow = std::numeric_limits<double>::max();
        OpeningRangeComplete = false;
        PreviousSessionVWAPTicks = 0.0;
        PreviousPOCTicks = 0;
        Armed = {{true, true, true, true, true}};
        EventArmed = {{true, true, true, true}};
        LastEventBar = {{-1000000, -1000000, -1000000, -1000000}};
        SessionEventCount = {{0, 0, 0, 0}};
        FeatureHistoryBars = 0;
        PreviousFlow1 = PreviousFlow2 = 0.0;
        PreviousVolumeZ1 = PreviousVolumeZ2 = 0.0;
        PreviousHigh1 = PreviousHigh2 = 0.0;
        PreviousLow1 = PreviousLow2 = 0.0;
        LastSessionCloseTicks = 0;
    }
};

struct BarContext
{
    int Index = 0;
    int NYDate = 0;
    int NYTime = 0;
    int Bucket = 0;
    bool InSession = false;

    double Open = 0.0;
    double High = 0.0;
    double Low = 0.0;
    double Close = 0.0;
    double Volume = 0.0;
    double BidVolume = 0.0;
    double AskVolume = 0.0;
    double Delta = 0.0;
    double Flow = 0.0;
    double Range = 0.0;
    double TrueRange = 0.0;
    double ATR = 1.0;
    double CLV = 0.0;
    double BodyFraction = 0.0;
    double LowerWickFraction = 0.0;
    double UpperWickFraction = 0.0;
    double ReturnNormalized = 0.0;
    double VolumeZ = 0.0;
    double RangeZ = 0.0;
    double AbsDeltaZ = 0.0;
    double TradesZ = 0.0;
    double ImpactExpected = 0.0;
    double ImpactResidual = 0.0;

    BarFootprint Footprint;
    MicroBar Micro;
    DepthBar Depth;
    ProfileSnapshot PreProfile;
    ProfileSnapshot PostProfile;
    WindowMetrics Trend15;
    WindowMetrics Trend60;
};

BarFootprint ExtractFootprint(SCStudyInterfaceRef sc, const int index, const double tickSize)
{
    BarFootprint footprint;

    const double barLow = sc.BaseDataIn[SC_LOW][index];
    const double barHigh = sc.BaseDataIn[SC_HIGH][index];
    const int lowTicks = PriceToTicks(barLow, tickSize);
    const int highTicks = PriceToTicks(barHigh, tickSize);
    const int tickRange = std::max(1, highTicks - lowTicks);
    const int lowerThird = lowTicks + tickRange / 3;
    const int upperThird = highTicks - tickRange / 3;

    if (sc.VolumeAtPriceForBars != nullptr)
    {
        const int count = sc.VolumeAtPriceForBars->GetSizeAtBarIndex(index);
        footprint.Levels.reserve(std::max(0, count));
        for (int levelIndex = 0; levelIndex < count; ++levelIndex)
        {
            const s_VolumeAtPriceV2* vap = nullptr;
            if (!sc.VolumeAtPriceForBars->GetVAPElementAtIndex(index, levelIndex, &vap) || vap == nullptr)
                continue;

            VAPPoint point;
            point.PriceTicks = vap->PriceInTicks;
            point.Volume = static_cast<double>(vap->Volume);
            point.BidVolume = static_cast<double>(vap->BidVolume);
            point.AskVolume = static_cast<double>(vap->AskVolume);
            point.Trades = static_cast<double>(vap->NumberOfTrades);
            footprint.Levels.push_back(point);
        }
    }

    if (footprint.Levels.empty())
    {
        // Graceful fallback when volume-at-price is not available. The profile
        // and signals still work from bar Bid/Ask volume, but data quality is lower.
        VAPPoint synthetic;
        synthetic.PriceTicks = PriceToTicks((sc.BaseDataIn[SC_HIGH][index]
            + sc.BaseDataIn[SC_LOW][index] + sc.BaseDataIn[SC_LAST][index]) / 3.0, tickSize);
        synthetic.Volume = sc.BaseDataIn[SC_VOLUME][index];
        synthetic.BidVolume = sc.BaseDataIn[SC_BIDVOL][index];
        synthetic.AskVolume = sc.BaseDataIn[SC_ASKVOL][index];
        synthetic.Trades = sc.BaseDataIn[SC_NUM_TRADES][index];
        footprint.Levels.push_back(synthetic);
        footprint.HasVAP = false;
    }
    else
    {
        footprint.HasVAP = true;
    }

    std::sort(footprint.Levels.begin(), footprint.Levels.end(),
        [](const VAPPoint& a, const VAPPoint& b) { return a.PriceTicks < b.PriceTicks; });

    double maximumVolume = -1.0;
    for (const VAPPoint& point : footprint.Levels)
    {
        footprint.TotalVolume += point.Volume;
        footprint.BidVolume += point.BidVolume;
        footprint.AskVolume += point.AskVolume;
        footprint.Trades += point.Trades;

        if (point.PriceTicks <= lowerThird)
        {
            footprint.LowerThirdBid += point.BidVolume;
            footprint.LowerThirdAsk += point.AskVolume;
        }
        if (point.PriceTicks >= upperThird)
        {
            footprint.UpperThirdBid += point.BidVolume;
            footprint.UpperThirdAsk += point.AskVolume;
        }

        if (point.Volume > maximumVolume)
        {
            maximumVolume = point.Volume;
            footprint.BarPOCTicks = point.PriceTicks;
        }
    }

    footprint.BarPOCLocation = Clamp(SafeDivide(footprint.BarPOCTicks - lowTicks,
        std::max(1, highTicks - lowTicks), 0.5), 0.0, 1.0);

    int askRun = 0;
    int bidRun = 0;
    for (std::size_t i = 0; i < footprint.Levels.size(); ++i)
    {
        // Diagonal imbalance: ask at current price versus bid one tick below,
        // and bid at current price versus ask one tick above.
        const double ask = footprint.Levels[i].AskVolume;
        const double bid = footprint.Levels[i].BidVolume;
        const double bidBelow = i > 0 ? footprint.Levels[i - 1].BidVolume : 0.0;
        const double askAbove = i + 1 < footprint.Levels.size() ? footprint.Levels[i + 1].AskVolume : 0.0;

        const bool askImbalance = ask >= 3.0 * std::max(1.0, bidBelow) && ask >= 4.0;
        const bool bidImbalance = bid >= 3.0 * std::max(1.0, askAbove) && bid >= 4.0;

        askRun = askImbalance ? askRun + 1 : 0;
        bidRun = bidImbalance ? bidRun + 1 : 0;
        footprint.MaxStackedAskImbalances = std::max(footprint.MaxStackedAskImbalances, askRun);
        footprint.MaxStackedBidImbalances = std::max(footprint.MaxStackedBidImbalances, bidRun);
    }

    return footprint;
}

WindowMetrics CalculateWindowMetrics(SCStudyInterfaceRef sc, const int index,
                                     const int bars, const double atr)
{
    WindowMetrics metrics;
    const int start = std::max(0, index - bars + 1);
    const int count = index - start + 1;
    if (count < 2)
        return metrics;

    double sumX = 0.0;
    double sumY = 0.0;
    double sumXX = 0.0;
    double sumXY = 0.0;
    double path = 0.0;
    double highest = -std::numeric_limits<double>::max();
    double lowest = std::numeric_limits<double>::max();

    for (int i = start; i <= index; ++i)
    {
        const double x = static_cast<double>(i - start);
        const double y = sc.BaseDataIn[SC_LAST][i];
        sumX += x;
        sumY += y;
        sumXX += x * x;
        sumXY += x * y;
        highest = std::max(highest, static_cast<double>(sc.BaseDataIn[SC_HIGH][i]));
        lowest = std::min(lowest, static_cast<double>(sc.BaseDataIn[SC_LOW][i]));
        if (i > start)
            path += std::fabs(y - sc.BaseDataIn[SC_LAST][i - 1]);
    }

    const double denominator = count * sumXX - sumX * sumX;
    const double slope = std::fabs(denominator) > kEpsilon
        ? (count * sumXY - sumX * sumY) / denominator
        : 0.0;

    const double first = sc.BaseDataIn[SC_LAST][start];
    const double last = sc.BaseDataIn[SC_LAST][index];
    const double net = last - first;
    const double safeATR = std::max(atr, sc.TickSize * 4.0);

    metrics.SlopeNormalized = Clamp(slope * (count - 1) / safeATR, -4.0, 4.0);
    metrics.Efficiency = Clamp(SafeDivide(std::fabs(net), path), 0.0, 1.0);
    metrics.Position = Clamp(SafeDivide(last - lowest, highest - lowest, 0.5), 0.0, 1.0);
    metrics.NetChangeNormalized = Clamp(net / safeATR, -5.0, 5.0);
    return metrics;
}

void ClearBarOutputs(SCStudyInterfaceRef sc, const int index)
{
    for (int subgraph = 0; subgraph < SG_COUNT; ++subgraph)
        sc.Subgraph[subgraph][index] = 0.0f;
}

void ProcessTimeAndSales(SCStudyInterfaceRef sc, EngineState& state)
{
    if (sc.IsFullRecalculation || sc.ArraySize <= 0)
        return;

    c_SCTimeAndSalesArray timeAndSales;
    sc.GetTimeAndSales(timeAndSales);
    if (timeAndSales.Size() == 0)
        return;

    const std::uint32_t newestSequence = timeAndSales[timeAndSales.Size() - 1].Sequence;
    if (newestSequence < state.LastTimeAndSalesSequence)
    {
        state.LastTimeAndSalesSequence = 0;
        state.MicroByBar.clear();
    }

    for (int i = 0; i < timeAndSales.Size(); ++i)
    {
        const s_TimeAndSales& record = timeAndSales[i];
        if (record.Sequence <= state.LastTimeAndSalesSequence)
            continue;

        state.LastTimeAndSalesSequence = record.Sequence;
        if (record.Type != SC_TS_BID && record.Type != SC_TS_ASK)
            continue;

        SCDateTime chartDateTime = sc.ConvertDateTimeUTCToChartTimeZone(record.DateTime);
        const SCDateTime nyDateTime = sc.ConvertDateTimeFromChartTimeZone(chartDateTime, TIMEZONE_NEW_YORK);
        const int nyTime = nyDateTime.GetTimeInSecondsWithoutMilliseconds();
        if (nyTime < kSessionStartSeconds || nyTime >= kSessionEndSeconds)
            continue;

        const int barIndex = sc.GetContainingIndexForSCDateTime(sc.ChartNumber, chartDateTime);
        if (barIndex < 0 || barIndex >= sc.ArraySize)
            continue;

        MicroBar& micro = state.MicroByBar[barIndex];
        const double price = static_cast<double>(record.Price) * sc.RealTimePriceMultiplier;
        const double volume = static_cast<double>(record.Volume);
        const int side = record.Type == SC_TS_ASK ? 1 : -1;

        if (!micro.HasTrades)
        {
            micro.FirstTradePrice = price;
            micro.LastTradePrice = price;
            micro.HighTradePrice = price;
            micro.LowTradePrice = price;
            micro.LastTradeSide = side;
            micro.ConsecutiveSameSide = 1;
            micro.MaximumConsecutiveSameSide = 1;
            micro.HasTrades = true;
        }
        else
        {
            if (side == micro.LastTradeSide)
                ++micro.ConsecutiveSameSide;
            else
                micro.ConsecutiveSameSide = 1;
            micro.MaximumConsecutiveSameSide = std::max(micro.MaximumConsecutiveSameSide,
                                                        micro.ConsecutiveSameSide);

            if (side > 0 && price > micro.LastTradePrice + 0.5 * sc.TickSize)
                ++micro.SweepsUp;
            if (side < 0 && price < micro.LastTradePrice - 0.5 * sc.TickSize)
                ++micro.SweepsDown;

            micro.LastTradeSide = side;
            micro.LastTradePrice = price;
            micro.HighTradePrice = std::max(micro.HighTradePrice, price);
            micro.LowTradePrice = std::min(micro.LowTradePrice, price);
        }

        if (side > 0)
        {
            micro.BuyVolume += volume;
            ++micro.BuyTrades;
        }
        else
        {
            micro.SellVolume += volume;
            ++micro.SellTrades;
        }
    }

    const int trimBefore = std::max(0, sc.ArraySize - 400);
    while (!state.MicroByBar.empty() && state.MicroByBar.begin()->first < trimBefore)
        state.MicroByBar.erase(state.MicroByBar.begin());
}

void CaptureDepth(SCStudyInterfaceRef sc, EngineState& state)
{
    if (sc.IsFullRecalculation || sc.ArraySize <= 0)
        return;

    const int barIndex = sc.ArraySize - 1;
    const SCDateTime nyDateTime = sc.ConvertDateTimeFromChartTimeZone(
        sc.BaseDateTimeIn[barIndex], TIMEZONE_NEW_YORK);
    const int nyTime = nyDateTime.GetTimeInSecondsWithoutMilliseconds();
    if (nyTime < kSessionStartSeconds || nyTime >= kSessionEndSeconds)
        return;

    DepthBar& aggregate = state.DepthByBar[barIndex];

    bool anyBid = false;
    bool anyAsk = false;
    std::array<DepthLevelState, kDepthLevels> currentBid{};
    std::array<DepthLevelState, kDepthLevels> currentAsk{};

    for (int level = 0; level < kDepthLevels; ++level)
    {
        const double weight = 1.0 / (1.0 + 0.55 * level);

        s_MarketDepthEntry bidEntry;
        if (sc.GetBidMarketDepthEntryAtLevel(bidEntry, level))
        {
            const double price = bidEntry.AdjustedPrice != 0.0f
                ? bidEntry.AdjustedPrice
                : static_cast<double>(bidEntry.Price) * sc.RealTimePriceMultiplier;
            const double quantity = static_cast<double>(bidEntry.Quantity);
            const int ticks = PriceToTicks(price, sc.TickSize);

            currentBid[level].PriceTicks = ticks;
            currentBid[level].Quantity = quantity;
            currentBid[level].Valid = true;
            aggregate.WeightedBid += weight * quantity;
            anyBid = true;

            const DepthLevelState& previous = state.PreviousBidDepth[level];
            if (previous.Valid && previous.PriceTicks == ticks)
            {
                const double change = quantity - previous.Quantity;
                if (change > 0.0)
                {
                    aggregate.BidAdded += weight * change;
                    if (level == 0)
                        aggregate.BestBidReplenished += change;
                }
                else
                {
                    aggregate.BidPulled += weight * (-change);
                }
            }
        }

        s_MarketDepthEntry askEntry;
        if (sc.GetAskMarketDepthEntryAtLevel(askEntry, level))
        {
            const double price = askEntry.AdjustedPrice != 0.0f
                ? askEntry.AdjustedPrice
                : static_cast<double>(askEntry.Price) * sc.RealTimePriceMultiplier;
            const double quantity = static_cast<double>(askEntry.Quantity);
            const int ticks = PriceToTicks(price, sc.TickSize);

            currentAsk[level].PriceTicks = ticks;
            currentAsk[level].Quantity = quantity;
            currentAsk[level].Valid = true;
            aggregate.WeightedAsk += weight * quantity;
            anyAsk = true;

            const DepthLevelState& previous = state.PreviousAskDepth[level];
            if (previous.Valid && previous.PriceTicks == ticks)
            {
                const double change = quantity - previous.Quantity;
                if (change > 0.0)
                {
                    aggregate.AskAdded += weight * change;
                    if (level == 0)
                        aggregate.BestAskReplenished += change;
                }
                else
                {
                    aggregate.AskPulled += weight * (-change);
                }
            }
        }
    }

    if (anyBid || anyAsk)
        ++aggregate.Samples;

    state.PreviousBidDepth = currentBid;
    state.PreviousAskDepth = currentAsk;

    const int trimBefore = std::max(0, sc.ArraySize - 400);
    while (!state.DepthByBar.empty() && state.DepthByBar.begin()->first < trimBefore)
        state.DepthByBar.erase(state.DepthByBar.begin());
}

BarContext BuildBarContext(SCStudyInterfaceRef sc, EngineState& state, const int index)
{
    BarContext context;
    context.Index = index;

    const SCDateTime nyDateTime = sc.ConvertDateTimeFromChartTimeZone(
        sc.BaseDateTimeIn[index], TIMEZONE_NEW_YORK);
    context.NYDate = nyDateTime.GetDate();
    context.NYTime = nyDateTime.GetTimeInSecondsWithoutMilliseconds();
    context.InSession = context.NYTime >= kSessionStartSeconds && context.NYTime < kSessionEndSeconds;
    context.Bucket = std::max(0, std::min(kSessionBuckets - 1,
        (context.NYTime - kSessionStartSeconds) / (30 * 60)));

    context.Open = sc.BaseDataIn[SC_OPEN][index];
    context.High = sc.BaseDataIn[SC_HIGH][index];
    context.Low = sc.BaseDataIn[SC_LOW][index];
    context.Close = sc.BaseDataIn[SC_LAST][index];
    context.Volume = std::max(0.0f, sc.BaseDataIn[SC_VOLUME][index]);
    context.BidVolume = std::max(0.0f, sc.BaseDataIn[SC_BIDVOL][index]);
    context.AskVolume = std::max(0.0f, sc.BaseDataIn[SC_ASKVOL][index]);
    context.Delta = context.AskVolume - context.BidVolume;
    context.Flow = SafeDivide(context.Delta, context.AskVolume + context.BidVolume);
    context.Range = std::max(static_cast<double>(sc.TickSize), context.High - context.Low);

    const double previousClose = index > 0 ? sc.BaseDataIn[SC_LAST][index - 1] : context.Open;
    context.TrueRange = std::max(context.Range,
        std::max(std::fabs(context.High - previousClose), std::fabs(context.Low - previousClose)));

    const double fallbackATR = std::max(context.TrueRange, sc.TickSize * 8.0);
    context.ATR = state.ATR > sc.TickSize ? state.ATR : fallbackATR;
    context.CLV = Clamp(SafeDivide(2.0 * context.Close - context.High - context.Low,
                                   context.Range), -1.0, 1.0);
    context.BodyFraction = Clamp(std::fabs(context.Close - context.Open) / context.Range, 0.0, 1.0);
    context.LowerWickFraction = Clamp((std::min(context.Open, context.Close) - context.Low) / context.Range, 0.0, 1.0);
    context.UpperWickFraction = Clamp((context.High - std::max(context.Open, context.Close)) / context.Range, 0.0, 1.0);
    context.ReturnNormalized = Clamp((context.Close - previousClose) / std::max(context.ATR, static_cast<double>(sc.TickSize)), -5.0, 5.0);

    if (!context.InSession)
        return context;

    context.Footprint = ExtractFootprint(sc, index, sc.TickSize);
    auto microIterator = state.MicroByBar.find(index);
    if (microIterator != state.MicroByBar.end())
        context.Micro = microIterator->second;
    auto depthIterator = state.DepthByBar.find(index);
    if (depthIterator != state.DepthByBar.end())
        context.Depth = depthIterator->second;

    // Signal flow deliberately remains bar-level Bid/Ask flow. Live Time and
    // Sales is collected only for the Data Quality diagnostic so a signal can
    // be reproduced after full recalculation without historical tape access.

    context.VolumeZ = state.Activity.VolumeZ(context.Bucket, context.Volume);
    context.RangeZ = state.Activity.RangeZ(context.Bucket, context.Range);
    context.AbsDeltaZ = state.Activity.AbsDeltaZ(context.Bucket, std::fabs(context.Delta));
    context.TradesZ = state.Activity.TradesZ(context.Bucket, context.Footprint.Trades);

    context.PreProfile = state.Profile.Snapshot(PriceToTicks(context.Close, sc.TickSize));

    const double beta = Clamp(state.ImpactNumerator / std::max(state.ImpactDenominator, 0.05), -3.0, 3.0);
    context.ImpactExpected = beta * context.Flow;
    context.ImpactResidual = Clamp(context.ReturnNormalized - context.ImpactExpected, -5.0, 5.0);

    const int barSeconds = std::max(1, sc.SecondsPerBar);
    context.Trend15 = CalculateWindowMetrics(sc, index, BarsForMinutes(barSeconds, 15), context.ATR);
    context.Trend60 = CalculateWindowMetrics(sc, index, BarsForMinutes(barSeconds, 60), context.ATR);

    return context;
}

double LevelNearness(const double close, const int levelTicks, const double tickSize, const double atr,
                     const double widthATR = 0.35)
{
    const double level = TicksToPrice(levelTicks, tickSize);
    return GaussianNear(std::fabs(close - level), std::max(tickSize * 3.0, widthATR * atr));
}

double DirectionalLevelContext(const BarContext& c, const EngineState& state,
                               const double tickSize, const bool bullish)
{
    const double close = c.Close;
    const double atr = c.ATR;
    double score = 0.0;
    double weight = 0.0;
    double best = 0.0;

    auto addLevel = [&](const double levelPrice, const double levelWeight, const bool lowerLevel)
    {
        if (!IsFinite(levelPrice) || levelPrice <= 0.0)
            return;
        const bool directionallyRelevant = bullish ? lowerLevel : !lowerLevel;
        if (!directionallyRelevant)
            return;
        const double nearness = GaussianNear(std::fabs(close - levelPrice),
            std::max(3.0 * tickSize, 0.42 * atr));
        score += levelWeight * nearness;
        weight += levelWeight;
        best = std::max(best, Clamp(levelWeight / 1.15, 0.35, 1.0) * nearness);
    };

    if (c.PreProfile.Valid)
    {
        const double poc = TicksToPrice(c.PreProfile.POCTicks, tickSize);
        const double val = TicksToPrice(c.PreProfile.VALTicks, tickSize);
        const double vah = TicksToPrice(c.PreProfile.VAHTicks, tickSize);
        const double hvn = TicksToPrice(c.PreProfile.NearestHVNTicks, tickSize);
        const double lvn = TicksToPrice(c.PreProfile.NearestLVNTicks, tickSize);

        addLevel(val, 1.15, val <= poc);
        addLevel(vah, 1.15, vah < poc);
        addLevel(hvn, 0.65 + 0.35 * c.PreProfile.HVNStrength, hvn <= poc);
        addLevel(lvn, 0.55 + 0.45 * c.PreProfile.LVNStrength, lvn <= poc);
        addLevel(poc, 0.45, close <= poc);
    }

    if (state.OpeningRangeLow < std::numeric_limits<double>::max())
        addLevel(state.OpeningRangeLow, 0.95, true);
    if (state.OpeningRangeHigh > -std::numeric_limits<double>::max())
        addLevel(state.OpeningRangeHigh, 0.95, false);

    if (state.HasPreviousSessionProfile)
    {
        addLevel(TicksToPrice(state.PreviousSessionProfile.VALTicks, tickSize), 0.90, true);
        addLevel(TicksToPrice(state.PreviousSessionProfile.VAHTicks, tickSize), 0.90, false);
        addLevel(TicksToPrice(state.PreviousSessionProfile.POCTicks, tickSize), 0.55,
                 close <= TicksToPrice(state.PreviousSessionProfile.POCTicks, tickSize));
        addLevel(state.PreviousSessionLow, 0.90, true);
        addLevel(state.PreviousSessionHigh, 0.90, false);
    }

    if (weight <= 0.0)
    {
        if (state.SessionLow < std::numeric_limits<double>::max()
            && state.SessionHigh > -std::numeric_limits<double>::max())
        {
            const double location = Clamp(SafeDivide(close - state.SessionLow,
                state.SessionHigh - state.SessionLow, 0.5), 0.0, 1.0);
            return bullish ? 1.0 - location : location;
        }
        return 0.35;
    }

    const double average = Clamp(score / weight, 0.0, 1.0);
    return Clamp(0.70 * best + 0.30 * average, 0.0, 1.0);
}

ScoreSet CalculateScores(const BarContext& c, const EngineState& state, const double tickSize)
{
    ScoreSet scores;

    const double buyPressure = SmoothStep(c.Flow, 0.08, 0.15);
    const double sellPressure = SmoothStep(-c.Flow, 0.08, 0.15);
    const double activeEffort = Clamp(0.40 * SmoothStep(c.VolumeZ, 0.10, 0.85)
        + 0.32 * SmoothStep(c.AbsDeltaZ, 0.10, 0.85)
        + 0.18 * SmoothStep(c.TradesZ, 0.00, 0.95)
        + 0.10 * SmoothStep(c.RangeZ, -0.10, 1.10), 0.0, 1.0);

    const double recoveryUp = Clamp(0.42 * (0.5 + 0.5 * c.CLV)
        + 0.33 * c.LowerWickFraction
        + 0.25 * SmoothStep(c.ImpactResidual, 0.05, 0.45), 0.0, 1.0);
    const double recoveryDown = Clamp(0.42 * (0.5 - 0.5 * c.CLV)
        + 0.33 * c.UpperWickFraction
        + 0.25 * SmoothStep(-c.ImpactResidual, 0.05, 0.45), 0.0, 1.0);

    const double lowerSellConcentration = SafeDivide(c.Footprint.LowerThirdBid,
        c.Footprint.LowerThirdBid + c.Footprint.LowerThirdAsk, 0.5);
    const double upperBuyConcentration = SafeDivide(c.Footprint.UpperThirdAsk,
        c.Footprint.UpperThirdBid + c.Footprint.UpperThirdAsk, 0.5);

    const double stackedBid = SmoothStep(static_cast<double>(c.Footprint.MaxStackedBidImbalances), 1.5, 0.8);
    const double stackedAsk = SmoothStep(static_cast<double>(c.Footprint.MaxStackedAskImbalances), 1.5, 0.8);

    double bullAbsorption = sellPressure * Clamp(
        0.24 * activeEffort
        + 0.25 * recoveryUp
        + 0.20 * SmoothStep(c.ImpactResidual, 0.10, 0.45)
        + 0.16 * SmoothStep(lowerSellConcentration, 0.56, 0.10)
        + 0.08 * stackedBid
        + 0.07 * recoveryUp,
        0.0, 1.0);

    double bearAbsorption = buyPressure * Clamp(
        0.24 * activeEffort
        + 0.25 * recoveryDown
        + 0.20 * SmoothStep(-c.ImpactResidual, 0.10, 0.45)
        + 0.16 * SmoothStep(upperBuyConcentration, 0.56, 0.10)
        + 0.08 * stackedAsk
        + 0.07 * recoveryDown,
        0.0, 1.0);

    // Repeated-test dry-up and terminal-volume-climax models.
    const bool hasFeatureHistory = state.FeatureHistoryBars >= 2;
    const bool lowerLow = hasFeatureHistory
        && c.Low < std::min(state.PreviousLow1, state.PreviousLow2) - 0.25 * tickSize;
    const bool higherHigh = hasFeatureHistory
        && c.High > std::max(state.PreviousHigh1, state.PreviousHigh2) + 0.25 * tickSize;
    const double sellFlowImprovement = SmoothStep(c.Flow - std::min(state.PreviousFlow1, state.PreviousFlow2),
                                                  0.04, 0.10);
    const double buyFlowDeterioration = SmoothStep(std::max(state.PreviousFlow1, state.PreviousFlow2) - c.Flow,
                                                   0.04, 0.10);
    const double volumeDryUp = SmoothStep(std::max(state.PreviousVolumeZ1, state.PreviousVolumeZ2) - c.VolumeZ,
                                          0.15, 0.65);

    const double bullDryExhaustion = (lowerLow ? 1.0 : 0.0)
        * Clamp(0.40 * sellFlowImprovement + 0.28 * volumeDryUp + 0.32 * recoveryUp, 0.0, 1.0);
    const double bearDryExhaustion = (higherHigh ? 1.0 : 0.0)
        * Clamp(0.40 * buyFlowDeterioration + 0.28 * volumeDryUp + 0.32 * recoveryDown, 0.0, 1.0);

    const double bullClimax = Clamp(
        SmoothStep(c.VolumeZ, 1.15, 0.65)
        * SmoothStep(c.AbsDeltaZ, 0.95, 0.70)
        * sellPressure
        * Clamp(0.55 * recoveryUp + 0.45 * SmoothStep(c.RangeZ, 0.45, 0.90), 0.0, 1.0),
        0.0, 1.0);
    const double bearClimax = Clamp(
        SmoothStep(c.VolumeZ, 1.15, 0.65)
        * SmoothStep(c.AbsDeltaZ, 0.95, 0.70)
        * buyPressure
        * Clamp(0.55 * recoveryDown + 0.45 * SmoothStep(c.RangeZ, 0.45, 0.90), 0.0, 1.0),
        0.0, 1.0);

    const double bullExhaustion = std::max(bullDryExhaustion, bullClimax);
    const double bearExhaustion = std::max(bearDryExhaustion, bearClimax);

    scores.BullAbsorption = bullAbsorption;
    scores.BearAbsorption = bearAbsorption;
    scores.BullExhaustion = bullExhaustion;
    scores.BearExhaustion = bearExhaustion;

    const double lowerContext = DirectionalLevelContext(c, state, tickSize, true);
    const double upperContext = DirectionalLevelContext(c, state, tickSize, false);

    const double trend15Up = Clamp(0.48 * SmoothStep(c.Trend15.SlopeNormalized, 0.12, 0.48)
        + 0.27 * c.Trend15.Efficiency
        + 0.25 * SmoothStep(c.Trend15.Position, 0.56, 0.16), 0.0, 1.0);
    const double trend15Down = Clamp(0.48 * SmoothStep(-c.Trend15.SlopeNormalized, 0.12, 0.48)
        + 0.27 * c.Trend15.Efficiency
        + 0.25 * SmoothStep(1.0 - c.Trend15.Position, 0.56, 0.16), 0.0, 1.0);
    const double trend60Up = Clamp(0.52 * SmoothStep(c.Trend60.SlopeNormalized, 0.12, 0.55)
        + 0.25 * c.Trend60.Efficiency
        + 0.23 * SmoothStep(c.Trend60.Position, 0.55, 0.18), 0.0, 1.0);
    const double trend60Down = Clamp(0.52 * SmoothStep(-c.Trend60.SlopeNormalized, 0.12, 0.55)
        + 0.25 * c.Trend60.Efficiency
        + 0.23 * SmoothStep(1.0 - c.Trend60.Position, 0.55, 0.18), 0.0, 1.0);

    // Acceptance beyond the nearest meaningful auction boundary.
    int resistanceTicks = 0;
    int supportTicks = 0;
    bool hasResistance = false;
    bool hasSupport = false;
    const int closeTicks = PriceToTicks(c.Close, tickSize);

    const int maximumAcceptanceDistanceTicks = std::max(4,
        static_cast<int>(std::ceil(1.25 * c.ATR / std::max(tickSize, 1e-8))));
    auto considerResistance = [&](const int ticks)
    {
        // A resistance boundary becomes an acceptance candidate after price has
        // crossed it and is holding no more than roughly 1.25 ATR beyond it.
        if (ticks > closeTicks || closeTicks - ticks > maximumAcceptanceDistanceTicks)
            return;
        if (!hasResistance || ticks > resistanceTicks)
        {
            resistanceTicks = ticks;
            hasResistance = true;
        }
    };
    auto considerSupport = [&](const int ticks)
    {
        // Symmetric acceptance below a previously supporting boundary.
        if (ticks < closeTicks || ticks - closeTicks > maximumAcceptanceDistanceTicks)
            return;
        if (!hasSupport || ticks < supportTicks)
        {
            supportTicks = ticks;
            hasSupport = true;
        }
    };

    if (c.PreProfile.Valid)
    {
        considerResistance(c.PreProfile.VAHTicks);
        considerResistance(c.PreProfile.NearestLVNTicks);
        considerSupport(c.PreProfile.VALTicks);
        considerSupport(c.PreProfile.NearestLVNTicks);
    }
    if (state.OpeningRangeHigh > -std::numeric_limits<double>::max())
        considerResistance(PriceToTicks(state.OpeningRangeHigh, tickSize));
    if (state.OpeningRangeLow < std::numeric_limits<double>::max())
        considerSupport(PriceToTicks(state.OpeningRangeLow, tickSize));
    if (state.HasPreviousSessionProfile)
    {
        considerResistance(state.PreviousSessionProfile.VAHTicks);
        considerSupport(state.PreviousSessionProfile.VALTicks);
    }

    double acceptanceUp = 0.0;
    double acceptanceDown = 0.0;
    if (hasResistance)
    {
        const double level = TicksToPrice(resistanceTicks, tickSize);
        const double through = (c.Close - level) / std::max(c.ATR, tickSize);
        acceptanceUp = Clamp(0.42 * SmoothStep(through, 0.04, 0.12)
            + 0.30 * SmoothStep(c.Footprint.FractionVolumeAbove(resistanceTicks), 0.56, 0.10)
            + 0.16 * buyPressure
            + 0.12 * stackedAsk, 0.0, 1.0);
    }
    else if (state.SessionHigh > -std::numeric_limits<double>::max())
    {
        acceptanceUp = Clamp(0.58 * SmoothStep((c.Close - state.SessionHigh) / std::max(c.ATR, tickSize), 0.02, 0.12)
            + 0.24 * buyPressure + 0.18 * stackedAsk, 0.0, 1.0);
    }

    if (hasSupport)
    {
        const double level = TicksToPrice(supportTicks, tickSize);
        const double through = (level - c.Close) / std::max(c.ATR, tickSize);
        acceptanceDown = Clamp(0.42 * SmoothStep(through, 0.04, 0.12)
            + 0.30 * SmoothStep(c.Footprint.FractionVolumeBelow(supportTicks), 0.56, 0.10)
            + 0.16 * sellPressure
            + 0.12 * stackedBid, 0.0, 1.0);
    }
    else if (state.SessionLow < std::numeric_limits<double>::max())
    {
        acceptanceDown = Clamp(0.58 * SmoothStep((state.SessionLow - c.Close) / std::max(c.ATR, tickSize), 0.02, 0.12)
            + 0.24 * sellPressure + 0.18 * stackedBid, 0.0, 1.0);
    }

    const double pocMigrationUp = c.PostProfile.Valid && c.PreProfile.Valid
        ? SmoothStep(static_cast<double>(c.PostProfile.POCTicks - c.PreProfile.POCTicks), 0.5, 1.4)
        : 0.5;
    const double pocMigrationDown = c.PostProfile.Valid && c.PreProfile.Valid
        ? SmoothStep(static_cast<double>(c.PreProfile.POCTicks - c.PostProfile.POCTicks), 0.5, 1.4)
        : 0.5;

    const double resultUp = Clamp(0.55 * SmoothStep(c.ReturnNormalized, 0.06, 0.32)
        + 0.45 * SmoothStep(c.ImpactResidual, 0.03, 0.38), 0.0, 1.0);
    const double resultDown = Clamp(0.55 * SmoothStep(-c.ReturnNormalized, 0.06, 0.32)
        + 0.45 * SmoothStep(-c.ImpactResidual, 0.03, 0.38), 0.0, 1.0);

    const double divergenceUp = (lowerLow ? 1.0 : 0.0)
        * Clamp(0.55 * sellFlowImprovement + 0.45 * SmoothStep(c.CLV, -0.10, 0.32), 0.0, 1.0);
    const double divergenceDown = (higherHigh ? 1.0 : 0.0)
        * Clamp(0.55 * buyFlowDeterioration + 0.45 * SmoothStep(-c.CLV, -0.10, 0.32), 0.0, 1.0);

    scores.ReversalUpRaw = Clamp(
        0.32 * bullAbsorption
        + 0.22 * bullExhaustion
        + 0.19 * lowerContext
        + 0.12 * divergenceUp
        + 0.10 * recoveryUp
        + 0.05 * (1.0 - trend60Down), 0.0, 1.0);

    scores.ReversalDownRaw = Clamp(
        0.32 * bearAbsorption
        + 0.22 * bearExhaustion
        + 0.19 * upperContext
        + 0.12 * divergenceDown
        + 0.10 * recoveryDown
        + 0.05 * (1.0 - trend60Up), 0.0, 1.0);

    const double pullbackContinuationUp = trend60Up * trend15Up
        * Clamp(0.58 * bullAbsorption + 0.42 * lowerContext, 0.0, 1.0);
    const double pullbackContinuationDown = trend60Down * trend15Down
        * Clamp(0.58 * bearAbsorption + 0.42 * upperContext, 0.0, 1.0);

    double directContinuationUp =
        0.22 * trend15Up
        + 0.18 * trend60Up
        + 0.16 * buyPressure
        + 0.21 * resultUp
        + 0.13 * acceptanceUp
        + 0.06 * pocMigrationUp
        + 0.04 * stackedAsk;
    directContinuationUp *= Clamp(1.0 - 0.34 * bearAbsorption - 0.16 * bearExhaustion, 0.45, 1.0);

    double directContinuationDown =
        0.22 * trend15Down
        + 0.18 * trend60Down
        + 0.16 * sellPressure
        + 0.21 * resultDown
        + 0.13 * acceptanceDown
        + 0.06 * pocMigrationDown
        + 0.04 * stackedBid;
    directContinuationDown *= Clamp(1.0 - 0.34 * bullAbsorption - 0.16 * bullExhaustion, 0.45, 1.0);

    scores.ContinuationUpRaw = Clamp(std::max(
        directContinuationUp,
        0.72 * pullbackContinuationUp + 0.18 * trend60Up + 0.10 * resultUp), 0.0, 1.0);

    scores.ContinuationDownRaw = Clamp(std::max(
        directContinuationDown,
        0.72 * pullbackContinuationDown + 0.18 * trend60Down + 0.10 * resultDown), 0.0, 1.0);

    const double directionalMaximum = std::max(
        std::max(scores.ReversalUpRaw, scores.ReversalDownRaw),
        std::max(scores.ContinuationUpRaw, scores.ContinuationDownRaw));
    const double directionalConflict = std::min(
        std::max(scores.ReversalUpRaw, scores.ContinuationUpRaw),
        std::max(scores.ReversalDownRaw, scores.ContinuationDownRaw));
    const double lowActivity = Clamp(0.55 * SmoothStep(-c.VolumeZ, 0.75, 0.75)
        + 0.45 * SmoothStep(-c.AbsDeltaZ, 0.80, 0.85), 0.0, 1.0);
    const double rotational = Clamp((1.0 - c.Trend15.Efficiency)
        * (1.0 - std::fabs(2.0 * c.Trend15.Position - 1.0)), 0.0, 1.0);
    scores.NoTradeRaw = Clamp(0.31 * lowActivity
        + 0.24 * rotational
        + 0.25 * directionalConflict
        + 0.20 * (1.0 - directionalMaximum), 0.0, 1.0);

    // Interpretable five-class softmax. These outputs are model scores, not
    // empirically calibrated probabilities until independently walk-forward tested.
    std::array<double, static_cast<int>(SignalClass::Count)> logits{};
    logits[static_cast<int>(SignalClass::ReversalUp)] = -1.35 + 4.05 * scores.ReversalUpRaw
        - 0.70 * scores.ContinuationDownRaw - 0.35 * scores.ReversalDownRaw;
    logits[static_cast<int>(SignalClass::ReversalDown)] = -1.35 + 4.05 * scores.ReversalDownRaw
        - 0.70 * scores.ContinuationUpRaw - 0.35 * scores.ReversalUpRaw;
    logits[static_cast<int>(SignalClass::ContinuationUp)] = -1.25 + 3.85 * scores.ContinuationUpRaw
        - 0.55 * scores.ReversalDownRaw - 0.45 * scores.ContinuationDownRaw;
    logits[static_cast<int>(SignalClass::ContinuationDown)] = -1.25 + 3.85 * scores.ContinuationDownRaw
        - 0.55 * scores.ReversalUpRaw - 0.45 * scores.ContinuationUpRaw;
    logits[static_cast<int>(SignalClass::NoTrade)] = -0.55 + 2.40 * scores.NoTradeRaw
        + 0.30 * (1.0 - directionalMaximum);

    const double maximumLogit = *std::max_element(logits.begin(), logits.end());
    double denominator = 0.0;
    for (double& logit : logits)
    {
        logit = std::exp(logit - maximumLogit);
        denominator += logit;
    }
    for (int i = 0; i < static_cast<int>(SignalClass::Count); ++i)
        scores.Probability[i] = logits[i] / std::max(denominator, kEpsilon);

    return scores;
}

void PlotDiagnostics(SCStudyInterfaceRef sc, const BarContext& c, const ScoreSet& scores)
{
    const int index = c.Index;
    const double upScore = 100.0 * (scores.Probability[static_cast<int>(SignalClass::ReversalUp)]
        + scores.Probability[static_cast<int>(SignalClass::ContinuationUp)]);
    const double downScore = 100.0 * (scores.Probability[static_cast<int>(SignalClass::ReversalDown)]
        + scores.Probability[static_cast<int>(SignalClass::ContinuationDown)]);

    sc.Subgraph[SG_UP_SCORE][index] = static_cast<float>(upScore);
    sc.Subgraph[SG_DOWN_SCORE][index] = static_cast<float>(downScore);
    sc.Subgraph[SG_NO_TRADE_SCORE][index] = static_cast<float>(100.0 * scores.Probability[static_cast<int>(SignalClass::NoTrade)]);
    sc.Subgraph[SG_REVERSAL_UP_SCORE][index] = static_cast<float>(100.0 * scores.Probability[static_cast<int>(SignalClass::ReversalUp)]);
    sc.Subgraph[SG_REVERSAL_DOWN_SCORE][index] = static_cast<float>(100.0 * scores.Probability[static_cast<int>(SignalClass::ReversalDown)]);
    sc.Subgraph[SG_CONTINUATION_UP_SCORE][index] = static_cast<float>(100.0 * scores.Probability[static_cast<int>(SignalClass::ContinuationUp)]);
    sc.Subgraph[SG_CONTINUATION_DOWN_SCORE][index] = static_cast<float>(100.0 * scores.Probability[static_cast<int>(SignalClass::ContinuationDown)]);
    sc.Subgraph[SG_BULL_ABSORPTION][index] = static_cast<float>(100.0 * scores.BullAbsorption);
    sc.Subgraph[SG_BEAR_ABSORPTION][index] = static_cast<float>(100.0 * scores.BearAbsorption);
    sc.Subgraph[SG_BULL_EXHAUSTION][index] = static_cast<float>(100.0 * scores.BullExhaustion);
    sc.Subgraph[SG_BEAR_EXHAUSTION][index] = static_cast<float>(100.0 * scores.BearExhaustion);

    if (c.PostProfile.Valid)
    {
        sc.Subgraph[SG_SESSION_VWAP][index] = static_cast<float>(TicksToPrice(
            static_cast<int>(std::llround(c.PostProfile.VWAPTicks)), sc.TickSize));
        sc.Subgraph[SG_SESSION_POC][index] = static_cast<float>(TicksToPrice(c.PostProfile.POCTicks, sc.TickSize));
        sc.Subgraph[SG_SESSION_VAH][index] = static_cast<float>(TicksToPrice(c.PostProfile.VAHTicks, sc.TickSize));
        sc.Subgraph[SG_SESSION_VAL][index] = static_cast<float>(TicksToPrice(c.PostProfile.VALTicks, sc.TickSize));
    }

    sc.Subgraph[SG_TREND_15M][index] = static_cast<float>(100.0 * Clamp(0.5 + 0.5 * c.Trend15.SlopeNormalized, 0.0, 1.0));
    sc.Subgraph[SG_TREND_60M][index] = static_cast<float>(100.0 * Clamp(0.5 + 0.5 * c.Trend60.SlopeNormalized, 0.0, 1.0));

    int regime = 0;
    if (c.Trend60.SlopeNormalized > 0.55 && c.Trend60.Efficiency > 0.35)
        regime = 2;
    else if (c.Trend60.SlopeNormalized > 0.16)
        regime = 1;
    else if (c.Trend60.SlopeNormalized < -0.55 && c.Trend60.Efficiency > 0.35)
        regime = -2;
    else if (c.Trend60.SlopeNormalized < -0.16)
        regime = -1;
    sc.Subgraph[SG_REGIME][index] = static_cast<float>(regime);

    double dataQuality = 35.0;
    if (c.Footprint.HasVAP)
        dataQuality += 30.0;
    if (c.Micro.TotalVolume() >= std::max(20.0, 0.15 * c.Volume))
        dataQuality += 20.0;
    if (c.Depth.Samples > 0)
        dataQuality += 15.0;
    sc.Subgraph[SG_DATA_QUALITY][index] = static_cast<float>(std::min(100.0, dataQuality));
}

void UpdateAdaptiveState(EngineState& state, const BarContext& c, const double tickSize)
{
    state.Activity.Update(c.Bucket, c.Volume, c.Range, std::fabs(c.Delta), c.Footprint.Trades);

    constexpr double atrAlpha = 2.0 / 21.0;
    if (state.ATR <= 0.0)
        state.ATR = c.TrueRange;
    else
        state.ATR = (1.0 - atrAlpha) * state.ATR + atrAlpha * c.TrueRange;

    constexpr double impactAlpha = 0.035;
    state.ImpactNumerator = (1.0 - impactAlpha) * state.ImpactNumerator
        + impactAlpha * c.Flow * c.ReturnNormalized;
    state.ImpactDenominator = (1.0 - impactAlpha) * state.ImpactDenominator
        + impactAlpha * c.Flow * c.Flow;

    state.PreviousFlow2 = state.PreviousFlow1;
    state.PreviousFlow1 = c.Flow;
    state.PreviousVolumeZ2 = state.PreviousVolumeZ1;
    state.PreviousVolumeZ1 = c.VolumeZ;
    state.PreviousHigh2 = state.PreviousHigh1;
    state.PreviousHigh1 = c.High;
    state.PreviousLow2 = state.PreviousLow1;
    state.PreviousLow1 = c.Low;
    state.PreviousClose = c.Close;
    state.LastSessionCloseTicks = PriceToTicks(c.Close, tickSize);
    ++state.FeatureHistoryBars;
    if (c.PostProfile.Valid)
    {
        state.PreviousSessionVWAPTicks = c.PostProfile.VWAPTicks;
        state.PreviousPOCTicks = c.PostProfile.POCTicks;
    }
}

int CooldownBars(const int barSeconds, const double minutes)
{
    return std::max(1, static_cast<int>(std::ceil(minutes * 60.0 / std::max(1, barSeconds))));
}

int RequiredBaselineBars(const int barSeconds)
{
    if (barSeconds <= 60)
        return 12;
    if (barSeconds <= 300)
        return 8;
    if (barSeconds <= 900)
        return 8;
    return 6;
}

int RequiredSessionWarmBars(const int barSeconds)
{
    if (barSeconds <= 60)
        return 4;
    if (barSeconds <= 300)
        return 3;
    return 2;
}

bool WarmEnough(const EngineState& state, const int barSeconds, const bool eventMarker)
{
    const int baselineBars = RequiredBaselineBars(barSeconds);
    const int sessionBars = std::max(1, RequiredSessionWarmBars(barSeconds) - (eventMarker ? 1 : 0));
    return state.Activity.Global.Volume.Count >= static_cast<std::uint64_t>(baselineBars)
        && state.SessionBars >= sessionBars;
}

void UpdateDirectionalArming(EngineState& state, const ScoreSet& scores)
{
    const int ru = static_cast<int>(SignalClass::ReversalUp);
    const int rd = static_cast<int>(SignalClass::ReversalDown);
    const int cu = static_cast<int>(SignalClass::ContinuationUp);
    const int cd = static_cast<int>(SignalClass::ContinuationDown);

    if (scores.ReversalUpRaw < 0.39 || scores.Probability[ru] < 0.20)
        state.Armed[ru] = true;
    if (scores.ReversalDownRaw < 0.39 || scores.Probability[rd] < 0.20)
        state.Armed[rd] = true;
    if (scores.ContinuationUpRaw < 0.47 || scores.Probability[cu] < 0.20)
        state.Armed[cu] = true;
    if (scores.ContinuationDownRaw < 0.47 || scores.Probability[cd] < 0.20)
        state.Armed[cd] = true;
}

bool SelectReversalSignal(const EngineState& state, const BarContext& c,
                          const ScoreSet& scores, const int barSeconds,
                          SignalClass& selectedClass)
{
    const int up = static_cast<int>(SignalClass::ReversalUp);
    const int down = static_cast<int>(SignalClass::ReversalDown);
    const double upEvidence = 0.70 * scores.ReversalUpRaw + 0.30 * scores.Probability[up];
    const double downEvidence = 0.70 * scores.ReversalDownRaw + 0.30 * scores.Probability[down];
    const bool selectUp = upEvidence >= downEvidence;
    const int best = selectUp ? up : down;
    const int opposite = selectUp ? down : up;
    selectedClass = selectUp ? SignalClass::ReversalUp : SignalClass::ReversalDown;

    const double bestRaw = selectUp ? scores.ReversalUpRaw : scores.ReversalDownRaw;
    const double oppositeRaw = selectUp ? scores.ReversalDownRaw : scores.ReversalUpRaw;
    const double eventStrength = selectUp
        ? std::max(scores.BullAbsorption, scores.BullExhaustion)
        : std::max(scores.BearAbsorption, scores.BearExhaustion);
    const double oppositeContinuation = selectUp
        ? scores.Probability[static_cast<int>(SignalClass::ContinuationDown)]
        : scores.Probability[static_cast<int>(SignalClass::ContinuationUp)];

    double rawThreshold = 0.48;
    double probabilityThreshold = 0.27;
    double eventThreshold = 0.48;
    if (barSeconds > 60 && barSeconds <= 300)
    {
        rawThreshold = 0.46;
        probabilityThreshold = 0.25;
        eventThreshold = 0.46;
    }
    else if (barSeconds > 300 && barSeconds <= 900)
    {
        rawThreshold = 0.44;
        probabilityThreshold = 0.23;
        eventThreshold = 0.43;
    }
    else if (barSeconds > 900)
    {
        rawThreshold = 0.42;
        probabilityThreshold = 0.22;
        eventThreshold = 0.41;
    }

    const bool strongOpposingTrend = selectUp
        ? (c.Trend60.SlopeNormalized < -0.70 && c.Trend60.Efficiency > 0.42)
        : (c.Trend60.SlopeNormalized > 0.70 && c.Trend60.Efficiency > 0.42);
    if (strongOpposingTrend)
    {
        rawThreshold += 0.05;
        eventThreshold += 0.03;
    }
    if (eventStrength >= 0.70)
    {
        rawThreshold -= 0.025;
        probabilityThreshold -= 0.015;
    }

    const double bestProbability = scores.Probability[best];
    const double oppositeProbability = scores.Probability[opposite];
    const double noTradeProbability = scores.Probability[static_cast<int>(SignalClass::NoTrade)];
    const bool dominance = bestProbability >= oppositeProbability + 0.025
        || bestRaw >= oppositeRaw + 0.055;
    const bool notOverrunByOppositeContinuation = oppositeContinuation <= bestProbability + 0.12
        || bestRaw >= 0.63 || eventStrength >= 0.70;
    const bool noTradeAcceptable = noTradeProbability <= 0.50
        || bestRaw >= 0.58 || eventStrength >= 0.64;
    const bool confidence = bestRaw >= rawThreshold
        && bestProbability >= probabilityThreshold
        && eventStrength >= eventThreshold
        && dominance && notOverrunByOppositeContinuation && noTradeAcceptable;
    const bool capacity = state.SessionSignalCount < kMaxPrimarySignalsPerSession
        && state.SessionReversalSignalCount < kMaxReversalSignalsPerSession;

    return WarmEnough(state, barSeconds, false) && confidence && capacity && state.Armed[best];
}

bool SelectContinuationSignal(const EngineState& state, const ScoreSet& scores,
                              const int barSeconds, SignalClass& selectedClass)
{
    const int up = static_cast<int>(SignalClass::ContinuationUp);
    const int down = static_cast<int>(SignalClass::ContinuationDown);
    const double upEvidence = 0.72 * scores.ContinuationUpRaw + 0.28 * scores.Probability[up];
    const double downEvidence = 0.72 * scores.ContinuationDownRaw + 0.28 * scores.Probability[down];
    const bool selectUp = upEvidence >= downEvidence;
    const int best = selectUp ? up : down;
    const int opposite = selectUp ? down : up;
    selectedClass = selectUp ? SignalClass::ContinuationUp : SignalClass::ContinuationDown;

    const double bestRaw = selectUp ? scores.ContinuationUpRaw : scores.ContinuationDownRaw;
    const double oppositeRaw = selectUp ? scores.ContinuationDownRaw : scores.ContinuationUpRaw;
    double rawThreshold = 0.56;
    double probabilityThreshold = 0.38;
    if (barSeconds > 60 && barSeconds <= 300)
    {
        rawThreshold = 0.55;
        probabilityThreshold = 0.36;
    }
    else if (barSeconds > 300 && barSeconds <= 900)
    {
        rawThreshold = 0.53;
        probabilityThreshold = 0.34;
    }
    else if (barSeconds > 900)
    {
        rawThreshold = 0.52;
        probabilityThreshold = 0.32;
    }

    const double bestProbability = scores.Probability[best];
    const double oppositeProbability = scores.Probability[opposite];
    const double noTradeProbability = scores.Probability[static_cast<int>(SignalClass::NoTrade)];
    const double opposingReversal = selectUp
        ? scores.Probability[static_cast<int>(SignalClass::ReversalDown)]
        : scores.Probability[static_cast<int>(SignalClass::ReversalUp)];
    const bool dominance = bestProbability >= oppositeProbability + 0.040
        || bestRaw >= oppositeRaw + 0.060;
    const bool notOpposed = opposingReversal <= bestProbability + 0.10 || bestRaw >= 0.65;
    const bool confidence = bestRaw >= rawThreshold
        && bestProbability >= probabilityThreshold
        && noTradeProbability <= 0.46 && dominance && notOpposed;
    const bool capacity = state.SessionSignalCount < kMaxPrimarySignalsPerSession
        && state.SessionContinuationSignalCount < kMaxContinuationSignalsPerSession;

    return WarmEnough(state, barSeconds, false) && confidence && capacity && state.Armed[best];
}

void PlotEventMarkers(SCStudyInterfaceRef sc, EngineState& state, const BarContext& c,
                      const ScoreSet& scores, const int barSeconds)
{
    const std::array<double, static_cast<int>(EventClass::Count)> strength{{
        scores.BullAbsorption,
        scores.BearAbsorption,
        scores.BullExhaustion,
        scores.BearExhaustion
    }};

    double absorptionThreshold = 0.62;
    double exhaustionThreshold = 0.58;
    if (barSeconds > 60 && barSeconds <= 300)
    {
        absorptionThreshold = 0.59;
        exhaustionThreshold = 0.54;
    }
    else if (barSeconds > 300 && barSeconds <= 900)
    {
        absorptionThreshold = 0.56;
        exhaustionThreshold = 0.50;
    }
    else if (barSeconds > 900)
    {
        absorptionThreshold = 0.54;
        exhaustionThreshold = 0.47;
    }

    for (int eventIndex = 0; eventIndex < static_cast<int>(EventClass::Count); ++eventIndex)
    {
        const bool absorption = eventIndex <= static_cast<int>(EventClass::BearAbsorption);
        const double threshold = absorption ? absorptionThreshold : exhaustionThreshold;
        const double rearmThreshold = absorption ? threshold - 0.22 : threshold - 0.24;
        if (strength[eventIndex] <= rearmThreshold)
            state.EventArmed[eventIndex] = true;
    }

    if (!WarmEnough(state, barSeconds, true))
        return;

    const double absorptionOffset = std::max(2.0 * sc.TickSize, 0.09 * c.ATR);
    const double exhaustionOffset = std::max(3.0 * sc.TickSize, 0.27 * c.ATR);

    auto emitEvent = [&](const EventClass eventClass, const double threshold,
                         const double cooldownMinutes, const int subgraph,
                         const double plotValue)
    {
        const int eventIndex = static_cast<int>(eventClass);
        const bool cooldownComplete = c.Index - state.LastEventBar[eventIndex]
            >= CooldownBars(barSeconds, cooldownMinutes);
        const bool capacity = state.SessionEventCount[eventIndex] < kMaxEventMarkersPerTypePerSession
            && state.SessionEventMarkerCount < kMaxEventMarkersPerSession;
        if (state.EventArmed[eventIndex] && strength[eventIndex] >= threshold
            && cooldownComplete && capacity)
        {
            sc.Subgraph[subgraph][c.Index] = static_cast<float>(plotValue);
            state.EventArmed[eventIndex] = false;
            state.LastEventBar[eventIndex] = c.Index;
            ++state.SessionEventCount[eventIndex];
            ++state.SessionEventMarkerCount;
        }
    };

    emitEvent(EventClass::BullAbsorption, absorptionThreshold, 7.0,
              SG_BULL_ABSORPTION_MARKER, c.Low - absorptionOffset);
    emitEvent(EventClass::BearAbsorption, absorptionThreshold, 7.0,
              SG_BEAR_ABSORPTION_MARKER, c.High + absorptionOffset);
    emitEvent(EventClass::SellerExhaustion, exhaustionThreshold, 10.0,
              SG_SELLER_EXHAUSTION_MARKER, c.Low - exhaustionOffset);
    emitEvent(EventClass::BuyerExhaustion, exhaustionThreshold, 10.0,
              SG_BUYER_EXHAUSTION_MARKER, c.High + exhaustionOffset);
}

void ProcessClosedBar(SCStudyInterfaceRef sc, EngineState& state, const int index)
{
    ClearBarOutputs(sc, index);

    // Reset session state before BuildBarContext snapshots the developing
    // profile. This keeps the first RTH bar from inheriting the prior session's
    // developing profile during a full recalculation.
    const SCDateTime nyDateTime = sc.ConvertDateTimeFromChartTimeZone(
        sc.BaseDateTimeIn[index], TIMEZONE_NEW_YORK);
    const int nyTime = nyDateTime.GetTimeInSecondsWithoutMilliseconds();
    const bool inNewYorkSession = nyTime >= kSessionStartSeconds && nyTime < kSessionEndSeconds;
    if (inNewYorkSession && state.CurrentNYDate != nyDateTime.GetDate())
        state.BeginNewSession(nyDateTime.GetDate());

    BarContext c = BuildBarContext(sc, state, index);
    if (!c.InSession)
    {
        // ATR and impact adaptation continue outside RTH, while the time-of-day
        // activity baseline and signal/profile engines remain NY-session only.
        constexpr double atrAlpha = 2.0 / 21.0;
        if (state.ATR <= 0.0)
            state.ATR = c.TrueRange;
        else
            state.ATR = (1.0 - atrAlpha) * state.ATR + atrAlpha * c.TrueRange;
        state.PreviousClose = c.Close;
        return;
    }

    // The pre-profile excludes this bar. Add the bar only after all features
    // that represent prior auction structure have been captured.
    state.Profile.Add(c.Footprint);
    c.PostProfile = state.Profile.Snapshot(PriceToTicks(c.Close, sc.TickSize));

    const double openingRangeHighBefore = state.OpeningRangeHigh;
    const double openingRangeLowBefore = state.OpeningRangeLow;
    (void)openingRangeHighBefore;
    (void)openingRangeLowBefore;

    ScoreSet scores = CalculateScores(c, state, sc.TickSize);

    PlotDiagnostics(sc, c, scores);

    const int barSeconds = std::max(1, sc.SecondsPerBar);
    PlotEventMarkers(sc, state, c, scores, barSeconds);
    UpdateDirectionalArming(state, scores);

    SignalClass selectedClass = SignalClass::NoTrade;
    const bool primarySpacingComplete = index - state.LastSignalBar
        >= CooldownBars(barSeconds, 3.0);
    const bool reversalCooldownComplete = index - state.LastReversalSignalBar
        >= CooldownBars(barSeconds, 7.0);
    const bool continuationCooldownComplete = index - state.LastContinuationSignalBar
        >= CooldownBars(barSeconds, 12.0);

    bool emitPrimarySignal = false;
    if (primarySpacingComplete && reversalCooldownComplete
        && SelectReversalSignal(state, c, scores, barSeconds, selectedClass))
    {
        emitPrimarySignal = true;
    }
    else if (primarySpacingComplete && continuationCooldownComplete
        && SelectContinuationSignal(state, scores, barSeconds, selectedClass))
    {
        emitPrimarySignal = true;
    }

    if (emitPrimarySignal)
    {
        const int classIndex = static_cast<int>(selectedClass);
        int signalDirection = 0;
        int signalCode = 0;
        double arrowOffset = std::max(2.0 * sc.TickSize, 0.18 * c.ATR);

        switch (selectedClass)
        {
            case SignalClass::ReversalUp:
                arrowOffset = std::max(4.0 * sc.TickSize, 0.42 * c.ATR);
                sc.Subgraph[SG_REVERSAL_UP][index] = static_cast<float>(c.Low - arrowOffset);
                signalDirection = 1;
                signalCode = 1;
                state.LastReversalSignalBar = index;
                ++state.SessionReversalSignalCount;
                break;
            case SignalClass::ReversalDown:
                arrowOffset = std::max(4.0 * sc.TickSize, 0.42 * c.ATR);
                sc.Subgraph[SG_REVERSAL_DOWN][index] = static_cast<float>(c.High + arrowOffset);
                signalDirection = -1;
                signalCode = -1;
                state.LastReversalSignalBar = index;
                ++state.SessionReversalSignalCount;
                break;
            case SignalClass::ContinuationUp:
                sc.Subgraph[SG_CONTINUATION_UP][index] = static_cast<float>(c.Low - arrowOffset);
                signalDirection = 1;
                signalCode = 2;
                state.LastContinuationSignalBar = index;
                ++state.SessionContinuationSignalCount;
                break;
            case SignalClass::ContinuationDown:
                sc.Subgraph[SG_CONTINUATION_DOWN][index] = static_cast<float>(c.High + arrowOffset);
                signalDirection = -1;
                signalCode = -2;
                state.LastContinuationSignalBar = index;
                ++state.SessionContinuationSignalCount;
                break;
            default:
                break;
        }

        if (signalCode != 0)
        {
            sc.Subgraph[SG_SIGNAL_CODE][index] = static_cast<float>(signalCode);
            state.LastSignalBar = index;
            state.LastSignalDirection = signalDirection;
            ++state.SessionSignalCount;
            state.Armed[classIndex] = false;
        }
    }

    if (c.NYTime < kOpeningRangeEndSeconds)
    {
        state.OpeningRangeHigh = std::max(state.OpeningRangeHigh, c.High);
        state.OpeningRangeLow = std::min(state.OpeningRangeLow, c.Low);
    }
    else
    {
        state.OpeningRangeComplete = true;
    }

    state.SessionHigh = std::max(state.SessionHigh, c.High);
    state.SessionLow = std::min(state.SessionLow, c.Low);
    ++state.SessionBars;

    UpdateAdaptiveState(state, c, sc.TickSize);

    state.MicroByBar.erase(index);
    state.DepthByBar.erase(index);
}

void ConfigureSubgraph(SCStudyInterfaceRef sc, const int index, const char* name,
                       const int drawStyle, const COLORREF color, const int lineWidth,
                       const bool drawZeros)
{
    sc.Subgraph[index].Name = name;
    sc.Subgraph[index].DrawStyle = drawStyle;
    sc.Subgraph[index].PrimaryColor = color;
    sc.Subgraph[index].LineWidth = lineWidth;
    sc.Subgraph[index].DrawZeros = drawZeros ? 1 : 0;
}

} // namespace ymaofs

SCSFExport scsf_YMAdaptiveOrderFlowSignals(SCStudyInterfaceRef sc)
{
    using namespace ymaofs;

    if (sc.SetDefaults)
    {
        sc.GraphName = "YM Adaptive Order Flow Signals v1.1 - NY Session";
        sc.StudyDescription =
            "Parameter-free, signal-only YM study. Displays independent absorption and exhaustion markers, "
            "buy/sell reversal arrows, and buy/sell trend-continuation arrows. It scores VSA-style effort/result, "
            "volume-at-price profile structure, HVN/LVN context, and internal 15-minute/60-minute trend. "
            "It uses actual 09:30-16:00 New York time and does not require Market by Order. Live Time and "
            "Sales and regular depth are monitored only for data-quality diagnostics, so emitted arrows "
            "remain reproducible after full recalculation.";

        sc.AutoLoop = 0;
        sc.GraphRegion = 0;
        sc.UpdateAlways = 1;
        sc.FreeDLL = 0;
        sc.MaintainVolumeAtPriceData = 1;
        sc.UsesMarketDepthData = 1;
        sc.CalculationPrecedence = LOW_PREC_LEVEL;
        sc.DisplayStudyInputValues = 0;
        sc.IncludeInStudySummary = 1;

        ConfigureSubgraph(sc, SG_REVERSAL_UP, "Buy Reversal", DRAWSTYLE_ARROW_UP, RGB(0, 185, 70), 4, false);
        ConfigureSubgraph(sc, SG_REVERSAL_DOWN, "Sell Reversal", DRAWSTYLE_ARROW_DOWN, RGB(225, 45, 45), 4, false);
        ConfigureSubgraph(sc, SG_CONTINUATION_UP, "Buy Continuation", DRAWSTYLE_ARROW_UP, RGB(35, 125, 235), 3, false);
        ConfigureSubgraph(sc, SG_CONTINUATION_DOWN, "Sell Continuation", DRAWSTYLE_ARROW_DOWN, RGB(235, 145, 25), 3, false);

        ConfigureSubgraph(sc, SG_UP_SCORE, "Up Score", DRAWSTYLE_IGNORE, RGB(0, 180, 70), 1, false);
        ConfigureSubgraph(sc, SG_DOWN_SCORE, "Down Score", DRAWSTYLE_IGNORE, RGB(220, 45, 45), 1, false);
        ConfigureSubgraph(sc, SG_NO_TRADE_SCORE, "No Trade Score", DRAWSTYLE_IGNORE, RGB(130, 130, 130), 1, false);
        ConfigureSubgraph(sc, SG_REVERSAL_UP_SCORE, "Reversal Up Score", DRAWSTYLE_IGNORE, RGB(0, 180, 70), 1, false);
        ConfigureSubgraph(sc, SG_REVERSAL_DOWN_SCORE, "Reversal Down Score", DRAWSTYLE_IGNORE, RGB(220, 45, 45), 1, false);
        ConfigureSubgraph(sc, SG_CONTINUATION_UP_SCORE, "Continuation Up Score", DRAWSTYLE_IGNORE, RGB(35, 125, 235), 1, false);
        ConfigureSubgraph(sc, SG_CONTINUATION_DOWN_SCORE, "Continuation Down Score", DRAWSTYLE_IGNORE, RGB(235, 145, 25), 1, false);
        ConfigureSubgraph(sc, SG_BULL_ABSORPTION, "Bullish Absorption Score", DRAWSTYLE_IGNORE, RGB(0, 160, 90), 1, false);
        ConfigureSubgraph(sc, SG_BEAR_ABSORPTION, "Bearish Absorption Score", DRAWSTYLE_IGNORE, RGB(200, 55, 55), 1, false);
        ConfigureSubgraph(sc, SG_BULL_EXHAUSTION, "Seller Exhaustion Score", DRAWSTYLE_IGNORE, RGB(0, 145, 115), 1, false);
        ConfigureSubgraph(sc, SG_BEAR_EXHAUSTION, "Buyer Exhaustion Score", DRAWSTYLE_IGNORE, RGB(185, 75, 45), 1, false);
        ConfigureSubgraph(sc, SG_SESSION_VWAP, "Session VWAP", DRAWSTYLE_IGNORE, RGB(120, 120, 220), 1, false);
        ConfigureSubgraph(sc, SG_SESSION_POC, "Session POC", DRAWSTYLE_IGNORE, RGB(170, 90, 190), 1, false);
        ConfigureSubgraph(sc, SG_SESSION_VAH, "Session VAH", DRAWSTYLE_IGNORE, RGB(150, 100, 175), 1, false);
        ConfigureSubgraph(sc, SG_SESSION_VAL, "Session VAL", DRAWSTYLE_IGNORE, RGB(150, 100, 175), 1, false);
        ConfigureSubgraph(sc, SG_TREND_15M, "Internal 15m Trend", DRAWSTYLE_IGNORE, RGB(80, 135, 210), 1, false);
        ConfigureSubgraph(sc, SG_TREND_60M, "Internal 60m Trend", DRAWSTYLE_IGNORE, RGB(55, 95, 170), 1, false);
        ConfigureSubgraph(sc, SG_REGIME, "Regime (-2 to +2)", DRAWSTYLE_IGNORE, RGB(110, 110, 110), 1, false);
        ConfigureSubgraph(sc, SG_DATA_QUALITY, "Data Quality", DRAWSTYLE_IGNORE, RGB(80, 160, 160), 1, false);
        ConfigureSubgraph(sc, SG_SIGNAL_CODE, "Signal Code", DRAWSTYLE_IGNORE, RGB(90, 90, 90), 1, false);

        ConfigureSubgraph(sc, SG_BULL_ABSORPTION_MARKER, "Bullish Absorption - Diamond Below", DRAWSTYLE_DIAMOND, RGB(0, 205, 145), 4, false);
        ConfigureSubgraph(sc, SG_BEAR_ABSORPTION_MARKER, "Bearish Absorption - Diamond Above", DRAWSTYLE_DIAMOND, RGB(215, 70, 190), 4, false);
        ConfigureSubgraph(sc, SG_SELLER_EXHAUSTION_MARKER, "Seller Exhaustion - Triangle Below", DRAWSTYLE_TRIANGLE_UP, RGB(0, 175, 200), 4, false);
        ConfigureSubgraph(sc, SG_BUYER_EXHAUSTION_MARKER, "Buyer Exhaustion - Triangle Above", DRAWSTYLE_TRIANGLE_DOWN, RGB(220, 105, 35), 4, false);
        return;
    }

    void*& persistentPointer = sc.GetPersistentPointer(1);
    EngineState* state = static_cast<EngineState*>(persistentPointer);

    if (sc.LastCallToFunction)
    {
        delete state;
        persistentPointer = nullptr;
        return;
    }

    if (state == nullptr)
    {
        state = new EngineState();
        persistentPointer = state;
    }

    if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0)
    {
        *state = EngineState();
    }

    // Live Time and Sales and regular depth are optional data-quality inputs.
    // The v1 signal classifier itself uses reproducible bar Bid/Ask volume plus
    // Volume at Price, so arrows do not depend on historical tape/depth access.
    ProcessTimeAndSales(sc, *state);
    CaptureDepth(sc, *state);

    if (sc.ArraySize > 0)
        ClearBarOutputs(sc, sc.ArraySize - 1);

    const int lastClosedBar = sc.ArraySize - 2;
    if (lastClosedBar < 0)
        return;

    int firstBar = state->LastProcessedClosedBar + 1;
    if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0)
        firstBar = 0;

    for (int index = firstBar; index <= lastClosedBar; ++index)
    {
        // All indices through ArraySize-2 are complete. This closed-bar rule
        // keeps signals stable and prevents intrabar repainting.
        ProcessClosedBar(sc, *state, index);
        state->LastProcessedClosedBar = index;
    }
}
