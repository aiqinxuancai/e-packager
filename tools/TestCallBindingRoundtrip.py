"""验证 IDE 裸调用显示、编辑后原生调用身份及绑定信息缺失时的失败行为。"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def read(path):
    return path.read_bytes().decode('utf-8-sig')


def write(path, text):
    path.write_bytes(text.encode('utf-8-sig'))


def methods(work):
    pages = json.loads(read(work / 'project/.native_source_map.json'))
    return {item['name']: base64.b64decode(item['expressionData'])
            for page in pages for item in page['methods']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--e-dir', type=Path, required=True)
    parser.add_argument('--architectures', nargs='+', default=['Win32', 'x64'])
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    args.output.mkdir(parents=True, exist_ok=False)
    original = hashlib.sha256(args.source.read_bytes()).hexdigest()
    results = []
    for arch in args.architectures:
        tool = repo / f'bin/{arch}/Release/e-packager.exe'
        root = args.output / arch
        root.mkdir()

        def run(*parts, error=None):
            command = [str(tool), *map(str, parts), '--e-dir', str(args.e_dir)]
            proc = subprocess.run(command, capture_output=True, timeout=120)
            output = (proc.stdout + proc.stderr).decode('utf-8', errors='replace')
            results.append(dict(command=command, exit=proc.returncode, output=output))
            write(args.output / 'results.json', json.dumps(results, ensure_ascii=False, indent=2))
            if error:
                assert proc.returncode != 0 and error in output, results[-1]
            else:
                assert proc.returncode == 0, results[-1]

        work = root / 'workspace'
        run('unpack', args.source, work, '--main-only')
        for name in ('同名类', '派生类'):
            assert 'krnln::取运行目录' not in read(work / f'src/{name}.txt')
        run('pack', work, root / 'untouched.e')
        assert args.source.read_bytes() == (root / 'untouched.e').read_bytes()
        before = methods(work)
        for iteration, (old, new) in enumerate([('xx', 'yy'), ('yy', 'zz')]):
            for name in ('同名类', '派生类', '程序集1'):
                page = work / f'src/{name}.txt'
                text = read(page).replace(f'\\{old}\\', f'\\{new}\\')
                write(page, text)
            run('update', work)
            packed = root / f'edited-{iteration}.e'
            run('pack', work, packed)
            work = root / f'reopened-{iteration}'
            run('unpack', packed, work, '--main-only')
            after = methods(work)
            for name in ('本类核心目录', '派生核心目录', '_启动子程序'):
                assert before[name].replace(f'\\{old}\\'.encode(), f'\\{new}\\'.encode()) == after[name], name
            before = after

        # 缺失绑定文件、页记录或格式标记均不得静默退回裸名称解析。
        for case in ('file', 'entry', 'marker'):
            target = root / ('missing-' + case)
            shutil.copytree(work, target)
            bindings = target / 'project/.call_bindings.json'
            if case == 'file':
                bindings.unlink()
                error = 'read_call_bindings_failed'
            elif case == 'entry':
                data = json.loads(read(bindings))
                del data['pages'][next(iter(data['pages']))]
                write(bindings, json.dumps(data, ensure_ascii=False))
                error = 'inconsistent_page_call_bindings'
            else:
                meta = target / 'project/_meta.json'
                data = json.loads(read(meta))
                del data['programCallFormat']
                write(meta, json.dumps(data, ensure_ascii=False))
                error = 'unsupported_program_call_format'
            run('pack', target, root / (case + '.e'), error=error)

        # 方法改名仍须保留原生调用；只比较表达式数据，不比较会变化的方法 ID。
        renamed = root / 'renamed'
        shutil.copytree(work, renamed)
        page = renamed / 'src/同名类.txt'
        write(page, read(page).replace('本类核心目录', '改名核心目录'))
        run('pack', renamed, root / 'renamed.e')
        run('unpack', root / 'renamed.e', root / 'renamed-open', '--main-only')
        assert methods(root / 'renamed-open')['改名核心目录'] == before['本类核心目录']

        # 新复制的裸调用没有来源，要求用户明确目标。
        duplicate = root / 'duplicate'
        shutil.copytree(work, duplicate)
        page = duplicate / 'src/同名类.txt'
        write(page, read(page) + '\r\n返回 (取运行目录 () ＋ “\\zz\\”)\r\n')
        run('pack', duplicate, root / 'duplicate.e', error='unmatched_call_binding')
        text = read(page)
        at = text.rfind('返回 (取运行目录')
        write(page, text[:at] + text[at:].replace('取运行目录', 'krnln::取运行目录', 1))
        run('pack', duplicate, root / 'explicit.e')
        print(arch, 'PASS', flush=True)
    assert original == hashlib.sha256(args.source.read_bytes()).hexdigest()


if __name__ == '__main__':
    main()
