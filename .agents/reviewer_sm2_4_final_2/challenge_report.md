# Adversarial Review Report — Sub-Milestone 2.4 Final Review

## Challenge Summary

**Overall risk assessment**: LOW

The overall risk assessment is low. The implemented safety, security, and concurrency controls are robust. The critical vulnerabilities (e.g. TOCTOU in signature replay, session lock collisions, battery monitor crashes) have been successfully mitigated.

---

## Challenges

### Low Challenge 1: Non-Constant Time Comparison on Signature Length Mismatch
- **Assumption challenged**: `constantTimeCompare` performs a strictly constant-time string comparison for any input.
- **Attack scenario**: If an attacker submits a signature of an invalid length (e.g. 1 character instead of 64), `cmp_len` is set to 1. The comparison loop finishes in 1 iteration instead of 64.
- **Blast radius**: Minimal. The length of a valid HMAC-SHA256 signature is always 64 characters (hexadecimal), which is public knowledge. The timing difference only leaks that the length is incorrect, not the signature's content. However, the comparison is not strictly constant-time across all possible input lengths.
- **Mitigation**: Calculate the comparison length based on the expected signature length (64 bytes) rather than the input signature length, or pad the input signature.

### Low Challenge 2: Signed Integer Overflow via Negative Timestamp Injections
- **Assumption challenged**: Inputs in headers are well-formed and positive.
- **Attack scenario**: An attacker sends `X-Timestamp: -9223372036854775808`. `std::stoll` parses it to `LLONG_MIN`. Evaluating `now_ms - request_time` triggers a signed integer overflow (since `now_ms` is positive, subtracting `LLONG_MIN` is equivalent to adding `LLONG_MAX + 1`, causing an overflow).
- **Blast radius**: Undefined behavior in C++ (compiler optimization dependent). This could cause the replay attack window check to be bypassed or crash the verification thread.
- **Mitigation**: Add a sanity check `if (request_time < 0) return false;` immediately after parsing.

---

## Stress Test Results

- **Negative Duration Pause Override** → API controller sanitizes negative/infinite durations to `3600.0` → **PASS** (Sanitized successfully)
- **Huge Duration Pause Override** → API controller sanitizes values larger than 1 year (31,536,000s) to `3600.0`, preventing integer overflows → **PASS** (Sanitized successfully)
- **Signature Replay Attack** → Verified that submitting the same valid signature twice within the active time window is blocked on the second attempt → **PASS** (Replay successfully blocked)
- **Future Timestamp Injection** → Verified that submitting a signature with a timestamp in the future (+59 seconds) is rejected → **PASS** (Rejected successfully)
- **Peripheral Battery Iteration Crash** → Checked that invalid/file power supply inputs do not crash the iterator due to the try-catch block → **PASS** (No crash occurs)

---

## Unchallenged Areas

- **Crostini Network State Routing Table Modifications** — Live routing table changes (e.g. adding new virtual interfaces mid-routing) were not simulated under load because they depend on system permissions that are unavailable in this environment.
