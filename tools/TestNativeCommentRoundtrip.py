"""验证空注释与流程首尾注释经过真实语义编辑仍各自保留。"""
import argparse
import base64
import json
from pathlib import Path
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--e-dir', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parents[1]
    log = []

    def run(*parts):
        cmd = [str(args.exe), *map(str, parts), '--e-dir', str(args.e_dir)]
        p = subprocess.run(cmd, capture_output=True, timeout=120)
        log.append(dict(command=cmd, exit=p.returncode, output=(p.stdout+p.stderr).decode('utf-8', 'replace')))
        (args.output/'commands.json').write_text(json.dumps(log, ensure_ascii=False, indent=2), encoding='utf-8')
        assert p.returncode == 0, log[-1]

    def write(p, t):
        p.write_bytes(t.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8-sig'))

    code = '''.版本 2
.程序集 程序集1
.子程序 _启动子程序, 整数型
.局部变量 a, 整数型
a ＝ 7  '
.如果 (a ＝ 0)  '
    '
.否则
    标准输出 (1, “评论测试”)  '
.如果结束
.' 如果真 (a ≠ 0)  '
    a ＝ 8  '
.如果真结束
.判断开始 (a ＝ 0)  '
.判断 (a ＝ 1)  ' 分支
.默认
.判断结束
.判断循环首 (a ＝ 0)  '
    a ＝ 1
.判断循环尾 ()  ' 尾部
.循环判断首 ()  ' 首部
    a ＝ 2
.循环判断尾 (a ＝ 0)  '
.计次循环首 (1, a)  '
.计次循环尾 ()  '
.变量循环首 (1, 2, 1, a)  '
.变量循环尾 ()  ' 尾部2
返回 (0)  '
'''
    code = code.replace("'\n", "' \n")
    workspace = args.output/'seed'
    run('unpack', repo/'eproj/e-console-exe-new-proj.e', workspace, '--main-only')
    write(workspace/'src/程序集1.txt', code)
    for index in range(2):
        packed = args.output/f'edited-{index}.e'
        run('pack', workspace, packed)
        workspace = args.output/f'decoded-{index}'
        run('unpack', packed, workspace, '--main-only')
        actual = (workspace/'src/程序集1.txt').read_text(encoding='utf-8-sig')
        native = json.loads((workspace/'project/.native_source_map.json').read_text(encoding='utf-8-sig'))
        expression = base64.b64decode(native[0]['methods'][0]['expressionData'])
        # 原生 IDE 不显示仅含终止符的注释槽；显式空行尾注释必须有内容长度。
        header = b'\x6a' + struct.pack('<ihh', 13, 0, 0)
        position = expression.index(header) + len(header)
        position += 4 + struct.unpack_from('<i', expression, position)[0]
        assert expression[position:position+6] == struct.pack('<i', 2) + b'\0\0'
        # 编辑器会添加声明间空行；不裁剪行尾，空注释本身必须保留。
        assert [x for x in actual.splitlines() if x] == [x for x in code.splitlines() if x], actual
        code = code.replace('a ＝ 7', 'a ＝ 9')
        write(workspace/'src/程序集1.txt', actual.replace('a ＝ 7', 'a ＝ 9'))
    print('PASS native comments')


if __name__ == '__main__':
    main()
