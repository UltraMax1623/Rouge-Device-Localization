#include "../shared/calibration_math.h"
#include "../shared/serial_commands.h"
#include <cassert>
#include <cstdio>

int main() {
  std::array<double, 5> readings{};
  for (unsigned i = 0; i < readings.size(); ++i)
    readings[i] = -47.5 - 10 * 2.7 * std::log10(double(i + 1));
  const auto fit = Calibration::fit(readings);
  assert(fit.valid());
  assert(std::fabs(fit.a + 47.5) < 1e-10);
  assert(std::fabs(fit.n - 2.7) < 1e-10);
  assert(fit.rmse < 1e-10);
  assert(Calibration::checkTraining(fit, readings, readings).passed);
  auto validation = Calibration::checkValidation(fit, -60, 2,
      fit.a - 10 * fit.n * std::log10(2.5), fit.a - 10 * fit.n * std::log10(2.5));
  assert(validation.passed() && validation.mae < 1e-10);
  validation = Calibration::checkValidation(fit, fit.a, fit.n,
      fit.a - 10 * fit.n * std::log10(2.5), fit.a - 10 * fit.n * std::log10(2.5));
  assert(validation.passed()); // Equivalent models need no artificial improvement.
  const std::array<double, 5> observedForward{{-59.69, -62.97, -68.91, -73.61, -83.42}};
  const std::array<double, 5> observedReverse{{-59.41, -63.30, -71.12, -71.76, -82.50}};
  std::array<double, 5> shared{};
  for (unsigned i = 0; i < 5; ++i) shared[i] = (observedForward[i] + observedReverse[i]) / 2;
  const auto observedFit = Calibration::fit(shared);
  assert(observedFit.valid()); // The old RMSE gate alone accepted the field run.
  const auto observedCheck = Calibration::checkTraining(observedFit, observedForward, observedReverse);
  assert(!observedCheck.passed && observedCheck.recheck[4]);
  assert(observedCheck.forward[4] > 7 && observedCheck.reverse[4] > 6.5);
  for (unsigned i = 0; i < 4; ++i) assert(!observedCheck.recheck[i]);
  validation = Calibration::checkValidation(observedFit, -60, 2, -68.40, -67.55);
  assert(validation.withinTolerance && !validation.noRegression && !validation.passed());
  assert(validation.mae > validation.previousMae + 0.02);
  validation = Calibration::checkValidation(fit, -60, 2, NAN, -60);
  assert(!validation.passed());
  readings.fill(-50);
  assert(!Calibration::fit(readings).valid());  // No distance response.
  readings[3] = NAN;
  assert(!Calibration::fit(readings).valid());
  readings = {-45, -55, -30, -75, -35};
  assert(!Calibration::fit(readings).valid());  // Nonphysical / poor fit.
  Calibration::Histogram histogram;
  assert(std::isnan(histogram.trimmedMean()));
  for (unsigned i = 0; i < 80; ++i) histogram.add(-50);
  for (unsigned i = 0; i < 10; ++i) { histogram.add(-20); histogram.add(-100); }
  assert(histogram.count == 100);
  assert(histogram.trimmedMean() == -50);  // Both tails discarded.
  assert(histogram.spread() > 4);  // Reject a noisy window despite a good median.
  histogram = {};
  for (unsigned i = 0; i < 60; ++i) histogram.add(i % 2 ? -49 : -51);
  histogram.add(-128); histogram.add(1);
  assert(histogram.count == 60 && histogram.trimmedMean() == -50);
  assert(std::fabs(histogram.spread() - 1) < 1e-10);
  assert(UserCommand::parse("an opt 02").kind == UserCommand::Kind::Calibrate);
  assert(UserCommand::parse("an diag 02").kind == UserCommand::Kind::Diagnostic);
  assert(UserCommand::parse("an diag 02").distance == 1);
  assert(UserCommand::parse("an diag 15 5").distance == 5);
  for (const char *bad : {"an diag", "an diag 1", "an diag 16", "an diag 02 0",
                          "an diag 02 6", "an diag 02 2 extra", "an opt 02 2"})
    assert(UserCommand::parse(bad).kind == UserCommand::Kind::Invalid);
  const auto summary = Calibration::summarizeWindows(
      std::array<double, 5>{{-80, -81, -79, NAN, -60}},
      std::array<bool, 5>{{true, true, true, true, false}});
  assert(summary.count == 3 && summary.mean == -80 && summary.minimum == -81 && summary.maximum == -79);
  assert(std::fabs(summary.spread - std::sqrt(2.0 / 3.0)) < 1e-10);
  assert(UserCommand::parse("an opt 15").board == 15);
  assert(UserCommand::parse(" an opt\t3 ").board == 3);
  for (const char *bad : {"an opt", "an opt 01", "an opt 16", "an opt 2 extra",
                          "an opt 0x02", "an opt 002", "an opt -2", "an opt "})
    assert(UserCommand::parse(bad).kind == UserCommand::Kind::Invalid);
  std::puts("Calibration math and commands passed");
}
