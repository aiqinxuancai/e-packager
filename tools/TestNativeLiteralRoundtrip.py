"""验证原生多行字面量、真实前缀及旧目录兼容性，连续编辑后检查原生表达式。"""
import argparse
import base64
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--architectures', nargs='+', default=['Win32', 'x64'])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parents[1]
    results = []

    def write(path, text):
        path.write_bytes(text.encode('utf-8-sig'))

    def run(exe, *parts, ok=True):
        result = subprocess.run([str(exe), *map(str, parts)], capture_output=True, timeout=120)
        text = (result.stdout + result.stderr).decode('utf-8', errors='replace')
        results.append(dict(command=list(map(str, [exe, *parts])), exit=result.returncode, output=text))
        write(args.output / 'results.json', json.dumps(results, ensure_ascii=False, indent=2))
        if ok:
            assert result.returncode == 0, results[-1]
        return result, text

    def methods(root):
        data = json.loads((root / 'project/.native_source_map.json').read_text(encoding='utf-8-sig'))
        return {m['name']: base64.b64decode(m['expressionData']) for p in data for m in p['methods']}

    values = ['取八位十六进制末尾', '取按钮末尾', '首行\r\n末尾', '首行\n末尾', '首行\r末尾', '首行\t末尾', '"引号"末尾',
              '#e2txt_text#literal末尾', '#e2txt_long_text#literal末尾', r'C:\new\test\末尾', r'\n\t末尾']
    remark = '备注“#e2txt_text#不解码\\n”'
    source = '.版本 2\r\n.程序集 程序集1\r\n.子程序 _启动子程序, 整数型, , ' + remark + '\r\n.局部变量 文本, 文本型\r\n.局部变量 取按钮位, 文本型\r\n取按钮位 ＝ “字符边界”\r\n'
    source += '\r\n'.join('文本 ＝ “' + v + '”' for v in values) + '\r\n'
    source += "' " + remark + '\r\n返回 (0)\r\n'
    for arch in args.architectures:
        exe = repo / f'bin/{arch}/Release/e-packager.exe'
        root = args.output / arch
        root.mkdir()
        workspace = root / 'input'
        run(exe, 'unpack', repo / 'eproj/e-console-exe-new-proj.e', workspace, '--main-only')
        write(workspace / 'src/程序集1.txt', source)
        packed = root / 'initial.e'
        run(exe, 'pack', workspace, packed)
        opened = root / 'initial'
        run(exe, 'unpack', packed, opened, '--main-only')
        baseline = methods(opened)
        # 验证实际原生载荷，避免编解码双方以相同方式错误仍然通过。
        for value in values:
            assert value.encode('gbk') in baseline['_启动子程序'], (arch, repr(value))
        identical = root / 'unchanged.e'
        run(exe, 'pack', opened, identical)
        assert identical.read_bytes() == packed.read_bytes(), arch
        for iteration, (old, new) in enumerate([('末尾', '结束'), ('结束', '完成')]):
            page = opened / 'src/程序集1.txt'
            text = page.read_bytes().decode('utf-8-sig')
            for value in values:
                assert '“' + value + '”' in text, (arch, repr(value), repr(text))
            assert remark in text
            write(page, text.replace(old, new))
            rebuilt = root / f'edit-{iteration}.e'
            run(exe, 'pack', opened, rebuilt)
            opened = root / f'reopened-{iteration}'
            run(exe, 'unpack', rebuilt, opened, '--main-only')
            after = methods(opened)
            assert baseline['_启动子程序'].replace(old.encode('gbk'), new.encode('gbk')) == after['_启动子程序'], arch
            baseline = after
            values = [v.replace(old, new) for v in values]
        # 对真实文件物理行报告错误：多个跨行字面量后插入位置不合法的参数。
        page = opened / 'src/程序集1.txt'
        text = page.read_bytes().decode('utf-8-sig') + '.参数 错位参数, 整数型\r\n'
        expected_line = len(text[:text.index('.参数 错位参数')].replace('\r\n', '\n').replace('\r', '\n').split('\n'))
        write(page, text)
        result, diagnostic = run(exe, 'validate', opened, '--level', 'editable', '--diagnostics', 'json', ok=False)
        assert result.returncode != 0, diagnostic
        data = json.loads(next(line for line in diagnostic.splitlines() if line.startswith('{')))
        errors = [d for d in data['diagnostics'] if d.get('code') == 'parameter_misplaced']
        assert any(d.get('line') == expected_line for d in errors), (expected_line, data)
        values = [v.replace('完成', '末尾') for v in values]
        print(arch, 'PASS', flush=True)


if __name__ == '__main__':
    main()

