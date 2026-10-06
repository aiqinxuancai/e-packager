"""使用实际支持库和 IDE 编译产物验证控件独有事件、参数和返回值。"""
import argparse
import copy
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time
import xml.etree.ElementTree as E


def run(args, log):
    result = subprocess.run([str(a) for a in args], capture_output=True)
    log.write_bytes(result.stdout + result.stderr)
    assert result.returncode == 0, f'{log}: exit={result.returncode}'


def save(tree, path):
    E.indent(tree)
    text = E.tostring(tree.getroot(), encoding='unicode', xml_declaration=True)
    path.write_bytes(b'\xef\xbb\xbf' + text.replace('\n', '\r\n').encode())


def prepare(repo, output):
    tool = repo / 'bin/Win32/Release/e-packager.exe'
    source = repo / 'eproj/e-window-exe-full+otherFne.e'
    original = source.read_bytes()
    shutil.copy2(source, output / 'original.e')
    workspace = output / 'workspace'
    run([tool, 'unpack', output / 'original.e', workspace, '--main-only'], output / 'unpack.log')
    a = E.parse(workspace / 'src/_启动窗口.xml')
    b = E.parse(workspace / 'src/窗口1.xml')
    originals = {n.get('名称'): n for tree in (a, b) for n in tree.iter()
                 if n.get('名称') and '宽度' in n.attrib}
    root = a.getroot()
    for child in list(root):
        root.remove(child)
    root.set('标题', 'unique-ready')
    root.set('宽度', '900')
    root.set('高度', '700')
    controls = {'_启动窗口': root}
    placements = [('列表框1', 20, 20, 160, 90), ('组合框1', 200, 20, 180, 120),
                  ('选择夹1', 20, 150, 300, 100), ('高级选择夹1', 350, 150, 350, 100),
                  ('动画框1', 350, 300, 240, 180), ('滑块条1', 20, 280, 250, 40),
                  ('日期框1', 20, 340, 200, 30), ('编辑框1', 20, 400, 200, 30),
                  ('标签1', 620, 300, 150, 40)]
    for name, x, y, width, height in placements:
        node = copy.deepcopy(originals[name])
        for descendant in node.iter():
            for child in list(descendant):
                if child.tag.endswith('.事件') or child.tag.endswith('.子夹'):
                    descendant.remove(child)
        for key, value in [('左边', x), ('顶边', y), ('宽度', width), ('高度', height)]:
            node.set(key, str(value))
        root.append(node)
        controls[name] = node
    controls['高级选择夹1'].set('子夹头高度', '24')
    controls['编辑框1'].set('调节器方式', '2')
    catalog = {n.get('控件类型'): {ev.get('名称'): ev for ev in n}
               for tree in (a, b) for n in tree.getroot().findall('窗口.事件定义')}
    # a 的原节点已清空，从未修改的 XML 重读核心控件事件目录。
    baseline = E.parse(workspace / 'src/_启动窗口.xml')
    catalog.update({n.get('控件类型'): {ev.get('名称'): ev for ev in n}
                    for n in baseline.getroot().findall('窗口.事件定义')})
    lines = ['.版本 2', '.支持库 iext', '.支持库 iext2', '.支持库 iext3',
             '.程序集 窗口程序集_启动窗口', '.程序集变量 允许改变, 逻辑型', '.程序集变量 物体, 整数型', '']
    bindings = []

    def record(name, expression='“1”'):
        return f'写到文件 (“{name}.txt”, 到字节集 ({expression}))'

    def bind(name, event, body, ret=None):
        node = controls[name]
        definition = catalog[node.tag][event]
        handler = '独有事件' + str(len(bindings))
        E.SubElement(node, node.tag + '.事件', {'名称': event, '处理器': '窗口程序集_启动窗口::' + handler})
        result_type = definition.get('返回类型')
        lines.append('.子程序 ' + handler + (', ' + result_type if result_type else ''))
        for param in definition:
            lines.append('.参数 ' + param.get('名称') + ', ' + param.get('类型') +
                         (', 参考' if param.get('参考') == '真' else ''))
        lines.extend(body)
        if result_type:
            lines.append('返回 (' + (ret if ret is not None else ('真' if result_type == '逻辑型' else '0')) + ')')
        lines.append('')
        bindings.append({'control': name, 'type': node.tag, 'event': event, 'index': int(definition.get('索引')),
                         'return_type': result_type, 'parameters': [p.attrib for p in definition]})

    bind('列表框1', '列表项被选择', [record('list-select', '到文本 (列表框1.现行选中项)')])
    bind('列表框1', '双击选择', [record('list-double', '到文本 (列表框1.现行选中项)')])
    bind('组合框1', '列表项被选择', [record('combo-select', '到文本 (组合框1.现行选中项)')])
    bind('组合框1', '将弹出列表', [record('combo-open')])
    bind('组合框1', '列表被关闭', [record('combo-close')])
    bind('选择夹1', '将改变子夹', [record('tab-changing')], '允许改变')
    bind('选择夹1', '子夹被改变', [record('tab-changed', '到文本 (选择夹1.现行子夹)')])
    bind('高级选择夹1', '将改变子夹', [record('advanced-changing', '到文本 (子夹索引)')], '允许改变')
    bind('高级选择夹1', '子夹被改变', [record('advanced-changed', '到文本 (高级选择夹1.现行子夹)')])
    bind('高级选择夹1', '子夹头被单击', [record('advanced-click', '到文本 (子夹索引)')])
    bind('高级选择夹1', '子夹头被右击', [record('advanced-right', '到文本 (子夹索引)')])
    bind('高级选择夹1', '子夹头被点燃', [record('advanced-hover', '到文本 (子夹索引)')])
    bind('动画框1', '动画框鼠标位置改变', [record('animation-mouse', '到文本 (横坐标) ＋ “,” ＋ 到文本 (纵坐标) ＋ “,” ＋ 到文本 (已被按下物体)')])
    bind('动画框1', '物体位置将改变', [record('animation-changing', '到文本 (物体标识值) ＋ “,” ＋ 到文本 (横坐标) ＋ “,” ＋ 到文本 (纵坐标)')], '允许改变')
    bind('动画框1', '物体位置已改变', [record('animation-changed', '到文本 (物体标识值) ＋ “,” ＋ 到文本 (横坐标) ＋ “,” ＋ 到文本 (纵坐标)')])
    bind('动画框1', '物体将销毁', [record('animation-destroy', '到文本 (物体标识值)')])
    bind('滑块条1', '位置被改变', [record('slider', '到文本 (滑块条1.位置)')])
    bind('日期框1', '选择日期被改变', [record('date')])
    bind('编辑框1', '调节钮被按下', [record('spin', '到文本 (按钮值)')])
    bind('标签1', '反馈事件', [record('feedback-args', '到文本 (参数一) ＋ “,” ＋ 到文本 (参数二)')], '参数一 × 10 ＋ 参数二')
    bind('_启动窗口', '创建完毕', ['允许改变 ＝ 假', '高级选择夹1.置子夹名称 (0, “PageA”)',
         '高级选择夹1.加入子夹 (“PageB”, , , , )', '物体 ＝ 动画框1.创建物体 (, 10, 20, 真, )',
         record('object-id', '到文本 (物体)'), record('ready')])
    actions = {
        'allow': ['允许改变 ＝ 真', record('allowed')],
        'deny': ['允许改变 ＝ 假', record('denied')],
        'query': [record('state', '到文本 (选择夹1.现行子夹) ＋ “,” ＋ 到文本 (高级选择夹1.现行子夹) ＋ “,” ＋ 到文本 (动画框1.取物体左边 (物体)) ＋ “,” ＋ 到文本 (动画框1.取物体顶边 (物体))')],
        'feedback': [record('feedback-result', '到文本 (标签1.调用反馈事件 (37, 9, 真))')],
        'move': ['动画框1.置物体位置 (物体, 42, 53, 假)', record('move-done')],
        'destroy': ['动画框1.销毁物体 (物体)', record('destroy-done')],
    }
    for index, (action, body) in enumerate(actions.items()):
        node = E.SubElement(root, '按钮', {'名称': '测试按钮' + str(index), '标题': action,
            '左边': str(20 + index * 120), '顶边': '550', '宽度': '110', '高度': '30'})
        controls[node.get('名称')] = node
        bind(node.get('名称'), '被单击', body)
    save(a, workspace / 'src/_启动窗口.xml')
    for node in b.iter():
        for child in list(node):
            if child.tag.endswith('.事件'):
                node.remove(child)
    save(b, workspace / 'src/窗口1.xml')
    (workspace / 'src/窗口程序集_启动窗口.txt').write_bytes(b'\xef\xbb\xbf' + '\r\n'.join(lines).encode())
    run([tool, 'pack', workspace, output / 'unique.e'], output / 'pack.log')
    run([tool, 'unpack', output / 'unique.e', output / 'reopened', '--main-only'], output / 'reopen.log')
    reopened = E.parse(output / 'reopened/src/_启动窗口.xml')
    actual = {(n.get('名称'), c.get('名称')): int(c.get('索引'))
              for n in reopened.iter() for c in n if c.tag.endswith('.事件')}
    for binding in bindings:
        assert actual[binding['control'], binding['event']] == binding['index'], binding
    assert source.read_bytes() == original
    (output / 'sample-sha256.txt').write_text(hashlib.sha256(original).hexdigest(), encoding='ascii')
    return bindings


def verify(output):
    u = C.windll.user32
    u.SetWindowPos.argtypes = [W.HWND, W.HWND, C.c_int, C.c_int, C.c_int, C.c_int, W.UINT]
    u.WindowFromPoint.argtypes = [W.POINT]
    u.WindowFromPoint.restype = W.HWND
    u.SendMessageW.argtypes = [C.c_void_p, C.c_uint, C.c_size_t, C.c_ssize_t]
    u.SendMessageW.restype = C.c_ssize_t
    rows = []
    callback = C.WINFUNCTYPE(C.c_bool, C.c_void_p, C.c_void_p)
    results = []
    cursor = W.POINT()
    u.GetCursorPos(C.byref(cursor))

    @callback
    def child(hwnd, unused):
        text = C.create_unicode_buffer(256)
        cls = C.create_unicode_buffer(256)
        u.GetWindowTextW(hwnd, text, len(text))
        u.GetClassNameW(hwnd, cls, len(cls))
        rows.append((hwnd, cls.value, text.value))
        return True

    @callback
    def top(hwnd, unused):
        pid = C.c_ulong()
        u.GetWindowThreadProcessId(hwnd, C.byref(pid))
        if pid.value == process.pid:
            child(hwnd, 0)
            u.EnumChildWindows(hwnd, child, 0)
        return True

    def expect(name, expected=None):
        path = output / (name + '.txt')
        for _ in range(100):
            assert process.poll() is None, f'process exited {process.returncode}: {name}'
            if path.exists():
                value = path.read_text(encoding='gbk')
                if value and (expected is None or value == expected):
                    results.append({'check': name, 'value': value})
                    print('PASS', name, value, flush=True)
                    return value
            time.sleep(.05)
        raise AssertionError((name, expected, path.read_bytes() if path.exists() else 'missing'))

    def clear(*names):
        for name in names:
            (output / (name + '.txt')).unlink(missing_ok=True)

    def action(name):
        hwnd = next(h for h, c, t in rows if c == 'Button' and t == name)
        u.PostMessageW(hwnd, 0xF5, 0, 0)

    def mouse(hwnd, x, y, button=0):
        point = (y << 16) | x
        u.PostMessageW(hwnd, 0x200, 0, point)
        u.PostMessageW(hwnd, 0x201 if button == 0 else 0x204, 1 if button == 0 else 2, point)
        u.PostMessageW(hwnd, 0x202 if button == 0 else 0x205, 0, point)

    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(output / 'unique.exe')], cwd=output, startupinfo=startup)
    try:
        expect('ready', '1')
        u.EnumWindows(top, 0)
        (output / 'windows.json').write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding='utf-8')
        main = next(h for h, c, t in rows if c == 'WTWindow')
        tab = next(h for h, c, t in rows if c == 'SysTabControl32')
        advanced = next(h for h, c, t in rows if c == 'CPageControl')
        listing = next(h for h, c, t in rows if c == 'ListBox')
        combo = next(h for h, c, t in rows if c == 'ComboBox')
        slider = next(h for h, c, t in rows if c == 'msctls_trackbar32')
        animation = next(h for h, c, t in rows if c.startswith('Afx:') and u.GetParent(h) == main)
        object_id = expect('object-id')
        assert int(object_id) > 0
        action('feedback')
        expect('feedback-args', '37,9')
        expect('feedback-result', '379')
        clear('list-select')
        mouse(listing, 20, 22)
        expect('list-select', '1')
        u.PostMessageW(listing, 0x203, 1, (22 << 16) | 20)
        expect('list-double', '1')
        u.SendMessageW(combo, 0x14F, 1, 0)
        expect('combo-open', '1')
        combo_list = next(h for h, c, t in rows if c == 'ComboLBox')
        mouse(combo_list, 20, 22)
        expect('combo-select')
        u.SendMessageW(combo, 0x14F, 0, 0)
        expect('combo-close', '1')
        clear('tab-changing', 'tab-changed')
        mouse(tab, 70, 10)
        expect('tab-changing', '1')
        action('query')
        assert expect('state').split(',')[0] == '0'
        assert not (output / 'tab-changed.txt').exists()
        clear('advanced-changing', 'advanced-changed', 'state')
        mouse(advanced, 95, 10)
        expect('advanced-changing', '1')
        action('query')
        assert expect('state').split(',')[1] == '0'
        assert not (output / 'advanced-changed.txt').exists()
        action('allow')
        expect('allowed', '1')
        mouse(tab, 70, 10)
        expect('tab-changed', '1')
        mouse(advanced, 95, 10)
        expect('advanced-changed', '1')
        expect('advanced-click', '1')
        mouse(advanced, 95, 10, button=1)
        expect('advanced-right', '1')
        clear('animation-mouse')
        u.PostMessageW(animation, 0x200, 0, (29 << 16) | 17)
        expect('animation-mouse', '17,29,0')
        clear('animation-changing', 'animation-changed')
        action('deny')
        expect('denied', '1')
        action('move')
        expect('move-done', '1')
        expect('animation-changing', object_id + ',42,53')
        clear('state')
        action('query')
        assert expect('state').split(',')[2:] == ['10', '20']
        assert not (output / 'animation-changed.txt').exists()
        clear('allowed', 'move-done')
        action('allow')
        expect('allowed', '1')
        action('move')
        expect('animation-changed', object_id + ',42,53')
        clear('state')
        action('query')
        assert expect('state').split(',')[2:] == ['42', '53']
        action('destroy')
        expect('animation-destroy', object_id)
        clear('slider')
        u.PostMessageW(slider, 0x100, 0x27, 0)
        expect('slider')
        date = next(h for h, c, t in rows if c == 'SysDateTimePick32')
        mouse(date, 20, 10)
        u.PostMessageW(date, 0x100, 0x26, 0)
        expect('date')
        spin = next(h for h, c, t in rows if c == 'msctls_updown32')
        u.SendMessageW(spin, 0x46F, 0, 100)
        u.SendMessageW(spin, 0x471, 0, 50)
        u.ShowWindow(main, 4)
        assert u.SetWindowPos(main, -1, 0, 0, 0, 0, 0x13)
        point = W.POINT(5, 5)
        u.ClientToScreen(spin, C.byref(point))
        u.SetCursorPos(point.x, point.y)
        u.PostMessageW(spin, 0x201, 1, (5 << 16) | 5)
        time.sleep(.15)
        u.PostMessageW(spin, 0x202, 0, (5 << 16) | 5)
        expect('spin', '1')
        # 支持库用 WindowFromPoint 排除被遮挡窗口；必须让控件真正位于鼠标下。
        for index, x in [(0, 25), (1, 95), (0, 25)]:
            clear('advanced-hover')
            point = W.POINT(x, 10)
            u.ClientToScreen(advanced, C.byref(point))
            u.SetCursorPos(point.x, point.y)
            assert u.WindowFromPoint(point) == advanced, 'advanced tab is obscured'
            u.PostMessageW(advanced, 0x200, 0, (10 << 16) | x)
            expect('advanced-hover', str(index))
        (output / 'hover-probe.json').write_text(json.dumps({
            'event': '子夹头被点燃', 'triggered': True, 'indices': [0, 1, 0]},
            ensure_ascii=False, indent=2), encoding='utf-8')
        u.PostMessageW(main, 0x10, 0, 0)
        assert process.wait(timeout=10) == 0
    finally:
        u.SetCursorPos(cursor.x, cursor.y)
        (output / 'runtime-results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ide', required=True)
    parser.add_argument('--launcher', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    bindings = prepare(repo, output)
    run([args.launcher, 'headless-compile', args.ide, output / 'unique.e', output / 'unique.exe',
         '--result', output / 'ide.json', '--timeout', '60'], output / 'ide.log')
    result = json.loads((output / 'ide.json').read_text(encoding='utf-8-sig'))
    assert result['compile_result']['artifact_verified']
    results = verify(output)
    (output / 'report.json').write_text(json.dumps({'bindings': bindings, 'runtime': results,
        'hover_probe': json.loads((output / 'hover-probe.json').read_text(encoding='utf-8'))},
        ensure_ascii=False, indent=2), encoding='utf-8')
    print('PASS unique events:', len(bindings), 'bindings,', len(results), 'runtime assertions')


if __name__ == '__main__':
    main()
