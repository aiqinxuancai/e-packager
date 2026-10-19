"""验证常量文本与资源索引独立排序；仅操作临时工程副本。"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def write_text(path, text):
    path.write_bytes(b'\xef\xbb\xbf' + text.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8'))


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def write_json(path, value):
    write_text(path, json.dumps(value, ensure_ascii=False, indent=2))


def run(exe, *args):
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True, timeout=120)
    if result.returncode:
        raise RuntimeError(f'{exe} {args}: {result.stdout!r}\n{result.stderr!r}')


def constants(work, names):
    values = {'AAA': '1', 'BBB': '2', 'CCC': '3', 'RENAMED': '2', 'NEW': '4', '': ''}
    write_text(work / 'src/.常量.txt', '.版本 2\n\n' + ''.join(
        f'.常量 {name}, "{values[name]}"\n' for name in names))


def constant_lines(work):
    return [line for line in (work / 'src/.常量.txt').read_text(encoding='utf-8-sig').splitlines()
            if line.startswith('.常量 ')]


def sequence(work):
    return [key for key in read_json(work / 'project/_meta.json')['rootChildKeys']
            if key.startswith(('constant:', 'image:', 'sound:'))]


def resources(work):
    result = []
    for folder in ['image', 'audio']:
        for item in read_json(work / folder / 'list.json')['items']:
            result.append((item['order'], item['key'], item['logicalName'], item['isPublic'],
                           item['comment'], (work / item['relativePath']).read_bytes()))
    return [item[1:] for item in sorted(result, key=lambda item: item[0])]


def identities(work):
    return {(item['pageType'], item['name']): item['id']
            for item in read_json(work / 'project/.native_symbol_map.json')['constants'] if item['name']}


def prepare(exe, fixture, root):
    copied = root / 'fixture.e'
    shutil.copy2(fixture, copied)
    work = root / 'seed-input'
    run(exe, 'unpack', copied, work, '--main-only')
    constants(work, ['AAA', 'BBB', 'CCC'])
    payloads = [('image', 'PIC1'), ('audio', 'SND1'), ('image', 'PIC2'), ('audio', 'SND2')]
    for index, (kind, name) in enumerate(payloads):
        data = root / f'{name}.bin'
        data.write_bytes(bytes([index + 1]) * (index + 2))
        run(exe, 'update', work, f'--add-{kind}', f'{name}={data}')
    # 构造真实原生样例：资源分散在不同常量之间，包含首部和相邻资源。
    meta_path = work / 'project/_meta.json'
    meta = read_json(meta_path)
    expected = ['image:PIC1', 'constant:AAA', 'sound:SND1', 'constant:BBB',
                'image:PIC2', 'sound:SND2', 'constant:CCC']
    meta['rootChildKeys'] = [key for key in meta['rootChildKeys']
                             if not key.startswith(('constant:', 'image:', 'sound:'))] + expected
    write_json(meta_path, meta)
    output = root / 'seed.e'
    run(exe, 'pack', work, output)
    seed = root / 'seed'
    run(exe, 'unpack', output, seed, '--main-only')
    assert sequence(seed) == expected, sequence(seed)
    return seed, output


def verify(exe, seed, seed_file, root):
    cases = {
        'unchanged': None,
        'reorder': ['BBB', 'AAA', 'CCC'],
        'reverse': ['CCC', 'BBB', 'AAA'],
        'insert': ['AAA', 'NEW', 'BBB', 'CCC'],
        'rename': ['AAA', 'RENAMED', 'CCC'],
        'delete': ['AAA', 'CCC'],
        'resources_only': [],
        'anonymous': ['AAA', '', 'BBB', '', 'CCC'],
        'resource_reorder': ['BBB', 'AAA', 'CCC'],
        'resource_add': ['BBB', 'AAA', 'CCC'],
        'resource_delete': ['BBB', 'AAA', 'CCC'],
        'constants_only': ['BBB', 'AAA', 'CCC'],
        'references': ['BBB', 'AAA', 'CCC'],
    }
    results = []
    before_ids = identities(seed)
    for case, names in cases.items():
        work = root / case
        shutil.copytree(seed, work)
        if names is not None:
            constants(work, names)
        if case == 'resource_reorder':
            # 同时调整资源索引的 order，验证语义重建尊重资源编辑且不受旧槽位逆序影响。
            desired = {'image:PIC2': 0, 'sound:SND2': 1, 'image:PIC1': 2, 'sound:SND1': 3}
            for folder in ['image', 'audio']:
                path = work / folder / 'list.json'
                data = read_json(path)
                for item in data['items']:
                    item['order'] = desired[item['key']]
                write_json(path, data)
        if case == 'resource_add':
            payload = root / 'new.bin'
            payload.write_bytes(b'new resource')
            run(exe, 'update', work, '--add-image', f'PIC3={payload}', '--add-audio', f'SND3={payload}')
        if case in ['resource_delete', 'constants_only']:
            run(exe, 'update', work, '--remove-image', 'PIC1', '--remove-audio', 'SND1')
        if case == 'constants_only':
            run(exe, 'update', work, '--remove-image', 'PIC2', '--remove-audio', 'SND2')
        if case == 'references':
            path = work / 'src/程序集1.txt'
            write_text(path, '.版本 2\n\n.程序集 程序集1\n\n.子程序 _启动子程序, 整数型\n'
                       '标准输出 (1, 到文本 (#AAA × 100 ＋ #BBB × 10 ＋ #CCC ＋ 取字节集长度 (#PIC1) '
                       '＋ 取字节集长度 (#SND1) ＋ 取字节集长度 (#PIC2) ＋ 取字节集长度 (#SND2)))\n返回 (0)\n')
        expected_resources = resources(work)
        output = root / f'{case}.e'
        after = root / f'{case}-out'
        run(exe, 'pack', work, output)
        run(exe, 'unpack', output, after, '--main-only')
        assert constant_lines(after) == constant_lines(work), case
        assert resources(after) == expected_resources, case
        after_ids = identities(after)
        for key in before_ids.keys() & after_ids.keys():
            assert before_ids[key] == after_ids[key], (case, key)
        if case == 'unchanged':
            assert output.read_bytes() == seed_file.read_bytes()
        if case == 'rename':
            expected = [key.replace('constant:BBB', 'constant:RENAMED') for key in sequence(seed)]
            assert sequence(after) == expected, sequence(after)
        # 再次往返应稳定，避免第二次回包才改变排序。
        second = root / f'{case}-second.e'
        run(exe, 'pack', after, second)
        assert output.read_bytes() == second.read_bytes(), case
        results.append({'case': case, 'sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
                        'sequence': sequence(after)})
        print(f'{exe.parent.parent.name}: {case} OK', flush=True)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, action='append', required=True)
    parser.add_argument('--fixture', type=Path, default=Path(__file__).resolve().parents[1] / 'eproj/e-console-exe-new-proj.e')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='constant-resource-order-'))
    root.mkdir(parents=True, exist_ok=True)
    seed, seed_file = prepare(args.exe[0].resolve(), args.fixture.resolve(), root)
    reports = []
    for index, executable in enumerate(args.exe):
        directory = root / f'build-{index}'
        directory.mkdir()
        reports.append(verify(executable.resolve(), seed, seed_file, directory))
    for report in reports[1:]:
        assert report == reports[0], 'Architecture outputs differ'
    write_json(root / 'results.json', {'ok': True, 'builds': reports})
    print(f'PASS: {len(reports)} builds, {len(reports[0])} cases each; evidence: {root}')


if __name__ == '__main__':
    main()
