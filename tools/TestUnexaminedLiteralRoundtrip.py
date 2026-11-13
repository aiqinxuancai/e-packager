"""验证未解析语句中的路径、转义文本和前缀字面量在连续语义回包后不改变。"""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bin-root', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parents[1]
    binaries = args.bin_root or repo / 'bin'
    decoder = binaries / 'Win32/Release/e-packager.exe'
    results = []

    def write(path, value):
        path.write_bytes(value.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8-sig'))

    def run(exe, *arguments):
        command = [str(exe), *map(str, arguments)]
        result = subprocess.run(command, capture_output=True, timeout=120)
        results.append(dict(command=command, exit=result.returncode,
                            output=(result.stdout + result.stderr).decode('utf-8', errors='replace')))
        write(args.output / 'results.json', json.dumps(results, ensure_ascii=False, indent=2))
        assert result.returncode == 0, results[-1]

    lines = [
        r'不存在的命令 (“#e2txt_text#C:\\data\\index.html”)',
        r'不存在的命令 (“#e2txt_text#第一行\r\n第二行\t末尾”)',
        r'不存在的命令 (“#e2txt_text##e2txt_text#literal”)',
        r'不存在的命令 (“#e2txt_text#\x22引号\x22”)',
        r'.如果真 (不存在的条件 (“#e2txt_text#C:\\path”))',
        '    不存在的命令 (“普通内容”)',
        '.如果真结束',
    ]
    # 普通路径使用 IDE 原文；控制字符和历史前缀仍须无损传输。
    expected_lines = [
        line.replace(r'“#e2txt_text#C:\\data\\index.html”', r'“C:\data\index.html”')
            .replace(r'“#e2txt_text#C:\\path”', r'“C:\path”')
        for line in lines
    ]
    for arch in ['Win32', 'x64']:
        exe = binaries / f'{arch}/Release/e-packager.exe'
        root = args.output / arch
        root.mkdir()
        workspace = root / 'workspace'
        run(decoder, 'unpack', repo / 'eproj/e-console-exe-new-proj.e', workspace, '--main-only')
        page = 'src/程序集1.txt'
        write(workspace / page, '.版本 2\n.程序集 程序集1\n.子程序 _启动子程序, 整数型\n' + '\n'.join(lines) + '\n返回 (0)\n')
        for iteration in range(2):
            target = workspace / page
            write(target, target.read_text(encoding='utf-8-sig') + f"\n' semantic-literal-{iteration}\n")
            packed = root / f'{iteration}.e'
            run(exe, 'pack', workspace, packed)
            workspace = root / f'decoded-{iteration}'
            run(decoder, 'unpack', packed, workspace, '--main-only')
            actual = (workspace / page).read_text(encoding='utf-8-sig')
            for expected in expected_lines:
                assert expected in actual, (arch, iteration, expected, actual)
            assert f'semantic-literal-{iteration}' in actual
        print(arch, 'PASS', flush=True)


if __name__ == '__main__':
    main()
