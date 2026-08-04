#include "sierrachart.h"

// Sierra Chart's headers define min/max as macros. Undefine them before
// using the C++ standard library versions and std::numeric_limits<T>::min/max.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

/*
    Sierra Chart MBO DOM Intelligence
    Package date: 2026-08-04

    Studies:
      1. MBO DOM Intelligence - live/custom-replay DOM-style large-order display,
         probable iceberg/refill highlighting, and spoof-like liquidity filtering.
      2. MBO Snapshot Recorder - records live MBO snapshots and Time & Sales to
         a custom binary file for use by Study 1 during Sierra Chart replay.

    Important limitations:
      - Spoofing is intent-based. This code detects repeated short-lived,
        low-execution cancellations which are only "spoof-like" behavior.
      - Sierra Chart does not natively record/replay MBO. The recorder writes
        periodic snapshots available to ACSIL. Book changes occurring between
        study calls can be missed.
      - Market-by-Order and aggregate Market Depth are separate streams in
        Sierra Chart and are not guaranteed to be synchronized.
*/

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

SCDLLName("Sierra Chart MBO DOM Intelligence")

namespace MboDom
{
    constexpr int SIDE_ASK = -1;
    constexpr int SIDE_BID = 1;
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

    enum SpoofDisplayMode
    {
        SPOOF_SHOW = 0,
        SPOOF_DIM = 1,
        SPOOF_HIDE = 2
    };

    enum IcebergKind
    {
        ICEBERG_NONE = 0,
        ICEBERG_NATIVE_ID = 1,
        ICEBERG_PERSISTENT_REFILL = 2,
        ICEBERG_SYNTHETIC = 3
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

    static_assert(sizeof(DiskFileHeader) == 256, "Unexpected MBO file header size");

    struct FrameIndexEntry
    {
        double DateTime = 0.0;
        uint64_t FileOffset = 0;
    };

    struct RecorderConfiguration
    {
        std::string Path;
        int FileMode = 0; // 0 off, 1 append, 2 rewrite once.
        int MinimumSnapshotIntervalMilliseconds = 50;
        int HeartbeatIntervalMilliseconds = 1000;
        int FlushIntervalSeconds = 1;
        uint32_t MinimumRecordedOrderQuantity = 3;
        uint32_t MaximumDepthLevels = 20;
        double TickSize = 0.0;
        std::string Symbol;
    };

    struct RecorderState
    {
        std::ofstream Stream;
        std::string OpenPath;
        bool IsOpen = false;
        int OpenFileMode = 0;
        uint32_t OpenMinimumRecordedOrderQuantity = 0;
        uint32_t OpenMaximumDepthLevels = 0;
        double OpenTickSize = 0.0;
        std::string OpenSymbol;
        bool RewriteConsumed = false;
        std::string RewriteConsumedPath;
        double LastWrittenDateTime = 0.0;
        double LastFlushDateTime = 0.0;
        uint64_t LastWrittenBookHash = 0;
        uint64_t FramesWritten = 0;
        std::vector<TradeEvent> PendingTrades;

        void Close()
        {
            if (Stream.is_open())
            {
                Stream.flush();
                Stream.close();
            }
            IsOpen = false;
            OpenPath.clear();
            OpenFileMode = 0;
            OpenMinimumRecordedOrderQuantity = 0;
            OpenMaximumDepthLevels = 0;
            OpenTickSize = 0.0;
            OpenSymbol.clear();
            PendingTrades.clear();
        }
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

    struct RemovedOrderCandidate
    {
        double DateTime = 0.0;
        uint64_t Quantity = 0;
        uint64_t ExecutedDuringLife = 0;
    };

    struct LevelBehavior
    {
        uint64_t TotalExecuted = 0;
        std::deque<double> SpoofLikeCancellationTimes;
        std::deque<RemovedOrderCandidate> RecentDepletedOrders;
        double SuppressUntil = 0.0;
        double IcebergUntil = 0.0;
        int IcebergKind = ICEBERG_NONE;
        int IcebergStrength = 0;
        int SpoofScore = 0;
    };

    struct OrderLife
    {
        double FirstSeen = 0.0;
        double LastSeen = 0.0;
        uint64_t InitialQuantity = 0;
        uint64_t LastQuantity = 0;
        uint64_t MaximumQuantity = 0;
        uint64_t LastExecutedCounter = 0;
        uint64_t LastVisibleQuantityAhead = 0;
        uint64_t EstimatedExecutedAtOrder = 0;
        int MinimumDistanceToTouchTicks = std::numeric_limits<int>::max();
        int RefreshObservations = 0;
        uint64_t CumulativeRefresh = 0;
    };

    struct DetectionConfiguration
    {
        uint64_t MinimumIcebergExecution = 10;
        uint64_t MinimumNativeRefreshQuantity = 3;
        int IcebergSignalPersistenceSeconds = 30;
        bool EnablePersistentQuantityHeuristic = true;
        double PersistentQuantityMinimumFraction = 0.80;
        bool EnableSyntheticIceberg = true;
        int SyntheticReplacementMilliseconds = 500;
        double SyntheticSizeToleranceFraction = 0.20;

        uint64_t SpoofMinimumQuantity = 50;
        int SpoofMaximumLifetimeMilliseconds = 1000;
        double SpoofMaximumExecutedFraction = 0.10;
        int SpoofMaximumDistanceToTouchTicks = 3;
        int SpoofRequiredCancellations = 3;
        int SpoofRepeatWindowSeconds = 10;
        int SpoofSuppressionSeconds = 5;
    };

    struct DetectionEvent
    {
        int Type = 0; // 1 iceberg, 2 spoof-like cluster.
        LevelKey Level;
        int IcebergKind = ICEBERG_NONE;
        int Strength = 0;
        SCString Message;
    };

    struct DetectionEngine
    {
        std::map<OrderKey, OrderLife> ActiveOrders;
        std::map<LevelKey, LevelBehavior> Levels;
        double LastFrameDateTime = 0.0;
        bool Initialized = false;

        void Reset()
        {
            ActiveOrders.clear();
            Levels.clear();
            LastFrameDateTime = 0.0;
            Initialized = false;
        }
    };

    struct DisplayRow
    {
        LevelKey Level;
        uint64_t AggregateQuantity = 0;
        uint64_t TrustedAggregateQuantity = 0;
        uint64_t SuppressedMboQuantity = 0;
        uint64_t LargeMboQuantity = 0;
        uint64_t TrustedLargeQuantity = 0;
        std::vector<uint64_t> TrustedOrderQuantities;
        bool HasMboOrders = false;
        bool IcebergActive = false;
        int IcebergKind = ICEBERG_NONE;
        int IcebergStrength = 0;
        bool SpoofSuppressed = false;
        int SpoofScore = 0;
    };

    struct VisualConfiguration
    {
        COLORREF BidColor = RGB(0, 135, 230);
        COLORREF AskColor = RGB(225, 80, 70);
        COLORREF BidIcebergColor = RGB(0, 230, 110);
        COLORREF AskIcebergColor = RGB(255, 170, 0);
        COLORREF SpoofColor = RGB(125, 125, 125);
        COLORREF TextColor = RGB(245, 245, 245);
        int FontHeight = 14;
        int SpoofMode = SPOOF_DIM;
        bool ShowAggregateDepth = true;
        bool ShowIndividualOrders = true;
        int MaximumOrdersInText = 4;
        bool DrawLargeLevelLines = true;
        bool DrawOnlyIcebergLines = false;
        bool DisplayLinePrice = true;
        int NormalLineWidth = 1;
        int IcebergLineWidth = 3;
    };

    struct IndicatorState
    {
        uint64_t LastTimeAndSalesSequence = 0;
        bool LiveBaselineEstablished = false;
        bool LoggedNoMbo = false;
        bool LoggedNoDomColumns = false;
        bool LoggedReplayLoadError = false;

        BookFrame CurrentFrame;
        DetectionEngine Engine;
        std::vector<DisplayRow> DisplayRows;
        VisualConfiguration Visual;

        RecorderState Recorder;
        ReplayReaderState ReplayReader;
        size_t NextReplayFrameIndex = 0;
        double LastReplayTargetDateTime = 0.0;
        std::string LastReplayFilePath;
        int LastDataSource = -1;

        std::map<LevelKey, int> DrawingLineNumbers;
        uint64_t LastDrawingHash = 0;
    };

    struct RecorderStudyState
    {
        uint64_t LastTimeAndSalesSequence = 0;
        bool LiveBaselineEstablished = false;
        RecorderState Recorder;
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
                HashMix(Hash, static_cast<uint64_t>(Order.ReturnedIndex));
            }
        }
        return Hash;
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
            const double AdjustedPrice = Record.Price * sc.RealTimePriceMultiplier;
            Event.PriceInTicks = sc.Round(AdjustedPrice / sc.TickSize);
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

        Frame.BookHash = ComputeBookHash(Frame);
        return GotBid || GotAsk;
    }

    DiskFileHeader MakeDiskHeader(const RecorderConfiguration& Configuration)
    {
        DiskFileHeader Header{};
        const char Magic[8] = {'S', 'C', 'M', 'B', 'O', 'D', '1', '\0'};
        std::memcpy(Header.Magic, Magic, sizeof(Magic));
        Header.Version = FILE_VERSION;
        Header.HeaderSize = sizeof(DiskFileHeader);
        Header.TickSize = Configuration.TickSize;
        Header.MinimumRecordedOrderQuantity = Configuration.MinimumRecordedOrderQuantity;
        Header.MaximumDepthLevels = Configuration.MaximumDepthLevels;
        std::strncpy(Header.Symbol, Configuration.Symbol.c_str(), sizeof(Header.Symbol) - 1);
        return Header;
    }

    bool ValidateHeader(const DiskFileHeader& Header)
    {
        const char Magic[8] = {'S', 'C', 'M', 'B', 'O', 'D', '1', '\0'};
        return std::memcmp(Header.Magic, Magic, sizeof(Magic)) == 0
            && Header.Version == FILE_VERSION
            && Header.HeaderSize == sizeof(DiskFileHeader);
    }

    std::string ReadFixedString(const char* Text, const size_t Capacity)
    {
        size_t Length = 0;
        while (Length < Capacity && Text[Length] != '\0')
            ++Length;
        return std::string(Text, Length);
    }

    bool ValidateFrameHeader(
        const DiskFrameHeader& Header,
        const DiskFileHeader& FileHeader,
        uint64_t& ExpectedPayloadBytes)
    {
        if (Header.Marker != FRAME_MARKER)
            return false;

        const uint64_t MaximumLevels = std::max<uint64_t>(2, static_cast<uint64_t>(FileHeader.MaximumDepthLevels) * 2ULL);
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

    bool OpenRecorder(
        RecorderState& State,
        const RecorderConfiguration& Configuration,
        SCString& Error)
    {
        if (Configuration.FileMode == 0)
        {
            State.Close();
            return false;
        }

        const bool SameOpenConfiguration = State.IsOpen
            && State.OpenPath == Configuration.Path
            && State.OpenFileMode == Configuration.FileMode
            && State.OpenMinimumRecordedOrderQuantity == Configuration.MinimumRecordedOrderQuantity
            && State.OpenMaximumDepthLevels == Configuration.MaximumDepthLevels
            && std::fabs(State.OpenTickSize - Configuration.TickSize) <= 1e-10
            && State.OpenSymbol == Configuration.Symbol;
        if (SameOpenConfiguration)
            return true;

        State.Close();

        const bool Rewrite = Configuration.FileMode == 2
            && (!State.RewriteConsumed || State.RewriteConsumedPath != Configuration.Path);
        bool ExistingValid = false;
        if (!Rewrite)
        {
            std::ifstream Existing(Configuration.Path.c_str(), std::ios::binary);
            if (Existing.is_open())
            {
                DiskFileHeader Header{};
                Existing.read(reinterpret_cast<char*>(&Header), sizeof(Header));
                ExistingValid = Existing.good() && ValidateHeader(Header);
                if (ExistingValid)
                {
                    const bool TickSizeMatches = std::fabs(Header.TickSize - Configuration.TickSize) <= 1e-10;
                    const bool SymbolMatches = ReadFixedString(Header.Symbol, sizeof(Header.Symbol)) == Configuration.Symbol;
                    const bool MinimumQuantityMatches = Header.MinimumRecordedOrderQuantity == Configuration.MinimumRecordedOrderQuantity;
                    const bool MaximumDepthMatches = Header.MaximumDepthLevels == Configuration.MaximumDepthLevels;
                    if (!TickSizeMatches || !SymbolMatches || !MinimumQuantityMatches || !MaximumDepthMatches)
                    {
                        Error.Format("Existing SCMBOD1 file header does not match symbol, tick size, minimum MBO quantity, or depth range: %s", Configuration.Path.c_str());
                        Existing.close();
                        return false;
                    }
                }
                Existing.close();
                if (!ExistingValid)
                {
                    Error.Format("Existing file is not a compatible SCMBOD1 recording: %s", Configuration.Path.c_str());
                    return false;
                }
            }
        }

        const std::ios::openmode Mode = std::ios::binary | std::ios::out
            | (Rewrite || !ExistingValid ? std::ios::trunc : std::ios::app);
        State.Stream.open(Configuration.Path.c_str(), Mode);
        if (!State.Stream.is_open())
        {
            Error.Format("Unable to open MBO recording file: %s", Configuration.Path.c_str());
            return false;
        }

        if (Rewrite || !ExistingValid)
        {
            const DiskFileHeader Header = MakeDiskHeader(Configuration);
            State.Stream.write(reinterpret_cast<const char*>(&Header), sizeof(Header));
            if (!State.Stream.good())
            {
                Error.Format("Unable to write MBO recording header: %s", Configuration.Path.c_str());
                State.Close();
                return false;
            }
        }

        State.IsOpen = true;
        State.OpenPath = Configuration.Path;
        State.OpenFileMode = Configuration.FileMode;
        State.OpenMinimumRecordedOrderQuantity = Configuration.MinimumRecordedOrderQuantity;
        State.OpenMaximumDepthLevels = Configuration.MaximumDepthLevels;
        State.OpenTickSize = Configuration.TickSize;
        State.OpenSymbol = Configuration.Symbol;
        if (Rewrite)
        {
            State.RewriteConsumed = true;
            State.RewriteConsumedPath = Configuration.Path;
        }
        State.LastWrittenDateTime = 0.0;
        State.LastFlushDateTime = 0.0;
        State.LastWrittenBookHash = 0;
        return true;
    }

    bool WriteFrame(
        RecorderState& State,
        const RecorderConfiguration& Configuration,
        const BookFrame& Frame,
        SCString& Error)
    {
        if (!State.IsOpen)
            return false;

        if (!Frame.Trades.empty())
            State.PendingTrades.insert(State.PendingTrades.end(), Frame.Trades.begin(), Frame.Trades.end());

        const double SinceLastMilliseconds = State.LastWrittenDateTime == 0.0
            ? std::numeric_limits<double>::max()
            : MillisecondsBetween(Frame.DateTime, State.LastWrittenDateTime);
        const double HeartbeatMilliseconds = static_cast<double>(Configuration.HeartbeatIntervalMilliseconds);
        const bool BookChanged = Frame.BookHash != State.LastWrittenBookHash;
        const bool HasTrades = !State.PendingTrades.empty();

        if (SinceLastMilliseconds < Configuration.MinimumSnapshotIntervalMilliseconds)
            return false;
        if (!BookChanged && !HasTrades && SinceLastMilliseconds < HeartbeatMilliseconds)
            return false;

        std::vector<DiskLevelRecord> DiskLevels;
        std::vector<DiskOrderRecord> DiskOrders;
        std::vector<DiskTradeRecord> DiskTrades;
        DiskLevels.reserve(Frame.Levels.size());

        for (const BookLevel& Level : Frame.Levels)
        {
            DiskLevelRecord DiskLevel{};
            DiskLevel.Side = static_cast<int8_t>(Level.Key.Side);
            DiskLevel.PriceInTicks = Level.Key.PriceInTicks;
            DiskLevel.AggregateQuantity = Level.AggregateQuantity;
            DiskLevel.AggregateOrderCount = Level.AggregateOrderCount;
            DiskLevel.FirstOrderIndex = static_cast<uint32_t>(DiskOrders.size());
            DiskLevel.OrderCount = static_cast<uint32_t>(Level.Orders.size());
            DiskLevels.push_back(DiskLevel);

            for (const BookOrder& Order : Level.Orders)
            {
                DiskOrderRecord DiskOrder{};
                DiskOrder.Side = static_cast<int8_t>(Order.Key.Side);
                DiskOrder.ReturnedIndex = static_cast<uint16_t>(Clamp(Order.ReturnedIndex, 0, 65535));
                DiskOrder.PriceInTicks = Order.Key.PriceInTicks;
                DiskOrder.OrderID = Order.Key.OrderID;
                DiskOrder.Quantity = Order.Quantity;
                DiskOrders.push_back(DiskOrder);
            }
        }

        DiskTrades.reserve(State.PendingTrades.size());
        for (const TradeEvent& Trade : State.PendingTrades)
        {
            DiskTradeRecord DiskTrade{};
            DiskTrade.DateTime = Trade.DateTime;
            DiskTrade.Sequence = Trade.Sequence;
            DiskTrade.Side = static_cast<int8_t>(Trade.Side);
            DiskTrade.PriceInTicks = Trade.PriceInTicks;
            DiskTrade.Volume = Trade.Volume;
            DiskTrades.push_back(DiskTrade);
        }

        DiskFrameHeader Header{};
        Header.Marker = FRAME_MARKER;
        Header.DateTime = Frame.DateTime;
        Header.BestBidInTicks = Frame.BestBidInTicks;
        Header.BestAskInTicks = Frame.BestAskInTicks;
        Header.LevelCount = static_cast<uint32_t>(DiskLevels.size());
        Header.OrderCount = static_cast<uint32_t>(DiskOrders.size());
        Header.TradeCount = static_cast<uint32_t>(DiskTrades.size());
        Header.BookHash = Frame.BookHash;
        Header.PayloadBytes = static_cast<uint32_t>(
            DiskLevels.size() * sizeof(DiskLevelRecord)
            + DiskOrders.size() * sizeof(DiskOrderRecord)
            + DiskTrades.size() * sizeof(DiskTradeRecord));

        State.Stream.write(reinterpret_cast<const char*>(&Header), sizeof(Header));
        if (!DiskLevels.empty())
            State.Stream.write(reinterpret_cast<const char*>(DiskLevels.data()), static_cast<std::streamsize>(DiskLevels.size() * sizeof(DiskLevelRecord)));
        if (!DiskOrders.empty())
            State.Stream.write(reinterpret_cast<const char*>(DiskOrders.data()), static_cast<std::streamsize>(DiskOrders.size() * sizeof(DiskOrderRecord)));
        if (!DiskTrades.empty())
            State.Stream.write(reinterpret_cast<const char*>(DiskTrades.data()), static_cast<std::streamsize>(DiskTrades.size() * sizeof(DiskTradeRecord)));

        if (!State.Stream.good())
        {
            Error.Format("Error writing MBO frame to: %s", Configuration.Path.c_str());
            State.Close();
            return false;
        }

        State.LastWrittenDateTime = Frame.DateTime;
        State.LastWrittenBookHash = Frame.BookHash;
        State.PendingTrades.clear();
        ++State.FramesWritten;

        if (State.LastFlushDateTime == 0.0
            || SecondsBetween(Frame.DateTime, State.LastFlushDateTime) >= Configuration.FlushIntervalSeconds)
        {
            State.Stream.flush();
            State.LastFlushDateTime = Frame.DateTime;
        }

        return true;
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
            Error.Format("Invalid or unsupported recorded MBO file: %s", Path.c_str());
            State.Close();
            return false;
        }

        State.Stream.seekg(0, std::ios::end);
        const std::streamoff FileSizeOffset = State.Stream.tellg();
        if (FileSizeOffset < static_cast<std::streamoff>(sizeof(DiskFileHeader)))
        {
            Error.Format("Recorded MBO file is truncated: %s", Path.c_str());
            State.Close();
            return false;
        }
        const uint64_t FileSize = static_cast<uint64_t>(FileSizeOffset);
        State.Stream.seekg(static_cast<std::streamoff>(sizeof(DiskFileHeader)), std::ios::beg);

        double PreviousDateTime = 0.0;
        while (true)
        {
            const std::streampos Offset = State.Stream.tellg();
            const uint64_t OffsetValue = static_cast<uint64_t>(static_cast<std::streamoff>(Offset));
            if (OffsetValue == FileSize)
                break;
            if (OffsetValue > FileSize || FileSize - OffsetValue < sizeof(DiskFrameHeader))
            {
                Error.Format("Truncated MBO frame header in: %s", Path.c_str());
                State.Close();
                return false;
            }

            DiskFrameHeader FrameHeader{};
            State.Stream.read(reinterpret_cast<char*>(&FrameHeader), sizeof(FrameHeader));
            uint64_t ExpectedPayloadBytes = 0;
            if (!State.Stream.good() || !ValidateFrameHeader(FrameHeader, State.Header, ExpectedPayloadBytes))
            {
                Error.Format("Corrupt MBO frame index in: %s", Path.c_str());
                State.Close();
                return false;
            }
            if (PreviousDateTime != 0.0 && FrameHeader.DateTime < PreviousDateTime)
            {
                Error.Format("Recorded MBO frames are not time ordered: %s", Path.c_str());
                State.Close();
                return false;
            }
            if (ExpectedPayloadBytes > FileSize - OffsetValue - sizeof(DiskFrameHeader))
            {
                Error.Format("Truncated MBO frame payload in: %s", Path.c_str());
                State.Close();
                return false;
            }

            FrameIndexEntry Entry;
            Entry.DateTime = FrameHeader.DateTime;
            Entry.FileOffset = OffsetValue;
            State.Index.push_back(Entry);
            PreviousDateTime = FrameHeader.DateTime;
            State.Stream.seekg(static_cast<std::streamoff>(ExpectedPayloadBytes), std::ios::cur);
            if (!State.Stream.good())
            {
                Error.Format("Corrupt MBO frame payload in: %s", Path.c_str());
                State.Close();
                return false;
            }
        }

        State.Stream.clear();
        State.IsLoaded = true;
        State.OpenPath = Path;
        if (State.Index.empty())
        {
            Error.Format("MBO recording contains no frames: %s", Path.c_str());
            State.Close();
            return false;
        }
        return true;
    }

    bool ReadReplayFrame(ReplayReaderState& State, const size_t Index, BookFrame& Frame, SCString& Error)
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
        return true;
    }

    int DistanceToTouch(const OrderKey& Key, const BookFrame& Frame)
    {
        if (Key.Side == SIDE_BID && Frame.BestBidInTicks != 0)
            return std::max(0, Frame.BestBidInTicks - Key.PriceInTicks);
        if (Key.Side == SIDE_ASK && Frame.BestAskInTicks != 0)
            return std::max(0, Key.PriceInTicks - Frame.BestAskInTicks);
        return std::numeric_limits<int>::max();
    }

    void PurgeBehaviorQueues(LevelBehavior& Behavior, const double Now, const DetectionConfiguration& Configuration)
    {
        while (!Behavior.SpoofLikeCancellationTimes.empty()
            && SecondsBetween(Now, Behavior.SpoofLikeCancellationTimes.front()) > Configuration.SpoofRepeatWindowSeconds)
        {
            Behavior.SpoofLikeCancellationTimes.pop_front();
        }

        const double SyntheticWindowSeconds = std::max(1.0, Configuration.SyntheticReplacementMilliseconds / 1000.0 * 4.0);
        while (!Behavior.RecentDepletedOrders.empty()
            && SecondsBetween(Now, Behavior.RecentDepletedOrders.front().DateTime) > SyntheticWindowSeconds)
        {
            Behavior.RecentDepletedOrders.pop_front();
        }

        if (Behavior.IcebergUntil < Now)
        {
            Behavior.IcebergKind = ICEBERG_NONE;
            Behavior.IcebergStrength = 0;
        }
        if (Behavior.SuppressUntil < Now)
            Behavior.SpoofScore = static_cast<int>(Behavior.SpoofLikeCancellationTimes.size()) * 20;
    }

    void MarkIceberg(
        LevelBehavior& Behavior,
        const int Kind,
        const int Strength,
        const double Now,
        const DetectionConfiguration& Configuration)
    {
        Behavior.IcebergKind = Kind;
        Behavior.IcebergStrength = std::max(Behavior.IcebergStrength, Strength);
        Behavior.IcebergUntil = std::max(
            Behavior.IcebergUntil,
            Now + static_cast<double>(Configuration.IcebergSignalPersistenceSeconds) / SECONDS_PER_DAY);
    }

    struct CurrentOrderObservation
    {
        const BookOrder* Order = nullptr;
        uint64_t VisibleQuantityAhead = 0;
    };

    uint64_t EstimateExecutionAtTrackedOrder(
        const uint64_t ExecutedAtPrice,
        const uint64_t VisibleQuantityAhead)
    {
        return ExecutedAtPrice > VisibleQuantityAhead
            ? ExecutedAtPrice - VisibleQuantityAhead
            : 0;
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

    void ProcessDetectionFrame(
        DetectionEngine& Engine,
        const BookFrame& Frame,
        const DetectionConfiguration& Configuration,
        std::vector<DetectionEvent>& Events)
    {
        const double Now = Frame.DateTime;

        for (const TradeEvent& Trade : Frame.Trades)
        {
            LevelKey Level{Trade.Side, Trade.PriceInTicks};
            Engine.Levels[Level].TotalExecuted += Trade.Volume;
        }

        for (auto& Pair : Engine.Levels)
            PurgeBehaviorQueues(Pair.second, Now, Configuration);

        std::map<OrderKey, CurrentOrderObservation> CurrentOrders;
        for (const BookLevel& Level : Frame.Levels)
        {
            uint64_t VisibleQuantityAhead = 0;
            for (const BookOrder& Order : Level.Orders)
            {
                CurrentOrderObservation Observation;
                Observation.Order = &Order;
                Observation.VisibleQuantityAhead = VisibleQuantityAhead;
                CurrentOrders[Order.Key] = Observation;
                VisibleQuantityAhead += Order.Quantity;
            }
        }

        // First process orders which disappeared from the current snapshot.
        for (auto Iterator = Engine.ActiveOrders.begin(); Iterator != Engine.ActiveOrders.end();)
        {
            if (CurrentOrders.find(Iterator->first) != CurrentOrders.end())
            {
                ++Iterator;
                continue;
            }

            const OrderKey Key = Iterator->first;
            const OrderLife Life = Iterator->second;

            // A disappearance outside the currently captured top-N depth range
            // is ambiguous: the order may still exist but simply moved beyond
            // the configured snapshot window. Drop tracking without calling it
            // a cancellation, spoof-like event, or synthetic refill candidate.
            if (!IsPriceInsideCapturedSideRange(Key, Frame))
            {
                Iterator = Engine.ActiveOrders.erase(Iterator);
                continue;
            }

            LevelBehavior& Behavior = Engine.Levels[LevelKey{Key.Side, Key.PriceInTicks}];
            const uint64_t ExecutedSinceLastAtPrice = Behavior.TotalExecuted >= Life.LastExecutedCounter
                ? Behavior.TotalExecuted - Life.LastExecutedCounter
                : 0;
            const uint64_t ExecutedDuringLife = Life.EstimatedExecutedAtOrder
                + EstimateExecutionAtTrackedOrder(ExecutedSinceLastAtPrice, Life.LastVisibleQuantityAhead);
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
                Behavior.SpoofLikeCancellationTimes.push_back(Now);
                while (!Behavior.SpoofLikeCancellationTimes.empty()
                    && SecondsBetween(Now, Behavior.SpoofLikeCancellationTimes.front()) > Configuration.SpoofRepeatWindowSeconds)
                {
                    Behavior.SpoofLikeCancellationTimes.pop_front();
                }

                Behavior.SpoofScore = std::min(100, static_cast<int>(Behavior.SpoofLikeCancellationTimes.size()) * 25);
                if (static_cast<int>(Behavior.SpoofLikeCancellationTimes.size()) >= Configuration.SpoofRequiredCancellations)
                {
                    const bool WasAlreadySuppressed = Behavior.SuppressUntil >= Now;
                    Behavior.SuppressUntil = std::max(
                        Behavior.SuppressUntil,
                        Now + static_cast<double>(Configuration.SpoofSuppressionSeconds) / SECONDS_PER_DAY);
                    if (!WasAlreadySuppressed)
                    {
                        DetectionEvent Event;
                        Event.Type = 2;
                        Event.Level = LevelKey{Key.Side, Key.PriceInTicks};
                        Event.Strength = Behavior.SpoofScore;
                        Event.Message.Format(
                            "SPOOF-LIKE cancellation cluster %s at %.8g | score %d",
                            Key.Side == SIDE_BID ? "BID" : "ASK",
                            static_cast<double>(Key.PriceInTicks),
                            Behavior.SpoofScore);
                        Events.push_back(Event);
                    }
                }
            }
            else if (ExecutedDuringLife >= Configuration.MinimumIcebergExecution)
            {
                RemovedOrderCandidate Candidate;
                Candidate.DateTime = Now;
                Candidate.Quantity = Life.LastQuantity;
                Candidate.ExecutedDuringLife = ExecutedDuringLife;
                Behavior.RecentDepletedOrders.push_back(Candidate);
            }

            Iterator = Engine.ActiveOrders.erase(Iterator);
        }

        // Then process new and continuing orders.
        for (const auto& Pair : CurrentOrders)
        {
            const OrderKey& Key = Pair.first;
            const CurrentOrderObservation& Observation = Pair.second;
            const BookOrder& Current = *Observation.Order;
            const LevelKey LevelKeyValue{Key.Side, Key.PriceInTicks};
            LevelBehavior& Behavior = Engine.Levels[LevelKeyValue];
            auto LifeIterator = Engine.ActiveOrders.find(Key);

            if (LifeIterator == Engine.ActiveOrders.end())
            {
                OrderLife Life;
                Life.FirstSeen = Now;
                Life.LastSeen = Now;
                Life.InitialQuantity = Current.Quantity;
                Life.LastQuantity = Current.Quantity;
                Life.MaximumQuantity = Current.Quantity;
                Life.LastExecutedCounter = Behavior.TotalExecuted;
                Life.LastVisibleQuantityAhead = Observation.VisibleQuantityAhead;
                Life.EstimatedExecutedAtOrder = 0;
                Life.MinimumDistanceToTouchTicks = DistanceToTouch(Key, Frame);
                Engine.ActiveOrders[Key] = Life;

                if (Configuration.EnableSyntheticIceberg && !Behavior.RecentDepletedOrders.empty())
                {
                    for (auto CandidateIterator = Behavior.RecentDepletedOrders.begin(); CandidateIterator != Behavior.RecentDepletedOrders.end(); ++CandidateIterator)
                    {
                        const double DelayMilliseconds = MillisecondsBetween(Now, CandidateIterator->DateTime);
                        const uint64_t Larger = std::max(CandidateIterator->Quantity, Current.Quantity);
                        const uint64_t Difference = Larger - std::min(CandidateIterator->Quantity, Current.Quantity);
                        const double SizeDifferenceFraction = Larger == 0 ? 1.0 : static_cast<double>(Difference) / static_cast<double>(Larger);
                        if (DelayMilliseconds >= 0.0
                            && DelayMilliseconds <= Configuration.SyntheticReplacementMilliseconds
                            && SizeDifferenceFraction <= Configuration.SyntheticSizeToleranceFraction)
                        {
                            const bool WasActive = Behavior.IcebergUntil >= Now && Behavior.IcebergKind == ICEBERG_SYNTHETIC;
                            MarkIceberg(Behavior, ICEBERG_SYNTHETIC, 2, Now, Configuration);
                            Behavior.RecentDepletedOrders.erase(CandidateIterator);
                            if (!WasActive)
                            {
                                DetectionEvent Event;
                                Event.Type = 1;
                                Event.Level = LevelKeyValue;
                                Event.IcebergKind = ICEBERG_SYNTHETIC;
                                Event.Strength = 2;
                                Event.Message.Format("PROBABLE SYNTHETIC ICEBERG %s at price ticks %d", Key.Side == SIDE_BID ? "BID" : "ASK", Key.PriceInTicks);
                                Events.push_back(Event);
                            }
                            break;
                        }
                    }
                }
                continue;
            }

            OrderLife& Life = LifeIterator->second;
            const uint64_t ExecutedSinceLastAtPrice = Behavior.TotalExecuted >= Life.LastExecutedCounter
                ? Behavior.TotalExecuted - Life.LastExecutedCounter
                : 0;
            const uint64_t EstimatedExecutedAtOrder = EstimateExecutionAtTrackedOrder(
                ExecutedSinceLastAtPrice, Life.LastVisibleQuantityAhead);
            const uint64_t QuantityIncrease = Current.Quantity > Life.LastQuantity
                ? Current.Quantity - Life.LastQuantity
                : 0;

            bool IcebergTriggered = false;
            int TriggeredKind = ICEBERG_NONE;
            int Strength = 0;

            if (EstimatedExecutedAtOrder >= Configuration.MinimumIcebergExecution
                && QuantityIncrease >= Configuration.MinimumNativeRefreshQuantity)
            {
                ++Life.RefreshObservations;
                Life.CumulativeRefresh += QuantityIncrease;
                IcebergTriggered = true;
                TriggeredKind = ICEBERG_NATIVE_ID;
                Strength = std::min(5, 2 + Life.RefreshObservations);
            }
            else if (Configuration.EnablePersistentQuantityHeuristic
                && EstimatedExecutedAtOrder >= Configuration.MinimumIcebergExecution
                && Life.LastQuantity > 0
                && static_cast<double>(Current.Quantity) >= static_cast<double>(Life.LastQuantity) * Configuration.PersistentQuantityMinimumFraction)
            {
                IcebergTriggered = true;
                TriggeredKind = ICEBERG_PERSISTENT_REFILL;
                Strength = 1;
            }

            if (IcebergTriggered)
            {
                const bool WasSameActiveKind = Behavior.IcebergUntil >= Now && Behavior.IcebergKind == TriggeredKind;
                MarkIceberg(Behavior, TriggeredKind, Strength, Now, Configuration);
                if (!WasSameActiveKind)
                {
                    DetectionEvent Event;
                    Event.Type = 1;
                    Event.Level = LevelKeyValue;
                    Event.IcebergKind = TriggeredKind;
                    Event.Strength = Strength;
                    Event.Message.Format(
                        "%s %s at price ticks %d | execution past visible queue %llu | visible %llu -> %llu",
                        TriggeredKind == ICEBERG_NATIVE_ID ? "SAME-ID REFILL" : "PERSISTENT REFILL HEURISTIC",
                        Key.Side == SIDE_BID ? "BID" : "ASK",
                        Key.PriceInTicks,
                        static_cast<unsigned long long>(EstimatedExecutedAtOrder),
                        static_cast<unsigned long long>(Life.LastQuantity),
                        static_cast<unsigned long long>(Current.Quantity));
                    Events.push_back(Event);
                }
            }

            Life.EstimatedExecutedAtOrder += EstimatedExecutedAtOrder;
            Life.LastSeen = Now;
            Life.LastQuantity = Current.Quantity;
            Life.MaximumQuantity = std::max(Life.MaximumQuantity, Current.Quantity);
            Life.LastExecutedCounter = Behavior.TotalExecuted;
            Life.LastVisibleQuantityAhead = Observation.VisibleQuantityAhead;
            Life.MinimumDistanceToTouchTicks = std::min(Life.MinimumDistanceToTouchTicks, DistanceToTouch(Key, Frame));
        }

        Engine.LastFrameDateTime = Now;
        Engine.Initialized = true;
    }

    bool IsOrderSpoofSuppressed(
        const OrderKey& Key,
        const DetectionEngine& Engine,
        const DetectionConfiguration& Configuration,
        const double Now)
    {
        const auto BehaviorIterator = Engine.Levels.find(LevelKey{Key.Side, Key.PriceInTicks});
        if (BehaviorIterator == Engine.Levels.end() || BehaviorIterator->second.SuppressUntil < Now)
            return false;

        const auto LifeIterator = Engine.ActiveOrders.find(Key);
        if (LifeIterator == Engine.ActiveOrders.end())
            return true;

        const OrderLife& Life = LifeIterator->second;
        const uint64_t TotalExecuted = BehaviorIterator->second.TotalExecuted;
        const uint64_t ExecutedSinceLastAtPrice = TotalExecuted >= Life.LastExecutedCounter
            ? TotalExecuted - Life.LastExecutedCounter
            : 0;
        const uint64_t ExecutedDuringLife = Life.EstimatedExecutedAtOrder
            + EstimateExecutionAtTrackedOrder(ExecutedSinceLastAtPrice, Life.LastVisibleQuantityAhead);
        const double ExecutedFraction = Life.MaximumQuantity == 0
            ? 0.0
            : static_cast<double>(ExecutedDuringLife) / static_cast<double>(Life.MaximumQuantity);
        const double AgeMilliseconds = MillisecondsBetween(Now, Life.FirstSeen);

        if (Life.MaximumQuantity < Configuration.SpoofMinimumQuantity)
            return false;

        // A quote that survives beyond the short-lived spoof window or receives
        // meaningful execution graduates out of suppression.
        return AgeMilliseconds <= Configuration.SpoofMaximumLifetimeMilliseconds * 2.0
            && ExecutedFraction <= std::max(0.25, Configuration.SpoofMaximumExecutedFraction * 2.0);
    }

    void BuildDisplayRows(
        const BookFrame& Frame,
        const DetectionEngine& Engine,
        const DetectionConfiguration& Detection,
        const uint64_t MinimumAggregateDepthQuantity,
        const uint64_t MinimumIndividualMboQuantity,
        const int SpoofMode,
        const int MaximumOrdersInText,
        std::vector<DisplayRow>& Rows)
    {
        Rows.clear();
        for (const BookLevel& Level : Frame.Levels)
        {
            DisplayRow Row;
            Row.Level = Level.Key;
            Row.AggregateQuantity = Level.AggregateQuantity;
            Row.HasMboOrders = !Level.Orders.empty();

            bool AnySuppressed = false;
            for (const BookOrder& Order : Level.Orders)
            {
                const bool Suppressed = IsOrderSpoofSuppressed(Order.Key, Engine, Detection, Frame.DateTime);
                AnySuppressed = AnySuppressed || Suppressed;
                if (Suppressed && SpoofMode != SPOOF_SHOW)
                    Row.SuppressedMboQuantity += Order.Quantity;

                if (Order.Quantity < MinimumIndividualMboQuantity)
                    continue;

                Row.LargeMboQuantity += Order.Quantity;
                if (!Suppressed || SpoofMode == SPOOF_SHOW)
                {
                    Row.TrustedLargeQuantity += Order.Quantity;
                    Row.TrustedOrderQuantities.push_back(Order.Quantity);
                }
            }

            Row.TrustedAggregateQuantity = Row.AggregateQuantity > Row.SuppressedMboQuantity
                ? Row.AggregateQuantity - Row.SuppressedMboQuantity
                : 0;

            const auto BehaviorIterator = Engine.Levels.find(Level.Key);
            if (BehaviorIterator != Engine.Levels.end())
            {
                const LevelBehavior& Behavior = BehaviorIterator->second;
                Row.IcebergActive = Behavior.IcebergUntil >= Frame.DateTime;
                Row.IcebergKind = Row.IcebergActive ? Behavior.IcebergKind : ICEBERG_NONE;
                Row.IcebergStrength = Row.IcebergActive ? Behavior.IcebergStrength : 0;
                Row.SpoofScore = Behavior.SpoofScore;
                Row.SpoofSuppressed = Behavior.SuppressUntil >= Frame.DateTime && AnySuppressed;
            }

            std::sort(Row.TrustedOrderQuantities.begin(), Row.TrustedOrderQuantities.end(), std::greater<uint64_t>());
            if (static_cast<int>(Row.TrustedOrderQuantities.size()) > MaximumOrdersInText)
                Row.TrustedOrderQuantities.resize(static_cast<size_t>(MaximumOrdersInText));

            const bool RawAggregateQualifies = Row.AggregateQuantity >= MinimumAggregateDepthQuantity;
            const bool TrustedAggregateQualifies = Row.TrustedAggregateQuantity >= MinimumAggregateDepthQuantity;
            const bool HasTrustedLargeMbo = Row.TrustedLargeQuantity >= MinimumIndividualMboQuantity;
            const bool ShouldDisplay = SpoofMode == SPOOF_HIDE
                ? (TrustedAggregateQualifies || HasTrustedLargeMbo || Row.IcebergActive)
                : (RawAggregateQualifies || HasTrustedLargeMbo || Row.IcebergActive);

            if (ShouldDisplay)
                Rows.push_back(Row);
        }

        std::stable_sort(Rows.begin(), Rows.end(), [](const DisplayRow& Left, const DisplayRow& Right)
        {
            if (Left.Level.Side != Right.Level.Side)
                return Left.Level.Side > Right.Level.Side; // bids before asks internally.
            if (Left.Level.Side == SIDE_BID)
                return Left.Level.PriceInTicks > Right.Level.PriceInTicks;
            return Left.Level.PriceInTicks < Right.Level.PriceInTicks;
        });
    }

    uint64_t ComputeDisplayHash(const std::vector<DisplayRow>& Rows, const VisualConfiguration& Visual)
    {
        uint64_t Hash = 1469598103934665603ULL;
        HashMix(Hash, static_cast<uint64_t>(Visual.SpoofMode));
        HashMix(Hash, static_cast<uint64_t>(Visual.DrawLargeLevelLines));
        HashMix(Hash, static_cast<uint64_t>(Visual.DrawOnlyIcebergLines));
        HashMix(Hash, static_cast<uint64_t>(Visual.DisplayLinePrice));
        HashMix(Hash, static_cast<uint64_t>(Visual.NormalLineWidth));
        HashMix(Hash, static_cast<uint64_t>(Visual.IcebergLineWidth));
        HashMix(Hash, static_cast<uint64_t>(Visual.BidColor));
        HashMix(Hash, static_cast<uint64_t>(Visual.AskColor));
        HashMix(Hash, static_cast<uint64_t>(Visual.BidIcebergColor));
        HashMix(Hash, static_cast<uint64_t>(Visual.AskIcebergColor));
        HashMix(Hash, static_cast<uint64_t>(Visual.SpoofColor));
        for (const DisplayRow& Row : Rows)
        {
            HashMix(Hash, static_cast<uint64_t>(Row.Level.Side + 2));
            HashMix(Hash, static_cast<uint64_t>(static_cast<uint32_t>(Row.Level.PriceInTicks)));
            HashMix(Hash, Row.AggregateQuantity);
            HashMix(Hash, Row.TrustedAggregateQuantity);
            HashMix(Hash, Row.SuppressedMboQuantity);
            HashMix(Hash, Row.TrustedLargeQuantity);
            HashMix(Hash, static_cast<uint64_t>(Row.IcebergKind));
            HashMix(Hash, static_cast<uint64_t>(Row.IcebergStrength));
            HashMix(Hash, static_cast<uint64_t>(Row.SpoofSuppressed));
            for (uint64_t Quantity : Row.TrustedOrderQuantities)
                HashMix(Hash, Quantity);
        }
        return Hash;
    }

    uint64_t DisplayedPrimaryQuantity(const DisplayRow& Row, const VisualConfiguration& Visual)
    {
        if (Visual.SpoofMode == SPOOF_SHOW)
            return Row.AggregateQuantity;
        return Row.TrustedAggregateQuantity;
    }

    COLORREF RowColor(const DisplayRow& Row, const VisualConfiguration& Visual)
    {
        if (Row.IcebergActive)
            return Row.Level.Side == SIDE_BID ? Visual.BidIcebergColor : Visual.AskIcebergColor;
        if (Row.SpoofSuppressed && Visual.SpoofMode == SPOOF_DIM)
            return Visual.SpoofColor;
        return Row.Level.Side == SIDE_BID ? Visual.BidColor : Visual.AskColor;
    }

    void DeleteAllStudyDrawings(SCStudyInterfaceRef sc, IndicatorState& State)
    {
        for (const auto& Pair : State.DrawingLineNumbers)
        {
            if (Pair.second != 0)
                sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, Pair.second);
        }
        State.DrawingLineNumbers.clear();
        State.LastDrawingHash = 0;
    }

    void UpdateChartLines(SCStudyInterfaceRef sc, IndicatorState& State)
    {
        const uint64_t DisplayHash = ComputeDisplayHash(State.DisplayRows, State.Visual);
        if (DisplayHash == State.LastDrawingHash)
            return;

        std::set<LevelKey> ActiveLevels;
        if (State.Visual.DrawLargeLevelLines && sc.ArraySize > 0 && sc.TickSize > 0.0f)
        {
            for (const DisplayRow& Row : State.DisplayRows)
            {
                if (State.Visual.DrawOnlyIcebergLines && !Row.IcebergActive)
                    continue;

                ActiveLevels.insert(Row.Level);
                s_UseTool Tool;
                Tool.Clear();
                Tool.ChartNumber = sc.ChartNumber;
                Tool.DrawingType = DRAWING_HORIZONTAL_RAY;
                Tool.AddMethod = UTAM_ADD_OR_ADJUST;
                Tool.Region = 0;
                Tool.BeginIndex = sc.ArraySize - 1;
                Tool.BeginValue = static_cast<double>(Row.Level.PriceInTicks) * sc.TickSize;
                Tool.Color = RowColor(Row, State.Visual);
                Tool.LineWidth = Row.IcebergActive ? State.Visual.IcebergLineWidth : State.Visual.NormalLineWidth;
                Tool.LineStyle = Row.IcebergActive ? LINESTYLE_SOLID : LINESTYLE_DASH;
                Tool.DisplayHorizontalLineValue = State.Visual.DisplayLinePrice ? 1 : 0;
                Tool.AddAsUserDrawnDrawing = 0;

                const auto Existing = State.DrawingLineNumbers.find(Row.Level);
                if (Existing != State.DrawingLineNumbers.end())
                    Tool.LineNumber = Existing->second;

                if (sc.UseTool(Tool) != 0)
                    State.DrawingLineNumbers[Row.Level] = Tool.LineNumber;
            }
        }

        for (auto Iterator = State.DrawingLineNumbers.begin(); Iterator != State.DrawingLineNumbers.end();)
        {
            if (ActiveLevels.find(Iterator->first) == ActiveLevels.end())
            {
                if (Iterator->second != 0)
                    sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, Iterator->second);
                Iterator = State.DrawingLineNumbers.erase(Iterator);
            }
            else
            {
                ++Iterator;
            }
        }
        State.LastDrawingHash = DisplayHash;
    }

    std::string FormatRowText(const DisplayRow& Row, const VisualConfiguration& Visual)
    {
        std::ostringstream Text;
        if (Row.IcebergActive)
        {
            if (Row.IcebergKind == ICEBERG_NATIVE_ID)
                Text << "ICE-ID ";
            else if (Row.IcebergKind == ICEBERG_SYNTHETIC)
                Text << "ICE-S ";
            else
                Text << "REFILL ";
        }
        else if (Row.SpoofSuppressed && Visual.SpoofMode == SPOOF_DIM)
        {
            Text << "S? ";
        }

        const uint64_t DisplayQuantity = DisplayedPrimaryQuantity(Row, Visual);
        Text << DisplayQuantity;

        if (Visual.ShowAggregateDepth
            && Row.AggregateQuantity > 0
            && Row.AggregateQuantity != DisplayQuantity)
        {
            Text << "/" << Row.AggregateQuantity;
        }

        if (Visual.ShowIndividualOrders && !Row.TrustedOrderQuantities.empty())
        {
            Text << " [";
            for (size_t Index = 0; Index < Row.TrustedOrderQuantities.size(); ++Index)
            {
                if (Index != 0)
                    Text << ',';
                Text << Row.TrustedOrderQuantities[Index];
            }
            Text << ']';
        }
        return Text.str();
    }

    void DrawMboDomToChart(HWND WindowHandle, HDC DeviceContext, SCStudyInterfaceRef sc)
    {
        (void)WindowHandle;
        IndicatorState* State = static_cast<IndicatorState*>(sc.GetPersistentPointer(1));
        if (State == nullptr || State->DisplayRows.empty() || sc.TickSize <= 0.0f)
            return;

        const int BidLeft = sc.GetDOMColumnLeftCoordinate(n_ACSIL::DOM_COLUMN_GENERAL_PURPOSE_1);
        const int BidRight = sc.GetDOMColumnRightCoordinate(n_ACSIL::DOM_COLUMN_GENERAL_PURPOSE_1);
        const int AskLeft = sc.GetDOMColumnLeftCoordinate(n_ACSIL::DOM_COLUMN_GENERAL_PURPOSE_2);
        const int AskRight = sc.GetDOMColumnRightCoordinate(n_ACSIL::DOM_COLUMN_GENERAL_PURPOSE_2);
        if (BidLeft == 0 || BidRight == 0 || AskLeft == 0 || AskRight == 0)
            return;

        const int FontHeight = Clamp(State->Visual.FontHeight, 9, 32);
        HFONT Font = CreateFontA(
            -FontHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
        HGDIOBJ PriorFont = SelectObject(DeviceContext, Font);
        const int PriorBkMode = SetBkMode(DeviceContext, TRANSPARENT);
        const COLORREF PriorTextColor = SetTextColor(DeviceContext, State->Visual.TextColor);

        uint64_t MaximumBidQuantity = 1;
        uint64_t MaximumAskQuantity = 1;
        for (const DisplayRow& Row : State->DisplayRows)
        {
            const uint64_t Quantity = DisplayedPrimaryQuantity(Row, State->Visual);
            if (Row.Level.Side == SIDE_BID)
                MaximumBidQuantity = std::max(MaximumBidQuantity, Quantity);
            else
                MaximumAskQuantity = std::max(MaximumAskQuantity, Quantity);
        }

        const int HalfRowHeight = std::max(6, FontHeight / 2 + 2);
        for (const DisplayRow& Row : State->DisplayRows)
        {
            const double Price = static_cast<double>(Row.Level.PriceInTicks) * sc.TickSize;
            const int Y = sc.RegionValueToYPixelCoordinate(static_cast<float>(Price), 0);
            if (Y < sc.StudyRegionTopCoordinate - HalfRowHeight || Y > sc.StudyRegionBottomCoordinate + HalfRowHeight)
                continue;

            const int Left = Row.Level.Side == SIDE_BID ? BidLeft : AskLeft;
            const int Right = Row.Level.Side == SIDE_BID ? BidRight : AskRight;
            if (Right <= Left)
                continue;

            const uint64_t Quantity = DisplayedPrimaryQuantity(Row, State->Visual);
            const uint64_t SideMaximum = Row.Level.Side == SIDE_BID ? MaximumBidQuantity : MaximumAskQuantity;
            const double Fraction = SideMaximum == 0 ? 0.0 : std::min(1.0, static_cast<double>(Quantity) / static_cast<double>(SideMaximum));
            const int FullWidth = Right - Left;
            const int FilledWidth = std::max(2, static_cast<int>(std::floor(FullWidth * Fraction)));

            RECT Background;
            Background.top = Y - HalfRowHeight;
            Background.bottom = Y + HalfRowHeight;
            if (Row.Level.Side == SIDE_BID)
            {
                Background.left = Right - FilledWidth;
                Background.right = Right;
            }
            else
            {
                Background.left = Left;
                Background.right = Left + FilledWidth;
            }

            HBRUSH Brush = CreateSolidBrush(RowColor(Row, State->Visual));
            FillRect(DeviceContext, &Background, Brush);
            DeleteObject(Brush);

            const std::string Text = FormatRowText(Row, State->Visual);
            SIZE TextSize{};
            GetTextExtentPoint32A(DeviceContext, Text.c_str(), static_cast<int>(Text.size()), &TextSize);
            const int TextX = Row.Level.Side == SIDE_BID
                ? std::max(Left + 2, Right - static_cast<int>(TextSize.cx) - 3)
                : Left + 3;
            const int TextY = Y - TextSize.cy / 2;
            TextOutA(DeviceContext, TextX, TextY, Text.c_str(), static_cast<int>(Text.size()));
        }

        SetTextColor(DeviceContext, PriorTextColor);
        SetBkMode(DeviceContext, PriorBkMode);
        SelectObject(DeviceContext, PriorFont);
        DeleteObject(Font);
    }

    int ResolveDataSource(const int Requested, const int ReplayStatus)
    {
        if (Requested == DATA_SOURCE_AUTO)
            return ReplayStatus == REPLAY_STOPPED ? DATA_SOURCE_LIVE : DATA_SOURCE_RECORDED;
        return Requested;
    }

    size_t LowerBoundFrame(const std::vector<FrameIndexEntry>& Index, const double DateTime)
    {
        return static_cast<size_t>(std::lower_bound(
            Index.begin(), Index.end(), DateTime,
            [](const FrameIndexEntry& Entry, const double Value)
            {
                return Entry.DateTime < Value;
            }) - Index.begin());
    }

    bool AdvanceRecordedData(
        IndicatorState& State,
        const double TargetDateTime,
        const int ReconstructionLookbackSeconds,
        const DetectionConfiguration& Configuration,
        std::vector<DetectionEvent>& Events,
        SCString& Error)
    {
        if (!State.ReplayReader.IsLoaded)
            return false;

        const bool MovedBackwards = State.LastReplayTargetDateTime != 0.0
            && TargetDateTime + (0.001 / SECONDS_PER_DAY) < State.LastReplayTargetDateTime;
        const bool NeedsInitialReconstruction = State.LastReplayTargetDateTime == 0.0
            && State.NextReplayFrameIndex == 0
            && State.CurrentFrame.DateTime == 0.0;
        if (MovedBackwards || NeedsInitialReconstruction || State.NextReplayFrameIndex > State.ReplayReader.Index.size())
        {
            State.Engine.Reset();
            const double StartTime = TargetDateTime - static_cast<double>(ReconstructionLookbackSeconds) / SECONDS_PER_DAY;
            State.NextReplayFrameIndex = LowerBoundFrame(State.ReplayReader.Index, StartTime);
            State.CurrentFrame = BookFrame();
        }

        while (State.NextReplayFrameIndex < State.ReplayReader.Index.size()
            && State.ReplayReader.Index[State.NextReplayFrameIndex].DateTime <= TargetDateTime)
        {
            BookFrame Frame;
            if (!ReadReplayFrame(State.ReplayReader, State.NextReplayFrameIndex, Frame, Error))
                return false;
            ProcessDetectionFrame(State.Engine, Frame, Configuration, Events);
            State.CurrentFrame = Frame;
            ++State.NextReplayFrameIndex;
        }

        State.LastReplayTargetDateTime = TargetDateTime;
        return State.CurrentFrame.DateTime != 0.0;
    }

    RecorderConfiguration MakeRecorderConfiguration(
        SCStudyInterfaceRef sc,
        const SCString& FileName,
        const int FileMode,
        const int MinimumSnapshotIntervalMilliseconds,
        const int HeartbeatIntervalMilliseconds,
        const int FlushIntervalSeconds,
        const uint32_t MinimumRecordedOrderQuantity,
        const uint32_t MaximumDepthLevels)
    {
        RecorderConfiguration Configuration;
        Configuration.Path = BuildDataFilePath(sc, FileName);
        Configuration.FileMode = FileMode;
        Configuration.MinimumSnapshotIntervalMilliseconds = MinimumSnapshotIntervalMilliseconds;
        Configuration.HeartbeatIntervalMilliseconds = HeartbeatIntervalMilliseconds;
        Configuration.FlushIntervalSeconds = FlushIntervalSeconds;
        Configuration.MinimumRecordedOrderQuantity = MinimumRecordedOrderQuantity;
        Configuration.MaximumDepthLevels = MaximumDepthLevels;
        Configuration.TickSize = sc.TickSize;
        Configuration.Symbol = SanitizeSymbol(sc.Symbol);
        return Configuration;
    }
}

using namespace MboDom;

/*============================================================================
    Study 1: MBO DOM Intelligence
============================================================================*/
SCSFExport scsf_MBODOMIntelligence(SCStudyInterfaceRef sc)
{
    SCInputRef DataSource = sc.Input[0];
    SCInputRef RecordedFileName = sc.Input[1];
    SCInputRef RecordLiveData = sc.Input[2];
    SCInputRef RecordingFileMode = sc.Input[3];
    SCInputRef MaximumDepthLevels = sc.Input[4];
    SCInputRef MaximumOrdersPerPrice = sc.Input[5];
    SCInputRef MinimumRecordedOrderQuantity = sc.Input[6];
    SCInputRef MinimumAggregateDepthQuantity = sc.Input[7];
    SCInputRef MaximumOrdersInText = sc.Input[8];
    SCInputRef SpoofDisplay = sc.Input[9];

    SCInputRef MinimumIcebergExecution = sc.Input[10];
    SCInputRef MinimumNativeRefreshQuantity = sc.Input[11];
    SCInputRef EnablePersistentQuantityHeuristic = sc.Input[12];
    SCInputRef PersistentQuantityMinimumPercent = sc.Input[13];
    SCInputRef EnableSyntheticIceberg = sc.Input[14];
    SCInputRef SyntheticReplacementMilliseconds = sc.Input[15];
    SCInputRef SyntheticSizeTolerancePercent = sc.Input[16];
    SCInputRef IcebergSignalPersistenceSeconds = sc.Input[17];

    SCInputRef SpoofMinimumQuantity = sc.Input[18];
    SCInputRef SpoofMaximumLifetimeMilliseconds = sc.Input[19];
    SCInputRef SpoofMaximumExecutedPercent = sc.Input[20];
    SCInputRef SpoofMaximumDistanceToTouchTicks = sc.Input[21];
    SCInputRef SpoofRequiredCancellations = sc.Input[22];
    SCInputRef SpoofRepeatWindowSeconds = sc.Input[23];
    SCInputRef SpoofSuppressionSeconds = sc.Input[24];

    SCInputRef MinimumSnapshotIntervalMilliseconds = sc.Input[25];
    SCInputRef HeartbeatIntervalMilliseconds = sc.Input[26];
    SCInputRef FlushIntervalSeconds = sc.Input[27];
    SCInputRef ReplayReconstructionLookbackSeconds = sc.Input[28];

    SCInputRef ShowAggregateDepth = sc.Input[29];
    SCInputRef ShowIndividualOrders = sc.Input[30];
    SCInputRef DrawLargeLevelLines = sc.Input[31];
    SCInputRef DrawOnlyIcebergLines = sc.Input[32];
    SCInputRef DisplayLinePrice = sc.Input[33];
    SCInputRef NormalLineWidth = sc.Input[34];
    SCInputRef IcebergLineWidth = sc.Input[35];
    SCInputRef FontHeight = sc.Input[36];

    SCInputRef BidColor = sc.Input[37];
    SCInputRef AskColor = sc.Input[38];
    SCInputRef BidIcebergColor = sc.Input[39];
    SCInputRef AskIcebergColor = sc.Input[40];
    SCInputRef SpoofColor = sc.Input[41];
    SCInputRef TextColor = sc.Input[42];

    SCInputRef EnableIcebergAlerts = sc.Input[43];
    SCInputRef IcebergAlertNumber = sc.Input[44];
    SCInputRef EnableSpoofAlerts = sc.Input[45];
    SCInputRef SpoofAlertNumber = sc.Input[46];
    SCInputRef WriteEventsToLog = sc.Input[47];
    SCInputRef ReplayClock = sc.Input[48];
    SCInputRef MinimumIndividualMboQuantity = sc.Input[49];

    if (sc.SetDefaults)
    {
        sc.GraphName = "MBO DOM Intelligence - Large Orders, Refill/Iceberg, Spoof-Like Filter, Recorder/Replay";
        sc.StudyDescription = "Displays large aggregate bid/ask depth with individual MBO detail in Chart/Trade DOM General Purpose columns 1 and 2, highlights probable refill/iceberg activity, heuristically dims or subtracts repeated short-lived low-execution quote cancellations from a custom trusted total, and can record/read a custom MBO snapshot file for replay. Spoofing intent cannot be established from public order-book data.";
        sc.AutoLoop = 0;
        sc.GraphRegion = 0;
        sc.UpdateAlways = 1;
        sc.UsesMarketDepthData = 1;
        sc.FreeDLL = 0;
        sc.p_GDIFunction = DrawMboDomToChart;
        sc.AlertOnlyOncePerBar = 0;
        sc.ResetAlertOnNewBar = 0;

        DataSource.Name = "Data Source";
        DataSource.SetCustomInputStrings("Auto: Live unless chart replaying;Live MBO;Recorded MBO File");
        DataSource.SetCustomInputIndex(DATA_SOURCE_AUTO);

        RecordedFileName.Name = "Recording / Playback File Name (Data folder or absolute path)";
        RecordedFileName.SetString("MBO_Record.scmbo");

        RecordLiveData.Name = "Record Live Data From This Indicator";
        RecordLiveData.SetYesNo(0);

        RecordingFileMode.Name = "Recording File Mode";
        RecordingFileMode.SetCustomInputStrings("Off;Append / Create;Rewrite Once Then Append");
        RecordingFileMode.SetCustomInputIndex(1);

        MaximumDepthLevels.Name = "Maximum Depth Levels Per Side";
        MaximumDepthLevels.SetInt(20);
        MaximumDepthLevels.SetIntLimits(1, 200);

        MaximumOrdersPerPrice.Name = "Maximum MBO Orders Read Per Price";
        MaximumOrdersPerPrice.SetInt(200);
        MaximumOrdersPerPrice.SetIntLimits(1, 5000);

        MinimumRecordedOrderQuantity.Name = "Minimum MBO Order Quantity Captured / Recorded";
        MinimumRecordedOrderQuantity.SetInt(3);
        MinimumRecordedOrderQuantity.SetIntLimits(1, 1000000);

        MinimumAggregateDepthQuantity.Name = "DOM: Minimum Aggregate Depth Quantity To Display";
        MinimumAggregateDepthQuantity.SetInt(50);
        MinimumAggregateDepthQuantity.SetIntLimits(1, 1000000);

        MinimumIndividualMboQuantity.Name = "DOM: Minimum Individual MBO Quantity To List";
        MinimumIndividualMboQuantity.SetInt(20);
        MinimumIndividualMboQuantity.SetIntLimits(1, 1000000);

        MaximumOrdersInText.Name = "Maximum Individual Order Quantities Shown Per Price";
        MaximumOrdersInText.SetInt(4);
        MaximumOrdersInText.SetIntLimits(0, 20);

        SpoofDisplay.Name = "Suspected Spoof-Like Liquidity Display";
        SpoofDisplay.SetCustomInputStrings("Show Normally;Dim and Mark S?;Hide From Custom Trusted DOM");
        SpoofDisplay.SetCustomInputIndex(SPOOF_DIM);

        MinimumIcebergExecution.Name = "Iceberg: Minimum Executed Volume At Price Between Observations";
        MinimumIcebergExecution.SetInt(10);
        MinimumIcebergExecution.SetIntLimits(1, 1000000);

        MinimumNativeRefreshQuantity.Name = "Iceberg: Minimum Same-Order-ID Visible Quantity Increase";
        MinimumNativeRefreshQuantity.SetInt(3);
        MinimumNativeRefreshQuantity.SetIntLimits(1, 1000000);

        EnablePersistentQuantityHeuristic.Name = "Iceberg: Enable Persistent Visible Quantity Heuristic";
        EnablePersistentQuantityHeuristic.SetYesNo(1);

        PersistentQuantityMinimumPercent.Name = "Iceberg: Persistent Quantity Minimum % Of Prior Visible";
        PersistentQuantityMinimumPercent.SetFloat(80.0f);
        PersistentQuantityMinimumPercent.SetFloatLimits(1.0f, 100.0f);

        EnableSyntheticIceberg.Name = "Iceberg: Enable Synthetic Replacement Heuristic";
        EnableSyntheticIceberg.SetYesNo(1);

        SyntheticReplacementMilliseconds.Name = "Iceberg: Maximum Synthetic Replacement Delay (ms)";
        SyntheticReplacementMilliseconds.SetInt(500);
        SyntheticReplacementMilliseconds.SetIntLimits(10, 10000);

        SyntheticSizeTolerancePercent.Name = "Iceberg: Synthetic Replacement Size Tolerance %";
        SyntheticSizeTolerancePercent.SetFloat(20.0f);
        SyntheticSizeTolerancePercent.SetFloatLimits(0.0f, 100.0f);

        IcebergSignalPersistenceSeconds.Name = "Iceberg: Highlight Persistence Seconds";
        IcebergSignalPersistenceSeconds.SetInt(30);
        IcebergSignalPersistenceSeconds.SetIntLimits(1, 3600);

        SpoofMinimumQuantity.Name = "Spoof-Like: Minimum Order Quantity";
        SpoofMinimumQuantity.SetInt(50);
        SpoofMinimumQuantity.SetIntLimits(1, 1000000);

        SpoofMaximumLifetimeMilliseconds.Name = "Spoof-Like: Maximum Order Lifetime (ms)";
        SpoofMaximumLifetimeMilliseconds.SetInt(1000);
        SpoofMaximumLifetimeMilliseconds.SetIntLimits(10, 60000);

        SpoofMaximumExecutedPercent.Name = "Spoof-Like: Maximum Executed Volume / Order Size %";
        SpoofMaximumExecutedPercent.SetFloat(10.0f);
        SpoofMaximumExecutedPercent.SetFloatLimits(0.0f, 100.0f);

        SpoofMaximumDistanceToTouchTicks.Name = "Spoof-Like: Maximum Distance From Touch (ticks)";
        SpoofMaximumDistanceToTouchTicks.SetInt(3);
        SpoofMaximumDistanceToTouchTicks.SetIntLimits(0, 100);

        SpoofRequiredCancellations.Name = "Spoof-Like: Repeated Cancellations Required At Price";
        SpoofRequiredCancellations.SetInt(3);
        SpoofRequiredCancellations.SetIntLimits(1, 20);

        SpoofRepeatWindowSeconds.Name = "Spoof-Like: Repetition Window Seconds";
        SpoofRepeatWindowSeconds.SetInt(10);
        SpoofRepeatWindowSeconds.SetIntLimits(1, 600);

        SpoofSuppressionSeconds.Name = "Spoof-Like: Trusted-DOM Suppression Seconds";
        SpoofSuppressionSeconds.SetInt(5);
        SpoofSuppressionSeconds.SetIntLimits(1, 600);

        MinimumSnapshotIntervalMilliseconds.Name = "Recorder: Minimum Snapshot Interval (ms)";
        MinimumSnapshotIntervalMilliseconds.SetInt(50);
        MinimumSnapshotIntervalMilliseconds.SetIntLimits(10, 5000);

        HeartbeatIntervalMilliseconds.Name = "Recorder: Unchanged-Book Heartbeat Interval (ms)";
        HeartbeatIntervalMilliseconds.SetInt(1000);
        HeartbeatIntervalMilliseconds.SetIntLimits(50, 60000);

        FlushIntervalSeconds.Name = "Recorder: File Flush Interval Seconds";
        FlushIntervalSeconds.SetInt(1);
        FlushIntervalSeconds.SetIntLimits(1, 60);

        ReplayReconstructionLookbackSeconds.Name = "Replay: Detection State Reconstruction Lookback Seconds";
        ReplayReconstructionLookbackSeconds.SetInt(20);
        ReplayReconstructionLookbackSeconds.SetIntLimits(1, 600);

        ReplayClock.Name = "Replay: Playback Clock";
        ReplayClock.SetCustomInputStrings("Latest Chart Data Record (synchronized);Replay Timer (can lead at accelerated speed)");
        ReplayClock.SetCustomInputIndex(REPLAY_CLOCK_LATEST_CHART_RECORD);

        ShowAggregateDepth.Name = "DOM Text: Show Aggregate Depth After Slash";
        ShowAggregateDepth.SetYesNo(1);

        ShowIndividualOrders.Name = "DOM Text: Show Individual Large MBO Quantities";
        ShowIndividualOrders.SetYesNo(1);

        DrawLargeLevelLines.Name = "Chart: Draw Horizontal Lines At Displayed Levels";
        DrawLargeLevelLines.SetYesNo(1);

        DrawOnlyIcebergLines.Name = "Chart: Draw Lines Only For Active Iceberg/Refill Levels";
        DrawOnlyIcebergLines.SetYesNo(0);

        DisplayLinePrice.Name = "Chart: Display Price On Lines";
        DisplayLinePrice.SetYesNo(1);

        NormalLineWidth.Name = "Chart: Normal Large-Level Line Width";
        NormalLineWidth.SetInt(1);
        NormalLineWidth.SetIntLimits(1, 10);

        IcebergLineWidth.Name = "Chart: Iceberg/Refill Line Width";
        IcebergLineWidth.SetInt(3);
        IcebergLineWidth.SetIntLimits(1, 10);

        FontHeight.Name = "DOM Column Font Height";
        FontHeight.SetInt(14);
        FontHeight.SetIntLimits(9, 32);

        BidColor.Name = "Large Bid Color";
        BidColor.SetColor(RGB(0, 135, 230));
        AskColor.Name = "Large Ask Color";
        AskColor.SetColor(RGB(225, 80, 70));
        BidIcebergColor.Name = "Bid Iceberg / Refill Color";
        BidIcebergColor.SetColor(RGB(0, 230, 110));
        AskIcebergColor.Name = "Ask Iceberg / Refill Color";
        AskIcebergColor.SetColor(RGB(255, 170, 0));
        SpoofColor.Name = "Spoof-Like Dim Color";
        SpoofColor.SetColor(RGB(125, 125, 125));
        TextColor.Name = "DOM Text Color";
        TextColor.SetColor(RGB(245, 245, 245));

        EnableIcebergAlerts.Name = "Enable Iceberg / Refill Alerts";
        EnableIcebergAlerts.SetYesNo(0);
        IcebergAlertNumber.Name = "Iceberg / Refill Alert Number";
        IcebergAlertNumber.SetInt(1);
        IcebergAlertNumber.SetIntLimits(1, 150);

        EnableSpoofAlerts.Name = "Enable Spoof-Like Cluster Alerts";
        EnableSpoofAlerts.SetYesNo(0);
        SpoofAlertNumber.Name = "Spoof-Like Cluster Alert Number";
        SpoofAlertNumber.SetInt(2);
        SpoofAlertNumber.SetIntLimits(1, 150);

        WriteEventsToLog.Name = "Write Detection Events To Message Log";
        WriteEventsToLog.SetYesNo(1);
        return;
    }

    IndicatorState* State = static_cast<IndicatorState*>(sc.GetPersistentPointer(1));
    if (sc.LastCallToFunction)
    {
        if (State != nullptr)
        {
            DeleteAllStudyDrawings(sc, *State);
            State->Recorder.Close();
            State->ReplayReader.Close();
            delete State;
            sc.SetPersistentPointer(1, nullptr);
        }
        return;
    }

    if (State == nullptr)
    {
        State = new IndicatorState;
        sc.SetPersistentPointer(1, State);
    }

    if (sc.IsFullRecalculation)
    {
        DeleteAllStudyDrawings(sc, *State);
        State->Engine.Reset();
        State->LiveBaselineEstablished = false;
        State->LastTimeAndSalesSequence = 0;
        State->NextReplayFrameIndex = 0;
        State->LastReplayTargetDateTime = 0.0;
        State->CurrentFrame = BookFrame();
        State->DisplayRows.clear();
    }

    const int MaxDepth = Clamp(MaximumDepthLevels.GetInt(), 1, 200);
    const int MaxOrders = Clamp(MaximumOrdersPerPrice.GetInt(), 1, 5000);
    const uint64_t MinRecordedQuantity = static_cast<uint64_t>(Clamp(MinimumRecordedOrderQuantity.GetInt(), 1, 1000000));
    const uint64_t MinAggregateQuantity = static_cast<uint64_t>(Clamp(MinimumAggregateDepthQuantity.GetInt(), 1, 1000000));
    const uint64_t MinIndividualMboQuantity = static_cast<uint64_t>(Clamp(MinimumIndividualMboQuantity.GetInt(), 1, 1000000));
    const int MaxOrdersText = Clamp(MaximumOrdersInText.GetInt(), 0, 20);

    DetectionConfiguration Detection;
    Detection.MinimumIcebergExecution = static_cast<uint64_t>(Clamp(MinimumIcebergExecution.GetInt(), 1, 1000000));
    Detection.MinimumNativeRefreshQuantity = static_cast<uint64_t>(Clamp(MinimumNativeRefreshQuantity.GetInt(), 1, 1000000));
    Detection.EnablePersistentQuantityHeuristic = EnablePersistentQuantityHeuristic.GetYesNo() != 0;
    Detection.PersistentQuantityMinimumFraction = Clamp(PersistentQuantityMinimumPercent.GetFloat() / 100.0, 0.01, 1.0);
    Detection.EnableSyntheticIceberg = EnableSyntheticIceberg.GetYesNo() != 0;
    Detection.SyntheticReplacementMilliseconds = Clamp(SyntheticReplacementMilliseconds.GetInt(), 10, 10000);
    Detection.SyntheticSizeToleranceFraction = Clamp(SyntheticSizeTolerancePercent.GetFloat() / 100.0, 0.0, 1.0);
    Detection.IcebergSignalPersistenceSeconds = Clamp(IcebergSignalPersistenceSeconds.GetInt(), 1, 3600);
    Detection.SpoofMinimumQuantity = static_cast<uint64_t>(Clamp(SpoofMinimumQuantity.GetInt(), 1, 1000000));
    Detection.SpoofMaximumLifetimeMilliseconds = Clamp(SpoofMaximumLifetimeMilliseconds.GetInt(), 10, 60000);
    Detection.SpoofMaximumExecutedFraction = Clamp(SpoofMaximumExecutedPercent.GetFloat() / 100.0, 0.0, 1.0);
    Detection.SpoofMaximumDistanceToTouchTicks = Clamp(SpoofMaximumDistanceToTouchTicks.GetInt(), 0, 100);
    Detection.SpoofRequiredCancellations = Clamp(SpoofRequiredCancellations.GetInt(), 1, 20);
    Detection.SpoofRepeatWindowSeconds = Clamp(SpoofRepeatWindowSeconds.GetInt(), 1, 600);
    Detection.SpoofSuppressionSeconds = Clamp(SpoofSuppressionSeconds.GetInt(), 1, 600);

    State->Visual.BidColor = BidColor.GetColor();
    State->Visual.AskColor = AskColor.GetColor();
    State->Visual.BidIcebergColor = BidIcebergColor.GetColor();
    State->Visual.AskIcebergColor = AskIcebergColor.GetColor();
    State->Visual.SpoofColor = SpoofColor.GetColor();
    State->Visual.TextColor = TextColor.GetColor();
    State->Visual.FontHeight = Clamp(FontHeight.GetInt(), 9, 32);
    State->Visual.SpoofMode = Clamp(static_cast<int>(SpoofDisplay.GetIndex()), static_cast<int>(SPOOF_SHOW), static_cast<int>(SPOOF_HIDE));
    State->Visual.ShowAggregateDepth = ShowAggregateDepth.GetYesNo() != 0;
    State->Visual.ShowIndividualOrders = ShowIndividualOrders.GetYesNo() != 0;
    State->Visual.MaximumOrdersInText = MaxOrdersText;
    State->Visual.DrawLargeLevelLines = DrawLargeLevelLines.GetYesNo() != 0;
    State->Visual.DrawOnlyIcebergLines = DrawOnlyIcebergLines.GetYesNo() != 0;
    State->Visual.DisplayLinePrice = DisplayLinePrice.GetYesNo() != 0;
    State->Visual.NormalLineWidth = Clamp(NormalLineWidth.GetInt(), 1, 10);
    State->Visual.IcebergLineWidth = Clamp(IcebergLineWidth.GetInt(), 1, 10);

    const bool HasBidCustomColumn = sc.GetDOMColumnLeftCoordinate(n_ACSIL::DOM_COLUMN_GENERAL_PURPOSE_1) != 0
        && sc.GetDOMColumnRightCoordinate(n_ACSIL::DOM_COLUMN_GENERAL_PURPOSE_1) != 0;
    const bool HasAskCustomColumn = sc.GetDOMColumnLeftCoordinate(n_ACSIL::DOM_COLUMN_GENERAL_PURPOSE_2) != 0
        && sc.GetDOMColumnRightCoordinate(n_ACSIL::DOM_COLUMN_GENERAL_PURPOSE_2) != 0;
    if ((!HasBidCustomColumn || !HasAskCustomColumn) && !State->LoggedNoDomColumns)
    {
        sc.AddMessageToLog("MBO DOM Intelligence: add General Purpose 1 and General Purpose 2 through Trade > Customize Chart/Trade DOM Columns for the custom bid/ask display.", 1);
        State->LoggedNoDomColumns = true;
    }
    else if (HasBidCustomColumn && HasAskCustomColumn)
    {
        State->LoggedNoDomColumns = false;
    }

    const int ResolvedSource = ResolveDataSource(DataSource.GetIndex(), sc.ReplayStatus);
    if (ResolvedSource != State->LastDataSource)
    {
        State->Engine.Reset();
        State->CurrentFrame = BookFrame();
        State->NextReplayFrameIndex = 0;
        State->LastReplayTargetDateTime = 0.0;
        State->LiveBaselineEstablished = false;
        State->LastTimeAndSalesSequence = 0;
        State->LastDataSource = ResolvedSource;
    }

    std::vector<DetectionEvent> Events;
    bool HaveFrame = false;

    if (ResolvedSource == DATA_SOURCE_LIVE)
    {
        bool AnyMboSeen = false;
        BookFrame Frame;
        if (CaptureLiveFrame(sc, State->LastTimeAndSalesSequence, State->LiveBaselineEstablished,
            MaxDepth, MaxOrders, MinRecordedQuantity, Frame, AnyMboSeen))
        {
            ProcessDetectionFrame(State->Engine, Frame, Detection, Events);
            State->CurrentFrame = Frame;
            HaveFrame = true;
        }

        if (HaveFrame && !AnyMboSeen && !State->LoggedNoMbo && sc.ReplayStatus == REPLAY_STOPPED)
        {
            sc.AddMessageToLog("MBO DOM Intelligence: aggregate depth is present but no MBO orders were returned. Verify Service Package 12, supported Sierra feed, MBO subscription settings, and use on a normal chart with Trading Chart DOM enabled.", 1);
            State->LoggedNoMbo = true;
        }
        if (AnyMboSeen)
            State->LoggedNoMbo = false;

        const int FileMode = RecordLiveData.GetYesNo() != 0 ? RecordingFileMode.GetIndex() : 0;
        if (FileMode != 0 && HaveFrame && sc.ReplayStatus == REPLAY_STOPPED && !sc.IsFullRecalculation)
        {
            RecorderConfiguration Recorder = MakeRecorderConfiguration(
                sc, RecordedFileName.GetString(), FileMode,
                Clamp(MinimumSnapshotIntervalMilliseconds.GetInt(), 10, 5000),
                Clamp(HeartbeatIntervalMilliseconds.GetInt(), 50, 60000),
                Clamp(FlushIntervalSeconds.GetInt(), 1, 60),
                static_cast<uint32_t>(MinRecordedQuantity),
                static_cast<uint32_t>(MaxDepth));
            SCString Error;
            if (OpenRecorder(State->Recorder, Recorder, Error))
                WriteFrame(State->Recorder, Recorder, State->CurrentFrame, Error);
            if (Error.GetLength() != 0)
                sc.AddMessageToLog(Error, 1);
        }
        else if (FileMode == 0)
        {
            State->Recorder.Close();
        }
    }
    else
    {
        State->Recorder.Close();
        const std::string ReplayPath = BuildDataFilePath(sc, RecordedFileName.GetString());
        if (ReplayPath != State->LastReplayFilePath)
        {
            State->ReplayReader.Close();
            State->LastReplayFilePath = ReplayPath;
            State->NextReplayFrameIndex = 0;
            State->LastReplayTargetDateTime = 0.0;
            State->Engine.Reset();
            State->LoggedReplayLoadError = false;
        }

        SCString Error;
        if (LoadReplayIndex(State->ReplayReader, ReplayPath, Error))
        {
            const std::string RecordedSymbol = ReadFixedString(
                State->ReplayReader.Header.Symbol, sizeof(State->ReplayReader.Header.Symbol));
            const std::string ChartSymbol = SanitizeSymbol(sc.Symbol);
            if (RecordedSymbol != ChartSymbol)
            {
                Error.Format("Recorded MBO symbol %s does not match chart symbol %s", RecordedSymbol.c_str(), ChartSymbol.c_str());
            }
            else if (std::fabs(State->ReplayReader.Header.TickSize - sc.TickSize) > 1e-10)
            {
                Error.Format("Recorded MBO tick size %.10g does not match chart tick size %.10g", State->ReplayReader.Header.TickSize, static_cast<double>(sc.TickSize));
            }
            else
            {
                double TargetDateTime = sc.GetCurrentDateTime().GetAsDouble();
                if (sc.ReplayStatus != REPLAY_STOPPED)
                {
                    if (ReplayClock.GetIndex() == REPLAY_CLOCK_LATEST_CHART_RECORD
                        && sc.LatestDateTimeForLastBar.GetAsDouble() != 0.0)
                    {
                        TargetDateTime = sc.LatestDateTimeForLastBar.GetAsDouble();
                    }
                    else
                    {
                        TargetDateTime = sc.CurrentDateTimeForReplay.GetAsDouble();
                    }
                }
                HaveFrame = AdvanceRecordedData(
                    *State, TargetDateTime,
                    Clamp(ReplayReconstructionLookbackSeconds.GetInt(), 1, 600),
                    Detection, Events, Error);
            }
        }

        if (Error.GetLength() != 0 && !State->LoggedReplayLoadError)
        {
            sc.AddMessageToLog(Error, 1);
            State->LoggedReplayLoadError = true;
        }
    }

    if (HaveFrame)
    {
        BuildDisplayRows(
            State->CurrentFrame, State->Engine, Detection, MinAggregateQuantity,
            MinIndividualMboQuantity, State->Visual.SpoofMode, MaxOrdersText, State->DisplayRows);
        UpdateChartLines(sc, *State);
    }

    for (const DetectionEvent& Event : Events)
    {
        const double Price = static_cast<double>(Event.Level.PriceInTicks) * sc.TickSize;
        SCString Message = Event.Message;
        Message.AppendFormat(" | price %.10g", Price);

        if (WriteEventsToLog.GetYesNo() != 0)
            sc.AddMessageToLog(Message, 0);

        if (sc.ReplayStatus == REPLAY_STOPPED)
        {
            if (Event.Type == 1 && EnableIcebergAlerts.GetYesNo() != 0)
                sc.SetAlert(Clamp(IcebergAlertNumber.GetInt(), 1, 150), Message);
            else if (Event.Type == 2 && EnableSpoofAlerts.GetYesNo() != 0)
                sc.SetAlert(Clamp(SpoofAlertNumber.GetInt(), 1, 150), Message);
        }
    }
}

/*============================================================================
    Study 2: Dedicated MBO snapshot recorder
============================================================================*/
SCSFExport scsf_MBOSnapshotRecorder(SCStudyInterfaceRef sc)
{
    SCInputRef RecordingFileName = sc.Input[0];
    SCInputRef RecordingFileMode = sc.Input[1];
    SCInputRef MaximumDepthLevels = sc.Input[2];
    SCInputRef MaximumOrdersPerPrice = sc.Input[3];
    SCInputRef MinimumRecordedOrderQuantity = sc.Input[4];
    SCInputRef MinimumSnapshotIntervalMilliseconds = sc.Input[5];
    SCInputRef HeartbeatIntervalMilliseconds = sc.Input[6];
    SCInputRef FlushIntervalSeconds = sc.Input[7];
    SCInputRef LogStatus = sc.Input[8];

    if (sc.SetDefaults)
    {
        sc.GraphName = "MBO Snapshot Recorder - Custom Replay File";
        sc.StudyDescription = "Records current ACSIL MBO snapshots, aggregate depth, and all new Time and Sales trades into an SCMBOD1 binary file. Sierra Chart does not natively record/replay MBO; use the MBO DOM Intelligence study to play this custom file during chart replay.";
        sc.AutoLoop = 0;
        sc.GraphRegion = 0;
        sc.UpdateAlways = 1;
        sc.UsesMarketDepthData = 1;
        sc.FreeDLL = 0;

        RecordingFileName.Name = "Recording File Name (Data folder or absolute path)";
        RecordingFileName.SetString("MBO_Record.scmbo");

        RecordingFileMode.Name = "Recording File Mode";
        RecordingFileMode.SetCustomInputStrings("Off;Append / Create;Rewrite Once Then Append");
        RecordingFileMode.SetCustomInputIndex(1);

        MaximumDepthLevels.Name = "Maximum Depth Levels Per Side";
        MaximumDepthLevels.SetInt(20);
        MaximumDepthLevels.SetIntLimits(1, 200);

        MaximumOrdersPerPrice.Name = "Maximum MBO Orders Read Per Price";
        MaximumOrdersPerPrice.SetInt(200);
        MaximumOrdersPerPrice.SetIntLimits(1, 5000);

        MinimumRecordedOrderQuantity.Name = "Minimum MBO Order Quantity Recorded";
        MinimumRecordedOrderQuantity.SetInt(3);
        MinimumRecordedOrderQuantity.SetIntLimits(1, 1000000);

        MinimumSnapshotIntervalMilliseconds.Name = "Minimum Snapshot Interval (ms)";
        MinimumSnapshotIntervalMilliseconds.SetInt(50);
        MinimumSnapshotIntervalMilliseconds.SetIntLimits(10, 5000);

        HeartbeatIntervalMilliseconds.Name = "Unchanged-Book Heartbeat Interval (ms)";
        HeartbeatIntervalMilliseconds.SetInt(1000);
        HeartbeatIntervalMilliseconds.SetIntLimits(50, 60000);

        FlushIntervalSeconds.Name = "File Flush Interval Seconds";
        FlushIntervalSeconds.SetInt(1);
        FlushIntervalSeconds.SetIntLimits(1, 60);

        LogStatus.Name = "Log Recorder Open / Errors";
        LogStatus.SetYesNo(1);
        return;
    }

    RecorderStudyState* State = static_cast<RecorderStudyState*>(sc.GetPersistentPointer(1));
    if (sc.LastCallToFunction)
    {
        if (State != nullptr)
        {
            State->Recorder.Close();
            delete State;
            sc.SetPersistentPointer(1, nullptr);
        }
        return;
    }

    if (State == nullptr)
    {
        State = new RecorderStudyState;
        sc.SetPersistentPointer(1, State);
    }

    if (sc.IsFullRecalculation)
    {
        State->LastTimeAndSalesSequence = 0;
        State->LiveBaselineEstablished = false;
    }

    const int FileMode = RecordingFileMode.GetIndex();
    if (FileMode == 0 || sc.ReplayStatus != REPLAY_STOPPED)
    {
        State->Recorder.Close();
        return;
    }

    const int MaxDepth = Clamp(MaximumDepthLevels.GetInt(), 1, 200);
    const int MaxOrders = Clamp(MaximumOrdersPerPrice.GetInt(), 1, 5000);
    const uint64_t MinRecorded = static_cast<uint64_t>(Clamp(MinimumRecordedOrderQuantity.GetInt(), 1, 1000000));

    BookFrame Frame;
    bool AnyMboSeen = false;
    if (!CaptureLiveFrame(sc, State->LastTimeAndSalesSequence, State->LiveBaselineEstablished,
        MaxDepth, MaxOrders, MinRecorded, Frame, AnyMboSeen))
    {
        return;
    }

    RecorderConfiguration Recorder = MakeRecorderConfiguration(
        sc, RecordingFileName.GetString(), FileMode,
        Clamp(MinimumSnapshotIntervalMilliseconds.GetInt(), 10, 5000),
        Clamp(HeartbeatIntervalMilliseconds.GetInt(), 50, 60000),
        Clamp(FlushIntervalSeconds.GetInt(), 1, 60),
        static_cast<uint32_t>(MinRecorded),
        static_cast<uint32_t>(MaxDepth));

    SCString Error;
    const bool WasOpen = State->Recorder.IsOpen;
    if (OpenRecorder(State->Recorder, Recorder, Error))
    {
        WriteFrame(State->Recorder, Recorder, Frame, Error);
        if (!WasOpen && LogStatus.GetYesNo() != 0)
        {
            SCString Message;
            Message.Format("MBO recorder opened: %s", Recorder.Path.c_str());
            sc.AddMessageToLog(Message, 0);
        }
    }

    if (Error.GetLength() != 0)
        sc.AddMessageToLog(Error, 1);
}
