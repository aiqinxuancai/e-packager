# Edited-source roundtrip audit

## Final acceptance, 2026-09-11

Completed for Jingyi 11.1.5 with Release Win32 and x64:

- 105 source pages independently edited per architecture, 210/210 passed.
- Every case passed semantic repacking, all source/resource comparisons and
  actual IDE ecom compilation with a verified nonempty artifact.
- All 105 architecture pairs produced byte-identical repacked `.e` files.
- Two additional cases edited all 105 pages together and passed the same
  checks, giving 212 successful IDE compilation cases in total.
- Resource indexes and original-input SHA256 were independently verified.
- Final all-pages-edited `.e`: 4,567,832 bytes, SHA256
  `544BA30A443C291CEE8B90170DC706BDE47D2B628D16843D3B2641BFAD38336E`.
- Deliverable: `temp/jingyi-final-all-pages/精易模块v11.1.5[源码]-回包.e`.
- Aggregate results: `temp/jingyi-indexed-full-audit/results.json`.
- Independent acceptance report:
  `temp/jingyi-indexed-full-audit/verification.json`.
- Combined-edit evidence: `temp/jingyi-final-indexed/results.json`.

Both Release builds passed. Support properties/member indexes, scoped calls,
inheritance, local recursion and support command parity regressions passed.
The historical investigations below are retained as failure/fix records;
their interrupted runs are not included in the final 210-case count.

This establishes the requested page-mutation coverage on this project, not
exhaustive coverage of every Easy Language construct or runtime behavior.

Run from the repository root after building Release Win32 and x64:

```powershell
& tools/TestEditedSourceRoundTrip.ps1 -InputFile 'path/to/project.e'
```

The script copies the input to a fresh temporary directory, unpacks it with
Win32 (to read installed x86 support libraries), and checks an unchanged
SHA256 roundtrip for each architecture. It then appends a unique comment and
blank line to each source page independently. Every successful pack must
produce different bytes, survive another unpack, preserve source text apart
from indentation and blank lines, and retain all resource file bytes.
Missing and unexpected files are rejected. Resource index contents are checked
separately by `VerifyEditedSourceAudit.ps1`. Results and logs are
retained, including failures. The original input is never written.

## Jingyi 11.1.5 audit, 2026-09-11

- Input: 4,559,569 bytes; 105 source pages, approximately 78,168 source lines.
- SHA256: `BEB59C22A4427ACB72F4E9D4E1B5C8B26C399286C81A9754EB2D575745DE585F`.
- Unchanged roundtrip: identical SHA256 on Win32 and x64.
- Edited pages: 105 attempts on each architecture, 210 total; zero passed.
- Logs: `temp/jingyi-edited-win32/results.json` and
  `temp/jingyi-edited-x64/results.json`.
- First discovered defect was fixed: global and DLL declarations were
  materialized after method encoding. They are now built first and registered
  in the encoding context; local variables take precedence over globals.
- `TestSemanticDeclarations.ps1` passes on both architectures, covering
  globals, DLL declarations, structures, forward calls and local shadowing.
- Current shared blocker: `集_组件.画板_去背景色`, source page
  `src/集_组件/集_组件.txt:16`, expression
  `-16777216 == 画板.画板背景色`.
- Native member resolution only consults user-defined members. Support
  library properties are not registered. This is an implementation gap, not
  evidence of incorrect user edits. x64 additionally reports that the local
  x86 support library cannot be loaded.

These are 210 attempts, NOT 210 independently covered code paths. Changed
source triggers whole-project semantic rebuilding, and the common first
failure prevents testing later methods. A passing unchanged roundtrip can
reuse the original native snapshot and is not proof of semantic encoding.

## Remaining coverage

The checkpoints in this section describe the investigation chronologically.
The final acceptance above supersedes their then-pending status.

### IDE validation checkpoint

After the constant-comment and operator-precedence fixes, the one-page edited
roundtrip passes on both architectures (`temp/jingyi-precedence-probe`). A new
full audit (`temp/jingyi-all-edited-final`) passed the first seven Win32 pages
before being stopped: IDE validation of the first rebuilt file terminated with
`0xC0000409` and `headless_result_missing`. The untouched input copy compiles
successfully to an ecom with the same launcher and IDE. The small regression
fixture also compiles successfully. This is unresolved and blocks acceptance;
source equivalence alone is insufficient. The interrupted audit is incomplete
and must be rerun after identifying the native-format defect.

### IDA and x32dbg reference-table investigation

The debugger connection was restored and the failing rebuilt Jingyi project
was reproduced in a separate IDE process (PID 53624). The first access fault
was at `e5.95.exe:0x463CD0`, reading an invalid pointer `0x39C87CAD`.
IDA identifies the caller as `sub_463840`: its variable-reference loop at
`0x463B99` accepts `0x1D` directly and otherwise assumes a call header.
The offending reference pointed at `expressionData + 0xAB5`, a nested `0x38`
array-index variable. Treating its bytes as call-header lengths produced the
invalid pointer. This is an encoder defect, not an incorrect source edit.

The same function's constant-reference loop at `0x463BF3` reads a project
constant ID at `offset + 1`; support constants (`0x1C`) and enumerations
(`0x23`) must not enter this table. The encoder now registers only `0x1B`
constants and omits standalone references for nested `0x38` index variables.

Nested variable indexes now reuse the recursive variable-chain encoder.
The decoder also respects each `0x38 ... 0x37` boundary instead of attaching
inner members/indexes to the outer variable. `TestSupportProperties.ps1`
checks nested property indexes, library constants and reference-table markers
on both architectures (`temp/support-reference-check3`, passed).

Release Win32 and x64 builds passed. The two-architecture Jingyi one-page
probe passed (`temp/jingyi-reference-probe`). IDE compilation no longer hits
the observed reference-table crash, but now reports error 10002, method
`写到文件` not found (`temp/jingyi-reference-probe/compile-result.json`).
The context builder currently puts methods from unrelated classes into the
unqualified function map; resolving scope and same-name calls is the next
investigation. Full 105-page-per-architecture acceptance is still pending.

The subsequent probe after the nested decoder fix also passed on both
architectures (`temp/jingyi-reference-decoder-probe`). The original SHA256
remains unchanged.

### Scoped calls and verified full audit

IDA `sub_4E9790` confirms that unqualified methods are looked up in the
current owner and its base classes, then in non-class owners. The encoder
previously registered every class method as a global function. It now keeps
the scopes separate and uses declared argument counts when choosing a local
method versus a same-name global/support command. The original Jingyi method
`写到文件` contains a two-argument core command call, while its destructor
uses the same-name optional-argument class method with no arguments.

Ordinary assemblies with comments were also misclassified as classes because
the comment requires an empty base-class column. Root classes are now exported
with an explicit `<对象>` base, and empty placeholder columns in longer
assembly headers no longer imply a class. Snapshot shape parsing uses the
same distinction. Legacy two-column empty-base class headers remain accepted.

x32dbg then identified another error 10002: `取列数` on a local variable named
`外部超级列表框` was encoded as a class-qualified call. Variable receivers now
take precedence over same-name class qualifiers. `TestFunctionScopes.ps1`
checks native call targets, receiver flags, and assembly/class IDs on both
architectures (`temp/function-scopes-check3`, passed).

Jingyi's edited full semantic rebuild now passes IDE ecom compilation:
`temp/jingyi-scoped-probe3/compile-result.json` reports `ok=true` and
`compile_result.artifact_verified=true`. This is the acceptance checkpoint
required before restarting the full per-page audit.

The full audits are running separately in
`temp/jingyi-all-pages-verified-win32` and
`temp/jingyi-all-pages-verified-x64`. Each case now also invokes the IDE,
requires a successful result report and a verified nonempty ecom artifact,
and records `CompilePassed` in `results.json`. These paths are live progress
records, not completion claims.

The first full audit was stopped after 15 Win32 and 16 x64 cases when binary
comparison revealed different native support-member command IDs on x64.
Text export had retained only member names, so different types' `清除`
commands all resolved to the last command with that name. IDE compilation can
repair such calls by name, masking the encoder defect. The support-library
text now exports `命令索引` for each command and type member; the importer uses
that index. Legacy text is matched only when its complete header signature is
unique, never by the last same-name command.

`temp/support-command-index-check3` covers same-name `清除` on a drawing
control, data source and COM object, and passes identical native/text-backed
output on both architectures. `temp/jingyi-final-indexed` modifies all 105
pages together, passes source/resource and IDE checks on both architectures,
and produces identical SHA256:
`544BA30A443C291CEE8B90170DC706BDE47D2B628D16843D3B2641BFAD38336E`.

The final independent-page audit runs in `temp/jingyi-indexed-full-audit`,
using four disjoint page ranges per architecture and separate workspaces.
`tools/TestEditedSourcePartitions.ps1` records process IDs and partitions in
`workers.json` and emits aggregate `results.json` only after all partitions
exit successfully with the expected case counts. Earlier interrupted roots
are historical records and do not establish completion.

```powershell
& tools/TestEditedSourceRoundTrip.ps1 -InputFile 'path/to/project.e' `
  -Architectures Win32 -CompileIde 'path/to/e5.95.exe' `
  -CompileLauncher 'path/to/AutoLinkerTest.exe'
```

### Support property follow-up

Support-library property and structure members are now registered from native
library metadata and exported text metadata, retaining one-based indexes and
member types. The SDK's eight fixed control properties are used when a control
does not publish its own property table. `TestSupportProperties.ps1` exercises
reads, writes, first/last properties, font member chains and array access, and
rejects unknown and read-only properties. It compares decoded source and the
bytes produced through native versus exported metadata.

The two-architecture Jingyi probe in `temp/jingyi-properties-probe/results.json`
now passes the background-color expression and stops at the next independent
gap: `画板.取窗口句柄()`, a common control method. This is not a successful
whole-project roundtrip. The earlier 210-case results remain historical records.

Continue resolving common control methods and subsequent failures before
repeating the full audit, without copying old method bodies as a fallback.

This test does not yet establish complete parser coverage, resource-index
semantic equivalence, executable behavior, IDE compilation, or individual
method mutation coverage. Fullwidth punctuation needs separate token-level
tests, including preservation inside strings and comments.
