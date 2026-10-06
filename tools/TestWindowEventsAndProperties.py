"""通过真实 IDE 编译并运行窗口工程；只在新测试目录中生成文件。"""
import argparse
import base64
import copy
import ctypes as C
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import time
import xml.etree.ElementTree as E


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ide', required=True)
    parser.add_argument('--launcher', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    tool = repo / 'bin/Win32/Release/e-packager.exe'
    original = repo / 'eproj/e-window-exe-full+otherFne.e'
    original_hash = hashlib.sha256(original.read_bytes()).hexdigest()
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    shutil.copy2(original, output / 'original.e')

    def run(arguments, log, expected_error=None):
        result = subprocess.run([str(v) for v in arguments], capture_output=True)
        (output / log).write_bytes(result.stdout + result.stderr)
        if expected_error is None:
            assert result.returncode == 0, f'{log}: exit {result.returncode}'
        else:
            assert result.returncode != 0, f'{log}: invalid input was accepted'
            assert expected_error.encode() in result.stdout + result.stderr, log
            assert not Path(arguments[-1]).exists(), 'failed pack wrote an output file'

    def pack(workspace, name, error=None):
        run([tool, 'pack', workspace, output / (name + '.e')], name + '.log', error)

    def unpack(source, workspace, name):
        run([tool, 'unpack', source, workspace, '--main-only'], name + '.log')

    def save(tree, path):
        E.indent(tree)
        text = E.tostring(tree.getroot(), encoding='unicode', xml_declaration=True)
        path.write_bytes(b'\xef\xbb\xbf' + text.replace('\n', '\r\n').encode())

    def write_source(workspace, lines):
        path = workspace / 'src/窗口程序集_启动窗口.txt'
        path.write_bytes(b'\xef\xbb\xbf' + '\r\n'.join(lines).encode())

    def compile_ide(name):
        result_path = output / (name + '-ide.json')
        run([args.launcher, 'headless-compile', args.ide, output / (name + '.e'),
             output / (name + '.exe'), '--result', result_path, '--timeout', '60'], name + '-ide.log')
        result = json.loads(result_path.read_text(encoding='utf-8-sig'))
        assert result['compile_result']['artifact_verified'], result_path

    baseline = output / 'baseline'
    unpack(output / 'original.e', baseline, 'unpack')
    pack(baseline, 'unchanged')
    assert (output / 'unchanged.e').read_bytes() == original.read_bytes()

    def fixture(name):
        workspace = output / name
        shutil.copytree(baseline, workspace)
        tree = E.parse(workspace / 'src/_启动窗口.xml')
        second = E.parse(workspace / 'src/窗口1.xml')
        for node in second.getroot():
            if node.tag in ('窗口.事件定义', '透明标签', '高级选择夹', '动画框', '状态条'):
                tree.getroot().append(copy.deepcopy(node))
        for node in tree.iter():
            for child in list(node):
                if child.tag.endswith('.事件'):
                    node.remove(child)
        return workspace, tree

    owner = '窗口程序集_启动窗口'
    prefix = ['.版本 2', '.支持库 iext', '.支持库 iext2', '.支持库 iext3', '.程序集 ' + owner]
    workspace, tree = fixture('all-events-workspace')
    catalog = {n.get('控件类型'): list(n) for n in tree.getroot().findall('窗口.事件定义')}
    types = set()
    lines = prefix.copy()
    count = 0
    for node in tree.iter():
        if '宽度' not in node.attrib or '名称' not in node.attrib or node.tag in types:
            continue
        types.add(node.tag)
        for event in catalog.get(node.tag, []):
            count += 1
            handler = '验证事件' + str(count)
            E.SubElement(node, node.tag + '.事件', {'名称': event.get('名称'), '处理器': owner + '::' + handler})
            result_type = event.get('返回类型')
            lines.append('.子程序 ' + handler + (', ' + result_type if result_type else ''))
            for index, parameter in enumerate(event):
                lines.append('.参数 参数' + str(index) + ', ' + parameter.get('类型') +
                             (', 参考' if parameter.get('参考') == '真' else ''))
            if result_type:
                lines.append('返回 (' + ('真' if result_type == '逻辑型' else '0') + ')')
            lines.append('')
    save(tree, workspace / 'src/_启动窗口.xml')
    write_source(workspace, lines)
    pack(workspace, 'all-events')
    compile_ide('all-events')
    unpack(output / 'all-events.e', output / 'all-events-reopened', 'all-events-reopen')
    actual = E.parse(output / 'all-events-reopened/src/_启动窗口.xml')
    assert sum(1 for n in actual.iter() if n.tag.endswith('.事件')) == count

    workspace, tree = fixture('runtime-workspace')
    root = tree.getroot()
    root.set('标题', 'event-ready')
    root.set('宽度', '900')
    root.set('高度', '650')
    controls = {n.get('名称'): n for n in tree.iter() if '宽度' in n.attrib and '名称' in n.attrib}
    lines = prefix.copy()
    bindings = [('_启动窗口', '创建完毕', 'event-ready', []),
                ('按钮1', '被单击', 'button-ok', []),
                ('按钮1', '按下某键', 'key-ok', ['键代码', '功能键状态']),
                ('编辑框1', '内容被改变', 'edit-ok', []),
                ('透明标签1', '被双击', 'third-ok', ['横向位置', '纵向位置', '功能键状态'])]
    for index, (name, event, result, parameters) in enumerate(bindings):
        node = controls[name]
        handler = '运行验证' + str(index)
        E.SubElement(node, node.tag + '.事件', {'名称': event, '处理器': owner + '::' + handler})
        lines.append('.子程序 ' + handler + (', 逻辑型' if parameters else ''))
        lines.extend('.参数 ' + p + ', 整数型' for p in parameters)
        lines.append('_启动窗口.标题 ＝ “' + result + '”')
        if parameters:
            lines.append('返回 (真)')
        lines.append('')
    menu_root = E.SubElement(root, '窗口.菜单')
    menu = E.SubElement(menu_root, '菜单', {'名称': '验证菜单', '标题': 'menu-probe'})
    item = E.SubElement(menu, '菜单', {'名称': '验证菜单项', '标题': 'menu-action'})
    E.SubElement(item, '菜单.事件', {'名称': '单击', '处理器': owner + '::菜单验证'})
    lines += ['.子程序 菜单验证', '_启动窗口.标题 ＝ “menu-ok”', '']
    controls['按钮1'].set('标题', 'button-probe')
    controls['透明标签1'].set('标题', 'third-probe')
    controls['透明标签1'].set('左边', '550')
    controls['透明标签1'].set('顶边', '500')
    for name in ['编辑框1', '透明标签1']:
        node = controls[name]
        node.set('文本颜色', '255')
        for child in list(node):
            if child.tag.endswith('.字体'):
                node.remove(child)
        E.SubElement(node, node.tag + '.字体', {'高度': '-24', '粗细': '700', '字符集': '134', '字体名': '宋体'})
    controls['日期框1'].set('今天', '45000')
    pixels = bytes([0, 0, 255] * 2 + [0, 0]) * 2
    bitmap = b'BM' + struct.pack('<IHHI', 54 + len(pixels), 0, 0, 54)
    bitmap += struct.pack('<IiiHHIIiiII', 40, 2, 2, 1, 24, 0, len(pixels), 0, 0, 0, 0) + pixels
    controls['图片框1'].set('图片', base64.b64encode(bitmap).decode())
    for node in tree.iter():
        if node.tag in ('列表框.列表项目', '组合框.列表项目'):
            for index, item in enumerate(node):
                item.set('文本', '修改项目' + str(index))
    xml_path = workspace / 'src/_启动窗口.xml'
    save(tree, xml_path)
    write_source(workspace, lines)
    pack(workspace, 'runtime')
    compile_ide('runtime')
    unpack(output / 'runtime.e', output / 'runtime-reopened', 'runtime-reopen')
    actual = E.parse(output / 'runtime-reopened/src/_启动窗口.xml')
    font = next(actual.iter('编辑框.字体'))
    assert font.get('高度') == '-24' and font.get('粗细') == '700' and font.get('字体名') == '宋体'
    picture = next(actual.iter('图片框'))
    assert base64.b64decode(picture.get('图片')) == bitmap
    assert next(actual.iter('日期框')).get('今天') == '45000'
    assert [n.get('文本') for group in actual.iter('列表框.列表项目') for n in group] == ['修改项目0', '修改项目1', '修改项目2']

    # 错误输入不得静默写成无效事件或未生效属性。
    good_xml = xml_path.read_bytes()
    good_source = (workspace / 'src/窗口程序集_启动窗口.txt').read_bytes()
    negative = [
        ('unknown-event', lambda: controls['按钮1'].find('按钮.事件').set('名称', '不存在的事件'), 'unknown_event'),
        ('wrong-index', lambda: controls['按钮1'].find('按钮.事件').set('索引', '-3'), 'name_index_mismatch'),
        ('wrong-owner', lambda: controls['按钮1'].find('按钮.事件').set('处理器', '不存在::运行验证1'), 'handler_not_found_or_ambiguous'),
        ('invalid-index', lambda: controls['按钮1'].find('按钮.事件').set('索引', '-100'), 'unknown_event'),
        ('duplicate', lambda: controls['按钮1'].append(copy.deepcopy(controls['按钮1'].find('按钮.事件'))), 'duplicate_event'),
        ('unknown-property', lambda: controls['按钮1'].set('错误属性', '1'), 'window_control_property_unknown'),
        ('invalid-value', lambda: controls['按钮1'].set('类型', '100000'), 'window_control_property_not_persisted'),
        ('readonly', lambda: controls['高级选择夹1'].set('页面区宽度', '999'), 'window_control_property_not_design_writable'),
    ]
    for name, mutate, expected in negative:
        mutate()
        save(tree, xml_path)
        pack(workspace, name, expected)
        xml_path.write_bytes(good_xml)
        tree = E.parse(xml_path)
        controls = {n.get('名称'): n for n in tree.iter() if '宽度' in n.attrib and '名称' in n.attrib}
    write_source(workspace, [s for s in lines if s != '.参数 功能键状态, 整数型'])
    pack(workspace, 'wrong-signature', 'handler_parameter_count_mismatch')
    (workspace / 'src/窗口程序集_启动窗口.txt').write_bytes(good_source)
    verify_runtime(output / 'runtime.exe', output)
    assert hashlib.sha256(original.read_bytes()).hexdigest() == original_hash
    report = {'event_bindings': count, 'control_types': len(types), 'negative_cases': len(negative) + 1,
              'runtime_events': ['button', 'edit', 'keyboard', 'third-party-double-click', 'menu'],
              'properties': ['font', 'color', 'bitmap', 'date', 'list-items'], 'original_unchanged': True}
    (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print('PASS', json.dumps(report), output)


def verify_runtime(executable, output):
    u = C.windll.user32
    u.SendMessageW.argtypes = [C.c_void_p, C.c_uint, C.c_size_t, C.c_ssize_t]
    u.SendMessageW.restype = C.c_ssize_t
    callback = C.WINFUNCTYPE(C.c_bool, C.c_void_p, C.c_void_p)
    rows = []

    def title(window):
        text = C.create_unicode_buffer(512)
        u.GetWindowTextW(window, text, len(text))
        return text.value

    @callback
    def child(window, unused):
        name = C.create_unicode_buffer(256)
        u.GetClassNameW(window, name, len(name))
        rows.append((window, name.value, title(window)))
        return True

    @callback
    def top(window, unused):
        pid = C.c_ulong()
        u.GetWindowThreadProcessId(window, C.byref(pid))
        if pid.value == process.pid:
            child(window, 0)
            u.EnumChildWindows(window, child, 0)
        return True

    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(executable)], cwd=executable.parent, startupinfo=startup)
    try:
        for _ in range(100):
            assert process.poll() is None, 'runtime exited during startup'
            rows.clear()
            u.EnumWindows(top, 0)
            if any(c == 'WTWindow' and t == 'event-ready' for h, c, t in rows) and any(c == 'SysDateTimePick32' for h, c, t in rows):
                break
            time.sleep(.1)
        main = next(h for h, c, t in rows if c == 'WTWindow')
        button = next(h for h, c, t in rows if t == 'button-probe')
        label = next(h for h, c, t in rows if t == 'third-probe')
        edit = next(h for h, c, t in rows if c == 'Edit' and u.GetParent(h) == main)

        def expect(expected):
            for _ in range(50):
                if title(main) == expected:
                    return
                assert process.poll() is None, 'runtime crashed'
                time.sleep(.05)
            raise AssertionError((expected, title(main)))

        u.PostMessageW(button, 0xF5, 0, 0)
        expect('button-ok')
        value = C.create_unicode_buffer('new text')
        u.SendMessageW(edit, 0xC, 0, C.addressof(value))
        expect('edit-ok')
        u.PostMessageW(button, 0x100, 0x77, 0)
        expect('key-ok')
        # 通用鼠标事件由消息循环过滤器分发，需排队消息而非直接 SendMessage。
        u.PostMessageW(label, 0x203, 1, (5 << 16) | 5)
        expect('third-ok')
        menu = u.GetMenu(main)
        submenu = u.GetSubMenu(menu, 0)
        command = u.GetMenuItemID(submenu, 0)
        assert menu and command != -1
        u.PostMessageW(main, 0x111, command, 0)
        expect('menu-ok')
        font = u.SendMessageW(edit, 0x31, 0, 0)
        gdi = C.windll.gdi32
        gdi.GetObjectW.argtypes = [C.c_void_p, C.c_int, C.c_void_p]
        data = C.create_string_buffer(92)
        assert gdi.GetObjectW(font, len(data), data) == 92
        assert struct.unpack_from('<i', data.raw)[0] == -24
        assert struct.unpack_from('<i', data.raw, 16)[0] == 700
        listbox = next(h for h, c, t in rows if c == 'ListBox')
        value = C.create_unicode_buffer(256)
        assert u.SendMessageW(listbox, 0x189, 0, C.addressof(value)) > 0
        assert value.value == '修改项目0'
        (output / 'runtime-verified.json').write_text(json.dumps({'final_title': title(main), 'font_height': -24,
             'font_weight': 700, 'first_list_item': value.value}, ensure_ascii=False, indent=2), encoding='utf-8')
        u.PostMessageW(main, 0x10, 0, 0)
        assert process.wait(timeout=10) == 0
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=10)


if __name__ == '__main__':
    main()
