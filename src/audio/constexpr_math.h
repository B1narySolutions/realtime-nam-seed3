#pragma once

// Taylor series, so filter designs run at compile time and the firmware
// links none of libm's sin, cos, exp or pow (about 13 KB of its 128 KB
// flash). Twelve terms are exact to double precision only for small
// arguments, so each caller notes the range it uses.
namespace constexpr_math {

constexpr double Sin(double x) {
    double term = x, sum = x;
    for (int n = 1; n < 12; ++n) {
        term *= -x * x / ((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

constexpr double Cos(double x) {
    double term = 1.0, sum = 1.0;
    for (int n = 1; n < 12; ++n) {
        term *= -x * x / ((2 * n - 1) * (2 * n));
        sum += term;
    }
    return sum;
}

constexpr double Exp(double x) {
    double term = 1.0, sum = 1.0;
    for (int n = 1; n < 12; ++n) {
        term *= x / n;
        sum += term;
    }
    return sum;
}

} // namespace constexpr_math
