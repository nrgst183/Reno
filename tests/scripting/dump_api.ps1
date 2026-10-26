# Lists the public classes, properties, methods, fields and enumerations of a .NET assembly (dump_api.cs), as
# gen_unsupported.py reads them:   pwsh dump_api.ps1 <assembly.dll> <out.txt>
param([string]$dll, [string]$out)
Add-Type -Path (Join-Path $PSScriptRoot 'dump_api.cs') -ReferencedAssemblies System.Reflection.Metadata,System.Collections.Immutable,System.Linq,System.Collections,System.Runtime,netstandard
[System.IO.File]::WriteAllText($out, [Dumper]::Run($dll))
