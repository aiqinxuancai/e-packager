"""Verify IDE long-text display, full-value editing, and strict placeholder handling."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bin-root', type=Path)
    parser.add_argument('--e-dir', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    binaries = args.bin_root or repo / 'bin'
    args.output.mkdir(parents=True, exist_ok=False)
    source_hash = hashlib.sha256(args.source.read_bytes()).hexdigest()
    results = []

    def run(exe, *arguments, expect=0):
        command = [str(exe), *map(str, arguments), '--e-dir', str(args.e_dir)]
        result = subprocess.run(command, capture_output=True, timeout=120)
        output = (result.stdout + result.stderr).decode('utf-8', errors='replace')
        results.append(dict(command=command, exit=result.returncode, output=output))
        (args.output / 'results.json').write_text(
            json.dumps(results, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        assert result.returncode == expect, results[-1]
        return result

    for arch in ('Win32', 'x64'):
        exe = binaries / f'{arch}/Release/e-packager.exe'
        root = args.output / arch
        root.mkdir()
        workspace = root / 'workspace'
        run(exe, 'unpack', args.source, workspace, '--main-only')
        unmodified = root / 'unmodified.e'
        run(exe, 'pack', workspace, unmodified)
        assert unmodified.read_bytes() == args.source.read_bytes()
        table_path = workspace / 'src/.常量.txt'
        table = table_path.read_bytes().decode('utf-8-sig')
        assert '.常量 常量2, "<文本长度: 84>"' in table, (arch, table)
        index = json.loads((workspace / 'text/list.json').read_text(encoding='utf-8-sig'))
        assert index['formatVersion'] == 1 and len(index['constants']) == 1
        entry = index['constants'][0]
        value_path = workspace / entry['relativePath']
        original_value = value_path.read_bytes().decode('utf-8-sig')
        assert len(original_value.encode('mbcs')) == entry['displayLength'] == 84
        value_path.write_bytes(original_value.replace('哈哈', '嘿嘿', 1).encode('utf-8-sig'))

        packed = root / 'edited.e'
        run(exe, 'pack', workspace, packed)
        reopened = root / 'reopened'
        run(exe, 'unpack', packed, reopened, '--main-only')
        reopened_table = (reopened / 'src/.常量.txt').read_bytes().decode('utf-8-sig')
        assert '.常量 常量2, "<文本长度: 84>"' in reopened_table
        reopened_index = json.loads((reopened / 'text/list.json').read_text(encoding='utf-8-sig'))
        reopened_entry = reopened_index['constants'][0]
        assert reopened_entry['name'] == entry['name']
        actual_value = (reopened / reopened_entry['relativePath']).read_bytes().decode('utf-8-sig')
        assert actual_value == original_value.replace('哈哈', '嘿嘿', 1)

        second = root / 'second.e'
        run(exe, 'pack', reopened, second)
        assert hashlib.sha256(packed.read_bytes()).digest() == hashlib.sha256(second.read_bytes()).digest()

        table_path = reopened / 'src/.常量.txt'
        table = table_path.read_bytes().decode('utf-8-sig')
        table_path.write_bytes(table.replace('84', '85').encode('utf-8-sig'))
        run(exe, 'pack', reopened, root / 'invalid.e', expect=1)
        assert not (root / 'invalid.e').exists()
        table_path.write_bytes(table.encode('utf-8-sig'))
        value_path = reopened / reopened_entry['relativePath']
        original_payload = value_path.read_bytes()

        def must_reject(label, code):
            target = root / (label + '.e')
            result = run(exe, 'pack', reopened, target, expect=1)
            assert code.encode() in result.stdout + result.stderr, results[-1]
            assert not target.exists()

        value_path.unlink()
        must_reject('missing-value', 'read_long_text_constant_failed')
        value_path.write_bytes(original_payload)
        index_path = reopened / 'text/list.json'
        index_bytes = index_path.read_bytes()
        index_path.unlink()
        must_reject('missing-index', 'invalid_long_text_index')
        index_path.write_bytes(index_bytes)
        table_path.write_bytes(table.replace('常量2', '改名常量').encode('utf-8-sig'))
        must_reject('unbound-name', 'long_text_content_missing')
        table_path.write_bytes(table.encode('utf-8-sig'))
        invalid_index = json.loads(index_bytes.decode('utf-8-sig'))
        invalid_index['constants'].append(invalid_index['constants'][0].copy())
        index_path.write_text(json.dumps(invalid_index, ensure_ascii=False), encoding='utf-8-sig')
        must_reject('duplicate-binding', 'invalid_long_text_entry')
        index_path.write_bytes(index_bytes)
        metadata_path = reopened / 'project/_meta.json'
        metadata_bytes = metadata_path.read_bytes()
        metadata = json.loads(metadata_bytes.decode('utf-8-sig'))
        metadata.pop('constantTextFormat', None)
        metadata_path.write_text(json.dumps(metadata, ensure_ascii=False), encoding='utf-8-sig')
        must_reject('legacy-placeholder-without-value', 'long_text_content_missing')
        metadata_path.write_bytes(metadata_bytes)

        # 不同长度、控制字符、引号和前缀都必须保留实际值，包含空长文本。
        for iteration, expected in enumerate(['首行\r\n第二行\n第三行\r末尾\t"引号”\x00#e2txt_text#\\n', '']):
            index = json.loads((reopened / 'text/list.json').read_text(encoding='utf-8-sig'))
            (reopened / index['constants'][0]['relativePath']).write_bytes(expected.encode('utf-8-sig'))
            run(exe, 'update', reopened)
            refreshed = json.loads((reopened / 'text/list.json').read_text(encoding='utf-8-sig'))
            assert refreshed['constants'][0]['displayLength'] == len(expected.encode('mbcs'))
            updated = root / f'payload-{iteration}.e'
            run(exe, 'pack', reopened, updated)
            reopened = root / f'payload-{iteration}'
            run(exe, 'unpack', updated, reopened, '--main-only')
            index = json.loads((reopened / 'text/list.json').read_text(encoding='utf-8-sig'))
            assert len(index['constants']) == 1, index
            entry = index['constants'][0]
            assert entry['displayLength'] == len(expected.encode('mbcs'))
            assert (reopened / entry['relativePath']).read_bytes().decode('utf-8-sig') == expected
            assert f'<文本长度: {len(expected.encode("mbcs"))}>' in (reopened / 'src/.常量.txt').read_text(encoding='utf-8-sig')

        print(arch, 'PASS', flush=True)

    assert hashlib.sha256(args.source.read_bytes()).hexdigest() == source_hash


if __name__ == '__main__':
    main()
