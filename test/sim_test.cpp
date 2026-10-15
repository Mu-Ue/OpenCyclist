// Checks the course generator and the physics. Run build\sim_test.exe; it prints failures and exits non-zero.
#include <cmath>
#include <cstdio>

#include "config.h"
#include "sim/course.h"
#include "sim/physics.h"
#include "sim/ride.h"

static int failures = 0;

static void Check(bool ok, const char* what, const CourseParams& p, double value) {
    if (ok) return;
    ++failures;
    std::printf("FAIL %s: %.3f (km %.0f gain %.0f max %.0f%% loop %d seed %u)\n", what, value, p.km, p.gainM,
                p.maxGradePct, p.loop, p.seed);
}

int main() {
    const double kms[] = {5, 30, 100, 200};
    const double gains[] = {0, 150, 1000, 2500};
    const double maxGrades[] = {6, 10, 15};
    int courses = 0;
    for (uint32_t seed = 1; seed <= 40; ++seed)
        for (double km : kms)
            for (double gain : gains)
                for (double maxGrade : maxGrades)
                    for (bool loop : {false, true}) {
                        const CourseParams p{km, gain, maxGrade, loop, seed};
                        const Course c = GenerateCourse(p);
                        ++courses;
                        Check(std::abs(c.Length() - km * 1000) < 1e-6, "length", p, c.Length());
                        const double tolerance = std::max(2.0, c.targetGain * 0.01);
                        Check(std::abs(c.Gain() - c.targetGain) <= tolerance, "gain", p, c.Gain());
                        double steepest = 0, sharpest = 0, lowest = 1e9;
                        double prevGrade = (c.elevation[1] - c.elevation[0]) / kCourseStep;
                        for (size_t i = 1; i < c.elevation.size(); ++i) {
                            const double grade = (c.elevation[i] - c.elevation[i - 1]) / kCourseStep;
                            steepest = std::max(steepest, std::abs(grade));
                            sharpest = std::max(sharpest, std::abs(grade - prevGrade));
                            lowest = std::min(lowest, double(c.elevation[i]));
                            prevGrade = grade;
                        }
                        Check(steepest <= maxGrade / 100 + 1e-4, "max grade", p, steepest * 100);
                        Check(sharpest <= kMaxGradeChange * 1.5 + 1e-4, "grade change", p, sharpest * 100);
                        Check(lowest >= 0, "below sea level", p, lowest);
                        if (loop)
                            Check(std::abs(c.elevation.back() - c.elevation.front()) <= tolerance, "loop closes", p,
                                  c.elevation.back() - c.elevation.front());
                        Check(GenerateCourse(p).elevation == c.elevation, "repeatable", p, 0);
                    }

    const double flat = physics::SteadySpeed(250, 0.0, 84) * 3.6;
    const double climb = physics::SteadySpeed(250, 0.08, 84) * 3.6;
    if (flat < 34 || flat > 39) ++failures, std::printf("FAIL flat speed %.1f km/h\n", flat);
    if (climb < 11 || climb > 15) ++failures, std::printf("FAIL 8%% speed %.1f km/h\n", climb);

    // A whole ride at a steady 250 W, 84 kg, over 30 km with 1000 m of climbing.
    Ride ride(GenerateCourse({30, 1000, 10, false, 7}), Mode::Erg, 84, 250, 0);
    ride.OnPower(250);
    ride.OnCadence(90);
    while (!ride.finished() && ride.elapsed() < 6 * 3600) ride.Tick(kTickMs / 1000.0);
    const double hours = ride.elapsed() / 3600;
    if (!ride.finished() || std::abs(ride.climbed() - ride.course().Gain()) > 1) ++failures, std::printf("FAIL ride\n");

    std::printf("%d courses, 250 W: %.1f km/h flat, %.1f km/h at 8%%, 30 km and %.0f m up in %d:%02d. %s\n", courses,
                flat, climb, ride.climbed(), int(hours), int(hours * 60) % 60, failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
