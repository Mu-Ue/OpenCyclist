#pragma once

#include <d2d1.h>
#include <dwrite.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <winrt/base.h>

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

// What the setup screen shows. The app owns the data.
struct SetupView {
    std::span<const SetupRow> rows;
    int selected = 0;
    const Course* course = nullptr;
    std::span<const std::wstring> stats;  // under the course preview, such as its length and gain
    std::span<const DeviceInfo> devices;
};

// Where the setup screen's parts are, in DIPs, for mouse hits.
struct SetupLayout {
    static constexpr size_t kMaxRows = 10;
    D2D1_RECT_F panel{}, rows[kMaxRows]{};
    D2D1_RECT_F preview{}, devices{}, reroll{}, start{};
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

// The ride screen's button to end the ride, and the question that follows, in DIPs.
D2D1_RECT_F LayoutRideEnd(D2D1_SIZE_F size);
struct ConfirmLayout {
    D2D1_RECT_F box{}, yes{}, no{};
};
ConfirmLayout LayoutConfirm(D2D1_SIZE_F size);

// Draws the screens into any Direct2D target: a window, or offscreen for a snapshot.
class Renderer {
public:
    explicit Renderer(IDWriteFactory* dwrite);

    // Call between BeginDraw and EndDraw. `status` is shown when not empty, such as a lost trainer. `confirmEnd`
    // asks whether to end the ride.
    void Draw(ID2D1RenderTarget* target, const Ride& ride, std::wstring_view status, bool confirmEnd = false);
    void DrawSetup(ID2D1RenderTarget* target, const SetupView& view);
    void DrawDevices(ID2D1RenderTarget* target, const DevicesView& view);
    void DrawSummary(ID2D1RenderTarget* target, const SummaryView& view);

private:
    enum class Button { Primary, Secondary };

    float Prepare(ID2D1RenderTarget* target, bool grow = false);
    void DrawHeader(ID2D1RenderTarget* target, std::wstring_view title, std::wstring_view hint);
    void DrawCard(ID2D1RenderTarget* target, D2D1_RECT_F box, bool focused = false);
    void DrawStat(ID2D1RenderTarget* target, D2D1_RECT_F box, std::wstring_view label, std::wstring_view value,
                  std::wstring_view unit, float valueSize, ID2D1Brush* valueBrush);
    void DrawProfile(ID2D1RenderTarget* target, const Course& course, double from, double to, double position,
                     D2D1_RECT_F box, double minSpan, bool graded);
    void DrawButton(ID2D1RenderTarget* target, D2D1_RECT_F box, std::wstring_view text, Button kind,
                    IDWriteTextFormat* format, bool enabled = true);
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
    winrt::com_ptr<IDWriteTextFormat> label_, small_, row_, title_, heading_, button_;
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
