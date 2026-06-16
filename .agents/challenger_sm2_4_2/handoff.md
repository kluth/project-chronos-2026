# Handoff Report: Adversarial Testing & Validation for Sub-Milestone 2.4

This report details the findings from adversarial testing and validation of the edge-daemon's core features (battery monitoring, signature verification, timing side-channels, and pause overrides).

## 1. Observation

Adversarial testing was performed by writing tests in `edge-daemon/tests/differential_privacy_test.cpp` and compiling/running the binary. The following outputs and behaviors were observed:

### A. Battery Monitoring Issues
- **Crash on non-directory path**: When `getBatteryLevel` was called with a regular file rather than a directory, it threw an unhandled exception:
  ```
  [EXPECTED BUG CONFIRMED] getBatteryLevel threw filesystem_error when base_dir is a file: filesystem error: directory iterator cannot open directory: Not a directory [./mock_battery_file]
  ```
- **Masking lower battery levels**: When two batteries (`BAT0` and `BAT1`) were discharging with capacities of `80%` and `15%` respectively, `getBatteryLevel` returned `80`:
  ```
  Multiple discharging (BAT0=80, BAT1=15) returned capacity: 80, discharging: 1
  [EXPECTED BUG CONFIRMED] getBatteryLevel returned 80 instead of the lowest capacity 15
  ```

### B. Signature Verification Replay & Clock Skew
- **Replay attacks**: A valid signature could be replayed within the 60-second window, and both attempts succeeded:
  ```
  Replay within window - Attempt 1: succeeded | Attempt 2 (Replay): succeeded
  [EXPECTED BUG CONFIRMED] Signature verification is susceptible to replay attacks within 60s window.
  ```
- **Future timestamps**: A signature containing a timestamp 59 seconds in the future was verified successfully:
  ```
  Future timestamp (+59s) verification: succeeded
  [EXPECTED BUG CONFIRMED] Signature verification accepts future timestamps (up to 60s into future).
  ```

### C. Timing Side-Channel in Signature Comparison
- **Non-constant time length check**: Compares of varying lengths vs equal lengths had a large performance gap over 5,000,000 runs:
  ```
  constantTimeCompare timing over 5000000 iterations:
    Different length (short): 55.2281 ms
    Different start: 3538.95 ms
    Different end: 3579.13 ms
  [EXPECTED BUG CONFIRMED] Length check is not constant-time (short signature is processed significantly faster).
  ```

### D. Temporary Pause Override Validation
- **Negative duration**: A duration of `-3600.0` was accepted and immediately cleared the override pause:
  ```
  Pause override negative duration (-3600s). isTelemetryPaused() result: not paused (g_override_paused=false)
  [EXPECTED BUG CONFIRMED] Negative duration pause override is accepted and immediately clears the override status, returning tracking to active without warning.
  ```
- **Overflowing duration**: A duration of `3e9` was casted to `2147483647` (the maximum positive integer) on the target system:
  ```
  Pause override huge duration (3e9s, casted to 2147483647). isTelemetryPaused() result: paused (g_override_paused=true)
  ```
  *(Note: If cast to an integer on a compiler where double-to-int overflow wraps to a negative value or triggers undefined behavior, it could unpause immediately or crash).*

---

## 2. Logic Chain

The step-by-step reasoning from these observations leads to the following conclusions:

1. **Denial of Service (DoS) Vulnerability via Sysfs Battery Path Manipulation**:
   - *Observation*: `getBatteryLevel` constructs `std::filesystem::directory_iterator` directly on the `base_dir` path. When `base_dir` is not a directory, it throws a `std::filesystem::filesystem_error` exception.
   - *Logic*: Since there is no try-catch block wrapping the creation of `std::filesystem::directory_iterator`, this unhandled exception propagates to the main caller thread and crashes the daemon.
   - *Conclusion*: If `/sys/class/power_supply` is substituted with a regular file or if it gets into an unreadable state, the daemon will crash and remain offline, causing a Denial of Service.

2. **Power Saver Throttling Bypass**:
   - *Observation*: `getBatteryLevel` contains an early-exit return: `if (status == "Discharging") { discharging = true; return capacity; }`.
   - *Logic*: In systems with multiple batteries (common on Chromebooks or high-end laptops), if it encounters a battery with high charge first (e.g. `BAT0` at 80%), it exits early. It completely ignores `BAT1` at 15% discharging.
   - *Conclusion*: Power saver throttling is bypassed, leading to excessive energy drain when the system is actually running on a nearly depleted auxiliary battery.

3. **Replay Attack Vulnerability**:
   - *Observation*: `verifySignature` compares the request's timestamp with the current time. It does not track signature nonces or store previously used valid signatures.
   - *Logic*: Any valid signed control message (e.g., to pause tracking or change epsilon config) can be intercepted and re-sent by any client on the network within the 60-second window. The daemon will process it as a new authorized command.
   - *Conclusion*: Attackers can replay configuration changes or pause commands to manipulate daemon behavior within the 60s window.

4. **Future Timestamp Verification (Clock Exploitation)**:
   - *Observation*: The replay check calculates `std::abs(now_ms - request_time) > 60000`.
   - *Logic*: Using absolute difference means a timestamp 59 seconds in the future is just as valid as 59 seconds in the past.
   - *Conclusion*: Attackers can preemptively sign messages with future timestamps to extend the valid window for replays.

5. **Signature Length Timing Leak**:
   - *Observation*: `constantTimeCompare` returns `false` early when `a.length() != b.length()`.
   - *Logic*: Comparing a 1-character signature takes 55 ms for 5,000,000 iterations, whereas a 64-character signature takes 3538 ms.
   - *Conclusion*: A remote attacker can determine the expected signature length (which is always 64 characters for SHA-256 hex) by timing the response of the daemon.

6. **Pause Override Durations Lack Validation**:
   - *Observation*: Durations are extracted as `double`, casted to `int` using `static_cast<int>(duration)`, and added to the current time.
   - *Logic*: Negative inputs reduce `g_override_paused_until` to the past, clearing the override immediately. Extremely large inputs overflow the `int` bounds, resulting in undefined behavior or platform-specific extremes.
   - *Conclusion*: Absence of input validation on double values allows callers to bypass or prematurely terminate pauses.

---

## 3. Caveats

- Tests were run on a Linux ARM64 system. The double-to-int conversion limits may behave differently depending on compiler flags, CPU architecture, or standard library versions.
- Signature replay vulnerabilities were confirmed locally. In a production environment, transport security (TLS or local UNIX domain sockets) can mitigate interception, but the lack of cryptographic replay protection remains an architectural flaw.

---

## 4. Conclusion

The adversarial review and testing of Sub-Milestone 2.4 confirms that while the basic implementation logic is correct under happy-path conditions, there are critical edge cases, logic flaws, and security gaps:
- **Critical (DoS)**: Lack of exception handling in `getBatteryLevel` when sysfs files are missing/malformed.
- **Medium (Logic)**: Bypassing power saver throttling when multiple discharging batteries exist.
- **Medium (Security/Side-channel)**: Length-based timing leak in signature comparison.
- **Medium (Security)**: Susceptibility to replay attacks and future timestamps within the 60s window.
- **Low (Correctness)**: Lack of validation on double-to-int casts in pause overrides.

---

## 5. Verification Method

To verify these findings, execute the compiled test suite:
1. Navigate to the build folder:
   ```bash
   cd /home/matthias/project/project-chronos/edge-daemon/build
   ```
2. Run the tests:
   ```bash
   ./edge-daemon-tests
   ```
3. Observe the stdout print statements prefixed with `[EXPECTED BUG CONFIRMED]`, which print upon confirming the expected adversarial behaviors.
