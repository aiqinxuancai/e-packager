"""验证 issue #16：语义回包保留空白语句、空子程序及各类流程块的空行。"""
import argparse
import base64
import difflib
import json
from pathlib import Path
import subprocess


def write(path, text):
    path.write_bytes(text.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8-sig'))


def source_page():
    lines = ['.版本 2', '', '.程序集 程序集1', '',
             '.子程序 _启动子程序, 整数型', '', '返回 (0)', '']
    for count in (3, 4, 5, 8):
        lines += [f'.子程序 空子程序{count}'] + [''] * count
        lines += [f'.子程序 空事件{count}', '.参数 参数, 整数型',
                  '.局部变量 局部, 整数型'] + [''] * count
    for leading in (1, 2, 3, 4, 8):
        for trailing in range(1, 7):
            lines += [f'.子程序 体边界{leading}_{trailing}'] + [''] * leading
            lines += ["' 保留注释"] + [''] * trailing

    blocks = [
        ['.如果真 (真)', '.如果真结束'],
        ['.如果 (真)', '.否则', '.如果结束'],
        ['.判断循环首 (假)', '.判断循环尾 ()'],
        ['.循环判断首 ()', '.循环判断尾 (假)'],
        ['.计次循环首 (1, i)', '.计次循环尾 ()'],
        ['.变量循环首 (1, 2, 1, i)', '.变量循环尾 ()'],
        ['.判断开始 (真)', '.判断 (假)', '.默认', '.判断结束'],
    ]
    for kind, markers in enumerate(blocks):
        for count in range(4):
            lines += [f'.子程序 流程{kind}_{count}', '.局部变量 i, 整数型', '']
            for marker in markers[:-1]:
                lines += [marker] + [''] * count + ["    ' 块内注释"] + [''] * count
            lines += [markers[-1]] + [''] * (count + 1)
            # 同时覆盖嵌套块作为父块最后一条语句，以及块尾之间的空行。
            lines += [f'.子程序 嵌套{kind}_{count}', '.局部变量 i, 整数型', '',
                      '.判断循环首 (假)']
            for marker in markers[:-1]:
                lines += ['    ' + marker] + [''] * count + ["        ' 嵌套注释"]
            lines += ['    ' + markers[-1]] + [''] * count + ['.判断循环尾 ()', '']

    # 截图中的空默认分支和嵌套判断，保留块结束后的流程收敛空行。
    lines += ['.子程序 截图复现', '', '.判断开始 (真)', '    输出调试文本 (0)',
              '.默认', '', '.判断结束', '', '.判断循环首 (真)',
              '    .判断开始 (1 ＝ 0)', '        输出调试文本 (1)',
              '    .判断 (2 ＝ 1)', '        输出调试文本 (2)',
              '    .默认', '', '    .判断结束', '', '.判断循环尾 ()', '',
              '.子程序 最后空子程序', '', '', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bin-root', type=Path)
    parser.add_argument('--eide', type=Path)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parents[1]
    binaries = args.bin_root or repo / 'bin'
    evidence = []

    def run(exe, *parts):
        command = [str(exe), *map(str, parts)]
        result = subprocess.run(command, capture_output=True, timeout=180)
        evidence.append(dict(command=command, exit=result.returncode,
                             output=(result.stdout + result.stderr).decode('utf-8', 'replace')))
        write(args.output / 'results.json', json.dumps(evidence, ensure_ascii=False, indent=2))
        assert result.returncode == 0, evidence[-1]

    def check_source(expected, actual):
        assert actual == expected, '\n'.join(difflib.unified_diff(
            expected.splitlines(), actual.splitlines(), fromfile='expected', tofile='actual'))

    def methods(workspace):
        mapping = json.loads((workspace / 'project/.native_source_map.json').read_text(encoding='utf-8-sig'))
        return [method for page in mapping for method in page['methods']]

    blank_statement = bytes.fromhex('6a00000000ffff000000000000000000003601')
    for arch in ('Win32', 'x64'):
        exe = binaries / arch / 'Release/e-packager.exe'
        root = args.output / arch
        workspace = root / 'workspace'
        original = repo / 'eproj/e-console-exe-new-proj.e'
        run(exe, 'unpack', original, workspace, '--main-only')
        run(exe, 'pack', workspace, root / 'unchanged.e')
        assert original.read_bytes() == (root / 'unchanged.e').read_bytes()
        expected = source_page()
        page = 'src/程序集1.txt'
        write(workspace / page, expected)
        for iteration in range(3):
            if iteration:
                # 仅修改常量页也必须重建；第二轮修改注释，避免只验证快照复用。
                write(workspace / 'src/.常量.txt', f'.版本 2\n\n.常量 触发重建, {iteration}\n')
                if iteration == 2:
                    expected = expected.replace('保留注释', '修改注释')
                    write(workspace / page, expected)
            packed = root / f'round-{iteration}.e'
            run(exe, 'pack', workspace, packed)
            workspace = root / f'decoded-{iteration}'
            run(exe, 'unpack', packed, workspace, '--main-only')
            check_source(expected, (workspace / page).read_text(encoding='utf-8-sig'))
            for method in methods(workspace):
                if method['name'].startswith(('空子程序', '空事件', '最后空子程序')):
                    count = int(method['name'][-1]) - 2 if method['name'][-1].isdigit() else 1
                    assert base64.b64decode(method['expressionData']) == blank_statement * count, method
                    assert len(base64.b64decode(method['lineOffset'])) == count * 4, method
            if args.eide and iteration == 2:
                run(exe, 'compile-check', packed, '--eide', args.eide, '--compile-static')

        # 原生 IDE 样本包含空事件、类生命周期方法和多层循环，覆盖真实原生布局。
        workspace = root / 'native'
        run(exe, 'unpack', repo / 'eproj/e-window-exe-full.e', workspace, '--main-only')
        expected_pages = {p.name: p.read_text(encoding='utf-8-sig') for p in (workspace / 'src').glob('*.txt')
                          if not p.name.startswith('.')}
        native_methods = methods(workspace)
        empty_methods = {m['id']: m for m in native_methods
                         if base64.b64decode(m['expressionData']) == blank_statement}
        assert empty_methods, '原生样本必须含空子程序'
        constants = workspace / 'src/.常量.txt'
        write(constants, constants.read_text(encoding='utf-8-sig') + '\n.常量 触发重建, 1\n')
        run(exe, 'pack', workspace, root / 'native-edited.e')
        workspace = root / 'native-decoded'
        run(exe, 'unpack', root / 'native-edited.e', workspace, '--main-only')
        for name, expected in expected_pages.items():
            check_source(expected, (workspace / 'src' / name).read_text(encoding='utf-8-sig'))
        for method in methods(workspace):
            original_method = next(m for m in native_methods if m['id'] == method['id'])
            # 原生样本所有方法的行/块位置表均保持一致，验证流程线使用的结构数据。
            for field in ('lineOffset', 'blockOffset'):
                assert method[field] == original_method[field], (method['name'], field)
            if method['id'] in empty_methods:
                original_method = empty_methods[method['id']]
                for field in ('expressionData', 'lineOffset', 'blockOffset'):
                    assert method[field] == original_method[field], (method['name'], field)

        # 手写的紧凑布局需生成最少一个可编辑行；未检查语句也须保留真实空行。
        workspace = root / 'workspace'
        compact = ['.版本 2', '', '.程序集 程序集1', '',
                   '.子程序 _启动子程序, 整数型', '返回 (0)']
        canonical = compact[:5] + ['', '返回 (0)', '']
        for count in range(3):
            compact += [f'.子程序 手写空子程序{count}'] + [''] * count
            canonical += [f'.子程序 手写空子程序{count}'] + [''] * 3
        raw = ['.子程序 未检查流程', '', '', '.如果真 (不存在的条件)', '',
               '    不存在的命令 ()', '', '.如果真结束', '', '', '.子程序 文件末尾']
        write(workspace / page, '\n'.join(compact + raw))
        run(exe, 'pack', workspace, root / 'compact.e')
        workspace = root / 'compact-decoded'
        run(exe, 'unpack', root / 'compact.e', workspace, '--main-only')
        check_source('\n'.join(canonical + raw + ['', '', '']),
                     (workspace / page).read_text(encoding='utf-8-sig'))
        for method in methods(workspace):
            if method['name'].startswith('手写空子程序') or method['name'] == '文件末尾':
                assert base64.b64decode(method['expressionData']) == blank_statement, method
        print(f'PASS {arch}: blank-line matrix, three rebuilds, native empty methods and flow blocks', flush=True)


if __name__ == '__main__':
    main()
