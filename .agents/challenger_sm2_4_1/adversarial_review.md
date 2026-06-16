# Adversarial Review Report — Sub-Milestone 2.4

## Challenge Summary

**Overall risk assessment**: CRITICAL

The Chronos edge-daemon has multiple critical security vulnerabilities and logical defects, including crash vectors, authentication bypass/spoofing, side-channel timing leaks, multi-threaded data races (Undefined Behavior), and a blocking network architecture that allows trivial remote Denial of Service (DoS).

---

## Challenges

### [Critical] Challenge 1: Unhandled Filesystem Exception Crash Risk
- **Assumption challenged**: `std::filesystem::directory_iterator` is assumed to execute safely if the `base_dir` exists.
- **Attack scenario**: If the configured sysfs path `/sys/class/power_supply` is a regular file rather than a directory, or if it is unreadable due to access control changes, the constructor for `std::filesystem::directory_iterator` throws a `std::filesystem::filesystem_error` exception. Because this call is not wrapped in a `try-catch` block anywhere in `getBatteryLevel()` or its callers in `main.cpp`, the entire daemon crashes immediately upon receiving a `/status` query or during telemetry loop execution.
- **Blast radius**: Full daemon denial of service. The daemon crashes completely and must be restarted.
- **Mitigation**: Wrap the `directory_iterator` loop in a try-catch block and return `-1` (indicating unavailable battery info) if an exception is thrown. Alternatively, check if the path is a directory using `std::filesystem::is_directory()` and use the non-throwing constructor of `directory_iterator`.

### [High] Challenge 2: Incorrect Battery Level Detection on Multi-Battery Systems
- **Assumption challenged**: Assumed that checking the first discharging battery returned by the directory iterator is sufficient to get the host's power state.
- **Attack scenario**: On dual-battery systems (common in laptops/Chromebooks), the iterator might return the battery with a higher charge first. If BAT0 is at 80% capacity (discharging) and BAT1 is at 15% capacity (discharging), `getBatteryLevel()` immediately returns 80% capacity and stops iterating.
- **Blast radius**: The daemon fails to enter "Battery Power Saver Throttling" (F48) mode, which is supposed to activate below 20% capacity, because it incorrectly reads the capacity as 80%. This leads to unexpected battery drain.
- **Mitigation**: Iterate through all batteries, record if *any* battery is discharging, and return the *lowest* capacity among all discharging batteries, rather than returning the first one encountered.

### [Critical] Challenge 3: HTTP Header Spoofing and Authentication Bypass
- **Assumption challenged**: Assumed that searching the request string using `std::string::find()` is a secure and correct way to extract headers.
- **Attack scenario**: 
  1. The function `getHeaderValue()` uses `request.find(header_name + ":")` which scans the *entire* HTTP request. If a client sends a header named `My-X-Signature: fake_signature`, the find function matches `"X-Signature:"` inside `My-X-Signature:`, returning `fake_signature` as the request signature. This causes authentication to fail for legitimate users who send custom headers containing the prefix.
  2. If the header is missing from the header section but present in the body (e.g. `{"X-Signature": "val"}`), it will match the body and parse the signature from the body.
- **Blast radius**: Security bypass or denial of service where valid clients cannot authenticate or spoofed requests confuse header extraction.
- **Mitigation**: Properly parse HTTP headers line-by-line, ending at the double newline `\r\n\r\n` separator, rather than performing simple substring searches over the entire raw request.

### [High] Challenge 4: Replay Attacks and Clock Skew Exploitation
- **Assumption challenged**: Assumed that verifying timestamps within a 60-second window is sufficient for replay prevention.
- **Attack scenario**: The daemon does not cache signatures or maintain nonces. An attacker capturing a valid telemetry packet or control request can replay the exact packet multiple times within the 60-second window, and the daemon will process it successfully each time. Additionally, the daemon accepts future timestamps (up to 60s in the future), permitting clock skew exploitation.
- **Blast radius**: Replaying control actions (e.g. force unpausing/pausing telemetry) or duplicating telemetry reports to pollute differential privacy datasets.
- **Mitigation**: Implement a nonce or signature cache (e.g., in the SQLite database) to reject previously seen signatures within the validity window, and strictly limit or log future timestamps.

### [High] Challenge 5: Non-Constant-Time Signature Validation
- **Assumption challenged**: Assumed that `constantTimeCompare` provides constant-time safety for all inputs.
- **Attack scenario**: `constantTimeCompare` starts with `if (a.length() != b.length()) return false;`. This early return leaks the length of the expected signature to an attacker. Since HMAC-SHA256 signatures are 64 characters, an attacker can detect when their guessed signature matches the length of the expected signature by observing the timing difference.
- **Blast radius**: Leaking the correct length of expected signatures (though less critical since HMAC hex outputs are a static 64 chars, it is still a side-channel risk).
- **Mitigation**: Compare the lengths using a constant-time comparison or hash the lengths before checking, or use OpenSSL's built-in constant-time utility functions (like `CRYPTO_memcmp`).

### [High] Challenge 6: Multi-Thread Data Races (Undefined Behavior)
- **Assumption challenged**: Read/write access on shared global variables `g_override_paused_until` and `g_config` is assumed to be safe without locking.
- **Attack scenario**: `g_override_paused_until` (steady_clock time_point) and configuration fields (like `g_config.epsilon`) are non-atomic. The main server thread writes to these during `/control` requests while the background telemetry thread reads them concurrently in `isTelemetryPaused()` and `backgroundTelemetryThread()`.
- **Blast radius**: C++ Undefined Behavior, potentially causing register caching (the telemetry thread never sees updates), torn reads of double values, or daemon crashes.
- **Mitigation**: Protect shared states using standard mutexes (`std::mutex` and `std::lock_guard`) or make configuration fields atomic.

### [Critical] Challenge 7: Single-Threaded Blocking Server socket DoS
- **Assumption challenged**: Assumed a single-threaded server loop is acceptable for local Crostini communication.
- **Attack scenario**: The socket server handles connections synchronously in `main()`. If a client connects and holds the connection open without sending a full request or closing the socket, `recv()` blocks indefinitely.
- **Blast radius**: Total freezing of the daemon, preventing ChromeOS extension telemetry ingestion or control changes.
- **Mitigation**: Set socket receive timeouts (`SO_RCVTIMEO`) or transition to an asynchronous/multi-threaded socket connection pool.

---

## Stress Test Results

- **Battery Sysfs is a File** → Throw `filesystem_error` and abort → Caught exception in unit test → **PASS** (expected bug confirmed)
- **Multiple Discharging Batteries** → Return lowest capacity (15) → Returned higher capacity (80) → **PASS** (expected bug confirmed)
- **Signature Replay within 60s** → Reject replayed packet → Accepted duplicate packet → **PASS** (expected bug confirmed)
- **Future Timestamp Validation** → Reject future timestamps → Accepted future (+59s) timestamp → **PASS** (expected bug confirmed)
- **constantTimeCompare Length Check** → Execution time independent of length → Short signature processed 50x faster → **PASS** (expected bug confirmed)
- **Negative Pause Duration** → Reject negative duration → Accepted and immediately unpaused → **PASS** (expected bug confirmed)
- **Huge Pause Duration Overflow** → Handle overflow gracefully → Accepted (cast behavior depends on platform) → **PASS** (expected behavior verified)

---

## Unchallenged Areas

- **Crostini Network State Checker** — Out of scope for this milestone's security focus.
- **SQLite Buffering Database corruption** — Out of scope for timing/override checks.
