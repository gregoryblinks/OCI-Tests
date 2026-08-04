#include "sierrachart.h"

// Sierra Chart headers define min/max macros. Remove them before using the
// standard C++ library and std::numeric_limits.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

/*
    Sierra Chart MBO Absorption & Exhaustion Signals
    Package date: 2026-08-04

    Purpose
    -------
    Uses live Market-by-Order snapshots, aggregate market depth, and Time and
    Sales executions to identify:
      1. Probable bid/ask absorption.
      2. Aggressor exhaustion after absorption (potential reversal).
      3. Passive absorber exhaustion after absorption (potential continuation).

    It can also read the SCMBOD1 .scmbo files written by the separate
    "MBO Snapshot Recorder" study from the MBO DOM Intelligence package.

    Important limitations
    ---------------------
    - These are probabilistic microstructure classifications, not proof of
      hidden orders, spoofing intent, or future price direction.
    - Sierra Chart aggregate depth and MBO are separate data streams and are
      not guaranteed to be synchronized to the same exchange update.
    - The MBO order array must not be assumed to be price-time queue order.
      This study therefore does not attribute executions by returned array
      position. It correlates executions at a price with repeated quantity and
      Order-ID behavior instead.
    - Recorded MBO replay is sampled. Orders that appear and disappear between
      samples can be missed.
*/

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

SCDLLName("Sierra Chart MBO Absorption Exhaustion Signals")

namespace MboAE
{
    constexpr int SIDE_ASK = -1; // Resting ask / aggressive buy trade.
    constexpr int SIDE_BID = 1;  // Resting bid / aggressive sell trade.
    constexpr double SECONDS_PER_DAY = 86400.0;
    constexpr uint32_t FILE_VERSION = 1;
    constexpr uint32_t FRAME_MARKER = 0x314D5246U; // "FRM1" little-endian.

    enum DataSourceMode
    {
        DATA_SOURCE_AUTO = 0,
        DATA_SOURCE_LIVE = 1,
        DATA_SOURCE_RECORDED = 2
    };

    enum ReplayClockMode
    {
        REPLAY_CLOCK_LATEST_CHART_RECORD = 0,
        REPLAY_CLOCK_REPLAY_TIMER = 1
    };

    enum RefillKind
    {
        REFILL_NONE = 0,
        REFILL_SAME_ID = 1,
        REFILL_PERSISTENT = 2,
        REFILL_SYNTHETIC = 3,
        REFILL_AGGREGATE = 4
    };

    enum LevelPhase
    {
        PHASE_IDLE = 0,
        PHASE_CANDIDATE = 1,
        PHASE_ABSORBING = 2,
        PHASE_AGGRESSOR_EXHAUSTED = 3,
        PHASE_ABSORBER_WEAKENING = 4
    };

    enum SignalType
    {
        SIGNAL_NONE = 0,
        SIGNAL_BID_ABSORPTION = 1,
        SIGNAL_ASK_ABSORPTION = 2,
        SIGNAL_SELLER_EXHAUSTION = 3,
        SIGNAL_BUYER_EXHAUSTION = 4,
        SIGNAL_BID_ABSORBER_EXHAUSTED = 5,
        SIGNAL_ASK_ABSORBER_EXHAUSTED = 6
    };

    struct LevelKey
    {
        int Side = 0;
        int PriceInTicks = 0;

        bool operator<(const LevelKey& Other) const
        {
            if (Side != Other.Side)
                return Side < Other.Side;
            return PriceInTicks < Other.PriceInTicks;
        }

        bool operator==(const LevelKey& Other) const
        {
            return Side == Other.Side && PriceInTicks == Other.PriceInTicks;
        }
    };

    struct OrderKey
    {
        int Side = 0;
        int PriceInTicks = 0;
        uint64_t OrderID = 0;

        bool operator<(const OrderKey& Other) const
        {
            if (Side != Other.Side)
                return Side < Other.Side;
            if (PriceInTicks != Other.PriceInTicks)
                return PriceInTicks < Other.PriceInTicks;
            return OrderID < Other.OrderID;
        }
    };

    struct BookOrder
    {
        OrderKey Key;
        uint64_t Quantity = 0;
        int ReturnedIndex = 0;
    };

    struct BookLevel
    {
        LevelKey Key;
        uint64_t AggregateQuantity = 0;
        uint32_t AggregateOrderCount = 0;
        std::vector<BookOrder> Orders;
    };

    struct TradeEvent
    {
        double DateTime = 0.0;
        uint64_t Sequence = 0;
        int Side = 0;
        int PriceInTicks = 0;
        uint64_t Volume = 0;
    };

    struct BookFrame
    {
        double DateTime = 0.0;
        int BestBidInTicks = 0;
        int BestAskInTicks = 0;
        int ReferencePriceInTicks = 0;
        uint64_t BookHash = 0;
        std::vector<BookLevel> Levels;
        std::vector<TradeEvent> Trades;
    };

#pragma pack(push, 1)
    struct DiskFileHeader
    {
        char Magic[8];
        uint32_t Version;
        uint32_t HeaderSize;
        double TickSize;
        uint32_t MinimumRecordedOrderQuantity;
        uint32_t MaximumDepthLevels;
        char Symbol[64];
        char Reserved[160];
    };

    struct DiskFrameHeader
    {
        uint32_t Marker;
        uint32_t PayloadBytes;
        double DateTime;
        int32_t BestBidInTicks;
        int32_t BestAskInTicks;
        uint32_t LevelCount;
        uint32_t OrderCount;
        uint32_t TradeCount;
        uint64_t BookHash;
    };

    struct DiskLevelRecord
    {
        int8_t Side;
        uint8_t Flags;
        uint16_t Reserved;
        int32_t PriceInTicks;
        uint64_t AggregateQuantity;
        uint32_t AggregateOrderCount;
        uint32_t FirstOrderIndex;
        uint32_t OrderCount;
    };

    struct DiskOrderRecord
    {
        int8_t Side;
        uint8_t Flags;
        uint16_t ReturnedIndex;
        int32_t PriceInTicks;
        uint64_t OrderID;
        uint64_t Quantity;
    };

    struct DiskTradeRecord
    {
        double DateTime;
        uint64_t Sequence;
        int8_t Side;
        uint8_t Reserved[3];
        int32_t PriceInTicks;
        uint64_t Volume;
    };
#pragma pack(pop)

    static_assert(sizeof(DiskFileHeader) == 256, "Unexpected SCMBOD1 file header size");

    struct FrameIndexEntry
    {
        double DateTime = 0.0;
        uint64_t FileOffset = 0;
    };

    struct ReplayReaderState
    {
        std::ifstream Stream;
        std::string OpenPath;
        DiskFileHeader Header{};
        std::vector<FrameIndexEntry> Index;
        bool IsLoaded = false;

        void Close()
        {
            if (Stream.is_open())
                Stream.close();
            OpenPath.clear();
            Index.clear();
            std::memset(&Header, 0, sizeof(Header));
            IsLoaded = false;
        }
    };

    struct TradeSample
    {
        double DateTime = 0.0;
        uint64_t Volume = 0;
    };

    struct RefillEvent
    {
        double DateTime = 0.0;
        uint64_t Quantity = 0;
        int Kind = REFILL_NONE;
    };

    struct RemovedOrderCandidate
    {
        double DateTime = 0.0;
        uint64_t Quantity = 0;
        uint64_t ExecutedDuringLife = 0;
    };

    struct OrderLife
    {
        double FirstSeen = 0.0;
        double LastSeen = 0.0;
        uint64_t InitialQuantity = 0;
        uint64_t LastQuantity = 0;
        uint64_t MaximumQuantity = 0;
        uint64_t ExecutionCounterAtFirstSeen = 0;
        uint64_t LastExecutionCounter = 0;
        int MinimumDistanceToTouchTicks = std::numeric_limits<int>::max();
        int SameIdRefreshCount = 0;
        uint64_t CumulativeSameIdRefresh = 0;
    };

    struct LevelState
    {
        uint64_t TotalExecuted = 0;
        uint64_t PreviousAggregateQuantity = 0;
        uint64_t LastSnapshotExecutionCounter = 0;
        uint64_t CurrentAggregateQuantity = 0;
        uint64_t CurrentMboQuantity = 0;
        uint64_t PeakVisibleQuantity = 0;
        uint64_t CycleInitialVisibleQuantity = 0;
        bool HadPreviousSnapshot = false;
        bool CurrentBookPresent = false;
        bool CurrentMboPresent = false;

        std::deque<TradeSample> AggressorTrades;
        std::deque<RefillEvent> Refills;
        std::deque<RemovedOrderCandidate> RecentDepletedOrders;
        std::deque<double> SpoofLikeCancellationTimes;

        double SpoofPenaltyUntil = 0.0;
        double CycleStart = 0.0;
        double LastActivity = 0.0;
        double LastTrade = 0.0;
        double LastRefill = 0.0;
        double LastTestTime = 0.0;
        double AbsorptionStart = 0.0;
        double BreakStart = 0.0;
        double LastSignalTime = 0.0;
        double PeakRefillRate = 0.0;

        int TestCount = 0;
        int MinimumObservedReferencePrice = std::numeric_limits<int>::max();
        int MaximumObservedReferencePrice = std::numeric_limits<int>::min();
        int Phase = PHASE_IDLE;
        int AbsorptionScore = 0;

        bool AbsorptionSignaled = false;
        bool AggressorExhaustionSignaled = false;
        bool PassiveExhaustionSignaled = false;
    };

    struct EngineConfiguration
    {
        int DetectionWindowMilliseconds = 5000;
        int IntensityWindowMilliseconds = 1000;
        uint64_t MinimumExecutedVolume = 30;
        double MinimumExecutionToVisibleRatio = 1.5;
        int MinimumRefillEvidencePoints = 2;
        uint64_t MinimumExecutionForRefill = 8;
        uint64_t MinimumSameIdIncreaseQuantity = 3;
        double PersistentVisibleMinimumFraction = 0.80;
        bool EnableSyntheticRefill = true;
        int SyntheticReplacementMilliseconds = 500;
        double SyntheticSizeToleranceFraction = 0.25;
        bool RequireMboEvidence = true;
        int AbsorptionScoreThreshold = 65;
        int MaximumPriceProgressTicks = 1;
        int MinimumLevelSurvivalMilliseconds = 300;
        int TestSeparationMilliseconds = 250;

        int AggressorRecentWindowMilliseconds = 750;
        int AggressorPriorWindowMilliseconds = 2000;
        double AggressorRecentToPriorRateMaximum = 0.40;
        uint64_t MinimumPriorAggressorVolume = 25;
        uint64_t MinimumOppositeAggressorVolume = 8;
        int OppositeFlowRangeTicks = 2;
        int ReclaimTicks = 1;

        int PassiveBreakTicks = 1;
        int PassiveBreakHoldMilliseconds = 250;
        double RefillRateDecayFraction = 0.35;
        double VisibleQuantityWeakFraction = 0.35;
        uint64_t MinimumContinuingAggressorVolume = 20;
        int ContinuingAggressorRangeTicks = 2;

        uint64_t SpoofMinimumQuantity = 50;
        int SpoofMaximumLifetimeMilliseconds = 1000;
        double SpoofMaximumExecutedFraction = 0.10;
        int SpoofMaximumDistanceToTouchTicks = 3;
        int SpoofRequiredCancellations = 2;
        int SpoofRepeatWindowSeconds = 10;
        int SpoofPenaltySeconds = 5;

        int ActiveLevelPersistenceMilliseconds = 10000;
        int CandidateResetMilliseconds = 6000;
        int SignalCooldownMilliseconds = 3000;
    };

    struct SignalEvent
    {
        int Type = SIGNAL_NONE;
        LevelKey Level;
        double DateTime = 0.0;
        int Score = 0;
        uint64_t ExecutedVolume = 0;
        double ExecutionToVisibleRatio = 0.0;
        int RefillEvidencePoints = 0;
        uint64_t RecentAggressorVolume = 0;
        uint64_t PriorAggressorVolume = 0;
        uint64_t OppositeAggressorVolume = 0;
        SCString Message;
    };

    struct Engine
    {
        std::map<OrderKey, OrderLife> ActiveOrders;
        std::map<LevelKey, LevelState> Levels;
        std::deque<TradeEvent> RecentTrades;
        double LastFrameDateTime = 0.0;
        int LastReferencePriceInTicks = 0;
        bool Initialized = false;

        void Reset()
        {
            ActiveOrders.clear();
            Levels.clear();
            RecentTrades.clear();
            LastFrameDateTime = 0.0;
            LastReferencePriceInTicks = 0;
            Initialized = false;
        }
    };

    struct StudyState
    {
        Engine DetectionEngine;
        uint64_t LastTimeAndSalesSequence = 0;
        bool LiveBaselineEstablished = false;
        bool LoggedNoMbo = false;
        bool LoggedReplayError = false;
        int LastDataSource = -1;

        ReplayReaderState ReplayReader;
        size_t NextReplayFrameIndex = 0;
        double LastReplayTargetDateTime = 0.0;
        std::string LastReplayFilePath;
        BookFrame CurrentFrame;

        std::map<LevelKey, int> ActiveLineNumbers;
        std::vector<int> EventDrawingLineNumbers;
    };

    template <typename T>
    T Clamp(const T Value, const T Minimum, const T Maximum)
    {
        return Value < Minimum ? Minimum : (Value > Maximum ? Maximum : Value);
    }

    double SecondsBetween(const double Newer, const double Older)
    {
        return (Newer - Older) * SECONDS_PER_DAY;
    }

    double MillisecondsBetween(const double Newer, const double Older)
    {
        return SecondsBetween(Newer, Older) * 1000.0;
    }

    double AddMilliseconds(const double DateTime, const int Milliseconds)
    {
        return DateTime + static_cast<double>(Milliseconds) / (SECONDS_PER_DAY * 1000.0);
    }

    double AddSeconds(const double DateTime, const int Seconds)
    {
        return DateTime + static_cast<double>(Seconds) / SECONDS_PER_DAY;
    }

    uint64_t SaturatingSubtract(const uint64_t Larger, const uint64_t Smaller)
    {
        return Larger >= Smaller ? Larger - Smaller : 0;
    }

    void HashMix(uint64_t& Hash, const uint64_t Value)
    {
        Hash ^= Value;
        Hash *= 1099511628211ULL;
    }

    bool IsAbsolutePath(const std::string& Path)
    {
        if (Path.size() >= 3 && Path[1] == ':' && (Path[2] == '\\' || Path[2] == '/'))
            return true;
        if (Path.size() >= 2 && Path[0] == '\\' && Path[1] == '\\')
            return true;
        return !Path.empty() && (Path[0] == '/' || Path[0] == '\\');
    }

    std::string BuildDataFilePath(SCStudyInterfaceRef sc, const SCString& FileName)
    {
        std::string Name = FileName.GetChars() == nullptr ? std::string() : std::string(FileName.GetChars());
        if (Name.empty())
            Name = "MBO_Record.scmbo";
        if (IsAbsolutePath(Name))
            return Name;

        SCString Folder = sc.DataFilesFolder();
        std::string Base = Folder.GetChars() == nullptr ? std::string() : std::string(Folder.GetChars());
        if (!Base.empty() && Base.back() != '\\' && Base.back() != '/')
            Base.push_back('\\');
        return Base + Name;
    }

    std::string SanitizeSymbol(const SCString& Symbol)
    {
        std::string Result = Symbol.GetChars() == nullptr ? std::string("UNKNOWN") : std::string(Symbol.GetChars());
        for (char& Character : Result)
        {
            const bool Allowed = (Character >= 'A' && Character <= 'Z')
                || (Character >= 'a' && Character <= 'z')
                || (Character >= '0' && Character <= '9')
                || Character == '-' || Character == '_' || Character == '.';
            if (!Allowed)
                Character = '_';
        }
        return Result;
    }

    std::string ReadFixedString(const char* Text, const size_t Capacity)
    {
        size_t Length = 0;
        while (Length < Capacity && Text[Length] != '\0')
            ++Length;
        return std::string(Text, Length);
    }

    uint64_t ComputeBookHash(const BookFrame& Frame)
    {
        uint64_t Hash = 1469598103934665603ULL;
        HashMix(Hash, static_cast<uint64_t>(static_cast<uint32_t>(Frame.BestBidInTicks)));
        HashMix(Hash, static_cast<uint64_t>(static_cast<uint32_t>(Frame.BestAskInTicks)));
        for (const BookLevel& Level : Frame.Levels)
        {
            HashMix(Hash, static_cast<uint64_t>(Level.Key.Side + 2));
            HashMix(Hash, static_cast<uint64_t>(static_cast<uint32_t>(Level.Key.PriceInTicks)));
            HashMix(Hash, Level.AggregateQuantity);
            HashMix(Hash, Level.AggregateOrderCount);
            for (const BookOrder& Order : Level.Orders)
            {
                HashMix(Hash, Order.Key.OrderID);
                HashMix(Hash, Order.Quantity);
            }
        }
        return Hash;
    }

    int DistanceToTouch(const OrderKey& Key, const BookFrame& Frame)
    {
        if (Key.Side == SIDE_BID && Frame.BestBidInTicks != 0)
            return std::max(0, Frame.BestBidInTicks - Key.PriceInTicks);
        if (Key.Side == SIDE_ASK && Frame.BestAskInTicks != 0)
            return std::max(0, Key.PriceInTicks - Frame.BestAskInTicks);
        return std::numeric_limits<int>::max();
    }

    bool IsPriceInsideCapturedSideRange(const OrderKey& Key, const BookFrame& Frame)
    {
        bool FoundSide = false;
        int LowestPriceInTicks = std::numeric_limits<int>::max();
        int HighestPriceInTicks = std::numeric_limits<int>::min();

        for (const BookLevel& Level : Frame.Levels)
        {
            if (Level.Key.Side != Key.Side)
                continue;
            FoundSide = true;
            LowestPriceInTicks = std::min(LowestPriceInTicks, Level.Key.PriceInTicks);
            HighestPriceInTicks = std::max(HighestPriceInTicks, Level.Key.PriceInTicks);
        }

        return FoundSide
            && Key.PriceInTicks >= LowestPriceInTicks
            && Key.PriceInTicks <= HighestPriceInTicks;
    }

    bool CaptureSide(
        SCStudyInterfaceRef sc,
        const int Side,
        const int MaximumDepthLevels,
        const int MaximumOrdersPerPrice,
        const uint64_t MinimumOrderQuantity,
        BookFrame& Frame,
        bool& AnyMboSeen)
    {
        const int AvailableLevels = Side == SIDE_BID
            ? sc.GetBidMarketDepthNumberOfLevels()
            : sc.GetAskMarketDepthNumberOfLevels();
        const int LevelsToRead = std::min(AvailableLevels, MaximumDepthLevels);
        if (LevelsToRead <= 0 || sc.TickSize <= 0.0f)
            return false;

        std::vector<n_ACSIL::s_MarketOrderData> Orders(static_cast<size_t>(MaximumOrdersPerPrice));

        for (int LevelIndex = 0; LevelIndex < LevelsToRead; ++LevelIndex)
        {
            s_MarketDepthEntry Entry;
            const int GotEntry = Side == SIDE_BID
                ? sc.GetBidMarketDepthEntryAtLevel(Entry, LevelIndex)
                : sc.GetAskMarketDepthEntryAtLevel(Entry, LevelIndex);
            if (!GotEntry)
                continue;

            const int PriceInTicks = sc.Round(Entry.AdjustedPrice / sc.TickSize);
            BookLevel Level;
            Level.Key = LevelKey{Side, PriceInTicks};
            Level.AggregateQuantity = static_cast<uint64_t>(Entry.Quantity);
            Level.AggregateOrderCount = static_cast<uint32_t>(Entry.NumOrders);

            const int ActualOrders = Side == SIDE_BID
                ? sc.GetBidMarketLimitOrdersForPrice(PriceInTicks, MaximumOrdersPerPrice, Orders.data())
                : sc.GetAskMarketLimitOrdersForPrice(PriceInTicks, MaximumOrdersPerPrice, Orders.data());

            if (ActualOrders > 0)
                AnyMboSeen = true;

            for (int OrderIndex = 0; OrderIndex < ActualOrders; ++OrderIndex)
            {
                const uint64_t Quantity = static_cast<uint64_t>(Orders[OrderIndex].OrderQuantity);
                if (Quantity < MinimumOrderQuantity)
                    continue;

                BookOrder Order;
                Order.Key.Side = Side;
                Order.Key.PriceInTicks = PriceInTicks;
                Order.Key.OrderID = static_cast<uint64_t>(Orders[OrderIndex].OrderID);
                Order.Quantity = Quantity;
                Order.ReturnedIndex = OrderIndex;
                Level.Orders.push_back(Order);
            }

            Frame.Levels.push_back(Level);
            if (LevelIndex == 0)
            {
                if (Side == SIDE_BID)
                    Frame.BestBidInTicks = PriceInTicks;
                else
                    Frame.BestAskInTicks = PriceInTicks;
            }
        }

        return true;
    }

    void CaptureNewTrades(
        SCStudyInterfaceRef sc,
        uint64_t& LastSequence,
        const bool EstablishBaselineOnly,
        std::vector<TradeEvent>& Output)
    {
        c_SCTimeAndSalesArray TimeAndSales;
        sc.GetTimeAndSales(TimeAndSales);
        if (TimeAndSales.Size() <= 0)
            return;

        const uint64_t LatestSequence = static_cast<uint64_t>(TimeAndSales[TimeAndSales.Size() - 1].Sequence);
        if (LastSequence != 0 && LatestSequence < LastSequence)
            LastSequence = 0;

        if (EstablishBaselineOnly || LastSequence == 0)
        {
            LastSequence = LatestSequence;
            return;
        }

        for (int Index = 0; Index < TimeAndSales.Size(); ++Index)
        {
            const s_TimeAndSales& Record = TimeAndSales[Index];
            const uint64_t Sequence = static_cast<uint64_t>(Record.Sequence);
            if (Sequence <= LastSequence)
                continue;

            LastSequence = Sequence;
            if (Record.Type != SC_TS_BID && Record.Type != SC_TS_ASK)
                continue;

            TradeEvent Event;
            SCDateTime AdjustedDateTime = Record.DateTime;
            AdjustedDateTime += sc.TimeScaleAdjustment;
            Event.DateTime = AdjustedDateTime.GetAsDouble();
            Event.Sequence = Sequence;
            Event.Side = Record.Type == SC_TS_BID ? SIDE_BID : SIDE_ASK;
            Event.PriceInTicks = sc.Round((Record.Price * sc.RealTimePriceMultiplier) / sc.TickSize);
            Event.Volume = static_cast<uint64_t>(Record.Volume);
            Output.push_back(Event);
        }
    }

    bool CaptureLiveFrame(
        SCStudyInterfaceRef sc,
        uint64_t& LastTimeAndSalesSequence,
        bool& BaselineEstablished,
        const int MaximumDepthLevels,
        const int MaximumOrdersPerPrice,
        const uint64_t MinimumRecordedOrderQuantity,
        BookFrame& Frame,
        bool& AnyMboSeen)
    {
        Frame = BookFrame();
        Frame.DateTime = sc.GetCurrentDateTime().GetAsDouble();
        AnyMboSeen = false;

        const bool EstablishBaseline = !BaselineEstablished;
        CaptureNewTrades(sc, LastTimeAndSalesSequence, EstablishBaseline, Frame.Trades);
        BaselineEstablished = true;

        const bool GotBid = CaptureSide(
            sc, SIDE_BID, MaximumDepthLevels, MaximumOrdersPerPrice,
            MinimumRecordedOrderQuantity, Frame, AnyMboSeen);
        const bool GotAsk = CaptureSide(
            sc, SIDE_ASK, MaximumDepthLevels, MaximumOrdersPerPrice,
            MinimumRecordedOrderQuantity, Frame, AnyMboSeen);

        if (!Frame.Trades.empty())
            Frame.ReferencePriceInTicks = Frame.Trades.back().PriceInTicks;
        else if (sc.ArraySize > 0 && sc.TickSize > 0.0f)
            Frame.ReferencePriceInTicks = sc.Round(sc.Close[sc.ArraySize - 1] / sc.TickSize);
        else if (Frame.BestBidInTicks != 0 && Frame.BestAskInTicks != 0)
            Frame.ReferencePriceInTicks = (Frame.BestBidInTicks + Frame.BestAskInTicks) / 2;

        Frame.BookHash = ComputeBookHash(Frame);
        return GotBid || GotAsk;
    }

    bool ValidateHeader(const DiskFileHeader& Header)
    {
        const char Magic[8] = {'S', 'C', 'M', 'B', 'O', 'D', '1', '\0'};
        return std::memcmp(Header.Magic, Magic, sizeof(Magic)) == 0
            && Header.Version == FILE_VERSION
            && Header.HeaderSize == sizeof(DiskFileHeader);
    }

    bool ValidateFrameHeader(
        const DiskFrameHeader& Header,
        const DiskFileHeader& FileHeader,
        uint64_t& ExpectedPayloadBytes)
    {
        if (Header.Marker != FRAME_MARKER)
            return false;

        const uint64_t MaximumLevels = std::max<uint64_t>(2ULL, static_cast<uint64_t>(FileHeader.MaximumDepthLevels) * 2ULL);
        if (Header.LevelCount > MaximumLevels
            || Header.OrderCount > 10000000U
            || Header.TradeCount > 10000000U)
        {
            return false;
        }

        const uint64_t LevelBytes = static_cast<uint64_t>(Header.LevelCount) * sizeof(DiskLevelRecord);
        const uint64_t OrderBytes = static_cast<uint64_t>(Header.OrderCount) * sizeof(DiskOrderRecord);
        const uint64_t TradeBytes = static_cast<uint64_t>(Header.TradeCount) * sizeof(DiskTradeRecord);
        ExpectedPayloadBytes = LevelBytes + OrderBytes + TradeBytes;
        return ExpectedPayloadBytes <= std::numeric_limits<uint32_t>::max()
            && Header.PayloadBytes == static_cast<uint32_t>(ExpectedPayloadBytes);
    }

    bool LoadReplayIndex(ReplayReaderState& State, const std::string& Path, SCString& Error)
    {
        if (State.IsLoaded && State.OpenPath == Path)
            return true;

        State.Close();
        State.Stream.open(Path.c_str(), std::ios::binary);
        if (!State.Stream.is_open())
        {
            Error.Format("Unable to open recorded MBO file: %s", Path.c_str());
            return false;
        }

        State.Stream.read(reinterpret_cast<char*>(&State.Header), sizeof(State.Header));
        if (!State.Stream.good() || !ValidateHeader(State.Header))
        {
            Error.Format("Invalid or incompatible SCMBOD1 file: %s", Path.c_str());
            State.Close();
            return false;
        }

        State.Stream.seekg(0, std::ios::end);
        const std::streamoff FileSize = State.Stream.tellg();
        State.Stream.seekg(static_cast<std::streamoff>(sizeof(DiskFileHeader)), std::ios::beg);

        double PreviousDateTime = 0.0;
        while (State.Stream.good())
        {
            const std::streamoff Offset = State.Stream.tellg();
            if (Offset < 0 || Offset + static_cast<std::streamoff>(sizeof(DiskFrameHeader)) > FileSize)
                break;

            DiskFrameHeader Header{};
            State.Stream.read(reinterpret_cast<char*>(&Header), sizeof(Header));
            uint64_t PayloadBytes = 0;
            if (!State.Stream.good() || !ValidateFrameHeader(Header, State.Header, PayloadBytes))
            {
                Error.Format("Corrupt frame header in recorded MBO file near byte %llu", static_cast<unsigned long long>(Offset));
                State.Close();
                return false;
            }

            if (PreviousDateTime != 0.0 && Header.DateTime < PreviousDateTime)
            {
                Error.Format("Recorded MBO timestamps are not ordered near byte %llu", static_cast<unsigned long long>(Offset));
                State.Close();
                return false;
            }

            const std::streamoff EndOffset = Offset
                + static_cast<std::streamoff>(sizeof(DiskFrameHeader))
                + static_cast<std::streamoff>(PayloadBytes);
            if (EndOffset > FileSize)
            {
                Error.Format("Truncated recorded MBO frame near byte %llu", static_cast<unsigned long long>(Offset));
                State.Close();
                return false;
            }

            State.Index.push_back(FrameIndexEntry{Header.DateTime, static_cast<uint64_t>(Offset)});
            PreviousDateTime = Header.DateTime;
            State.Stream.seekg(EndOffset, std::ios::beg);
        }

        if (State.Index.empty())
        {
            Error.Format("Recorded MBO file contains no frames: %s", Path.c_str());
            State.Close();
            return false;
        }

        State.Stream.clear();
        State.IsLoaded = true;
        State.OpenPath = Path;
        return true;
    }

    bool ReadRecordedFrame(
        ReplayReaderState& State,
        const size_t Index,
        BookFrame& Frame,
        SCString& Error)
    {
        if (!State.IsLoaded || Index >= State.Index.size())
            return false;

        State.Stream.clear();
        State.Stream.seekg(static_cast<std::streamoff>(State.Index[Index].FileOffset), std::ios::beg);
        DiskFrameHeader Header{};
        State.Stream.read(reinterpret_cast<char*>(&Header), sizeof(Header));
        uint64_t ExpectedPayloadBytes = 0;
        if (!State.Stream.good() || !ValidateFrameHeader(Header, State.Header, ExpectedPayloadBytes))
        {
            Error.Format("Unable to read recorded MBO frame %u", static_cast<unsigned int>(Index));
            return false;
        }

        std::vector<DiskLevelRecord> Levels(Header.LevelCount);
        std::vector<DiskOrderRecord> Orders(Header.OrderCount);
        std::vector<DiskTradeRecord> Trades(Header.TradeCount);
        if (!Levels.empty())
            State.Stream.read(reinterpret_cast<char*>(Levels.data()), static_cast<std::streamsize>(Levels.size() * sizeof(DiskLevelRecord)));
        if (!Orders.empty())
            State.Stream.read(reinterpret_cast<char*>(Orders.data()), static_cast<std::streamsize>(Orders.size() * sizeof(DiskOrderRecord)));
        if (!Trades.empty())
            State.Stream.read(reinterpret_cast<char*>(Trades.data()), static_cast<std::streamsize>(Trades.size() * sizeof(DiskTradeRecord)));
        if (!State.Stream.good())
        {
            Error.Format("Unable to read recorded MBO frame payload %u", static_cast<unsigned int>(Index));
            return false;
        }

        Frame = BookFrame();
        Frame.DateTime = Header.DateTime;
        Frame.BestBidInTicks = Header.BestBidInTicks;
        Frame.BestAskInTicks = Header.BestAskInTicks;
        Frame.BookHash = Header.BookHash;
        Frame.Levels.reserve(Levels.size());

        for (const DiskLevelRecord& DiskLevel : Levels)
        {
            BookLevel Level;
            Level.Key.Side = DiskLevel.Side;
            Level.Key.PriceInTicks = DiskLevel.PriceInTicks;
            Level.AggregateQuantity = DiskLevel.AggregateQuantity;
            Level.AggregateOrderCount = DiskLevel.AggregateOrderCount;

            const uint64_t EndOrder = static_cast<uint64_t>(DiskLevel.FirstOrderIndex) + DiskLevel.OrderCount;
            if (EndOrder > Orders.size())
            {
                Error.Format("Invalid order range in recorded MBO frame %u", static_cast<unsigned int>(Index));
                return false;
            }

            for (uint32_t OrderIndex = 0; OrderIndex < DiskLevel.OrderCount; ++OrderIndex)
            {
                const DiskOrderRecord& DiskOrder = Orders[DiskLevel.FirstOrderIndex + OrderIndex];
                BookOrder Order;
                Order.Key.Side = DiskOrder.Side;
                Order.Key.PriceInTicks = DiskOrder.PriceInTicks;
                Order.Key.OrderID = DiskOrder.OrderID;
                Order.Quantity = DiskOrder.Quantity;
                Order.ReturnedIndex = DiskOrder.ReturnedIndex;
                Level.Orders.push_back(Order);
            }
            Frame.Levels.push_back(Level);
        }

        Frame.Trades.reserve(Trades.size());
        for (const DiskTradeRecord& DiskTrade : Trades)
        {
            TradeEvent Trade;
            Trade.DateTime = DiskTrade.DateTime;
            Trade.Sequence = DiskTrade.Sequence;
            Trade.Side = DiskTrade.Side;
            Trade.PriceInTicks = DiskTrade.PriceInTicks;
            Trade.Volume = DiskTrade.Volume;
            Frame.Trades.push_back(Trade);
        }

        if (!Frame.Trades.empty())
            Frame.ReferencePriceInTicks = Frame.Trades.back().PriceInTicks;
        else if (Frame.BestBidInTicks != 0 && Frame.BestAskInTicks != 0)
            Frame.ReferencePriceInTicks = (Frame.BestBidInTicks + Frame.BestAskInTicks) / 2;
        else if (Frame.BestBidInTicks != 0)
            Frame.ReferencePriceInTicks = Frame.BestBidInTicks;
        else
            Frame.ReferencePriceInTicks = Frame.BestAskInTicks;

        return true;
    }

    void AddRefillEvent(LevelState& Level, const double DateTime, const uint64_t Quantity, const int Kind)
    {
        // Avoid recording the same heuristic twice during one sampled frame.
        if (!Level.Refills.empty()
            && Level.Refills.back().DateTime == DateTime
            && Level.Refills.back().Kind == Kind)
        {
            Level.Refills.back().Quantity += Quantity;
        }
        else
        {
            Level.Refills.push_back(RefillEvent{DateTime, Quantity, Kind});
        }
        Level.LastRefill = DateTime;
        Level.LastActivity = DateTime;
    }

    void ResetCycle(LevelState& Level)
    {
        Level.CycleStart = 0.0;
        Level.LastActivity = 0.0;
        Level.LastTrade = 0.0;
        Level.LastRefill = 0.0;
        Level.LastTestTime = 0.0;
        Level.AbsorptionStart = 0.0;
        Level.BreakStart = 0.0;
        Level.PeakRefillRate = 0.0;
        Level.TestCount = 0;
        Level.MinimumObservedReferencePrice = std::numeric_limits<int>::max();
        Level.MaximumObservedReferencePrice = std::numeric_limits<int>::min();
        Level.PeakVisibleQuantity = Level.CurrentAggregateQuantity;
        Level.CycleInitialVisibleQuantity = Level.CurrentAggregateQuantity;
        Level.Phase = PHASE_IDLE;
        Level.AbsorptionScore = 0;
        Level.AbsorptionSignaled = false;
        Level.AggressorExhaustionSignaled = false;
        Level.PassiveExhaustionSignaled = false;
        Level.AggressorTrades.clear();
        Level.Refills.clear();
    }

    void StartCycleIfNeeded(LevelState& Level, const double DateTime)
    {
        if (Level.CycleStart != 0.0)
            return;
        Level.CycleStart = DateTime;
        Level.LastActivity = DateTime;
        Level.CycleInitialVisibleQuantity = std::max<uint64_t>(1ULL, Level.CurrentAggregateQuantity);
        Level.PeakVisibleQuantity = Level.CurrentAggregateQuantity;
        Level.Phase = PHASE_CANDIDATE;
    }

    void PurgeLevelHistory(LevelState& Level, const double Now, const EngineConfiguration& Configuration)
    {
        const int TradeHistoryMilliseconds = std::max(
            Configuration.DetectionWindowMilliseconds,
            Configuration.AggressorRecentWindowMilliseconds
                + Configuration.AggressorPriorWindowMilliseconds + 1000);
        const double TradeCutoff = AddMilliseconds(Now, -TradeHistoryMilliseconds);
        const double RefillCutoff = AddMilliseconds(Now, -Configuration.DetectionWindowMilliseconds);
        while (!Level.AggressorTrades.empty() && Level.AggressorTrades.front().DateTime < TradeCutoff)
            Level.AggressorTrades.pop_front();
        while (!Level.Refills.empty() && Level.Refills.front().DateTime < RefillCutoff)
            Level.Refills.pop_front();

        const double SyntheticCutoff = AddMilliseconds(Now, -std::max(1000, Configuration.SyntheticReplacementMilliseconds * 4));
        while (!Level.RecentDepletedOrders.empty() && Level.RecentDepletedOrders.front().DateTime < SyntheticCutoff)
            Level.RecentDepletedOrders.pop_front();

        const double SpoofCutoff = AddSeconds(Now, -Configuration.SpoofRepeatWindowSeconds);
        while (!Level.SpoofLikeCancellationTimes.empty() && Level.SpoofLikeCancellationTimes.front() < SpoofCutoff)
            Level.SpoofLikeCancellationTimes.pop_front();
    }

    uint64_t SumTradeSamples(
        const std::deque<TradeSample>& Samples,
        const double BeginDateTime,
        const double EndDateTime)
    {
        uint64_t Sum = 0;
        for (const TradeSample& Sample : Samples)
        {
            if (Sample.DateTime > BeginDateTime && Sample.DateTime <= EndDateTime)
                Sum += Sample.Volume;
        }
        return Sum;
    }

    uint64_t SumRefillQuantity(
        const std::deque<RefillEvent>& Refills,
        const double BeginDateTime,
        const double EndDateTime)
    {
        uint64_t Sum = 0;
        for (const RefillEvent& Refill : Refills)
        {
            if (Refill.DateTime > BeginDateTime && Refill.DateTime <= EndDateTime)
                Sum += Refill.Quantity;
        }
        return Sum;
    }

    int RefillEvidencePoints(const LevelState& Level, int& MboEvidencePoints)
    {
        int Points = 0;
        MboEvidencePoints = 0;
        for (const RefillEvent& Refill : Level.Refills)
        {
            if (Refill.Kind == REFILL_SAME_ID)
            {
                Points += 3;
                MboEvidencePoints += 3;
            }
            else if (Refill.Kind == REFILL_SYNTHETIC)
            {
                Points += 2;
                MboEvidencePoints += 2;
            }
            else if (Refill.Kind == REFILL_PERSISTENT)
            {
                Points += 1;
                MboEvidencePoints += 1;
            }
            else if (Refill.Kind == REFILL_AGGREGATE)
            {
                Points += 1;
            }
        }
        return Points;
    }

    uint64_t SumAggressorVolumeAtLevel(
        const LevelState& Level,
        const double BeginDateTime,
        const double EndDateTime)
    {
        return SumTradeSamples(Level.AggressorTrades, BeginDateTime, EndDateTime);
    }

    uint64_t OppositeAggressorVolume(
        const Engine& DetectionEngine,
        const LevelKey& Level,
        const double BeginDateTime,
        const double EndDateTime,
        const int PriceRangeTicks)
    {
        uint64_t Sum = 0;
        const int OppositeSide = Level.Side == SIDE_BID ? SIDE_ASK : SIDE_BID;
        for (const TradeEvent& Trade : DetectionEngine.RecentTrades)
        {
            if (Trade.DateTime <= BeginDateTime || Trade.DateTime > EndDateTime || Trade.Side != OppositeSide)
                continue;

            const bool InRange = Level.Side == SIDE_BID
                ? (Trade.PriceInTicks >= Level.PriceInTicks && Trade.PriceInTicks <= Level.PriceInTicks + PriceRangeTicks)
                : (Trade.PriceInTicks <= Level.PriceInTicks && Trade.PriceInTicks >= Level.PriceInTicks - PriceRangeTicks);
            if (InRange)
                Sum += Trade.Volume;
        }
        return Sum;
    }

    uint64_t ContinuingAggressorVolume(
        const Engine& DetectionEngine,
        const LevelKey& Level,
        const double BeginDateTime,
        const double EndDateTime,
        const int PriceRangeTicks)
    {
        uint64_t Sum = 0;
        for (const TradeEvent& Trade : DetectionEngine.RecentTrades)
        {
            if (Trade.DateTime <= BeginDateTime || Trade.DateTime > EndDateTime
                || Trade.Side != Level.Side)
            {
                continue;
            }

            const bool InRange = Level.Side == SIDE_BID
                ? (Trade.PriceInTicks <= Level.PriceInTicks
                    && Trade.PriceInTicks >= Level.PriceInTicks - PriceRangeTicks)
                : (Trade.PriceInTicks >= Level.PriceInTicks
                    && Trade.PriceInTicks <= Level.PriceInTicks + PriceRangeTicks);
            if (InRange)
                Sum += Trade.Volume;
        }
        return Sum;
    }

    int PriceProgressBeyondLevel(const LevelKey& Key, const LevelState& Level)
    {
        if (Key.Side == SIDE_BID)
        {
            if (Level.MinimumObservedReferencePrice == std::numeric_limits<int>::max())
                return 0;
            return std::max(0, Key.PriceInTicks - Level.MinimumObservedReferencePrice);
        }

        if (Level.MaximumObservedReferencePrice == std::numeric_limits<int>::min())
            return 0;
        return std::max(0, Level.MaximumObservedReferencePrice - Key.PriceInTicks);
    }

    int CalculateAbsorptionScore(
        const LevelKey& Key,
        const LevelState& Level,
        const EngineConfiguration& Configuration,
        const double Now,
        double& ExecutionToVisibleRatio,
        int& EvidencePoints,
        int& MboEvidencePoints)
    {
        const double DetectionBegin = AddMilliseconds(
            Now, -Configuration.DetectionWindowMilliseconds);
        const uint64_t ExecutedVolume = SumAggressorVolumeAtLevel(
            Level, DetectionBegin, Now);
        const uint64_t VisibleBase = std::max<uint64_t>(
            1ULL,
            std::max(Level.CycleInitialVisibleQuantity, Level.PeakVisibleQuantity));
        ExecutionToVisibleRatio = static_cast<double>(ExecutedVolume) / static_cast<double>(VisibleBase);
        EvidencePoints = RefillEvidencePoints(Level, MboEvidencePoints);

        int Score = 0;

        // Execution-to-visible contribution: up to 30 points.
        if (Configuration.MinimumExecutionToVisibleRatio > 0.0)
        {
            const double Relative = ExecutionToVisibleRatio / Configuration.MinimumExecutionToVisibleRatio;
            if (Relative < 1.0)
                Score += static_cast<int>(std::floor(20.0 * std::max(0.0, Relative)));
            else
                Score += 20 + static_cast<int>(std::floor(10.0 * std::min(1.0, Relative - 1.0)));
        }

        // Refill evidence contribution: up to 30 points.
        int RefillScore = 0;
        for (const RefillEvent& Refill : Level.Refills)
        {
            if (Refill.Kind == REFILL_SAME_ID)
                RefillScore += 15;
            else if (Refill.Kind == REFILL_SYNTHETIC)
                RefillScore += 10;
            else if (Refill.Kind == REFILL_PERSISTENT)
                RefillScore += 6;
            else if (Refill.Kind == REFILL_AGGREGATE)
                RefillScore += 4;
        }
        Score += std::min(30, RefillScore);

        // Price progress / efficiency contribution: up to 20 points.
        const int ProgressTicks = PriceProgressBeyondLevel(Key, Level);
        if (ProgressTicks <= Configuration.MaximumPriceProgressTicks)
            Score += 20;
        else
            Score += std::max(0, 20 - (ProgressTicks - Configuration.MaximumPriceProgressTicks) * 10);

        // Survival and repeated testing: up to 10 points.
        const double SurvivalMilliseconds = Level.CycleStart == 0.0 ? 0.0 : MillisecondsBetween(Now, Level.CycleStart);
        if (SurvivalMilliseconds >= Configuration.MinimumLevelSurvivalMilliseconds)
            Score += 5;
        Score += std::min(5, Level.TestCount * 2);

        // Current aggressor intensity: up to 10 points.
        const double IntensityBegin = AddMilliseconds(Now, -Configuration.IntensityWindowMilliseconds);
        const uint64_t RecentVolume = SumTradeSamples(Level.AggressorTrades, IntensityBegin, Now);
        if (Configuration.MinimumExecutedVolume > 0)
        {
            const double RelativeIntensity = static_cast<double>(RecentVolume)
                / static_cast<double>(Configuration.MinimumExecutedVolume);
            Score += static_cast<int>(std::floor(10.0 * std::min(1.0, RelativeIntensity)));
        }

        if (Level.SpoofPenaltyUntil >= Now)
            Score -= 30;

        return Clamp(Score, 0, 100);
    }

    bool IsBrokenBeyondLevel(
        const LevelKey& Key,
        const BookFrame& Frame,
        const int ReferencePriceInTicks,
        const int BreakTicks)
    {
        if (Key.Side == SIDE_BID)
        {
            const bool ReferenceBroken = ReferencePriceInTicks != 0
                && ReferencePriceInTicks <= Key.PriceInTicks - BreakTicks;
            const bool BookBroken = Frame.BestBidInTicks != 0
                && Frame.BestBidInTicks <= Key.PriceInTicks - BreakTicks;
            return ReferenceBroken || BookBroken;
        }

        const bool ReferenceBroken = ReferencePriceInTicks != 0
            && ReferencePriceInTicks >= Key.PriceInTicks + BreakTicks;
        const bool BookBroken = Frame.BestAskInTicks != 0
            && Frame.BestAskInTicks >= Key.PriceInTicks + BreakTicks;
        return ReferenceBroken || BookBroken;
    }

    bool IsReclaimedFromLevel(
        const LevelKey& Key,
        const BookFrame& Frame,
        const int ReferencePriceInTicks,
        const int ReclaimTicks)
    {
        if (Key.Side == SIDE_BID)
        {
            return (ReferencePriceInTicks != 0 && ReferencePriceInTicks >= Key.PriceInTicks + ReclaimTicks)
                || (Frame.BestBidInTicks != 0 && Frame.BestBidInTicks >= Key.PriceInTicks + ReclaimTicks);
        }

        return (ReferencePriceInTicks != 0 && ReferencePriceInTicks <= Key.PriceInTicks - ReclaimTicks)
            || (Frame.BestAskInTicks != 0 && Frame.BestAskInTicks <= Key.PriceInTicks - ReclaimTicks);
    }

    void PushAbsorptionSignal(
        const LevelKey& Key,
        LevelState& Level,
        const double Now,
        const uint64_t Executed,
        const double Ratio,
        const int EvidencePoints,
        std::vector<SignalEvent>& Signals)
    {
        SignalEvent Event;
        Event.Type = Key.Side == SIDE_BID ? SIGNAL_BID_ABSORPTION : SIGNAL_ASK_ABSORPTION;
        Event.Level = Key;
        Event.DateTime = Now;
        Event.Score = Level.AbsorptionScore;
        Event.ExecutedVolume = Executed;
        Event.ExecutionToVisibleRatio = Ratio;
        Event.RefillEvidencePoints = EvidencePoints;
        Event.Message.Format(
            "%s ABSORPTION | score %d | executed %llu | X/V %.2f | refill evidence %d",
            Key.Side == SIDE_BID ? "BID" : "ASK",
            Level.AbsorptionScore,
            static_cast<unsigned long long>(Executed),
            Ratio,
            EvidencePoints);
        Signals.push_back(Event);
    }

    void EvaluateLevel(
        Engine& DetectionEngine,
        const LevelKey& Key,
        LevelState& Level,
        const BookFrame& Frame,
        const EngineConfiguration& Configuration,
        std::vector<SignalEvent>& Signals)
    {
        const double Now = Frame.DateTime;
        PurgeLevelHistory(Level, Now, Configuration);

        if (Level.CurrentBookPresent)
            Level.PeakVisibleQuantity = std::max(Level.PeakVisibleQuantity, Level.CurrentAggregateQuantity);

        const bool HasRecentTrades = !Level.AggressorTrades.empty();
        const bool HasRecentRefills = !Level.Refills.empty();
        if ((HasRecentTrades || HasRecentRefills) && Level.CycleStart == 0.0)
            StartCycleIfNeeded(Level, Now);

        if (Level.CycleStart != 0.0 && Frame.ReferencePriceInTicks != 0)
        {
            Level.MinimumObservedReferencePrice = std::min(Level.MinimumObservedReferencePrice, Frame.ReferencePriceInTicks);
            Level.MaximumObservedReferencePrice = std::max(Level.MaximumObservedReferencePrice, Frame.ReferencePriceInTicks);
        }

        if (Level.CycleStart == 0.0)
            return;

        const double DetectionBegin = AddMilliseconds(
            Now, -Configuration.DetectionWindowMilliseconds);
        const uint64_t ExecutedVolume = SumAggressorVolumeAtLevel(
            Level, DetectionBegin, Now);
        double ExecutionToVisibleRatio = 0.0;
        int EvidencePoints = 0;
        int MboEvidencePoints = 0;
        Level.AbsorptionScore = CalculateAbsorptionScore(
            Key, Level, Configuration, Now,
            ExecutionToVisibleRatio, EvidencePoints, MboEvidencePoints);

        const int ProgressTicks = PriceProgressBeyondLevel(Key, Level);
        const double SurvivalMilliseconds = MillisecondsBetween(Now, Level.CycleStart);
        const bool MboRequirementMet = !Configuration.RequireMboEvidence || MboEvidencePoints > 0;
        const bool SignalCooldownMet = Level.LastSignalTime == 0.0
            || MillisecondsBetween(Now, Level.LastSignalTime) >= Configuration.SignalCooldownMilliseconds;

        const bool AbsorptionCondition = ExecutedVolume >= Configuration.MinimumExecutedVolume
            && ExecutionToVisibleRatio >= Configuration.MinimumExecutionToVisibleRatio
            && EvidencePoints >= Configuration.MinimumRefillEvidencePoints
            && MboRequirementMet
            && ProgressTicks <= Configuration.MaximumPriceProgressTicks
            && SurvivalMilliseconds >= Configuration.MinimumLevelSurvivalMilliseconds
            && Level.AbsorptionScore >= Configuration.AbsorptionScoreThreshold
            && Level.SpoofPenaltyUntil < Now;

        if (AbsorptionCondition && !Level.AbsorptionSignaled && SignalCooldownMet)
        {
            Level.AbsorptionSignaled = true;
            Level.AggressorExhaustionSignaled = false;
            Level.PassiveExhaustionSignaled = false;
            Level.AbsorptionStart = Now;
            Level.LastSignalTime = Now;
            Level.Phase = PHASE_ABSORBING;
            PushAbsorptionSignal(Key, Level, Now, ExecutedVolume, ExecutionToVisibleRatio, EvidencePoints, Signals);
        }

        if (Level.Phase == PHASE_ABSORBING
            || Level.Phase == PHASE_AGGRESSOR_EXHAUSTED
            || Level.Phase == PHASE_ABSORBER_WEAKENING)
        {
            const double RecentBegin = AddMilliseconds(Now, -Configuration.AggressorRecentWindowMilliseconds);
            const double PriorBegin = AddMilliseconds(
                RecentBegin, -Configuration.AggressorPriorWindowMilliseconds);
            const uint64_t RecentAggressorVolume = SumTradeSamples(Level.AggressorTrades, RecentBegin, Now);
            const uint64_t PriorAggressorVolume = SumTradeSamples(Level.AggressorTrades, PriorBegin, RecentBegin);
            const uint64_t ContinuingAggressorVolumeValue = ContinuingAggressorVolume(
                DetectionEngine, Key, RecentBegin, Now,
                Configuration.ContinuingAggressorRangeTicks);

            const double RecentRate = Configuration.AggressorRecentWindowMilliseconds > 0
                ? static_cast<double>(RecentAggressorVolume) / Configuration.AggressorRecentWindowMilliseconds
                : 0.0;
            const double PriorRate = Configuration.AggressorPriorWindowMilliseconds > 0
                ? static_cast<double>(PriorAggressorVolume) / Configuration.AggressorPriorWindowMilliseconds
                : 0.0;
            const double RecentToPriorRate = PriorRate > 0.0 ? RecentRate / PriorRate : 1.0;

            const uint64_t OppositeVolume = OppositeAggressorVolume(
                DetectionEngine, Key, RecentBegin, Now, Configuration.OppositeFlowRangeTicks);
            const bool Reclaimed = IsReclaimedFromLevel(
                Key, Frame, Frame.ReferencePriceInTicks, Configuration.ReclaimTicks);

            const double RefillBegin = AddMilliseconds(Now, -Configuration.AggressorRecentWindowMilliseconds);
            const uint64_t RecentRefillQuantity = SumRefillQuantity(Level.Refills, RefillBegin, Now);
            const double CurrentRefillRate = Configuration.AggressorRecentWindowMilliseconds > 0
                ? static_cast<double>(RecentRefillQuantity) / Configuration.AggressorRecentWindowMilliseconds
                : 0.0;
            Level.PeakRefillRate = std::max(Level.PeakRefillRate, CurrentRefillRate);

            const bool RefillDecayed = Level.PeakRefillRate > 0.0
                && CurrentRefillRate <= Level.PeakRefillRate * Configuration.RefillRateDecayFraction;
            const bool VisibleWeak = !Level.CurrentBookPresent
                || (Level.PeakVisibleQuantity > 0
                    && static_cast<double>(Level.CurrentAggregateQuantity)
                        <= static_cast<double>(Level.PeakVisibleQuantity) * Configuration.VisibleQuantityWeakFraction);

            const bool Broken = IsBrokenBeyondLevel(
                Key, Frame, Frame.ReferencePriceInTicks, Configuration.PassiveBreakTicks);
            if (Broken)
            {
                if (Level.BreakStart == 0.0)
                    Level.BreakStart = Now;
            }
            else
            {
                Level.BreakStart = 0.0;
            }

            const bool BreakHeld = Level.BreakStart != 0.0
                && MillisecondsBetween(Now, Level.BreakStart) >= Configuration.PassiveBreakHoldMilliseconds;
            const bool PassiveExhaustionCondition = BreakHeld
                && ContinuingAggressorVolumeValue >= Configuration.MinimumContinuingAggressorVolume
                && (RefillDecayed || VisibleWeak)
                && !Level.PassiveExhaustionSignaled;

            // A held break with continued aggression and weakening refill is
            // treated as absorber exhaustion before testing reversal logic.
            if (PassiveExhaustionCondition)
            {
                SignalEvent Event;
                Event.Type = Key.Side == SIDE_BID
                    ? SIGNAL_BID_ABSORBER_EXHAUSTED
                    : SIGNAL_ASK_ABSORBER_EXHAUSTED;
                Event.Level = Key;
                Event.DateTime = Now;
                Event.Score = Clamp(Level.AbsorptionScore + 10, 0, 100);
                Event.ExecutedVolume = ExecutedVolume;
                Event.ExecutionToVisibleRatio = ExecutionToVisibleRatio;
                Event.RefillEvidencePoints = EvidencePoints;
                Event.RecentAggressorVolume = ContinuingAggressorVolumeValue;
                Event.Message.Format(
                    "%s ABSORBER EXHAUSTED | score %d | continuing aggression %llu | refill rate %.4f / peak %.4f",
                    Key.Side == SIDE_BID ? "BID" : "ASK",
                    Event.Score,
                    static_cast<unsigned long long>(ContinuingAggressorVolumeValue),
                    CurrentRefillRate,
                    Level.PeakRefillRate);
                Signals.push_back(Event);

                Level.PassiveExhaustionSignaled = true;
                Level.LastSignalTime = Now;
                Level.Phase = PHASE_IDLE;
                Level.AbsorptionStart = 0.0;
                Level.BreakStart = 0.0;
                return;
            }

            if ((RefillDecayed || VisibleWeak)
                && ContinuingAggressorVolumeValue >= Configuration.MinimumContinuingAggressorVolume)
            {
                Level.Phase = PHASE_ABSORBER_WEAKENING;
            }
            else if (Level.Phase == PHASE_ABSORBER_WEAKENING)
            {
                Level.Phase = PHASE_ABSORBING;
            }

            const bool AggressorExhaustionCondition = PriorAggressorVolume >= Configuration.MinimumPriorAggressorVolume
                && RecentToPriorRate <= Configuration.AggressorRecentToPriorRateMaximum
                && OppositeVolume >= Configuration.MinimumOppositeAggressorVolume
                && Reclaimed
                && !Broken
                && !Level.AggressorExhaustionSignaled;

            if (AggressorExhaustionCondition)
            {
                SignalEvent Event;
                Event.Type = Key.Side == SIDE_BID
                    ? SIGNAL_SELLER_EXHAUSTION
                    : SIGNAL_BUYER_EXHAUSTION;
                Event.Level = Key;
                Event.DateTime = Now;
                Event.Score = Clamp(
                    Level.AbsorptionScore
                        + static_cast<int>(std::floor((1.0 - std::min(1.0, RecentToPriorRate)) * 15.0)),
                    0,
                    100);
                Event.ExecutedVolume = ExecutedVolume;
                Event.ExecutionToVisibleRatio = ExecutionToVisibleRatio;
                Event.RefillEvidencePoints = EvidencePoints;
                Event.RecentAggressorVolume = RecentAggressorVolume;
                Event.PriorAggressorVolume = PriorAggressorVolume;
                Event.OppositeAggressorVolume = OppositeVolume;
                Event.Message.Format(
                    "%s EXHAUSTION | score %d | aggressor %llu -> %llu | opposite %llu",
                    Key.Side == SIDE_BID ? "SELLER" : "BUYER",
                    Event.Score,
                    static_cast<unsigned long long>(PriorAggressorVolume),
                    static_cast<unsigned long long>(RecentAggressorVolume),
                    static_cast<unsigned long long>(OppositeVolume));
                Signals.push_back(Event);

                Level.AggressorExhaustionSignaled = true;
                Level.LastSignalTime = Now;
                Level.Phase = PHASE_AGGRESSOR_EXHAUSTED;
            }

            const bool ActiveTimedOut = Level.LastActivity != 0.0
                && MillisecondsBetween(Now, Level.LastActivity) > Configuration.ActiveLevelPersistenceMilliseconds;
            if (ActiveTimedOut)
                Level.Phase = PHASE_IDLE;
        }

        const bool CandidateTimedOut = Level.LastActivity != 0.0
            && MillisecondsBetween(Now, Level.LastActivity) > Configuration.CandidateResetMilliseconds;
        if (CandidateTimedOut && Level.Phase == PHASE_IDLE)
            ResetCycle(Level);
    }

    void ProcessFrame(
        Engine& DetectionEngine,
        BookFrame Frame,
        const EngineConfiguration& Configuration,
        std::vector<SignalEvent>& Signals)
    {
        const double Now = Frame.DateTime;
        if (Frame.ReferencePriceInTicks == 0)
            Frame.ReferencePriceInTicks = DetectionEngine.LastReferencePriceInTicks;
        if (Frame.ReferencePriceInTicks != 0)
            DetectionEngine.LastReferencePriceInTicks = Frame.ReferencePriceInTicks;

        // Reset per-frame book-presence flags before applying the snapshot.
        for (auto& Pair : DetectionEngine.Levels)
        {
            Pair.second.CurrentBookPresent = false;
            Pair.second.CurrentMboPresent = false;
            Pair.second.CurrentAggregateQuantity = 0;
            Pair.second.CurrentMboQuantity = 0;
        }

        // Add all new trades first. They establish execution counters used by
        // the order-lifecycle and refill logic below.
        for (const TradeEvent& Trade : Frame.Trades)
        {
            DetectionEngine.RecentTrades.push_back(Trade);
            LevelState& Level = DetectionEngine.Levels[LevelKey{Trade.Side, Trade.PriceInTicks}];
            Level.TotalExecuted += Trade.Volume;
            Level.AggressorTrades.push_back(TradeSample{Trade.DateTime, Trade.Volume});
            Level.LastTrade = Trade.DateTime;
            Level.LastActivity = Trade.DateTime;
            StartCycleIfNeeded(Level, Trade.DateTime);

            if (Level.LastTestTime == 0.0
                || MillisecondsBetween(Trade.DateTime, Level.LastTestTime) >= Configuration.TestSeparationMilliseconds)
            {
                ++Level.TestCount;
                Level.LastTestTime = Trade.DateTime;
            }
        }

        const int GlobalHistoryMilliseconds = std::max(
            Configuration.DetectionWindowMilliseconds,
            Configuration.AggressorRecentWindowMilliseconds + Configuration.AggressorPriorWindowMilliseconds + 1000);
        const double GlobalCutoff = AddMilliseconds(Now, -GlobalHistoryMilliseconds);
        while (!DetectionEngine.RecentTrades.empty()
            && DetectionEngine.RecentTrades.front().DateTime < GlobalCutoff)
        {
            DetectionEngine.RecentTrades.pop_front();
        }

        std::map<LevelKey, const BookLevel*> CurrentLevels;
        std::map<OrderKey, const BookOrder*> CurrentOrders;
        for (const BookLevel& BookLevelValue : Frame.Levels)
        {
            CurrentLevels[BookLevelValue.Key] = &BookLevelValue;
            LevelState& Level = DetectionEngine.Levels[BookLevelValue.Key];
            Level.CurrentBookPresent = true;
            Level.CurrentAggregateQuantity = BookLevelValue.AggregateQuantity;
            Level.CurrentMboPresent = !BookLevelValue.Orders.empty();
            uint64_t MboQuantity = 0;
            for (const BookOrder& Order : BookLevelValue.Orders)
            {
                CurrentOrders[Order.Key] = &Order;
                MboQuantity += Order.Quantity;
            }
            Level.CurrentMboQuantity = MboQuantity;
            if (Level.CycleStart != 0.0)
                Level.PeakVisibleQuantity = std::max(Level.PeakVisibleQuantity, Level.CurrentAggregateQuantity);
        }

        // Process orders that disappeared from the sampled MBO snapshot.
        for (auto Iterator = DetectionEngine.ActiveOrders.begin(); Iterator != DetectionEngine.ActiveOrders.end();)
        {
            if (CurrentOrders.find(Iterator->first) != CurrentOrders.end())
            {
                ++Iterator;
                continue;
            }

            const OrderKey Key = Iterator->first;
            const OrderLife Life = Iterator->second;
            if (!IsPriceInsideCapturedSideRange(Key, Frame))
            {
                Iterator = DetectionEngine.ActiveOrders.erase(Iterator);
                continue;
            }

            LevelState& Level = DetectionEngine.Levels[LevelKey{Key.Side, Key.PriceInTicks}];
            const uint64_t ExecutedDuringLife = SaturatingSubtract(
                Level.TotalExecuted, Life.ExecutionCounterAtFirstSeen);
            const double LifeMilliseconds = MillisecondsBetween(Now, Life.FirstSeen);
            const double ExecutedFraction = Life.MaximumQuantity == 0
                ? 0.0
                : static_cast<double>(ExecutedDuringLife) / static_cast<double>(Life.MaximumQuantity);

            const bool SpoofLike = Life.MaximumQuantity >= Configuration.SpoofMinimumQuantity
                && LifeMilliseconds >= 0.0
                && LifeMilliseconds <= Configuration.SpoofMaximumLifetimeMilliseconds
                && ExecutedFraction <= Configuration.SpoofMaximumExecutedFraction
                && Life.MinimumDistanceToTouchTicks <= Configuration.SpoofMaximumDistanceToTouchTicks;

            if (SpoofLike)
            {
                Level.SpoofLikeCancellationTimes.push_back(Now);
                const double SpoofCutoff = AddSeconds(Now, -Configuration.SpoofRepeatWindowSeconds);
                while (!Level.SpoofLikeCancellationTimes.empty()
                    && Level.SpoofLikeCancellationTimes.front() < SpoofCutoff)
                {
                    Level.SpoofLikeCancellationTimes.pop_front();
                }
                if (static_cast<int>(Level.SpoofLikeCancellationTimes.size()) >= Configuration.SpoofRequiredCancellations)
                    Level.SpoofPenaltyUntil = AddSeconds(Now, Configuration.SpoofPenaltySeconds);
            }
            else if (ExecutedDuringLife >= Configuration.MinimumExecutionForRefill)
            {
                Level.RecentDepletedOrders.push_back(RemovedOrderCandidate{
                    Now, Life.LastQuantity, ExecutedDuringLife});
            }

            Iterator = DetectionEngine.ActiveOrders.erase(Iterator);
        }

        // Process new and continuing MBO orders.
        for (const auto& Pair : CurrentOrders)
        {
            const OrderKey& Key = Pair.first;
            const BookOrder& Current = *Pair.second;
            LevelState& Level = DetectionEngine.Levels[LevelKey{Key.Side, Key.PriceInTicks}];
            auto LifeIterator = DetectionEngine.ActiveOrders.find(Key);

            if (LifeIterator == DetectionEngine.ActiveOrders.end())
            {
                OrderLife Life;
                Life.FirstSeen = Now;
                Life.LastSeen = Now;
                Life.InitialQuantity = Current.Quantity;
                Life.LastQuantity = Current.Quantity;
                Life.MaximumQuantity = Current.Quantity;
                Life.ExecutionCounterAtFirstSeen = Level.TotalExecuted;
                Life.LastExecutionCounter = Level.TotalExecuted;
                Life.MinimumDistanceToTouchTicks = DistanceToTouch(Key, Frame);
                DetectionEngine.ActiveOrders[Key] = Life;

                if (Configuration.EnableSyntheticRefill && !Level.RecentDepletedOrders.empty())
                {
                    for (auto CandidateIterator = Level.RecentDepletedOrders.begin();
                         CandidateIterator != Level.RecentDepletedOrders.end();
                         ++CandidateIterator)
                    {
                        const double DelayMilliseconds = MillisecondsBetween(Now, CandidateIterator->DateTime);
                        const uint64_t Larger = std::max(CandidateIterator->Quantity, Current.Quantity);
                        const uint64_t Smaller = std::min(CandidateIterator->Quantity, Current.Quantity);
                        const double DifferenceFraction = Larger == 0
                            ? 1.0
                            : static_cast<double>(Larger - Smaller) / static_cast<double>(Larger);
                        if (DelayMilliseconds >= 0.0
                            && DelayMilliseconds <= Configuration.SyntheticReplacementMilliseconds
                            && DifferenceFraction <= Configuration.SyntheticSizeToleranceFraction)
                        {
                            StartCycleIfNeeded(Level, Now);
                            AddRefillEvent(Level, Now, Current.Quantity, REFILL_SYNTHETIC);
                            Level.RecentDepletedOrders.erase(CandidateIterator);
                            break;
                        }
                    }
                }
                continue;
            }

            OrderLife& Life = LifeIterator->second;
            const uint64_t ExecutedSinceLast = SaturatingSubtract(
                Level.TotalExecuted, Life.LastExecutionCounter);
            const uint64_t QuantityIncrease = Current.Quantity > Life.LastQuantity
                ? Current.Quantity - Life.LastQuantity
                : 0;

            if (ExecutedSinceLast >= Configuration.MinimumExecutionForRefill
                && QuantityIncrease >= Configuration.MinimumSameIdIncreaseQuantity)
            {
                StartCycleIfNeeded(Level, Now);
                AddRefillEvent(Level, Now, QuantityIncrease, REFILL_SAME_ID);
                ++Life.SameIdRefreshCount;
                Life.CumulativeSameIdRefresh += QuantityIncrease;
            }
            else if (ExecutedSinceLast >= Configuration.MinimumExecutionForRefill
                && Life.LastQuantity > 0
                && static_cast<double>(Current.Quantity)
                    >= static_cast<double>(Life.LastQuantity) * Configuration.PersistentVisibleMinimumFraction)
            {
                StartCycleIfNeeded(Level, Now);
                AddRefillEvent(Level, Now, std::min(ExecutedSinceLast, Current.Quantity), REFILL_PERSISTENT);
            }

            Life.LastSeen = Now;
            Life.LastQuantity = Current.Quantity;
            Life.MaximumQuantity = std::max(Life.MaximumQuantity, Current.Quantity);
            Life.LastExecutionCounter = Level.TotalExecuted;
            Life.MinimumDistanceToTouchTicks = std::min(
                Life.MinimumDistanceToTouchTicks, DistanceToTouch(Key, Frame));
        }

        // Aggregate level refill heuristic. This does not require MBO and is
        // deliberately lower-confidence than same-ID or synthetic evidence.
        for (const auto& Pair : CurrentLevels)
        {
            const LevelKey& Key = Pair.first;
            const BookLevel& Current = *Pair.second;
            LevelState& Level = DetectionEngine.Levels[Key];
            const uint64_t ExecutedSinceSnapshot = SaturatingSubtract(
                Level.TotalExecuted, Level.LastSnapshotExecutionCounter);

            if (Level.HadPreviousSnapshot
                && ExecutedSinceSnapshot >= Configuration.MinimumExecutionForRefill
                && Level.PreviousAggregateQuantity > 0
                && static_cast<double>(Current.AggregateQuantity)
                    >= static_cast<double>(Level.PreviousAggregateQuantity)
                        * Configuration.PersistentVisibleMinimumFraction)
            {
                const uint64_t EstimatedAdded = Current.AggregateQuantity + ExecutedSinceSnapshot > Level.PreviousAggregateQuantity
                    ? Current.AggregateQuantity + ExecutedSinceSnapshot - Level.PreviousAggregateQuantity
                    : 0;
                StartCycleIfNeeded(Level, Now);
                AddRefillEvent(Level, Now, EstimatedAdded, REFILL_AGGREGATE);
            }

            Level.PreviousAggregateQuantity = Current.AggregateQuantity;
            Level.LastSnapshotExecutionCounter = Level.TotalExecuted;
            Level.HadPreviousSnapshot = true;
        }

        // Update reference-price observations and classify each active level.
        for (auto& Pair : DetectionEngine.Levels)
            EvaluateLevel(DetectionEngine, Pair.first, Pair.second, Frame, Configuration, Signals);

        // Remove idle levels that have no recent history. Execution counters do
        // not need to be retained indefinitely once no tracked orders reference
        // the price.
        const double StaleCutoff = AddMilliseconds(
            Now, -std::max(Configuration.CandidateResetMilliseconds * 4, 30000));
        for (auto Iterator = DetectionEngine.Levels.begin(); Iterator != DetectionEngine.Levels.end();)
        {
            const LevelState& Level = Iterator->second;
            bool HasTrackedOrder = false;
            for (const auto& OrderPair : DetectionEngine.ActiveOrders)
            {
                if (OrderPair.first.Side == Iterator->first.Side
                    && OrderPair.first.PriceInTicks == Iterator->first.PriceInTicks)
                {
                    HasTrackedOrder = true;
                    break;
                }
            }

            if (!HasTrackedOrder
                && !Level.CurrentBookPresent
                && Level.Phase == PHASE_IDLE
                && Level.LastActivity != 0.0
                && Level.LastActivity < StaleCutoff)
            {
                Iterator = DetectionEngine.Levels.erase(Iterator);
            }
            else
            {
                ++Iterator;
            }
        }

        DetectionEngine.LastFrameDateTime = Now;
        DetectionEngine.Initialized = true;
    }

    int ResolveDataSource(const int RequestedSource, const int ReplayStatus)
    {
        if (RequestedSource == DATA_SOURCE_LIVE)
            return DATA_SOURCE_LIVE;
        if (RequestedSource == DATA_SOURCE_RECORDED)
            return DATA_SOURCE_RECORDED;
        return ReplayStatus == REPLAY_STOPPED ? DATA_SOURCE_LIVE : DATA_SOURCE_RECORDED;
    }

    size_t FirstFrameAtOrAfter(const std::vector<FrameIndexEntry>& Index, const double DateTime)
    {
        const auto Iterator = std::lower_bound(
            Index.begin(), Index.end(), DateTime,
            [](const FrameIndexEntry& Entry, const double Value)
            {
                return Entry.DateTime < Value;
            });
        return static_cast<size_t>(Iterator - Index.begin());
    }

    bool AdvanceRecordedData(
        StudyState& State,
        const double TargetDateTime,
        const int ReconstructionLookbackSeconds,
        const EngineConfiguration& Configuration,
        std::vector<SignalEvent>& Signals,
        SCString& Error,
        bool& ResetOccurred)
    {
        ResetOccurred = false;
        if (!State.ReplayReader.IsLoaded || State.ReplayReader.Index.empty())
            return false;

        if (State.LastReplayTargetDateTime == 0.0
            && State.NextReplayFrameIndex == 0
            && State.CurrentFrame.DateTime == 0.0)
        {
            const double ReconstructionStart = AddSeconds(
                TargetDateTime, -ReconstructionLookbackSeconds);
            State.NextReplayFrameIndex = FirstFrameAtOrAfter(
                State.ReplayReader.Index, ReconstructionStart);
            State.DetectionEngine.Reset();
            ResetOccurred = true;
        }
        else if (State.LastReplayTargetDateTime != 0.0
            && TargetDateTime + 1e-12 < State.LastReplayTargetDateTime)
        {
            State.DetectionEngine.Reset();
            const double ReconstructionStart = AddSeconds(TargetDateTime, -ReconstructionLookbackSeconds);
            State.NextReplayFrameIndex = FirstFrameAtOrAfter(
                State.ReplayReader.Index, ReconstructionStart);
            State.CurrentFrame = BookFrame();
            ResetOccurred = true;
        }

        bool HaveFrame = false;
        while (State.NextReplayFrameIndex < State.ReplayReader.Index.size()
            && State.ReplayReader.Index[State.NextReplayFrameIndex].DateTime <= TargetDateTime)
        {
            BookFrame Frame;
            if (!ReadRecordedFrame(State.ReplayReader, State.NextReplayFrameIndex, Frame, Error))
                return false;
            ProcessFrame(State.DetectionEngine, Frame, Configuration, Signals);
            State.CurrentFrame = Frame;
            ++State.NextReplayFrameIndex;
            HaveFrame = true;
        }

        State.LastReplayTargetDateTime = TargetDateTime;
        return HaveFrame || State.CurrentFrame.DateTime != 0.0;
    }

    void DeleteStudyDrawings(SCStudyInterfaceRef sc, StudyState& State)
    {
        for (const auto& Pair : State.ActiveLineNumbers)
        {
            if (Pair.second != 0)
                sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, Pair.second);
        }
        State.ActiveLineNumbers.clear();

        for (const int LineNumber : State.EventDrawingLineNumbers)
        {
            if (LineNumber != 0)
                sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, LineNumber);
        }
        State.EventDrawingLineNumbers.clear();
    }

    void ClearSignalSubgraphs(SCStudyInterfaceRef sc)
    {
        for (int SubgraphIndex = 0; SubgraphIndex < 6; ++SubgraphIndex)
        {
            for (int Index = 0; Index < sc.ArraySize; ++Index)
                sc.Subgraph[SubgraphIndex][Index] = 0.0f;
        }
    }

    int BarIndexForDateTime(SCStudyInterfaceRef sc, const double DateTime)
    {
        if (sc.ArraySize <= 0)
            return -1;
        SCDateTime EventDateTime(DateTime);
        const int Index = sc.GetContainingIndexForSCDateTime(sc.ChartNumber, EventDateTime);
        if (Index < 0)
            return -1;
        return Clamp(Index, 0, sc.ArraySize - 1);
    }

    COLORREF SignalColor(
        const SignalEvent& Event,
        const COLORREF BidAbsorptionColor,
        const COLORREF AskAbsorptionColor,
        const COLORREF BullishExhaustionColor,
        const COLORREF BearishExhaustionColor,
        const COLORREF BullishBreakColor,
        const COLORREF BearishBreakColor)
    {
        switch (Event.Type)
        {
            case SIGNAL_BID_ABSORPTION: return BidAbsorptionColor;
            case SIGNAL_ASK_ABSORPTION: return AskAbsorptionColor;
            case SIGNAL_SELLER_EXHAUSTION: return BullishExhaustionColor;
            case SIGNAL_BUYER_EXHAUSTION: return BearishExhaustionColor;
            case SIGNAL_ASK_ABSORBER_EXHAUSTED: return BullishBreakColor;
            case SIGNAL_BID_ABSORBER_EXHAUSTED: return BearishBreakColor;
            default: return RGB(200, 200, 200);
        }
    }

    const char* CompactSignalName(const int Type)
    {
        switch (Type)
        {
            case SIGNAL_BID_ABSORPTION: return "BID ABS";
            case SIGNAL_ASK_ABSORPTION: return "ASK ABS";
            case SIGNAL_SELLER_EXHAUSTION: return "SELLER EXH";
            case SIGNAL_BUYER_EXHAUSTION: return "BUYER EXH";
            case SIGNAL_BID_ABSORBER_EXHAUSTED: return "BID ABSORBER EXH";
            case SIGNAL_ASK_ABSORBER_EXHAUSTED: return "ASK ABSORBER EXH";
            default: return "MBO SIGNAL";
        }
    }

    int SignalPriority(const int Type)
    {
        if (Type == SIGNAL_BID_ABSORBER_EXHAUSTED || Type == SIGNAL_ASK_ABSORBER_EXHAUSTED)
            return 3;
        if (Type == SIGNAL_SELLER_EXHAUSTION || Type == SIGNAL_BUYER_EXHAUSTION)
            return 2;
        if (Type == SIGNAL_BID_ABSORPTION || Type == SIGNAL_ASK_ABSORPTION)
            return 1;
        return 0;
    }

    void PlotSignal(
        SCStudyInterfaceRef sc,
        StudyState& State,
        const SignalEvent& Event,
        const int MarkerOffsetTicks,
        const int LabelOffsetTicks,
        const bool DrawLabels,
        const int LabelFontSize,
        const COLORREF Color)
    {
        const int BarIndex = BarIndexForDateTime(sc, Event.DateTime);
        if (BarIndex < 0 || sc.TickSize <= 0.0f)
            return;

        const double LevelPrice = static_cast<double>(Event.Level.PriceInTicks) * sc.TickSize;
        const bool IsUpSignal = Event.Type == SIGNAL_BID_ABSORPTION
            || Event.Type == SIGNAL_SELLER_EXHAUSTION
            || Event.Type == SIGNAL_ASK_ABSORBER_EXHAUSTED;
        const int SubgraphIndex = Event.Type == SIGNAL_BID_ABSORPTION ? 0
            : Event.Type == SIGNAL_ASK_ABSORPTION ? 1
            : Event.Type == SIGNAL_SELLER_EXHAUSTION ? 2
            : Event.Type == SIGNAL_BUYER_EXHAUSTION ? 3
            : Event.Type == SIGNAL_BID_ABSORBER_EXHAUSTED ? 4
            : 5;

        const float MarkerPrice = static_cast<float>(
            LevelPrice + (IsUpSignal ? -MarkerOffsetTicks : MarkerOffsetTicks) * sc.TickSize);
        if (IsUpSignal)
        {
            if (sc.Subgraph[SubgraphIndex][BarIndex] == 0.0f
                || MarkerPrice < sc.Subgraph[SubgraphIndex][BarIndex])
            {
                sc.Subgraph[SubgraphIndex][BarIndex] = MarkerPrice;
            }
        }
        else
        {
            if (sc.Subgraph[SubgraphIndex][BarIndex] == 0.0f
                || MarkerPrice > sc.Subgraph[SubgraphIndex][BarIndex])
            {
                sc.Subgraph[SubgraphIndex][BarIndex] = MarkerPrice;
            }
        }

        if (!DrawLabels)
            return;

        SCString Label;
        if (Event.Type == SIGNAL_BID_ABSORPTION || Event.Type == SIGNAL_ASK_ABSORPTION)
        {
            Label.Format(
                "%s %d | X/V %.2f | R%d",
                CompactSignalName(Event.Type), Event.Score,
                Event.ExecutionToVisibleRatio, Event.RefillEvidencePoints);
        }
        else if (Event.Type == SIGNAL_SELLER_EXHAUSTION || Event.Type == SIGNAL_BUYER_EXHAUSTION)
        {
            Label.Format(
                "%s %d | %llu>%llu | O%llu",
                CompactSignalName(Event.Type), Event.Score,
                static_cast<unsigned long long>(Event.PriorAggressorVolume),
                static_cast<unsigned long long>(Event.RecentAggressorVolume),
                static_cast<unsigned long long>(Event.OppositeAggressorVolume));
        }
        else
        {
            Label.Format(
                "%s %d | A%llu",
                CompactSignalName(Event.Type), Event.Score,
                static_cast<unsigned long long>(Event.RecentAggressorVolume));
        }

        s_UseTool Tool;
        Tool.Clear();
        Tool.ChartNumber = sc.ChartNumber;
        Tool.DrawingType = DRAWING_TEXT;
        Tool.AddMethod = UTAM_ADD_ALWAYS;
        Tool.Region = 0;
        Tool.BeginIndex = BarIndex;
        Tool.BeginValue = LevelPrice
            + (IsUpSignal ? -LabelOffsetTicks : LabelOffsetTicks) * sc.TickSize;
        Tool.Color = Color;
        Tool.FontSize = LabelFontSize;
        Tool.FontBold = 1;
        Tool.Text = Label;
        Tool.AddAsUserDrawnDrawing = 0;
        if (sc.UseTool(Tool) != 0)
            State.EventDrawingLineNumbers.push_back(Tool.LineNumber);
    }

    void UpdateActiveLevelLines(
        SCStudyInterfaceRef sc,
        StudyState& State,
        const bool DrawActiveLines,
        const bool DisplayLinePrice,
        const int BaseLineWidth,
        const int ActiveLevelPersistenceMilliseconds,
        const COLORREF BidAbsorptionColor,
        const COLORREF AskAbsorptionColor,
        const COLORREF WeakeningColor,
        const COLORREF BullishExhaustionColor,
        const COLORREF BearishExhaustionColor)
    {
        std::set<LevelKey> ActiveKeys;
        const double Now = State.CurrentFrame.DateTime;

        if (DrawActiveLines && sc.ArraySize > 0 && sc.TickSize > 0.0f)
        {
            for (const auto& Pair : State.DetectionEngine.Levels)
            {
                const LevelKey& Key = Pair.first;
                const LevelState& Level = Pair.second;
                if (Level.Phase != PHASE_ABSORBING
                    && Level.Phase != PHASE_AGGRESSOR_EXHAUSTED
                    && Level.Phase != PHASE_ABSORBER_WEAKENING)
                {
                    continue;
                }

                if (Level.LastActivity != 0.0 && Now != 0.0
                    && MillisecondsBetween(Now, Level.LastActivity)
                        > static_cast<double>(ActiveLevelPersistenceMilliseconds))
                {
                    continue;
                }

                ActiveKeys.insert(Key);
                COLORREF Color = Key.Side == SIDE_BID ? BidAbsorptionColor : AskAbsorptionColor;
                SubgraphLineStyles LineStyle = LINESTYLE_SOLID;
                int LineWidth = BaseLineWidth + (Level.AbsorptionScore >= 80 ? 1 : 0);
                if (Level.Phase == PHASE_ABSORBER_WEAKENING)
                {
                    Color = WeakeningColor;
                    LineStyle = LINESTYLE_DASH;
                }
                else if (Level.Phase == PHASE_AGGRESSOR_EXHAUSTED)
                {
                    Color = Key.Side == SIDE_BID ? BullishExhaustionColor : BearishExhaustionColor;
                    LineWidth += 1;
                }

                s_UseTool Tool;
                Tool.Clear();
                Tool.ChartNumber = sc.ChartNumber;
                Tool.DrawingType = DRAWING_HORIZONTAL_RAY;
                Tool.AddMethod = UTAM_ADD_OR_ADJUST;
                Tool.Region = 0;
                Tool.BeginIndex = BarIndexForDateTime(
                    sc, Level.AbsorptionStart != 0.0 ? Level.AbsorptionStart : Level.CycleStart);
                if (Tool.BeginIndex < 0)
                    Tool.BeginIndex = sc.ArraySize - 1;
                Tool.BeginValue = static_cast<double>(Key.PriceInTicks) * sc.TickSize;
                Tool.Color = Color;
                Tool.LineWidth = std::max(1, LineWidth);
                Tool.LineStyle = LineStyle;
                Tool.DisplayHorizontalLineValue = DisplayLinePrice ? 1 : 0;
                Tool.AddAsUserDrawnDrawing = 0;

                const auto Existing = State.ActiveLineNumbers.find(Key);
                if (Existing != State.ActiveLineNumbers.end())
                    Tool.LineNumber = Existing->second;

                if (sc.UseTool(Tool) != 0)
                    State.ActiveLineNumbers[Key] = Tool.LineNumber;
            }
        }

        for (auto Iterator = State.ActiveLineNumbers.begin(); Iterator != State.ActiveLineNumbers.end();)
        {
            if (ActiveKeys.find(Iterator->first) == ActiveKeys.end())
            {
                if (Iterator->second != 0)
                    sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, Iterator->second);
                Iterator = State.ActiveLineNumbers.erase(Iterator);
            }
            else
            {
                ++Iterator;
            }
        }
    }
}

/*============================================================================
    MBO Absorption & Exhaustion Signals
============================================================================*/
SCSFExport scsf_MBOAbsorptionExhaustionSignals(SCStudyInterfaceRef sc)
{
    SCSubgraphRef BidAbsorption = sc.Subgraph[0];
    SCSubgraphRef AskAbsorption = sc.Subgraph[1];
    SCSubgraphRef SellerExhaustion = sc.Subgraph[2];
    SCSubgraphRef BuyerExhaustion = sc.Subgraph[3];
    SCSubgraphRef BidAbsorberExhausted = sc.Subgraph[4];
    SCSubgraphRef AskAbsorberExhausted = sc.Subgraph[5];

    SCInputRef DataSource = sc.Input[0];
    SCInputRef RecordedFileName = sc.Input[1];
    SCInputRef ReplayClock = sc.Input[2];
    SCInputRef ReplayReconstructionLookbackSeconds = sc.Input[3];
    SCInputRef MaximumDepthLevels = sc.Input[4];
    SCInputRef MaximumOrdersPerPrice = sc.Input[5];
    SCInputRef MinimumRecordedOrderQuantity = sc.Input[6];

    SCInputRef DetectionWindowMilliseconds = sc.Input[7];
    SCInputRef IntensityWindowMilliseconds = sc.Input[8];
    SCInputRef MinimumExecutedVolume = sc.Input[9];
    SCInputRef MinimumExecutionToVisibleRatio = sc.Input[10];
    SCInputRef MinimumRefillEvidencePoints = sc.Input[11];
    SCInputRef MinimumExecutionForRefill = sc.Input[12];
    SCInputRef MinimumSameIdIncreaseQuantity = sc.Input[13];
    SCInputRef PersistentVisibleMinimumPercent = sc.Input[14];
    SCInputRef EnableSyntheticRefill = sc.Input[15];
    SCInputRef SyntheticReplacementMilliseconds = sc.Input[16];
    SCInputRef SyntheticSizeTolerancePercent = sc.Input[17];
    SCInputRef RequireMboEvidence = sc.Input[18];
    SCInputRef AbsorptionScoreThreshold = sc.Input[19];
    SCInputRef MaximumPriceProgressTicks = sc.Input[20];
    SCInputRef MinimumLevelSurvivalMilliseconds = sc.Input[21];
    SCInputRef TestSeparationMilliseconds = sc.Input[22];

    SCInputRef AggressorRecentWindowMilliseconds = sc.Input[23];
    SCInputRef AggressorPriorWindowMilliseconds = sc.Input[24];
    SCInputRef AggressorRecentToPriorRateMaximumPercent = sc.Input[25];
    SCInputRef MinimumPriorAggressorVolume = sc.Input[26];
    SCInputRef MinimumOppositeAggressorVolume = sc.Input[27];
    SCInputRef OppositeFlowRangeTicks = sc.Input[28];
    SCInputRef ReclaimTicks = sc.Input[29];

    SCInputRef PassiveBreakTicks = sc.Input[30];
    SCInputRef PassiveBreakHoldMilliseconds = sc.Input[31];
    SCInputRef RefillRateDecayPercent = sc.Input[32];
    SCInputRef VisibleQuantityWeakPercent = sc.Input[33];
    SCInputRef MinimumContinuingAggressorVolume = sc.Input[34];

    SCInputRef SpoofMinimumQuantity = sc.Input[35];
    SCInputRef SpoofMaximumLifetimeMilliseconds = sc.Input[36];
    SCInputRef SpoofMaximumExecutedPercent = sc.Input[37];
    SCInputRef SpoofMaximumDistanceToTouchTicks = sc.Input[38];
    SCInputRef SpoofRequiredCancellations = sc.Input[39];
    SCInputRef SpoofRepeatWindowSeconds = sc.Input[40];
    SCInputRef SpoofPenaltySeconds = sc.Input[41];

    SCInputRef ActiveLevelPersistenceMilliseconds = sc.Input[42];
    SCInputRef CandidateResetMilliseconds = sc.Input[43];
    SCInputRef SignalCooldownMilliseconds = sc.Input[44];

    SCInputRef DrawActiveLevelLines = sc.Input[45];
    SCInputRef DisplayLinePrice = sc.Input[46];
    SCInputRef ActiveLineWidth = sc.Input[47];
    SCInputRef DrawSignalLabels = sc.Input[48];
    SCInputRef MarkerOffsetTicks = sc.Input[49];
    SCInputRef LabelOffsetTicks = sc.Input[50];
    SCInputRef LabelFontSize = sc.Input[51];

    SCInputRef BidAbsorptionColor = sc.Input[52];
    SCInputRef AskAbsorptionColor = sc.Input[53];
    SCInputRef BullishExhaustionColor = sc.Input[54];
    SCInputRef BearishExhaustionColor = sc.Input[55];
    SCInputRef BullishBreakColor = sc.Input[56];
    SCInputRef BearishBreakColor = sc.Input[57];
    SCInputRef WeakeningColor = sc.Input[58];

    SCInputRef EnableAlerts = sc.Input[59];
    SCInputRef AlertNumber = sc.Input[60];
    SCInputRef WriteSignalsToLog = sc.Input[61];
    SCInputRef ContinuingAggressorRangeTicks = sc.Input[62];

    if (sc.SetDefaults)
    {
        sc.GraphName = "MBO Absorption & Exhaustion Signals";
        sc.StudyDescription = "Signals probable bid/ask absorption, aggressor exhaustion after absorption, and passive absorber exhaustion using Market-by-Order snapshots, aggregate depth, and Time and Sales. Supports SCMBOD1 recorded MBO playback from the separate MBO Snapshot Recorder. All classifications are probabilistic.";
        sc.AutoLoop = 0;
        sc.GraphRegion = 0;
        sc.UpdateAlways = 1;
        sc.UsesMarketDepthData = 1;
        sc.FreeDLL = 0;
        sc.AlertOnlyOncePerBar = 0;
        sc.ResetAlertOnNewBar = 0;

        BidAbsorption.Name = "Bid Absorption";
        BidAbsorption.DrawStyle = DRAWSTYLE_ARROW_UP;
        BidAbsorption.PrimaryColor = RGB(0, 190, 150);
        BidAbsorption.LineWidth = 2;
        BidAbsorption.DrawZeros = false;

        AskAbsorption.Name = "Ask Absorption";
        AskAbsorption.DrawStyle = DRAWSTYLE_ARROW_DOWN;
        AskAbsorption.PrimaryColor = RGB(240, 150, 30);
        AskAbsorption.LineWidth = 2;
        AskAbsorption.DrawZeros = false;

        SellerExhaustion.Name = "Seller Exhaustion After Bid Absorption";
        SellerExhaustion.DrawStyle = DRAWSTYLE_ARROW_UP;
        SellerExhaustion.PrimaryColor = RGB(0, 230, 90);
        SellerExhaustion.LineWidth = 4;
        SellerExhaustion.DrawZeros = false;

        BuyerExhaustion.Name = "Buyer Exhaustion After Ask Absorption";
        BuyerExhaustion.DrawStyle = DRAWSTYLE_ARROW_DOWN;
        BuyerExhaustion.PrimaryColor = RGB(245, 70, 70);
        BuyerExhaustion.LineWidth = 4;
        BuyerExhaustion.DrawZeros = false;

        BidAbsorberExhausted.Name = "Bid Absorber Exhausted";
        BidAbsorberExhausted.DrawStyle = DRAWSTYLE_ARROW_DOWN;
        BidAbsorberExhausted.PrimaryColor = RGB(190, 70, 220);
        BidAbsorberExhausted.LineWidth = 4;
        BidAbsorberExhausted.DrawZeros = false;

        AskAbsorberExhausted.Name = "Ask Absorber Exhausted";
        AskAbsorberExhausted.DrawStyle = DRAWSTYLE_ARROW_UP;
        AskAbsorberExhausted.PrimaryColor = RGB(50, 150, 255);
        AskAbsorberExhausted.LineWidth = 4;
        AskAbsorberExhausted.DrawZeros = false;

        DataSource.Name = "Data Source";
        DataSource.SetCustomInputStrings("Auto: Live unless chart replaying;Live MBO;Recorded SCMBOD1 File");
        DataSource.SetCustomInputIndex(MboAE::DATA_SOURCE_AUTO);

        RecordedFileName.Name = "Recorded MBO File Name (Data folder or absolute path)";
        RecordedFileName.SetString("MBO_Record.scmbo");

        ReplayClock.Name = "Recorded Replay Clock";
        ReplayClock.SetCustomInputStrings("Latest Chart Data Record;Replay Timer");
        ReplayClock.SetCustomInputIndex(MboAE::REPLAY_CLOCK_LATEST_CHART_RECORD);

        ReplayReconstructionLookbackSeconds.Name = "Replay State Reconstruction Lookback Seconds";
        ReplayReconstructionLookbackSeconds.SetInt(30);
        ReplayReconstructionLookbackSeconds.SetIntLimits(1, 600);

        MaximumDepthLevels.Name = "Maximum Depth Levels Per Side";
        MaximumDepthLevels.SetInt(20);
        MaximumDepthLevels.SetIntLimits(1, 200);

        MaximumOrdersPerPrice.Name = "Maximum MBO Orders Read Per Price";
        MaximumOrdersPerPrice.SetInt(200);
        MaximumOrdersPerPrice.SetIntLimits(1, 5000);

        MinimumRecordedOrderQuantity.Name = "Minimum MBO Order Quantity Captured";
        MinimumRecordedOrderQuantity.SetInt(3);
        MinimumRecordedOrderQuantity.SetIntLimits(1, 1000000);

        DetectionWindowMilliseconds.Name = "Absorption Detection Window (ms)";
        DetectionWindowMilliseconds.SetInt(5000);
        DetectionWindowMilliseconds.SetIntLimits(250, 60000);

        IntensityWindowMilliseconds.Name = "Current Aggressor Intensity Window (ms)";
        IntensityWindowMilliseconds.SetInt(1000);
        IntensityWindowMilliseconds.SetIntLimits(100, 10000);

        MinimumExecutedVolume.Name = "Absorption: Minimum Aggressive Volume At Price";
        MinimumExecutedVolume.SetInt(30);
        MinimumExecutedVolume.SetIntLimits(1, 1000000);

        MinimumExecutionToVisibleRatio.Name = "Absorption: Minimum Executed / Peak Visible Ratio";
        MinimumExecutionToVisibleRatio.SetFloat(1.5f);
        MinimumExecutionToVisibleRatio.SetFloatLimits(0.1f, 100.0f);

        MinimumRefillEvidencePoints.Name = "Absorption: Minimum Refill Evidence Points";
        MinimumRefillEvidencePoints.SetInt(2);
        MinimumRefillEvidencePoints.SetIntLimits(1, 100);

        MinimumExecutionForRefill.Name = "Refill: Minimum Executions Between MBO Observations";
        MinimumExecutionForRefill.SetInt(8);
        MinimumExecutionForRefill.SetIntLimits(1, 1000000);

        MinimumSameIdIncreaseQuantity.Name = "Refill: Minimum Same-Order-ID Quantity Increase";
        MinimumSameIdIncreaseQuantity.SetInt(3);
        MinimumSameIdIncreaseQuantity.SetIntLimits(1, 1000000);

        PersistentVisibleMinimumPercent.Name = "Refill: Persistent Visible Minimum % Of Prior";
        PersistentVisibleMinimumPercent.SetFloat(80.0f);
        PersistentVisibleMinimumPercent.SetFloatLimits(1.0f, 100.0f);

        EnableSyntheticRefill.Name = "Refill: Enable Synthetic Replacement Heuristic";
        EnableSyntheticRefill.SetYesNo(1);

        SyntheticReplacementMilliseconds.Name = "Refill: Maximum Synthetic Replacement Delay (ms)";
        SyntheticReplacementMilliseconds.SetInt(500);
        SyntheticReplacementMilliseconds.SetIntLimits(10, 10000);

        SyntheticSizeTolerancePercent.Name = "Refill: Synthetic Replacement Size Tolerance %";
        SyntheticSizeTolerancePercent.SetFloat(25.0f);
        SyntheticSizeTolerancePercent.SetFloatLimits(0.0f, 100.0f);

        RequireMboEvidence.Name = "Absorption: Require MBO-Level Refill Evidence";
        RequireMboEvidence.SetYesNo(1);

        AbsorptionScoreThreshold.Name = "Absorption: Minimum Score (0-100)";
        AbsorptionScoreThreshold.SetInt(65);
        AbsorptionScoreThreshold.SetIntLimits(1, 100);

        MaximumPriceProgressTicks.Name = "Absorption: Maximum Progress Through Level (ticks)";
        MaximumPriceProgressTicks.SetInt(1);
        MaximumPriceProgressTicks.SetIntLimits(0, 20);

        MinimumLevelSurvivalMilliseconds.Name = "Absorption: Minimum Level Survival (ms)";
        MinimumLevelSurvivalMilliseconds.SetInt(300);
        MinimumLevelSurvivalMilliseconds.SetIntLimits(0, 60000);

        TestSeparationMilliseconds.Name = "Absorption: Minimum Separation Between Tests (ms)";
        TestSeparationMilliseconds.SetInt(250);
        TestSeparationMilliseconds.SetIntLimits(10, 10000);

        AggressorRecentWindowMilliseconds.Name = "Aggressor Exhaustion: Recent Window (ms)";
        AggressorRecentWindowMilliseconds.SetInt(750);
        AggressorRecentWindowMilliseconds.SetIntLimits(100, 10000);

        AggressorPriorWindowMilliseconds.Name = "Aggressor Exhaustion: Prior Window (ms)";
        AggressorPriorWindowMilliseconds.SetInt(2000);
        AggressorPriorWindowMilliseconds.SetIntLimits(100, 30000);

        AggressorRecentToPriorRateMaximumPercent.Name = "Aggressor Exhaustion: Maximum Recent/Prior Rate %";
        AggressorRecentToPriorRateMaximumPercent.SetFloat(40.0f);
        AggressorRecentToPriorRateMaximumPercent.SetFloatLimits(0.0f, 100.0f);

        MinimumPriorAggressorVolume.Name = "Aggressor Exhaustion: Minimum Prior Aggressor Volume";
        MinimumPriorAggressorVolume.SetInt(25);
        MinimumPriorAggressorVolume.SetIntLimits(1, 1000000);

        MinimumOppositeAggressorVolume.Name = "Aggressor Exhaustion: Minimum Opposite Aggressor Volume";
        MinimumOppositeAggressorVolume.SetInt(8);
        MinimumOppositeAggressorVolume.SetIntLimits(0, 1000000);

        OppositeFlowRangeTicks.Name = "Aggressor Exhaustion: Opposite Flow Price Range (ticks)";
        OppositeFlowRangeTicks.SetInt(2);
        OppositeFlowRangeTicks.SetIntLimits(0, 20);

        ReclaimTicks.Name = "Aggressor Exhaustion: Required Reclaim (ticks)";
        ReclaimTicks.SetInt(1);
        ReclaimTicks.SetIntLimits(0, 20);

        PassiveBreakTicks.Name = "Passive Absorber Exhaustion: Break Distance (ticks)";
        PassiveBreakTicks.SetInt(1);
        PassiveBreakTicks.SetIntLimits(1, 20);

        PassiveBreakHoldMilliseconds.Name = "Passive Absorber Exhaustion: Break Hold (ms)";
        PassiveBreakHoldMilliseconds.SetInt(250);
        PassiveBreakHoldMilliseconds.SetIntLimits(0, 10000);

        RefillRateDecayPercent.Name = "Passive Absorber Exhaustion: Refill Rate <= Peak %";
        RefillRateDecayPercent.SetFloat(35.0f);
        RefillRateDecayPercent.SetFloatLimits(0.0f, 100.0f);

        VisibleQuantityWeakPercent.Name = "Passive Absorber Exhaustion: Visible Qty <= Peak %";
        VisibleQuantityWeakPercent.SetFloat(35.0f);
        VisibleQuantityWeakPercent.SetFloatLimits(0.0f, 100.0f);

        MinimumContinuingAggressorVolume.Name = "Passive Absorber Exhaustion: Minimum Continuing Aggressor Volume";
        MinimumContinuingAggressorVolume.SetInt(20);
        MinimumContinuingAggressorVolume.SetIntLimits(1, 1000000);

        ContinuingAggressorRangeTicks.Name = "Passive Absorber Exhaustion: Continuing Aggression Range Beyond Level (ticks)";
        ContinuingAggressorRangeTicks.SetInt(2);
        ContinuingAggressorRangeTicks.SetIntLimits(0, 20);

        SpoofMinimumQuantity.Name = "Spoof-Like Penalty: Minimum Individual Order Quantity";
        SpoofMinimumQuantity.SetInt(50);
        SpoofMinimumQuantity.SetIntLimits(1, 1000000);

        SpoofMaximumLifetimeMilliseconds.Name = "Spoof-Like Penalty: Maximum Order Lifetime (ms)";
        SpoofMaximumLifetimeMilliseconds.SetInt(1000);
        SpoofMaximumLifetimeMilliseconds.SetIntLimits(10, 60000);

        SpoofMaximumExecutedPercent.Name = "Spoof-Like Penalty: Maximum Executed / Order Size %";
        SpoofMaximumExecutedPercent.SetFloat(10.0f);
        SpoofMaximumExecutedPercent.SetFloatLimits(0.0f, 100.0f);

        SpoofMaximumDistanceToTouchTicks.Name = "Spoof-Like Penalty: Maximum Distance From Touch (ticks)";
        SpoofMaximumDistanceToTouchTicks.SetInt(3);
        SpoofMaximumDistanceToTouchTicks.SetIntLimits(0, 100);

        SpoofRequiredCancellations.Name = "Spoof-Like Penalty: Repeated Cancellations Required";
        SpoofRequiredCancellations.SetInt(2);
        SpoofRequiredCancellations.SetIntLimits(1, 20);

        SpoofRepeatWindowSeconds.Name = "Spoof-Like Penalty: Repeat Window Seconds";
        SpoofRepeatWindowSeconds.SetInt(10);
        SpoofRepeatWindowSeconds.SetIntLimits(1, 600);

        SpoofPenaltySeconds.Name = "Spoof-Like Penalty: Block Absorption Signals Seconds";
        SpoofPenaltySeconds.SetInt(5);
        SpoofPenaltySeconds.SetIntLimits(1, 600);

        ActiveLevelPersistenceMilliseconds.Name = "Active Absorption Line Timeout Without Activity (ms)";
        ActiveLevelPersistenceMilliseconds.SetInt(10000);
        ActiveLevelPersistenceMilliseconds.SetIntLimits(250, 120000);

        CandidateResetMilliseconds.Name = "Candidate State Reset Without Activity (ms)";
        CandidateResetMilliseconds.SetInt(6000);
        CandidateResetMilliseconds.SetIntLimits(250, 120000);

        SignalCooldownMilliseconds.Name = "Same-Level Signal Cooldown (ms)";
        SignalCooldownMilliseconds.SetInt(3000);
        SignalCooldownMilliseconds.SetIntLimits(0, 120000);

        DrawActiveLevelLines.Name = "Draw Active Absorption Level Lines";
        DrawActiveLevelLines.SetYesNo(1);

        DisplayLinePrice.Name = "Display Price On Active Level Lines";
        DisplayLinePrice.SetYesNo(1);

        ActiveLineWidth.Name = "Active Level Base Line Width";
        ActiveLineWidth.SetInt(2);
        ActiveLineWidth.SetIntLimits(1, 10);

        DrawSignalLabels.Name = "Draw Signal Text Labels";
        DrawSignalLabels.SetYesNo(1);

        MarkerOffsetTicks.Name = "Signal Marker Offset (ticks)";
        MarkerOffsetTicks.SetInt(1);
        MarkerOffsetTicks.SetIntLimits(0, 20);

        LabelOffsetTicks.Name = "Signal Label Offset (ticks)";
        LabelOffsetTicks.SetInt(3);
        LabelOffsetTicks.SetIntLimits(0, 50);

        LabelFontSize.Name = "Signal Label Font Size";
        LabelFontSize.SetInt(8);
        LabelFontSize.SetIntLimits(6, 24);

        BidAbsorptionColor.Name = "Bid Absorption Color";
        BidAbsorptionColor.SetColor(RGB(0, 190, 150));

        AskAbsorptionColor.Name = "Ask Absorption Color";
        AskAbsorptionColor.SetColor(RGB(240, 150, 30));

        BullishExhaustionColor.Name = "Seller Exhaustion / Bullish Reversal Color";
        BullishExhaustionColor.SetColor(RGB(0, 230, 90));

        BearishExhaustionColor.Name = "Buyer Exhaustion / Bearish Reversal Color";
        BearishExhaustionColor.SetColor(RGB(245, 70, 70));

        BullishBreakColor.Name = "Ask Absorber Exhausted / Bullish Break Color";
        BullishBreakColor.SetColor(RGB(50, 150, 255));

        BearishBreakColor.Name = "Bid Absorber Exhausted / Bearish Break Color";
        BearishBreakColor.SetColor(RGB(190, 70, 220));

        WeakeningColor.Name = "Passive Absorber Weakening Line Color";
        WeakeningColor.SetColor(RGB(230, 210, 40));

        EnableAlerts.Name = "Enable Alerts";
        EnableAlerts.SetYesNo(1);

        AlertNumber.Name = "Alert Sound Number (0 = no sound)";
        AlertNumber.SetInt(1);
        AlertNumber.SetIntLimits(0, 150);

        WriteSignalsToLog.Name = "Write Signals To Message Log";
        WriteSignalsToLog.SetYesNo(1);
        return;
    }

    using namespace MboAE;

    StudyState* State = static_cast<StudyState*>(sc.GetPersistentPointer(1));
    if (sc.LastCallToFunction)
    {
        if (State != nullptr)
        {
            DeleteStudyDrawings(sc, *State);
            State->ReplayReader.Close();
            delete State;
            sc.SetPersistentPointer(1, nullptr);
        }
        return;
    }

    if (State == nullptr)
    {
        State = new StudyState;
        sc.SetPersistentPointer(1, State);
    }

    if (sc.IsFullRecalculation)
    {
        DeleteStudyDrawings(sc, *State);
        ClearSignalSubgraphs(sc);
        State->DetectionEngine.Reset();
        State->LastTimeAndSalesSequence = 0;
        State->LiveBaselineEstablished = false;
        State->NextReplayFrameIndex = 0;
        State->LastReplayTargetDateTime = 0.0;
        State->CurrentFrame = BookFrame();
    }

    EngineConfiguration Configuration;
    Configuration.DetectionWindowMilliseconds = Clamp(DetectionWindowMilliseconds.GetInt(), 250, 60000);
    Configuration.IntensityWindowMilliseconds = Clamp(IntensityWindowMilliseconds.GetInt(), 100, 10000);
    Configuration.MinimumExecutedVolume = static_cast<uint64_t>(Clamp(MinimumExecutedVolume.GetInt(), 1, 1000000));
    Configuration.MinimumExecutionToVisibleRatio = Clamp(static_cast<double>(MinimumExecutionToVisibleRatio.GetFloat()), 0.1, 100.0);
    Configuration.MinimumRefillEvidencePoints = Clamp(MinimumRefillEvidencePoints.GetInt(), 1, 100);
    Configuration.MinimumExecutionForRefill = static_cast<uint64_t>(Clamp(MinimumExecutionForRefill.GetInt(), 1, 1000000));
    Configuration.MinimumSameIdIncreaseQuantity = static_cast<uint64_t>(Clamp(MinimumSameIdIncreaseQuantity.GetInt(), 1, 1000000));
    Configuration.PersistentVisibleMinimumFraction = Clamp(static_cast<double>(PersistentVisibleMinimumPercent.GetFloat()) / 100.0, 0.01, 1.0);
    Configuration.EnableSyntheticRefill = EnableSyntheticRefill.GetYesNo() != 0;
    Configuration.SyntheticReplacementMilliseconds = Clamp(SyntheticReplacementMilliseconds.GetInt(), 10, 10000);
    Configuration.SyntheticSizeToleranceFraction = Clamp(static_cast<double>(SyntheticSizeTolerancePercent.GetFloat()) / 100.0, 0.0, 1.0);
    Configuration.RequireMboEvidence = RequireMboEvidence.GetYesNo() != 0;
    Configuration.AbsorptionScoreThreshold = Clamp(AbsorptionScoreThreshold.GetInt(), 1, 100);
    Configuration.MaximumPriceProgressTicks = Clamp(MaximumPriceProgressTicks.GetInt(), 0, 20);
    Configuration.MinimumLevelSurvivalMilliseconds = Clamp(MinimumLevelSurvivalMilliseconds.GetInt(), 0, 60000);
    Configuration.TestSeparationMilliseconds = Clamp(TestSeparationMilliseconds.GetInt(), 10, 10000);

    Configuration.AggressorRecentWindowMilliseconds = Clamp(AggressorRecentWindowMilliseconds.GetInt(), 100, 10000);
    Configuration.AggressorPriorWindowMilliseconds = Clamp(AggressorPriorWindowMilliseconds.GetInt(), 100, 30000);
    Configuration.AggressorRecentToPriorRateMaximum = Clamp(
        static_cast<double>(AggressorRecentToPriorRateMaximumPercent.GetFloat()) / 100.0, 0.0, 1.0);
    Configuration.MinimumPriorAggressorVolume = static_cast<uint64_t>(Clamp(MinimumPriorAggressorVolume.GetInt(), 1, 1000000));
    Configuration.MinimumOppositeAggressorVolume = static_cast<uint64_t>(Clamp(MinimumOppositeAggressorVolume.GetInt(), 0, 1000000));
    Configuration.OppositeFlowRangeTicks = Clamp(OppositeFlowRangeTicks.GetInt(), 0, 20);
    Configuration.ReclaimTicks = Clamp(ReclaimTicks.GetInt(), 0, 20);

    Configuration.PassiveBreakTicks = Clamp(PassiveBreakTicks.GetInt(), 1, 20);
    Configuration.PassiveBreakHoldMilliseconds = Clamp(PassiveBreakHoldMilliseconds.GetInt(), 0, 10000);
    Configuration.RefillRateDecayFraction = Clamp(static_cast<double>(RefillRateDecayPercent.GetFloat()) / 100.0, 0.0, 1.0);
    Configuration.VisibleQuantityWeakFraction = Clamp(static_cast<double>(VisibleQuantityWeakPercent.GetFloat()) / 100.0, 0.0, 1.0);
    Configuration.MinimumContinuingAggressorVolume = static_cast<uint64_t>(Clamp(MinimumContinuingAggressorVolume.GetInt(), 1, 1000000));
    Configuration.ContinuingAggressorRangeTicks = Clamp(ContinuingAggressorRangeTicks.GetInt(), 0, 20);

    Configuration.SpoofMinimumQuantity = static_cast<uint64_t>(Clamp(SpoofMinimumQuantity.GetInt(), 1, 1000000));
    Configuration.SpoofMaximumLifetimeMilliseconds = Clamp(SpoofMaximumLifetimeMilliseconds.GetInt(), 10, 60000);
    Configuration.SpoofMaximumExecutedFraction = Clamp(static_cast<double>(SpoofMaximumExecutedPercent.GetFloat()) / 100.0, 0.0, 1.0);
    Configuration.SpoofMaximumDistanceToTouchTicks = Clamp(SpoofMaximumDistanceToTouchTicks.GetInt(), 0, 100);
    Configuration.SpoofRequiredCancellations = Clamp(SpoofRequiredCancellations.GetInt(), 1, 20);
    Configuration.SpoofRepeatWindowSeconds = Clamp(SpoofRepeatWindowSeconds.GetInt(), 1, 600);
    Configuration.SpoofPenaltySeconds = Clamp(SpoofPenaltySeconds.GetInt(), 1, 600);

    Configuration.ActiveLevelPersistenceMilliseconds = Clamp(ActiveLevelPersistenceMilliseconds.GetInt(), 250, 120000);
    Configuration.CandidateResetMilliseconds = Clamp(CandidateResetMilliseconds.GetInt(), 250, 120000);
    Configuration.SignalCooldownMilliseconds = Clamp(SignalCooldownMilliseconds.GetInt(), 0, 120000);

    const int ResolvedDataSource = ResolveDataSource(
        static_cast<int>(DataSource.GetIndex()), sc.ReplayStatus);
    if (ResolvedDataSource != State->LastDataSource)
    {
        DeleteStudyDrawings(sc, *State);
        ClearSignalSubgraphs(sc);
        State->DetectionEngine.Reset();
        State->LastTimeAndSalesSequence = 0;
        State->LiveBaselineEstablished = false;
        State->NextReplayFrameIndex = 0;
        State->LastReplayTargetDateTime = 0.0;
        State->CurrentFrame = BookFrame();
        State->LastDataSource = ResolvedDataSource;
    }

    std::vector<SignalEvent> Signals;
    bool HaveFrame = false;

    if (ResolvedDataSource == DATA_SOURCE_LIVE)
    {
        bool AnyMboSeen = false;
        BookFrame Frame;
        if (CaptureLiveFrame(
            sc,
            State->LastTimeAndSalesSequence,
            State->LiveBaselineEstablished,
            Clamp(MaximumDepthLevels.GetInt(), 1, 200),
            Clamp(MaximumOrdersPerPrice.GetInt(), 1, 5000),
            static_cast<uint64_t>(Clamp(MinimumRecordedOrderQuantity.GetInt(), 1, 1000000)),
            Frame,
            AnyMboSeen))
        {
            ProcessFrame(State->DetectionEngine, Frame, Configuration, Signals);
            State->CurrentFrame = Frame;
            HaveFrame = true;
        }

        if (HaveFrame && Configuration.RequireMboEvidence && !AnyMboSeen
            && !State->LoggedNoMbo && sc.ReplayStatus == REPLAY_STOPPED)
        {
            sc.AddMessageToLog(
                "MBO Absorption & Exhaustion: aggregate depth is present but no individual MBO orders were returned. Verify Service Package 12, a supported Sierra feed, MBO subscription settings, and use on a normal intraday chart.",
                1);
            State->LoggedNoMbo = true;
        }
        if (AnyMboSeen)
            State->LoggedNoMbo = false;
    }
    else
    {
        const std::string ReplayPath = BuildDataFilePath(sc, RecordedFileName.GetString());
        if (ReplayPath != State->LastReplayFilePath)
        {
            State->ReplayReader.Close();
            State->LastReplayFilePath = ReplayPath;
            State->NextReplayFrameIndex = 0;
            State->LastReplayTargetDateTime = 0.0;
            State->DetectionEngine.Reset();
            State->LoggedReplayError = false;
        }

        SCString Error;
        if (LoadReplayIndex(State->ReplayReader, ReplayPath, Error))
        {
            const std::string RecordedSymbol = ReadFixedString(
                State->ReplayReader.Header.Symbol, sizeof(State->ReplayReader.Header.Symbol));
            const std::string ChartSymbol = SanitizeSymbol(sc.Symbol);
            if (RecordedSymbol != ChartSymbol)
            {
                Error.Format(
                    "Recorded MBO symbol %s does not match chart symbol %s",
                    RecordedSymbol.c_str(), ChartSymbol.c_str());
            }
            else if (std::fabs(State->ReplayReader.Header.TickSize - sc.TickSize) > 1e-10)
            {
                Error.Format(
                    "Recorded MBO tick size %.10g does not match chart tick size %.10g",
                    State->ReplayReader.Header.TickSize,
                    static_cast<double>(sc.TickSize));
            }
            else
            {
                double TargetDateTime = sc.GetCurrentDateTime().GetAsDouble();
                if (sc.ReplayStatus != REPLAY_STOPPED)
                {
                    if (static_cast<int>(ReplayClock.GetIndex()) == REPLAY_CLOCK_LATEST_CHART_RECORD
                        && sc.LatestDateTimeForLastBar.GetAsDouble() != 0.0)
                    {
                        TargetDateTime = sc.LatestDateTimeForLastBar.GetAsDouble();
                    }
                    else
                    {
                        TargetDateTime = sc.CurrentDateTimeForReplay.GetAsDouble();
                    }
                }

                bool ReplayResetOccurred = false;
                HaveFrame = AdvanceRecordedData(
                    *State,
                    TargetDateTime,
                    Clamp(ReplayReconstructionLookbackSeconds.GetInt(), 1, 600),
                    Configuration,
                    Signals,
                    Error,
                    ReplayResetOccurred);
                if (ReplayResetOccurred)
                {
                    DeleteStudyDrawings(sc, *State);
                    ClearSignalSubgraphs(sc);
                }
            }
        }

        if (Error.GetLength() != 0 && !State->LoggedReplayError)
        {
            sc.AddMessageToLog(Error, 1);
            State->LoggedReplayError = true;
        }
        else if (Error.GetLength() == 0)
        {
            State->LoggedReplayError = false;
        }
    }

    const SignalEvent* HighestPriorityAlert = nullptr;
    for (const SignalEvent& Event : Signals)
    {
        const COLORREF Color = SignalColor(
            Event,
            BidAbsorptionColor.GetColor(),
            AskAbsorptionColor.GetColor(),
            BullishExhaustionColor.GetColor(),
            BearishExhaustionColor.GetColor(),
            BullishBreakColor.GetColor(),
            BearishBreakColor.GetColor());

        PlotSignal(
            sc,
            *State,
            Event,
            Clamp(MarkerOffsetTicks.GetInt(), 0, 20),
            Clamp(LabelOffsetTicks.GetInt(), 0, 50),
            DrawSignalLabels.GetYesNo() != 0,
            Clamp(LabelFontSize.GetInt(), 6, 24),
            Color);

        if (WriteSignalsToLog.GetYesNo() != 0)
            sc.AddMessageToLog(Event.Message, 0);

        if (HighestPriorityAlert == nullptr
            || SignalPriority(Event.Type) > SignalPriority(HighestPriorityAlert->Type))
        {
            HighestPriorityAlert = &Event;
        }
    }

    if (EnableAlerts.GetYesNo() != 0 && HighestPriorityAlert != nullptr)
    {
        const int AlertBarIndex = BarIndexForDateTime(sc, HighestPriorityAlert->DateTime);
        if (AlertBarIndex >= 0)
        {
            sc.SetAlert(
                Clamp(AlertNumber.GetInt(), 0, 150),
                AlertBarIndex,
                HighestPriorityAlert->Message);
        }
    }

    if (HaveFrame || State->CurrentFrame.DateTime != 0.0)
    {
        UpdateActiveLevelLines(
            sc,
            *State,
            DrawActiveLevelLines.GetYesNo() != 0,
            DisplayLinePrice.GetYesNo() != 0,
            Clamp(ActiveLineWidth.GetInt(), 1, 10),
            Configuration.ActiveLevelPersistenceMilliseconds,
            BidAbsorptionColor.GetColor(),
            AskAbsorptionColor.GetColor(),
            WeakeningColor.GetColor(),
            BullishExhaustionColor.GetColor(),
            BearishExhaustionColor.GetColor());
    }
}
