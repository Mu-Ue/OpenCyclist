#pragma once

#include <cstdint>
#include <vector>

struct CourseParams {
    double km = 30.0;
    double gainM = 500.0;
    double maxGradePct = 10.0;
    bool loop = false;    // ends where it starts
    uint32_t seed = 0;
};

struct Course {
    CourseParams params;
    double targetGain = 0.0;     // the requested gain, lowered to what the max grade allows
    std::vector<float> elevation;  // m, every kCourseStep m

    double Length() const;
    double ElevationAt(double distance) const;
    double GradeAt(double distance) const;  // fraction
    double Gain() const;
};

// Builds a road-like profile. The same params always give the same course.
Course GenerateCourse(const CourseParams& params);

// A fresh non-zero seed.
uint32_t RandomSeed();
