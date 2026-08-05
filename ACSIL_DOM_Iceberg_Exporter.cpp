#include "sierrachart.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>

SCDLLName("DOM Iceberg CSV Exporter")

namespace
{
    const int PERSISTENT_STATE_KEY = 1;
    const double MILLISECONDS_PER_DAY = 86400000.0;

    struct DepthLevelState
    {
        int Level;
        double Price;
        unsigned long long Quantity;
        unsigned int NumOrders;

        DepthLevelState()
            : Level(-1)
            , Price(0.0)
            , Quantity(0)
            , NumOrders(0)
        {
        }
    };

    struct ExporterState
    {
        std::ofstream File;
        std::string OpenPath;
        std::string LastOpenErrorPath;
        std::map<long long, DepthLevelState> PreviousBid;
        std::map<long long, DepthLevelState> PreviousAsk;
        unsigned int LastTimeAndSalesSequence;
        long long CaptureID;
        SCDateTime LastDepthSnapshotTime;
        bool HasLastDepthSnapshotTime;
        bool TimeAndSalesInitialized;
        unsigned long long RowsSinceFlush;

        ExporterState()
            : LastTimeAndSalesSequence(0)
            , CaptureID(0)
            , HasLastDepthSnapshotTime(false)
            , TimeAndSalesInitialized(false)
            , RowsSinceFlush(0)
        {
        }
    };

    std::string CsvEscape(const std::string& Value)
    {
        if (Value.find_first_of(",\"\r\n") == std::string::npos)
            return Value;

        std::string Escaped;
        Escaped.reserve(Value.size() + 2);
        Escaped.push_back('"');

        for (std::string::const_iterator Character = Value.begin(); Character != Value.end(); ++Character)
        {
            if (*Character == '"')
                Escaped.push_back('"');

            Escaped.push_back(*Character);
        }

        Escaped.push_back('"');
        return Escaped;
    }

    template <typename QuantityType>
    unsigned long long ToUnsignedQuantity(const QuantityType Quantity)
    {
        return Quantity > 0 ? static_cast<unsigned long long>(Quantity) : 0ULL;
    }

    std::string DateTimeToIso8601(SCDateTime DateTime)
    {
        int Year = 0;
        int Month = 0;
        int Day = 0;
        int Hour = 0;
        int Minute = 0;
        int Second = 0;
        int Millisecond = 0;

        DateTime.GetDateTimeYMDHMS_MS(
            Year,
            Month,
            Day,
            Hour,
            Minute,
            Second,
            Millisecond);

        std::ostringstream Output;
        Output << std::setfill('0')
               << std::setw(4) << Year << '-'
               << std::setw(2) << Month << '-'
               << std::setw(2) << Day << 'T'
               << std::setw(2) << Hour << ':'
               << std::setw(2) << Minute << ':'
               << std::setw(2) << Second << '.'
               << std::setw(3) << Millisecond;

        return Output.str();
    }

    long long PriceToTicks(const double Price, const double TickSize)
    {
        if (TickSize <= 0.0)
            return static_cast<long long>(std::llround(Price * 1000000.0));

        return static_cast<long long>(std::llround(Price / TickSize));
    }

    bool SameDepthLevel(const DepthLevelState& Left, const DepthLevelState& Right)
    {
        return Left.Level == Right.Level
            && Left.Quantity == Right.Quantity
            && Left.NumOrders == Right.NumOrders;
    }

    void WriteHeader(ExporterState& State)
    {
        State.File
            << "timestamp,record_type,action,capture_id,sequence,symbol,side,level,price,quantity,num_orders,"
               "best_bid,best_ask,bid_size,ask_size,trade_type,unbundled_trade_indicator,tick_size\n";
    }

    void WriteEvent(
        ExporterState& State,
        SCDateTime Timestamp,
        const char* RecordType,
        const char* Action,
        const long long CaptureID,
        const unsigned int Sequence,
        const std::string& Symbol,
        const char* Side,
        const int Level,
        const double Price,
        const unsigned long long Quantity,
        const unsigned int NumOrders,
        const double BestBid,
        const double BestAsk,
        const unsigned long long BidSize,
        const unsigned long long AskSize,
        const char* TradeType,
        const int UnbundledTradeIndicator,
        const double TickSize)
    {
        if (!State.File.is_open())
            return;

        State.File
            << DateTimeToIso8601(Timestamp) << ','
            << RecordType << ','
            << Action << ','
            << CaptureID << ','
            << Sequence << ','
            << CsvEscape(Symbol) << ','
            << Side << ','
            << Level << ','
            << std::setprecision(12) << Price << ','
            << Quantity << ','
            << NumOrders << ','
            << std::setprecision(12) << BestBid << ','
            << std::setprecision(12) << BestAsk << ','
            << BidSize << ','
            << AskSize << ','
            << TradeType << ','
            << UnbundledTradeIndicator << ','
            << std::setprecision(12) << TickSize
            << '\n';

        ++State.RowsSinceFlush;
    }

    void FlushIfNeeded(ExporterState& State, const int FlushEveryRows)
    {
        const unsigned long long EffectiveFlushInterval =
            static_cast<unsigned long long>(std::max(1, FlushEveryRows));

        if (State.File.is_open() && State.RowsSinceFlush >= EffectiveFlushInterval)
        {
            State.File.flush();
            State.RowsSinceFlush = 0;
        }
    }

    void ResetCaptureState(ExporterState& State)
    {
        State.PreviousBid.clear();
        State.PreviousAsk.clear();
        State.LastTimeAndSalesSequence = 0;
        State.CaptureID = 0;
        State.HasLastDepthSnapshotTime = false;
        State.TimeAndSalesInitialized = false;
    }

    void CloseOutputFile(
        ExporterState& State,
        SCStudyInterfaceRef Sc,
        const bool WriteStopRecord,
        const int FlushEveryRows)
    {
        if (State.File.is_open())
        {
            if (WriteStopRecord)
            {
                SCDateTime CurrentTime = Sc.GetCurrentDateTime();
                WriteEvent(
                    State,
                    CurrentTime,
                    "SESSION",
                    "STOP",
                    State.CaptureID,
                    State.LastTimeAndSalesSequence,
                    Sc.Symbol.GetChars(),
                    "",
                    -1,
                    0.0,
                    0,
                    0,
                    Sc.Bid,
                    Sc.Ask,
                    ToUnsignedQuantity(Sc.BidSize),
                    ToUnsignedQuantity(Sc.AskSize),
                    "",
                    0,
                    Sc.TickSize);
            }

            FlushIfNeeded(State, FlushEveryRows);
            State.File.flush();
            State.File.close();
        }

        State.OpenPath.clear();
        State.RowsSinceFlush = 0;
        ResetCaptureState(State);
    }

    bool OpenOutputFile(
        ExporterState& State,
        SCStudyInterfaceRef Sc,
        const std::string& Path,
        const bool Overwrite,
        const int FlushEveryRows)
    {
        bool FileAlreadyHasContent = false;

        if (!Overwrite)
        {
            std::ifstream ExistingFile(Path.c_str(), std::ios::binary | std::ios::ate);
            if (ExistingFile.is_open())
            {
                const std::ifstream::pos_type Size = ExistingFile.tellg();
                FileAlreadyHasContent = Size > std::ifstream::pos_type(0);
            }
        }

        std::ios::openmode Mode = std::ios::out;
        Mode |= Overwrite ? std::ios::trunc : std::ios::app;

        State.File.open(Path.c_str(), Mode);

        if (!State.File.is_open())
        {
            if (State.LastOpenErrorPath != Path)
            {
                std::string Message = "DOM Iceberg CSV Exporter: unable to open output file: " + Path
                    + ". Verify that the parent directory exists and is writable.";
                Sc.AddMessageToLog(Message.c_str(), 1);
                State.LastOpenErrorPath = Path;
            }

            return false;
        }

        State.OpenPath = Path;
        State.LastOpenErrorPath.clear();
        ResetCaptureState(State);

        if (Overwrite || !FileAlreadyHasContent)
            WriteHeader(State);

        SCDateTime CurrentTime = Sc.GetCurrentDateTime();
        WriteEvent(
            State,
            CurrentTime,
            "SESSION",
            "START",
            0,
            0,
            Sc.Symbol.GetChars(),
            "",
            -1,
            0.0,
            0,
            0,
            Sc.Bid,
            Sc.Ask,
            ToUnsignedQuantity(Sc.BidSize),
            ToUnsignedQuantity(Sc.AskSize),
            "",
            0,
            Sc.TickSize);

        FlushIfNeeded(State, FlushEveryRows);
        return true;
    }

    void ProcessTimeAndSales(
        ExporterState& State,
        SCStudyInterfaceRef Sc,
        const bool ExportBufferedRecords,
        const int FlushEveryRows)
    {
        c_SCTimeAndSalesArray TimeAndSales;
        Sc.GetTimeAndSales(TimeAndSales);

        if (TimeAndSales.Size() == 0)
        {
            State.TimeAndSalesInitialized = true;
            return;
        }

        const unsigned int LatestSequence = TimeAndSales[TimeAndSales.Size() - 1].Sequence;

        if (!State.TimeAndSalesInitialized)
        {
            State.LastTimeAndSalesSequence = ExportBufferedRecords ? 0 : LatestSequence;
            State.TimeAndSalesInitialized = true;
        }
        else if (LatestSequence < State.LastTimeAndSalesSequence)
        {
            SCDateTime ResetTime = Sc.GetCurrentDateTime();
            WriteEvent(
                State,
                ResetTime,
                "SESSION",
                "SEQUENCE_RESET",
                State.CaptureID,
                LatestSequence,
                Sc.Symbol.GetChars(),
                "",
                -1,
                0.0,
                0,
                0,
                Sc.Bid,
                Sc.Ask,
                ToUnsignedQuantity(Sc.BidSize),
                ToUnsignedQuantity(Sc.AskSize),
                "",
                0,
                Sc.TickSize);

            State.LastTimeAndSalesSequence = ExportBufferedRecords ? 0 : LatestSequence;
            State.PreviousBid.clear();
            State.PreviousAsk.clear();
            State.HasLastDepthSnapshotTime = false;
        }

        const double PriceMultiplier =
            Sc.RealTimePriceMultiplier == 0.0f ? 1.0 : Sc.RealTimePriceMultiplier;

        for (int Index = 0; Index < TimeAndSales.Size(); ++Index)
        {
            const s_TimeAndSales& Record = TimeAndSales[Index];

            if (Record.Sequence <= State.LastTimeAndSalesSequence)
                continue;

            State.LastTimeAndSalesSequence = Record.Sequence;

            if (Record.Type != SC_TS_BID && Record.Type != SC_TS_ASK)
                continue;

            SCDateTime AdjustedTimestamp = Record.DateTime;
            AdjustedTimestamp += Sc.TimeScaleAdjustment;

            const char* RestingSide = Record.Type == SC_TS_BID ? "BID" : "ASK";
            const char* TradeType = Record.Type == SC_TS_BID ? "AT_BID" : "AT_ASK";

            WriteEvent(
                State,
                AdjustedTimestamp,
                "TRADE",
                "EXECUTE",
                State.CaptureID,
                Record.Sequence,
                Sc.Symbol.GetChars(),
                RestingSide,
                -1,
                static_cast<double>(Record.Price) * PriceMultiplier,
                ToUnsignedQuantity(Record.Volume),
                0,
                static_cast<double>(Record.Bid) * PriceMultiplier,
                static_cast<double>(Record.Ask) * PriceMultiplier,
                ToUnsignedQuantity(Record.BidSize),
                ToUnsignedQuantity(Record.AskSize),
                TradeType,
                static_cast<int>(Record.UnbundledTradeIndicator),
                Sc.TickSize);
        }

        FlushIfNeeded(State, FlushEveryRows);
    }

    void CaptureDepthSnapshot(
        ExporterState& State,
        SCStudyInterfaceRef Sc,
        const int RequestedLevels,
        const int FlushEveryRows,
        SCDateTime SnapshotTime)
    {
        const int BidLevels = std::min(
            std::max(0, Sc.GetBidMarketDepthNumberOfLevels()),
            std::max(1, RequestedLevels));
        const int AskLevels = std::min(
            std::max(0, Sc.GetAskMarketDepthNumberOfLevels()),
            std::max(1, RequestedLevels));

        std::map<long long, DepthLevelState> CurrentBid;
        std::map<long long, DepthLevelState> CurrentAsk;

        double BestBid = Sc.Bid;
        double BestAsk = Sc.Ask;
        unsigned long long BestBidSize = ToUnsignedQuantity(Sc.BidSize);
        unsigned long long BestAskSize = ToUnsignedQuantity(Sc.AskSize);

        const double TickSize = Sc.TickSize > 0.0f ? Sc.TickSize : 0.00000001;

        for (int Level = 0; Level < BidLevels; ++Level)
        {
            s_MarketDepthEntry Entry;
            if (!Sc.GetBidMarketDepthEntryAtLevel(Entry, Level))
                continue;

            DepthLevelState LevelState;
            LevelState.Level = Level;
            LevelState.Price = Entry.AdjustedPrice != 0.0 ? Entry.AdjustedPrice : Entry.Price;
            LevelState.Quantity = ToUnsignedQuantity(Entry.Quantity);
            LevelState.NumOrders = Entry.NumOrders;

            CurrentBid[PriceToTicks(LevelState.Price, TickSize)] = LevelState;

            if (Level == 0)
            {
                BestBid = LevelState.Price;
                BestBidSize = LevelState.Quantity;
            }
        }

        for (int Level = 0; Level < AskLevels; ++Level)
        {
            s_MarketDepthEntry Entry;
            if (!Sc.GetAskMarketDepthEntryAtLevel(Entry, Level))
                continue;

            DepthLevelState LevelState;
            LevelState.Level = Level;
            LevelState.Price = Entry.AdjustedPrice != 0.0 ? Entry.AdjustedPrice : Entry.Price;
            LevelState.Quantity = ToUnsignedQuantity(Entry.Quantity);
            LevelState.NumOrders = Entry.NumOrders;

            CurrentAsk[PriceToTicks(LevelState.Price, TickSize)] = LevelState;

            if (Level == 0)
            {
                BestAsk = LevelState.Price;
                BestAskSize = LevelState.Quantity;
            }
        }

        ++State.CaptureID;

        WriteEvent(
            State,
            SnapshotTime,
            "SNAPSHOT",
            "FULL",
            State.CaptureID,
            State.LastTimeAndSalesSequence,
            Sc.Symbol.GetChars(),
            "",
            -1,
            0.0,
            0,
            0,
            BestBid,
            BestAsk,
            BestBidSize,
            BestAskSize,
            "",
            0,
            Sc.TickSize);

        for (std::map<long long, DepthLevelState>::const_iterator Current = CurrentBid.begin();
             Current != CurrentBid.end();
             ++Current)
        {
            const std::map<long long, DepthLevelState>::const_iterator Previous =
                State.PreviousBid.find(Current->first);

            if (Previous == State.PreviousBid.end() || !SameDepthLevel(Previous->second, Current->second))
            {
                WriteEvent(
                    State,
                    SnapshotTime,
                    "DEPTH",
                    "UPSERT",
                    State.CaptureID,
                    State.LastTimeAndSalesSequence,
                    Sc.Symbol.GetChars(),
                    "BID",
                    Current->second.Level,
                    Current->second.Price,
                    Current->second.Quantity,
                    Current->second.NumOrders,
                    BestBid,
                    BestAsk,
                    BestBidSize,
                    BestAskSize,
                    "",
                    0,
                    Sc.TickSize);
            }
        }

        for (std::map<long long, DepthLevelState>::const_iterator Previous = State.PreviousBid.begin();
             Previous != State.PreviousBid.end();
             ++Previous)
        {
            if (CurrentBid.find(Previous->first) == CurrentBid.end())
            {
                WriteEvent(
                    State,
                    SnapshotTime,
                    "DEPTH",
                    "DELETE",
                    State.CaptureID,
                    State.LastTimeAndSalesSequence,
                    Sc.Symbol.GetChars(),
                    "BID",
                    Previous->second.Level,
                    Previous->second.Price,
                    0,
                    0,
                    BestBid,
                    BestAsk,
                    BestBidSize,
                    BestAskSize,
                    "",
                    0,
                    Sc.TickSize);
            }
        }

        for (std::map<long long, DepthLevelState>::const_iterator Current = CurrentAsk.begin();
             Current != CurrentAsk.end();
             ++Current)
        {
            const std::map<long long, DepthLevelState>::const_iterator Previous =
                State.PreviousAsk.find(Current->first);

            if (Previous == State.PreviousAsk.end() || !SameDepthLevel(Previous->second, Current->second))
            {
                WriteEvent(
                    State,
                    SnapshotTime,
                    "DEPTH",
                    "UPSERT",
                    State.CaptureID,
                    State.LastTimeAndSalesSequence,
                    Sc.Symbol.GetChars(),
                    "ASK",
                    Current->second.Level,
                    Current->second.Price,
                    Current->second.Quantity,
                    Current->second.NumOrders,
                    BestBid,
                    BestAsk,
                    BestBidSize,
                    BestAskSize,
                    "",
                    0,
                    Sc.TickSize);
            }
        }

        for (std::map<long long, DepthLevelState>::const_iterator Previous = State.PreviousAsk.begin();
             Previous != State.PreviousAsk.end();
             ++Previous)
        {
            if (CurrentAsk.find(Previous->first) == CurrentAsk.end())
            {
                WriteEvent(
                    State,
                    SnapshotTime,
                    "DEPTH",
                    "DELETE",
                    State.CaptureID,
                    State.LastTimeAndSalesSequence,
                    Sc.Symbol.GetChars(),
                    "ASK",
                    Previous->second.Level,
                    Previous->second.Price,
                    0,
                    0,
                    BestBid,
                    BestAsk,
                    BestBidSize,
                    BestAskSize,
                    "",
                    0,
                    Sc.TickSize);
            }
        }

        State.PreviousBid.swap(CurrentBid);
        State.PreviousAsk.swap(CurrentAsk);
        State.LastDepthSnapshotTime = SnapshotTime;
        State.HasLastDepthSnapshotTime = true;

        FlushIfNeeded(State, FlushEveryRows);
    }
}

SCSFExport scsf_DOMIcebergCSVExporter(SCStudyInterfaceRef sc)
{
    SCInputRef OutputCSVFile = sc.Input[0];
    SCInputRef ExportEnabled = sc.Input[1];
    SCInputRef LevelsPerSide = sc.Input[2];
    SCInputRef MinimumSnapshotIntervalMilliseconds = sc.Input[3];
    SCInputRef OverwriteFileWhenStarted = sc.Input[4];
    SCInputRef ExportExistingTimeAndSalesBuffer = sc.Input[5];
    SCInputRef FlushEveryRows = sc.Input[6];

    if (sc.SetDefaults)
    {
        sc.GraphName = "DOM Iceberg CSV Exporter";
        sc.StudyDescription =
            "Exports changed market-depth levels and trade-at-bid/ask records to a CSV file for offline iceberg-candidate analysis.";
        sc.AutoLoop = 0;
        sc.UpdateAlways = 1;
        sc.UsesMarketDepthData = 1;
        sc.GraphRegion = 0;

        OutputCSVFile.Name = "Output CSV File";
        OutputCSVFile.SetPathAndFileName("");

        ExportEnabled.Name = "Export Enabled";
        ExportEnabled.SetYesNo(0);

        LevelsPerSide.Name = "DOM Levels Per Side";
        LevelsPerSide.SetInt(20);
        LevelsPerSide.SetIntLimits(1, 500);

        MinimumSnapshotIntervalMilliseconds.Name = "Minimum Depth Snapshot Interval (Milliseconds)";
        MinimumSnapshotIntervalMilliseconds.SetInt(100);
        MinimumSnapshotIntervalMilliseconds.SetIntLimits(10, 60000);

        OverwriteFileWhenStarted.Name = "Overwrite File When Export Starts";
        OverwriteFileWhenStarted.SetYesNo(1);

        ExportExistingTimeAndSalesBuffer.Name = "Export Existing Time and Sales Buffer on Start";
        ExportExistingTimeAndSalesBuffer.SetYesNo(0);

        FlushEveryRows.Name = "Flush File Every N Rows";
        FlushEveryRows.SetInt(250);
        FlushEveryRows.SetIntLimits(1, 100000);

        return;
    }

    ExporterState* State = static_cast<ExporterState*>(sc.GetPersistentPointer(PERSISTENT_STATE_KEY));

    if (sc.LastCallToFunction)
    {
        if (State != NULL)
        {
            CloseOutputFile(*State, sc, true, FlushEveryRows.GetInt());
            delete State;
            sc.SetPersistentPointer(PERSISTENT_STATE_KEY, NULL);
        }

        return;
    }

    if (State == NULL)
    {
        State = new ExporterState;
        sc.SetPersistentPointer(PERSISTENT_STATE_KEY, State);
    }

    const std::string RequestedPath = OutputCSVFile.GetPathAndFileName();
    const bool Enabled = ExportEnabled.GetYesNo() != 0;

    if (!Enabled || RequestedPath.empty())
    {
        if (State->File.is_open())
            CloseOutputFile(*State, sc, true, FlushEveryRows.GetInt());

        return;
    }

    if (!State->File.is_open() || State->OpenPath != RequestedPath)
    {
        if (State->File.is_open())
            CloseOutputFile(*State, sc, true, FlushEveryRows.GetInt());

        if (!OpenOutputFile(
                *State,
                sc,
                RequestedPath,
                OverwriteFileWhenStarted.GetYesNo() != 0,
                FlushEveryRows.GetInt()))
        {
            return;
        }
    }

    ProcessTimeAndSales(
        *State,
        sc,
        ExportExistingTimeAndSalesBuffer.GetYesNo() != 0,
        FlushEveryRows.GetInt());

    SCDateTime CurrentTime = sc.GetCurrentDateTime();
    bool SnapshotIsDue = !State->HasLastDepthSnapshotTime;

    if (!SnapshotIsDue)
    {
        const double ElapsedMilliseconds =
            (CurrentTime.GetAsDouble() - State->LastDepthSnapshotTime.GetAsDouble())
            * MILLISECONDS_PER_DAY;

        SnapshotIsDue = ElapsedMilliseconds < 0.0
            || ElapsedMilliseconds >= MinimumSnapshotIntervalMilliseconds.GetInt();
    }

    if (SnapshotIsDue)
    {
        CaptureDepthSnapshot(
            *State,
            sc,
            LevelsPerSide.GetInt(),
            FlushEveryRows.GetInt(),
            CurrentTime);
    }
}
