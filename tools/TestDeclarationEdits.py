"""声明添删改、跨页面使用、回包和真实 IDE 编译运行回归。"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import uuid
from TestEComUpdateRoundTrip import write, read_json
from TestEComHeadlessRoundTrip import compile_project

PAGES = {
'程序集1.txt': '''.版本 2
.程序集 程序集1
.程序集变量 程序集数值, 整数型
.程序集变量 程序集数组, 整数型, , "2"
.程序集变量 待删变量, 整数型
.子程序 _启动子程序, 整数型
.局部变量 实例, 计算类
程序集数值 ＝ #基数
程序集数组 [2] ＝ 程序集数值
全局数值 ＝ 程序集数组 [2]
全局记录.数值 ＝ 全局数值
全局记录.数组 [2] ＝ 2
实例.设置 (全局记录.数值)
退出进程 (实例.取值 () ＋ 辅助求值 () ＋ 全局记录.数组 [2] ＋ 读取程序集 ())
返回 (0)
.子程序 读取程序集, 整数型
返回 (程序集数值)
''',
'辅助程序集.txt': '''.版本 2
.程序集 辅助程序集
.程序集变量 私有计数, 整数型
.子程序 辅助求值, 整数型, 公开
私有计数 ＝ 3
返回 (私有计数)
''',
'计算类.txt': '''.版本 2
.程序集 计算类, <对象>, 公开
.程序集变量 类数值, 整数型
.子程序 设置, , 公开
.参数 输入, 整数型
类数值 ＝ 输入
.子程序 取值, 整数型, 公开
返回 (类数值)
''',
'.数据类型.txt': '''.版本 2
.数据类型 记录类型
    .成员 数值, 整数型
    .成员 数组, 整数型, , "2"
.数据类型 待删类型
    .成员 无用字段, 整数型
''',
'.全局变量.txt': '''.版本 2
.全局变量 全局数值, 整数型
.全局变量 全局记录, 记录类型
.全局变量 待删全局, 整数型
''',
'.常量.txt': '''.版本 2
.常量 基数, "10"
.常量 待删常量, "99"
''',
'.DLL声明.txt': '''.版本 2
.DLL命令 退出进程, , "kernel32.dll", "ExitProcess"
    .参数 返回码, 整数型
.DLL命令 待删命令, 整数型, "kernel32.dll", "GetTickCount"
''',
'待删程序集.txt': '''.版本 2
.程序集 待删程序集
.子程序 待删方法, 整数型
返回 (#待删常量)
''',
'待删类.txt': '''.版本 2
.程序集 待删类, <对象>, 公开
.子程序 待删类方法, 整数型, 公开
返回 (99)
''',
}


def main():
    sys.stdout.reconfigure(errors='replace')
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--ide', type=Path, required=True)
    ap.add_argument('--launcher', type=Path, required=True)
    ap.add_argument('--output',type=Path)
    a=ap.parse_args()
    repo=Path(__file__).resolve().parent.parent
    root=(a.output or repo/'temp'/('declaration-edits-'+uuid.uuid4().hex)).resolve()
    root.mkdir(parents=True,exist_ok=False)
    decoder=repo/'bin/Win32/Release/e-packager.exe'
    shutil.copy2(repo/'eproj/e-console-exe-new-proj.e',root/'original.e')
    results=[]
    def call(tool,args,log,fail=False):
        r=subprocess.run([str(tool),*map(str,args)],capture_output=True,timeout=60)
        log.write_bytes(r.stdout+r.stderr)
        assert (r.returncode!=0 if fail else r.returncode==0),str(log)
        return r.stdout+r.stderr
    def record(arch,case):
        results.append(dict(architecture=arch,case=case,passed=True))
        write(root/'results.json',json.dumps(results,indent=2))
        print('PASS',arch,case,flush=True)
    for arch in ('Win32','x64'):
        folder=root/arch;folder.mkdir()
        tool=repo/f'bin/{arch}/Release/e-packager.exe'
        ws=folder/'initial'
        call(decoder,['unpack',root/'original.e',ws],folder/'unpack.log')
        pages=dict(PAGES)
        for stage,expected in [('add',25),('modify',47),('delete',47)]:
            if stage=='modify':
                replacements={'程序集数值':'程序集改名','程序集数组':'数组改名','全局数值':'全局改名','全局记录':'记录改名','记录类型':'结构改名','基数':'常量改名','退出进程':'退出改名','返回码':'退出码','辅助求值':'辅助改名','辅助程序集':'新辅助程序集','计算类':'新计算类','类数值':'类字段改名','私有计数':'辅助字段改名','数值':'字段改名'}
                for name,text in list(pages.items()):
                    for before,after in replacements.items():text=text.replace(before,after)
                    text=text.replace('"10"','"20"').replace('＝ 3','＝ 5').replace('"2"','"3"').replace('[2]','[3]')
                    pages[name]=text
                pages['程序集1.txt']=pages['程序集1.txt'].replace('程序集改名, 整数型','程序集改名, 长整数型')
                pages['.数据类型.txt']=pages['.数据类型.txt'].replace('字段改名, 整数型','字段改名, 长整数型')
                pages['计算类.txt']=pages['计算类.txt'].replace('类字段改名, 整数型','类字段改名, 长整数型')
                pages['.全局变量.txt']=pages['.全局变量.txt'].replace('全局改名, 整数型','全局改名, 长整数型')
            if stage=='delete':
                for name in ('待删程序集.txt','待删类.txt'):
                    pages.pop(name)
                    (ws/'src'/name).unlink()
                # 删除页面时同步索引；仍保留引用的负例在下面单独验证。
                meta=read_json(ws/'project/_meta.json')
                removed={p['key'] for p in meta['sourceFiles'] if p['relativePath'].split('/')[-1] not in pages}
                meta['sourceFiles']=[p for p in meta['sourceFiles'] if p['key'] not in removed]
                meta['rootChildKeys']=[k for k in meta['rootChildKeys'] if k not in removed]
                write(ws/'project/_meta.json',json.dumps(meta,ensure_ascii=False,indent=2))
                for name,text in list(pages.items()):
                    pages[name]='\n'.join(line for line in text.splitlines() if '待删' not in line and '无用字段' not in line)+'\n'
            for name,text in pages.items():write(ws/'src'/name,text)
            output=folder/f'{stage}.e'
            call(tool,['pack',ws,output],folder/f'{stage}-pack.log')
            decoded=folder/f'{stage}-decoded'
            call(decoder,['unpack',output,decoded],folder/f'{stage}-unpack.log')
            for name,text in pages.items():
                actual_path=decoded/'src'/name
                if not name.startswith('.'):
                    assembly=next(line for line in text.splitlines() if line.startswith('.程序集 ')).split(' ',1)[1].split(',')[0]
                    actual_path=decoded/'src'/(assembly+'.txt')
                actual=actual_path.read_text(encoding='utf-8-sig')
                # 比较全部声明和语句，忽略格式化的空白及加法符号宽度。
                normalize=lambda t: ''.join(t.replace('＋','+').split())
                if name in ('.常量.txt','.全局变量.txt'):
                    assert sorted(normalize(line) for line in actual.splitlines() if line.strip())==sorted(normalize(line) for line in text.splitlines() if line.strip()),(arch,stage,name)
                else:
                    assert normalize(actual)==normalize(text),(arch,stage,name)
            if stage=='delete':assert not any('待删' in p.name for p in (decoded/'src').glob('*.txt'))
            artifact=folder/f'{stage}.exe'
            compile_project(a.launcher,a.ide,output,artifact,'auto')
            startup=subprocess.STARTUPINFO();startup.dwFlags=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
            executed=subprocess.run([str(artifact)],capture_output=True,timeout=15,startupinfo=startup)
            assert executed.returncode==expected,(arch,stage,expected,executed.returncode)
            record(arch,f'{stage}-roundtrip-compile-runtime-{expected}')
            ws=decoded
            # 解包以程序集实际名称命名页面，改名后刷新下一阶段的路径。
            pages={p.name:p.read_text(encoding='utf-8-sig') for p in (ws/'src').glob('*.txt')}
        checks=[('assembly-variable','程序集1.txt','.程序集变量 程序集改名,'),
                ('global','.全局变量.txt','.全局变量 全局改名,'),
                ('constant','.常量.txt','.常量 常量改名,'),
                ('dll','.DLL声明.txt','.DLL命令 退出改名,'),
                ('struct-member','.数据类型.txt','.成员 字段改名,'),
                ('class-method','新计算类.txt','.子程序 取值,'),
                ('assembly-method','新辅助程序集.txt','.子程序 辅助改名,'),
                ('class-type','新计算类.txt','.程序集 新计算类,'),
                ('struct-type','.数据类型.txt','.数据类型 结构改名')]
        for name,file,needle in checks:
            path=ws/'src'/file;original=path.read_bytes();text=path.read_text(encoding='utf-8-sig')
            assert needle in text,(file,needle)
            if name in ('class-method','assembly-method'):
                text=text.replace(needle,needle.replace('取值','已删除').replace('辅助改名','已删除'))
            elif name=='dll':text=text.replace('退出改名','已删除')
            elif name=='class-type':text=text.replace('.程序集 新计算类,','.程序集 已删除类,')
            elif name=='struct-type':text=text.replace('.数据类型 结构改名','.数据类型 已删除类型')
            else:text='\n'.join(line for line in text.splitlines() if needle not in line)
            write(path,text)
            try:
                destination=folder/f'invalid-{name}.e'
                call(tool,['pack',ws,destination],folder/f'invalid-{name}.log',True)
                assert not destination.exists()
                record(arch,'dangling-reference-'+name)
            finally:path.write_bytes(original)
        # 普通程序集变量和类字段均不可由无关程序集直接访问。
        for identifier in ('辅助字段改名','类字段改名'):
            path=ws/'src/程序集1.txt';original=path.read_bytes()
            write(path,path.read_text(encoding='utf-8-sig')+f'\n.子程序 越界访问, 整数型\n返回 ({identifier})\n')
            try:
                destination=folder/f'private-{identifier}.e'
                call(tool,['pack',ws,destination],folder/f'private-{identifier}.log',True)
                assert not destination.exists()
                record(arch,'private-field-scope-'+identifier)
            finally:path.write_bytes(original)
    print('Results:',root)

if __name__=='__main__':main()
