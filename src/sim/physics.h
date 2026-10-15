#pragma once

// A rider on a road bike outdoors. Grades are fractions (0.05 = 5 %), speeds m/s, power W, mass kg of rider
// plus bike.
namespace physics {

// Pedal power needed to hold speed v on a grade. Negative when gravity alone would go faster.
double PowerFor(double v, double grade, double mass);

// Speed after dt seconds of pedalling at the given power, with inertia.
double Step(double v, double power, double grade, double mass, double dt);

// The speed the rider settles at.
double SteadySpeed(double power, double grade, double mass);

}  // namespace physics
