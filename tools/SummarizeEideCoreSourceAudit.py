"""汇总原生源码审计，分别报告命令结果、独立源码差异和覆盖边界。"""
import argparse
from collections import Counter
import csv
import hashlib
import json
from pathlib import Path

from TestEideCoreSourceAudit import read_json, normalize
from TestModuleRoundtripAudit import save


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def content_integrity(case):
    result = {'compared': (case / 'decoded').is_dir(), 'differences': []}
    if not result['compared']:
        return result
    for folder in ('src', 'text', 'image', 'audio'):
        expected_root, actual_root = case / 'workspace' / folder, case / 'decoded' / folder
        expected = {p.relative_to(expected_root).as_posix(): p for p in expected_root.rglob('*') if p.is_file()}
        actual = {p.relative_to(actual_root).as_posix(): p for p in actual_root.rglob('*') if p.is_file()}
        for path in sorted(expected.keys() | actual.keys()):
            left, right = expected.get(path), actual.get(path)
            if left is None or right is None:
                equal = False
            elif path == 'list.json':
                equal = read_json(left) == read_json(right)
            else:
                equal = digest(left) == digest(right)
            if not equal:
                result['differences'].append({'folder': folder, 'path': path, 'missing': right is None,
                                               'added': left is None})
    return result


def source_scope(row):
    case = Path(row['directory'])
    path = case / 'workspace/project/.module.json'
    dependencies = read_json(path).get('dependencies', []) if path.exists() else []
    extras = []
    for item in row.get('source_parity', {}).get('extra_items', []):
        owners = [d.get('name', '') for d in dependencies if any(
            block['start'] <= item['id'] < block['start'] + block['count']
            for block in d.get('definedIds', []))]
        reason = ('unnamed_placeholder' if not item['name'] else
                  'dependency_defined_id' if owners else 'unclassified_extra')
        extras.append({**item, 'reason': reason, 'dependencies': owners})
    return {'id': row['id'], 'arch': row['arch'], 'items': extras}


def identity_remaps(root, row):
    missing = [i for i in row.get('native_roundtrip', {}).get('differences', []) if i['missing']]
    if not missing:
        return []
    original = read_json(root / 'native' / f'{row["id"]:04}' / 'original-source.json')['source']['items']
    rebuilt = read_json(Path(row['directory']) / 'rebuilt-source.json')['source']['items']
    def group(items):
        result = {}
        for item in items:
            kind = 'page' if item['kind'] in ('assembly','class') else item['kind']
            result.setdefault((kind,item['name']), []).append(item)
        return result
    old, new = group(original), group(rebuilt)
    result = []
    for item in missing:
        key = (item['kind'], item['name'])
        a, b = old.get(key, []), new.get(key, [])
        if len(a) == len(b) == 1 and a[0]['id'] != b[0]['id']:
            result.append({'case': row['id'], 'arch': row['arch'], 'kind': key[0], 'name': key[1],
                'before_id': a[0]['id'], 'after_id': b[0]['id'],
                'same_source_text': normalize(a[0]['source_text']) == normalize(b[0]['source_text'])})
    return result


def summarize(root):
    manifest = read_json(root / 'manifest.json')
    rows = [read_json(p) for p in sorted((root / 'cases').glob('*/*/source-audit.json'))]
    integrity = [{'id': r['id'], 'arch': r['arch'], **content_integrity(Path(r['directory']))} for r in rows]
    save(root / 'raw-content-integrity.json', integrity)
    scope = [source_scope(r) for r in rows]
    save(root / 'source-scope.json', scope)
    remaps = [item for row in rows for item in identity_remaps(root, row)]
    save(root / 'native-identity-remaps.json', remaps)
    summary = {'input_files': len(manifest), 'nonempty_inputs': sum(bool(i['size']) for i in manifest),
               'architecture_runs': len(rows), 'expected_runs': 2 * sum(bool(i['size']) for i in manifest),
               'architectures': {}, 'issues': dict(Counter(i for r in rows for i in r['issues'])),
               'native_audit_errors': [{'id': r['id'], 'arch': r['arch'], 'error': r['native_audit_error']}
                                       for r in rows if r.get('native_audit_error')],
               'binaries': {str(p.relative_to(root)): digest(p) for p in sorted((root / 'bin').rglob('*.exe'))}}
    summary['complete'] = len(rows) == summary['expected_runs']
    for arch in ('Win32', 'x64'):
        selected = [r for r in rows if r['arch'] == arch]
        counts = {'runs': len(selected)}
        for stage in ('unpack', 'baseline_pack', 'semantic_pack', 'reunpack', 'second_pack'):
            counts[stage + '_success'] = sum(r['stages'].get(stage, {}).get('status') == 'ok' for r in selected)
        counts['baseline_identical'] = sum(r.get('baseline_identical', False) for r in selected)
        counts['roundtrip_without_issues'] = sum(r['status'] == 'passed' for r in selected)
        counts['original_native_available'] = sum(r['native_original']['valid'] for r in selected)
        counts['rebuilt_native_available'] = sum(r.get('native_rebuilt', {}).get('valid', False) for r in selected)
        counts['native_original_errors'] = dict(Counter(r['native_original'].get('error', 'unknown')
                                                       for r in selected if not r['native_original']['valid']))
        counts['second_pack_identical'] = sum((Path(r['directory']) / 'second.e').exists()
            and digest(Path(r['directory']) / 'second.e') == digest(Path(r['directory']) / 'edited.e') for r in selected)
        for key in ('source_parity', 'rebuilt_parity', 'native_roundtrip'):
            comparisons = [r[key] for r in selected if key in r]
            counts[key] = {'compared_runs': len(comparisons),
                'equal_local_runs': sum(c['total'] == c['equal'] for c in comparisons),
                'equal_without_extras_runs': sum(c['total'] == c['equal'] and not c['extra_items'] for c in comparisons),
                'compared_items': sum(c['total'] for c in comparisons),
                'equal_items': sum(c['equal'] for c in comparisons),
                'extra_items': sum(len(c['extra_items']) for c in comparisons),
                'different_kinds': dict(Counter(d['kind'] for c in comparisons for d in c['differences']))}
        counts['fully_verified'] = sum(r['fully_verified'] for r in selected)
        raw = [i for i in integrity if i['arch'] == arch]
        counts['raw_content'] = {'compared_runs': sum(i['compared'] for i in raw),
            'equal_runs': sum(i['compared'] and not i['differences'] for i in raw),
            'different_folders': dict(Counter(d['folder'] for i in raw for d in i['differences']))}
        counts['extra_source_scope'] = dict(Counter(i['reason'] for r in scope if r['arch'] == arch for i in r['items']))
        counts['native_id_remaps'] = dict(Counter(i['kind'] for i in remaps if i['arch'] == arch))
        summary['architectures'][arch] = counts
    final = root / 'source-audit-summary.json'
    if final.exists():
        summary['originals_unchanged'] = read_json(final).get('originals_unchanged') if summary['complete'] else None
    save(root / 'detailed-summary.json', summary)
    with (root / 'all-e-results.csv').open('w', encoding='utf-8-sig', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['id', 'file', 'arch', 'roundtrip_status', 'issues', 'original_native', 'rebuilt_native',
                         'original_different_items', 'rebuilt_different_items', 'native_roundtrip_different_items',
                         'native_roundtrip_added_items', 'evidence'])
        for r in rows:
            writer.writerow([r['id'], r['source'], r['arch'], r['status'], ';'.join(r['issues']),
                r['native_original']['valid'], r.get('native_rebuilt', {}).get('valid', False),
                *[len(r[k]['differences']) if k in r else 'unavailable'
                  for k in ('source_parity', 'rebuilt_parity', 'native_roundtrip')],
                len(r.get('native_roundtrip', {}).get('extra_items', [])), r['directory']])
    return summary


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.root), ensure_ascii=False, indent=2))
