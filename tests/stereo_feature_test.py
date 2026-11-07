"""Authorized, short live test: preserve bitrate/codec and restore the channel."""
import argparse
import pathlib
import subprocess

p = argparse.ArgumentParser()
p.add_argument("cli", type=pathlib.Path)
p.add_argument("--authorized-live-test", action="store_true", required=True)
args = p.parse_args()

def run(*argv):
    return subprocess.run([str(args.cli.resolve()), *argv], capture_output=True,
                          text=True, timeout=20, creationflags=subprocess.CREATE_NO_WINDOW)

original = run("--read")
assert original.returncode == 0, original.stdout + original.stderr
_, bitrate, channel, codec = original.stdout.strip().split()
target = "2" if channel == "1" else "1"
changed = False
try:
    result = run("--channels", target)
    changed = result.returncode == 0
    assert changed, result.stdout + result.stderr
    assert result.stdout.strip() == f"CHANNELS {bitrate} {target} {codec} CAPTURE {target} ENCODER {target}", result.stdout
    readback = run("--read")
    assert readback.returncode == 0 and readback.stdout.strip() == f"READ {bitrate} {target} {codec}", readback.stdout
    invalid = run("--channels", "3")
    assert invalid.returncode != 0 and "OUT_OF_RANGE" in invalid.stdout, invalid.stdout
finally:
    if changed:
        restored = run("--channels", channel)
        assert restored.returncode == 0, "RESTORE FAILED: " + restored.stdout + restored.stderr
        final = run("--read")
        assert final.returncode == 0 and final.stdout == original.stdout, "RESTORE MISMATCH: " + final.stdout
        print("RESTORED", final.stdout.strip())
print("PASS: real game client switches local capture and encoder channels, preserves bitrate/codec, rejects invalid channels, and restores original")
