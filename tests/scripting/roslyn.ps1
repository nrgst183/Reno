# Compiles C# for tests/scripting with Roslyn (the compiler inside PowerShell 7): Reno's ScriptPortal.Vegas.cs as C# 5
# (the language of the .NET Framework's csc.exe, which compiles it on users' machines) and the test scripts.
#   pwsh roslyn.ps1 <out.dll> <assembly name> <5|latest> <defines a;b> <references a;b> <sources a;b>
#   pwsh roslyn.ps1 -jobs <file>     each line: out|name|5 or latest|defines|references|sources (one PowerShell for all)
param([string]$out, [string]$name, [string]$lang, [string]$defines, [string]$refs, [string]$sources, [string]$jobs)
$ErrorActionPreference = 'Stop'
$ca = [System.Reflection.Assembly]::LoadFrom((Join-Path $PSHOME 'Microsoft.CodeAnalysis.dll'))
$cs = [System.Reflection.Assembly]::LoadFrom((Join-Path $PSHOME 'Microsoft.CodeAnalysis.CSharp.dll'))
$runtime = [System.Collections.Generic.List[Microsoft.CodeAnalysis.MetadataReference]]::new()
$rd = Split-Path ([object].Assembly.Location)
foreach ($f in Get-ChildItem $rd -Filter *.dll) {
    $n = $f.Name
    if (($n.StartsWith('System.') -or $n -eq 'mscorlib.dll' -or $n -eq 'netstandard.dll' -or $n -eq 'Microsoft.CSharp.dll') -and $n -ne 'System.Windows.Forms.dll' -and $n -ne 'System.Management.Automation.dll') {
        try { $runtime.Add([Microsoft.CodeAnalysis.MetadataReference]::CreateFromFile($f.FullName)) } catch { }
    }
}
function Compile([string]$out, [string]$name, [string]$lang, [string]$defines, [string]$refs, [string]$sources) {
    $ver = if ($lang -eq '5') { [Microsoft.CodeAnalysis.CSharp.LanguageVersion]::CSharp5 } else { [Microsoft.CodeAnalysis.CSharp.LanguageVersion]::Latest }
    $syms = [string[]]@($defines.Split(';', [System.StringSplitOptions]::RemoveEmptyEntries))
    $opts = [Microsoft.CodeAnalysis.CSharp.CSharpParseOptions]::new($ver, [Microsoft.CodeAnalysis.DocumentationMode]::Parse, [Microsoft.CodeAnalysis.SourceCodeKind]::Regular, $syms)
    $trees = [System.Collections.Generic.List[Microsoft.CodeAnalysis.SyntaxTree]]::new()
    foreach ($s in $sources.Split(';', [System.StringSplitOptions]::RemoveEmptyEntries)) {
        $text = [System.IO.File]::ReadAllText($s)
        $trees.Add([Microsoft.CodeAnalysis.CSharp.CSharpSyntaxTree]::ParseText($text, $opts, $s, [System.Text.Encoding]::UTF8, [System.Threading.CancellationToken]::None))
    }
    $all = [System.Collections.Generic.List[Microsoft.CodeAnalysis.MetadataReference]]::new($runtime)
    foreach ($r in $refs.Split(';', [System.StringSplitOptions]::RemoveEmptyEntries)) { $all.Add([Microsoft.CodeAnalysis.MetadataReference]::CreateFromFile($r)) }
    $copts = [Microsoft.CodeAnalysis.CSharp.CSharpCompilationOptions]::new([Microsoft.CodeAnalysis.OutputKind]::DynamicallyLinkedLibrary)
    $comp = [Microsoft.CodeAnalysis.CSharp.CSharpCompilation]::Create($name, $trees, $all, $copts)
    $res = [Microsoft.CodeAnalysis.FileSystemExtensions]::Emit($comp, $out, [NullString]::Value, [NullString]::Value, [NullString]::Value, $null, [System.Threading.CancellationToken]::None)
    $errs = @($res.Diagnostics | Where-Object { $_.Severity -eq [Microsoft.CodeAnalysis.DiagnosticSeverity]::Error })
    if ($errs.Count -gt 0) { $errs | Select-Object -First 40 | ForEach-Object { [Console]::WriteLine($_.ToString()) }; return $false }
    $warns = @($res.Diagnostics | Where-Object { $_.Severity -eq [Microsoft.CodeAnalysis.DiagnosticSeverity]::Warning -and $_.Id -ne 'CS1701' -and $_.Id -ne 'CS1702' })
    $warns | Select-Object -First 20 | ForEach-Object { [Console]::WriteLine('warning: ' + $_.ToString()) }
    return $true
}
if ($jobs) {
    $failed = 0
    foreach ($line in [System.IO.File]::ReadAllLines($jobs)) {
        if ($line.Trim().Length -eq 0) { continue }
        $p = $line.Split('|')
        $ok = Compile $p[0] $p[1] $p[2] $p[3] $p[4] $p[5]
        if (-not $ok) { [Console]::WriteLine('COMPILE FAILED: ' + $p[5]); $failed++ }
    }
    if ($failed -gt 0) { exit 1 }
    exit 0
}
$ok = Compile $out $name $lang $defines $refs $sources
if (-not $ok) { exit 1 }
exit 0
