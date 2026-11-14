"""验证缺失原生 ID 与真实同名变量隔离，且编辑后重新语义编码。"""
import argparse
import base64
import json
from pathlib import Path
import struct
import subprocess


def write(path, text):
    path.write_bytes(text.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8-sig'))


def checksum(data):
    value = bytearray(4)
    for i, byte in enumerate(data):
        value[i % 4] ^= byte
    return bytes(value)


def replace_expression(source, before, after):
    assert source.count(before) == 1
    result, offset, replaced = bytearray(source[:8]), 8, False
    while offset < len(source):
        header = bytearray(source[offset:offset + 100])
        assert header[:4] == bytes.fromhex('19731115')
        size = struct.unpack_from('<I', header, 56)[0]
        payload = source[offset + 100:offset + 100 + size]
        if before in payload:
            assert not replaced
            payload = payload.replace(before, after)
            header[52:56] = checksum(payload)
            struct.pack_into('<I', header, 56, len(payload))
            header[4:8] = checksum(header[8:])
            replaced = True
        result += header + payload
        offset += 100 + size
    assert replaced and offset == len(source)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = args.output.resolve()
    repo = Path(__file__).resolve().parent.parent
    evidence = []

    def run(*parts, expected=0):
        command = [str(args.exe), *map(str, parts)]
        result = subprocess.run(command, cwd=root, capture_output=True, timeout=120,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        evidence.append(dict(command=command, exit=result.returncode,
                             stdout=result.stdout.decode('utf-8-sig', 'replace'),
                             stderr=result.stderr.decode('utf-8-sig', 'replace')))
        (root / 'commands.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding='utf-8')
        assert result.returncode == expected, evidence[-1]

    def method(workspace):
        pages = json.loads((workspace / 'project/.native_source_map.json').read_text(encoding='utf-8-sig'))
        return next(m for p in pages for m in p['methods'] if m['name'] == '_启动子程序')

    run('unpack', repo / 'eproj/e-console-exe-new-proj.e', root / 'seed', '--main-only')
    page = root / 'seed/src/程序集1.txt'
    code = ('.版本 2\n.程序集 程序集1\n.子程序 _启动子程序, 整数型\n'
            '.局部变量 _Local_0x1E217, 整数型\n_Local_0x1E217 ＝ 7\n'
            ".' 如果真 (_Local_0x1E217 ≠ 0)\n    标准输出 (1, “IDENTITY_OK”)\n"
            '.如果真结束\n返回 (0)\n')
    write(page, code)
    run('pack', root / 'seed', root / 'seed.e')
    run('unpack', root / 'seed.e', root / 'seed-decoded', '--main-only')
    original = method(root / 'seed-decoded')
    existing, missing = original['localIds'][0], 0x2501E217
    assert existing != missing
    expression = base64.b64decode(original['expressionData'])
    offsets = list(struct.unpack('<' + 'i' * (len(base64.b64decode(original['lineOffset'])) // 4),
                                 base64.b64decode(original['lineOffset'])))
    header = b'\x6c' + struct.pack('<ihh', 1, 0, 32)
    assert expression.count(header) == 1
    start = expression.index(header)
    end = next(offset for offset in offsets if offset > start)
    before = expression[start:end]
    assert before.count(struct.pack('<I', existing)) == 1
    patched = expression[:start] + before.replace(struct.pack('<I', existing), struct.pack('<I', missing)) + expression[end:]
    (root / 'missing.e').write_bytes(replace_expression((root / 'seed.e').read_bytes(), expression, patched))
    run('unpack', root / 'missing.e', root / 'workspace', '--main-only')
    page = root / 'workspace/src/程序集1.txt'
    displayed = page.read_text(encoding='utf-8-sig')
    assert '.局部变量 _Local_0x1E217, 整数型' in displayed
    assert ".' 如果真 ( ≠ 0)" in displayed
    native = method(root / 'workspace')
    assert native['unresolvedVariables'] == [{'id': missing, 'name': '_Local_0x1E217_'}]
    write(page, displayed.replace('＝ 7', '＝ 9') + "\n' force semantic edit\n")
    run('pack', root / 'workspace', root / 'edited.e')
    run('unpack', root / 'edited.e', root / 'decoded', '--main-only')
    native = method(root / 'decoded')
    data = base64.b64decode(native['expressionData'])
    assert native['localIds'] == [existing]
    assert data.count(struct.pack('<I', existing)) == 1
    assert data.count(struct.pack('<I', missing)) == 1
    assert data.count(header) == 1
    assert '_Local_0x1E217 ＝ 9' in (root / 'decoded/src/程序集1.txt').read_text(encoding='utf-8-sig')
    write(page, displayed.replace('( ≠ 0)', '(_Local_0x1E217 ≠ 0)'))
    run('pack', root / 'workspace', root / 'resolved.e')
    run('unpack', root / 'resolved.e', root / 'resolved', '--main-only')
    data = base64.b64decode(method(root / 'resolved')['expressionData'])
    assert struct.pack('<I', missing) not in data
    assert data.count(struct.pack('<I', existing)) == 2
    write(page, displayed.replace('.局部变量 _Local_0x1E217, 整数型',
        '.局部变量 _Local_0x1E217, 整数型\n.局部变量 _Local_0x1E217_, 整数型'))
    run('pack', root / 'workspace', root / 'collision.e', expected=1)
    assert not (root / 'collision.e').exists()
    assert 'native_identity_name_conflict' in str(evidence[-1])
    write(page, displayed.replace('( ≠ 0)', '(不存在的变量 ≠ 0)'))
    run('pack', root / 'workspace', root / 'invalid.e', expected=1)
    assert not (root / 'invalid.e').exists()
    assert 'semantic_method_rebuild_failed' in str(evidence[-1])
    (root / 'result.json').write_text(json.dumps(dict(ok=True, declared_id=existing, missing_id=missing,
        edited_native_references_preserved=True, explicit_resolution_verified=True,
        invalid_control_rejected=True, new_declaration_collision_rejected=True), indent=2), encoding='utf-8')
    print('PASS native identity roundtrip')


if __name__ == '__main__':
    main()
