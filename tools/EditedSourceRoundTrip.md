# Edited-source roundtrip audit

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
Missing and unexpected files are rejected. Resource indexes are checked for
existence, but their contents are not yet compared. Results and logs are
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

Implement typed support-library members in both native and exported metadata
paths, using the documented decoder representation (one-based member index
plus owner type ID). Validate property reads, writes and chained access with
focused fixtures before repeating this audit. Continue resolving subsequent
failures without copying old method bodies as a fallback.

This test does not yet establish complete parser coverage, resource-index
semantic equivalence, executable behavior, IDE compilation, or individual
method mutation coverage. Fullwidth punctuation needs separate token-level
tests, including preservation inside strings and comments.
