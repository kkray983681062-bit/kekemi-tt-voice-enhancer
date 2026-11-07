/* Exercise the exact experimental RPC shim inside this disposable process. */
#define main stereo_probe_entry
#include "stereo_live_probe.c"
#undef main
#include <assert.h>

static volatile int received_index = -1, received_mode = -1;
static int fake_capture(int index, int mode) {
    received_index = index; received_mode = mode;
    return 0;
}

int main(void) {
    Target target = {0};
    target.process = GetCurrentProcess();
    target.abi = ABI_MODERN64;
    target.engine_size = 0x2289000;
    target.engine_base = (uintptr_t)fake_capture - 0xb97b10;
    RemoteCall remote;
    DWORD error = 0;
    assert(remote_prepare(&target, &remote, &error) == TT_OK);
    assert(remote_capture_mode(&target, &remote, 1, &error) == TT_OK);
    assert(received_index == 0 && received_mode == 1);
    assert(remote_capture_mode(&target, &remote, 0, &error) == TT_OK);
    assert(received_index == 0 && received_mode == 0);
    remote_free(&target, &remote);
    puts("PASS: stereo RPC passes engine index and mode; restores mono");
    return 0;
}
