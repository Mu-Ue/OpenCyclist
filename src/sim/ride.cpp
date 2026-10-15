#include "sim/ride.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "config.h"
#include "sim/physics.h"

namespace {

double GearRatio(int gear) {
    return kLowestRatio * std::pow(kHighestRatio / kLowestRatio, (gear - 1) / double(kGearCount - 1));
}

}  // namespace

Ride::Ride(Course course, Mode mode, double massKg, int ergW, int resistancePct)
    : course_(std::move(course)), mode_(mode), mass_(massKg), ergW_(ergW), resistancePct_(resistancePct),
      gear_(kStartGear) {}

void Ride::Tick(double dt) {
    if (!paused_ && !finished_) {
        const double before = elevation();
        speed_ = physics::Step(speed_, power_, grade(), mass_, dt);
        distance_ = std::min(distance_ + speed_ * dt, course_.Length());
        climbed_ += std::max(0.0, elevation() - before);
        if (speed_ > 0.0) elapsed_ += dt;
        if (distance_ >= course_.Length()) {
            finished_ = true;
            speed_ = 0.0;
        }
    }

    // Virtual gears: the cadence and gear give a road speed, and the trainer holds the power that speed takes on
    // this grade. Smoothed so a cadence wobble doesn't jerk the resistance.
    const double v = cadence_ / 60.0 * GearRatio(gear_) * kWheelCircumference;
    const double want = std::clamp(physics::PowerFor(v, grade(), mass_), 0.0, double(kMaxTargetW));
    simTarget_ += (want - simTarget_) * 0.5;
}

void Ride::Adjust(int dir, bool big) {
    switch (mode_) {
    case Mode::Sim: gear_ = std::clamp(gear_ + dir * (big ? 4 : 1), 1, kGearCount); break;
    case Mode::Erg: ergW_ = std::clamp(ergW_ + dir * (big ? 25 : 5), 0, kMaxTargetW); break;
    case Mode::Resistance: resistancePct_ = std::clamp(resistancePct_ + dir * (big ? 10 : 5), 0, 100); break;
    }
}

void Ride::NextMode() {
    mode_ = mode_ == Mode::Sim ? Mode::Erg : mode_ == Mode::Erg ? Mode::Resistance : Mode::Sim;
}

TrainerTarget Ride::Target() const {
    using Kind = TrainerTarget::Kind;
    if (paused_ || finished_) return {Kind::Resistance, 0};
    switch (mode_) {
    case Mode::Sim: return {Kind::Power, static_cast<int>(std::lround(simTarget_))};
    case Mode::Erg: return {Kind::Power, ergW_};
    case Mode::Resistance: break;
    }
    return {Kind::Resistance, resistancePct_};
}

double EstimateTime(const Course& course, double massKg, int watts) {
    const double length = course.Length();
    double v = 0.0, d = 0.0, t = 0.0;
    while (d < length) {
        v = physics::Step(v, watts, course.GradeAt(d), massKg, 1.0);
        if (v <= 0.0 || t > 100 * 3600.0) return 0.0;
        d += v;
        t += 1.0;
    }
    return t;
}
