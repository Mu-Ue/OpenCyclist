#pragma once

#include <d2d1.h>
#include <dwrite.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <winrt/base.h>

#include "sim/fit.h"
#include "sim/ride.h"

// How a device is doing, which sets its color.
enum class Tone { Good, Waiting, Problem };

// The device for one role, as the setup and devices screens show it.
struct DeviceInfo {
    std::wstring_view role;
    std::wstring name;  // empty when none is set
    std::wstring status;
    Tone tone = Tone::Waiting;
};

// One setting on the setup screen.
struct SetupRow {
    std::wstring_view label;
    std::wstring value;
};

// A ride from before, in the setup screen's history and on the history screen.
struct PastRide {
    std::wstring name;    // the log's file name
    std::wstring date;
    std::wstring course;  // the route and its settings
    LogTotals totals;
};

// What the setup screen shows. The app owns the data.
struct SetupView {
    std::span<const SetupRow> rows;
    int selected = 0;
    const Course* course = nullptr;
    std::span<const std::wstring> stats;  // under the course preview, such as its rough time
    std::span<const DeviceInfo> devices;
    std::span<const PastRide> history;  // newest first
};

// Where the setup screen's parts are, in DIPs, for mouse hits.
struct SetupLayout {
    static constexpr size_t kMaxRows = 10;
    D2D1_RECT_F panel{}, rows[kMaxRows]{};
    D2D1_RECT_F preview{}, history{}, devices{}, reroll{}, start{};
    float valueLeft(size_t row) const { return (rows[row].left + rows[row].right) / 2; }  // values sit on the right half
};

SetupLayout LayoutSetup(D2D1_SIZE_F size, size_t rows);

// A device found nearby, on the devices screen.
struct NearbyRow {
    std::wstring_view role, name;
};

// What the devices screen shows.
struct DevicesView {
    std::span<const DeviceInfo> cards;  // one per role
    std::span<const NearbyRow> nearby;
    int focus = 0;                // the cards, then the nearby rows
    std::wstring_view bluetooth;  // the radio's state
    Tone radio = Tone::Waiting;
    std::wstring_view action;     // a button to fix the radio, such as "Turn on", or empty
    std::wstring_view footer;
};

// Where the devices screen's parts are, in DIPs, for mouse hits.
struct DevicesLayout {
    static constexpr size_t kCards = 3, kMaxNearby = 8;
    D2D1_RECT_F banner{}, action{}, cards[kCards]{}, disconnect[kCards]{}, nearby[kMaxNearby]{}, next{};
    float nearbyTop = 0;
    size_t nearbyRows = 0;  // the rows that fit
};

DevicesLayout LayoutDevices(D2D1_SIZE_F size, size_t nearby);

// What the summary screen after a ride shows.
struct SummaryView {
    const Ride* ride = nullptr;
    std::wstring_view title, date;
    std::wstring_view note;  // where the ride was saved
    std::wstring_view file;  // the button to show the saved file, hidden when empty
};

struct SummaryLayout {
    D2D1_RECT_F done{}, file{};
};

SummaryLayout LayoutSummary(D2D1_SIZE_F size);

// What the history screen shows: a list of past rides, and more about the selected one.
struct HistoryView {
    std::span<const PastRide> rides;     // newest first
    size_t selected = 0, first = 0;      // first: the ride in the top row
    std::span<const LogSample> samples;  // the selected ride's, for its graph
    const Course* route = nullptr;       // the course it was ridden on
    bool confirmDelete = false;
};

struct HistoryLayout {
    static constexpr size_t kMaxRows = 16;
    D2D1_RECT_F list{}, rows[kMaxRows]{}, detail{}, remove{}, file{}, back{};
    size_t rowsShown = 0;  // the rows that fit
};

HistoryLayout LayoutHistory(D2D1_SIZE_F size);

// The ride screen's parts, in DIPs, and the question before ending the ride. Number cards sit in slots along the
// top, on both sides of the two graphs and along the bottom. In ERG mode the target sits right of the upper graph,
// with arrows over and under its three digits.
struct RideLayout {
    static constexpr int kTop = 5, kSide = 2, kBottom = 5, kTiles = kTop + 2 * kSide + kBottom, kGraphs = 2,
                         kSlots = kTiles + kGraphs;
    // The top row, the left side, the right side and the bottom row, each from left or top.
    D2D1_RECT_F tiles[kTiles]{}, graphs[kGraphs]{}, footer{}, target{}, up[3]{}, down[3]{}, pause{}, end{};
    // A slot is a tile, then a graph.
    D2D1_RECT_F Slot(int i) const { return i < kTiles ? tiles[i] : graphs[i - kTiles]; }
    int Hit(D2D1_POINT_2F p) const;  // the slot under p, or -1
    static bool Swappable(int a, int b) { return (a < kTiles) == (b < kTiles); }
};
RideLayout LayoutRide(D2D1_SIZE_F size, bool erg);

// The numbers and graphs of the ride screen. Saved by number, so new ones go at the end.
enum class Tile : uint8_t {
    Power, Cadence, Speed, Grade, Gear, HeartRate, AverageHeartRate, MaxPower, AverageCadence, Distance, Climbed,
    AveragePower, Calories, Time
};
enum class Graph : uint8_t { Next, Course };

// What sits in each of the ride screen's slots, which riders rearrange by dragging one onto another.
struct Board {
    uint8_t slots[RideLayout::kSlots];  // a Tile for each tile slot, then a Graph for each graph slot
    Board();                             // in the order of the enums
    static Board Parse(std::wstring_view text);  // the default board when the text isn't one
    std::wstring Format() const;
    Tile tile(int slot) const { return static_cast<Tile>(slots[slot]); }
    Graph graph(int slot) const { return static_cast<Graph>(slots[slot]); }
};

// A slot being dragged to the point `at`. It lifts once the mouse has moved a little, so a click isn't a drag.
struct RideDrag {
    int from = -1;
    D2D1_POINT_2F start{}, at{};
    bool lifted = false;
};
struct ConfirmLayout {
    D2D1_RECT_F box{}, yes{}, no{};
};
ConfirmLayout LayoutConfirm(D2D1_SIZE_F size, bool grow = true);  // grow as the ride screen does

// Draws the screens into any Direct2D target: a window, or offscreen for a snapshot.
class Renderer {
public:
    explicit Renderer(IDWriteFactory* dwrite);

    // Call between BeginDraw and EndDraw. `status` is shown when not empty, such as a lost trainer. `confirmEnd`
    // asks whether to end the ride. A lifted `drag` outlines where it can go.
    void Draw(ID2D1RenderTarget* target, const Ride& ride, const Board& board, std::wstring_view status,
              bool confirmEnd = false, const RideDrag* drag = nullptr);
    void DrawSetup(ID2D1RenderTarget* target, const SetupView& view);
    void DrawDevices(ID2D1RenderTarget* target, const DevicesView& view);
    void DrawSummary(ID2D1RenderTarget* target, const SummaryView& view);
    void DrawHistory(ID2D1RenderTarget* target, const HistoryView& view);

private:
    enum class Button { Primary, Secondary };

    float Prepare(ID2D1RenderTarget* target, bool grow = false);
    void DrawHeader(ID2D1RenderTarget* target, std::wstring_view title, std::wstring_view hint);
    void DrawCard(ID2D1RenderTarget* target, D2D1_RECT_F box, bool focused = false);
    void DrawStat(ID2D1RenderTarget* target, D2D1_RECT_F box, std::wstring_view label, std::wstring_view value,
                  std::wstring_view unit, float valueSize, ID2D1Brush* valueBrush);
    void DrawProfile(ID2D1RenderTarget* target, const Course& course, double from, double to, double position,
                     D2D1_RECT_F box, double minSpan, bool graded);
    void DrawRideGraph(ID2D1RenderTarget* target, std::span<const LogSample> samples, D2D1_RECT_F box);
    void DrawConfirm(ID2D1RenderTarget* target, const ConfirmLayout& box, std::wstring_view title,
                     std::wstring_view text, std::wstring_view no, std::wstring_view yes);
    void DrawButton(ID2D1RenderTarget* target, D2D1_RECT_F box, std::wstring_view text, Button kind,
                    IDWriteTextFormat* format, bool enabled = true);
    void DrawArrow(ID2D1RenderTarget* target, D2D1_RECT_F box, bool up);
    void DrawPill(ID2D1RenderTarget* target, D2D1_POINT_2F center, std::wstring_view text, ID2D1Brush* brush);
    void DrawDot(ID2D1RenderTarget* target, D2D1_POINT_2F center, Tone tone);
    ID2D1SolidColorBrush* ToneBrush(Tone tone) const;
    ID2D1SolidColorBrush* GradeBrush(double grade) const;
    void DrawLabel(ID2D1RenderTarget* target, std::wstring_view text, IDWriteTextFormat* format, D2D1_RECT_F box,
                  ID2D1Brush* brush, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    float TextWidth(std::wstring_view text, IDWriteTextFormat* format);

    winrt::com_ptr<IDWriteFactory> dwrite_;
    winrt::com_ptr<ID2D1Factory> factory_;
    std::wstring textFont_, numberFont_;
    winrt::com_ptr<IDWriteTypography> tabular_;  // digits of one width, so live numbers don't jiggle
    // Text sizes follow the layout scale; the formats are remade when it changes.
    float scale_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> label_, small_, row_, title_, heading_, button_, digit_;
    // Brushes belong to one target; they're remade when the target changes. Holding it keeps its address unique.
    winrt::com_ptr<ID2D1RenderTarget> brushTarget_;
    winrt::com_ptr<ID2D1SolidColorBrush> background_, surface_, raised_, border_, text_, muted_, faint_, accent_, accentText_,
        good_, warn_, grades_[6];
    winrt::com_ptr<ID2D1LinearGradientBrush> backdrop_, rideFill_, trackFill_, fade_;
    // The elevations of the last profile drawn of a whole course, which stays put while the rider moves.
    struct ProfileCache {
        CourseParams params;
        size_t samples = 0;
        double from = 0, to = 0;
        winrt::com_ptr<ID2D1PathGeometry> fill, line;
        float width = 0, height = 0;
    } cache_[2];
    int nextCache_ = 0;
    std::vector<float> heights_;  // reused between frames
};
