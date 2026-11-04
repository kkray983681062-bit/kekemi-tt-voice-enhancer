const assert = require('assert');
const fs = require('fs');
const path = require('path');
const cp = require('child_process');

const root = process.env.TT_TEST_ROOT || '/tmp/tt-legacy-test';
const controller = path.join(root, 'controller-test.exe');
const ready = '/tmp/tt-bitrate-fixture-ready.log';
const env = {...process.env, WINEDEBUG: '-all'};
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function verify(architecture) {
    try { fs.unlinkSync(ready); } catch (error) { if (error.code !== 'ENOENT') throw error; }
    const dir = path.join(root, architecture);
    const mock = cp.spawn('wine', [path.join(dir, 'TTVoice.exe')], {cwd: dir, env, stdio: 'ignore'});
    try {
        for (let i = 0; i < 150 && !fs.existsSync(ready); ++i) {
            if (mock.exitCode !== null) throw new Error(`${architecture}: simulated client exited`);
            await sleep(100);
        }
        const readyText = fs.readFileSync(ready, 'utf8');
        assert.match(readyText, /ready/, `${architecture}: engine not loaded`);
        const [, pid, base, size] = readyText.trim().split(/\s+/);
        for (const [args, expected] of [
            [['--read'], /READ 64 1 3/],
            [['--set', '128'], /APPLIED 128 1 3/],
            [['--read'], /READ 128 1 3/],
            [['--set', '192'], /APPLIED 192 1 3/],
            [['--read'], /READ 192 1 3/],
        ]) {
            const direct = architecture === 'x86' && env.TT_WINE_DIRECT_X86 === '1';
            const program = direct ? path.join(root, 'direct-target-test.exe') : controller;
            const params = direct ? [pid, base, size, ...args] : args;
            const result = cp.spawnSync('wine', [program, ...params], {cwd: root, env, encoding: 'utf8', timeout: 30000});
            if (result.error) throw result.error;
            assert.strictEqual(result.status, 0, `${architecture}: ${args.join(' ')}: ${result.stdout}${result.stderr}`);
            assert.match(result.stdout, expected, `${architecture}: ${args.join(' ')}`);
        }
        console.log(`PASS: ${architecture} legacy engine read, apply, and readback`);
    } finally {
        mock.kill('SIGTERM');
        await sleep(1000);
    }
}

(async () => {
    await verify('x64');
    await verify('x86');
})().catch(error => { console.error(error.stack || error); process.exitCode = 1; });
