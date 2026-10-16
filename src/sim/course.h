#pragma once

#include <cstdint>
#include <vector>

struct CourseParams {
    double km = 30.0;
    double gainM = 500.0;
    double maxGradePct = 10.0;
    bool loop = false;    // ends where it starts
    uint32_t seed = 0;
    bool endless = false;  // grows as it's ridden, ignoring km and gainM
    int terrain = 1;       // for endless: 0 flat, 1 rolling, 2 hilly, 3 mountains
};

constexpr int kTerrainCount = 4;

struct Course {
    CourseParams params;
    double targetGain = 0.0;     // the requested gain, lowered to what the max grade allows
    std::vector<float> elevation;  // m, every kCourseStep m

    double Length() const;
    double ElevationAt(double distance) const;
    double GradeAt(double distance) const;  // fraction
    double Gain() const;

    // Adds the next stretch of an endless course, joined on smoothly.
    void Extend();
};

// Builds a road-like profile. The same params always give the same course.
Course GenerateCourse(const CourseParams& params);

// A generic look at a terrain, for showing what an endless course will be like.
Course TerrainPreview(int terrain);

// The most a course can climb, from its length and max grade.
double MaxGain(const CourseParams& params);

// A fresh non-zero seed.
uint32_t RandomSeed();
