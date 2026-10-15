#include "ui/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "config.h"

namespace {

// Dark and flat. The accent matches the icon.
constexpr UINT32 kBackground = 0x111418, kText = 0xE6E8EB, kMuted = 0x8A9099, kAccent = 0xF97316,
                 kTrack = 0x262C34, kRidden = 0x7C3A12;

// Layout sizes are in DIPs for a 1280 x 720 window and scale with it.
float Scale(D2D1_SIZE_F size) { return std::max(0.1f, std::min(size.width / 1280.0f, size.height / 720.0f)); }

winrt::com_ptr<IDWriteTextFormat> MakeFormat(IDWriteFactory* dwrite, DWRITE_FONT_WEIGHT weight, float size,
                                             bool centered = false) {
    winrt::com_ptr<IDWriteTextFormat> format;
    winrt::check_hresult(dwrite->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                  DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", format.put()));
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (centered) format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return format;
}

}  // namespace

SetupLayout LayoutSetup(D2D1_SIZE_F size, size_t rows) {
    const float s = Scale(size), pad = 32.0f * s;
    const float top = pad + 72.0f * s, rowHeight = 46.0f * s, bottom = size.height - pad;
    const float left = (size.width - 2 * pad) * 0.42f;
    SetupLayout l;
    for (size_t i = 0; i < std::min(rows, SetupLayout::kMaxRows); ++i)
        l.rows[i] = {pad, top + rowHeight * i, pad + left, top + rowHeight * (i + 1) - 6.0f * s};
    l.start = {size.width - pad - 180.0f * s, bottom - 52.0f * s, size.width - pad, bottom};
    l.reroll = {l.start.left - 196.0f * s, l.start.top, l.start.left - 16.0f * s, bottom};
    l.preview = {pad + left + 56.0f * s, top + 28.0f * s, size.width - pad, l.start.top - 96.0f * s};
    return l;
}

Renderer::Renderer(IDWriteFactory* dwrite) {
    dwrite_.copy_from(dwrite);
    value_ = MakeFormat(dwrite, DWRITE_FONT_WEIGHT_SEMI_BOLD, 56.0f);
}

// Brushes for this target and text formats for its size. Returns the layout scale.
float Renderer::Prepare(ID2D1RenderTarget* target) {
    if (brushTarget_.get() != target) {
        auto make = [&](UINT32 rgb, winrt::com_ptr<ID2D1SolidColorBrush>& brush) {
            brush = nullptr;
            winrt::check_hresult(target->CreateSolidColorBrush(D2D1::ColorF(rgb), brush.put()));
        };
        make(kBackground, background_);
        make(kText, text_);
        make(kMuted, muted_);
        make(kAccent, accent_);
        make(kTrack, track_);
        make(kRidden, ridden_);
        brushTarget_.copy_from(target);
    }
    const float s = Scale(target->GetSize());
    if (s != scale_) {
        label_ = MakeFormat(dwrite_.get(), DWRITE_FONT_WEIGHT_NORMAL, 15.0f * s);
        row_ = MakeFormat(dwrite_.get(), DWRITE_FONT_WEIGHT_NORMAL, 19.0f * s, true);
        title_ = MakeFormat(dwrite_.get(), DWRITE_FONT_WEIGHT_SEMI_BOLD, 30.0f * s, true);
        scale_ = s;
    }
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    target->Clear(D2D1::ColorF(kBackground));
    return s;
}

void Renderer::Draw(ID2D1RenderTarget* target, const Ride& ride, std::wstring_view status) {
    const float s = Prepare(target), pad = 32.0f * s;
    const D2D1_SIZE_F size = target->GetSize();

    // Live numbers in five columns. The last one depends on the mode.
    wchar_t power[16], cadence[16], speed[16], grade[16], last[16];
    swprintf_s(power, L"%d", ride.power());
    swprintf_s(cadence, L"%.0f", ride.cadence());
    swprintf_s(speed, L"%.1f", ride.speed() * 3.6);
    swprintf_s(grade, L"%.1f", ride.grade() * 100);
    const wchar_t* lastLabel = L"Gear";
    const wchar_t* lastUnit = L"";
    switch (ride.mode()) {
    case Mode::Sim: swprintf_s(last, L"%d", ride.gear()); break;
    case Mode::Erg: swprintf_s(last, L"%d", ride.ergW()), lastLabel = L"Target", lastUnit = L"W"; break;
    case Mode::Resistance: swprintf_s(last, L"%d", ride.resistancePct()), lastLabel = L"Resistance", lastUnit = L"%"; break;
    }
    const float colWidth = (size.width - 2 * pad) / 5;
    const float top = pad, metricsBottom = top + 100.0f * s;
    const std::wstring_view labels[] = {L"Power", L"Cadence", L"Speed", L"Grade", lastLabel};
    const std::wstring_view values[] = {power, cadence, speed, grade, last};
    const std::wstring_view units[] = {L"W", L"rpm", L"km/h", L"%", lastUnit};
    for (int i = 0; i < 5; ++i) {
        const float x = pad + colWidth * i;
        DrawMetric(target, {x, top, x + colWidth, metricsBottom}, labels[i], values[i], units[i], s);
    }

    // The next 2 km in detail, starting a little behind the rider.
    const double length = ride.course().Length();
    const double from = std::clamp(ride.distance() - 250.0, 0.0, std::max(0.0, length - 2250.0));
    const D2D1_RECT_F next{pad, metricsBottom + 40.0f * s, size.width - pad, size.height * 0.60f};
    DrawProfile(target, ride.course(), from, std::min(length, from + 2250.0), ride.distance(), next, 30.0);
    wchar_t caption[64];
    swprintf_s(caption, L"Next 2 km · %.0f m", ride.elevation());
    DrawLabel(target, caption, label_.get(), {next.left, next.top - 26.0f * s, next.right, next.top}, muted_.get());
    if (ride.finished()) {
        DrawLabel(target, L"Finished", title_.get(), {next.left, next.top, next.right, next.top + 56.0f * s},
                  accent_.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawLabel(target, L"Enter or A for a new ride", label_.get(),
                  {next.left, next.top + 56.0f * s, next.right, next.top + 80.0f * s}, text_.get(),
                  DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // The whole course.
    const D2D1_RECT_F whole{pad, size.height * 0.67f, size.width - pad, size.height - 84.0f * s};
    DrawProfile(target, ride.course(), 0.0, length, ride.distance(), whole, 50.0);
    swprintf_s(caption, L"Course · seed %u", ride.course().params.seed);
    DrawLabel(target, caption, label_.get(), {whole.left, whole.top - 26.0f * s, whole.right, whole.top}, muted_.get());

    // Footer: progress on the left, climbing in the middle, time and state on the right.
    const int t = static_cast<int>(ride.elapsed());
    wchar_t distance[48], climbed[48], time[48];
    swprintf_s(distance, L"%.1f / %.1f km", ride.distance() / 1000, length / 1000);
    swprintf_s(climbed, L"%.0f / %.0f m up", ride.climbed(), ride.course().targetGain);
    swprintf_s(time, L"%d:%02d:%02d%s", t / 3600, t / 60 % 60, t % 60,
               ride.finished() ? L" · Finished" : ride.paused() ? L" · Paused" : L"");
    const D2D1_RECT_F footer{pad, size.height - 56.0f * s, size.width - pad, size.height - pad * 0.5f};
    DrawLabel(target, distance, label_.get(), footer, text_.get());
    DrawLabel(target, climbed, label_.get(), footer, text_.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawLabel(target, status.empty() ? std::wstring_view(time) : status, label_.get(), footer,
             status.empty() ? text_.get() : accent_.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void Renderer::DrawSetup(ID2D1RenderTarget* target, const SetupView& view) {
    const float s = Prepare(target), pad = 32.0f * s;
    const D2D1_SIZE_F size = target->GetSize();
    const SetupLayout l = LayoutSetup(size, view.rows.size());

    DrawLabel(target, kAppName, title_.get(), {pad, pad, size.width - pad, pad + 44.0f * s}, text_.get());
    DrawLabel(target, L"↑↓ choose · ←→ change · R new course · Enter start · F11 full screen", label_.get(),
              {pad, pad + 12.0f * s, size.width - pad, pad + 44.0f * s}, muted_.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);

    // Settings on the left. The selected one is highlighted, with arrows to change it.
    const float inset = 14.0f * s;
    for (size_t i = 0; i < std::min(view.rows.size(), SetupLayout::kMaxRows); ++i) {
        const D2D1_RECT_F r = l.rows[i];
        const D2D1_RECT_F value{l.valueLeft(i), r.top, r.right - inset, r.bottom};
        const bool selected = static_cast<int>(i) == view.selected;
        if (selected) target->FillRoundedRectangle(D2D1::RoundedRect(r, 6.0f * s, 6.0f * s), track_.get());
        DrawLabel(target, view.rows[i].label, row_.get(), {r.left + inset, r.top, value.left, r.bottom},
                  selected ? text_.get() : muted_.get());
        DrawLabel(target, view.rows[i].value, row_.get(), value, text_.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        if (selected) {
            DrawLabel(target, L"‹", row_.get(), value, accent_.get());
            DrawLabel(target, L"›", row_.get(), value, accent_.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
    }

    // The course on the right, with what it takes to ride it underneath.
    if (view.course) {
        const D2D1_RECT_F p = l.preview;
        wchar_t caption[64];
        swprintf_s(caption, L"Course · seed %u", view.course->params.seed);
        DrawLabel(target, caption, label_.get(), {p.left, p.top - 26.0f * s, p.right, p.top}, muted_.get());
        DrawProfile(target, *view.course, 0.0, view.course->Length(), 0.0, p, 50.0);
        DrawLabel(target, view.summary, row_.get(), {p.left, p.bottom + 12.0f * s, p.right, p.bottom + 44.0f * s},
                  text_.get());
    }

    // Connections at the bottom left, buttons at the bottom right.
    const float lineHeight = 24.0f * s;
    DrawLabel(target, view.trainer, label_.get(), {pad, l.start.top, l.reroll.left, l.start.top + lineHeight},
              view.trainerReady ? text_.get() : accent_.get());
    DrawLabel(target, view.shifters, label_.get(), {pad, l.start.top + lineHeight, l.reroll.left, l.start.bottom},
              view.shiftersReady ? text_.get() : muted_.get());
    target->FillRoundedRectangle(D2D1::RoundedRect(l.reroll, 6.0f * s, 6.0f * s), track_.get());
    DrawLabel(target, L"New course", row_.get(), l.reroll, text_.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    target->FillRoundedRectangle(D2D1::RoundedRect(l.start, 6.0f * s, 6.0f * s), accent_.get());
    DrawLabel(target, L"Start ride", row_.get(), l.start, background_.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
}

void Renderer::DrawMetric(ID2D1RenderTarget* target, D2D1_RECT_F box, std::wstring_view label, std::wstring_view value,
                          std::wstring_view unit, float s) {
    DrawLabel(target, label, label_.get(), {box.left, box.top, box.right, box.top + 22.0f * s}, muted_.get());
    // The unit sits after the value in a smaller size, in one layout so they share a baseline.
    std::wstring text(value);
    if (!unit.empty()) text.append(L" ").append(unit);
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), value_.get(),
                                         box.right - box.left, 100.0f * s, layout.put())))
        return;
    layout->SetFontSize(56.0f * s, {0, static_cast<UINT32>(text.size())});
    if (!unit.empty())
        layout->SetFontSize(20.0f * s, {static_cast<UINT32>(value.size()), static_cast<UINT32>(unit.size() + 1)});
    target->DrawTextLayout({box.left, box.top + 18.0f * s}, layout.get(), text_.get());
}

// An elevation profile from `from` to `to` metres, filled below the line. The ridden part is darker, and a
// marker shows the rider. Flat stretches stay flat: the vertical scale covers at least `minSpan` metres.
void Renderer::DrawProfile(ID2D1RenderTarget* target, const Course& course, double from, double to, double position,
                           D2D1_RECT_F box, double minSpan) {
    if (to <= from || box.right <= box.left || box.bottom <= box.top) return;
    const int columns = std::max(2, static_cast<int>(box.right - box.left) / 2);
    std::vector<double> heights(columns);
    double lo = 1e9, hi = -1e9;
    for (int i = 0; i < columns; ++i) {
        heights[i] = course.ElevationAt(from + (to - from) * i / (columns - 1));
        lo = std::min(lo, heights[i]);
        hi = std::max(hi, heights[i]);
    }
    const double span = std::max(hi - lo, minSpan) * 1.15;
    const double base = (lo + hi) / 2 - span / 2;
    const float width = box.right - box.left, height = box.bottom - box.top;
    auto y = [&](double e) { return box.bottom - static_cast<float>((e - base) / span) * height; };

    winrt::com_ptr<ID2D1Factory> factory;
    target->GetFactory(factory.put());
    winrt::com_ptr<ID2D1PathGeometry> path;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) return;
    sink->BeginFigure({box.left, box.bottom}, D2D1_FIGURE_BEGIN_FILLED);
    for (int i = 0; i < columns; ++i)
        sink->AddLine({box.left + width * i / (columns - 1), y(heights[i])});
    sink->AddLine({box.right, box.bottom});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();

    const float x = box.left + static_cast<float>(std::clamp((position - from) / (to - from), 0.0, 1.0)) * width;
    target->FillGeometry(path.get(), track_.get());
    target->PushAxisAlignedClip({box.left, box.top, x, box.bottom}, D2D1_ANTIALIAS_MODE_ALIASED);
    target->FillGeometry(path.get(), ridden_.get());
    target->PopAxisAlignedClip();

    const float markerY = y(course.ElevationAt(position));
    const float marker = std::max(1.0f, scale_);
    target->DrawLine({x, markerY}, {x, box.bottom}, accent_.get(), 2.0f * marker);
    target->FillEllipse(D2D1::Ellipse({x, markerY}, 6.0f * marker, 6.0f * marker), accent_.get());
}

void Renderer::DrawLabel(ID2D1RenderTarget* target, std::wstring_view text, IDWriteTextFormat* format, D2D1_RECT_F box,
                        ID2D1Brush* brush, DWRITE_TEXT_ALIGNMENT align) {
    format->SetTextAlignment(align);
    target->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format, box, brush);
}
