#pragma once

#include <d2d1.h>
#include <dwrite.h>

#include <span>
#include <string>
#include <string_view>

#include <winrt/base.h>

#include "sim/ride.h"

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
    std::wstring_view trainer, shifters;
    bool trainerReady = false, shiftersReady = false;
};

// Where the setup screen's parts are, in DIPs, for mouse hits.
struct SetupLayout {
    static constexpr size_t kMaxRows = 10;
    D2D1_RECT_F rows[kMaxRows]{};
    D2D1_RECT_F preview{}, reroll{}, start{};
    float valueLeft(size_t row) const { return (rows[row].left + rows[row].right) / 2; }  // values sit on the right half
};

SetupLayout LayoutSetup(D2D1_SIZE_F size, size_t rows);

// Draws the setup screen and the ride view into any Direct2D target: a window, or offscreen for a snapshot.
class Renderer {
public:
    explicit Renderer(IDWriteFactory* dwrite);

    // Call between BeginDraw and EndDraw. `status` is shown when not empty, such as a lost trainer.
    void Draw(ID2D1RenderTarget* target, const Ride& ride, std::wstring_view status);
    void DrawSetup(ID2D1RenderTarget* target, const SetupView& view);

private:
    float Prepare(ID2D1RenderTarget* target);
    void DrawMetric(ID2D1RenderTarget* target, D2D1_RECT_F box, std::wstring_view label, std::wstring_view value,
                    std::wstring_view unit, float scale);
    void DrawProfile(ID2D1RenderTarget* target, const Course& course, double from, double to, double position,
                     D2D1_RECT_F box, double minSpan);
    void DrawLabel(ID2D1RenderTarget* target, std::wstring_view text, IDWriteTextFormat* format, D2D1_RECT_F box,
                  ID2D1Brush* brush, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);

    winrt::com_ptr<IDWriteFactory> dwrite_;
    // Text sizes follow the window size; the formats are remade when it changes.
    float scale_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> value_, label_, row_, title_;
    // Brushes belong to one target; they're remade when the target changes. Holding it keeps its address unique.
    winrt::com_ptr<ID2D1RenderTarget> brushTarget_;
    winrt::com_ptr<ID2D1SolidColorBrush> background_, text_, muted_, accent_, track_, ridden_;
};
