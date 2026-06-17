#pragma once

// wrapper around float versions of functions
// in gta they are in CMaths but that makes the code rather noisy

inline float Sin(const float x) { return sinf(x); }
inline float Asin(const float x) { return asinf(x); }
inline float Cos(const float x) { return cosf(x); }
inline float Acos(const float x) { return acosf(x); }
inline float Tan(const float x) { return tanf(x); }
inline float Atan(const float x) { return atanf(x); }
inline float Atan2(const float y, const float x) { return atan2f(y, x); }
inline float Abs(const float x) { return fabs(x); }
inline float Sqrt(const float x) { return sqrtf(x); }
inline float RecipSqrt(const float x, const float y) { return x/Sqrt(y); }
inline float RecipSqrt(const float x) { return RecipSqrt(1.0f, x); }
inline float Pow(const float x, const float y) { return powf(x, y); }
inline float Floor(const float x) { return floorf(x); }
inline float Ceil(const float x) { return ceilf(x); }
