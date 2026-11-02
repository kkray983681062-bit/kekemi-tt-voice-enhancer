import pathlib
import subprocess
import sys
import time

root = pathlib.Path(sys.argv[1])
mock = root / "TTVoiceTest.exe"
controller = root / "controller-native-test.exe"
no_window = subprocess.CREATE_NO_WINDOW

def run(*args):
    return subprocess.run(
        [str(controller), *args],
        cwd=root,
        capture_output=True,
        text=True,
        timeout=20,
        creationflags=no_window,
    )

process = subprocess.Popen(
    [str(mock)], cwd=root, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    creationflags=no_window,
)
try:
    time.sleep(1.5)
    if process.poll() is not None:
        raise AssertionError("模拟 TT 进程启动失败")
    for args, expected in [
        (("--read",), "READ 64 1 3"),
        (("--set", "192"), "APPLIED 192 1 3"),
        (("--read",), "READ 192 1 3"),
    ]:
        result = run(*args)
        if result.returncode != 0 or expected not in result.stdout:
            raise AssertionError(f"{args}: {result.returncode}: {result.stdout} {result.stderr}")
    result = run("--set", "193")
    if result.returncode == 0 or "OUT_OF_RANGE" not in result.stdout:
        raise AssertionError("无效码率未被拒绝")
    if process.poll() is not None:
        raise AssertionError("模拟 TT 在读写后异常退出")
    print("PASS: Windows 原生进程读取、设置、回读和无效输入")
finally:
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
