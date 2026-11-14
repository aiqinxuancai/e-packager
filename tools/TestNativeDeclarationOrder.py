"""独立读取原生声明表，检查语义回包没有丢失或重排程序集、方法、结构及 DLL 声明。"""
import argparse
import json
from pathlib import Path
import struct


class Reader:
    def __init__(self, data):
        self.data, self.offset = data, 0

    def take(self, size):
        if size < 0 or self.offset + size > len(self.data):
            raise ValueError('invalid native block length')
        value = self.data[self.offset:self.offset + size]
        self.offset += size
        return value

    def integer(self):
        return struct.unpack('<i', self.take(4))[0]

    def dynamic(self):
        return self.take(self.integer())

    def variables(self):
        self.integer()
        self.dynamic()

    def table(self, read_item):
        size = self.integer()
        if size < 0 or size % 8:
            raise ValueError('invalid declaration index length')
        count = size // 8
        ids = [self.integer() for _ in range(count)]
        self.take(count * 4)  # memory addresses are not identities
        for _ in ids:
            read_item()
        return ids

    def assembly(self):
        self.take(8)
        self.dynamic()
        self.dynamic()
        self.dynamic()
        self.variables()

    def method(self):
        self.take(12)
        self.dynamic()
        self.dynamic()
        self.variables()
        self.variables()
        for _ in range(6):
            self.dynamic()

    def structure(self):
        self.take(4)
        self.dynamic()
        self.dynamic()
        self.variables()

    def dll(self):
        self.take(8)
        for _ in range(4):
            self.dynamic()
        self.variables()

    def declarations(self):
        self.take(8)
        for _ in range(3):
            self.dynamic()
        for _ in range(struct.unpack('<h', self.take(2))[0]):
            self.dynamic()
        flags = self.integer()
        self.integer()
        if flags & 1:
            self.take(16)
        self.dynamic()
        self.dynamic()
        result = {'classes': self.table(self.assembly), 'methods': self.table(self.method)}
        self.variables()
        result['structs'] = self.table(self.structure)
        result['dlls'] = self.table(self.dll)
        return result


def declarations(path):
    data = path.read_bytes()
    offset = 8
    while offset < len(data):
        if offset + 100 > len(data):
            raise ValueError('truncated section header')
        key = struct.unpack_from('<I', data, offset + 8)[0]
        size = struct.unpack_from('<I', data, offset + 56)[0]
        payload = data[offset + 100:offset + 100 + size]
        if len(payload) != size:
            raise ValueError('truncated section')
        if key == 0x03007319:
            return Reader(payload).declarations()
        offset += 100 + size
    raise ValueError('code section absent')


def audit(case):
    before, after = declarations(case / 'baseline.e'), declarations(case / 'edited.e')
    row = {'case': str(case), 'tables': {}, 'ok': True}
    for name, old in before.items():
        new = after[name]
        old_set, new_set = set(old), set(new)
        unchanged_order = [x for x in old if x in new_set] == [x for x in new if x in old_set]
        unique = len(new_set) == len(new)
        row['tables'][name] = {'original': len(old), 'rebuilt': len(new),
                              'order_preserved': unchanged_order, 'unique': unique,
                              'removed': sorted(old_set - new_set), 'added': sorted(new_set - old_set)}
        row['ok'] &= unchanged_order and unique and old_set <= new_set
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    rows = []
    for case in sorted((args.root / 'cases').glob('*/*')):
        if not (case / 'edited.e').exists():
            continue
        try:
            rows.append(audit(case))
        except Exception as error:
            rows.append({'case': str(case), 'ok': False, 'error': repr(error)})
    summary = {'cases': len(rows), 'passed': sum(r['ok'] for r in rows)}
    (args.root / 'declaration-order-audit.json').write_text(
        json.dumps({'summary': summary, 'cases': rows}, ensure_ascii=False, indent=2), encoding='utf-8-sig')
    print(json.dumps(summary))
    return 0 if rows and all(r['ok'] for r in rows) else 1


if __name__ == '__main__':
    raise SystemExit(main())
