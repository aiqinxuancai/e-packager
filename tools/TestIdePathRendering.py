"""验证 IDE 路径文本、成员作用域和旧转义输入，防止回包改变命令身份。"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8-sig'))


def methods(workspace):
    data = json.loads((workspace / 'project/.native_source_map.json').read_text(encoding='utf-8-sig'))
    return {item['name']: base64.b64decode(item['expressionData'])
            for page in data for item in page['methods']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--e-dir', type=Path, required=True)
    parser.add_argument('--sample', type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    results = []

    def run(exe, *parts):
        command = [str(exe), *map(str, parts), '--e-dir', str(args.e_dir)]
        result = subprocess.run(command, cwd=args.e_dir, capture_output=True, timeout=120)
        results.append(dict(command=command, exit_code=result.returncode,
                            output=(result.stdout + result.stderr).decode('utf-8', errors='replace')))
        write(root / 'results.json', json.dumps(results, ensure_ascii=False, indent=2))
        assert result.returncode == 0, results[-1]

    decoder = repo / 'bin/Win32/Release/e-packager.exe'
    original_sample_hash = hashlib.sha256(args.sample.read_bytes()).hexdigest() if args.sample else None
    for arch in ('Win32', 'x64'):
        tool = repo / f'bin/{arch}/Release/e-packager.exe'
        work = root / arch
        work.mkdir()
        workspace = work / 'workspace'
        run(decoder, 'unpack', repo / 'eproj/e-console-exe-new-proj.e', workspace, '--main-only')
        source = '''.版本 2
.程序集 程序集1
.子程序 _启动子程序, 整数型
.局部变量 路径, 文本型
路径 ＝ 取运行目录 () ＋ “\\xx\\”
路径 ＝ “C:\\new\\test.txt”
路径 ＝ “\\n\\t”
路径 ＝ “#e2txt_text#C:\\\\legacy\\\\”
返回 (0)
'''
        write(workspace / 'src/程序集1.txt', source)
        write(workspace / 'src/同名类.txt', '''.版本 2
.程序集 同名类, <对象>, 公开
.子程序 取运行目录, 文本型, 公开
返回 (“成员方法”)
.子程序 本类核心目录, 文本型, 公开
返回 (krnln::取运行目录 () ＋ “\\xx\\”)
''')
        write(workspace / 'src/派生类.txt', '''.版本 2
.程序集 派生类, 同名类, 公开
.子程序 派生核心目录, 文本型, 公开
返回 (krnln::取运行目录 () ＋ “\\xx\\”)
''')
        packed = work / 'first.e'
        run(tool, 'pack', workspace, packed)
        decoded = work / 'decoded'
        run(tool, 'unpack', packed, decoded, '--main-only')
        expected = source.replace('“#e2txt_text#C:\\\\legacy\\\\”', '“C:\\legacy\\”')
        actual = (decoded / 'src/程序集1.txt').read_text(encoding='utf-8-sig')
        assert [s.strip() for s in actual.splitlines() if s.strip()] == [s.strip() for s in expected.splitlines() if s.strip()], actual
        # 其他类不能污染全局；当前类和继承链的真实冲突仍须保住原生调用。
        for page in ('同名类', '派生类'):
            text = (decoded / f'src/{page}.txt').read_text(encoding='utf-8-sig')
            assert '返回 (krnln::取运行目录 () ＋ “\\xx\\”)' in text, text
        before = methods(decoded)
        assert b'\\xx\\' in before['_启动子程序']
        for iteration in range(2):
            old_path, new_path = (('xx', 'yy'), ('yy', 'zz'))[iteration]
            for page in ('程序集1', '同名类', '派生类'):
                path = decoded / f'src/{page}.txt'
                text = path.read_text(encoding='utf-8-sig')
                # 修改真实字符串值，避免只改空白而复用原方法字节码。
                text = text.replace(f'\\{old_path}\\', f'\\{new_path}\\')
                write(path, text)
            rebuilt = work / f'rebuilt-{iteration}.e'
            run(tool, 'pack', decoded, rebuilt)
            decoded = work / f'reopened-{iteration}'
            run(tool, 'unpack', rebuilt, decoded, '--main-only')
            after = methods(decoded)
            assert before['_启动子程序'].replace(f'\\{old_path}\\'.encode(), f'\\{new_path}\\'.encode()) == after['_启动子程序'], (arch, iteration, 'path_or_call_identity_changed')
            for method in ('本类核心目录', '派生核心目录'):
                assert before[method].replace(f'\\{old_path}\\'.encode(), f'\\{new_path}\\'.encode()) == after[method], (arch, iteration, method)
            before = after

        if args.sample:
            sample_workspace = work / 'sample'
            run(tool, 'unpack', args.sample, sample_workspace, '--main-only')
            page = sample_workspace / 'src/窗口程序集_启动窗口.txt'
            text = page.read_text(encoding='utf-8-sig')
            assert 'a ＝ 读入文件 (取运行目录 () ＋ “\\xx\\”)' in text, text
            before = methods(sample_workspace)['__启动窗口_创建完毕']
            write(page, text.replace('\\xx\\', '\\yy\\'))
            rebuilt = work / 'sample-rebuilt.e'
            run(tool, 'pack', sample_workspace, rebuilt)
            reopened = work / 'sample-reopened'
            run(tool, 'unpack', rebuilt, reopened, '--main-only')
            after = methods(reopened)['__启动窗口_创建完毕']
            assert before.replace(b'\\xx\\', b'\\yy\\') == after, (arch, 'sample_native_identity_changed')
        print(f'{arch} PASS', flush=True)
    if args.sample:
        assert original_sample_hash == hashlib.sha256(args.sample.read_bytes()).hexdigest()


if __name__ == '__main__':
    main()
