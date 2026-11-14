"""以 EideCore 原生源码为独立基准，审计全部非空 .e 的双架构语义回包。"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
import difflib
import json
from pathlib import Path
import tempfile
import traceback

import TestModuleRoundtripAudit as rt
from TestIdePageParity import normalize


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def native(args, source, root, label):
    stage = rt.invoke([args.output / 'bin/EideCore/EideCore.exe', 'source',
                       '--project', source, '--e-dir', args.e_dir], root, label, args.timeout)
    result = {'stage': stage, 'valid': False}
    try:
        data = read_json(Path(stage['log']))
        rt.save(root / (label + '.json'), data)
        result['error'] = data.get('error')
        result['valid'] = (stage['status'] == 'ok' and data.get('ok') is True
                           and data.get('loaded') is True and data.get('core_code_intact') is True
                           and data.get('self_integrity_verified') is True
                           and data.get('exception_code') == 0 and data.get('top_level_windows') == 0
                           and data.get('blocked_ui_calls') == 0
                           and data.get('source', {}).get('format') == 'e_native_text')
        result['items'] = data.get('source', {}).get('items', [])
    except Exception:
        result['error'] = traceback.format_exc()
    return result


def exported(workspace):
    meta = read_json(workspace / 'project/_meta.json')
    pages = read_json(workspace / 'project/.native_source_map.json')
    symbols = read_json(workspace / 'project/.native_symbol_map.json')
    files = meta['sourceFiles']
    if len(files) != len(pages):
        raise ValueError('source file/native ID mapping count differs')
    result = {}
    for file, page in zip(files, pages):
        path = workspace / file['relativePath']
        result[('page', page['classId'])] = {'name': file['logicalName'], 'path': file['relativePath'],
                                           'text': path.read_bytes().decode('utf-8-sig')}
    for kind, key, filename, directive in (
        ('dll', 'dlls', '.DLL声明.txt', '.DLL命令'),
        ('data_type', 'structs', '.数据类型.txt', '.数据类型'),
        ('global', 'globals', '.全局变量.txt', '.全局变量'),
        ('constant', 'constants', '.常量.txt', '.常量')):
        path = workspace / 'src' / filename
        text = path.read_bytes().decode('utf-8-sig') if path.exists() else ''
        # 仅在声明行切分，跨行字符串中的行保持原样。
        chunks, current, quoted = [], [], False
        for line in text.replace('\r\n', '\n').split('\n'):
            if not quoted and line.startswith(directive + ' '):
                if current:
                    chunks.append('\n'.join(current))
                current = [line]
            elif current:
                current.append(line)
            for ch in line:
                if not quoted and ch == "'":
                    break
                if ch == '“' and not quoted:
                    quoted = True
                elif ch == '”' and quoted:
                    quoted = False
        if current:
            chunks.append('\n'.join(current))
        by_name = {}
        for chunk in chunks:
            name = chunk.split('\n', 1)[0][len(directive):].split(',', 1)[0].strip()
            by_name.setdefault(name, []).append(chunk)
        for symbol in symbols.get(key, []):
            choices = by_name.get(symbol['name'], [])
            if not choices:
                continue
            # 同名声明按原生元数据顺序匹配，保留每项原生 ID。
            result[(kind, symbol['id'])] = {'name': symbol['name'], 'path': 'src/' + filename,
                                           'text': choices.pop(0)}
        if any(by_name.values()):
            raise ValueError('fixed declaration lacks native ID: ' + kind)
    return result


def identity(item):
    return ('page' if item['kind'] in ('class', 'assembly') else item['kind'], item['id'])


def compare_items(expected, actual, root, label):
    rows, diffs = [], []
    for key, item in expected.items():
        other = actual.get(key)
        left = normalize(item['text'])
        right = normalize(other['text']) if other else ''
        equal = other is not None and left == right
        rows.append({'kind': key[0], 'id': key[1], 'name': item['name'], 'equal': equal,
                     'missing': other is None})
        if not equal:
            diffs.extend(difflib.unified_diff(left.split('\n'), right.split('\n'),
                         fromfile=f'native/{key}/{item["name"]}', tofile=f'export/{key}', lineterm=''))
    if diffs:
        (root / (label + '.diff')).write_bytes(('\n'.join(diffs) + '\n').encode('utf-8-sig'))
    result = {'total': len(rows), 'equal': sum(r['equal'] for r in rows),
              'differences': [r for r in rows if not r['equal']],
              'extra_items': [{'kind': k[0], 'id': k[1], 'name': v['name']} for k, v in actual.items() if k not in expected]}
    rt.save(root / (label + '.comparison.json'), result)
    return result


def native_items(result):
    return {identity(i): {'name': i['name'], 'text': i['source_text']} for i in result['items']}


def run_item(args, item):
    root = args.output / 'native' / f'{item["id"]:04}'
    root.mkdir(parents=True, exist_ok=True)
    source = args.output / 'inputs' / item['relative']
    original = native(args, source, root, 'original-source')
    summaries = []
    for arch in args.architectures:
        completed = args.output / 'cases' / f'{item["id"]:04}' / arch / 'source-audit.json'
        if completed.exists():
            summaries.append(read_json(completed))
            continue
        row = rt.audit(args, item, arch)
        case = Path(row['directory'])
        row['native_original'] = {k: v for k, v in original.items() if k != 'items'}
        try:
            if original['valid'] and (case / 'workspace/project/_meta.json').exists():
                actual = exported(case / 'workspace')
                for value in actual.values():
                    for page in row.get('modified_pages', []):
                        if value['path'] == page['path']:
                            value['text'] = value['text'].removesuffix(page['marker'] + '\r\n')
                expected = native_items(original)
                row['source_parity'] = compare_items(expected, actual, case, 'original-parity')
            if (case / 'edited.e').exists():
                with tempfile.NamedTemporaryFile(prefix='__eidecore_audit_', suffix='.e', dir=source.parent, delete=False) as stream:
                    staged = Path(stream.name)
                    stream.write((case / 'edited.e').read_bytes())
                try:
                    rebuilt = native(args, staged, case, 'rebuilt-source')
                finally:
                    staged.unlink()
                row['native_rebuilt'] = {k: v for k, v in rebuilt.items() if k != 'items'}
                if rebuilt['valid']:
                    current = native_items(rebuilt)
                    if (case / 'decoded/project/_meta.json').exists():
                        row['rebuilt_parity'] = compare_items(current, exported(case / 'decoded'), case, 'rebuilt-parity')
                    if original['valid']:
                        expected = native_items(original)
                        mapping = exported(case / 'workspace')
                        markers = {p['path']: p['marker'] for p in row.get('modified_pages', [])}
                        for key, value in expected.items():
                            marker = markers.get(mapping.get(key, {}).get('path'))
                            if marker:
                                value['text'] += '\n' + marker + '\n'
                        row['native_roundtrip'] = compare_items(expected, current, case, 'native-roundtrip')
        except Exception:
            row['native_audit_error'] = traceback.format_exc()
        row['fully_verified'] = (row['status'] == 'passed' and original['valid']
            and row.get('native_rebuilt', {}).get('valid', False)
            and all(k in row and row[k]['equal'] == row[k]['total'] for k in
                    ('source_parity', 'rebuilt_parity', 'native_roundtrip'))
            and not row.get('native_roundtrip', {}).get('extra_items'))
        rt.save(case / 'source-audit.json', row)
        summaries.append(row)
    return summaries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--e-dir', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=Path(r'C:\Users\aiqin\OneDrive\Code\模块'))
    parser.add_argument('--workers', type=int, default=8)
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--ids', type=int, nargs='*')
    args = parser.parse_args()
    args.bin_root = args.output / 'bin'
    args.architectures = ['Win32', 'x64']
    args.preserve_blank_lines = True
    manifest = read_json(args.output / 'manifest.json')
    inputs = [i for i in manifest if i['size'] and (not args.ids or i['id'] in args.ids)]
    for item in inputs:
        actual = rt.hashes(args.output / 'inputs' / item['relative'])
        assert actual['sha256'] == item['sha256']
        item['md5'] = actual['md5']
    rows = []
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        pending = {pool.submit(run_item, args, i): i for i in inputs}
        for future in as_completed(pending):
            item = pending[future]
            try:
                results = future.result()
                rows.extend(results)
                print(json.dumps({'done': len(rows), 'id': item['id'], 'file': item['relative'],
                    'roundtrip': [r['status'] for r in results],
                    'native': [r['fully_verified'] for r in results]}, ensure_ascii=False), flush=True)
            except Exception:
                rows.append({'id': item['id'], 'error': traceback.format_exc(), 'fully_verified': False})
            rt.save(args.output / 'source-audit-progress.json', rows)
    originals_unchanged = all(rt.hashes(args.source / i['relative'])['sha256'] == i['sha256'] for i in manifest)
    summary = {'inputs': len(inputs), 'runs': len(rows), 'empty': [i for i in manifest if not i['size']],
               'fully_verified': sum(r['fully_verified'] for r in rows),
               'originals_unchanged': originals_unchanged,
               'roundtrip_statuses': dict(Counter(r.get('status', 'error') for r in rows)),
               'issues': dict(Counter(i for r in rows for i in r.get('issues', [])))}
    rt.save(args.output / 'source-audit-summary.json', summary)
    print(json.dumps(summary, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
