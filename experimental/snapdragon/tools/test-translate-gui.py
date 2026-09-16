"""Native Win32 GUI integration; Python is test-only."""
import ctypes as ct
from ctypes import wintypes as wt
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
user = ct.WinDLL('user32', use_last_error=True)
kernel = ct.WinDLL('kernel32', use_last_error=True)
gdi = ct.WinDLL('gdi32', use_last_error=True)


def api(library, name, result, *args):
    function = getattr(library, name)
    function.restype = result
    function.argtypes = args
    return function


send = api(user, 'SendMessageW', ct.c_ssize_t, wt.HWND, wt.UINT, ct.c_size_t, ct.c_ssize_t)
post = api(user, 'PostMessageW', wt.BOOL, wt.HWND, wt.UINT, ct.c_size_t, ct.c_ssize_t)
child = api(user, 'GetDlgItem', wt.HWND, wt.HWND, ct.c_int)
enabled = api(user, 'IsWindowEnabled', wt.BOOL, wt.HWND)
pid_of = api(user, 'GetWindowThreadProcessId', wt.DWORD, wt.HWND, ct.POINTER(wt.DWORD))
class_name = api(user, 'GetClassNameW', ct.c_int, wt.HWND, wt.LPWSTR, ct.c_int)
wait_process = api(kernel, 'WaitForSingleObject', wt.DWORD, wt.HANDLE, wt.DWORD)
move = api(user, 'MoveWindow', wt.BOOL, wt.HWND, ct.c_int, ct.c_int, ct.c_int, ct.c_int, wt.BOOL)
rect_of = api(user, 'GetWindowRect', wt.BOOL, wt.HWND, ct.POINTER(wt.RECT))
callback_type = ct.WINFUNCTYPE(wt.BOOL, wt.HWND, ct.c_ssize_t)
enum_windows = api(user, 'EnumWindows', wt.BOOL, callback_type, ct.c_ssize_t)


def text(window):
    value = ct.create_unicode_buffer(200000)
    send(window, 0x0d, len(value), ct.addressof(value))
    return value.value


def set_text(window, value):
    buffer = ct.create_unicode_buffer(value)
    assert send(window, 0x0c, 0, ct.addressof(buffer))
    parent = api(user, 'GetParent', wt.HWND, wt.HWND)(window)
    control_id = api(user, 'GetDlgCtrlID', ct.c_int, wt.HWND)(window)
    send(parent, 0x111, control_id | (0x300 << 16), window)


def wait(process, predicate, seconds=60):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        assert process.poll() is None, ('early exit', process.returncode)
        wait_process(int(process._handle), 30)
    raise AssertionError('GUI deadline exceeded')


def find(process):
    found = []

    @callback_type
    def visit(window, unused):
        owner = wt.DWORD()
        pid_of(window, ct.byref(owner))
        name = ct.create_unicode_buffer(128)
        class_name(window, name, len(name))
        if owner.value == process.pid and name.value == 'NewosTranslateGemma':
            found.append(window)
        return True

    enum_windows(visit, 0)
    return found[0] if found else None


def choose(combo, code):
    for index in range(send(combo, 0x146, 0, 0)):
        value = ct.create_unicode_buffer(256)
        send(combo, 0x148, index, ct.addressof(value))
        if value.value.endswith('(' + code + ')'):
            send(combo, 0x14e, index, 0)
            return
    raise AssertionError(('missing language', code))


def screenshot(window, path):
    rect = wt.RECT()
    assert rect_of(window, ct.byref(rect))
    width, height = rect.right - rect.left, rect.bottom - rect.top
    get_dc = api(user, 'GetWindowDC', wt.HDC, wt.HWND)
    release = api(user, 'ReleaseDC', ct.c_int, wt.HWND, wt.HDC)
    compatible = api(gdi, 'CreateCompatibleDC', wt.HDC, wt.HDC)
    bitmap_create = api(gdi, 'CreateCompatibleBitmap', wt.HBITMAP, wt.HDC, ct.c_int, ct.c_int)
    select = api(gdi, 'SelectObject', wt.HANDLE, wt.HDC, wt.HANDLE)
    delete = api(gdi, 'DeleteObject', wt.BOOL, wt.HANDLE)
    delete_dc = api(gdi, 'DeleteDC', wt.BOOL, wt.HDC)
    capture = api(user, 'PrintWindow', wt.BOOL, wt.HWND, wt.HDC, wt.UINT)
    get_bits = api(gdi, 'GetDIBits', ct.c_int, wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ct.c_void_p, ct.c_void_p, wt.UINT)
    source = get_dc(window)
    destination = compatible(source)
    bitmap = bitmap_create(source, width, height)
    previous = select(destination, bitmap)
    try:
        assert capture(window, destination, 2)
        select(destination, previous)
        info = ct.create_string_buffer(struct.pack('<IiiHHIIiiII', 40, width, -height, 1, 32, 0, 0, 0, 0, 0, 0))
        pixels = ct.create_string_buffer(width * height * 4)
        assert get_bits(source, bitmap, 0, height, pixels, info, 0) == height
        raw = pixels.raw
        rows = []
        for row in range(height):
            data = raw[row * width * 4:(row + 1) * width * 4]
            rgb = bytearray(width * 3)
            rgb[0::3], rgb[1::3], rgb[2::3] = data[2::4], data[1::4], data[0::4]
            rows.append(b'\0' + rgb)

        def chunk(kind, data):
            return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

        path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                         chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))
    finally:
        delete(bitmap)
        delete_dc(destination)
        release(window, source)


def main():
    api(user, 'SetProcessDpiAwarenessContext', wt.BOOL, wt.HANDLE)(ct.c_void_p(-4))
    records = []
    process = subprocess.Popen([str(ROOT / 'build/translate-gui.exe')], cwd=ROOT.parent.parent)
    try:
        started = time.perf_counter()
        window = wait(process, lambda: find(process))
        status = child(window, 106)
        wait(process, lambda: text(status) == 'Ready')
        records.append(dict(name='preload', seconds=time.perf_counter() - started))
        source, target, input_box, output, button = [child(window, number) for number in (101, 102, 103, 104, 105)]
        assert send(source, 0x146, 0, 0) == 581
        assert not enabled(button)
        for name, language_from, language_to, value, expected in [
            ('first', 'de', 'en', 'Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts.', 'Good day, my name is Hase. I know nothing.'),
            ('repeat', 'de', 'en', 'Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts.', 'Good day, my name is Hase. I know nothing.'),
            ('unicode', 'en', 'de', 'The door is open.', 'Die T\u00fcr ist offen.'),
            ('multiline', 'de', 'en', 'Guten Tag.\r\nGuten Tag.', None),
        ]:
            choose(source, language_from)
            choose(target, language_to)
            set_text(input_box, value)
            assert enabled(button)
            started = time.perf_counter()
            send(button, 0xf5, 0, 0)
            assert not enabled(button)
            wait(process, lambda: bool(text(output)))
            first = time.perf_counter() - started
            wait(process, lambda: text(status) in ('Ready', 'Cannot complete a text segment.'))
            assert text(status) == 'Ready'
            result = text(output).strip()
            if expected is not None:
                assert result == expected, (name, result)
            records.append(dict(name=name, seconds=time.perf_counter() - started, first_seconds=first, output=result))
        screenshot(window, ROOT / 'build/translate-gui-desktop.png')
        dpi = api(user, 'GetDpiForWindow', wt.UINT, wt.HWND)(window)
        assert move(window, 30, 30, 460 * dpi // 96, 500 * dpi // 96, True)

        def layout_settled():
            outer, editor = wt.RECT(), wt.RECT()
            assert rect_of(window, ct.byref(outer))
            assert rect_of(output, ct.byref(editor))
            return outer.left < editor.left < editor.right < outer.right and editor.bottom < outer.bottom

        wait(process, layout_settled)
        screenshot(window, ROOT / 'build/translate-gui-compact.png')
        sentence = 'Guten Tag, mein Name ist Hase. Ich wei\u00df von nichts.'
        set_text(input_box, ' '.join([sentence] * 32) + '\r\n\r\nGuten Tag.')
        started = time.perf_counter()
        send(button, 0xf5, 0, 0)
        wait(process, lambda: text(status) == 'Ready', seconds=180)
        result = text(output)
        assert result.count('Hase') == 32, result
        assert '\r\n\r\n' in result and result.rstrip().endswith('Good day.'), result
        records.append(dict(name='long-paragraph', seconds=time.perf_counter() - started, output=result))
        assert enabled(button)
        set_text(input_box, 'Guten Tag.')
        send(button, 0xf5, 0, 0)
        wait(process, lambda: text(status) == 'Ready')
        set_text(input_box, 'Guten Tag. ' * 30)
        send(button, 0xf5, 0, 0)
        post(window, 0x10, 0, 0)
        assert process.wait(timeout=60) == 0
        records.append(dict(name='close-during-request', passed=True))
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        (ROOT / 'build/translate-gui-results.json').write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding='utf-8')
    process = subprocess.Popen([str(ROOT / 'build/translate-gui.exe')], cwd=ROOT.parent.parent)
    try:
        window = wait(process, lambda: find(process))
        post(window, 0x10, 0, 0)
        assert process.wait(timeout=60) == 0
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    records.append(dict(name='close-during-loading', passed=True))
    with tempfile.TemporaryDirectory(prefix='translate-gui-test-') as directory:
        executable = Path(directory) / 'translate-gui.exe'
        shutil.copy2(ROOT / 'build/translate-gui.exe', executable)
        process = subprocess.Popen([str(executable)], cwd=directory)
        try:
            window = wait(process, lambda: find(process))
            wait(process, lambda: text(child(window, 106)).startswith('Model failed.'))
            assert not enabled(child(window, 105))
            post(window, 0x10, 0, 0)
            assert process.wait(timeout=10) != 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    records.append(dict(name='missing-assets', passed=True))
    (ROOT / 'build/translate-gui-results.json').write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(records, ensure_ascii=True), flush=True)
    print('PASS resident GUI, language changes, Unicode, long paragraphs and shutdown', flush=True)


if __name__ == '__main__':
    main()