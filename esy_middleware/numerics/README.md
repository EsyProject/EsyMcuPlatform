<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# esy_middleware/numerics

## Purpose

Small dense linear algebra, least-squares/ridge regression, polynomial fitting,
explicit leave-one-out cross validation (LOOCV), and optional Q15 quantization.
The implementation uses caller-owned storage and bounded stack workspaces.
It has no hardware, board, RTOS or third-party inference-library dependency.

## Public Surface

Headers under `include/esy_middleware/numerics/`:

- `esy_numerics_config.h`: shared dimension/sample limits and model ABI configuration.
- `esy_linalg.h`: Cholesky solve and Gauss-Jordan matrix inverse.
- `esy_fit.h`: synchronous feature callbacks, training and model-size selection.
- `esy_poly_fit.h`: scaled polynomial training, raw coefficient expansion, degree
  selection and unchecked inline prediction helpers.
- `esy_numerics_quantize.h`: normalization, rounding and signed 16-bit saturation.

## Integration

Enable `ENABLE_ESY_NUMERICS` in the consumer's `esy_feature_import.cmake`, then link
`esy_numerics`. Enable `ENABLE_ESY_NUMERICS_QUANTIZE` for the quantization
implementation. Disabled numerics creates no target; disabled quantization adds
no quantization source.
Public common types and required mathematical link dependencies are propagated.
The private optimization target does not expose compile options to consumers.

Configure limits as CMake variables before adding EsyMcuPlatform:

```cmake
set(ENABLE_ESY_NUMERICS ON)
set(ENABLE_ESY_NUMERICS_QUANTIZE ON)
set(ESY_LINALG_MAX_M 8)
set(ESY_FIT_MAX_M 8)
set(ESY_FIT_MAX_N 32)
set(ESY_FIT_MAX_DEG 6)
```

These are the defaults. The target exports its generated build configuration to
all consumers. Dimensions must satisfy `1 <= FIT_MAX_M <= LINALG_MAX_M <= 46340`,
`1 <= FIT_MAX_N <= INT32_MAX`, and `0 <= FIT_MAX_DEG < FIT_MAX_M`. These arithmetic
bounds do not certify stack feasibility: keep dimensions small and budget stack
and execution time for the actual target. No local `#define` may disagree with
the build configuration. Direct source builds use the defaults in
`esy_numerics_config.h`; for nondefault direct builds, supply the same generated
`esy_numerics_build_config.h` and `ESY_NUMERICS_CONFIGURED_BUILD=1` to every
translation unit.
Do not modify generated configuration or redefine macros after including headers.

Changing limits changes the model ABI: rebuild the library and every consumer,
and do not exchange stored model bytes across configurations. Per-file overrides
are rejected. Selection APIs reject invalid bounds; numerical failures return
errors rather than publishing NaN/Inf. LOOCV uses explicit refits.

### Usage Example

Fit three calibration points with a first-degree polynomial, then evaluate an
interior input. The default limits support these dimensions; custom limits must
allow at least three samples, two features and degree one. Link `esy_numerics`.

```c
#include "esy_middleware/numerics/esy_poly_fit.h"

esy_err_t estimate_calibration(float *prediction) {
    if (prediction == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    const float x[] = {0.0f, 1.0f, 2.0f};
    const float y[] = {1.0f, 3.0f, 5.0f};
    float coefficients[ESY_FIT_MAX_DEG + 1];
    esy_err_t result = esy_poly_fit(
        x, y, (int)(sizeof(x) / sizeof(x[0])), 1, 0.0f,
        coefficients, NULL, NULL);
    if (result != ESY_OK) {
        return result;
    }
    *prediction = esy_poly_predict_raw(coefficients, 1, 1.5f);
    return ESY_OK;
}
```

The result is approximately `4.0f`. Fitting is synchronous, keeps no pointers
and needs no initialization or cleanup. Run it in normal owner context with
adequate stack and execution budgets; use prediction only after a successful fit.

## Contracts

All APIs are synchronous and task-context only. No input, output, callback or
context pointer is retained. Input arrays must remain unchanged until return;
input/output storage and mutable callback storage must not overlap. Independent
buffers and contexts may be used concurrently. Callers serialize access to shared
mutable buffers/models/contexts. Callbacks run in the calling task; any callback
blocking extends call duration. A callback writes exactly `m` finite values and
must be deterministic for a fixed `(x, m, user)`. Keep its context alive until
return. There are no locks, tasks, asynchronous resources, or init/deinit stages.

- Inputs and lambda must be finite. Invalid dimensions, NULL required pointers,
  negative/nonfinite lambda or nonfinite samples return `ESY_ERR_INVALID_ARG`.
  Nonfinite callback results, failed solves, unrepresentable scaling/coefficients
  or nonfinite numerical results return `ESY_ERR_FAIL` from fitting APIs.
- Training requires `n >= m`, including positive ridge. Polynomial training
  requires `n >= degree + 1`. Output models need no prior initialization and may
  be reused. Only active coefficients are valid after success. Failure preserves
  models and raw coefficient arrays; optional RMSE/CV outputs reset to zero.
- Cholesky requires a symmetric positive-definite matrix; symmetry is a caller
  precondition. Small absolute or relative pivots are rejected. Solve failure
  can leave `A` and `x` partially written; discard those results. Inverse failure
  preserves its output. Linalg APIs return zero on success and nonzero on failure.
- LOOCV solves each held-out dataset with the same feature map. OLS candidates
  require `n > m`; ridge candidates require `n >= m` for the full dataset and
  may solve a fold with fewer samples than parameters. Any failed/nonfinite fold
  invalidates the candidate. If none is valid, returns `ESY_ERR_FAIL`. Ties
  within `1e-6` retain the smaller dimension. Polynomial LOOCV fixes one transform
  from the complete X array for every fold; it does not evaluate a pipeline that
  relearns normalization (and hence the ridge penalty) within each fold.
- Quantization validates all elements before writing output. Nonfinite elements
  and zero std return `ESY_ERR_INVALID_ARG`, preserving the entire output. Negative
  std/scale are accepted. Double intermediates avoid float overflow for finite
  float inputs. Saturation precedes integer conversion; halfway rounding is away
  from zero. Zero length accepts NULL and does not access storage.
- Prediction helpers require valid pointers/degrees and immutable model data.
  They do not report numerical overflow for extrapolation beyond the fitted data.

## Resource Model

No dynamic allocation or persistent mutable static state. The caller owns all
arrays and models. Temporaries live on the calling task's stack with natural
C type alignment. The two model layouts depend on `ESY_FIT_MAX_M` and
`ESY_FIT_MAX_DEG`; input and coefficient buffers are sized by their API lengths.

Stack usage is O(M squared) for dense matrices, plus O(D) double coefficients
for polynomial expansion. It also depends on `ESY_LINALG_MAX_M` in nested solves
and on callback stack use. `ESY_FIT_MAX_N` bounds input validation and iteration
counts, not an N-sized stack array; lowering it does not shrink matrix workspaces.
Measure peak stack on the target with the selected compiler and callback.

For each candidate dimension M, explicit LOOCV costs
O(N * (N*M*M + M*M*M)), with O(M*M) workspace. Do not run fitting in ISRs;
account for latency in task and
watchdog budgets. Polynomial normalization/expansion and RMSE use double
intermediates, which may be implemented in software on MCUs.

## Verification

From the repository root:

```sh
ruby tools/quality/run_module_tests.rb esy_middleware/numerics/tests /tmp/esy-numerics-tests test:all
ruby tools/quality/run_module_tests.rb esy_middleware/numerics/tests /tmp/esy-numerics-gcov gcov:all
python3 -B esy_middleware/numerics/tests/check_coverage.py /tmp/esy-numerics-gcov/artifacts/gcov/gcovr/coverage.json
ruby tools/quality/run_module_tests.rb esy_middleware/numerics/tests /tmp/esy-numerics-sanitizer test:all --sanitizer
python3 -B esy_middleware/numerics/tests/test_cmake_consumer.py
python3 -B esy_middleware/numerics/tests/test_coverage_gate.py
```

The suite covers golden fits, singular and nonfinite systems, finite arithmetic
and coefficient overflow, output preservation/retry, explicit LOOCV reference
values, extreme saturation, halfway rounding and argument bounds. Consumer tests
compile/link/run C11 and C++17 programs using only `esy_numerics`, exercise feature
combinations, share nondefault limits, and reject ABI mismatch.
The quality gate includes source contracts, consumers, sanitizers and an 85%
line/branch minimum for every production source, rejecting omitted sources.
Its JSON inventory verifies canonical local source paths and rejects foreign
namesakes and duplicate aliases; accepted path roots are documented in the
[quality tools guide](../../tools/README.md).
Host tests do not certify target stack, latency or hardware execution.

## Known Limits

Small dense problems only. Normal equations amplify conditioning problems;
ridge and normalized features can help but cannot guarantee a successful solve.
Very small pivots and unrepresentable raw polynomial coefficients fail closed.
Explicit LOOCV can take substantially longer than training. Polynomial model
selection validates a fixed normalization map as described above.
Standard finite-aware floating-point semantics are required; do not compile with
fast-math/finite-math-only assumptions. Supported numeric targets use IEEE-style
32-bit float and wider double intermediates. Persist models with an explicit
configuration/version rather than treating the public structure as a wire format.
