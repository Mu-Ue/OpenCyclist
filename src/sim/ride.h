#pragma once

#include "sim/course.h"

enum class Mode { Sim, Erg, Resistance };

// What the trainer should do: hold a power (ERG) or a resistance level in percent of its range.
struct TrainerTarget {
    enum class Kind { Power, Resistance } kind = Kind::Resistance;
    int value = 0;
};

class Ride {
public:
    Ride(Course course, Mode mode, double massKg, int ergW, int resistancePct);

    void OnPower(int watts) { power_ = watts; }
    void OnCadence(double rpm) { cadence_ = rpm; }
    void OnHeartRate(int bpm) { heartRate_ = bpm; }  // -1 without a heart rate monitor, 0 when it has nothing
    void ClearInputs() { power_ = 0; cadence_ = 0.0; }

    void Tick(double dt);

    // dir is +1 for harder, -1 for easier. Big steps come from the D-pad.
    void Adjust(int dir, bool big);
    void NextMode();
    void SetMode(Mode mode) { mode_ = mode; }
    void TogglePause() { paused_ = !paused_; }

    TrainerTarget Target() const;

    const Course& course() const { return course_; }
    Mode mode() const { return mode_; }
    int gear() const { return gear_; }
    int ergW() const { return ergW_; }
    int resistancePct() const { return resistancePct_; }
    int power() const { return power_; }
    double cadence() const { return cadence_; }
    int heartRate() const { return heartRate_; }
    double speed() const { return speed_; }  // m/s
    double distance() const { return distance_; }
    double elevation() const { return course_.ElevationAt(distance_); }
    double grade() const { return course_.GradeAt(distance_); }
    double climbed() const { return climbed_; }
    double elapsed() const { return elapsed_; }  // s, moving time
    double work() const { return work_; }        // J at the pedals
    double calories() const;                     // kcal burned
    int averagePower() const;
    int averageCadence() const;
    int averageHeartRate() const;                // 0 without one
    int maxPower() const { return maxPower_; }
    bool paused() const { return paused_; }
    bool finished() const { return finished_; }

private:
    double SimPower() const;

    Course course_;
    Mode mode_;
    double mass_;
    int ergW_, resistancePct_;
    int gear_;
    int power_ = 0;
    double cadence_ = 0.0;
    int heartRate_ = -1;
    double speed_ = 0.0, distance_ = 0.0, climbed_ = 0.0, elapsed_ = 0.0;
    double work_ = 0.0, cadenceSum_ = 0.0, heartSum_ = 0.0, heartTime_ = 0.0;
    int maxPower_ = 0;
    double simTarget_ = 0.0;
    bool paused_ = false, finished_ = false;
};

// Moving time in seconds to ride the course at a steady power, or 0 if that power stalls on it.
double EstimateTime(const Course& course, double massKg, int watts);
