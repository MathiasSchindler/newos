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

ROOT = Path(__file__).resolve().parents[2]
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


def find(process, expected_class='NewosTranslateGemma'):
    found = []

    @callback_type
    def visit(window, unused):
        owner = wt.DWORD()
        pid_of(window, ct.byref(owner))
        name = ct.create_unicode_buffer(128)
        class_name(window, name, len(name))
        if owner.value == process.pid and name.value == expected_class:
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
    api(user,'RedrawWindow',wt.BOOL,wt.HWND,ct.c_void_p,wt.HANDLE,wt.UINT)(window,None,None,0x185)
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


def test_ocr(layout_only=False,build=None):
    import struct
    import zlib
    api(user, 'SetProcessDpiAwarenessContext', wt.BOOL, wt.HANDLE)(ct.c_void_p(-4))
    build = (build or ROOT/'build/ocr-app').resolve()
    records = []
    process = subprocess.Popen([str(build/'ocr-gui.exe')],cwd=ROOT.parent.parent)
    def open_path(value):
        set_text(child(window,101),str(value))
        send(window,0x111,101 | (0x200 << 16),child(window,101))
    try:
        window = wait(process,lambda:find(process,'NewosGlmOcr'))
        status,button,output = [child(window,number) for number in (106,105,104)]
        assert not enabled(button)
        assert send(child(window,103),0x146,0,0) == 3
        with tempfile.TemporaryDirectory(prefix='ocr-gui-',dir=ROOT/'build') as temporary:
            image = Path(temporary)/'OCR \u00e4 test.png'
            def chunk(kind,payload):
                return struct.pack('>I',len(payload))+kind+payload+struct.pack('>I',zlib.crc32(kind+payload))
            bitmap = (ROOT/'models/glm-ocr-vision-v2/pattern.bmp').read_bytes()
            width,height = struct.unpack_from('<ii',bitmap,18)
            assert width == height == 224 and struct.unpack_from('<H',bitmap,28)[0] == 24
            offset = struct.unpack_from('<I',bitmap,10)[0]; raw = bytearray()
            for row in range(height):
                source = bitmap[offset+(height-row-1)*width*3:offset+(height-row)*width*3]
                expanded = b''.join(source[column:column+3][::-1]*4 for column in range(0,len(source),3))
                raw.extend((b'\0'+expanded)*4)
            image.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',896,896,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))
            portrait = Path(temporary)/'palette-portrait.png'
            portrait.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',400,600,1,3,0,0,0))+
                                 chunk(b'PLTE',b'\xff\xff\xff\x20\x40\x80')+chunk(b'tRNS',b'\0\xff')+
                                 chunk(b'IDAT',zlib.compress((b'\0'+b'\x55'*50)*600))+chunk(b'IEND',b''))
            open_path(portrait); assert enabled(button),text(status)
            screenshot(window,build/'gui-portrait.png')
            open_path(image)
            assert enabled(button), text(status)
            screenshot(window,build/'gui-preview.png')
            for name in (() if layout_only else ('first','repeat')):
                started = time.perf_counter(); send(button,0xf5,0,0)
                assert not enabled(button) and enabled(child(window,107))
                wait(process,lambda:bool(text(output)),seconds=240)
                first = time.perf_counter()-started
                wait(process,lambda:enabled(button),seconds=240)
                assert text(status) == 'Ready',text(status)
                actual = text(output).replace('\r\n','\n')
                expected = (ROOT/'models/glm-ocr-vision-v2/pattern.expected.txt').read_text(encoding='utf-8').rstrip('\n')
                assert actual == expected,actual
                records.append(dict(case=name,seconds=time.perf_counter()-started,first_seconds=first,text=actual))
            screenshot(window,build/'gui-desktop.png')
            dpi = api(user,'GetDpiForWindow',wt.UINT,wt.HWND)(window)
            assert move(window,30,30,460*dpi//96,620*dpi//96,True)
            def settled():
                outer = wt.RECT(); rect_of(window,ct.byref(outer))
                for number in (101,102,103,104,105,106,107,108):
                    editor = wt.RECT(); rect_of(child(window,number),ct.byref(editor))
                    if not (outer.left < editor.left < editor.right < outer.right and editor.bottom < outer.bottom): return False
                return True
            wait(process,settled); screenshot(window,build/'gui-compact.png')
            if layout_only:
                print('PASS OCR large PNG / transparent palette portrait import and desktop/compact layout bounds',flush=True)
                return
            invalid = Path(temporary)/'invalid.png'; invalid.write_bytes(b'bad image')
            open_path(invalid); assert not enabled(button) and text(status).startswith('Cannot open image')
            open_path(ROOT/'models/glm-ocr-vision-v2/receipt.png'); assert enabled(button)
            send(button,0xf5,0,0); send(child(window,107),0xf5,0,0)
            wait(process,lambda:enabled(button),seconds=30)
            assert text(status).startswith('Cancelled'),text(status)
            records.append(dict(case='cancel',passed=True))
            send(button,0xf5,0,0); post(window,0x10,0,0)
            assert process.wait(timeout=30) == 0
            records.append(dict(case='close-active',passed=True))
    finally:
        if process.poll() is None:
            post(window,0x10,0,0)
            process.wait(timeout=30)
    with tempfile.TemporaryDirectory(prefix='ocr-gui-missing-') as temporary:
        executable = Path(temporary)/'ocr-gui.exe'; shutil.copy2(build/'ocr-gui.exe',executable)
        process = subprocess.Popen([str(executable)],cwd=temporary)
        try:
            window = wait(process,lambda:find(process,'NewosGlmOcr'))
            open_path(ROOT/'models/glm-ocr-vision-v2/pattern.png')
            send(child(window,105),0xf5,0,0)
            assert text(child(window,106)).startswith('Cannot start OCR')
            assert enabled(child(window,105))
            post(window,0x10,0,0); assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                post(window,0x10,0,0); process.wait(timeout=30)
    records.append(dict(case='missing-engine',passed=True))
    (build/'gui-results.json').write_text(json.dumps(records,indent=2,ensure_ascii=False),encoding='utf-8')
    print(json.dumps(records,ensure_ascii=True),flush=True)
    print('PASS OCR GUI: Unicode paths, preview, repeated recognition, resize, invalid input, cancellation and shutdown',flush=True)


if __name__ == '__main__':
    import sys
    if sys.argv[1:] == ['--ocr']: test_ocr()
    elif sys.argv[1:] == ['--ocr-layout']: test_ocr(layout_only=True)
    elif len(sys.argv) == 3 and sys.argv[1] in ('--ocr','--ocr-layout'): test_ocr(sys.argv[1]=='--ocr-layout',Path(sys.argv[2]))
    elif not sys.argv[1:]: main()
    else: raise SystemExit('Expected --ocr or no arguments')