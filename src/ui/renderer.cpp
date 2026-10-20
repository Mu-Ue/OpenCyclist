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
// read from the bike, and more slowly than the window so the graphs take the rest. The menus keep their size and
// spread out instead.
float Scale(D2D1_SIZE_F size, bool grow = false) {
    const float fit = std::min(size.width / 1280.0f, size.height / 720.0f);
    return std::max(0.1f, grow && fit > 1.0f ? std::sqrt(fit) : std::min(fit, 1.0f));
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

// The ride screen's tiles and graphs by name, in the order of their enums.
struct TileName {
    std::wstring_view label, unit;
};
constexpr TileName kTileNames[] = {
    {L"Power", L"W"},         {L"Cadence", L"rpm"},       {L"Speed", L"km/h"},
    {L"Grade", L"%"},         {L"Gear", L""},             {L"Heart rate", L"bpm"},
    {L"Average heart rate", L"bpm"}, {L"Max power", L"W"}, {L"Average cadence", L"rpm"},
    {L"Distance", L"km"},     {L"Climbed", L"m"},         {L"Average power", L"W"},
    {L"Calories", L"kcal"},   {L"Time", L""}};
static_assert(std::size(kTileNames) == RideLayout::kTiles);
constexpr std::wstring_view kGraphNames[] = {L"Next 2 km", L"Course"};
static_assert(std::size(kGraphNames) == RideLayout::kGraphs);

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
    l.devices = {l.start.left - 172.0f * s, l.start.top, l.start.left - 12.0f * s, bottom};
    l.reroll = {l.devices.left - 172.0f * s, l.start.top, l.devices.left - 12.0f * s, bottom};
    l.preview = {l.panel.right + 16.0f * s, l.panel.top, right, l.start.top - 24.0f * s};
    // Past rides fill the rest of the left side, above a line for each device.
    l.history = {l.panel.left, l.panel.bottom + 16.0f * s, l.panel.right, bottom - 3 * 22.0f * s - 16.0f * s};
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

HistoryLayout LayoutHistory(D2D1_SIZE_F size) {
    const float s = Scale(size), pad = 32.0f * s, right = size.width - pad, bottom = size.height - pad;
    HistoryLayout l;
    l.back = {right - 160.0f * s, bottom - 52.0f * s, right, bottom};
    l.file = {l.back.left - 172.0f * s, l.back.top, l.back.left - 12.0f * s, bottom};
    l.remove = {pad, l.back.top, pad + 160.0f * s, bottom};  // away from the others, so it isn't hit by mistake
    l.list = {pad, pad + 64.0f * s, pad + (right - pad) * 0.34f, l.back.top - 24.0f * s};
    l.detail = {l.list.right + 16.0f * s, l.list.top, right, l.list.bottom};
    const float inset = 8.0f * s, rowHeight = 62.0f * s;
    l.rowsShown = std::min(HistoryLayout::kMaxRows,
                           static_cast<size_t>(std::max(0.0f, (l.list.bottom - l.list.top - 2 * inset) / rowHeight)));
    for (size_t i = 0; i < l.rowsShown; ++i) {
        const float top = l.list.top + inset + rowHeight * i;
        l.rows[i] = {l.list.left + inset, top, l.list.right - inset, top + rowHeight - 4.0f * s};
    }
    return l;
}

RideLayout LayoutRide(D2D1_SIZE_F size, bool erg) {
    using L = RideLayout;
    const float s = Scale(size, true), pad = 24.0f * s, gap = 12.0f * s, right = size.width - pad;
    L l;
    l.end = {right - 140.0f * s, size.height - pad - 76.0f * s, right, size.height - pad};
    l.pause = {l.end.left - gap - 140.0f * s, l.end.top, l.end.left - gap, l.end.bottom};
    const float cardWidth = (right - pad - gap * (L::kTop - 1)) / L::kTop;
    for (int i = 0; i < L::kTop; ++i) {
        const float x = pad + (cardWidth + gap) * i;
        l.tiles[i] = {x, pad, x + cardWidth, pad + 112.0f * s};
    }
    // The graphs, with a column of cards on each side.
    const float side = 200.0f * s, above = l.tiles[0].bottom + gap, below = l.end.top - gap;
    const float split = std::round(above + (below - above) * 0.61f);
    l.graphs[0] = {pad + side + gap, above, right - side - gap, split};
    l.graphs[1] = {l.graphs[0].left, split + gap, l.graphs[0].right, below};
    for (int row = 0; row < L::kSide; ++row) {
        const D2D1_RECT_F g = l.graphs[row];
        l.tiles[L::kTop + row] = {pad, g.top, pad + side, g.bottom};
        l.tiles[L::kTop + L::kSide + row] = {right - side, g.top, right, g.bottom};
    }
    // The totals share one card left of the buttons.
    l.footer = {pad, l.end.top, l.pause.left - gap, l.end.bottom};
    const float statWidth = (l.footer.right - l.footer.left) / L::kBottom;
    for (int i = 0; i < L::kBottom; ++i) {
        const float x = l.footer.left + statWidth * i;
        l.tiles[L::kTop + 2 * L::kSide + i] = {x, l.footer.top, x + statWidth, l.footer.bottom};
    }
    if (!erg) return l;
    D2D1_RECT_F& next = l.graphs[0];
    l.target = {next.right - 260.0f * s, next.top, next.right, next.bottom};
    next.right = l.target.left - gap;
    // Three digit columns, a little left of center to leave room for the unit.
    const float column = 64.0f * s, space = 8.0f * s, arrow = 44.0f * s, digits = 96.0f * s;
    const float left = (l.target.left + l.target.right) / 2 - 12.0f * s - (3 * column + 2 * space) / 2;
    const float top = (l.target.top + 36.0f * s + l.target.bottom - 12.0f * s) / 2 - (2 * arrow + digits) / 2;
    for (int i = 0; i < 3; ++i) {
        const float x = left + (column + space) * i;
        l.up[i] = {x, top, x + column, top + arrow};
        l.down[i] = {x, top + arrow + digits, x + column, top + 2 * arrow + digits};
    }
    return l;
}

int RideLayout::Hit(D2D1_POINT_2F p) const {
    for (int i = 0; i < kSlots; ++i)
        if (Contains(Slot(i), p)) return i;
    return -1;
}

Board::Board() {
    for (int i = 0; i < RideLayout::kSlots; ++i)
        slots[i] = static_cast<uint8_t>(i < RideLayout::kTiles ? i : i - RideLayout::kTiles);
}

// One hex digit a slot. Each tile and each graph must show exactly once.
Board Board::Parse(std::wstring_view text) {
    Board b;
    if (text.size() != RideLayout::kSlots) return {};
    bool seen[RideLayout::kSlots]{};
    for (int i = 0; i < RideLayout::kSlots; ++i) {
        const wchar_t c = text[i];
        const int v = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
        const bool tile = i < RideLayout::kTiles;
        const int count = tile ? RideLayout::kTiles : RideLayout::kGraphs, at = tile ? v : RideLayout::kTiles + v;
        if (v < 0 || v >= count || seen[at]) return {};
        seen[at] = true;
        b.slots[i] = static_cast<uint8_t>(v);
    }
    return b;
}

std::wstring Board::Format() const {
    std::wstring text;
    for (uint8_t v : slots) text += L"0123456789abcdef"[v];
    return text;
}

ConfirmLayout LayoutConfirm(D2D1_SIZE_F size, bool grow) {
    const float s = Scale(size, grow), w = 480.0f * s, h = 200.0f * s, in = 24.0f * s;
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
        digit_ = MakeFormat(dwrite_.get(), numberFont_, DWRITE_FONT_WEIGHT_SEMI_BOLD, 76.0f * s);
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

void Renderer::Draw(ID2D1RenderTarget* target, const Ride& ride, const Board& board, std::wstring_view status,
                    bool confirmEnd, const RideDrag* drag) {
    using L = RideLayout;
    const float s = Prepare(target, true);
    const D2D1_SIZE_F size = target->GetSize();
    const bool erg = ride.mode() == Mode::Erg;
    const RideLayout layout = LayoutRide(size, erg);
    const double length = ride.course().Length();

    // A tile's live number. A dash stands for one there isn't, such as heart rate without a monitor or the gear in
    // ERG mode. An endless course has no totals to ride towards.
    wchar_t text[48];
    const bool endless = ride.course().params.endless;
    auto value = [&](Tile tile) -> const wchar_t* {
        switch (tile) {
        case Tile::Power: swprintf_s(text, L"%d", ride.power()); break;
        case Tile::Cadence: swprintf_s(text, L"%.0f", ride.cadence()); break;
        case Tile::Speed: swprintf_s(text, L"%.1f", ride.speed() * 3.6); break;
        case Tile::Grade: swprintf_s(text, L"%.1f", ride.grade() * 100 + 0.0); break;  // + 0.0 turns -0.0 into 0.0
        case Tile::Gear:
            if (erg) return L"–";
            swprintf_s(text, L"%d", ride.gear());
            break;
        case Tile::HeartRate:
            if (ride.heartRate() <= 0) return L"–";
            swprintf_s(text, L"%d", ride.heartRate());
            break;
        case Tile::AverageHeartRate:
            if (ride.averageHeartRate() <= 0) return L"–";
            swprintf_s(text, L"%d", ride.averageHeartRate());
            break;
        case Tile::MaxPower: swprintf_s(text, L"%d", ride.maxPower()); break;
        case Tile::AverageCadence: swprintf_s(text, L"%d", ride.averageCadence()); break;
        case Tile::Distance:
            if (endless) swprintf_s(text, L"%.1f", ride.distance() / 1000);
            else swprintf_s(text, L"%.1f / %.1f", ride.distance() / 1000, length / 1000);
            break;
        case Tile::Climbed:
            if (endless) swprintf_s(text, L"%.0f", ride.climbed());
            else swprintf_s(text, L"%.0f / %.0f", ride.climbed(), ride.course().targetGain);
            break;
        case Tile::AveragePower: swprintf_s(text, L"%d", ride.averagePower()); break;
        case Tile::Calories: swprintf_s(text, L"%.0f", ride.calories()); break;
        case Tile::Time: wcscpy_s(text, Clock(ride.elapsed()).c_str()); break;
        }
        return text;
    };

    // Live numbers in cards along the top and the sides, and totals in one card along the bottom.
    DrawCard(target, layout.footer);
    for (int i = 0; i < L::kTiles; ++i) {
        const Tile tile = board.tile(i);
        const TileName& name = kTileNames[static_cast<size_t>(tile)];
        ID2D1Brush* brush = tile == Tile::Power ? accentText_.get() : text_.get();
        const D2D1_RECT_F r = layout.tiles[i];
        if (i < L::kTop + 2 * L::kSide) {
            DrawCard(target, r);
            // A tall side card keeps its number with its label, as high as a top card, in the middle.
            const float band = 84.0f * s, mid = (r.top + r.bottom) / 2;
            const D2D1_RECT_F box = Inset(r, 18.0f * s, 14.0f * s);
            DrawStat(target, i < L::kTop ? box : D2D1_RECT_F{box.left, std::max(box.top, mid - band / 2), box.right,
                                                             std::min(box.bottom, mid + band / 2)},
                     name.label, value(tile), name.unit, 50.0f * s, brush);
            continue;
        }
        DrawStat(target, {r.left + 18.0f * s, r.top + 10.0f * s, r.right - 8.0f * s, r.bottom - 8.0f * s}, name.label,
                 value(tile), name.unit, 26.0f * s, brush);
        if (r.left > layout.footer.left)
            target->DrawLine({r.left, r.top + 16.0f * s}, {r.left, r.bottom - 16.0f * s}, border_.get(), 1.0f);
    }

    // The graphs: the next 2 km in detail, starting a little behind the rider and colored by grade, and the whole
    // course.
    for (int row = 0; row < L::kGraphs; ++row) {
        const D2D1_RECT_F g = layout.graphs[row];
        const D2D1_RECT_F caption{g.left + 18.0f * s, g.top + 10.0f * s, g.right - 18.0f * s, g.top + 34.0f * s};
        const D2D1_RECT_F plot{g.left + 18.0f * s, g.top + 40.0f * s, g.right - 18.0f * s, g.bottom - 12.0f * s};
        const Graph graph = board.graph(L::kTiles + row);
        DrawCard(target, g);
        DrawLabel(target, kGraphNames[static_cast<size_t>(graph)], label_.get(), caption, muted_.get());
        if (graph == Graph::Next) {
            const double from = std::clamp(ride.distance() - 250.0, 0.0, std::max(0.0, length - 2250.0));
            swprintf_s(text, L"%.0f m", ride.elevation());
            DrawLabel(target, text, label_.get(), caption, muted_.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
            DrawProfile(target, ride.course(), from, std::min(length, from + 2250.0), ride.distance(), plot, 30.0, true);
        } else {
            DrawProfile(target, ride.course(), 0.0, length, ride.distance(), plot, 50.0, false);
        }
    }

    // A message about the ride or the devices floats over the upper graph.
    const D2D1_POINT_2F pill{(layout.graphs[0].left + layout.graphs[0].right) / 2, layout.graphs[0].top + 22.0f * s};
    if (!status.empty()) DrawPill(target, pill, status, warn_.get());
    else if (ride.finished()) DrawPill(target, pill, L"Finished", good_.get());
    else if (ride.paused()) DrawPill(target, pill, L"Paused", text_.get());

    // The ERG target, a digit at a time, with leading zeros dimmed.
    if (erg) {
        const D2D1_RECT_F t = layout.target;
        DrawCard(target, t);
        DrawLabel(target, L"ERG target", label_.get(), {t.left + 18.0f * s, t.top + 10.0f * s, t.right, t.top + 34.0f * s},
                  muted_.get());
        const int w = ride.ergW();
        const wchar_t digits[3] = {wchar_t(L'0' + w / 100), wchar_t(L'0' + w / 10 % 10), wchar_t(L'0' + w % 10)};
        for (int i = 0; i < 3; ++i) {
            const D2D1_RECT_F up = layout.up[i], down = layout.down[i];
            DrawArrow(target, up, true);
            DrawArrow(target, down, false);
            const bool leading = (i == 0 && w < 100) || (i == 1 && w < 10);
            DrawLabel(target, {&digits[i], 1}, digit_.get(), {up.left, up.bottom, up.right, down.top},
                      leading ? faint_.get() : accentText_.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        DrawLabel(target, L"W", heading_.get(),
                  {layout.up[2].right + 10.0f * s, layout.up[2].bottom, t.right - 12.0f * s, layout.down[2].top},
                  muted_.get());
    }

    DrawButton(target, layout.pause, ride.paused() ? L"Resume" : L"Pause", Button::Secondary, button_.get());
    DrawButton(target, layout.end, L"End ride", Button::Secondary, button_.get());

    // A dragged card dims in place and its name follows the mouse. The slots it can swap with are outlined, and
    // the one under the mouse brighter.
    if (drag && drag->lifted && !confirmEnd) {
        const int over = layout.Hit(drag->at);
        const float w = 2.0f * std::max(1.0f, s);
        for (int i = 0; i < L::kSlots; ++i) {
            if (!L::Swappable(i, drag->from)) continue;
            const bool total = i >= L::kTop + 2 * L::kSide && i < L::kTiles;
            const float radius = (total ? 8.0f : 12.0f) * s;
            const D2D1_RECT_F slot = total ? Inset(layout.Slot(i), 4.0f * s, 6.0f * s) : layout.Slot(i);
            if (i == drag->from) {
                background_->SetOpacity(0.6f);
                target->FillRoundedRectangle(D2D1::RoundedRect(slot, radius, radius), background_.get());
                background_->SetOpacity(1.0f);
            }
            target->DrawRoundedRectangle(D2D1::RoundedRect(Inset(slot, w / 2, w / 2), radius, radius),
                                         i == over ? accent_.get() : muted_.get(), i == over ? 1.5f * w : w);
        }
        const std::wstring_view name = drag->from < L::kTiles ? kTileNames[board.slots[drag->from]].label
                                                              : kGraphNames[board.slots[drag->from]];
        DrawPill(target, {drag->at.x, drag->at.y - 28.0f * s}, name, text_.get());
    }

    // Ending the ride asks first, over a dimmed screen.
    if (confirmEnd)
        DrawConfirm(target, LayoutConfirm(size), L"End the ride?", L"It's saved, and the summary comes next.",
                    L"No, keep riding", L"Yes, end it");
}

// A question over the dimmed screen.
void Renderer::DrawConfirm(ID2D1RenderTarget* target, const ConfirmLayout& c, std::wstring_view title,
                           std::wstring_view text, std::wstring_view no, std::wstring_view yes) {
    const float s = scale_, in = 24.0f * s;
    const D2D1_SIZE_F size = target->GetSize();
    background_->SetOpacity(0.75f);
    target->FillRectangle({0, 0, size.width, size.height}, background_.get());
    background_->SetOpacity(1.0f);
    DrawCard(target, c.box);
    DrawLabel(target, title, heading_.get(), {c.box.left + in, c.box.top + 20.0f * s, c.box.right - in, c.box.top + 56.0f * s},
              text_.get());
    DrawLabel(target, text, small_.get(), {c.box.left + in, c.box.top + 60.0f * s, c.box.right - in, c.box.top + 90.0f * s},
              muted_.get());
    DrawButton(target, c.no, no, Button::Secondary, button_.get());
    DrawButton(target, c.yes, yes, Button::Primary, button_.get());
}

void Renderer::DrawSetup(ID2D1RenderTarget* target, const SetupView& view) {
    const float s = Prepare(target);
    const SetupLayout l = LayoutSetup(target->GetSize(), view.rows.size());
    const bool endless = view.course && view.course->params.endless;
    DrawHeader(target, kAppName, L"");

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
                    {p.left + 20.0f * s, p.top + 52.0f * s, p.right - 20.0f * s, chipTop - 20.0f * s},
                    endless ? 700.0 : 50.0, false);  // terrains on one scale, so mountains look bigger than rollers
    }

    // Past rides under the settings, newest first, as many as fit.
    const D2D1_RECT_F h = l.history;
    const float rowHeight = 30.0f * s, captionHeight = 40.0f * s;
    if (h.bottom - h.top >= captionHeight + rowHeight) {
        DrawCard(target, h);
        DrawLabel(target, L"History", label_.get(), {h.left + 20.0f * s, h.top + 8.0f * s, h.right, h.top + 32.0f * s},
                  muted_.get());
        DrawLabel(target, L"H see all ›", label_.get(), {h.left, h.top + 8.0f * s, h.right - 20.0f * s, h.top + 32.0f * s},
                  faint_.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
        const float left = h.left + 20.0f * s, right = h.right - 20.0f * s;
        if (view.history.empty())
            DrawLabel(target, L"No rides yet", small_.get(),
                      {left, h.top + captionHeight, right, h.top + captionHeight + rowHeight}, faint_.get());
        // The date, then distance, climb, time and average power in columns.
        const float column = (right - left) * 0.16f;
        const auto rows = static_cast<size_t>((h.bottom - h.top - captionHeight - 8.0f * s) / rowHeight);
        for (size_t i = 0; i < std::min(rows, view.history.size()); ++i) {
            const PastRide& ride = view.history[i];
            const LogTotals& t = ride.totals;
            const float top = h.top + captionHeight + rowHeight * i;
            if (i) target->DrawLine({left, top}, {right, top}, border_.get(), 1.0f);
            DrawLabel(target, ride.date, small_.get(), {left, top, right - 4 * column, top + rowHeight}, text_.get());
            wchar_t cells[4][24];
            swprintf_s(cells[0], L"%.1f km", t.distance / 1000);
            swprintf_s(cells[1], L"%.0f m", t.climbed);
            wcscpy_s(cells[2], Clock(t.seconds).c_str());
            swprintf_s(cells[3], L"%d W", t.averagePower);
            for (int c = 0; c < 4; ++c)
                DrawLabel(target, cells[c], small_.get(),
                          {right - (4 - c) * column, top, right - (3 - c) * column, top + rowHeight}, muted_.get(),
                          DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
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
        const float textRight = (endless ? l.devices : l.reroll).left - 16.0f * s;
        DrawLabel(target, text, label_.get(), {pad + 16.0f * s, top, textRight, top + lineHeight},
                  d.tone == Tone::Problem ? warn_.get() : d.tone == Tone::Good ? text_.get() : muted_.get());
    }
    DrawButton(target, l.devices, L"Devices", Button::Secondary, button_.get());
    if (!endless) DrawButton(target, l.reroll, L"New course", Button::Secondary, button_.get());
    DrawButton(target, l.start, L"Start ride", Button::Primary, button_.get());
}

void Renderer::DrawDevices(ID2D1RenderTarget* target, const DevicesView& view) {
    const float s = Prepare(target), pad = 32.0f * s, inset = 20.0f * s;
    const D2D1_SIZE_F size = target->GetSize();
    const DevicesLayout l = LayoutDevices(size, view.nearby.size());
    DrawHeader(target, L"Devices", L"");

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

void Renderer::DrawHistory(ID2D1RenderTarget* target, const HistoryView& view) {
    const float s = Prepare(target), inset = 14.0f * s, radius = 8.0f * s;
    const D2D1_SIZE_F size = target->GetSize();
    const HistoryLayout l = LayoutHistory(size);
    DrawHeader(target, L"History", L"");
    const bool any = !view.rides.empty();

    // The rides on the left, newest first, with a bar along the side when they don't all fit.
    DrawCard(target, l.list);
    if (!any && l.rowsShown)
        DrawLabel(target, L"No rides yet", row_.get(), Inset(l.rows[0], inset, 0), muted_.get());
    for (size_t row = 0; row < l.rowsShown && view.first + row < view.rides.size(); ++row) {
        const size_t i = view.first + row;
        const PastRide& ride = view.rides[i];
        const D2D1_RECT_F r = l.rows[row];
        const bool selected = i == view.selected;
        if (selected) {
            target->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), raised_.get());
            target->FillRoundedRectangle(
                D2D1::RoundedRect({r.left, r.top + 12.0f * s, r.left + 3.0f * s, r.bottom - 12.0f * s}, 1.5f * s, 1.5f * s),
                accent_.get());
        }
        const float mid = (r.top + r.bottom) / 2;
        DrawLabel(target, ride.date, row_.get(), {r.left + inset, r.top + 4.0f * s, r.right - inset, mid + 2.0f * s},
                  selected ? text_.get() : muted_.get());
        wchar_t line[64];
        swprintf_s(line, L"%.1f km · %s · %d W", ride.totals.distance / 1000, Clock(ride.totals.seconds).c_str(),
                   ride.totals.averagePower);
        DrawLabel(target, line, small_.get(), {r.left + inset, mid, r.right - inset, r.bottom - 6.0f * s}, faint_.get());
    }
    if (view.rides.size() > l.rowsShown && l.rowsShown) {
        const float top = l.rows[0].top, height = l.rows[l.rowsShown - 1].bottom - top, x = l.list.right - 5.0f * s;
        const float n = static_cast<float>(view.rides.size());
        const float a = top + height * view.first / n, b = top + height * (view.first + l.rowsShown) / n;
        target->FillRoundedRectangle(D2D1::RoundedRect({x - 1.5f * s, a, x + 1.5f * s, std::min(b, top + height)}, 1.5f * s,
                                                       1.5f * s),
                                     border_.get());
    }

    // The selected ride: where it went, its numbers, and its power and elevation from start to end.
    if (any) {
        const PastRide& ride = view.rides[view.selected];
        const LogTotals& t = ride.totals;
        const D2D1_RECT_F d = l.detail;
        DrawCard(target, d);
        const float left = d.left + 24.0f * s, right = d.right - 24.0f * s;
        DrawLabel(target, ride.date, heading_.get(), {left, d.top + 16.0f * s, right, d.top + 50.0f * s}, text_.get());
        DrawLabel(target, ride.course, small_.get(), {left, d.top + 50.0f * s, right, d.top + 74.0f * s}, muted_.get());

        wchar_t distance[16], climbed[16], speed[16], kcal[16], avg[16], max[16], cadence[16], heart[16], peak[16];
        swprintf_s(distance, L"%.1f", t.distance / 1000);
        swprintf_s(climbed, L"%.0f", t.climbed);
        swprintf_s(speed, L"%.1f", t.seconds ? t.distance / t.seconds * 3.6 : 0.0);
        swprintf_s(kcal, L"%.0f", t.calories);
        swprintf_s(avg, L"%d", t.averagePower);
        swprintf_s(max, L"%d", t.maxPower);
        // A dash for what wasn't measured, such as heart rate without a monitor.
        auto optional = [](wchar_t (&buf)[16], int v) { v > 0 ? swprintf_s(buf, L"%d", v) : wcscpy_s(buf, L"–"); };
        optional(cadence, t.averageCadence);
        optional(heart, t.averageHeartRate);
        optional(peak, t.maxHeartRate);
        const std::wstring time = Clock(t.seconds);
        struct Stat {
            std::wstring_view label, value, unit;
        } const stats[] = {{L"Time", time, L""},           {L"Distance", distance, L"km"},
                           {L"Climbed", climbed, L"m"},     {L"Average speed", speed, L"km/h"},
                           {L"Calories", kcal, L"kcal"},    {L"Average power", avg, L"W"},
                           {L"Max power", max, L"W"},       {L"Average cadence", cadence, L"rpm"},
                           {L"Average heart rate", heart, L"bpm"}, {L"Max heart rate", peak, L"bpm"}};
        constexpr size_t kColumns = 5;
        const float top = d.top + 86.0f * s, cellWidth = (right - left) / kColumns, cellHeight = 64.0f * s;
        for (size_t i = 0; i < std::size(stats); ++i) {
            const float x = left + cellWidth * (i % kColumns), y = top + cellHeight * (i / kColumns);
            DrawStat(target, {x, y, x + cellWidth - 12.0f * s, y + cellHeight - 6.0f * s}, stats[i].label, stats[i].value,
                     stats[i].unit, 28.0f * s, i == 5 ? accentText_.get() : text_.get());
        }

        // The route, lit as far as it was ridden. Of an endless one, only the part ridden.
        const float routeTop = top + 2 * cellHeight + 12.0f * s;
        const float graphTop = routeTop + std::round((d.bottom - routeTop) / 2);
        target->DrawLine({left, routeTop - 6.0f * s}, {right, routeTop - 6.0f * s}, border_.get(), 1.0f);
        if (view.route) {
            const Course& route = *view.route;
            const double ridden = std::min(t.distance, route.Length());
            const bool endless = route.params.endless;
            const double shown = endless ? std::clamp(ridden, 1000.0, route.Length()) : route.Length();
            wchar_t caption[64];
            if (endless || ridden >= route.Length() - kCourseStep) swprintf_s(caption, L"%.1f km", ridden / 1000);
            else swprintf_s(caption, L"%.1f of %.1f km", ridden / 1000, route.Length() / 1000);
            DrawLabel(target, L"Route", label_.get(), {left, routeTop, right, routeTop + 24.0f * s}, muted_.get());
            DrawLabel(target, caption, label_.get(), {left, routeTop, right, routeTop + 24.0f * s}, muted_.get(),
                      DWRITE_TEXT_ALIGNMENT_TRAILING);
            DrawProfile(target, route, 0.0, shown, ridden, {left, routeTop + 30.0f * s, right, graphTop - 18.0f * s}, 50.0,
                        false);
        }

        target->DrawLine({left, graphTop - 6.0f * s}, {right, graphTop - 6.0f * s}, border_.get(), 1.0f);
        DrawLabel(target, L"Power", label_.get(), {left, graphTop, right, graphTop + 24.0f * s}, accentText_.get());
        DrawLabel(target, L"Elevation", label_.get(), {left, graphTop, right, graphTop + 24.0f * s}, muted_.get(),
                  DWRITE_TEXT_ALIGNMENT_TRAILING);
        DrawRideGraph(target, view.samples, {left, graphTop + 30.0f * s, right, d.bottom - 14.0f * s});
    }

    DrawButton(target, l.remove, L"Delete", Button::Secondary, button_.get(), any);
    DrawButton(target, l.file, L"Show file", Button::Secondary, button_.get(), any);
    DrawButton(target, l.back, L"Back", Button::Primary, button_.get());

    if (view.confirmDelete)
        DrawConfirm(target, LayoutConfirm(size, false), L"Delete this ride?",
                    L"Its log and FIT file go to the Recycle Bin.", L"No, keep it", L"Yes, delete");
}

// A ride's power as a line over its elevation as a shape, from start to end. Power runs up the left and the time
// along the bottom. Each column averages its stretch of seconds, so the line shows efforts rather than noise.
void Renderer::DrawRideGraph(ID2D1RenderTarget* target, std::span<const LogSample> samples, D2D1_RECT_F box) {
    const D2D1_RECT_F frame = box;
    box = {box.left + 52.0f * scale_, box.top, box.right, box.bottom - 22.0f * scale_};
    const float width = box.right - box.left, height = box.bottom - box.top;
    if (samples.size() < 2 || width < 2 || height < 2) return;
    const size_t columns = std::clamp<size_t>(static_cast<size_t>(width / 3.0f), 2, samples.size());
    const float dx = width / (columns - 1);
    std::vector<float> power(columns);
    heights_.resize(columns);
    float top = 0, lo = 1e9f, hi = -1e9f;
    for (size_t c = 0; c < columns; ++c) {
        const size_t a = samples.size() * c / columns, b = samples.size() * (c + 1) / columns;
        double sum = 0;
        for (size_t i = a; i < b; ++i) sum += samples[i].power;
        power[c] = static_cast<float>(sum / (b - a));
        heights_[c] = static_cast<float>(samples[(a + b) / 2].elevation);
        top = std::max(top, power[c]);
        lo = std::min(lo, heights_[c]);
        hi = std::max(hi, heights_[c]);
    }
    // Power fills the height. The elevation keeps to the lower half, and flat roads stay flat.
    const double watts = std::max(100.0, top * 1.1);
    const float span = std::max(hi - lo, 30.0f);
    auto yPower = [&](double w) { return height - static_cast<float>(w / watts) * height; };
    auto yElevation = [&](float e) { return height - (e - lo) / span * height * 0.5f; };

    wchar_t tick[24];
    const double step = NiceStep(watts / 4);
    for (double w = 0; w <= watts; w += step) {
        const float at = box.top + yPower(w);
        target->DrawLine({box.left, at}, {box.right, at}, border_.get(), 1.0f);
        swprintf_s(tick, L"%.0f W", w);
        DrawLabel(target, tick, label_.get(), {frame.left, at - 10.0f * scale_, box.left - 8.0f * scale_, at + 10.0f * scale_},
                  faint_.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    // Ticks every round number of minutes.
    const double seconds = static_cast<double>(samples.size());
    double stride = 60;
    for (const double m : {1, 2, 5, 10, 15, 20, 30, 60, 120, 240})
        if (stride = m * 60; seconds / stride <= width / (110.0f * scale_)) break;
    for (double t = 0; t <= seconds; t += stride) {
        const float at = box.left + static_cast<float>(t / seconds) * width;
        target->DrawLine({at, box.bottom}, {at, box.bottom + 4.0f * scale_}, faint_.get(), 1.0f);
        const int m = static_cast<int>(t / 60);
        swprintf_s(tick, L"%d:%02d", m / 60, m % 60);
        const bool last = at > box.right - 24.0f * scale_;
        DrawLabel(target, tick, label_.get(),
                  {at - 100.0f * scale_, box.bottom + 4.0f * scale_, at + (last ? 6.0f : 100.0f) * scale_, frame.bottom},
                  faint_.get(), last ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // A shape through the columns' points, filled down to the bottom when closed.
    auto shape = [&](auto y, bool closed) {
        winrt::com_ptr<ID2D1PathGeometry> path;
        winrt::com_ptr<ID2D1GeometrySink> sink;
        winrt::check_hresult(factory_->CreatePathGeometry(path.put()));
        winrt::check_hresult(path->Open(sink.put()));
        sink->BeginFigure(closed ? D2D1_POINT_2F{0, height} : D2D1_POINT_2F{0, y(0)},
                          closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (size_t i = closed ? 0 : 1; i < columns; ++i) sink->AddLine({dx * i, y(i)});
        if (closed) sink->AddLine({width, height});
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        winrt::check_hresult(sink->Close());
        return path;
    };
    D2D1_MATRIX_3X2_F saved;
    target->GetTransform(&saved);
    target->SetTransform(D2D1::Matrix3x2F::Translation(box.left, box.top) * saved);
    const float s = std::max(1.0f, scale_);
    trackFill_->SetStartPoint({0, height / 2});
    trackFill_->SetEndPoint({0, height});
    target->FillGeometry(shape([&](size_t i) { return yElevation(heights_[i]); }, true).get(), trackFill_.get());
    target->DrawGeometry(shape([&](size_t i) { return yPower(power[i]); }, false).get(), accentText_.get(), 1.5f * s);
    target->SetTransform(saved);
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
    if (tabular_) layout->SetTypography(tabular_.get(), {0, valueLength});
    auto size = [&](float v) {
        layout->SetFontSize(v, {0, valueLength});
        if (valueLength < length) layout->SetFontSize(std::max(14.0f * s, v * 0.4f), {valueLength, length - valueLength});
    };
    size(valueSize);
    if (valueLength < length) {
        layout->SetFontWeight(DWRITE_FONT_WEIGHT_NORMAL, {valueLength, length - valueLength});
        layout->SetDrawingEffect(muted_.get(), {valueLength, length - valueLength});
    }
    // A long value, such as the distance in a narrow card, shrinks to fit rather than being cut short.
    const DWRITE_TRIMMING none{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
    layout->SetTrimming(&none, nullptr);
    DWRITE_TEXT_METRICS m{};
    const float room = box.right - box.left;
    if (SUCCEEDED(layout->GetMetrics(&m)) && m.widthIncludingTrailingWhitespace > room)
        size(valueSize * room / m.widthIncludingTrailingWhitespace * 0.97f);
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
        for (ProfileCache& c : cache_)
            if (c.fill && c.params == course.params && c.samples == columns && c.from == from && c.to == to &&
                c.width == width && c.height == height)
                hit = &c;
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

// A button with a triangle pointing up or down.
void Renderer::DrawArrow(ID2D1RenderTarget* target, D2D1_RECT_F box, bool up) {
    const float s = scale_, radius = 10.0f * s;
    target->FillRoundedRectangle(D2D1::RoundedRect(Inset(box, 0, 3.0f * s), radius, radius), raised_.get());
    const float cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2, w = 11.0f * s, h = 7.0f * s;
    winrt::com_ptr<ID2D1PathGeometry> path;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    winrt::check_hresult(factory_->CreatePathGeometry(path.put()));
    winrt::check_hresult(path->Open(sink.put()));
    sink->BeginFigure({cx, up ? cy - h : cy + h}, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine({cx + w, up ? cy + h : cy - h});
    sink->AddLine({cx - w, up ? cy + h : cy - h});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    winrt::check_hresult(sink->Close());
    target->FillGeometry(path.get(), text_.get());
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
