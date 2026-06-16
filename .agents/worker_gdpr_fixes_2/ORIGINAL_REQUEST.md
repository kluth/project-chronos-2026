## 2026-06-16T15:25:55Z
You are the Worker for GDPR metrics robustness fixes (Milestone 2).
Your working directory is `/home/matthias/project/project-chronos/.agents/worker_gdpr_fixes_2/`.

Please implement the following fixes in the `edge-daemon` C++ codebase:
1. **Laplace Noise Boundary Fix**:
   - In `generateLaplaceNoise` (in `src/differential_privacy.cpp`), ensure that the uniform variable `u` cannot be exactly `-0.5` or `0.5` to avoid `log(0.0)` returning `-inf`. Implement a redraw loop or clamp `u`:
     ```cpp
     double u;
     do {
         u = distribution(generator);
     } while (u == -0.5 || u == 0.5);
     ```
2. **Division by Zero / Underflow Prevention**:
   - In `generateLaplaceNoise`, add validation: if `epsilon <= 1e-15` or `!std::isfinite(epsilon)` or `sensitivity <= 0.0` or `!std::isfinite(sensitivity)`, fallback to a safe default noise (e.g., scale based on `epsilon = 0.0001` or return `0.0` noise and log a warning).
   - In parameter parsing / control APIs (CLI parser in `main.cpp`, `/control` endpoint), validate that inputs are strictly positive finite numbers. If validation fails, do not apply the invalid parameters and return/log a warning.
3. **Metric Output Clamping**:
   - In `anonymizeEvent`, clamp the anonymized value to `0.0` if the metric name is `"keystrokes_per_minute"`, `"mouse_pixels_per_minute"`, or any other activity duration metric, to prevent negative counts or times.
4. **NaN/Inf SQLite Database Buffering**:
   - Before inserting any event into the SQLite database, verify that `safe_event.value` is finite. If it is `NaN` or `inf`/`-inf`, clamp it to `0.0` or a safe default and log a warning.
   - Check the return value of `bufferEvent` and log warnings on failure.
5. **Invalid JSON Backup Serialization**:
   - In `dumpBackupToJson` (in `src/differential_privacy.cpp`), check if the retrieved value is finite. If not, serialize it as `0.0` to ensure valid JSON output.

Build the daemon and tests. Run the unit test suite (`./build/edge-daemon-tests`) to verify. Expand the tests in `tests/differential_privacy_test.cpp` to verify these boundary conditions and protections (like testing with epsilon=0, epsilon=1e-300, u boundary checks, clamping of negative metrics, and JSON backup safety).
Commit the changes incrementally via Git.
Write a detailed handoff.md in your working directory and message the parent.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A Forensic Auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.
