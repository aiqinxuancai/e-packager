"""核验 FNE TXT 的完整表计数、索引、V1/V2 签名及引用导出的一致性。"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import shutil


def run(args, log):
    result = subprocess.run([str(a) for a in args], capture_output=True)
    log.write_bytes(result.stdout + result.stderr)
    assert result.returncode == 0, log


def validate(text):
    totals = dict(types=0, properties=0, events=0, event_parameters=0)
    for block in re.split(r'(?m)^\.数据类型 ', text)[1:]:
        header = block.splitlines()[0]
        totals['types'] += 1
        properties = re.findall(r'^  \.成员 .*属性索引=(\d+)', block, re.M)
        if '属性数=' in header:
            count = int(re.search(r'属性数=(\d+)', header)[1])
            assert list(map(int, properties)) == list(range(count)), header
        events = re.split(r'(?m)^  \.事件 ', block)[1:]
        assert len(events) == int(re.search(r'事件数=(\d+)', header)[1]), header
        totals['properties'] += len(properties)
        totals['events'] += len(events)
        for index, event in enumerate(events):
            event = event.split('  .成员命令 ')[0]
            line = event.splitlines()[0]
            assert int(re.search(r'事件索引=(\d+)', line)[1]) == index
            args = re.findall(r'^    \.参数 .*参数索引=(\d+)', event, re.M)
            assert list(map(int, args)) == list(range(int(re.search(r'参数数=(\d+)', line)[1]))), line
            assert '状态标志=' in line and '支持平台=' in line and '接口版本=' in line
            totals['event_parameters'] += len(args)
    assert '<无法' not in text, 'incomplete metadata'
    return totals


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--fixture', required=True, help='用 Win32 编译的 FnePublicInfoTest.cpp')
    parser.add_argument('--lib-dir', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    tool = repo / 'bin/Win32/Release/e-packager.exe'
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    reports = {}
    exported = {}
    for name in ('krnln', 'iext', 'iext2', 'iext3', 'fixture'):
        fne = Path(args.fixture).resolve() if name == 'fixture' else Path(args.lib_dir).resolve() / (name + '.fne')
        destination = output / (name + '.txt')
        run([tool, 'decrypt-fne', fne, destination], output / (name + '.log'))
        data = destination.read_bytes()
        assert data.startswith(b'\xef\xbb\xbf') and b'\n' not in data.replace(b'\r\n', b'')
        text = data.decode('utf-8-sig').replace('\r\n', '\n')
        exported[name] = text
        if name != 'fixture':
            reports[name] = validate(text)
    fixture = exported['fixture']
    for expected in ('128:Last', '对话框标题=""', '文件过滤器="Text|*.txt"', '默认后缀=""',
                     '保存文件标志="1"', '可选值=-7:|42:Answer', '属性索引=3',
                     '.事件 Legacy, 返回值=逻辑型', '.事件 LegacyInt, 返回值=整数型',
                     '.参数 Enabled, 逻辑型', '.参数 Value, 整数型', '属性=传址',
                     '支持平台=Linux, 接口版本=2', '属性表：<无法读取>，声明数量=2',
                     '事件表：<无法读取>，声明数量=3'):
        assert expected in fixture, expected
    assert '.成员 左边' not in fixture, 'invented fallback properties'
    reports['fixture'] = 'V1/V2, reference argument, 129 choices, empty fields, hidden slot, invalid tables passed'
    workspace = output / 'workspace'
    shutil.copy2(repo / 'eproj/e-window-exe-full+otherFne.e', output / 'original.e')
    shutil.copy2(Path(args.fixture).resolve(), output / 'fixture.fne')
    run([tool, 'unpack', output / 'original.e', workspace, '--main-only'], output / 'unpack.log')
    run([tool, 'update', workspace, '--add-elib', output / 'fixture.fne'], output / 'update.log')
    referenced = [p.read_text(encoding='utf-8-sig') for p in workspace.rglob('*.txt') if p.parent.name == 'elib']
    for name, text in exported.items():
        # 路径等导出头允许不同，类型表及其后正文必须与独立导出完全一致。
        body = text.split('[数据类型]', 1)[1]
        assert any('[数据类型]' in t and t.split('[数据类型]', 1)[1] == body for t in referenced), name
    reports['dependency_export'] = 'all five match direct export'
    (output / 'report.json').write_text(json.dumps(reports, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(reports, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
