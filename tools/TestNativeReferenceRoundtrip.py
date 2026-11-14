"""独立读取原生表达式，验证语义回包后历史符号引用的 ID、成员所有者和次数不变。"""
import argparse
import base64
from collections import Counter
import json
from pathlib import Path
import struct

class Indexer:
 def __init__(self,data):self.data=data;self.pos=0;self.lines=[];self.blocks=[];self.calls=[]
 def get(self,n):
  b=self.data[self.pos:self.pos+n];assert len(b)==n,(self.pos,n);self.pos+=n;return b
 def u8(self):return self.get(1)[0]
 def i32(self):return struct.unpack('<i',self.get(4))[0]
 def bstr(self):return self.get(self.i32())
 def params(self):
  while self.expr()!=1:pass
 def call(self):
  start=self.pos;method=self.i32();lib,flags=struct.unpack('<hh',self.get(4));a=self.bstr();b=self.bstr();self.calls.append(dict(offset=start-1,method=method,lib=lib,flags=flags,text=a.rstrip(b'\0').decode('gbk',errors='replace'),comment=b.rstrip(b'\0').decode('gbk',errors='replace')))
  token=self.u8()
  if token==0x38:self.pos-=1;self.expr()
  else:assert token==0x36,(start,hex(token))
  self.params();return method
 def expr(self,member=True):
  token=self.u8()
  while token in [0x1d,0x37]:token=self.u8()
  chain=False
  if token in [1,0x16,0x20]:pass
  elif token in [0x17,0x19,0x23]:self.get(8)
  elif token==0x18:self.get(2)
  elif token==0x1a:self.bstr()
  elif token in [0x1b,0x1c,0x1e,0x3b]:self.get(4)
  elif token==0x1f:
   while self.expr()!=0x20:pass
  elif token==0x21:self.call();chain=True
  elif token==0x38:
   ident=self.i32();chain=True
   if ident==0:assert self.u8()==0x3a;self.expr(False)
  else:raise ValueError((self.pos-1,hex(token)))
  if chain and (member or token==0x38):
   while self.pos<len(self.data):
    n=self.data[self.pos]
    if n==0x39:self.get(9)
    elif n==0x3a:self.get(1);self.expr(False)
    elif n==0x37:
     if token==0x38:self.get(1)
     break
    else:break
  return token
 def expect(self,n):
  got=self.u8();assert got==n,(self.pos-1,hex(got),hex(n))
 def block(self,stops=()):
  while self.pos<len(self.data):
   token=self.data[self.pos]
   if token in stops:return
   start=self.pos;self.pos+=1
   if token in [0x55,0x72,0x73,0x74]:continue
   entry=None
   if token in [0x6b,0x6c,0x6d,0x70]:entry=[{0x6b:1,0x6c:2,0x6d:4,0x70:3}[token],start,None];self.blocks.append(entry)
   if token==0x6d:
    while self.data[self.pos]==0x6e:
     self.lines.append(self.pos);self.pos+=1;self.call();self.block([0x53]);self.expect(0x53)
    self.expect(0x6f);self.block([0x54]);self.expect(0x54);entry[2]=self.pos;self.expect(0x74)
   elif token in [0x6a,0x6b,0x6c,0x70,0x71]:
    self.lines.append(start);self.call()
    if token==0x6b:self.block([0x50]);self.expect(0x50);self.block([0x51]);self.expect(0x51);entry[2]=self.pos;self.expect(0x72)
    elif token==0x6c:self.block([0x52]);self.expect(0x52);entry[2]=self.pos;self.expect(0x73)
    elif token==0x70:self.block([0x55,0x71]);self.expect(0x55);entry[2]=self.pos;self.lines.append(self.pos);self.expect(0x71);self.call()
   else:raise ValueError(('statement',self.pos-1,hex(token)))
 def run(self):
  self.block();assert self.pos==len(self.data)
  return b''.join(struct.pack('<i',i) for i in self.lines),b''.join(struct.pack('<Bii',*b) for b in self.blocks)

class Ast(Indexer):
 def params(self):
  out=[]
  while True:
   t,x=self.expr()
   if t==1:return out
   out.append(x)
 def call(self):
  method=self.i32();lib,flags=struct.unpack('<hh',self.get(4));text=self.bstr().rstrip(b'\0');comment=self.bstr()
  token=self.u8();target=None
  if token==0x38:self.pos-=1;_,target=self.expr()
  else:assert token==0x36
  args=self.params()
  return ['call',lib,method,flags,text.hex(),target,args]
 def expr(self,member=True):
  token=self.u8()
  while token in [0x1d,0x37]:token=self.u8()
  chain=False
  if token in [1,0x16,0x20]:value=None
  elif token in [0x17,0x19,0x23]:value=self.get(8).hex()
  elif token==0x18:value=self.get(2).hex()
  elif token==0x1a:value=self.bstr().hex()
  elif token in [0x1b,0x1c,0x1e,0x3b]:value=self.i32()
  elif token==0x1f:
   value=[]
   while True:
    t,x=self.expr()
    if t==0x20:break
    value.append(x)
  elif token==0x21:value=self.call();chain=True
  elif token==0x38:
   ident=self.i32();value=ident;chain=True
   if ident==0:assert self.u8()==0x3a;value=self.expr(False)[1]
  else:raise ValueError((self.pos-1,hex(token)))
  links=[]
  if chain and (member or token==0x38):
   while self.pos<len(self.data):
    n=self.data[self.pos]
    if n==0x39:self.get(1);links.append(['member',self.i32(),self.i32()])
    elif n==0x3a:self.get(1);links.append(['index',self.expr(False)[1]])
    elif n==0x37:
     if token==0x38:self.get(1)
     break
    else:break
  return token,[token,value,links]
 def run(self):
  events=[]
  while self.pos<len(self.data):
   token=self.u8()
   if token in [0x6a,0x6b,0x6c,0x6e,0x70,0x71]:
    call=self.call()
    # Pure comments/blank editor rows carry no executable operation.
    if token==0x6a and call[1:3]==[-1,0] and call[3] in [0,32]:continue
    events.append([token,call])
   else:
    assert token in [0x50,0x51,0x52,0x53,0x54,0x55,0x6d,0x6f,0x72,0x73,0x74],hex(token)
    events.append(token)
  return events


def references(node, counts, unexamined):
    if not isinstance(node, list) or not node:
        return
    if node[0] == 'call':
        if node[1] != -1:
            counts[('function', node[2], max(0, node[1]))] += 1
        if node[3] & 64 and node[4]:
            unexamined.append(bytes.fromhex(node[4]).decode('gb18030', 'replace'))
    elif node[0] == 'member':
        counts[('member', node[1], node[2])] += 1
    elif node[0] == 56 and isinstance(node[1], int):
        counts[('variable', node[1], 0)] += 1
    elif node[0] == 27:
        counts[('constant', node[1], 0)] += 1
    elif node[0] == 30:
        counts[('function', node[1], 0)] += 1
    for item in node:
        references(item, counts, unexamined)


def load_methods(path):
    return {m['id']: m for page in json.loads(path.read_text(encoding='utf-8-sig')) for m in page['methods']}


def audit(case):
    before = load_methods(case / 'workspace/project/.native_source_map.json')
    after = load_methods(case / 'decoded/project/.native_source_map.json')
    result = dict(case=str(case), checked_methods=0, checked_references=0, mismatches=[], errors=[],
                  original_unexamined=0, rebuilt_unexamined=0, new_unexamined=[])
    for ident, method in before.items():
        refs = method.get('identityReferences', method.get('retiredReferences', [])) + [dict(kind='variable', ownerType=0, **r)
                                                       for r in method.get('unresolvedVariables', [])]
        try:
            counts, texts = [], []
            for current in (method, after[ident]):
                count, raw = Counter(), []
                references(Ast(base64.b64decode(current['expressionData'])).run(), count, raw)
                counts.append(count)
                texts.append(raw)
            result['original_unexamined'] += len(texts[0])
            result['rebuilt_unexamined'] += len(texts[1])
            for text, count in (Counter(texts[1]) - Counter(texts[0])).items():
                result['new_unexamined'].append(dict(method=ident, name=method['name'], source=text, count=count))
            if refs:
                result['checked_methods'] += 1
            for ref in refs:
                owner = max(0, ref.get('library', -2)) if ref['kind'] == 'function' else ref.get('ownerType', 0)
                key = (ref['kind'], ref['id'], owner)
                result['checked_references'] += 1
                if counts[0][key] != counts[1][key]:
                    result['mismatches'].append(dict(method=ident, name=method['name'], reference=ref,
                                                     before=counts[0][key], after=counts[1][key]))
        except Exception as error:
            result['errors'].append(dict(method=ident, name=method['name'], error=repr(error), has_identity=bool(refs)))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    rows = [audit(case) for case in sorted((args.root / 'cases').glob('*/*'))
            if (case / 'decoded/project/.native_source_map.json').exists()]
    report = dict(cases=len(rows), checked_methods=sum(r['checked_methods'] for r in rows),
                  checked_references=sum(r['checked_references'] for r in rows),
                  mismatches=sum(len(r['mismatches']) for r in rows),
                  reader_errors=sum(len(r['errors']) for r in rows),
                  original_unexamined=sum(r['original_unexamined'] for r in rows),
                  rebuilt_unexamined=sum(r['rebuilt_unexamined'] for r in rows),
                  new_unexamined=sum(len(r['new_unexamined']) for r in rows))
    (args.root / 'reference-audit.json').write_text(json.dumps(dict(summary=report, cases=rows),
        ensure_ascii=False, indent=2), encoding='utf-8-sig')
    print(json.dumps(report, ensure_ascii=False))
    return 1 if report['mismatches'] or any(e['has_identity'] for r in rows for e in r['errors']) else 0


if __name__ == '__main__':
    raise SystemExit(main())
