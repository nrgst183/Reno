#!/usr/bin/env python3
"""A rough check of a VEGAS script without a C# compiler (none is available on the build machine):
  1. braces, brackets and parentheses balance (strings and comments left out);
  2. no syntax newer than C# 5 (VEGAS Pro 18 compiles scripts with the old compiler);
  3. every name written after a dot is one that VEGAS's API (the dump made by tests/scripting/dump_api.ps1), the .NET
     classes the script uses, or the script itself knows - so a typo like Envelopes.AddEnvelope shows up.
Usage: csheck.py script.cs api_dump.txt     (exit code: number of problems)
This does not replace compiling: types and overloads are not checked."""
import re, sys

src = open(sys.argv[1], encoding='utf-8').read()
api = open(sys.argv[2], encoding='utf-8').read()

# ---- strip comments, strings and chars (keeping line structure)
out = []
i, n = 0, len(src)
line = 1
problems = []
while i < n:
    c = src[i]
    if src.startswith('//', i):
        j = src.find('\n', i)
        j = n if j < 0 else j
        i = j
        continue
    if src.startswith('/*', i):
        j = src.find('*/', i + 2)
        j = n if j < 0 else j + 2
        out.append('\n' * src.count('\n', i, j))
        i = j
        continue
    if c == '@' and src.startswith('@"', i):
        j = i + 2
        while j < n:
            if src[j] == '"':
                if src.startswith('""', j):
                    j += 2
                    continue
                break
            j += 1
        out.append('""' + '\n' * src.count('\n', i, j))
        i = j + 1
        continue
    if c == '"':
        j = i + 1
        while j < n and src[j] != '"':
            if src[j] == '\\':
                j += 1
            if src[j] == '\n':
                problems.append('unterminated string near line %d' % (src.count('\n', 0, i) + 1))
                break
            j += 1
        out.append('""')
        i = j + 1
        continue
    if c == "'":
        j = i + 1
        while j < n and src[j] != "'":
            if src[j] == '\\':
                j += 1
            j += 1
        out.append("'x'")
        i = j + 1
        continue
    out.append(c)
    i += 1
code = ''.join(out)

# ---- 1. balance
stack = []
pairs = {')': '(', ']': '[', '}': '{'}
ln = 1
for ch in code:
    if ch == '\n':
        ln += 1
    elif ch in '([{':
        stack.append((ch, ln))
    elif ch in ')]}':
        if not stack or stack[-1][0] != pairs[ch]:
            problems.append('unbalanced %s at line %d' % (ch, ln))
            break
        stack.pop()
if stack:
    problems.append('unclosed %s from line %d' % stack[-1])

# ---- 2. newer syntax
newer = [(r'\?\.', 'null-conditional ?.'), (r'\$"', 'string interpolation'), (r'\bnameof\s*\(', 'nameof'), (r'\bout\s+var\b', 'out var'),
         (r'\bis\s+[A-Za-z_][\w<>\.]*\s+[a-z_]\w*\s*[\)&|;]', 'pattern matching is T x'), (r'\)\s*=>\s*[^{(]', None),
         (r'\bawait\b', 'await'), (r'\basync\b', 'async'), (r'\bvar\s*\(', 'tuple var'), (r'\?\[', 'null-conditional index'),
         (r'\bwhen\s*\(', 'exception filter'), (r'\bin\s+\w+\)\s*\{', None)]
for pat, what in newer:
    if what is None:
        continue
    for m in re.finditer(pat, code):
        problems.append('%s at line %d' % (what, code.count('\n', 0, m.start()) + 1))
# expression-bodied members: ") =>" at the start of a member (lambdas are inside parentheses/assignments)
for m in re.finditer(r'^\s*(public|private|protected|internal|static)[^;{}=(]*\([^;{}()]*\)\s*=>', code, re.M):
    problems.append('expression-bodied member at line %d' % (code.count('\n', 0, m.start()) + 1))
for m in re.finditer(r'\b(public|private)\s+[\w<>\[\]]+\s+\w+\s*\{\s*get;\s*(private\s+)?set;\s*\}\s*=', code):
    problems.append('auto-property initializer at line %d' % (code.count('\n', 0, m.start()) + 1))

# ---- 3. names after dots
known = set(re.findall(r'[A-Za-z_][A-Za-z_0-9]*', api))
dotnet = '''Add AddRange AppendLine Append Combine Contains ContainsKey Count CompareOrdinal CreateDirectory Exists Delete Dispose DoEvents Empty
Equals Exists File Folder GetDirectoryName GetExtension GetFileName GetFileNameWithoutExtension GetFiles GetHashCode GetMethod GetProperty GetField GetType
GetValue IndexOf IsNullOrEmpty Insert Invoke Join Length Max Min Name Next NextDouble Now Parse Remove Replace Round Sort Split StartsWith EndsWith Substring ToArray
ToString ToLowerInvariant ToUpperInvariant Trim TrimEnd Trunc TryGetValue Write WriteAllText WriteLine ReadAllLines Close Flush AutoFlush Text Left Top Width Height
Checked Controls Enabled Show ShowDialog DialogResult OK Cancel Click Description SelectedPath AcceptButton CancelButton StartPosition CenterScreen FormBorderStyle
FixedDialog MaximizeBox MinimizeBox ShowInTaskbar ClientSize ControlBox Minimum Maximum Value Size Application Abs Sin Cos Floor Ceiling Sqrt Pow IsNaN IsInfinity
ASCII Encoding UTF8 UTF8Encoding Environment SpecialFolder MyDocuments Directory Path Invariant InvariantCulture ReadOnly Ordinal OrdinalIgnoreCase
Items Key Values Keys Pi Reflection Collections Generic Globalization IO Text Windows Forms System Drawing Imaging Drawing2D Color FromArgb Rectangle Pen Font FontStyle
GetFolderPath PI Bold SolidBrush Brush Graphics FromImage Clear FillEllipse DrawRectangle DrawLine DrawString Bitmap Save PixelFormat Format32bppArgb ImageFormat Png LinearGradientBrush Yellow White
ParameterType GetRange Message TotalMinutes Exception FileMode Create BinaryWriter FileStream StreamWriter MessageBoxButtons MessageBoxIcon Information Warning YesNo OK
Label CheckBox RadioButton Button TextBox NumericUpDown GroupBox FolderBrowserDialog Form Control Radio Checked ToDigit IsLetterOrDigit Convert ToDouble ToBoolean ToInt32
Nanos TrimStart Sleep Thread Func Action List Dictionary HashSet KeyValuePair Enum GetValues typeof Compare String Math Char IEnumerable IEnumerator
SetValue FieldInfo PropertyInfo MemberInfo BindingFlags Instance Public Static Name Label Rating'''.split()
known |= set(dotnet)
# the script's own members
known |= set(re.findall(r'\b(?:void|bool|int|double|string|float|long|Media|Effect|JObj|JArr|VideoTrack|AudioTrack|Envelope|Timecode|object|static)\s+([A-Za-z_]\w*)\s*[\(;=,]', code))
known |= set(re.findall(r'\b(?:public|private|static|const|readonly)\s+[\w<>\[\],]+\s+([A-Za-z_]\w*)', code))
known |= set(re.findall(r'\b([A-Za-z_]\w*)\s*=\s*', code))   # fields set in initialisers (Folder = ...)
unknown = {}
for m in re.finditer(r'(?<=[A-Za-z_\)\]>])\.\s*([A-Za-z_]\w*)', code):
    name = m.group(1)
    if name not in known:
        unknown.setdefault(name, code.count('\n', 0, m.start()) + 1)
for name, ln in sorted(unknown.items(), key=lambda kv: kv[1]):
    problems.append('unknown member ".%s" (first at line %d)' % (name, ln))

for p in problems:
    print('PROBLEM:', p)
print('%d problems' % len(problems))
sys.exit(min(len(problems), 100))
