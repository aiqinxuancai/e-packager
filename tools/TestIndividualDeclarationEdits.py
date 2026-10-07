"""每项声明修改分别回包、解包核对、无头编译和运行。"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import uuid
from TestDeclarationEdits import PAGES
from TestEComUpdateRoundTrip import write, read_json
from TestEComHeadlessRoundTrip import compile_project


def cases():
    tests=[]
    def add(name, edits, expected=25):
        pages=dict(PAGES)
        for file,before,after in edits:
            if file=='*':
                pages={key:value.replace(before,after) for key,value in pages.items()}
            elif before is None:
                if after is None:pages.pop(file)
                else:pages[file]=after
            else:
                assert before in pages[file],(name,file,before)
                pages[file]=pages[file].replace(before,after)
        tests.append((name,pages,expected))
    def rename(name,before,after):add(name,[('*',before,after)])
    def append(name,file,declaration,use,expected=26):
        add(name,[(file,None,PAGES[file]+declaration),('程序集1.txt','退出进程 (',use+'\n退出进程 (')],expected)
    add('assembly-variable-add-use', [('程序集1.txt','.程序集变量 待删变量, 整数型','.程序集变量 待删变量, 整数型\n.程序集变量 新增变量, 整数型'),('程序集1.txt','退出进程 (','新增变量 ＝ 1\n程序集数值 ＝ 程序集数值 ＋ 新增变量\n退出进程 (')],26)
    rename('assembly-variable-rename-use','程序集数值','程序集改名')
    add('assembly-variable-type-use',[('程序集1.txt','程序集数值, 整数型','程序集数值, 长整数型')])
    add('assembly-array-resize-use',[('程序集1.txt','"2"','"3"'),('程序集1.txt','程序集数组 [2]','程序集数组 [3]')])
    add('assembly-variable-delete',[('程序集1.txt','.程序集变量 待删变量, 整数型\n','')])
    append('global-add-use','.全局变量.txt','.全局变量 新增全局, 整数型\n','新增全局 ＝ 1\n程序集数值 ＝ 程序集数值 ＋ 新增全局')
    rename('global-rename-use','全局数值','全局改名')
    add('global-type-use',[('.全局变量.txt','全局数值, 整数型','全局数值, 长整数型')])
    add('global-delete',[('.全局变量.txt','.全局变量 待删全局, 整数型\n','')])
    append('constant-add-use','.常量.txt','.常量 新增常量, "1"\n','程序集数值 ＝ 程序集数值 ＋ #新增常量')
    rename('constant-rename-use','基数','常量改名')
    add('constant-value-use',[('.常量.txt','"10"','"20"')],45)
    add('constant-delete',[('.常量.txt','.常量 待删常量, "99"\n',''),('待删程序集.txt','#待删常量','99')])
    append('dll-add-use','.DLL声明.txt','.DLL命令 新增DLL, 整数型, "kernel32.dll", "GetCurrentProcessId"\n','新增DLL ()',25)
    rename('dll-rename-use','退出进程','退出改名')
    rename('dll-parameter-rename','返回码','退出码')
    add('dll-entry-change-use',[('.DLL声明.txt','"GetTickCount"','"GetCurrentProcessId"'),('程序集1.txt','退出进程 (','待删命令 ()\n退出进程 (')])
    add('dll-delete',[('.DLL声明.txt','.DLL命令 待删命令, 整数型, "kernel32.dll", "GetTickCount"\n','')])
    add('struct-add-use',[('.数据类型.txt',None,PAGES['.数据类型.txt']+'.数据类型 新增结构\n    .成员 字段, 整数型\n'),('程序集1.txt','.局部变量 实例, 计算类','.局部变量 实例, 计算类\n.局部变量 新结构, 新增结构'),('程序集1.txt','退出进程 (','新结构.字段 ＝ 1\n程序集数值 ＝ 程序集数值 ＋ 新结构.字段\n退出进程 (')],26)
    rename('struct-rename-use','记录类型','结构改名')
    add('struct-member-rename-use',[('.数据类型.txt','.成员 数值,','.成员 字段改名,'),('程序集1.txt','全局记录.数值','全局记录.字段改名')])
    add('struct-member-type-use',[('.数据类型.txt','.成员 数值, 整数型','.成员 数值, 长整数型')])
    add('struct-array-resize-use',[('.数据类型.txt','"2"','"3"'),('程序集1.txt','全局记录.数组 [2]','全局记录.数组 [3]')])
    add('struct-delete',[('.数据类型.txt','.数据类型 待删类型\n    .成员 无用字段, 整数型\n','')])
    add('assembly-add-use',[('新增程序集.txt',None,'.版本 2\n.程序集 新增程序集\n.子程序 新方法, 整数型, 公开\n返回 (1)\n'),('程序集1.txt','退出进程 (','程序集数值 ＝ 程序集数值 ＋ 新方法 ()\n退出进程 (')],26)
    rename('assembly-rename','辅助程序集','辅助改名')
    add('assembly-implementation-change',[('辅助程序集.txt','私有计数 ＝ 3','私有计数 ＝ 5')],27)
    add('assembly-delete',[('待删程序集.txt',None,None)])
    add('class-add-use',[('新增类.txt',None,'.版本 2\n.程序集 新增类, <对象>, 公开\n.子程序 取值, 整数型, 公开\n返回 (1)\n'),('程序集1.txt','.局部变量 实例, 计算类','.局部变量 实例, 计算类\n.局部变量 新实例, 新增类'),('程序集1.txt','退出进程 (','程序集数值 ＝ 程序集数值 ＋ 新实例.取值 ()\n退出进程 (')],26)
    rename('class-rename-use','计算类','类改名')
    add('class-implementation-change',[('计算类.txt','返回 (类数值)','返回 (类数值 ＋ 1)')],26)
    rename('class-field-rename-use','类数值','类字段改名')
    add('class-field-type-use',[('计算类.txt','类数值, 整数型','类数值, 长整数型')])
    add('class-delete',[('待删类.txt',None,None)])
    return tests


def main():
    sys.stdout.reconfigure(errors='replace')
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--ide',type=Path,required=True)
    ap.add_argument('--launcher',type=Path,required=True)
    ap.add_argument('--output',type=Path)
    ap.add_argument('--resume',action='store_true')
    a=ap.parse_args()
    repo=Path(__file__).resolve().parent.parent
    root=(a.output or repo/'temp'/('individual-declarations-'+uuid.uuid4().hex)).resolve()
    root.mkdir(parents=True,exist_ok=a.resume)
    decoder=repo/'bin/Win32/Release/e-packager.exe'
    original=root/'original.e';shutil.copy2(repo/'eproj/e-console-exe-new-proj.e',original)
    def run(tool,args,log):
        r=subprocess.run([str(tool),*map(str,args)],capture_output=True,timeout=60)
        log.write_bytes(r.stdout+r.stderr)
        assert r.returncode==0,log
    seed=root/'seed';run(decoder,['unpack',original,seed],root/'seed-unpack.log')
    for name,text in PAGES.items():write(seed/'src'/name,text)
    baseline=root/'baseline.e';run(decoder,['pack',seed,baseline],root/'baseline-pack.log')
    results=read_json(root/'results.json') if a.resume and (root/'results.json').exists() else []
    completed={(r['architecture'],r['case']) for r in results}
    for arch in ('Win32','x64'):
        tool=repo/f'bin/{arch}/Release/e-packager.exe'
        for name,pages,expected in cases():
            if (arch,name) in completed:continue
            folder=root/arch/name;folder.mkdir(parents=True,exist_ok=a.resume)
            ws=folder/'workspace';run(decoder,['unpack',baseline,ws],folder/'unpack.log')
            meta=read_json(ws/'project/_meta.json')
            removed={entry['key'] for entry in meta['sourceFiles'] if Path(entry['relativePath']).name not in pages}
            for entry in meta['sourceFiles']:
                if entry['key'] in removed:(ws/entry['relativePath']).unlink()
            meta['sourceFiles']=[entry for entry in meta['sourceFiles'] if entry['key'] not in removed]
            meta['rootChildKeys']=[key for key in meta['rootChildKeys'] if key not in removed]
            write(ws/'project/_meta.json',json.dumps(meta,ensure_ascii=False,indent=2))
            for file,text in pages.items():write(ws/'src'/file,text)
            packed=folder/'edited.e';run(tool,['pack',ws,packed],folder/'pack.log')
            decoded=folder/'decoded';run(decoder,['unpack',packed,decoded],folder/'decode.log')
            actual_files=set()
            for file,text in pages.items():
                if file.startswith('.'):
                    actual_file=file
                else:
                    declaration=next(line for line in text.splitlines() if line.startswith('.程序集 '))
                    actual_file=declaration.split(' ',1)[1].split(',')[0]+'.txt'
                actual_files.add(actual_file)
                actual=(decoded/'src'/actual_file).read_text(encoding='utf-8-sig')
                normalize=lambda s:''.join(s.replace('＋','+').split())
                if file in ('.常量.txt','.全局变量.txt'):
                    assert sorted(normalize(l) for l in text.splitlines() if l.strip())==sorted(normalize(l) for l in actual.splitlines() if l.strip()),(name,file)
                else:assert normalize(text)==normalize(actual),(name,file)
            assert actual_files=={p.name for p in (decoded/'src').glob('*.txt')}
            artifact=folder/'edited.exe'
            compile_project(a.launcher,a.ide,packed,artifact,'auto')
            startup=subprocess.STARTUPINFO();startup.dwFlags=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
            r=subprocess.run([str(artifact)],capture_output=True,timeout=15,startupinfo=startup)
            assert r.returncode==expected,(arch,name,expected,r.returncode)
            results.append(dict(architecture=arch,case=name,headless=True,artifact_verified=True,runtime_exit=r.returncode,passed=True))
            write(root/'results.json',json.dumps(results,indent=2))
            print('PASS',arch,name,'runtime='+str(r.returncode),flush=True)
    print('Results:',root,'cases=',len(results))

if __name__=='__main__':main()
