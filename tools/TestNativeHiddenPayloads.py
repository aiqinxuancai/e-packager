"""独立读取隐藏结构及 DLL 的字段、参数和成员，验证语义回包完整保留声明。"""
import argparse
import json
from pathlib import Path
import struct
from TestNativeDeclarationOrder import Reader


class PayloadReader(Reader):
    def table(self, read_item):
        size = self.integer()
        if size < 0 or size % 8:
            raise ValueError('invalid native declaration index')
        count = size // 8
        ids = [self.integer() for _ in range(count)]
        self.take(count * 4)
        return {identity: read_item() for identity in ids}

    def variables(self):
        count = self.integer()
        data = self.dynamic()
        result = []
        for index in range(count):
            identity = struct.unpack_from('<i', data, index * 4)[0]
            offset = count * 8 + struct.unpack_from('<i', data, (count + index) * 4)[0]
            # 历史文件的 length 可能是旧分配长度；按字段格式读取，不把它当作块边界。
            dimensions = data[offset + 10]
            end = offset + 11 + dimensions * 4
            if end > len(data):
                raise ValueError('truncated native array bounds')
            end = data.index(b'\x00', end) + 1  # name
            end = data.index(b'\x00', end) + 1  # comment
            # 比较类型、标记、数组维度、名称及备注，不比较存储偏移和长度字段。
            result.append([identity, data[offset + 4:end].hex()])
        return result

    def structure(self):
        return [self.integer(), self.dynamic().hex(), self.dynamic().hex(), self.variables()]

    def dll(self):
        return [self.integer(), self.integer(), *[self.dynamic().hex() for _ in range(4)], self.variables()]


def declarations(path):
    data, offset = path.read_bytes(), 8
    while offset < len(data):
        size = struct.unpack_from('<I', data, offset + 56)[0]
        if struct.unpack_from('<I', data, offset + 8)[0] == 0x03007319:
            return PayloadReader(data[offset + 100:offset + 100 + size]).declarations()
        offset += 100 + size
    raise ValueError('native code section absent')


def audit(case):
    before, after = declarations(case / 'baseline.e'), declarations(case / 'edited.e')
    differences, checked = [], 0
    for kind, flag in [('structs', 2), ('dlls', 4)]:
        for identity, original in before[kind].items():
            if not original[0] & flag:
                continue
            checked += 1
            rebuilt = after[kind].get(identity)
            if original != rebuilt:
                differences.append({'kind': kind, 'id': identity, 'original': original, 'rebuilt': rebuilt})
    return {'case': str(case), 'checked': checked, 'differences': differences, 'ok': not differences}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    rows = []
    for path in sorted((args.root / 'cases').glob('*/*/source-audit.json')):
        try:
            rows.append(audit(path.parent))
        except Exception as error:
            rows.append({'case': str(path.parent), 'ok': False, 'error': repr(error)})
    summary = {'cases': len(rows), 'passed': sum(r['ok'] for r in rows),
               'checked_declarations': sum(r.get('checked', 0) for r in rows),
               'changed_declarations': sum(len(r.get('differences', [])) for r in rows),
               'reader_errors': sum('error' in r for r in rows)}
    (args.root / 'hidden-declaration-audit.json').write_text(
        json.dumps({'summary': summary, 'cases': rows}, ensure_ascii=False, indent=2), encoding='utf-8-sig')
    print(json.dumps(summary))
    return 0 if rows and all(r['ok'] for r in rows) else 1


if __name__ == '__main__':
    raise SystemExit(main())
