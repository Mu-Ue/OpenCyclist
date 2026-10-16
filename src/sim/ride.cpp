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

Ride::Ride(Course course, Mode mode, double massKg, int ergW)
    : course_(std::move(course)), mode_(mode), mass_(massKg), ergW_(std::clamp(ergW, 0, kMaxErgW)),
      gear_(kStartGear) {}

void Ride::Tick(double dt) {
    if (!paused_ && !finished_) {
        // Before the height is taken, since growing the road can lift it.
        while (course_.params.endless && course_.Length() - distance_ < kEndlessAheadM) course_.Extend();
        const double before = elevation();
        speed_ = physics::Step(speed_, power_, grade(), mass_, dt);
        distance_ = std::min(distance_ + speed_ * dt, course_.Length());
        climbed_ += std::max(0.0, elevation() - before);
        if (speed_ > 0.0) {
            elapsed_ += dt;
            cadenceSum_ += cadence_ * dt;
        }
        work_ += power_ * dt;
        maxPower_ = std::max(maxPower_, power_);
        if (heartRate_ > 0) heartSum_ += heartRate_ * dt, heartTime_ += dt;
        if (distance_ >= course_.Length()) {
            finished_ = true;
            speed_ = 0.0;
        }
    }

    // Smoothed so a cadence wobble doesn't jerk the resistance.
    simTarget_ += (SimPower() - simTarget_) * 0.5;
}

// Virtual gears: the cadence and gear give a road speed, and the trainer holds the power that speed takes on this
// grade.
double Ride::SimPower() const {
    const double v = cadence_ / 60.0 * GearRatio(gear_) * kWheelCircumference;
    return std::clamp(physics::PowerFor(v, grade(), mass_), 0.0, double(kMaxTargetW));
}

double Ride::calories() const { return work_ / 4184.0 / kGrossEfficiency; }
int Ride::averagePower() const { return elapsed_ > 0 ? static_cast<int>(std::lround(work_ / elapsed_)) : 0; }
int Ride::averageCadence() const { return elapsed_ > 0 ? static_cast<int>(std::lround(cadenceSum_ / elapsed_)) : 0; }
int Ride::averageHeartRate() const {
    return heartTime_ > 0 ? static_cast<int>(std::lround(heartSum_ / heartTime_)) : 0;
}

void Ride::Adjust(int dir, bool big) {
    switch (mode_) {
    case Mode::Sim:
        // A shift takes hold at once, like a real gear, rather than easing in with the cadence.
        gear_ = std::clamp(gear_ + dir * (big ? 4 : 1), 1, kGearCount);
        simTarget_ = SimPower();
        break;
    case Mode::Erg: SetErgW(ergW_ + dir * (big ? 25 : 5)); break;
    }
}

void Ride::SetErgW(int watts) { ergW_ = std::clamp(watts, 0, kMaxErgW); }

void Ride::NextMode() {
    mode_ = mode_ == Mode::Sim ? Mode::Erg : Mode::Sim;
}

TrainerTarget Ride::Target() const {
    using Kind = TrainerTarget::Kind;
    if (paused_ || finished_) return {Kind::Resistance, 0};
    return {Kind::Power, mode_ == Mode::Erg ? ergW_ : static_cast<int>(std::lround(simTarget_))};
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
