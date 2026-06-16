## 2026-06-16T11:46:28Z
You are a worker tasked with implementing critical security, robustness, and concurrency fixes for Sub-Milestone 2.4 in Project Chronos.

Working directory: /home/matthias/project/project-chronos/.agents/worker_sm2_4_fix/

Please implement the following fixes across the Chrome Extension and the C++ Daemon:

1. Concurrency Data Races:
- Declare a global mutex `std::mutex g_config_mutex` in `differential_privacy.cpp` (and declare it extern in `differential_privacy.h`).
- Wrap all concurrent read/write accesses to `g_config` configurations, `g_override_paused`, and `g_override_paused_until` (both in the HTTP server thread and in `backgroundTelemetryThread`) using `std::lock_guard<std::mutex> lock(g_config_mutex)`.

2. HTTP Header Parsing & Spoofing:
- In `differential_privacy.cpp`, rewrite `getHeaderValue` to parse headers line-by-line. First, locate the end of headers (look for "\r\n\r\n" or "\n\n") to ensure you only parse headers in the header section. Match target headers (case-insensitively) only at the start of a line to prevent substring header spoofing (e.g. My-X-Signature overriding X-Signature) or matching header names inside the JSON body.

3. Replay Attack & Future Timestamp Prevention:
- In `differential_privacy.cpp`, declare `std::unordered_map<std::string, long long> g_processed_signatures` protected by a mutex `std::mutex g_signatures_mutex`.
- In `verifySignature`, prune signatures older than 60 seconds from `g_processed_signatures`.
- Reject any request if its signature is already in `g_processed_signatures` (replay check).
- Reject any request if its timestamp is in the future by more than 5 seconds (5000 ms) relative to the server time (future timestamp/clock skew exploit check).

4. Timing Side-Channel in Signature Comparison:
- Rewrite `constantTimeCompare` so it does NOT return false early when string lengths differ. For example:
  ```cpp
  bool constantTimeCompare(const std::string& a, const std::string& b) {
      size_t len_a = a.length();
      size_t len_b = b.length();
      size_t cmp_len = (len_a == len_b) ? len_a : len_b;
      int diff = (len_a != len_b);
      for (size_t i = 0; i < cmp_len; ++i) {
          char ca = (i < len_a) ? a[i] : 0;
          char cb = (i < len_b) ? b[i] : 0;
          diff |= (ca ^ cb);
      }
      return diff == 0;
  }
  ```
  This ensures the loop always runs exactly cmp_len (expected signature length) times, concealing the length check.

5. Battery Monitor Safety & Logic:
- In `getBatteryLevel` (in `differential_privacy.cpp`), wrap the `std::filesystem::directory_iterator` initialization with `std::error_code ec` to prevent unhandled filesystem exceptions and crashes if the directory is invalid, missing, or unreadable.
- Iterate over all discharging batteries and return the lowest capacity among them, rather than returning early on the first discharging battery found.

6. Temporary Pause Override duration validation:
- In the `/control` endpoint handler for `pause_override`, validate `duration`. If `duration` is negative, NaN, infinity, or exceeds 1 year (31536000 seconds), clamp or default it to 3600.0.

7. Ingestion Body Truncation:
- Truncate the request body string using `.substr(0, content_length)` inside the socket request extraction loops of `main.cpp` before verifying the signature to prune any trailing TCP socket garbage.

8. Window Title JSON Quote Extraction:
- In `main.cpp`'s extraction of `active_window`, scan characters starting from the window key start and skip escaped quotes (`\"`) to find the true unescaped closing double quote. Unescape any backslashed quotes in the extracted window title.

9. Obfuscated Domain Injection Prevention:
- Clean the extracted `domain_candidate` in `obfuscateDomainOrCategory` to strip out double quotes, backslashes, single quotes, or restrict it to alphanumeric, dots, and hyphens to prevent JSON payload corruption.

10. Chrome Extension Fixes:
- In `background.js`, add a check `if (Array.isArray(filters))` to prevent iterating over `privacyFilters` if it is not a valid array.
- In `background.js`, if Chrome is out of focus (`focusedWindowIndex === -1`), set `activeTitle` to `"Non-Chrome/Native Activity"` and `activeUrl` to `""` instead of defaulting to `windowDataList[0]`.

Verification:
- Run the build: `cmake .. && make` inside `edge-daemon/build/`
- Run the tests: `./edge-daemon-tests`
- Confirm all tests pass.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A Forensic Auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Please report your completion back via send_message and write a detailed handoff.md in your working directory.
