"""验证隐藏结构和 DLL 声明不因语义编辑丢失，可见声明仍能正常删除。"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
from TestNativeDeclarationOrder import Reader, declarations
from TestNativeIdentityRoundtrip import checksum, write


def hide_declarations(data, ids):
    output, offset, changed = bytearray(data[:8]), 8, set()
    while offset < len(data):
        header = bytearray(data[offset:offset + 100])
        size = struct.unpack_from('<I', header, 56)[0]
        payload = bytearray(data[offset + 100:offset + 100 + size])
        if struct.unpack_from('<I', header, 8)[0] == 0x03007319:
            class Mutator(Reader):
                def table(self, fn):
                    count = self.integer() // 8
                    keys = [self.integer() for _ in range(count)]
                    self.take(count * 4)
                    for key in keys:
                        position = self.offset
                        fn()
                        if key in ids:
                            assert fn.__name__ in ('structure', 'dll')
                            struct.pack_into('<i', payload, position, 2 if fn.__name__ == 'structure' else 4)
                            changed.add(key)
                    return keys
            Mutator(payload).declarations()
            header[52:56] = checksum(payload)
            header[4:8] = checksum(header[8:])
        output += header + payload
        offset += 100 + size
    assert changed == ids
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    commands = []
    def run(*parts):
        command = [str(args.exe.resolve()), *map(str, parts)]
        result = subprocess.run(command, cwd=root, capture_output=True, timeout=120)
        commands.append(dict(command=command, exit=result.returncode,
                             stderr=result.stderr.decode('utf-8', 'replace')))
        (root / 'commands.json').write_text(json.dumps(commands, ensure_ascii=False, indent=2), encoding='utf-8-sig')
        assert result.returncode == 0, commands[-1]
    repo = Path(__file__).resolve().parent.parent
    run('unpack', repo / 'eproj/e-console-exe-new-proj.e', root / 'seed', '--main-only')
    write(root / 'seed/src/.数据类型.txt', '.版本 2\n\n.数据类型 历史结构\n    .成员 数值, 整数型\n\n.数据类型 可删除结构\n    .成员 数值, 整数型\n')
    write(root / 'seed/src/.DLL声明.txt', '.版本 2\n\n.DLL命令 历史命令, 整数型, "kernel32.dll", "GetCurrentProcessId"\n')
    run('pack', root / 'seed', root / 'seed.e')
    run('unpack', root / 'seed.e', root / 'visible', '--main-only')
    symbols = json.loads((root / 'visible/project/.native_symbol_map.json').read_text(encoding='utf-8-sig'))
    hidden = {symbols['structs'][0]['id'], symbols['dlls'][0]['id']}
    removed = symbols['structs'][1]['id']
    (root / 'hidden.e').write_bytes(hide_declarations((root / 'seed.e').read_bytes(), hidden))
    run('unpack', root / 'hidden.e', root / 'workspace', '--main-only')
    write(root / 'workspace/src/.数据类型.txt', '.版本 2\n')
    page = root / 'workspace/src/程序集1.txt'
    write(page, page.read_text(encoding='utf-8-sig') + "\n' semantic edit\n")
    run('pack', root / 'workspace', root / 'edited.e')
    tables = declarations(root / 'edited.e')
    actual = set(tables['structs'] + tables['dlls'])
    assert hidden <= actual, (hidden, actual)
    assert removed not in actual, 'deleted visible declaration resurrected'
    run('unpack', root / 'edited.e', root / 'decoded', '--main-only')
    run('pack', root / 'decoded', root / 'second.e')
    assert (root / 'second.e').read_bytes() == (root / 'edited.e').read_bytes()
    print('PASS hidden declaration identities and visible deletion')


if __name__ == '__main__':
    main()
