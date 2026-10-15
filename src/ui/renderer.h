#pragma once

#include <d2d1.h>
#include <dwrite.h>

#include <span>
#include <string>
#include <string_view>

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
    std::wstring_view summary;  // under the course preview
    std::span<const DeviceInfo> devices;
};

// Where the setup screen's parts are, in DIPs, for mouse hits.
struct SetupLayout {
    static constexpr size_t kMaxRows = 10;
    D2D1_RECT_F rows[kMaxRows]{};
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
    std::wstring_view action;     // a button to fix the radio, such as "Turn on", or empty
    std::wstring_view footer;
    std::wstring_view refresh = L"Refresh";  // the button, which shows a search under way
    bool searching = false;                  // just refreshed
};

// Where the devices screen's parts are, in DIPs, for mouse hits.
struct DevicesLayout {
    static constexpr size_t kCards = 3, kMaxNearby = 8;
    D2D1_RECT_F banner{}, action{}, cards[kCards]{}, disconnect[kCards]{}, nearby[kMaxNearby]{}, refresh{}, next{};
    float nearbyTop = 0;
    size_t nearbyRows = 0;  // the rows that fit
};

DevicesLayout LayoutDevices(D2D1_SIZE_F size, size_t nearby);

// Draws the screens into any Direct2D target: a window, or offscreen for a snapshot.
class Renderer {
public:
    explicit Renderer(IDWriteFactory* dwrite);

    // Call between BeginDraw and EndDraw. `status` is shown when not empty, such as a lost trainer.
    void Draw(ID2D1RenderTarget* target, const Ride& ride, std::wstring_view status);
    void DrawSetup(ID2D1RenderTarget* target, const SetupView& view);
    void DrawDevices(ID2D1RenderTarget* target, const DevicesView& view);

private:
    float Prepare(ID2D1RenderTarget* target);
    void DrawMetric(ID2D1RenderTarget* target, D2D1_RECT_F box, std::wstring_view label, std::wstring_view value,
                    std::wstring_view unit, float scale);
    void DrawProfile(ID2D1RenderTarget* target, const Course& course, double from, double to, double position,
                     D2D1_RECT_F box, double minSpan);
    void DrawButton(ID2D1RenderTarget* target, D2D1_RECT_F box, std::wstring_view text, bool primary,
                    IDWriteTextFormat* format);
    void DrawDot(ID2D1RenderTarget* target, D2D1_POINT_2F center, Tone tone);
    ID2D1Brush* TextBrush(Tone tone) const;
    void DrawLabel(ID2D1RenderTarget* target, std::wstring_view text, IDWriteTextFormat* format, D2D1_RECT_F box,
                  ID2D1Brush* brush, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);

    winrt::com_ptr<IDWriteFactory> dwrite_;
    // Text sizes follow the window size; the formats are remade when it changes.
    float scale_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> value_, label_, small_, row_, title_;
    // Brushes belong to one target; they're remade when the target changes. Holding it keeps its address unique.
    winrt::com_ptr<ID2D1RenderTarget> brushTarget_;
    winrt::com_ptr<ID2D1SolidColorBrush> background_, panel_, text_, muted_, accent_, good_, track_, ridden_;
};
