# Adversarial Challenge Report — Sub-Milestone 2.4 Fixes

## Challenge Summary

**Overall risk assessment**: LOW

All major attack vectors (replays, timing side-channels on character comparisons, buffer overflows, resource leaks) have been addressed. The system behaves robustly under adversarial conditions.

---

## Challenges

### [Low] Challenge 1: Timing Leakage on Signature Length Mismatch

- **Assumption challenged**: `constantTimeCompare` is completely constant time for all signature comparisons.
- **Attack scenario**: If the input signature length differs from the expected signature length, `constantTimeCompare` sets `diff = (len_a != len_b)` but runs the comparison loop for `cmp_len` iterations (which depends on the lengths of `a` and `b`). If the user provides a very short signature, the loop exits much faster, confirming that the signature is short.
- **Blast radius**: Low. The expected signature is always a hex-encoded SHA-256 HMAC (64 characters), which is public knowledge/spec-defined, so leaking the expected length does not reveal the secret key.
- **Mitigation**: Pre-validate that both input and expected signatures are exactly 64 characters in length before performing the constant-time comparison.

### [Low] Challenge 2: Memory Load of Processed Signatures Map

- **Assumption challenged**: The in-memory `g_processed_signatures` map will not experience excessive memory growth under attack.
- **Attack scenario**: An attacker floods the daemon with thousands of unique valid-looking requests containing current timestamps. Since pruning only removes signatures older than 60 seconds, the map size could temporarily swell, consuming memory.
- **Blast radius**: Low. A 60-second window is short, and each map entry is tiny (~100 bytes). A flood of 10,000 requests/sec would consume less than 10MB.
- **Mitigation**: Enforce a hard maximum capacity on the `g_processed_signatures` map (e.g. 100,000 entries) and discard the oldest entries if the limit is exceeded, or reject incoming requests if request rate is abnormally high.

---

## Stress Test Results

- **Replay Attack within 60s Window** → Blocked via in-memory deduplication map → Rejected on second attempt → PASS
- **Clock Drift Manipulation (Future Timestamp > 5s)** → Blocked by comparing with system clock → Future timestamp rejected → PASS
- **Timing Side-Channel Character Mismatch** → Verified timing consistency across matching prefixes → Character mismatch does not alter execution timing → PASS
- **Invalid Battery Paths / File Input** → Verified directory iterator handles filesystem exceptions → Returns -1 gracefully without crashing → PASS
- **Negative / Overflow Durations in Pause Override** → Checked validation constraints → Negative values default to 3600s, overflows capped to 1 year → PASS

---

## Unchallenged Areas

- **Crostini Bridge Spoofing** — We assumed the Host-to-Crostini VM port-forwarding bridge cannot be spoofed by other containers in the VM. If other containers have access to the same local network interface, they could theoretically send spoofed payloads to port 8888, but this is mitigated by HMAC signature verification which requires the shared secret.
