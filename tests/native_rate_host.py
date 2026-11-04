import os
import pathlib
import subprocess
import sys
import time

root = pathlib.Path(sys.argv[1]).resolve()
ready = root / "rate-ready.txt"
probe = root / "rate-process-probe.exe"

for width in (8, 4):
    for inactive in (False, True):
        ready.unlink(missing_ok=True)
        env = os.environ.copy()
        env["TT_FIXTURE_READY"] = str(ready)
        env.pop("TT_FIXTURE_INACTIVE", None)
        if inactive:
            env["TT_FIXTURE_INACTIVE"] = "1"
        process = subprocess.Popen(
            [str(root / f"rate-fixture-{width}.exe")], cwd=root, env=env,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        try:
            for _ in range(100):
                if ready.exists():
                    break
                if process.poll() is not None:
                    raise AssertionError(f"{width}-byte fixture exited before becoming ready")
                time.sleep(0.05)
            info = ready.read_text(encoding="ascii").strip().split()
            if len(info) != 3 or int(info[0]) != process.pid or int(info[2]) != width:
                raise AssertionError("fixture process information does not match")
            result = subprocess.run(
                [str(probe), *info], cwd=root, capture_output=True, text=True, timeout=20,
                creationflags=subprocess.CREATE_NO_WINDOW,
            )
            expected = "WAITING" if inactive else "ACTIVE 137.25"
            if result.returncode != 0 or expected not in result.stdout:
                raise AssertionError(f"{width}: {result.returncode}: {result.stdout} {result.stderr}")
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            ready.unlink(missing_ok=True)

print("PASS: Windows 32-bit and 64-bit mock-process sampling and inactive-state guard")
