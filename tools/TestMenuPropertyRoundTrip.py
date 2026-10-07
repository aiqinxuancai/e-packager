"""验证菜单属性逐项修改、语义回包、再次解包和真实 IDE 无头编译。"""
import argparse
import ctypes as C
import json
from pathlib import Path
import re
import subprocess
import time
import xml.etree.ElementTree as E

from TestEComUpdateRoundTrip import run, unpack, write, read_json, source_text
from TestEComHeadlessRoundTrip import compile_project


def check_runtime(artifact):
    user32 = C.windll.user32
    callback_type = C.WINFUNCTYPE(C.c_bool, C.c_void_p, C.c_void_p)
    process = subprocess.Popen([str(artifact)])
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            titles = []

            @callback_type
            def inspect(hwnd, unused):
                pid = C.c_ulong()
                user32.GetWindowThreadProcessId(C.c_void_p(hwnd), C.byref(pid))
                if pid.value == process.pid:
                    text = C.create_unicode_buffer(256)
                    user32.GetWindowTextW(C.c_void_p(hwnd), text, len(text))
                    titles.append(text.value)
                return True

            user32.EnumWindows(inspect, 0)
            if 'menu-property-pass' in titles:
                return
            assert process.poll() is None, f'Unexpected exit: {artifact}'
            time.sleep(0.05)
        raise AssertionError(f'Property runtime check failed: {artifact}')
    finally:
        if process.poll() is None:
            process.terminate()
        process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ide', type=Path, required=True)
    parser.add_argument('--launcher', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    tool = repo / 'bin/Win32/Release/e-packager.exe'
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    workspace = root / 'workspace'
    unpack(tool, repo / 'eproj/e-window-exe-new-proj.e', workspace)
    owner = '窗口程序集_启动窗口'
    xml = workspace / 'src/_启动窗口.xml'
    tree = E.parse(xml)
    form = tree.getroot()
    form.set('标题', 'menu-property-fail')
    E.SubElement(form, '窗口.事件', {'名称': '创建完毕', '处理器': owner + '::验证'})
    menu_root = E.SubElement(form, '窗口.菜单')
    E.SubElement(menu_root, '菜单', {'名称': '填充空号', '标题': '菜单'})
    write(xml, E.tostring(form, encoding='unicode'))
    meta = workspace / 'project/_meta.json'
    data = read_json(meta)
    data['windowBindings'] = [{'className': owner, 'formName': '_启动窗口'}]
    write(meta, json.dumps(data, ensure_ascii=False, indent=2))
    results = []
    cases = [('标题', '“初始标题”'), ('标题', '“修改标题”')]
    cases += [(name, value) for name in ('选中', '禁止', '可视') for value in ('真', '假')]
    for index, (name, value) in enumerate(cases):
        # 连续使用上一轮解包结果，确保新旧内部 ID 都可正确重建。
        expression = f'填充空号.{name}'
        source = (f'.版本 2\n.程序集 {owner}\n.子程序 验证\n'
                  f'{expression} ＝ {value}\n'
                  f'.如果真 ({expression} ＝ {value})\n'
                  '    _启动窗口.标题 ＝ “menu-property-pass”\n.如果真结束\n')
        write(workspace / f'src/{owner}.txt', source)
        packed = root / f'case-{index}.e'
        run(tool, 'pack', workspace, packed)
        artifact = root / f'case-{index}.exe'
        compile_project(args.launcher, args.ide, packed, artifact, 'win_exe')
        check_runtime(artifact)
        reopened = root / f'reopened-{index}'
        unpack(tool, packed, reopened)
        actual = source_text(reopened)
        assert not re.search(r'_Lib\d', actual), actual
        assert f'{expression} ＝ {value}' in actual, actual
        exported = (reopened / 'elib/系统核心支持库.txt').read_text(encoding='utf-8-sig')
        menu = exported.split('.数据类型 菜单,', 1)[1].split('.数据类型 ', 1)[0]
        for property_name in ('标题', '选中', '禁止', '可视'):
            assert '.成员 ' + property_name + ',' in menu
        results.append({'property': name, 'value': value, 'headless': True,
                        'runtime': True, 'no_placeholders': True})
        workspace = reopened
    # x64 无法执行 x86 窗口属性接口；用无窗口工程验证导出元数据的成员解析。
    console = root / 'console-workspace'
    unpack(tool, repo / 'eproj/e-console-exe-new-proj.e', console)
    x64 = repo / 'bin/x64/Release/e-packager.exe'
    for index, (name, value) in enumerate(cases[::2]):
        source = ('.版本 2\n.程序集 程序集1\n'
                  '.子程序 _启动子程序, 整数型\n返回 (0)\n'
                  '.子程序 修改菜单\n.参数 项目, 菜单\n'
                  f'项目.{name} ＝ {value}\n')
        write(console / 'src/程序集1.txt', source)
        packed = root / f'x64-{index}.e'
        run(x64, 'pack', console, packed)
        artifact = root / f'x64-{index}.exe'
        compile_project(args.launcher, args.ide, packed, artifact, 'win_console_exe')
        reopened = root / f'x64-reopened-{index}'
        unpack(tool, packed, reopened)
        actual = source_text(reopened)
        assert not re.search(r'_Lib\d', actual), actual
        assert f'项目.{name} ＝ {value}' in actual, actual
        results.append({'property': name, 'value': value, 'packer': 'x64',
                        'headless': True, 'no_placeholders': True})
    write(root / 'results.json', json.dumps(results, ensure_ascii=False, indent=2))
    print(f'PASS {len(results)} independent headless tests (8 runtime checks): {root}', flush=True)


if __name__ == '__main__':
    main()
