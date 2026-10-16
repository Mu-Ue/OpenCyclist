#include "ui/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

#include "config.h"

namespace {

// Black, dark gray and white. What matters is brighter: white for the main action, what works and what needs a
// look, gray for the rest. Grades go from dark gray downhill to near white on the steepest walls.
constexpr UINT32 kBackdropTop = 0x141414, kBackground = 0x0A0A0A, kSurface = 0x151515, kRaised = 0x232323,
                 kBorder = 0x2C2C2C, kText = 0xF2F2F2, kMuted = 0x9A9A9A, kFaint = 0x5C5C5C, kAccent = 0xF2F2F2,
                 kAccentText = 0xFFFFFF, kGood = 0xF2F2F2, kWarn = 0xFFFFFF;
constexpr UINT32 kGrades[] = {0x2E2E2E, 0x404040, 0x5E5E5E, 0x838383, 0xABABAB, 0xD6D6D6};
constexpr double kGradeSteps[] = {-0.02, 0.03, 0.06, 0.09, 0.12};  // where each grade shade starts

// Layout sizes are in DIPs for a 1280 x 720 window and shrink with it. Only the ride screen grows past that, to be
// read from the bike; the menus keep their size and spread out instead.
float Scale(D2D1_SIZE_F size, bool grow = false) {
    const float fit = std::min(size.width / 1280.0f, size.height / 720.0f);
    return std::max(0.1f, grow ? fit : std::min(fit, 1.0f));
}

bool HasFont(IDWriteFactory* dwrite, const wchar_t* family) {
    winrt::com_ptr<IDWriteFontCollection> fonts;
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(dwrite->GetSystemFontCollection(fonts.put())) &&
           SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists;
}

winrt::com_ptr<IDWriteTextFormat> MakeFormat(IDWriteFactory* dwrite, const std::wstring& font, DWRITE_FONT_WEIGHT weight,
                                             float size) {
    winrt::com_ptr<IDWriteTextFormat> format;
    winrt::check_hresult(dwrite->CreateTextFormat(font.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                  DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", format.put()));
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // Text too long for its box, such as a long device name, ends in an ellipsis.
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    winrt::com_ptr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(format.get(), ellipsis.put())))
        format->SetTrimming(&trimming, ellipsis.get());
    return format;
}

// A round step near `raw`: 1, 2 or 5 times a power of ten.
double NiceStep(double raw) {
    const double power = std::pow(10.0, std::floor(std::log10(std::max(raw, 1e-6))));
    const double f = raw / power;
    return power * (f < 1.5 ? 1 : f < 3.5 ? 2 : f < 7.5 ? 5 : 10);
}

D2D1_RECT_F Inset(D2D1_RECT_F r, float x, float y) { return {r.left + x, r.top + y, r.right - x, r.bottom - y}; }

std::wstring Clock(double seconds) {
    const int t = static_cast<int>(seconds);
    wchar_t buf[16];
    swprintf_s(buf, L"%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    return buf;
}

}  // namespace

SetupLayout LayoutSetup(D2D1_SIZE_F size, size_t rows) {
    const float s = Scale(size), pad = 32.0f * s, bottom = size.height - pad, right = size.width - pad;
    const float rowHeight = 46.0f * s, inset = 8.0f * s;
    SetupLayout l;
    const size_t shown = std::min(rows, SetupLayout::kMaxRows);
    l.panel = {pad, pad + 64.0f * s, pad + (right - pad) * 0.40f, pad + 64.0f * s + rowHeight * shown + 2 * inset};
    for (size_t i = 0; i < shown; ++i) {
        const float top = l.panel.top + inset + rowHeight * i;
        l.rows[i] = {l.panel.left + inset, top, l.panel.right - inset, top + rowHeight - 4.0f * s};
    }
    l.start = {right - 180.0f * s, bottom - 52.0f * s, right, bottom};
    l.reroll = {l.start.left - 172.0f * s, l.start.top, l.start.left - 12.0f * s, bottom};
    l.devices = {l.reroll.left - 172.0f * s, l.start.top, l.reroll.left - 12.0f * s, bottom};
    l.preview = {l.panel.right + 16.0f * s, l.panel.top, right, l.start.top - 24.0f * s};
    return l;
}

DevicesLayout LayoutDevices(D2D1_SIZE_F size, size_t nearby) {
    const float s = Scale(size), pad = 32.0f * s, right = size.width - pad, bottom = size.height - pad;
    DevicesLayout l;
    l.next = {right - 180.0f * s, bottom - 52.0f * s, right, bottom};
    l.banner = {pad, pad + 64.0f * s, right, pad + 120.0f * s};
    l.action = {right - 150.0f * s, l.banner.top + 10.0f * s, right - 10.0f * s, l.banner.bottom - 10.0f * s};
    const float gap = 16.0f * s, cardWidth = (right - pad - 2 * gap) / DevicesLayout::kCards;
    const float cardTop = l.banner.bottom + gap, cardBottom = cardTop + 148.0f * s;
    for (size_t i = 0; i < DevicesLayout::kCards; ++i) {
        const float left = pad + (cardWidth + gap) * i;
        l.cards[i] = {left, cardTop, left + cardWidth, cardBottom};
        l.disconnect[i] = {left + cardWidth - 130.0f * s, cardBottom - 56.0f * s, left + cardWidth - 16.0f * s,
                           cardBottom - 16.0f * s};
    }
    l.nearbyTop = cardBottom + 56.0f * s;
    const float rowHeight = 56.0f * s;
    const auto fit = static_cast<size_t>(std::max(0.0f, (l.next.top - gap - l.nearbyTop) / rowHeight));
    l.nearbyRows = std::min({nearby, fit, DevicesLayout::kMaxNearby});
    for (size_t i = 0; i < l.nearbyRows; ++i)
        l.nearby[i] = {pad, l.nearbyTop + rowHeight * i, right, l.nearbyTop + rowHeight * (i + 1) - 8.0f * s};
    return l;
}

SummaryLayout LayoutSummary(D2D1_SIZE_F size) {
    const float s = Scale(size), pad = 32.0f * s, right = size.width - pad, bottom = size.height - pad;
    SummaryLayout l;
    l.done = {right - 160.0f * s, bottom - 52.0f * s, right, bottom};
    l.file = {l.done.left - 172.0f * s, l.done.top, l.done.left - 12.0f * s, bottom};
    return l;
}

D2D1_RECT_F LayoutRideEnd(D2D1_SIZE_F size) {
    const float s = Scale(size, true), pad = 24.0f * s;
    return {size.width - pad - 170.0f * s, size.height - pad - 76.0f * s, size.width - pad, size.height - pad};
}

ConfirmLayout LayoutConfirm(D2D1_SIZE_F size) {
    const float s = Scale(size, true), w = 480.0f * s, h = 200.0f * s, in = 24.0f * s;
    ConfirmLayout l;
    l.box = {(size.width - w) / 2, (size.height - h) / 2, (size.width + w) / 2, (size.height + h) / 2};
    const float mid = (l.box.left + l.box.right) / 2, top = l.box.bottom - in - 52.0f * s;
    l.no = {l.box.left + in, top, mid - 6.0f * s, l.box.bottom - in};
    l.yes = {mid + 6.0f * s, top, l.box.right - in, l.box.bottom - in};
    return l;
}

Renderer::Renderer(IDWriteFactory* dwrite) {
    dwrite_.copy_from(dwrite);
    // Windows 11's variable Segoe UI where there is one, otherwise the classic.
    const bool variable = HasFont(dwrite, L"Segoe UI Variable Text");
    textFont_ = variable ? L"Segoe UI Variable Text" : L"Segoe UI";
    numberFont_ = variable ? L"Segoe UI Variable Display" : L"Segoe UI";
    if (SUCCEEDED(dwrite->CreateTypography(tabular_.put())))
        tabular_->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
}

// Brushes for this target and text formats for its size. Returns the layout scale.
float Renderer::Prepare(ID2D1RenderTarget* target, bool grow) {
    if (brushTarget_.get() != target) {
        auto make = [&](UINT32 rgb, winrt::com_ptr<ID2D1SolidColorBrush>& brush) {
            brush = nullptr;
            winrt::check_hresult(target->CreateSolidColorBrush(D2D1::ColorF(rgb), brush.put()));
        };
        make(kBackground, background_);
        make(kSurface, surface_);
        make(kRaised, raised_);
        make(kBorder, border_);
        make(kText, text_);
        make(kMuted, muted_);
        make(kFaint, faint_);
        make(kAccent, accent_);
        make(kAccentText, accentText_);
        make(kGood, good_);
        make(kWarn, warn_);
        for (size_t i = 0; i < std::size(kGrades); ++i) make(kGrades[i], grades_[i]);
        // Gradients run along a unit line here; each use sets its own ends.
        auto gradient = [&](std::initializer_list<D2D1_GRADIENT_STOP> stops,
                            winrt::com_ptr<ID2D1LinearGradientBrush>& brush) {
            winrt::com_ptr<ID2D1GradientStopCollection> collection;
            winrt::check_hresult(target->CreateGradientStopCollection(
                stops.begin(), static_cast<UINT32>(stops.size()), collection.put()));
            brush = nullptr;
            winrt::check_hresult(target->CreateLinearGradientBrush({{0, 0}, {0, 1}}, collection.get(), brush.put()));
        };
        gradient({{0.0f, D2D1::ColorF(kBackdropTop)}, {1.0f, D2D1::ColorF(kBackground)}}, backdrop_);
        gradient({{0.0f, D2D1::ColorF(0xFFFFFF, 0.38f)}, {1.0f, D2D1::ColorF(0xFFFFFF, 0.04f)}}, rideFill_);
        gradient({{0.0f, D2D1::ColorF(0x2E2E2E)}, {1.0f, D2D1::ColorF(kRaised, 0.25f)}}, trackFill_);
        gradient({{0.0f, D2D1::ColorF(kSurface, 0.15f)}, {1.0f, D2D1::ColorF(kSurface, 0.85f)}}, fade_);
        // Shapes belong to the target's factory, which a snapshot doesn't share with the window.
        factory_ = nullptr;
        target->GetFactory(factory_.put());
        for (ProfileCache& c : cache_) c = {};
        brushTarget_.copy_from(target);
    }
    const float s = Scale(target->GetSize(), grow);
    if (s != scale_) {
        label_ = MakeFormat(dwrite_.get(), textFont_, DWRITE_FONT_WEIGHT_NORMAL, 14.0f * s);
        small_ = MakeFormat(dwrite_.get(), textFont_, DWRITE_FONT_WEIGHT_NORMAL, 15.0f * s);
        row_ = MakeFormat(dwrite_.get(), textFont_, DWRITE_FONT_WEIGHT_NORMAL, 18.0f * s);
        button_ = MakeFormat(dwrite_.get(), textFont_, DWRITE_FONT_WEIGHT_SEMI_BOLD, 17.0f * s);
        title_ = MakeFormat(dwrite_.get(), numberFont_, DWRITE_FONT_WEIGHT_SEMI_BOLD, 30.0f * s);
        heading_ = MakeFormat(dwrite_.get(), numberFont_, DWRITE_FONT_WEIGHT_SEMI_BOLD, 23.0f * s);
        scale_ = s;
    }
    const D2D1_SIZE_F size = target->GetSize();
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    backdrop_->SetStartPoint({0, 0});
    backdrop_->SetEndPoint({0, size.height});
    target->FillRectangle({0, 0, size.width, size.height}, backdrop_.get());
    return s;
}

void Renderer::DrawHeader(ID2D1RenderTarget* target, std::wstring_view title, std::wstring_view hint) {
    const float s = scale_, pad = 32.0f * s;
    const float right = target->GetSize().width - pad;
    DrawLabel(target, title, title_.get(), {pad, pad, right, pad + 40.0f * s}, text_.get());
    DrawLabel(target, hint, label_.get(), {pad + 300.0f * s, pad, right, pad + 40.0f * s}, faint_.get(),
              DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void Renderer::Draw(ID2D1RenderTarget* target, const Ride& ride, std::wstring_view status, bool confirmEnd) {
    const float s = Prepare(target, true), pad = 24.0f * s, gap = 12.0f * s;
    const D2D1_SIZE_F size = target->GetSize();

    // Live numbers in cards, with heart rate when there is a monitor. The last one depends on the mode.
    wchar_t power[16], heart[16], cadence[16], speed[16], grade[16], last[16];
    swprintf_s(power, L"%d", ride.power());
    if (ride.heartRate() > 0) swprintf_s(heart, L"%d", ride.heartRate());
    else wcscpy_s(heart, L"–");
    swprintf_s(cadence, L"%.0f", ride.cadence());
    swprintf_s(speed, L"%.1f", ride.speed() * 3.6);
    swprintf_s(grade, L"%.1f", ride.grade() * 100 + 0.0);  // + 0.0 turns -0.0 into 0.0
    const wchar_t* lastLabel = L"Gear";
    const wchar_t* lastUnit = L"";
    if (ride.mode() == Mode::Erg) swprintf_s(last, L"%d", ride.ergW()), lastLabel = L"ERG target", lastUnit = L"W";
    else swprintf_s(last, L"%d", ride.gear());
    struct Metric {
        std::wstring_view label, value, unit;
        ID2D1Brush* brush;
    };
    Metric metrics[6];
    int count = 0;
    metrics[count++] = {L"Power", power, L"W", accentText_.get()};
    if (ride.heartRate() >= 0) metrics[count++] = {L"Heart rate", heart, L"bpm", text_.get()};
    metrics[count++] = {L"Cadence", cadence, L"rpm", text_.get()};
    metrics[count++] = {L"Speed", speed, L"km/h", text_.get()};
    metrics[count++] = {L"Grade", grade, L"%", text_.get()};
    metrics[count++] = {lastLabel, last, lastUnit, text_.get()};
    const float cardWidth = (size.width - 2 * pad - gap * (count - 1)) / count;
    const float metricsBottom = pad + 112.0f * s;
    for (int i = 0; i < count; ++i) {
        const float x = pad + (cardWidth + gap) * i;
        const D2D1_RECT_F card{x, pad, x + cardWidth, metricsBottom};
        DrawCard(target, card);
        DrawStat(target, Inset(card, 18.0f * s, 14.0f * s), metrics[i].label, metrics[i].value, metrics[i].unit,
                 50.0f * s, metrics[i].brush);
    }

    // The next 2 km in detail, starting a little behind the rider and colored by grade.
    const float footerTop = size.height - pad - 76.0f * s;
    const D2D1_RECT_F next{pad, metricsBottom + gap, size.width - pad, std::round(size.height * 0.585f)};
    const D2D1_RECT_F whole{pad, next.bottom + gap, size.width - pad, footerTop - gap};
    const double length = ride.course().Length();
    const double from = std::clamp(ride.distance() - 250.0, 0.0, std::max(0.0, length - 2250.0));
    DrawCard(target, next);
    wchar_t caption[64];
    DrawLabel(target, L"Next 2 km", label_.get(), {next.left + 18.0f * s, next.top + 10.0f * s, next.right, next.top + 34.0f * s},
              muted_.get());
    swprintf_s(caption, L"%.0f m", ride.elevation());
    DrawLabel(target, caption, label_.get(), {next.left, next.top + 10.0f * s, next.right - 18.0f * s, next.top + 34.0f * s},
              muted_.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
    DrawProfile(target, ride.course(), from, std::min(length, from + 2250.0), ride.distance(),
                {next.left + 18.0f * s, next.top + 40.0f * s, next.right - 18.0f * s, next.bottom - 12.0f * s}, 30.0, true);

    // A message about the ride or the devices floats over the road ahead.
    const D2D1_POINT_2F pill{(next.left + next.right) / 2, next.top + 22.0f * s};
    if (!status.empty()) DrawPill(target, pill, status, warn_.get());
    else if (ride.finished()) DrawPill(target, pill, L"Finished", good_.get());
    else if (ride.paused()) DrawPill(target, pill, L"Paused · Space or Z to go on", text_.get());

    // The whole course.
    DrawCard(target, whole);
    DrawLabel(target, L"Course", label_.get(), {whole.left + 18.0f * s, whole.top + 10.0f * s, whole.right, whole.top + 34.0f * s},
              muted_.get());
    DrawProfile(target, ride.course(), 0.0, length, ride.distance(),
                {whole.left + 18.0f * s, whole.top + 40.0f * s, whole.right - 18.0f * s, whole.bottom - 12.0f * s}, 50.0,
                false);

    // Progress, climbing, power, calories and time along the bottom, and a button to end the ride.
    wchar_t distance[48], climbed[48], avg[16], kcal[32];
    // An endless course has no totals to ride towards.
    if (ride.course().params.endless) {
        swprintf_s(distance, L"%.1f", ride.distance() / 1000);
        swprintf_s(climbed, L"%.0f", ride.climbed());
    } else {
        swprintf_s(distance, L"%.1f / %.1f", ride.distance() / 1000, length / 1000);
        swprintf_s(climbed, L"%.0f / %.0f", ride.climbed(), ride.course().targetGain);
    }
    swprintf_s(avg, L"%d", ride.averagePower());
    swprintf_s(kcal, L"%.0f", ride.calories());
    const std::wstring time = Clock(ride.elapsed());
    const D2D1_RECT_F end = LayoutRideEnd(size);
    const D2D1_RECT_F footer{pad, footerTop, end.left - gap, size.height - pad};
    DrawCard(target, footer);
    DrawButton(target, end, L"End ride", Button::Secondary, button_.get());
    const Metric totals[] = {{L"Distance", distance, L"km", text_.get()},
                             {L"Climbed", climbed, L"m", text_.get()},
                             {L"Average power", avg, L"W", text_.get()},
                             {L"Calories", kcal, L"kcal", text_.get()},
                             {L"Time", time, L"", text_.get()}};
    const float statWidth = (footer.right - footer.left) / std::size(totals);
    for (size_t i = 0; i < std::size(totals); ++i) {
        const float x = footer.left + statWidth * i;
        DrawStat(target, {x + 18.0f * s, footer.top + 10.0f * s, x + statWidth - 8.0f * s, footer.bottom - 8.0f * s},
                 totals[i].label, totals[i].value, totals[i].unit, 26.0f * s, totals[i].brush);
        if (i) target->DrawLine({x, footer.top + 16.0f * s}, {x, footer.bottom - 16.0f * s}, border_.get(), 1.0f);
    }

    // Ending the ride asks first, over a dimmed screen.
    if (confirmEnd) {
        background_->SetOpacity(0.75f);
        target->FillRectangle({0, 0, size.width, size.height}, background_.get());
        background_->SetOpacity(1.0f);
        const ConfirmLayout c = LayoutConfirm(size);
        DrawCard(target, c.box);
        const float in = 24.0f * s;
        DrawLabel(target, L"End the ride?", heading_.get(), {c.box.left + in, c.box.top + 20.0f * s, c.box.right - in, c.box.top + 56.0f * s},
                  text_.get());
        DrawLabel(target, L"It's saved, and the summary comes next.", small_.get(),
                  {c.box.left + in, c.box.top + 60.0f * s, c.box.right - in, c.box.top + 90.0f * s}, muted_.get());
        DrawButton(target, c.no, L"No, keep riding", Button::Secondary, button_.get());
        DrawButton(target, c.yes, L"Yes, end it", Button::Primary, button_.get());
    }
}

void Renderer::DrawSetup(ID2D1RenderTarget* target, const SetupView& view) {
    const float s = Prepare(target);
    const SetupLayout l = LayoutSetup(target->GetSize(), view.rows.size());
    DrawHeader(target, kAppName, L"↑↓ choose · ←→ change · R new course · D devices · Enter start · F11 full screen");

    // Settings on the left. The selected one is highlighted, with arrows to change it.
    DrawCard(target, l.panel);
    const float inset = 14.0f * s, radius = 8.0f * s;
    for (size_t i = 0; i < std::min(view.rows.size(), SetupLayout::kMaxRows); ++i) {
        const D2D1_RECT_F r = l.rows[i];
        const D2D1_RECT_F value{l.valueLeft(i), r.top, r.right - inset, r.bottom};
        const bool selected = static_cast<int>(i) == view.selected;
        if (selected) {
            target->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), raised_.get());
            target->FillRoundedRectangle(
                D2D1::RoundedRect({r.left, r.top + 10.0f * s, r.left + 3.0f * s, r.bottom - 10.0f * s}, 1.5f * s, 1.5f * s),
                accent_.get());
        }
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
        DrawCard(target, p);
        DrawLabel(target, L"Course", label_.get(), {p.left + 20.0f * s, p.top + 12.0f * s, p.right, p.top + 36.0f * s},
                  muted_.get());
        // The numbers as chips along the bottom.
        float x = p.left + 20.0f * s;
        const float chipTop = p.bottom - 52.0f * s, chipBottom = p.bottom - 18.0f * s;
        for (const std::wstring& stat : view.stats) {
            if (stat.empty()) continue;
            const float w = TextWidth(stat, small_.get()) + 28.0f * s;
            const D2D1_RECT_F chip{x, chipTop, x + w, chipBottom};
            const float h = (chip.bottom - chip.top) / 2;
            target->FillRoundedRectangle(D2D1::RoundedRect(chip, h, h), raised_.get());
            DrawLabel(target, stat, small_.get(), chip, text_.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            x += w + 8.0f * s;
        }
        DrawProfile(target, *view.course, 0.0, view.course->Length(), -1.0,
                    {p.left + 20.0f * s, p.top + 52.0f * s, p.right - 20.0f * s, chipTop - 20.0f * s}, 50.0, false);
    }

    // Devices at the bottom left, one line each, buttons at the bottom right.
    const float lineHeight = 22.0f * s, pad = 32.0f * s;
    const size_t lines = view.devices.size();
    for (size_t i = 0; i < lines; ++i) {
        const DeviceInfo& d = view.devices[i];
        const float top = l.start.bottom - lineHeight * (lines - i);
        DrawDot(target, {pad + 4.0f * s, top + lineHeight / 2}, d.tone);
        std::wstring text(d.role);
        if (!d.name.empty()) text.append(L" · ").append(d.name);
        text.append(L" · ").append(d.status);
        DrawLabel(target, text, label_.get(), {pad + 16.0f * s, top, l.devices.left - 16.0f * s, top + lineHeight},
                  d.tone == Tone::Problem ? warn_.get() : d.tone == Tone::Good ? text_.get() : muted_.get());
    }
    DrawButton(target, l.devices, L"Devices", Button::Secondary, button_.get());
    DrawButton(target, l.reroll, L"New course", Button::Secondary, button_.get());
    DrawButton(target, l.start, L"Start ride", Button::Primary, button_.get());
}

void Renderer::DrawDevices(ID2D1RenderTarget* target, const DevicesView& view) {
    const float s = Prepare(target), pad = 32.0f * s, inset = 20.0f * s;
    const D2D1_SIZE_F size = target->GetSize();
    const DevicesLayout l = LayoutDevices(size, view.nearby.size());
    DrawHeader(target, L"Devices", L"↑↓ choose · Enter connect or disconnect · Esc continue · F11 full screen");

    // Bluetooth itself, with a button to fix it when it's off.
    const bool off = !view.action.empty();
    DrawCard(target, l.banner);
    DrawDot(target, {l.banner.left + inset + 4.0f * s, (l.banner.top + l.banner.bottom) / 2}, view.radio);
    DrawLabel(target, view.bluetooth, row_.get(),
              {l.banner.left + inset + 18.0f * s, l.banner.top, (off ? l.action.left : l.banner.right) - inset, l.banner.bottom},
              off ? warn_.get() : text_.get());
    if (off) DrawButton(target, l.action, view.action, Button::Primary, button_.get());

    // A card for each role: what it is, which device, and how it's doing.
    for (size_t i = 0; i < std::min(view.cards.size(), DevicesLayout::kCards); ++i) {
        const DeviceInfo& d = view.cards[i];
        const D2D1_RECT_F c = l.cards[i], b = l.disconnect[i];
        DrawCard(target, c, view.focus == static_cast<int>(i));
        DrawLabel(target, d.role, label_.get(), {c.left + inset, c.top + 16.0f * s, c.right - inset, c.top + 36.0f * s},
                  muted_.get());
        DrawLabel(target, d.name.empty() ? L"None" : std::wstring_view(d.name), heading_.get(),
                  {c.left + inset, c.top + 42.0f * s, c.right - inset, c.top + 78.0f * s},
                  d.name.empty() ? faint_.get() : text_.get());
        DrawDot(target, {c.left + inset + 4.0f * s, (b.top + b.bottom) / 2}, d.tone);
        DrawLabel(target, d.status, small_.get(),
                  {c.left + inset + 16.0f * s, b.top, (d.name.empty() ? c.right - inset : b.left - 8.0f * s), b.bottom},
                  d.tone == Tone::Problem ? warn_.get() : d.tone == Tone::Good ? text_.get() : muted_.get());
        if (!d.name.empty()) DrawButton(target, b, L"Disconnect", Button::Secondary, small_.get());
    }

    // Devices nearby, each a click away. The list keeps itself up to date.
    DrawLabel(target, L"Nearby", label_.get(), {pad, l.nearbyTop - 32.0f * s, size.width - pad, l.nearbyTop - 8.0f * s},
              muted_.get());
    if (view.nearby.empty()) {
        const D2D1_RECT_F empty{pad, l.nearbyTop, size.width - pad, l.nearbyTop + 64.0f * s};
        DrawCard(target, empty);
        DrawLabel(target, L"Nothing new yet. Wake your trainer, Zwift Ride or heart rate strap and it shows up here.",
                  row_.get(), Inset(empty, inset, 0), muted_.get());
    }
    for (size_t i = 0; i < l.nearbyRows; ++i) {
        const D2D1_RECT_F r = l.nearby[i];
        const bool focused = view.focus == static_cast<int>(DevicesLayout::kCards + i);
        DrawCard(target, r, focused);
        const float roleRight = r.left + inset + 150.0f * s;
        const D2D1_RECT_F button{r.right - 130.0f * s, r.top + 8.0f * s, r.right - 8.0f * s, r.bottom - 8.0f * s};
        DrawLabel(target, view.nearby[i].role, row_.get(), {r.left + inset, r.top, roleRight, r.bottom}, muted_.get());
        DrawLabel(target, view.nearby[i].name, row_.get(), {roleRight, r.top, button.left - inset, r.bottom}, text_.get());
        DrawButton(target, button, L"Connect", focused ? Button::Primary : Button::Secondary, small_.get());
    }

    DrawLabel(target, view.footer, label_.get(), {pad, l.next.top, l.next.left - inset, l.next.bottom}, muted_.get());
    DrawButton(target, l.next, L"Continue", Button::Primary, button_.get());
}

void Renderer::DrawSummary(ID2D1RenderTarget* target, const SummaryView& view) {
    const float s = Prepare(target), pad = 32.0f * s, gap = 12.0f * s;
    const D2D1_SIZE_F size = target->GetSize();
    const SummaryLayout l = LayoutSummary(size);
    DrawHeader(target, view.title, view.date);
    if (!view.ride) return;
    const Ride& r = *view.ride;

    // The numbers in two rows of four.
    wchar_t distance[16], climbed[16], kcal[16], avg[16], max[16], speed[16], last[16];
    swprintf_s(distance, L"%.1f", r.distance() / 1000);
    swprintf_s(climbed, L"%.0f", r.climbed());
    swprintf_s(kcal, L"%.0f", r.calories());
    swprintf_s(avg, L"%d", r.averagePower());
    swprintf_s(max, L"%d", r.maxPower());
    swprintf_s(speed, L"%.1f", r.elapsed() > 0 ? r.distance() / r.elapsed() * 3.6 : 0.0);
    const bool heart = r.averageHeartRate() > 0;
    swprintf_s(last, L"%d", heart ? r.averageHeartRate() : r.averageCadence());
    const std::wstring time = Clock(r.elapsed());
    struct Stat {
        std::wstring_view label, value, unit;
    } const stats[] = {{L"Time", time, L""},
                       {L"Distance", distance, L"km"},
                       {L"Climbed", climbed, L"m"},
                       {L"Calories", kcal, L"kcal"},
                       {L"Average power", avg, L"W"},
                       {L"Max power", max, L"W"},
                       {L"Average speed", speed, L"km/h"},
                       {heart ? L"Average heart rate" : L"Average cadence", last, heart ? L"bpm" : L"rpm"}};
    const float top = pad + 64.0f * s, cardHeight = 100.0f * s;
    const float cardWidth = (size.width - 2 * pad - 3 * gap) / 4;
    for (size_t i = 0; i < std::size(stats); ++i) {
        const float x = pad + (cardWidth + gap) * (i % 4), y = top + (cardHeight + gap) * (i / 4);
        const D2D1_RECT_F card{x, y, x + cardWidth, y + cardHeight};
        DrawCard(target, card);
        DrawStat(target, Inset(card, 20.0f * s, 14.0f * s), stats[i].label, stats[i].value, stats[i].unit, 42.0f * s,
                 i == 3 ? accentText_.get() : text_.get());
    }

    // The course, with how much of it was ridden. Of an endless one, only the part ridden.
    const D2D1_RECT_F course{pad, top + 2 * (cardHeight + gap), size.width - pad, l.done.top - 24.0f * s};
    DrawCard(target, course);
    const double shown = r.course().params.endless ? std::clamp(r.distance(), 1000.0, r.course().Length())
                                                    : r.course().Length();
    wchar_t caption[64];
    swprintf_s(caption, L"Course · %.1f km", shown / 1000);
    DrawLabel(target, caption, label_.get(), {course.left + 20.0f * s, course.top + 12.0f * s, course.right, course.top + 36.0f * s},
              muted_.get());
    DrawProfile(target, r.course(), 0.0, shown, r.distance(),
                {course.left + 20.0f * s, course.top + 48.0f * s, course.right - 20.0f * s, course.bottom - 16.0f * s}, 50.0,
                false);

    // Where the ride went, along the bottom.
    const float left = view.file.empty() ? l.done.left : l.file.left;
    DrawLabel(target, view.note, small_.get(), {pad, l.done.top, left - 16.0f * s, l.done.bottom}, muted_.get());
    if (!view.file.empty()) DrawButton(target, l.file, view.file, Button::Secondary, button_.get());
    DrawButton(target, l.done, L"Done", Button::Primary, button_.get());
}

void Renderer::DrawCard(ID2D1RenderTarget* target, D2D1_RECT_F box, bool focused) {
    const float radius = 12.0f * scale_;
    const D2D1_ROUNDED_RECT r = D2D1::RoundedRect(box, radius, radius);
    target->FillRoundedRectangle(r, surface_.get());
    // The outline sits half a pixel in so it stays crisp.
    const float w = focused ? 2.0f * std::max(1.0f, scale_) : 1.0f;
    target->DrawRoundedRectangle(D2D1::RoundedRect(Inset(box, w / 2, w / 2), radius, radius),
                                 focused ? accent_.get() : border_.get(), w);
}

// A label over a value, with the unit after the value in a smaller size and quieter color.
void Renderer::DrawStat(ID2D1RenderTarget* target, D2D1_RECT_F box, std::wstring_view label, std::wstring_view value,
                        std::wstring_view unit, float valueSize, ID2D1Brush* valueBrush) {
    const float s = scale_;
    DrawLabel(target, label, label_.get(), {box.left, box.top, box.right, box.top + 20.0f * s}, muted_.get());
    wchar_t text[64];
    const int n = swprintf_s(text, L"%.*s%s%.*s", static_cast<int>(value.size()), value.data(), unit.empty() ? L"" : L" ",
                             static_cast<int>(unit.size()), unit.data());
    if (n <= 0) return;
    const auto length = static_cast<UINT32>(n), valueLength = static_cast<UINT32>(value.size());
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text, length, title_.get(), box.right - box.left, box.bottom - box.top - 20.0f * s,
                                         layout.put())))
        return;
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    layout->SetFontSize(valueSize, {0, length});
    if (tabular_) layout->SetTypography(tabular_.get(), {0, valueLength});
    if (valueLength < length) {
        layout->SetFontSize(std::max(14.0f * s, valueSize * 0.4f), {valueLength, length - valueLength});
        layout->SetFontWeight(DWRITE_FONT_WEIGHT_NORMAL, {valueLength, length - valueLength});
        layout->SetDrawingEffect(muted_.get(), {valueLength, length - valueLength});
    }
    target->DrawTextLayout({box.left, box.top + 20.0f * s}, layout.get(), valueBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// An elevation profile from `from` to `to` metres, with elevations up the left and distances along the bottom.
// Flat stretches stay flat: the vertical scale covers at least `minSpan` metres. A graded profile is colored by how
// steep the road is, with the ridden part dimmed. Otherwise the ridden part is lit. A marker shows the rider,
// unless `position` is negative.
void Renderer::DrawProfile(ID2D1RenderTarget* target, const Course& course, double from, double to, double position,
                           D2D1_RECT_F box, double minSpan, bool graded) {
    const D2D1_RECT_F frame = box;
    box = {box.left + 52.0f * scale_, box.top, box.right, box.bottom - 22.0f * scale_};
    const float width = box.right - box.left, height = box.bottom - box.top;
    if (to <= from || width < 2 || height < 2) return;
    const auto columns = static_cast<size_t>(std::max(2.0f, width / (graded ? 4.0f : 3.0f)));
    const float dx = width / (columns - 1);
    heights_.resize(columns);
    double lo = 1e9, hi = -1e9;
    for (size_t i = 0; i < columns; ++i) {
        heights_[i] = static_cast<float>(course.ElevationAt(from + (to - from) * i / (columns - 1)));
        lo = std::min(lo, double(heights_[i]));
        hi = std::max(hi, double(heights_[i]));
    }
    const double span = std::max(hi - lo, minSpan) * 1.12;
    const double base = std::max(0.0, (lo + hi) / 2 - span / 2);  // courses start at 0 m
    auto y = [&](double e) { return height - static_cast<float>((e - base) / span) * height; };

    // Grid lines at round elevations, and ticks at round distances.
    wchar_t tick[24];
    const double rise = NiceStep(span / 4);
    for (double e = std::ceil(base / rise) * rise; e <= base + span; e += rise) {
        const float at = box.top + y(e);
        target->DrawLine({box.left, at}, {box.right, at}, border_.get(), 1.0f);
        swprintf_s(tick, L"%.0f m", std::round(e) + 0.0);  // + 0.0 turns -0 into 0
        DrawLabel(target, tick, label_.get(), {frame.left, at - 10.0f * scale_, box.left - 8.0f * scale_, at + 10.0f * scale_},
                  faint_.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    const double stride = NiceStep((to - from) / std::max(2.0f, width / (110.0f * scale_)));
    const int decimals = stride >= 1000 ? 0 : stride >= 100 ? 1 : 2;
    for (double d = std::ceil(from / stride) * stride; d <= to; d += stride) {
        const float at = box.left + static_cast<float>((d - from) / (to - from)) * width;
        if (at > box.right - 24.0f * scale_ && d < to) continue;  // leave the end's label room
        target->DrawLine({at, box.bottom}, {at, box.bottom + 4.0f * scale_}, faint_.get(), 1.0f);
        swprintf_s(tick, L"%.*f km", decimals, d / 1000);
        // Centered under its tick, except at the very end, where it ends at the tick to stay in the card.
        const bool last = at > box.right - 24.0f * scale_;
        DrawLabel(target, tick, label_.get(),
                  {at - 100.0f * scale_, box.bottom + 4.0f * scale_, at + (last ? 6.0f : 100.0f) * scale_, frame.bottom},
                  faint_.get(), last ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // A filled shape under the columns from a to b, and the line along its top.
    auto shape = [&](size_t a, size_t b, bool closed) {
        winrt::com_ptr<ID2D1PathGeometry> path;
        winrt::com_ptr<ID2D1GeometrySink> sink;
        winrt::check_hresult(factory_->CreatePathGeometry(path.put()));
        winrt::check_hresult(path->Open(sink.put()));
        const D2D1_POINT_2F first{dx * a, y(heights_[a])};
        sink->BeginFigure(closed ? D2D1_POINT_2F{first.x, height} : first,
                          closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (size_t i = closed ? a : a + 1; i <= b; ++i) sink->AddLine({dx * i, y(heights_[i])});
        if (closed) sink->AddLine({dx * b, height});
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        winrt::check_hresult(sink->Close());
        return path;
    };

    D2D1_MATRIX_3X2_F saved;
    target->GetTransform(&saved);
    target->SetTransform(D2D1::Matrix3x2F::Translation(box.left, box.top) * saved);
    const float x = static_cast<float>(std::clamp((position - from) / (to - from), 0.0, 1.0)) * width;
    const float s = std::max(1.0f, scale_);

    if (graded) {
        // Runs of columns with the same grade color, each its own shape. The grade is over 50 m, so a color
        // holds for a stretch rather than flickering with each sample.
        auto bucket = [&](size_t i) {
            const double d = from + (to - from) * (i + 0.5) / (columns - 1);
            const double g = (course.ElevationAt(d + 25.0) - course.ElevationAt(d - 25.0)) / 50.0;
            return GradeBrush(g);
        };
        winrt::com_ptr<ID2D1PathGeometry> whole = shape(0, columns - 1, true);
        size_t start = 0;
        ID2D1SolidColorBrush* run = bucket(0);
        for (size_t i = 1; i < columns; ++i) {
            ID2D1SolidColorBrush* next = i + 1 < columns ? bucket(i) : nullptr;
            if (next == run) continue;
            target->FillGeometry(shape(start, i, true).get(), run);
            start = i;
            run = next;
        }
        // Deeper down the colors sink into the card, and a light line traces the road.
        fade_->SetStartPoint({0, 0});
        fade_->SetEndPoint({0, height});
        target->FillGeometry(whole.get(), fade_.get());
        text_->SetOpacity(0.8f);
        target->DrawGeometry(shape(0, columns - 1, false).get(), text_.get(), 1.5f * s);
        text_->SetOpacity(1.0f);
        // The road already ridden fades into the card.
        surface_->SetOpacity(0.65f);
        target->PushAxisAlignedClip({0, 0, x, height}, D2D1_ANTIALIAS_MODE_ALIASED);
        target->FillRectangle({0, 0, x, height}, surface_.get());
        target->PopAxisAlignedClip();
        surface_->SetOpacity(1.0f);
    } else {
        // A whole course stays put while the rider moves along it, so its shapes are kept for the next frame.
        ProfileCache* hit = nullptr;
        for (ProfileCache& c : cache_) {
            const CourseParams& p = c.params;
            const CourseParams& q = course.params;
            if (c.fill && p.km == q.km && p.gainM == q.gainM && p.maxGradePct == q.maxGradePct && p.loop == q.loop &&
                p.seed == q.seed && p.endless == q.endless && p.terrain == q.terrain && c.samples == columns && c.from == from && c.to == to && c.width == width &&
                c.height == height)
                hit = &c;
        }
        if (!hit) {
            hit = &cache_[nextCache_];
            nextCache_ = (nextCache_ + 1) % static_cast<int>(std::size(cache_));
            *hit = {course.params, columns, from, to, shape(0, columns - 1, true), shape(0, columns - 1, false), width,
                    height};
        }
        trackFill_->SetStartPoint({0, 0});
        trackFill_->SetEndPoint({0, height});
        target->FillGeometry(hit->fill.get(), trackFill_.get());
        target->DrawGeometry(hit->line.get(), faint_.get(), 1.5f * s);
        if (position > from) {
            rideFill_->SetStartPoint({0, 0});
            rideFill_->SetEndPoint({0, height});
            target->PushAxisAlignedClip({0, 0, x, height}, D2D1_ANTIALIAS_MODE_ALIASED);
            target->FillGeometry(hit->fill.get(), rideFill_.get());
            target->DrawGeometry(hit->line.get(), accentText_.get(), 2.0f * s);
            target->PopAxisAlignedClip();
        }
    }

    if (position >= 0) {
        const float markerY = y(course.ElevationAt(position));
        target->DrawLine({x, markerY}, {x, height}, text_.get(), 1.0f * s);
        target->FillEllipse(D2D1::Ellipse({x, markerY}, 7.0f * s, 7.0f * s), text_.get());
        target->FillEllipse(D2D1::Ellipse({x, markerY}, 3.5f * s, 3.5f * s), background_.get());
    }
    target->SetTransform(saved);
}

void Renderer::DrawButton(ID2D1RenderTarget* target, D2D1_RECT_F box, std::wstring_view text, Button kind,
                          IDWriteTextFormat* format, bool enabled) {
    const float radius = 10.0f * scale_;
    const D2D1_ROUNDED_RECT r = D2D1::RoundedRect(box, radius, radius);
    ID2D1Brush* label = kind == Button::Secondary ? text_.get() : background_.get();
    if (!enabled) {
        target->FillRoundedRectangle(r, raised_.get());
        label = muted_.get();
    } else if (kind != Button::Secondary) {
        target->FillRoundedRectangle(r, accent_.get());
    } else {
        target->FillRoundedRectangle(r, raised_.get());
        target->DrawRoundedRectangle(D2D1::RoundedRect(Inset(box, 0.5f, 0.5f), radius, radius), border_.get(), 1.0f);
    }
    DrawLabel(target, text, format, box, label, DWRITE_TEXT_ALIGNMENT_CENTER);
}

// A short message in a rounded tag tinted with its color.
void Renderer::DrawPill(ID2D1RenderTarget* target, D2D1_POINT_2F center, std::wstring_view text, ID2D1Brush* brush) {
    const float s = scale_, w = TextWidth(text, small_.get()) + 32.0f * s, h = 30.0f * s;
    const D2D1_RECT_F box{center.x - w / 2, center.y - h / 2, center.x + w / 2, center.y + h / 2};
    const D2D1_ROUNDED_RECT r = D2D1::RoundedRect(box, h / 2, h / 2);
    target->FillRoundedRectangle(r, background_.get());
    brush->SetOpacity(0.18f);
    target->FillRoundedRectangle(r, brush);
    brush->SetOpacity(1.0f);
    DrawLabel(target, text, small_.get(), box, brush, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void Renderer::DrawDot(ID2D1RenderTarget* target, D2D1_POINT_2F center, Tone tone) {
    const float r = 4.0f * scale_;
    ID2D1SolidColorBrush* brush = ToneBrush(tone);
    // A problem is a ring, so it reads apart from a good dot without color. Both get a soft halo.
    if (tone != Tone::Waiting) {
        brush->SetOpacity(0.2f);
        target->FillEllipse(D2D1::Ellipse(center, r * 2, r * 2), brush);
        brush->SetOpacity(1.0f);
    }
    if (tone == Tone::Problem) target->DrawEllipse(D2D1::Ellipse(center, r - 0.75f * scale_, r - 0.75f * scale_), brush, 1.5f * scale_);
    else target->FillEllipse(D2D1::Ellipse(center, r, r), brush);
}

ID2D1SolidColorBrush* Renderer::ToneBrush(Tone tone) const {
    return tone == Tone::Good ? good_.get() : tone == Tone::Problem ? warn_.get() : faint_.get();
}

ID2D1SolidColorBrush* Renderer::GradeBrush(double grade) const {
    size_t i = 0;
    while (i < std::size(kGradeSteps) && grade >= kGradeSteps[i]) ++i;
    return grades_[i].get();
}

void Renderer::DrawLabel(ID2D1RenderTarget* target, std::wstring_view text, IDWriteTextFormat* format, D2D1_RECT_F box,
                        ID2D1Brush* brush, DWRITE_TEXT_ALIGNMENT align) {
    format->SetTextAlignment(align);
    target->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format, box, brush);
}

float Renderer::TextWidth(std::wstring_view text, IDWriteTextFormat* format) {
    winrt::com_ptr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS m{};
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (FAILED(dwrite_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, 10000.0f, 100.0f,
                                         layout.put())) ||
        FAILED(layout->GetMetrics(&m)))
        return 0.0f;
    return m.widthIncludingTrailingWhitespace;
}
