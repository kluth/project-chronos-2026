import subprocess
import time
import urllib.request
import urllib.error
import json
import sys

def run_api_tests():
    print("Starting integration tests for edge-daemon HTTP API...")
    
    # Start the daemon in the background
    daemon_proc = subprocess.Popen(
        ["./build/edge-daemon"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    # Wait for the daemon to start and bind to port 8888
    time.sleep(1.5)
    
    try:
        # 1. Test GET /status (initial state)
        print("Testing GET /status (initial)...")
        req = urllib.request.Request("http://127.0.0.1:8888/status")
        with urllib.request.urlopen(req, timeout=2) as response:
            status_code = response.getcode()
            body = response.read().decode('utf-8')
            print(f"Response: {body}")
            assert status_code == 200, f"Expected 200, got {status_code}"
            data = json.loads(body)
            assert data["paused"] is False, "Expected paused to be false"
            assert data["epsilon"] == 0.5, f"Expected epsilon 0.5, got {data['epsilon']}"
            assert data["budget"] == 0.0, f"Expected budget 0.0, got {data['budget']}"
            
        # 2. Test POST /control (pause)
        print("Testing POST /control (pause)...")
        pause_data = json.dumps({"action": "pause"}).encode('utf-8')
        req = urllib.request.Request(
            "http://127.0.0.1:8888/control",
            data=pause_data,
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(req, timeout=2) as response:
            status_code = response.getcode()
            body = response.read().decode('utf-8')
            print(f"Response: {body}")
            assert status_code == 200, f"Expected 200, got {status_code}"
            data = json.loads(body)
            assert data["status"] == "ok", "Expected status ok"
            
        # 3. Verify status is paused
        print("Verifying status is paused...")
        req = urllib.request.Request("http://127.0.0.1:8888/status")
        with urllib.request.urlopen(req, timeout=2) as response:
            body = response.read().decode('utf-8')
            print(f"Response: {body}")
            data = json.loads(body)
            assert data["paused"] is True, "Expected paused to be true"

        # 4. Test POST /control (configure)
        print("Testing POST /control (configure)...")
        config_data = json.dumps({
            "action": "configure",
            "epsilon": 0.75,
            "budget": 2.5
        }).encode('utf-8')
        req = urllib.request.Request(
            "http://127.0.0.1:8888/control",
            data=config_data,
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(req, timeout=2) as response:
            status_code = response.getcode()
            body = response.read().decode('utf-8')
            print(f"Response: {body}")
            assert status_code == 200, f"Expected 200, got {status_code}"
            data = json.loads(body)
            assert data["status"] == "ok", "Expected status ok"

        # 5. Verify configuration update
        print("Verifying configuration update...")
        req = urllib.request.Request("http://127.0.0.1:8888/status")
        with urllib.request.urlopen(req, timeout=2) as response:
            body = response.read().decode('utf-8')
            print(f"Response: {body}")
            data = json.loads(body)
            assert data["epsilon"] == 0.75, f"Expected epsilon 0.75, got {data['epsilon']}"
            assert data["budget"] == 2.5, f"Expected budget 2.5, got {data['budget']}"

        # 6. Test POST /control (resume)
        print("Testing POST /control (resume)...")
        resume_data = json.dumps({"action": "resume"}).encode('utf-8')
        req = urllib.request.Request(
            "http://127.0.0.1:8888/control",
            data=resume_data,
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(req, timeout=2) as response:
            status_code = response.getcode()
            body = response.read().decode('utf-8')
            print(f"Response: {body}")
            assert status_code == 200, f"Expected 200, got {status_code}"
            data = json.loads(body)
            assert data["status"] == "ok", "Expected status ok"

        # 7. Verify status is resumed
        print("Verifying status is resumed...")
        req = urllib.request.Request("http://127.0.0.1:8888/status")
        with urllib.request.urlopen(req, timeout=2) as response:
            body = response.read().decode('utf-8')
            print(f"Response: {body}")
            data = json.loads(body)
            assert data["paused"] is False, "Expected paused to be false"

        print("API integration tests PASSED successfully!")
        
    except Exception as e:
        print(f"Test failure: {e}", file=sys.stderr)
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
    run_api_tests()
