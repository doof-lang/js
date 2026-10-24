# Builds lib\quickjs.lib and installs the public QuickJS header into include\.
# Runs inside a private copy of vendor\ with CC, AR, and CFLAGS supplied by
# the Doof compiler from a Visual Studio developer environment.
$ErrorActionPreference = 'Stop'

$library = 'quickjs.lib'
$defines = @('/D_GNU_SOURCE', '/DQUICKJS_NG_BUILD')
$includes = @('/Iquickjs')
$sources = 'dtoa', 'libregexp', 'libunicode', 'quickjs' | ForEach-Object { "quickjs\$_.c" }

function Install-Headers { Copy-Item quickjs\quickjs.h include\ }

New-Item -ItemType Directory -Force -Path include, lib, obj | Out-Null
Install-Headers

$flags = @()
if ($env:CFLAGS) { $flags = $env:CFLAGS -split ' ' | Where-Object { $_ -ne '' } }
$objects = @()
foreach ($source in $sources) {
  $object = 'obj\' + ($source -replace '[\\/]', '_' -replace '\.c$', '.obj')
  & $env:CC @flags @defines @includes /TC /c $source "/Fo$object"
  if ($LASTEXITCODE -ne 0) { throw "Compiling $source failed" }
  $objects += $object
}

& $env:AR /nologo "/OUT:lib\$library" @objects
if ($LASTEXITCODE -ne 0) { throw "Archiving $library failed" }
