import subprocess
import time
import urllib.request
import urllib.error
import json
import sys
import hmac
import hashlib

def compute_signature(secret, method, path, timestamp_str, body):
    message = f"{method}\n{path}\n{timestamp_str}\n{body}".encode('utf-8')
    return hmac.new(secret.encode('utf-8'), message, hashlib.sha256).hexdigest()

def make_request(url, method, body_dict, headers):
    body = json.dumps(body_dict).encode('utf-8') if body_dict is not None else b""
    req = urllib.request.Request(
        url,
        data=body if method in ["POST", "PUT"] else None,
        headers=headers,
        method=method
    )
    try:
        with urllib.request.urlopen(req, timeout=2) as response:
            return response.getcode(), response.read().decode('utf-8')
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode('utf-8')
    except Exception as e:
        return 0, str(e)

def run_tests():
    secret = "adversarial_test_secret"
    print("Starting edge-daemon with signature verification enabled...")
    
    # Start the daemon in the background
    daemon_proc = subprocess.Popen(
        ["./build/edge-daemon", "--secret", secret],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        cwd="/home/matthias/project/project-chronos/edge-daemon"
    )
    
    # Wait for the daemon to start
    time.sleep(1.5)
    
    try:
        # Helper to get signed GET /status
        def get_status():
            now_ms = int(time.time() * 1000)
            ts_str = str(now_ms)
            sig = compute_signature(secret, "GET", "/status", ts_str, "")
            headers = {
                "X-Signature": sig,
                "X-Timestamp": ts_str
            }
            return make_request("http://127.0.0.1:8888/status", "GET", None, headers)

        # Helper to send signed POST /control
        def send_control(body_dict):
            now_ms = int(time.time() * 1000)
            ts_str = str(now_ms)
            body_str = json.dumps(body_dict)
            sig = compute_signature(secret, "POST", "/control", ts_str, body_str)
            headers = {
                "Content-Type": "application/json",
                "X-Signature": sig,
                "X-Timestamp": ts_str
            }
            return make_request("http://127.0.0.1:8888/control", "POST", body_dict, headers)

        # --- TEST 1: GET /status (Signed, should succeed) ---
        print("Test 1: GET /status (Authenticated)...")
        code, body = get_status()
        assert code == 200, f"Expected 200, got {code}: {body}"
        print("Test 1 PASSED.")

        # --- TEST 2: POST /control with missing signature -> should fail ---
        print("Test 2: POST /control (Missing Signature)...")
        code, body = make_request("http://127.0.0.1:8888/control", "POST", {"action": "pause"}, {})
        assert code == 401, f"Expected 401, got {code}"
        print("Test 2 PASSED.")

        # --- TEST 3: POST /control with valid signature -> should succeed ---
        print("Test 3: POST /control (Valid Signature)...")
        code, body = send_control({"action": "pause"})
        assert code == 200, f"Expected 200, got {code}: {body}"
        
        # Verify it actually paused
        code_s, body_s = get_status()
        assert json.loads(body_s)["paused"] is True, "Expected paused to be true"
        print("Test 3 PASSED.")

        # --- TEST 4: Replay Attack -> should fail ---
        print("Test 4: POST /control (Replay Attack)...")
        # Reuse same signature headers by manually sending the same timestamp/sig
        now_ms = int(time.time() * 1000)
        ts_str = str(now_ms)
        req_body = {"action": "pause"}
        req_body_str = json.dumps(req_body)
        sig = compute_signature(secret, "POST", "/control", ts_str, req_body_str)
        
        headers = {
            "Content-Type": "application/json",
            "X-Signature": sig,
            "X-Timestamp": ts_str
        }
        # First attempt
        code, body = make_request("http://127.0.0.1:8888/control", "POST", req_body, headers)
        assert code == 200 or code == 401, f"First attempt should be 200 or 401: {code}"
        
        # Second attempt (exact same request headers and body)
        code, body = make_request("http://127.0.0.1:8888/control", "POST", req_body, headers)
        assert code == 401, f"Expected 401 (Replay blocked), got {code}: {body}"
        print("Test 4 PASSED.")

        # --- TEST 5: Spoofed Request (Body modified) -> should fail ---
        print("Test 5: POST /control (Spoofed Body)...")
        now_ms = int(time.time() * 1000)
        ts_str = str(now_ms)
        sig = compute_signature(secret, "POST", "/control", ts_str, '{"action":"pause"}')
        headers = {
            "Content-Type": "application/json",
            "X-Signature": sig,
            "X-Timestamp": ts_str
        }
        # Send different body than signed
        code, body = make_request("http://127.0.0.1:8888/control", "POST", {"action": "resume"}, headers)
        assert code == 401, f"Expected 401, got {code}"
        print("Test 5 PASSED.")

        # --- TEST 6: Future Timestamp (Clock skew/tampering) -> should fail ---
        print("Test 6: POST /control (Future Timestamp)...")
        future_ms = int(time.time() * 1000) + 10000 # +10 seconds
        ts_str = str(future_ms)
        req_body = {"action": "resume"}
        req_body_str = json.dumps(req_body)
        sig = compute_signature(secret, "POST", "/control", ts_str, req_body_str)
        headers = {
            "Content-Type": "application/json",
            "X-Signature": sig,
            "X-Timestamp": ts_str
        }
        code, body = make_request("http://127.0.0.1:8888/control", "POST", req_body, headers)
        assert code == 401, f"Expected 401 (Future timestamp blocked), got {code}"
        print("Test 6 PASSED.")

        # --- TEST 7: Expired Timestamp -> should fail ---
        print("Test 7: POST /control (Expired Timestamp)...")
        past_ms = int(time.time() * 1000) - 70000 # -70 seconds
        ts_str = str(past_ms)
        req_body = {"action": "resume"}
        req_body_str = json.dumps(req_body)
        sig = compute_signature(secret, "POST", "/control", ts_str, req_body_str)
        headers = {
            "Content-Type": "application/json",
            "X-Signature": sig,
            "X-Timestamp": ts_str
        }
        code, body = make_request("http://127.0.0.1:8888/control", "POST", req_body, headers)
        assert code == 401, f"Expected 401 (Expired timestamp blocked), got {code}"
        print("Test 7 PASSED.")

        # --- TEST 8: Pause Override Negative Duration -> should be validated and defaulted to 3600s (remain paused) ---
        print("Test 8: POST /control (Pause Override Negative Duration)...")
        # Resume first
        code, body = send_control({"action": "resume"})
        assert code == 200, f"Resume failed: {body}"
        
        # Verify resumed
        code_s, body_s = get_status()
        assert json.loads(body_s)["paused"] is False, "Expected paused to be false"

        # Now send negative duration pause override
        code, body = send_control({"action": "pause_override", "duration": -3600.0})
        assert code == 200, f"Expected 200, got {code}"

        # Verify it remains paused (since negative duration was corrected to 3600)
        code_s, body_s = get_status()
        assert json.loads(body_s)["paused"] is True, "Expected paused to be true (negative duration corrected to default)"
        print("Test 8 PASSED.")

        # --- TEST 9: Pause Override Huge Duration (Overflow) -> should be validated and defaulted to 3600s (remain paused) ---
        print("Test 9: POST /control (Pause Override Huge Duration)...")
        # Resume first
        code, body = send_control({"action": "resume"})
        assert code == 200, f"Resume failed: {body}"

        # Now send huge duration pause override (3e9 seconds)
        code, body = send_control({"action": "pause_override", "duration": 3e9})
        assert code == 200, f"Expected 200, got {code}"

        # Verify it remains paused (huge duration is validated and limited to 3600s default/limit, so it doesn't overflow to negative and instantly unpause)
        code_s, body_s = get_status()
        assert json.loads(body_s)["paused"] is True, "Expected paused to be true (huge duration corrected to default/limit)"
        print("Test 9 PASSED.")

        print("\nAll HTTP API adversarial tests PASSED successfully!")

    except Exception as e:
        print(f"\nTest failure: {e}", file=sys.stderr)
        daemon_proc.terminate()
        out, err = daemon_proc.communicate()
        print("--- Daemon stdout ---")
        print(out)
        print("--- Daemon stderr ---")
        print(err)
        sys.exit(1)
        
    finally:
        daemon_proc.terminate()
        daemon_proc.wait()

if __name__ == "__main__":
    run_tests()
