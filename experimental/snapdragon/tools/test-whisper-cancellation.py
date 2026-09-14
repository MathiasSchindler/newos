import argparse
import ctypes
import pathlib
import subprocess
import sys
import threading


def run_case(args):
    command = [str(args.probe), '--model=medium', '--decoder-offload=fused,self,logits']
    if args.phase == 'quiet':
        command += ['--quiet', str(args.wav)]
        marker = b''
    else:
        command += ['--single-window=' + str(args.wav)]
        marker = (b'contextFree before cache load:' if args.phase == 'startup'
                  else b'Transcript (German):')
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    handler_type = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_uint)
    handler = handler_type(lambda event: 1)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.SetConsoleCtrlHandler.argtypes = [handler_type, ctypes.c_int]
    kernel.GenerateConsoleCtrlEvent.argtypes = [ctypes.c_uint, ctypes.c_uint]
    if not kernel.SetConsoleCtrlHandler(handler, 1):
        process.kill()
        process.wait()
        raise ctypes.WinError(ctypes.get_last_error())
    watchdog = threading.Timer(60, process.kill)
    watchdog.start()
    output = bytearray()
    sent = False
    try:
        while True:
            chunk = process.stdout.read(1)
            if not chunk:
                break
            output.extend(chunk)
            ready = len(output) >= 32 if args.phase == 'quiet' else marker in output
            if ready and not sent:
                if not kernel.GenerateConsoleCtrlEvent(args.event, 0):
                    raise ctypes.WinError(ctypes.get_last_error())
                sent = True
        status = process.wait(timeout=10)
    finally:
        watchdog.cancel()
        if process.poll() is None:
            process.kill()
            process.wait()
    text = output.decode('utf-8', errors='replace')
    log = args.output / f'{args.phase}-{args.event}.txt'
    log.write_text(text, encoding='utf-8')
    assert sent, f'Cancellation trigger not reached: {log}'
    assert status == 130, f'Expected exit 130, got {status}: {log}'
    assert 'npu_probe: interrupted; resources released.' in text, log
    assert 'm_mutex' not in text and '<E>' not in text and 'npu_probe: failed' not in text, log
    if args.phase != 'quiet':
        for stage in ('contextFree', 'deviceFree', 'backendFree', 'logFree'):
            assert f'{stage}: 0x0000000000000000' in text, (stage, log)
    else:
        assert text.index('npu_probe: interrupted') >= 32, log
    print(f'PASS {args.phase} console event {args.event}: exit 130, clean shutdown')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=pathlib.Path,
                        default=pathlib.Path('experimental/snapdragon/build/memory-candidate/npu_probe.exe'))
    parser.add_argument('--wav', type=pathlib.Path,
                        default=pathlib.Path('experimental/snapdragon/build/long-form-35s.wav'))
    parser.add_argument('--output', type=pathlib.Path,
                        default=pathlib.Path('tests/tmp/whisper-cancellation'))
    parser.add_argument('--phase', choices=['startup', 'decode', 'quiet'])
    parser.add_argument('--event', type=int, choices=[0, 1], default=0)
    args = parser.parse_args()
    args.probe = args.probe.resolve()
    args.wav = args.wav.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.phase:
        run_case(args)
        return
    for phase, event in [('startup', 0), ('decode', 0), ('decode', 1), ('quiet', 0)]:
        command = [sys.executable, str(pathlib.Path(__file__).resolve()),
                   '--probe', str(args.probe), '--wav', str(args.wav),
                   '--output', str(args.output), '--phase', phase, '--event', str(event)]
        result = subprocess.run(command, creationflags=subprocess.CREATE_NEW_CONSOLE,
                                capture_output=True, timeout=90)
        print(result.stdout.decode('utf-8', errors='replace'), end='')
        if result.returncode:
            raise RuntimeError(result.stderr.decode('utf-8', errors='replace'))


if __name__ == '__main__':
    main()