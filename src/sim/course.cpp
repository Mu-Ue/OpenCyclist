#include "sim/course.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>

#include "config.h"

namespace {

// Climbs and descents share the length, so together they can't beat the max grade over all of it. This leaves
// headroom so the climbs aren't one long wall at the max grade.
constexpr double kHeadroom = 0.6;

// Climbing per stretch of an endless course, from flat to mountains.
constexpr double kTerrainGain[kTerrainCount] = {0.0, 150.0, 350.0, 600.0};

size_t Steps(const CourseParams& params) {
    return std::max<size_t>(1, static_cast<size_t>(std::lround(params.km * 1000.0 / kCourseStep)));
}

// SplitMix64: tiny, fast, and gives the same numbers on every compiler, unlike the std distributions.
struct Rng {
    uint64_t state;
    uint64_t Next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double Uniform(double a, double b) { return a + (b - a) * static_cast<double>(Next() >> 11) * 0x1.0p-53; }
};

// Smooth random wiggle: random values every `wavelength` m, eased between with a cosine.
void AddNoise(std::vector<double>& g, Rng& rng, double wavelength, double amplitude) {
    const double knotSteps = wavelength / kCourseStep;
    double prev = rng.Uniform(-amplitude, amplitude), next = rng.Uniform(-amplitude, amplitude);
    size_t knot = 0;
    for (size_t i = 0; i < g.size(); ++i) {
        const double t = static_cast<double>(i) / knotSteps - static_cast<double>(knot);
        if (t >= 1.0) {
            prev = next;
            next = rng.Uniform(-amplitude, amplitude);
            ++knot;
        }
        const double u = static_cast<double>(i) / knotSteps - static_cast<double>(knot);
        const double w = 0.5 - 0.5 * std::cos(u * std::numbers::pi);
        g[i] += prev + (next - prev) * w;
    }
}

// Limits how fast the grade changes from step to step. The backward pass alone guarantees the limit; the
// forward pass first keeps ramps symmetric.
void LimitGradeChange(std::vector<double>& g) {
    for (size_t i = 1; i < g.size(); ++i) g[i] = std::clamp(g[i], g[i - 1] - kMaxGradeChange, g[i - 1] + kMaxGradeChange);
    for (size_t i = g.size() - 1; i-- > 0;)
        g[i] = std::clamp(g[i], g[i + 1] - kMaxGradeChange, g[i + 1] + kMaxGradeChange);
}

// Maps every grade to clamp(scale * g + offset) so the climbs total `gain` and the course ends at its start
// height. One scale and offset for the whole course keep it as smooth as it was. The end height only grows with
// the offset and, with the offset solved for, the gain only grows with the scale, so two nested bisections find
// them. Sorted grades with prefix sums make each total a pair of binary searches.
void FitTotals(std::vector<double>& g, double gain, double maxGrade) {
    std::vector<double> sorted(g);
    std::sort(sorted.begin(), sorted.end());
    std::vector<double> prefix(sorted.size() + 1, 0.0);
    for (size_t i = 0; i < sorted.size(); ++i) prefix[i + 1] = prefix[i] + sorted[i];
    auto below = [&](double t) { return static_cast<size_t>(std::lower_bound(sorted.begin(), sorted.end(), t) - sorted.begin()); };
    // Sum of clamp(scale * x + offset, floor, maxGrade) over all grades.
    auto clampedSum = [&](double scale, double offset, double floor) {
        const size_t a = below((floor - offset) / scale), b = std::max(a, below((maxGrade - offset) / scale));
        return floor * static_cast<double>(a) + scale * (prefix[b] - prefix[a]) + offset * static_cast<double>(b - a) +
               maxGrade * static_cast<double>(sorted.size() - b);
    };
    auto offsetFor = [&](double scale) {
        double lo = -2.0 * maxGrade - 1.0, hi = 2.0 * maxGrade + 1.0;
        for (int i = 0; i < 50; ++i) {
            const double mid = 0.5 * (lo + hi);
            (clampedSum(scale, mid, -maxGrade) < 0.0 ? lo : hi) = mid;
        }
        return hi;
    };
    auto climbFor = [&](double scale, double offset) { return clampedSum(scale, offset, 0.0) * kCourseStep; };
    auto map = [&](double x, double scale, double offset) { return std::clamp(scale * x + offset, -maxGrade, maxGrade); };
    double lo = 1e-9, hi = 1.0;
    while (climbFor(hi, offsetFor(hi)) < gain && hi < 1e4) hi *= 2.0;
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        (climbFor(mid, offsetFor(mid)) < gain ? lo : hi) = mid;
    }
    const double offset = offsetFor(hi);
    for (double& x : g) x = map(x, hi, offset);
}

}  // namespace

double MaxGain(const CourseParams& params) {
    const double length = static_cast<double>(Steps(params)) * kCourseStep;
    return length * std::clamp(params.maxGradePct, 1.0, 25.0) / 100.0 * kHeadroom / 2.0;
}

double Course::Length() const { return elevation.size() < 2 ? 0.0 : (elevation.size() - 1) * kCourseStep; }

double Course::ElevationAt(double distance) const {
    if (elevation.empty()) return 0.0;
    const double x = std::clamp(distance, 0.0, Length()) / kCourseStep;
    const size_t i = std::min(static_cast<size_t>(x), elevation.size() - 1);
    if (i + 1 >= elevation.size()) return elevation.back();
    return elevation[i] + (elevation[i + 1] - elevation[i]) * (x - static_cast<double>(i));
}

double Course::GradeAt(double distance) const {
    const double half = kCourseStep / 2;
    const double a = std::max(distance - half, 0.0), b = std::min(distance + half, Length());
    return b > a ? (ElevationAt(b) - ElevationAt(a)) / (b - a) : 0.0;
}

double Course::Gain() const {
    double gain = 0.0;
    for (size_t i = 1; i < elevation.size(); ++i) gain += std::max(0.0f, elevation[i] - elevation[i - 1]);
    return gain;
}

// Each stretch has its own seed. Grades ease from the old road into the new one as fast as real roads change.
void Course::Extend() {
    CourseParams next = params;
    next.seed += static_cast<uint32_t>(std::lround(Length() / (kEndlessStretchKm * 1000.0)));
    const std::vector<float> stretch = GenerateCourse(next).elevation;
    const size_t n = elevation.size();
    double grade = n < 2 ? 0.0 : (elevation[n - 1] - elevation[n - 2]) / kCourseStep;
    double e = n ? elevation.back() : 0.0, lowest = 0.0;
    if (!n) elevation.push_back(0.0f);
    elevation.reserve(elevation.size() + stretch.size() - 1);
    for (size_t i = 1; i < stretch.size(); ++i) {
        grade = std::clamp((stretch[i] - stretch[i - 1]) / kCourseStep, grade - kMaxGradeChange, grade + kMaxGradeChange);
        e += grade * kCourseStep;
        lowest = std::min(lowest, e);
        elevation.push_back(static_cast<float>(e));
    }
    // Heights stay above 0 m, like any course.
    if (lowest < 0.0)
        for (float& x : elevation) x -= static_cast<float>(lowest);
}

uint32_t RandomSeed() { return std::random_device{}() | 1u; }

Course GenerateCourse(const CourseParams& params) {
    // An endless course starts with one stretch. It ends at its start height, so the road doesn't drift up or down.
    if (params.endless) {
        CourseParams stretch = params;
        stretch.km = kEndlessStretchKm;
        stretch.endless = false;
        stretch.gainM = std::min(kTerrainGain[std::clamp(params.terrain, 0, kTerrainCount - 1)], MaxGain(stretch));
        Course course = GenerateCourse(stretch);
        course.params = params;
        return course;
    }

    Course course;
    course.params = params;
    const size_t n = Steps(params);
    const double length = static_cast<double>(n) * kCourseStep;
    const double maxGrade = std::clamp(params.maxGradePct, 1.0, 25.0) / 100.0;
    Rng rng{params.seed};

    // Every course descends as much as it climbs and ends at its start height. Loop and point to point get the
    // same course, so the route type is only a name.
    const double gain = std::clamp(params.gainM, 0.0, MaxGain(params));
    course.targetGain = gain;

    std::vector<double> g(n, 0.0);
    if (gain > 0.0) {
        // Lay out climbs, descents, flats and rollers. Hillier courses have fewer flats, and the choice between
        // climbing and descending leans back towards the start height.
        const double hilliness = gain / length;
        const double flatChance = std::clamp(1.0 - hilliness * 40.0, 0.1, 0.8);
        const double longest = std::clamp(length / 4.0, 600.0, 8000.0);
        double height = 0.0;
        for (size_t i = 0; i < n;) {
            const double r = rng.Uniform(0.0, 1.0);
            double meters, grade = 0.0, rollerAmp = 0.0, rollerPeriod = 1.0;
            if (r < flatChance) {
                meters = rng.Uniform(300.0, 2500.0);
                grade = rng.Uniform(-0.005, 0.005);
            } else if (rng.Uniform(0.0, 1.0) < 0.25) {
                meters = rng.Uniform(800.0, 4000.0);
                rollerAmp = rng.Uniform(0.02, 0.05);
                rollerPeriod = rng.Uniform(300.0, 900.0);
            } else {
                const double climbChance = std::clamp(0.5 - height / (gain + 1.0), 0.15, 0.85);
                meters = std::exp(rng.Uniform(std::log(400.0), std::log(longest)));
                grade = rng.Uniform(0.03, 0.08) * (rng.Uniform(0.0, 1.0) < climbChance ? 1.0 : -1.0);
            }
            const size_t end = std::min(n, i + std::max<size_t>(1, static_cast<size_t>(meters / kCourseStep)));
            const double phase = rng.Uniform(0.0, 2.0 * std::numbers::pi);
            for (size_t j = i; j < end; ++j) {
                const double x = static_cast<double>(j - i) * kCourseStep;
                g[j] = grade + rollerAmp * std::sin(phase + 2.0 * std::numbers::pi * x / rollerPeriod);
                height += g[j] * kCourseStep;
            }
            i = end;
        }

        // Variation within each segment, from long swells down to short kicks.
        AddNoise(g, rng, 1500.0, 0.012);
        AddNoise(g, rng, 400.0, 0.006);
        AddNoise(g, rng, 100.0, 0.003);

        // Smoothing and fitting fight each other a little, so alternate until both settle. Fitting goes last so
        // the totals are exact.
        for (int pass = 0; pass < 6; ++pass) {
            LimitGradeChange(g);
            FitTotals(g, gain, maxGrade);
        }
    }

    course.elevation.resize(n + 1);
    // Heights count up from the lowest point, so the profile reads as how far up the rider is.
    double e = 0.0, lowest = 0.0;
    for (size_t i = 0; i < n; ++i) {
        e += g[i] * kCourseStep;
        lowest = std::min(lowest, e);
        course.elevation[i + 1] = static_cast<float>(e);
    }
    for (float& x : course.elevation) x -= static_cast<float>(lowest);
    return course;
}
