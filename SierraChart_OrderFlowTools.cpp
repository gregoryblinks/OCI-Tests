/*
    Sierra Chart Order Flow Tools
    Package date: 2026-08-04

    Studies:
      1. MBO Probable Iceberg / Refill Detector
      2. Volume Profile POC-Relative Node Tiers

    The MBO study detects probable displayed-liquidity replenishment from
    live snapshots and Time and Sales. It does not prove hidden quantity or
    predict direction. See README.md for data-feed requirements, setup, and
    limitations.
*/

#include "sierrachart.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <vector>

SCDLLName("Sierra Chart Order Flow Tools")

namespace
{
    enum MboSide
    {
        MBO_SIDE_ASK = -1,
        MBO_SIDE_BID = 1
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

    struct OrderSnapshot
    {
        uint64_t Quantity = 0;
        uint64_t QuantityAhead = 0;
        int QueuePosition = 0;
    };

    struct LevelSnapshot
    {
        uint64_t TotalQuantity = 0;
        int OrderCount = 0;
    };

    struct RefillCandidate
    {
        int ObservationCount = 0;
        uint64_t CumulativeExecutedVolume = 0;
        uint64_t CumulativeEstimatedRefill = 0;
        bool SignalAlreadyDrawn = false;
    };

    struct NativeRefillEvent
    {
        bool Valid = false;
        OrderKey Key;
        uint64_t ExecutedAtPrice = 0;
        uint64_t AggressiveVolumePastQueueAhead = 0;
        uint64_t EstimatedRefill = 0;
        uint64_t PreviousQuantity = 0;
        uint64_t CurrentQuantity = 0;
        int QueuePosition = 0;
    };

    struct MboDetectorState
    {
        uint64_t LastProcessedTimeAndSalesSequence = 0;
        bool Initialized = false;
        bool LoggedMissingMboMessage = false;

        std::map<OrderKey, OrderSnapshot> PreviousOrders;
        std::map<LevelKey, LevelSnapshot> PreviousLevels;
        std::map<OrderKey, RefillCandidate> NativeCandidates;
        std::map<LevelKey, RefillCandidate> LevelCandidates;
        std::deque<int> DrawingLineNumbers;
    };

    struct ProfileNode
    {
        int PriceInTicks = 0;
        uint64_t Volume = 0;
        int Tier = 0;
        double PercentOfPOC = 0.0;
    };

    struct ProfileNodeSignature
    {
        int PriceInTicks = 0;
        uint64_t Volume = 0;
        int Tier = 0;

        bool operator==(const ProfileNodeSignature& Other) const
        {
            return PriceInTicks == Other.PriceInTicks
                && Volume == Other.Volume
                && Tier == Other.Tier;
        }
    };

    struct ProfileTierState
    {
        std::vector<int> DrawingLineNumbers;
        std::vector<ProfileNodeSignature> LastNodes;
        uint64_t LastPOCVolume = 0;
        int LastSourceStudyID = 0;
        int LastProfileIndex = -1;
        int LastBeginIndex = -1;
        float LastUpperFloor = -1.0f;
        float LastMiddleFloor = -1.0f;
        float LastLowerFloor = -1.0f;
        int LastShowPrice = -1;
        int LastShowLabels = -1;
        int LastStartAtProfileEnd = -1;
        COLORREF LastTier1Color = 0;
        COLORREF LastTier2Color = 0;
        COLORREF LastTier3Color = 0;
        int LastTier1Width = -1;
        int LastTier2Width = -1;
        int LastTier3Width = -1;
        bool LoggedSourceError = false;
    };

    template <typename T>
    T ClampValue(const T Value, const T Minimum, const T Maximum)
    {
        if (Value < Minimum)
            return Minimum;
        if (Value > Maximum)
            return Maximum;
        return Value;
    }

    void DeleteDequeDrawings(SCStudyInterfaceRef sc, std::deque<int>& LineNumbers)
    {
        for (const int LineNumber : LineNumbers)
        {
            if (LineNumber != 0)
                sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, LineNumber);
        }

        LineNumbers.clear();
    }

    void DeleteVectorDrawings(SCStudyInterfaceRef sc, std::vector<int>& LineNumbers)
    {
        for (const int LineNumber : LineNumbers)
        {
            if (LineNumber != 0)
                sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, LineNumber);
        }

        LineNumbers.clear();
    }

    void RememberDrawing(
        SCStudyInterfaceRef sc,
        MboDetectorState& State,
        const int LineNumber,
        const int MaximumDrawingCount)
    {
        if (LineNumber == 0)
            return;

        State.DrawingLineNumbers.push_back(LineNumber);

        while (static_cast<int>(State.DrawingLineNumbers.size()) > MaximumDrawingCount)
        {
            const int OldestLineNumber = State.DrawingLineNumbers.front();
            State.DrawingLineNumbers.pop_front();

            if (OldestLineNumber != 0)
                sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, OldestLineNumber);
        }
    }

    int AddStudyDrawing(SCStudyInterfaceRef sc, s_UseTool& Tool)
    {
        const int Result = sc.UseTool(Tool);
        if (Result == 0)
            return 0;

        return Tool.LineNumber;
    }

    void DrawIcebergSignal(
        SCStudyInterfaceRef sc,
        MboDetectorState& State,
        const int Side,
        const int PriceInTicks,
        const SCString& Label,
        const COLORREF Color,
        const bool DrawHorizontalRay,
        const bool ShowTextLabel,
        const bool DisplayRayPrice,
        const int MarkerSize,
        const int RayLineWidth,
        const int MaximumSignalsToRetain)
    {
        if (sc.ArraySize <= 0 || sc.TickSize <= 0.0)
            return;

        const int LastBarIndex = sc.ArraySize - 1;
        const double Price = static_cast<double>(PriceInTicks) * sc.TickSize;
        const int DrawingMultiplier = 1 + (DrawHorizontalRay ? 1 : 0) + (ShowTextLabel ? 1 : 0);
        const int MaximumDrawingCount = ClampValue(MaximumSignalsToRetain, 1, 1000) * DrawingMultiplier;

        s_UseTool Marker;
        Marker.Clear();
        Marker.ChartNumber = sc.ChartNumber;
        Marker.DrawingType = DRAWING_MARKER;
        Marker.AddMethod = UTAM_ADD_ALWAYS;
        Marker.Region = 0;
        Marker.BeginIndex = LastBarIndex;
        Marker.BeginValue = Price + (Side == MBO_SIDE_BID ? -sc.TickSize : sc.TickSize);
        Marker.Color = Color;
        Marker.MarkerType = Side == MBO_SIDE_BID ? MARKER_TRIANGLEUP : MARKER_TRIANGLEDOWN;
        Marker.MarkerSize = ClampValue(MarkerSize, 1, 30);
        Marker.LineWidth = 2;
        Marker.AddAsUserDrawnDrawing = 0;
        RememberDrawing(sc, State, AddStudyDrawing(sc, Marker), MaximumDrawingCount);

        if (DrawHorizontalRay)
        {
            s_UseTool Ray;
            Ray.Clear();
            Ray.ChartNumber = sc.ChartNumber;
            Ray.DrawingType = DRAWING_HORIZONTAL_RAY;
            Ray.AddMethod = UTAM_ADD_ALWAYS;
            Ray.Region = 0;
            Ray.BeginIndex = LastBarIndex;
            Ray.BeginValue = Price;
            Ray.Color = Color;
            Ray.LineWidth = ClampValue(RayLineWidth, 1, 10);
            Ray.LineStyle = LINESTYLE_DASH;
            Ray.DisplayHorizontalLineValue = DisplayRayPrice ? 1 : 0;
            Ray.AddAsUserDrawnDrawing = 0;
            RememberDrawing(sc, State, AddStudyDrawing(sc, Ray), MaximumDrawingCount);
        }

        if (ShowTextLabel)
        {
            s_UseTool Text;
            Text.Clear();
            Text.ChartNumber = sc.ChartNumber;
            Text.DrawingType = DRAWING_TEXT;
            Text.AddMethod = UTAM_ADD_ALWAYS;
            Text.Region = 0;
            Text.BeginIndex = LastBarIndex;
            Text.BeginValue = Price + (Side == MBO_SIDE_BID ? -2.0 * sc.TickSize : 2.0 * sc.TickSize);
            Text.Color = Color;
            Text.FontSize = 8;
            Text.FontBold = 1;
            Text.Text = Label;
            Text.AddAsUserDrawnDrawing = 0;
            RememberDrawing(sc, State, AddStudyDrawing(sc, Text), MaximumDrawingCount);
        }
    }

    int CaptureMboSide(
        SCStudyInterfaceRef sc,
        const int Side,
        const int MaximumDepthLevels,
        const int MaximumOrdersPerPrice,
        const uint64_t MinimumDisplayedQuantity,
        std::map<OrderKey, OrderSnapshot>& Orders,
        std::map<LevelKey, LevelSnapshot>& Levels,
        int& DepthLevelsRead)
    {
        const int AvailableLevels = Side == MBO_SIDE_BID
            ? sc.GetBidMarketDepthNumberOfLevels()
            : sc.GetAskMarketDepthNumberOfLevels();

        const int LevelsToRead = AvailableLevels < MaximumDepthLevels
            ? AvailableLevels
            : MaximumDepthLevels;

        if (LevelsToRead <= 0 || MaximumOrdersPerPrice <= 0 || sc.TickSize <= 0.0)
            return 0;

        std::vector<n_ACSIL::s_MarketOrderData> MarketOrders(MaximumOrdersPerPrice);
        int RawOrderCount = 0;

        for (int DepthIndex = 0; DepthIndex < LevelsToRead; ++DepthIndex)
        {
            s_MarketDepthEntry DepthEntry;
            const int GotDepthEntry = Side == MBO_SIDE_BID
                ? sc.GetBidMarketDepthEntryAtLevel(DepthEntry, DepthIndex)
                : sc.GetAskMarketDepthEntryAtLevel(DepthEntry, DepthIndex);

            if (GotDepthEntry == 0)
                continue;

            ++DepthLevelsRead;
            // Use the chart-adjusted depth price so the price-in-ticks key stays
            // aligned with Time and Sales when a Real-Time Price Multiplier is used.
            const int PriceInTicks = static_cast<int>(sc.Round(DepthEntry.AdjustedPrice / sc.TickSize));
            const LevelKey PriceLevel{Side, PriceInTicks};

            int ReturnedOrderCount = Side == MBO_SIDE_BID
                ? sc.GetBidMarketLimitOrdersForPrice(PriceInTicks, MaximumOrdersPerPrice, MarketOrders.data())
                : sc.GetAskMarketLimitOrdersForPrice(PriceInTicks, MaximumOrdersPerPrice, MarketOrders.data());

            if (ReturnedOrderCount < 0)
                ReturnedOrderCount = 0;
            if (ReturnedOrderCount > MaximumOrdersPerPrice)
                ReturnedOrderCount = MaximumOrdersPerPrice;

            RawOrderCount += ReturnedOrderCount;

            uint64_t QuantityAhead = 0;
            int QueuePosition = 0;

            for (int OrderIndex = 0; OrderIndex < ReturnedOrderCount; ++OrderIndex)
            {
                ++QueuePosition;

                const uint64_t OrderID = static_cast<uint64_t>(MarketOrders[OrderIndex].OrderID);
                const uint64_t OrderQuantity = static_cast<uint64_t>(MarketOrders[OrderIndex].OrderQuantity);

                if (OrderID != 0 && OrderQuantity >= MinimumDisplayedQuantity)
                {
                    const OrderKey Key{Side, PriceInTicks, OrderID};
                    const OrderSnapshot Snapshot{OrderQuantity, QuantityAhead, QueuePosition};
                    Orders[Key] = Snapshot;
                }

                QuantityAhead += OrderQuantity;
            }

            // The weaker level-refill path deliberately uses the aggregate
            // market-depth quantity. This lets it operate even when individual
            // MBO orders are unavailable, but it is less specific than an
            // OrderID-preserving native-refill observation.
            const uint64_t AggregateDepthQuantity = static_cast<uint64_t>(DepthEntry.Quantity);
            Levels[PriceLevel] = LevelSnapshot{
                AggregateDepthQuantity,
                static_cast<int>(DepthEntry.NumOrders)};
        }

        return RawOrderCount;
    }

    void ResetMboStateForNewStream(MboDetectorState& State)
    {
        State.LastProcessedTimeAndSalesSequence = 0;
        State.Initialized = false;
        State.PreviousOrders.clear();
        State.PreviousLevels.clear();
        State.NativeCandidates.clear();
        State.LevelCandidates.clear();
    }

    void RemoveMissingCandidates(
        MboDetectorState& State,
        const std::map<OrderKey, OrderSnapshot>& CurrentOrders,
        const std::map<LevelKey, LevelSnapshot>& CurrentLevels)
    {
        for (auto Iterator = State.NativeCandidates.begin(); Iterator != State.NativeCandidates.end();)
        {
            if (CurrentOrders.find(Iterator->first) == CurrentOrders.end())
                Iterator = State.NativeCandidates.erase(Iterator);
            else
                ++Iterator;
        }

        for (auto Iterator = State.LevelCandidates.begin(); Iterator != State.LevelCandidates.end();)
        {
            if (CurrentLevels.find(Iterator->first) == CurrentLevels.end())
                Iterator = State.LevelCandidates.erase(Iterator);
            else
                ++Iterator;
        }
    }

    bool ProfileSettingsAndNodesUnchanged(
        const ProfileTierState& State,
        const std::vector<ProfileNodeSignature>& Signatures,
        const uint64_t POCVolume,
        const int SourceStudyID,
        const int ProfileIndex,
        const int BeginIndex,
        const float UpperFloor,
        const float MiddleFloor,
        const float LowerFloor,
        const int ShowPrice,
        const int ShowLabels,
        const int StartAtProfileEnd,
        const COLORREF Tier1Color,
        const COLORREF Tier2Color,
        const COLORREF Tier3Color,
        const int Tier1Width,
        const int Tier2Width,
        const int Tier3Width)
    {
        if (State.LastPOCVolume != POCVolume
            || State.LastSourceStudyID != SourceStudyID
            || State.LastProfileIndex != ProfileIndex
            || State.LastBeginIndex != BeginIndex
            || State.LastUpperFloor != UpperFloor
            || State.LastMiddleFloor != MiddleFloor
            || State.LastLowerFloor != LowerFloor
            || State.LastShowPrice != ShowPrice
            || State.LastShowLabels != ShowLabels
            || State.LastStartAtProfileEnd != StartAtProfileEnd
            || State.LastTier1Color != Tier1Color
            || State.LastTier2Color != Tier2Color
            || State.LastTier3Color != Tier3Color
            || State.LastTier1Width != Tier1Width
            || State.LastTier2Width != Tier2Width
            || State.LastTier3Width != Tier3Width
            || State.LastNodes.size() != Signatures.size())
        {
            return false;
        }

        for (size_t Index = 0; Index < Signatures.size(); ++Index)
        {
            if (!(State.LastNodes[Index] == Signatures[Index]))
                return false;
        }

        return true;
    }
}

/*
    Study 1: Probable iceberg detector.

    Detection modes:
      1. Native Order-ID refill: the same MBO order ID remains at a price and
         its displayed quantity is replenished after sufficient aggressive
         volume is estimated to have reached it.
      2. Level refill heuristic: displayed quantity at a price replenishes after
         executions, without requiring a persistent order ID. This can also be
         caused by unrelated new orders joining the queue, so it is weaker.

    This is a live microstructure detector. Sierra Chart does not provide a
    historical incremental MBO event stream through ACSIL, so this study is not
    intended for historical chart calculation or ordinary replay validation.
*/
SCSFExport scsf_MBOProbableIcebergDetector(SCStudyInterfaceRef sc)
{
    SCInputRef MaximumDepthLevels = sc.Input[0];
    SCInputRef MaximumOrdersPerPrice = sc.Input[1];
    SCInputRef MaximumQueuePosition = sc.Input[2];
    SCInputRef MinimumDisplayedOrderQuantity = sc.Input[3];
    SCInputRef MinimumAggressiveVolumeReachingOrder = sc.Input[4];
    SCInputRef MinimumEstimatedRefillQuantity = sc.Input[5];
    SCInputRef RequiredRefillObservations = sc.Input[6];
    SCInputRef EnableLevelRefillHeuristic = sc.Input[7];
    SCInputRef DrawHorizontalRay = sc.Input[8];
    SCInputRef ShowTextLabel = sc.Input[9];
    SCInputRef DisplayRayPrice = sc.Input[10];
    SCInputRef EnableAlerts = sc.Input[11];
    SCInputRef AlertNumber = sc.Input[12];
    SCInputRef MarkerSize = sc.Input[13];
    SCInputRef RayLineWidth = sc.Input[14];
    SCInputRef MaximumSignalsToRetain = sc.Input[15];
    SCInputRef BidIcebergColor = sc.Input[16];
    SCInputRef AskIcebergColor = sc.Input[17];
    SCInputRef WriteSignalsToMessageLog = sc.Input[18];

    if (sc.SetDefaults)
    {
        sc.GraphName = "MBO Probable Iceberg / Refill Detector";
        sc.StudyDescription = "Live Market-by-Order and Time-and-Sales detector for probable native order-ID refills and weaker price-level replenishment. Signals are evidence of displayed-liquidity replenishment, not proof of undisclosed total order size or future direction.";
        sc.AutoLoop = 0;
        sc.GraphRegion = 0;
        sc.UpdateAlways = 1;
        sc.UsesMarketDepthData = 1;

        MaximumDepthLevels.Name = "Maximum DOM Depth Levels Per Side";
        MaximumDepthLevels.SetInt(12);
        MaximumDepthLevels.SetIntLimits(1, 100);

        MaximumOrdersPerPrice.Name = "Maximum MBO Orders Read Per Price";
        MaximumOrdersPerPrice.SetInt(64);
        MaximumOrdersPerPrice.SetIntLimits(1, 500);

        MaximumQueuePosition.Name = "Maximum Returned Queue Position For Native Detection";
        MaximumQueuePosition.SetInt(5);
        MaximumQueuePosition.SetIntLimits(1, 100);

        MinimumDisplayedOrderQuantity.Name = "Minimum Displayed Order Quantity";
        MinimumDisplayedOrderQuantity.SetInt(3);
        MinimumDisplayedOrderQuantity.SetIntLimits(1, 1000000);

        MinimumAggressiveVolumeReachingOrder.Name = "Minimum Aggressive Volume Estimated To Reach Order";
        MinimumAggressiveVolumeReachingOrder.SetInt(10);
        MinimumAggressiveVolumeReachingOrder.SetIntLimits(1, 1000000);

        MinimumEstimatedRefillQuantity.Name = "Minimum Estimated Refill Quantity";
        MinimumEstimatedRefillQuantity.SetInt(5);
        MinimumEstimatedRefillQuantity.SetIntLimits(1, 1000000);

        RequiredRefillObservations.Name = "Required Refill Observations Before Signal";
        RequiredRefillObservations.SetInt(2);
        RequiredRefillObservations.SetIntLimits(1, 20);

        EnableLevelRefillHeuristic.Name = "Enable Weaker Price-Level Refill Heuristic";
        EnableLevelRefillHeuristic.SetYesNo(1);

        DrawHorizontalRay.Name = "Draw Signal Horizontal Ray";
        DrawHorizontalRay.SetYesNo(1);

        ShowTextLabel.Name = "Show Signal Text Label";
        ShowTextLabel.SetYesNo(1);

        DisplayRayPrice.Name = "Display Price On Signal Ray";
        DisplayRayPrice.SetYesNo(1);

        EnableAlerts.Name = "Enable Alerts";
        EnableAlerts.SetYesNo(0);

        AlertNumber.Name = "Alert Number";
        AlertNumber.SetInt(1);
        AlertNumber.SetIntLimits(1, 150);

        MarkerSize.Name = "Marker Size";
        MarkerSize.SetInt(8);
        MarkerSize.SetIntLimits(1, 30);

        RayLineWidth.Name = "Ray Line Width";
        RayLineWidth.SetInt(1);
        RayLineWidth.SetIntLimits(1, 10);

        MaximumSignalsToRetain.Name = "Maximum Signals To Retain";
        MaximumSignalsToRetain.SetInt(100);
        MaximumSignalsToRetain.SetIntLimits(1, 1000);

        BidIcebergColor.Name = "Probable Bid Iceberg Color";
        BidIcebergColor.SetColor(RGB(0, 190, 80));

        AskIcebergColor.Name = "Probable Ask Iceberg Color";
        AskIcebergColor.SetColor(RGB(220, 60, 60));

        WriteSignalsToMessageLog.Name = "Write Signals To Message Log";
        WriteSignalsToMessageLog.SetYesNo(0);

        return;
    }

    MboDetectorState* State = static_cast<MboDetectorState*>(sc.GetPersistentPointer(1));

    if (sc.LastCallToFunction)
    {
        if (State != nullptr)
        {
            DeleteDequeDrawings(sc, State->DrawingLineNumbers);
            delete State;
            sc.SetPersistentPointer(1, nullptr);
        }
        return;
    }

    if (State == nullptr)
    {
        State = new MboDetectorState;
        sc.SetPersistentPointer(1, State);
    }

    if (sc.IsFullRecalculation)
    {
        DeleteDequeDrawings(sc, State->DrawingLineNumbers);
        ResetMboStateForNewStream(*State);
        State->LoggedMissingMboMessage = false;
    }

    const int MaxDepth = ClampValue(MaximumDepthLevels.GetInt(), 1, 100);
    const int MaxOrders = ClampValue(MaximumOrdersPerPrice.GetInt(), 1, 500);
    const int MaxQueuePosition = ClampValue(MaximumQueuePosition.GetInt(), 1, 100);
    const uint64_t MinDisplayedQuantity = static_cast<uint64_t>(ClampValue(MinimumDisplayedOrderQuantity.GetInt(), 1, 1000000));
    const uint64_t MinAggressiveVolume = static_cast<uint64_t>(ClampValue(MinimumAggressiveVolumeReachingOrder.GetInt(), 1, 1000000));
    const uint64_t MinRefillQuantity = static_cast<uint64_t>(ClampValue(MinimumEstimatedRefillQuantity.GetInt(), 1, 1000000));
    const int RequiredObservations = ClampValue(RequiredRefillObservations.GetInt(), 1, 20);

    c_SCTimeAndSalesArray TimeAndSales;
    sc.GetTimeAndSales(TimeAndSales);

    uint64_t LatestSequence = 0;
    if (TimeAndSales.Size() > 0)
        LatestSequence = static_cast<uint64_t>(TimeAndSales[TimeAndSales.Size() - 1].Sequence);

    if (State->Initialized
        && LatestSequence != 0
        && LatestSequence < State->LastProcessedTimeAndSalesSequence)
    {
        ResetMboStateForNewStream(*State);
    }

    std::map<OrderKey, OrderSnapshot> CurrentOrders;
    std::map<LevelKey, LevelSnapshot> CurrentLevels;
    int DepthLevelsRead = 0;

    const int BidRawOrderCount = CaptureMboSide(
        sc,
        MBO_SIDE_BID,
        MaxDepth,
        MaxOrders,
        MinDisplayedQuantity,
        CurrentOrders,
        CurrentLevels,
        DepthLevelsRead);

    const int AskRawOrderCount = CaptureMboSide(
        sc,
        MBO_SIDE_ASK,
        MaxDepth,
        MaxOrders,
        MinDisplayedQuantity,
        CurrentOrders,
        CurrentLevels,
        DepthLevelsRead);

    const int TotalRawMboOrders = BidRawOrderCount + AskRawOrderCount;

    if (DepthLevelsRead > 0 && TotalRawMboOrders == 0 && !State->LoggedMissingMboMessage)
    {
        SCString Message;
        Message.Format(
            "MBO Probable Iceberg Detector: Market depth is present but no MBO orders were returned. Confirm that the symbol/feed supplies Market by Order data, the required Sierra Chart service package is active, and the study is running on a normal chart with depth enabled.");
        sc.AddMessageToLog(Message, 1);
        State->LoggedMissingMboMessage = true;
    }
    else if (TotalRawMboOrders > 0)
    {
        State->LoggedMissingMboMessage = false;
    }

    if (!State->Initialized)
    {
        State->LastProcessedTimeAndSalesSequence = LatestSequence;
        State->PreviousOrders.swap(CurrentOrders);
        State->PreviousLevels.swap(CurrentLevels);
        State->Initialized = true;
        return;
    }

    std::map<LevelKey, uint64_t> ExecutedVolumeBySideAndPrice;

    for (int TimeAndSalesIndex = 0; TimeAndSalesIndex < TimeAndSales.Size(); ++TimeAndSalesIndex)
    {
        const s_TimeAndSales& Record = TimeAndSales[TimeAndSalesIndex];
        const uint64_t Sequence = static_cast<uint64_t>(Record.Sequence);

        if (Sequence <= State->LastProcessedTimeAndSalesSequence)
            continue;

        if (Record.Type != SC_TS_BID && Record.Type != SC_TS_ASK)
            continue;

        if (sc.TickSize <= 0.0)
            continue;

        const double TradePrice = Record.Price * sc.RealTimePriceMultiplier;
        const int PriceInTicks = static_cast<int>(sc.Round(TradePrice / sc.TickSize));
        const int Side = Record.Type == SC_TS_BID ? MBO_SIDE_BID : MBO_SIDE_ASK;
        const uint64_t Volume = static_cast<uint64_t>(Record.Volume);

        if (Volume == 0)
            continue;

        ExecutedVolumeBySideAndPrice[LevelKey{Side, PriceInTicks}] += Volume;
    }

    State->LastProcessedTimeAndSalesSequence = LatestSequence;
    RemoveMissingCandidates(*State, CurrentOrders, CurrentLevels);

    std::map<LevelKey, NativeRefillEvent> BestNativeEventByLevel;

    for (const auto& CurrentPair : CurrentOrders)
    {
        const OrderKey& Key = CurrentPair.first;
        const OrderSnapshot& CurrentOrder = CurrentPair.second;

        const auto PreviousOrderIterator = State->PreviousOrders.find(Key);
        if (PreviousOrderIterator == State->PreviousOrders.end())
            continue;

        const OrderSnapshot& PreviousOrder = PreviousOrderIterator->second;
        if (PreviousOrder.QueuePosition > MaxQueuePosition)
            continue;

        const LevelKey PriceLevel{Key.Side, Key.PriceInTicks};
        const auto ExecutionIterator = ExecutedVolumeBySideAndPrice.find(PriceLevel);
        if (ExecutionIterator == ExecutedVolumeBySideAndPrice.end())
            continue;

        const uint64_t ExecutedAtPrice = ExecutionIterator->second;
        if (ExecutedAtPrice <= PreviousOrder.QuantityAhead)
            continue;

        const uint64_t AggressiveVolumePastQueueAhead = ExecutedAtPrice - PreviousOrder.QuantityAhead;
        if (AggressiveVolumePastQueueAhead < MinAggressiveVolume)
            continue;

        const uint64_t ExpectedRemainingQuantity = PreviousOrder.Quantity > AggressiveVolumePastQueueAhead
            ? PreviousOrder.Quantity - AggressiveVolumePastQueueAhead
            : 0;

        const uint64_t EstimatedRefill = CurrentOrder.Quantity > ExpectedRemainingQuantity
            ? CurrentOrder.Quantity - ExpectedRemainingQuantity
            : 0;

        if (EstimatedRefill < MinRefillQuantity)
            continue;

        NativeRefillEvent Event;
        Event.Valid = true;
        Event.Key = Key;
        Event.ExecutedAtPrice = ExecutedAtPrice;
        Event.AggressiveVolumePastQueueAhead = AggressiveVolumePastQueueAhead;
        Event.EstimatedRefill = EstimatedRefill;
        Event.PreviousQuantity = PreviousOrder.Quantity;
        Event.CurrentQuantity = CurrentOrder.Quantity;
        Event.QueuePosition = PreviousOrder.QueuePosition;

        auto ExistingEventIterator = BestNativeEventByLevel.find(PriceLevel);
        if (ExistingEventIterator == BestNativeEventByLevel.end()
            || Event.EstimatedRefill > ExistingEventIterator->second.EstimatedRefill
            || (Event.EstimatedRefill == ExistingEventIterator->second.EstimatedRefill
                && Event.AggressiveVolumePastQueueAhead > ExistingEventIterator->second.AggressiveVolumePastQueueAhead))
        {
            BestNativeEventByLevel[PriceLevel] = Event;
        }
    }

    std::set<OrderKey> NativeEventsThisUpdate;
    std::set<LevelKey> NativeSignalsThisUpdate;

    for (const auto& EventPair : BestNativeEventByLevel)
    {
        const NativeRefillEvent& Event = EventPair.second;
        if (!Event.Valid)
            continue;

        NativeEventsThisUpdate.insert(Event.Key);
        RefillCandidate& Candidate = State->NativeCandidates[Event.Key];
        ++Candidate.ObservationCount;
        Candidate.CumulativeExecutedVolume += Event.AggressiveVolumePastQueueAhead;
        Candidate.CumulativeEstimatedRefill += Event.EstimatedRefill;

        if (!Candidate.SignalAlreadyDrawn && Candidate.ObservationCount >= RequiredObservations)
        {
            const bool IsBid = Event.Key.Side == MBO_SIDE_BID;
            const double Price = static_cast<double>(Event.Key.PriceInTicks) * sc.TickSize;
            SCString Label;
            Label.Format(
                "MBO NATIVE-ID REFILL %s | %.10g | obs %d | vol past queue %llu | refill %llu | ID %llu",
                IsBid ? "BID" : "ASK",
                Price,
                Candidate.ObservationCount,
                static_cast<unsigned long long>(Candidate.CumulativeExecutedVolume),
                static_cast<unsigned long long>(Candidate.CumulativeEstimatedRefill),
                static_cast<unsigned long long>(Event.Key.OrderID));

            DrawIcebergSignal(
                sc,
                *State,
                Event.Key.Side,
                Event.Key.PriceInTicks,
                Label,
                IsBid ? BidIcebergColor.GetColor() : AskIcebergColor.GetColor(),
                DrawHorizontalRay.GetYesNo() != 0,
                ShowTextLabel.GetYesNo() != 0,
                DisplayRayPrice.GetYesNo() != 0,
                MarkerSize.GetInt(),
                RayLineWidth.GetInt(),
                MaximumSignalsToRetain.GetInt());

            Candidate.SignalAlreadyDrawn = true;
            NativeSignalsThisUpdate.insert(EventPair.first);

            if (EnableAlerts.GetYesNo())
                sc.SetAlert(ClampValue(AlertNumber.GetInt(), 1, 150), Label);

            if (WriteSignalsToMessageLog.GetYesNo())
                sc.AddMessageToLog(Label, 0);
        }
    }

    for (auto& CandidatePair : State->NativeCandidates)
    {
        if (CandidatePair.second.SignalAlreadyDrawn)
            continue;

        const OrderKey& Key = CandidatePair.first;
        const LevelKey PriceLevel{Key.Side, Key.PriceInTicks};

        if (ExecutedVolumeBySideAndPrice.find(PriceLevel) != ExecutedVolumeBySideAndPrice.end()
            && NativeEventsThisUpdate.find(Key) == NativeEventsThisUpdate.end())
        {
            CandidatePair.second.ObservationCount = 0;
            CandidatePair.second.CumulativeExecutedVolume = 0;
            CandidatePair.second.CumulativeEstimatedRefill = 0;
        }
    }

    if (EnableLevelRefillHeuristic.GetYesNo())
    {
        std::set<LevelKey> LevelEventsThisUpdate;

        for (const auto& CurrentLevelPair : CurrentLevels)
        {
            const LevelKey& PriceLevel = CurrentLevelPair.first;
            const LevelSnapshot& CurrentLevel = CurrentLevelPair.second;

            const auto PreviousLevelIterator = State->PreviousLevels.find(PriceLevel);
            if (PreviousLevelIterator == State->PreviousLevels.end())
                continue;

            const auto ExecutionIterator = ExecutedVolumeBySideAndPrice.find(PriceLevel);
            if (ExecutionIterator == ExecutedVolumeBySideAndPrice.end())
                continue;

            const uint64_t ExecutedAtPrice = ExecutionIterator->second;
            if (ExecutedAtPrice < MinAggressiveVolume)
                continue;

            const uint64_t PreviousDisplayedQuantity = PreviousLevelIterator->second.TotalQuantity;
            const uint64_t CurrentDisplayedQuantity = CurrentLevel.TotalQuantity;
            const uint64_t EstimatedRefill = CurrentDisplayedQuantity + ExecutedAtPrice > PreviousDisplayedQuantity
                ? CurrentDisplayedQuantity + ExecutedAtPrice - PreviousDisplayedQuantity
                : 0;

            if (EstimatedRefill < MinRefillQuantity)
                continue;

            LevelEventsThisUpdate.insert(PriceLevel);
            RefillCandidate& Candidate = State->LevelCandidates[PriceLevel];
            ++Candidate.ObservationCount;
            Candidate.CumulativeExecutedVolume += ExecutedAtPrice;
            Candidate.CumulativeEstimatedRefill += EstimatedRefill;

            if (!Candidate.SignalAlreadyDrawn
                && Candidate.ObservationCount >= RequiredObservations
                && NativeSignalsThisUpdate.find(PriceLevel) == NativeSignalsThisUpdate.end())
            {
                const bool IsBid = PriceLevel.Side == MBO_SIDE_BID;
                const double Price = static_cast<double>(PriceLevel.PriceInTicks) * sc.TickSize;
                SCString Label;
                Label.Format(
                    "LEVEL-REFILL HEURISTIC %s | %.10g | obs %d | exec %llu | est refill %llu",
                    IsBid ? "BID" : "ASK",
                    Price,
                    Candidate.ObservationCount,
                    static_cast<unsigned long long>(Candidate.CumulativeExecutedVolume),
                    static_cast<unsigned long long>(Candidate.CumulativeEstimatedRefill));

                DrawIcebergSignal(
                    sc,
                    *State,
                    PriceLevel.Side,
                    PriceLevel.PriceInTicks,
                    Label,
                    IsBid ? BidIcebergColor.GetColor() : AskIcebergColor.GetColor(),
                    DrawHorizontalRay.GetYesNo() != 0,
                    ShowTextLabel.GetYesNo() != 0,
                    DisplayRayPrice.GetYesNo() != 0,
                    MarkerSize.GetInt(),
                    RayLineWidth.GetInt(),
                    MaximumSignalsToRetain.GetInt());

                Candidate.SignalAlreadyDrawn = true;

                if (EnableAlerts.GetYesNo())
                    sc.SetAlert(ClampValue(AlertNumber.GetInt(), 1, 150), Label);

                if (WriteSignalsToMessageLog.GetYesNo())
                    sc.AddMessageToLog(Label, 0);
            }
        }

        for (auto& CandidatePair : State->LevelCandidates)
        {
            if (CandidatePair.second.SignalAlreadyDrawn)
                continue;

            const LevelKey& PriceLevel = CandidatePair.first;
            if (ExecutedVolumeBySideAndPrice.find(PriceLevel) != ExecutedVolumeBySideAndPrice.end()
                && LevelEventsThisUpdate.find(PriceLevel) == LevelEventsThisUpdate.end())
            {
                CandidatePair.second.ObservationCount = 0;
                CandidatePair.second.CumulativeExecutedVolume = 0;
                CandidatePair.second.CumulativeEstimatedRefill = 0;
            }
        }
    }

    State->PreviousOrders.swap(CurrentOrders);
    State->PreviousLevels.swap(CurrentLevels);
}

/*
    Study 2: Mark price rows whose row volume is a selected percentage of the
    profile POC row volume.

    Default tiers:
      Tier 1: 90% through 100% of POC row volume
      Tier 2: 80% through less than 90%
      Tier 3: 75% through less than 80%

    This is not the conventional 70%/75% cumulative Value Area calculation.
    It is a direct row-volume / POC-row-volume comparison.
*/
SCSFExport scsf_VolumeProfilePOCNodeTiers(SCStudyInterfaceRef sc)
{
    SCInputRef SourceVolumeByPriceStudy = sc.Input[0];
    SCInputRef ProfileIndex = sc.Input[1];
    SCInputRef Tier1MinimumPercent = sc.Input[2];
    SCInputRef Tier2MinimumPercent = sc.Input[3];
    SCInputRef Tier3MinimumPercent = sc.Input[4];
    SCInputRef StartRaysAtProfileEnd = sc.Input[5];
    SCInputRef DisplayPriceOnRay = sc.Input[6];
    SCInputRef ShowTierLabelAtOrigin = sc.Input[7];
    SCInputRef MaximumRowsToDraw = sc.Input[8];
    SCInputRef Tier1Color = sc.Input[9];
    SCInputRef Tier2Color = sc.Input[10];
    SCInputRef Tier3Color = sc.Input[11];
    SCInputRef Tier1LineWidth = sc.Input[12];
    SCInputRef Tier2LineWidth = sc.Input[13];
    SCInputRef Tier3LineWidth = sc.Input[14];

    if (sc.SetDefaults)
    {
        sc.GraphName = "Volume Profile POC-Relative Node Tiers";
        sc.StudyDescription = "Reads a selected Sierra Chart Volume by Price study and draws horizontal rays for rows whose volume is at least the selected percentage of the profile's highest-volume (POC) row. Defaults: 90-100%, 80-90%, and 75-80% of POC row volume.";
        sc.AutoLoop = 0;
        sc.GraphRegion = 0;
        sc.UpdateAlways = 0;
        sc.CalculationPrecedence = LOW_PREC_LEVEL;

        SourceVolumeByPriceStudy.Name = "Source Volume by Price Study";
        SourceVolumeByPriceStudy.SetStudyID(1);

        ProfileIndex.Name = "Profile Index (0 = Latest / Developing, 1 = Previous)";
        ProfileIndex.SetInt(0);
        ProfileIndex.SetIntLimits(0, 10000);

        Tier1MinimumPercent.Name = "Tier 1 Minimum Percent Of POC Row Volume";
        Tier1MinimumPercent.SetFloat(90.0f);
        Tier1MinimumPercent.SetFloatLimits(0.0f, 100.0f);

        Tier2MinimumPercent.Name = "Tier 2 Minimum Percent Of POC Row Volume";
        Tier2MinimumPercent.SetFloat(80.0f);
        Tier2MinimumPercent.SetFloatLimits(0.0f, 100.0f);

        Tier3MinimumPercent.Name = "Tier 3 Minimum Percent Of POC Row Volume";
        Tier3MinimumPercent.SetFloat(75.0f);
        Tier3MinimumPercent.SetFloatLimits(0.0f, 100.0f);

        StartRaysAtProfileEnd.Name = "Start Rays At Profile End (No = Profile Start)";
        StartRaysAtProfileEnd.SetYesNo(1);

        DisplayPriceOnRay.Name = "Display Price On Ray";
        DisplayPriceOnRay.SetYesNo(1);

        ShowTierLabelAtOrigin.Name = "Show Tier And Percent Label At Ray Origin";
        ShowTierLabelAtOrigin.SetYesNo(0);

        MaximumRowsToDraw.Name = "Maximum Qualifying Rows To Draw";
        MaximumRowsToDraw.SetInt(500);
        MaximumRowsToDraw.SetIntLimits(1, 5000);

        Tier1Color.Name = "Tier 1 Color (90-100% Default)";
        Tier1Color.SetColor(RGB(230, 190, 20));

        Tier2Color.Name = "Tier 2 Color (80-90% Default)";
        Tier2Color.SetColor(RGB(70, 150, 240));

        Tier3Color.Name = "Tier 3 Color (75-80% Default)";
        Tier3Color.SetColor(RGB(150, 150, 150));

        Tier1LineWidth.Name = "Tier 1 Line Width";
        Tier1LineWidth.SetInt(3);
        Tier1LineWidth.SetIntLimits(1, 10);

        Tier2LineWidth.Name = "Tier 2 Line Width";
        Tier2LineWidth.SetInt(2);
        Tier2LineWidth.SetIntLimits(1, 10);

        Tier3LineWidth.Name = "Tier 3 Line Width";
        Tier3LineWidth.SetInt(1);
        Tier3LineWidth.SetIntLimits(1, 10);

        return;
    }

    ProfileTierState* State = static_cast<ProfileTierState*>(sc.GetPersistentPointer(1));

    if (sc.LastCallToFunction)
    {
        if (State != nullptr)
        {
            DeleteVectorDrawings(sc, State->DrawingLineNumbers);
            delete State;
            sc.SetPersistentPointer(1, nullptr);
        }
        return;
    }

    if (State == nullptr)
    {
        State = new ProfileTierState;
        sc.SetPersistentPointer(1, State);
    }

    if (sc.IsFullRecalculation)
    {
        DeleteVectorDrawings(sc, State->DrawingLineNumbers);
        State->LastNodes.clear();
        State->LastPOCVolume = 0;
        State->LastSourceStudyID = 0;
        State->LastProfileIndex = -1;
        State->LastBeginIndex = -1;
        State->LoggedSourceError = false;
    }

    if (sc.ArraySize <= 0 || sc.TickSize <= 0.0)
        return;

    const int SourceStudyID = SourceVolumeByPriceStudy.GetStudyID();
    const int RequestedProfileIndex = ClampValue(ProfileIndex.GetInt(), 0, 10000);
    const int NumberOfProfiles = SourceStudyID > 0
        ? sc.GetNumStudyProfiles(SourceStudyID)
        : 0;

    if (SourceStudyID <= 0 || NumberOfProfiles <= RequestedProfileIndex)
    {
        DeleteVectorDrawings(sc, State->DrawingLineNumbers);
        State->LastNodes.clear();
        State->LastPOCVolume = 0;

        if (!State->LoggedSourceError)
        {
            SCString Message;
            Message.Format(
                "Volume Profile POC-Relative Node Tiers: source study ID %d does not expose profile index %d. Add/configure a Volume by Price study on this chart and select it in the Source Study input.",
                SourceStudyID,
                RequestedProfileIndex);
            sc.AddMessageToLog(Message, 1);
            State->LoggedSourceError = true;
        }
        return;
    }

    State->LoggedSourceError = false;

    float Thresholds[3] =
    {
        ClampValue(Tier1MinimumPercent.GetFloat(), 0.0f, 100.0f),
        ClampValue(Tier2MinimumPercent.GetFloat(), 0.0f, 100.0f),
        ClampValue(Tier3MinimumPercent.GetFloat(), 0.0f, 100.0f)
    };

    std::sort(Thresholds, Thresholds + 3, std::greater<float>());
    const float UpperFloor = Thresholds[0];
    const float MiddleFloor = Thresholds[1];
    const float LowerFloor = Thresholds[2];

    const int NumberOfPriceLevels = sc.GetNumPriceLevelsForStudyProfile(SourceStudyID, RequestedProfileIndex);
    if (NumberOfPriceLevels <= 0)
    {
        DeleteVectorDrawings(sc, State->DrawingLineNumbers);
        State->LastNodes.clear();
        State->LastPOCVolume = 0;
        return;
    }

    std::vector<ProfileNode> AllRows;
    AllRows.reserve(NumberOfPriceLevels);
    uint64_t POCVolume = 0;

    for (int PriceIndex = 0; PriceIndex < NumberOfPriceLevels; ++PriceIndex)
    {
        s_VolumeAtPriceV2 VolumeAtPrice;
        if (sc.GetVolumeAtPriceDataForStudyProfile(
                SourceStudyID,
                RequestedProfileIndex,
                PriceIndex,
                VolumeAtPrice) == 0)
        {
            continue;
        }

        const uint64_t RowVolume = static_cast<uint64_t>(VolumeAtPrice.Volume);
        if (RowVolume > POCVolume)
            POCVolume = RowVolume;

        ProfileNode Row;
        Row.PriceInTicks = VolumeAtPrice.PriceInTicks;
        Row.Volume = RowVolume;
        AllRows.push_back(Row);
    }

    if (POCVolume == 0 || AllRows.empty())
    {
        DeleteVectorDrawings(sc, State->DrawingLineNumbers);
        State->LastNodes.clear();
        State->LastPOCVolume = 0;
        return;
    }

    std::vector<ProfileNode> QualifyingRows;
    QualifyingRows.reserve(AllRows.size());

    for (ProfileNode Row : AllRows)
    {
        Row.PercentOfPOC = 100.0 * static_cast<double>(Row.Volume) / static_cast<double>(POCVolume);

        if (Row.PercentOfPOC >= UpperFloor)
            Row.Tier = 1;
        else if (Row.PercentOfPOC >= MiddleFloor)
            Row.Tier = 2;
        else if (Row.PercentOfPOC >= LowerFloor)
            Row.Tier = 3;
        else
            continue;

        QualifyingRows.push_back(Row);
    }

    const int MaximumRows = ClampValue(MaximumRowsToDraw.GetInt(), 1, 5000);
    if (static_cast<int>(QualifyingRows.size()) > MaximumRows)
    {
        std::sort(
            QualifyingRows.begin(),
            QualifyingRows.end(),
            [](const ProfileNode& Left, const ProfileNode& Right)
            {
                if (Left.Volume != Right.Volume)
                    return Left.Volume > Right.Volume;
                return Left.PriceInTicks < Right.PriceInTicks;
            });

        QualifyingRows.resize(MaximumRows);
    }

    std::sort(
        QualifyingRows.begin(),
        QualifyingRows.end(),
        [](const ProfileNode& Left, const ProfileNode& Right)
        {
            return Left.PriceInTicks < Right.PriceInTicks;
        });

    n_ACSIL::s_StudyProfileInformation ProfileInformation;
    int RayBeginIndex = sc.ArraySize - 1;

    if (sc.GetStudyProfileInformation(SourceStudyID, RequestedProfileIndex, ProfileInformation) != 0)
    {
        RayBeginIndex = StartRaysAtProfileEnd.GetYesNo()
            ? ProfileInformation.m_EndIndex
            : ProfileInformation.m_BeginIndex;
    }

    RayBeginIndex = ClampValue(RayBeginIndex, 0, sc.ArraySize - 1);

    const int ShowPrice = DisplayPriceOnRay.GetYesNo();
    const int ShowLabels = ShowTierLabelAtOrigin.GetYesNo();
    const int StartAtEnd = StartRaysAtProfileEnd.GetYesNo();

    // When labels are hidden, only price membership and tier affect the drawings.
    // Ignoring within-tier volume changes avoids unnecessary delete/redraw cycles
    // while a developing profile is receiving trades.
    const uint64_t ComparablePOCVolume = ShowLabels ? POCVolume : 0;

    std::vector<ProfileNodeSignature> Signatures;
    Signatures.reserve(QualifyingRows.size());
    for (const ProfileNode& Row : QualifyingRows)
    {
        const uint64_t SignatureVolume = ShowLabels ? Row.Volume : 0;
        Signatures.push_back(ProfileNodeSignature{Row.PriceInTicks, SignatureVolume, Row.Tier});
    }
    const int Tier1Width = ClampValue(Tier1LineWidth.GetInt(), 1, 10);
    const int Tier2Width = ClampValue(Tier2LineWidth.GetInt(), 1, 10);
    const int Tier3Width = ClampValue(Tier3LineWidth.GetInt(), 1, 10);

    if (ProfileSettingsAndNodesUnchanged(
            *State,
            Signatures,
            ComparablePOCVolume,
            SourceStudyID,
            RequestedProfileIndex,
            RayBeginIndex,
            UpperFloor,
            MiddleFloor,
            LowerFloor,
            ShowPrice,
            ShowLabels,
            StartAtEnd,
            Tier1Color.GetColor(),
            Tier2Color.GetColor(),
            Tier3Color.GetColor(),
            Tier1Width,
            Tier2Width,
            Tier3Width))
    {
        return;
    }

    DeleteVectorDrawings(sc, State->DrawingLineNumbers);

    for (const ProfileNode& Row : QualifyingRows)
    {
        COLORREF Color = Tier3Color.GetColor();
        int LineWidth = Tier3Width;
        int LineStyle = LINESTYLE_DOT;

        if (Row.Tier == 1)
        {
            Color = Tier1Color.GetColor();
            LineWidth = Tier1Width;
            LineStyle = LINESTYLE_SOLID;
        }
        else if (Row.Tier == 2)
        {
            Color = Tier2Color.GetColor();
            LineWidth = Tier2Width;
            LineStyle = LINESTYLE_DASH;
        }

        const double Price = static_cast<double>(Row.PriceInTicks) * sc.TickSize;

        s_UseTool Ray;
        Ray.Clear();
        Ray.ChartNumber = sc.ChartNumber;
        Ray.DrawingType = DRAWING_HORIZONTAL_RAY;
        Ray.AddMethod = UTAM_ADD_ALWAYS;
        Ray.Region = 0;
        Ray.BeginIndex = RayBeginIndex;
        Ray.BeginValue = Price;
        Ray.Color = Color;
        Ray.LineWidth = LineWidth;
        Ray.LineStyle = LineStyle;
        Ray.DisplayHorizontalLineValue = ShowPrice;
        Ray.AddAsUserDrawnDrawing = 0;

        const int RayLineNumber = AddStudyDrawing(sc, Ray);
        if (RayLineNumber != 0)
            State->DrawingLineNumbers.push_back(RayLineNumber);

        if (ShowLabels)
        {
            SCString Label;
            if (Row.Tier == 1)
                Label.Format("Tier 1 | %.1f%% of POC", Row.PercentOfPOC);
            else if (Row.Tier == 2)
                Label.Format("Tier 2 | %.1f%% of POC", Row.PercentOfPOC);
            else
                Label.Format("Tier 3 | %.1f%% of POC", Row.PercentOfPOC);

            s_UseTool Text;
            Text.Clear();
            Text.ChartNumber = sc.ChartNumber;
            Text.DrawingType = DRAWING_TEXT;
            Text.AddMethod = UTAM_ADD_ALWAYS;
            Text.Region = 0;
            Text.BeginIndex = RayBeginIndex;
            Text.BeginValue = Price;
            Text.Color = Color;
            Text.FontSize = 8;
            Text.FontBold = Row.Tier == 1 ? 1 : 0;
            Text.Text = Label;
            Text.AddAsUserDrawnDrawing = 0;

            const int TextLineNumber = AddStudyDrawing(sc, Text);
            if (TextLineNumber != 0)
                State->DrawingLineNumbers.push_back(TextLineNumber);
        }
    }

    State->LastNodes = Signatures;
    State->LastPOCVolume = ComparablePOCVolume;
    State->LastSourceStudyID = SourceStudyID;
    State->LastProfileIndex = RequestedProfileIndex;
    State->LastBeginIndex = RayBeginIndex;
    State->LastUpperFloor = UpperFloor;
    State->LastMiddleFloor = MiddleFloor;
    State->LastLowerFloor = LowerFloor;
    State->LastShowPrice = ShowPrice;
    State->LastShowLabels = ShowLabels;
    State->LastStartAtProfileEnd = StartAtEnd;
    State->LastTier1Color = Tier1Color.GetColor();
    State->LastTier2Color = Tier2Color.GetColor();
    State->LastTier3Color = Tier3Color.GetColor();
    State->LastTier1Width = Tier1Width;
    State->LastTier2Width = Tier2Width;
    State->LastTier3Width = Tier3Width;
}
