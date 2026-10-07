"""验证组合框、列表框的空集合、清空操作及 UTF-8 错误定位。"""
import argparse
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import uuid
import xml.etree.ElementTree as ET

from TestEComHeadlessRoundTrip import compile_project
from TestEComUpdateRoundTrip import read_json, source_text, write


def main():
    sys.stdout.reconfigure(errors='replace')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--ide', type=Path)
    parser.add_argument('--launcher', type=Path)
    args = parser.parse_args()
    assert bool(args.ide) == bool(args.launcher)
    repo = Path(__file__).resolve().parent.parent
    tool = repo / 'bin/Win32/Release/e-packager.exe'
    output = (args.output or repo / 'temp' / f'issue13-{uuid.uuid4().hex}').resolve()
    output.mkdir(parents=True, exist_ok=False)
    original = output / 'original.e'
    shutil.copy2(repo / 'eproj/e-window-exe-full+otherFne.e', original)
    workspace = output / 'workspace'
    results = []

    def invoke(*arguments, log):
        result = subprocess.run([str(tool), *map(str, arguments)], capture_output=True)
        data = result.stdout + result.stderr
        (output / log).write_bytes(data)
        return result, data

    def checked(*arguments, log):
        result, data = invoke(*arguments, log=log)
        assert result.returncode == 0, f'{log}: {data[-500:]!r}'

    def passed(case):
        results.append(dict(case=case, passed=True))
        write(output / 'results.json', json.dumps(results, indent=2))
        print(f'PASS {case}', flush=True)

    def save(root):
        ET.indent(root)
        write(xml_file, ET.tostring(root, encoding='unicode'))

    def controls(root):
        return [next(root.iter(tag)) for tag in ('列表框', '组合框')]

    def collection(node, name):
        tag = f'{node.tag}.{name}'
        child = node.find(tag)
        if child is None:
            child = ET.SubElement(node, tag)
        child.clear()
        return child

    checked('unpack', original, workspace, '--main-only', log='unpack.log')
    checked('pack', workspace, output / 'unchanged.e', log='unchanged.log')
    assert original.read_bytes() == (output / 'unchanged.e').read_bytes()
    passed('unchanged-byte-identical')
    xml_file = workspace / 'src/_启动窗口.xml'
    initial_xml = ET.parse(xml_file).getroot()
    meta = read_json(workspace / 'project/_meta.json')
    page = workspace / meta['sourceFiles'][0]['relativePath']
    write(page, page.read_text(encoding='utf-8-sig') + "\n' issue13-source-edit\n")

    for mode in ('empty', 'clear-populated', 'no-native', 'no-native-or-scalar', 'populated'):
        root = copy.deepcopy(initial_xml)
        for node in controls(root):
            texts = collection(node, '列表项目')
            integers = collection(node, '项目数值')
            if mode in ('empty', 'no-native', 'no-native-or-scalar'):
                node.set('列表项目', 'AAA=')
                node.set('项目数值', '')
            if mode in ('no-native', 'no-native-or-scalar'):
                node.attrib.pop('扩展属性数据', None)
            if mode == 'no-native-or-scalar':
                node.attrib.pop('列表项目', None)
                node.attrib.pop('项目数值', None)
            if mode == 'populated':
                for index, text in enumerate(('甲', '乙')):
                    ET.SubElement(texts, '项目', 索引=str(index), 文本=text)
                    ET.SubElement(integers, '项目', 索引=str(index), 数值=str(index + 7))
        save(root)
        packed = output / f'{mode}.e'
        checked('pack', workspace, packed, log=f'{mode}.log')
        decoded = output / f'{mode}-decoded'
        checked('unpack', packed, decoded, '--main-only', log=f'{mode}-unpack.log')
        actual = ET.parse(decoded / 'src/_启动窗口.xml').getroot()
        for node in controls(actual):
            texts = node.find(f'{node.tag}.列表项目')
            assert texts is not None
            if mode == 'populated':
                assert [item.get('文本') for item in texts] == ['甲', '乙']
                numbers = node.find(f'{node.tag}.项目数值')
                assert numbers is not None and [item.get('数值') for item in numbers] == ['7', '8']
            else:
                assert len(texts) == 0 and node.get('列表项目') == 'AAA='
                assert node.get('项目数值') == ''
        assert 'issue13-source-edit' in source_text(decoded)
        passed(mode)
        if args.ide and mode in ('no-native-or-scalar', 'populated'):
            compile_project(args.launcher, args.ide, packed, output / f'{mode}.exe', 'auto')
            passed(mode + '-headless')

    for mode in ('missing-value', 'mixed-kinds', 'duplicate-index', 'bad-number', 'duplicate-node'):
        for tag in ('列表框', '组合框'):
            root = copy.deepcopy(initial_xml)
            node = next(root.iter(tag))
            group = collection(node, '列表项目')
            if mode == 'missing-value':
                ET.SubElement(group, '项目')
            elif mode == 'mixed-kinds':
                ET.SubElement(group, '项目', 文本='甲')
                ET.SubElement(group, '项目', 数值='1')
            elif mode == 'duplicate-index':
                ET.SubElement(group, '项目', 索引='0', 文本='甲')
                ET.SubElement(group, '项目', 索引='0', 文本='乙')
            elif mode == 'bad-number':
                ET.SubElement(group, '项目', 数值='not-a-number')
            else:
                ET.SubElement(node, group.tag)
            save(root)
            name = f'{mode}-{tag}'
            destination = output / f'{name}.e'
            result, data = invoke('pack', workspace, destination, log=f'{name}.log')
            assert result.returncode != 0 and not destination.exists()
            diagnostic = data.decode('utf-8')
            for marker in (tag, node.get('名称'), 'src/_启动窗口.xml', '列表项目'):
                assert marker in diagnostic, (name, marker)
            assert 'window_control_' in diagnostic
            passed(name + '-utf8-diagnostic')
    print(f'Results: {output}')


if __name__ == '__main__':
    main()
