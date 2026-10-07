"""窗口对象与变量同名、隐式窗口成员及跨窗口对象访问的无头编译回归。"""
import argparse
import json
from pathlib import Path
import re
import shutil
import xml.etree.ElementTree as E

from TestEComUpdateRoundTrip import run, unpack, write, read_json, source_text
from TestEComHeadlessRoundTrip import compile_project
from TestMenuPropertyRoundTrip import check_runtime


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
    baseline = root / 'baseline'
    unpack(tool, repo / 'eproj/e-window-exe-new-proj.e', baseline)
    owner = '窗口程序集_启动窗口'
    xml = baseline / 'src/_启动窗口.xml'
    form = E.parse(xml).getroot()
    form.set('标题', 'menu-property-fail')
    E.SubElement(form, '窗口.事件', {'名称': '创建完毕', '处理器': owner + '::验证'})
    menus = E.SubElement(form, '窗口.菜单')
    E.SubElement(menus, '菜单', {'名称': '初始化', '标题': '初始化'})
    E.SubElement(menus, '菜单', {'名称': '填充空号', '标题': '填充空号'})
    E.SubElement(form, '按钮', {'名称': '打印按钮', '标题': '原始',
                             '左边': '10', '顶边': '10', '宽度': '80', '高度': '25'})
    write(xml, E.tostring(form, encoding='unicode'))
    meta = baseline / 'project/_meta.json'
    data = read_json(meta)
    data['windowBindings'] = [{'className': owner, 'formName': '_启动窗口'}]
    write(meta, json.dumps(data, ensure_ascii=False))
    write(baseline / 'src/.全局变量.txt', '.版本 2\n.全局变量 初始化, 字节型\n')
    cases = [
        ('global-menu', '', '初始化 ＝ 1', '初始化 ＝ 1', ''),
        ('local-global-menu', '.局部变量 初始化, 整数型', '初始化 ＝ 2', '初始化 ＝ 2', ''),
        ('chinese-boundary', '', '打印按钮.标题 ＝ “打 印(&P)”', '打印按钮.标题 ＝ “打 印(&P)”', ''),
        ('implicit-method', '', '', '取用户区高度 () ＞ 0', ''),
        ('implicit-property', '', '可视 ＝ 真', '可视 ＝ 真', ''),
        ('qualified-control', '', '跨程序集修改 ()', '打印按钮.标题 ＝ “跨窗口”',
         '.版本 2\n.程序集 其他程序集\n.子程序 跨程序集修改\n_启动窗口.打印按钮.标题 ＝ “跨窗口”\n'),
        ('qualified-menu', '', '跨程序集修改 ()', '填充空号.选中 ＝ 真',
         '.版本 2\n.程序集 其他程序集\n.子程序 跨程序集修改\n_启动窗口.填充空号.选中 ＝ 真\n'),
    ]
    results = []
    for name, declaration, code, condition, extra in cases:
        workspace = root / name
        shutil.copytree(baseline, workspace)
        source = (f'.版本 2\n.程序集 {owner}\n.子程序 验证\n{declaration}\n{code}\n'
                  f'.如果真 ({condition})\n    _启动窗口.标题 ＝ “menu-property-pass”\n.如果真结束\n')
        write(workspace / f'src/{owner}.txt', source)
        if extra:
            write(workspace / 'src/其他程序集.txt', extra)
        packed = root / f'{name}.e'
        run(tool, 'pack', workspace, packed)
        compile_project(args.launcher, args.ide, packed, root / f'{name}.exe', 'win_exe')
        check_runtime(root / f'{name}.exe')
        reopened = root / f'{name}-reopened'
        unpack(tool, packed, reopened)
        actual = source_text(reopened)
        assert not re.search(r'_Lib\d', actual), actual
        assert code in actual and condition in actual, actual
        results.append({'case': name, 'headless': True, 'runtime': True, 'roundtrip': True})
    # 仍需拒绝真实类型错误，不能通过放宽整个赋值校验来掩盖名称解析问题。
    invalid = root / 'invalid'
    shutil.copytree(root / 'global-menu', invalid)
    source_path = invalid / f'src/{owner}.txt'
    write(source_path, source_path.read_text(encoding='utf-8-sig').replace('初始化 ＝ 1', '初始化 ＝ “错误”', 1))
    result, log = run(tool, 'pack', invalid, root / 'invalid.e', succeeds=False)
    assert result.returncode != 0 and 'assignment_type_mismatch' in log
    write(root / 'results.json', json.dumps(results, ensure_ascii=False, indent=2))
    print(f'PASS {len(results)} independent headless/runtime tests and invalid assignment rejection', flush=True)


if __name__ == '__main__':
    main()
