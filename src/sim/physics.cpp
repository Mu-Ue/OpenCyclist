#include "sim/physics.h"

#include <algorithm>
#include <cmath>

#include "config.h"

namespace physics {
namespace {

// Gravity and rolling resistance, which don't depend on speed.
double SlopeForce(double grade, double mass) {
    const double theta = std::atan(grade);
    return mass * kGravity * (std::sin(theta) + kCrr * std::cos(theta));
}

double DragForce(double v) { return 0.5 * kAirDensity * kCdA * v * v; }

}  // namespace

double PowerFor(double v, double grade, double mass) {
    return (SlopeForce(grade, mass) + DragForce(v)) * v / (1.0 - kDrivetrainLoss);
}

double Step(double v, double power, double grade, double mass, double dt) {
    // Pedal force is power over speed, held finite from a standstill.
    const double drive = std::max(power, 0.0) * (1.0 - kDrivetrainLoss) / std::max(v, 1.0);
    double resist = SlopeForce(grade, mass) + DragForce(v);
    // Rolling resistance and the slope can't push a stopped bike backwards.
    if (v <= 0.0 && drive < resist) return 0.0;
    const double a = (drive - resist) / (mass + kWheelInertiaKg);
    return std::max(0.0, v + a * dt);
}

double SteadySpeed(double power, double grade, double mass) {
    double lo = 0.0, hi = 40.0;
    for (int i = 0; i < 60; ++i) {
        const double mid = 0.5 * (lo + hi);
        (PowerFor(mid, grade, mass) < power ? lo : hi) = mid;
    }
    return lo;
}

}  // namespace physics
