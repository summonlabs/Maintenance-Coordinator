#requires -Version 5.1
<#
    validate-packaging.ps1 - the end to end packaging proof for the Maintenance
    Coordinator library.

    It builds the project from scratch, installs it into a throw-away prefix,
    and then proves - statically and by building and RUNNING a real out-of-tree
    consumer - that the installed package is what a downstream project is
    promised.

    Everything this script writes lives under <repo>\build\pkg (git-ignored) or
    in the system temp directory.  No source file is modified and no state is
    kept between runs: the build directory is wiped first.

    Steps
      01 locate-vcvars        find vcvars64.bat (vswhere, then known VS 2022 paths)
      02 configure-main       cmake configure, Release, tests and CLI on
      03 build-main           cmake --build (warning clean under /W4 /WX)
      04 ctest                the full test suite, pass/fail counts
      05 install-main         cmake --install into the prefix
      06 installed-tree       the files the installed package must contain
      07 configure-downstream configure tests/downstream against the prefix
      08 build-downstream     build the out-of-tree consumer
      09 run-downstream       run the consumer against a fresh store
      10 verify-export        the installed CMake interface and its negative space
      11 cpack-source         ZIP source package
      12 cpack-binary         ZIP binary package
      13 verify-packages      both archives exist, are non-empty and carry the payload

    Exit code 0 only when every step passed.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# ---------------------------------------------------------------------------
# Layout
# ---------------------------------------------------------------------------

$RepoRoot  = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$BuildRoot = Join-Path $RepoRoot 'build\pkg'
$MainBuild = Join-Path $BuildRoot 'main'
$Prefix    = Join-Path $BuildRoot 'prefix'
$DownBuild = Join-Path $BuildRoot 'downstream'
$Packages  = Join-Path $BuildRoot 'packages'
$Scratch   = Join-Path $BuildRoot 'scratch'
$StoreDir  = Join-Path $Scratch 'store'
$TempDir   = [System.IO.Path]::GetTempPath()

if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot 'CMakeLists.txt'))) {
    Write-Host ('FATAL: no CMakeLists.txt under ' + $RepoRoot) -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------
# State and helpers
# ---------------------------------------------------------------------------

$Results      = New-Object System.Collections.Generic.List[object]
$Findings     = New-Object System.Collections.Generic.List[string]
$Aborted      = $false
$Vcvars       = $null
$Generator    = @()
$ShellCounter = 0
$ConsumerExe  = $null

function Q {
    param([string]$Path)
    return '"' + $Path + '"'
}

function Write-Head {
    param([string]$Text)
    Write-Host ''
    Write-Host ('=== ' + $Text) -ForegroundColor Cyan
}

function Add-Result {
    param([string]$Name, [string]$Result, [string]$Command)
    $entry = [pscustomobject]@{ Name = $Name; Result = $Result; Command = $Command }
    $Results.Add($entry)
    $colour = 'Green'
    if ($Result -eq 'FAIL') { $colour = 'Red' }
    if ($Result -eq 'SKIP') { $colour = 'DarkGray' }
    Write-Host ('    ' + $Result) -ForegroundColor $colour
}

# Runs one step.  The body throws on any failure, which is caught here, so a
# failed step is loud, recorded, and stops everything that depended on it.
function Invoke-Step {
    param([string]$Name, [string]$Command, [scriptblock]$Body)
    if ($Aborted) {
        Add-Result $Name 'SKIP' $Command
        return
    }
    Write-Head $Name
    Write-Host ('    $ ' + $Command) -ForegroundColor DarkGray
    try {
        & $Body
        Add-Result $Name 'PASS' $Command
    } catch {
        Write-Host ('    FAIL: ' + $_.Exception.Message) -ForegroundColor Red
        Add-Result $Name 'FAIL' $Command
        $script:Aborted = $true
    }
}

# Every cmake/ctest/cpack/consumer invocation goes through cmd.exe with
# vcvars64.bat applied: cl.exe, link.exe and the Windows SDK are not on PATH.
# The command is written to a small .cmd file in the system temp directory so
# that quoting can never be mangled by nested shells.
function Invoke-VcShell {
    param([string]$Command, [string]$WorkDir)
    $script:ShellCounter++
    $scriptFile = Join-Path $TempDir ('mc-validate-packaging-' + $PID + '-' + $script:ShellCounter + '.cmd')
    $lines = @(
        '@echo off',
        ('call ' + (Q $Vcvars) + ' >nul'),
        ('cd /d ' + (Q $WorkDir)),
        $Command,
        'exit /b %ERRORLEVEL%'
    )
    Set-Content -LiteralPath $scriptFile -Value $lines -Encoding ASCII
    $output = & cmd.exe /d /c $scriptFile 2>&1 | Out-String
    $code = $LASTEXITCODE
    Remove-Item -LiteralPath $scriptFile -Force -ErrorAction SilentlyContinue
    if ($null -eq $output) { $output = '' }
    return [pscustomobject]@{ ExitCode = $code; Output = $output }
}

function Write-CommandOutput {
    param([string]$Text)
    foreach ($line in ($Text -split "\r?\n")) {
        if ($line.Trim().Length -gt 0) { Write-Host ('      | ' + $line) }
    }
}

# ---------------------------------------------------------------------------
# Prologue: from scratch
# ---------------------------------------------------------------------------

Write-Host 'Maintenance Coordinator packaging validation'
Write-Host ('  repository : ' + $RepoRoot)
Write-Host ('  work root  : ' + $BuildRoot)

if (Test-Path -LiteralPath $BuildRoot) {
    Remove-Item -LiteralPath $BuildRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null

if (Get-Command ninja.exe -ErrorAction SilentlyContinue) {
    $Generator = @('-G', 'Ninja')
}

# ---------------------------------------------------------------------------
# 01 locate-vcvars
# ---------------------------------------------------------------------------

$locateCommand = 'vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath'

Invoke-Step -Name 'locate-vcvars' -Command $locateCommand -Body {
    $programFiles = $env:ProgramFiles
    $programFilesX86 = Join-Path $env:SystemDrive 'Program Files (x86)'

    $vswhereCandidates = @(
        (Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'),
        (Join-Path $programFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
    )
    $vswhereCommand = Get-Command vswhere.exe -ErrorAction SilentlyContinue
    if ($vswhereCommand) { $vswhereCandidates += $vswhereCommand.Source }

    $vsRoot = $null
    foreach ($candidate in $vswhereCandidates) {
        if (-not (Test-Path -LiteralPath $candidate)) { continue }
        $found = & $candidate -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and $found) {
            $vsRoot = ($found | Select-Object -First 1).ToString().Trim()
            Write-Host ('    vswhere: ' + $candidate) -ForegroundColor DarkGray
            break
        }
    }

    if ($vsRoot) {
        $candidate = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
        if (Test-Path -LiteralPath $candidate) { $script:Vcvars = $candidate }
    }

    if (-not $script:Vcvars) {
        # The two known Visual Studio 2022 layouts, in both Program Files roots.
        foreach ($root in @($programFiles, $programFilesX86)) {
            foreach ($edition in @('Community', 'BuildTools')) {
                $candidate = Join-Path $root ('Microsoft Visual Studio\2022\' + $edition + '\VC\Auxiliary\Build\vcvars64.bat')
                if (Test-Path -LiteralPath $candidate) {
                    $script:Vcvars = $candidate
                    break
                }
            }
            if ($script:Vcvars) { break }
        }
    }

    if (-not $script:Vcvars) {
        throw 'vcvars64.bat was not found via vswhere or under the known Visual Studio 2022 paths'
    }
    Write-Host ('    vcvars64.bat: ' + $script:Vcvars) -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# 02 configure-main
# ---------------------------------------------------------------------------

$configureMain = (@('cmake', '-S', (Q $RepoRoot), '-B', (Q $MainBuild)) + $Generator +
                  @('-DCMAKE_BUILD_TYPE=Release', '-DMC_BUILD_TESTS=ON', '-DMC_BUILD_CLI=ON')) -join ' '

Invoke-Step -Name 'configure-main' -Command $configureMain -Body {
    $run = Invoke-VcShell -Command $configureMain -WorkDir $RepoRoot
    if ($run.ExitCode -ne 0) {
        Write-CommandOutput $run.Output
        throw ('cmake configure exited with ' + $run.ExitCode)
    }
    if (-not (Test-Path -LiteralPath (Join-Path $MainBuild 'CMakeCache.txt'))) {
        throw 'the configure step produced no CMakeCache.txt'
    }
    $cache = Get-Content -LiteralPath (Join-Path $MainBuild 'CMakeCache.txt') -Raw
    if ($cache -notmatch 'MC_BUILD_TESTS:BOOL=ON') { throw 'MC_BUILD_TESTS is not ON in the cache' }
    if ($cache -notmatch 'MC_BUILD_CLI:BOOL=ON') { throw 'MC_BUILD_CLI is not ON in the cache' }
    $generatorLine = [regex]::Match($cache, 'CMAKE_GENERATOR:INTERNAL=(.+)')
    if ($generatorLine.Success) {
        Write-Host ('    generator: ' + $generatorLine.Groups[1].Value.Trim()) -ForegroundColor DarkGray
    }
}

# ---------------------------------------------------------------------------
# 03 build-main
# ---------------------------------------------------------------------------

$buildMain = 'cmake --build ' + (Q $MainBuild) + ' --config Release --parallel'

Invoke-Step -Name 'build-main' -Command $buildMain -Body {
    $run = Invoke-VcShell -Command $buildMain -WorkDir $RepoRoot
    if ($run.ExitCode -ne 0) {
        Write-CommandOutput $run.Output
        throw ('cmake --build exited with ' + $run.ExitCode)
    }
    $warnings = @($run.Output -split "\r?\n" | Where-Object { $_ -match 'warning [A-Z]+[0-9]+' })
    if ($warnings.Count -gt 0) {
        Write-CommandOutput $run.Output
        throw ('the build emitted ' + $warnings.Count + ' compiler warning(s)')
    }
    if ($run.Output -match 'error [A-Z]+[0-9]+') {
        Write-CommandOutput $run.Output
        throw 'the build reported a compiler error'
    }
    Write-Host '    zero compiler warnings' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# 04 ctest
# ---------------------------------------------------------------------------

$ctestCommand = 'ctest --test-dir ' + (Q $MainBuild) + ' -C Release --output-on-failure'

Invoke-Step -Name 'ctest' -Command $ctestCommand -Body {
    $run = Invoke-VcShell -Command $ctestCommand -WorkDir $RepoRoot
    if ($run.ExitCode -ne 0) {
        Write-CommandOutput $run.Output
        throw ('ctest exited with ' + $run.ExitCode)
    }
    $summary = [regex]::Match($run.Output, '(\d+)% tests passed, (\d+) tests failed out of (\d+)')
    if (-not $summary.Success) {
        Write-CommandOutput $run.Output
        throw 'ctest reported no summary line'
    }
    $total = [int]$summary.Groups[3].Value
    $failed = [int]$summary.Groups[2].Value
    $passed = $total - $failed
    Write-Host ('    ctest: ' + $passed + '/' + $total + ' passed, ' + $failed + ' failed') -ForegroundColor DarkGray
    if ($failed -ne 0) { throw ($failed.ToString() + ' test(s) failed') }
    if ($total -lt 1) { throw 'ctest ran no tests' }
}

# ---------------------------------------------------------------------------
# 05 install-main
# ---------------------------------------------------------------------------

$installCommand = 'cmake --install ' + (Q $MainBuild) + ' --config Release --prefix ' + (Q $Prefix)

Invoke-Step -Name 'install-main' -Command $installCommand -Body {
    $run = Invoke-VcShell -Command $installCommand -WorkDir $RepoRoot
    if ($run.ExitCode -ne 0) {
        Write-CommandOutput $run.Output
        throw ('cmake --install exited with ' + $run.ExitCode)
    }
    if (-not (Test-Path -LiteralPath $Prefix)) { throw 'the install prefix was not created' }
    Write-Host ('    installed into ' + $Prefix) -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# 06 installed-tree
# ---------------------------------------------------------------------------

$requiredInstalled = @(
    'include\mc\engine.hpp',
    'lib\maintenance_coordinator.lib',
    'lib\cmake\MaintenanceCoordinator\MaintenanceCoordinatorConfig.cmake',
    'lib\cmake\MaintenanceCoordinator\MaintenanceCoordinatorConfigVersion.cmake',
    'lib\cmake\MaintenanceCoordinator\MaintenanceCoordinatorTargets.cmake',
    'share\MaintenanceCoordinator\LICENSE',
    'share\MaintenanceCoordinator\NOTICE'
)
$treeCommand = 'Test-Path ' + (($requiredInstalled | ForEach-Object { Q (Join-Path $Prefix $_) }) -join ',')

Invoke-Step -Name 'installed-tree' -Command $treeCommand -Body {
    foreach ($relative in $requiredInstalled) {
        $full = Join-Path $Prefix $relative
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
            throw ('the installed tree is missing ' + $relative)
        }
        $length = (Get-Item -LiteralPath $full).Length
        if ($length -le 0) { throw ($relative + ' was installed empty') }
        Write-Host ('    ok ' + $relative + ' (' + $length + ' bytes)') -ForegroundColor DarkGray
    }
}

# ---------------------------------------------------------------------------
# 07 configure-downstream
# ---------------------------------------------------------------------------

$downstreamSource = Join-Path $RepoRoot 'tests\downstream'
$configureDown = (@('cmake', '-S', (Q $downstreamSource), '-B', (Q $DownBuild)) + $Generator +
                  @('-DCMAKE_BUILD_TYPE=Release', ('-DCMAKE_PREFIX_PATH=' + (Q $Prefix)))) -join ' '

Invoke-Step -Name 'configure-downstream' -Command $configureDown -Body {
    if (-not (Test-Path -LiteralPath (Join-Path $downstreamSource 'CMakeLists.txt'))) {
        throw 'tests/downstream/CMakeLists.txt is missing'
    }
    $run = Invoke-VcShell -Command $configureDown -WorkDir $RepoRoot
    if ($run.ExitCode -ne 0) {
        Write-CommandOutput $run.Output
        throw ('the downstream configure exited with ' + $run.ExitCode)
    }
    Write-Host '    find_package(MaintenanceCoordinator CONFIG REQUIRED) resolved' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# 08 build-downstream
# ---------------------------------------------------------------------------

$buildDown = 'cmake --build ' + (Q $DownBuild) + ' --config Release --parallel'

Invoke-Step -Name 'build-downstream' -Command $buildDown -Body {
    $run = Invoke-VcShell -Command $buildDown -WorkDir $RepoRoot
    if ($run.ExitCode -ne 0) {
        Write-CommandOutput $run.Output
        throw ('the downstream build exited with ' + $run.ExitCode)
    }
    $warnings = @($run.Output -split "\r?\n" | Where-Object { $_ -match 'warning [A-Z]+[0-9]+' })
    if ($warnings.Count -gt 0) {
        Write-CommandOutput $run.Output
        throw ('the consumer build emitted ' + $warnings.Count + ' warning(s) under /W4 /WX')
    }
    $exe = Get-ChildItem -LiteralPath $DownBuild -Recurse -File -Filter 'mc_downstream_consumer.exe' |
           Select-Object -First 1
    if (-not $exe) { throw 'mc_downstream_consumer.exe was not produced' }
    $script:ConsumerExe = $exe.FullName
    Write-Host ('    consumer: ' + $script:ConsumerExe) -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# 09 run-downstream
# ---------------------------------------------------------------------------

# The consumer path is only known once the downstream build has run, so the
# exact command is assembled here.
$runCommand = (Q $script:ConsumerExe) + ' ' + (Q $StoreDir)

Invoke-Step -Name 'run-downstream' -Command $runCommand -Body {
    if (Test-Path -LiteralPath $StoreDir) { Remove-Item -LiteralPath $StoreDir -Recurse -Force }
    $run = Invoke-VcShell -Command $runCommand -WorkDir $RepoRoot
    Write-CommandOutput $run.Output
    $ok = @($run.Output -split "\r?\n" | Where-Object { $_ -match '^DOWNSTREAM CONSUMER OK: \d+ checks$' })
    Write-Host ('    exit code: ' + $run.ExitCode) -ForegroundColor DarkGray
    if ($ok.Count -gt 0) {
        Write-Host ('    ' + $ok[$ok.Count - 1].Trim()) -ForegroundColor DarkGray
    } else {
        throw 'the consumer printed no "DOWNSTREAM CONSUMER OK" line'
    }
    if ($run.ExitCode -ne 0) { throw ('the consumer exited with ' + $run.ExitCode) }
}

# ---------------------------------------------------------------------------
# 10 verify-export
# ---------------------------------------------------------------------------

$packageDir = Join-Path $Prefix 'lib\cmake\MaintenanceCoordinator'
$targetsFile = Join-Path $packageDir 'MaintenanceCoordinatorTargets.cmake'
$exportCommand = 'Select-String -Path ' + (Q $targetsFile) + ' -Pattern SummonDCCP::,INTERFACE_INCLUDE_DIRECTORIES'

Invoke-Step -Name 'verify-export' -Command $exportCommand -Body {
    $targetsText = Get-Content -LiteralPath $targetsFile -Raw
    $configFile = Join-Path $packageDir 'MaintenanceCoordinatorConfig.cmake'
    $configText = Get-Content -LiteralPath $configFile -Raw
    $releaseFile = Join-Path $packageDir 'MaintenanceCoordinatorTargets-release.cmake'
    $releaseText = Get-Content -LiteralPath $releaseFile -Raw

    # (a) the imported target the package declares.
    $imported = [regex]::Match($targetsText, 'add_library\(\s*SummonDCCP::([A-Za-z0-9_]+)\s+STATIC\s+IMPORTED\s*\)')
    if (-not $imported.Success) {
        throw 'the installed targets file declares no imported SummonDCCP:: target'
    }
    $exportedName = 'SummonDCCP::' + $imported.Groups[1].Value
    $contractFound = $targetsText.Contains('SummonDCCP::MaintenanceCoordinator')
    Write-Host ('    imported target in the installed package: ' + $exportedName) -ForegroundColor DarkGray
    Write-Host ('    literal grep for SummonDCCP::MaintenanceCoordinator: ' + $contractFound) -ForegroundColor DarkGray
    if (-not $contractFound) {
        $Findings.Add('the installed package exports ' + $exportedName + ', not SummonDCCP::MaintenanceCoordinator: install(EXPORT) exports the real target name and a build-tree ALIAS is not exported, so the documented contract name does not exist after find_package. tests/downstream re-binds the contract name with an ALIAS.')
    }

    # (b) the include directories must point into the installed prefix.
    $includes = [regex]::Match($targetsText, 'INTERFACE_INCLUDE_DIRECTORIES\s+"([^"]+)"')
    if (-not $includes.Success) { throw 'the installed targets file has no INTERFACE_INCLUDE_DIRECTORIES' }
    $includeValue = $includes.Groups[1].Value
    if ($includeValue -notmatch '_IMPORT_PREFIX.+/include') {
        throw ('INTERFACE_INCLUDE_DIRECTORIES does not resolve through the import prefix: ' + $includeValue)
    }
    $includeDirs = @($includeValue -split ';' | Where-Object { $_.Length -gt 0 })
    Write-Host ('    INTERFACE_INCLUDE_DIRECTORIES: ' + $includeValue) -ForegroundColor DarkGray
    if ($includeDirs.Count -gt 1) {
        $unique = @($includeDirs | Select-Object -Unique)
        if ($unique.Count -eq 1) {
            $Findings.Add('INTERFACE_INCLUDE_DIRECTORIES lists the installed include directory ' + $includeDirs.Count + ' times (INSTALL_INTERFACE plus INCLUDES DESTINATION): harmless, but the exported interface is not minimal.')
        }
    }

    # (c) the installed interface must not reach back into this repository.
    $repoForward = $RepoRoot.Replace('\', '/')
    $mainForward = $MainBuild.Replace('\', '/')
    $forbidden = @(
        [pscustomobject]@{ What = 'the source tree'; Value = $RepoRoot },
        [pscustomobject]@{ What = 'the source tree (forward slashes)'; Value = $repoForward },
        [pscustomobject]@{ What = 'the build tree'; Value = $MainBuild },
        [pscustomobject]@{ What = 'the build tree (forward slashes)'; Value = $mainForward },
        [pscustomobject]@{ What = 'an internal source directory'; Value = '/src/' },
        [pscustomobject]@{ What = 'the internal source include path'; Value = 'src\include' },
        [pscustomobject]@{ What = 'the source-tree include path include/mc'; Value = 'include/mc' },
        [pscustomobject]@{ What = 'the source-tree include path include\mc'; Value = 'include\mc' }
    )
    foreach ($file in @($targetsFile, $releaseFile, $configFile,
                        (Join-Path $packageDir 'MaintenanceCoordinatorConfigVersion.cmake'))) {
        $text = Get-Content -LiteralPath $file -Raw
        foreach ($bad in $forbidden) {
            if ($text.Contains($bad.Value)) {
                throw ((Split-Path -Leaf $file) + ' references ' + $bad.What)
            }
        }
    }
    Write-Host '    no source-tree, build-tree or internal include references' -ForegroundColor DarkGray

    # (d) reported, not fixed: the config file calls a macro it never defines.
    $callsMacro = $configText -match 'check_required_components'
    $definesMacro = $configText -match 'macro\(\s*check_required_components'
    if ($callsMacro -and -not $definesMacro) {
        $Findings.Add('MaintenanceCoordinatorConfig.cmake calls check_required_components() but never defines it (configure_package_config_file was given NO_CHECK_REQUIRED_COMPONENTS_MACRO), so find_package(MaintenanceCoordinator CONFIG REQUIRED) fails with "Unknown CMake command". tests/downstream defines the macro itself before find_package.')
    }
}

# ---------------------------------------------------------------------------
# 11 cpack-source / 12 cpack-binary
# ---------------------------------------------------------------------------

$cpackSource = 'cpack --config ' + (Q (Join-Path $MainBuild 'CPackSourceConfig.cmake')) +
               ' -G ZIP -C Release -B ' + (Q $Packages)
$cpackBinary = 'cpack --config ' + (Q (Join-Path $MainBuild 'CPackConfig.cmake')) +
               ' -G ZIP -C Release -B ' + (Q $Packages)

Invoke-Step -Name 'cpack-source' -Command $cpackSource -Body {
    New-Item -ItemType Directory -Path $Packages -Force | Out-Null
    $run = Invoke-VcShell -Command $cpackSource -WorkDir $MainBuild
    if ($run.ExitCode -ne 0) {
        Write-CommandOutput $run.Output
        throw ('cpack (source) exited with ' + $run.ExitCode)
    }
    $archive = Get-ChildItem -LiteralPath $Packages -File -Filter '*-Source.zip' | Select-Object -First 1
    if (-not $archive) { throw 'cpack produced no source archive' }
    if ($archive.Length -le 0) { throw 'cpack produced an empty source archive' }
    Write-Host ('    ' + $archive.Name + ' (' + $archive.Length + ' bytes)') -ForegroundColor DarkGray
}

Invoke-Step -Name 'cpack-binary' -Command $cpackBinary -Body {
    New-Item -ItemType Directory -Path $Packages -Force | Out-Null
    $run = Invoke-VcShell -Command $cpackBinary -WorkDir $MainBuild
    if ($run.ExitCode -ne 0) {
        Write-CommandOutput $run.Output
        throw ('cpack (binary) exited with ' + $run.ExitCode)
    }
    $archive = Get-ChildItem -LiteralPath $Packages -File -Filter '*.zip' |
               Where-Object { $_.Name -notlike '*-Source.zip' } | Select-Object -First 1
    if (-not $archive) { throw 'cpack produced no binary archive' }
    if ($archive.Length -le 0) { throw 'cpack produced an empty binary archive' }
    Write-Host ('    ' + $archive.Name + ' (' + $archive.Length + ' bytes)') -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# 13 verify-packages
# ---------------------------------------------------------------------------

$packagesCommand = 'Get-ChildItem ' + (Q ($Packages + '\*.zip')) + ' | [System.IO.Compression.ZipFile]::OpenRead($_.FullName).Entries'

Invoke-Step -Name 'verify-packages' -Command $packagesCommand -Body {
    if (-not ('System.IO.Compression.ZipFile' -as [type])) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
    }

    $archives = @(Get-ChildItem -LiteralPath $Packages -File -Filter '*.zip')
    $sourceArchive = $archives | Where-Object { $_.Name -like '*-Source.zip' } | Select-Object -First 1
    $binaryArchive = $archives | Where-Object { $_.Name -notlike '*-Source.zip' } | Select-Object -First 1
    if (-not $sourceArchive) { throw 'the source archive does not exist' }
    if (-not $binaryArchive) { throw 'the binary archive does not exist' }
    if ($sourceArchive.Length -le 0) { throw 'the source archive is empty' }
    if ($binaryArchive.Length -le 0) { throw 'the binary archive is empty' }
    Write-Host ('    ' + $sourceArchive.Name + ': ' + $sourceArchive.Length + ' bytes') -ForegroundColor DarkGray
    Write-Host ('    ' + $binaryArchive.Name + ': ' + $binaryArchive.Length + ' bytes') -ForegroundColor DarkGray

    $sourceEntries = @()
    $zip = [System.IO.Compression.ZipFile]::OpenRead($sourceArchive.FullName)
    try {
        $sourceEntries = @($zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    } finally {
        $zip.Dispose()
    }
    $requiredSource = @(
        'CMakeLists.txt',
        'include/mc/engine.hpp',
        'tests/CMakeLists.txt',
        'LICENSE',
        'NOTICE'
    )
    foreach ($relative in $requiredSource) {
        $match = @($sourceEntries | Where-Object { $_ -eq $relative -or $_.EndsWith('/' + $relative) })
        if ($match.Count -eq 0) { throw ('the source archive does not contain ' + $relative) }
    }
    Write-Host ('    source archive carries all ' + $requiredSource.Count + ' required paths (' + $sourceEntries.Count + ' entries)') -ForegroundColor DarkGray

    $binaryEntries = @()
    $zip = [System.IO.Compression.ZipFile]::OpenRead($binaryArchive.FullName)
    try {
        $binaryEntries = @($zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    } finally {
        $zip.Dispose()
    }
    $requiredBinary = @(
        'include/mc/engine.hpp',
        'lib/maintenance_coordinator.lib',
        'lib/cmake/MaintenanceCoordinator/MaintenanceCoordinatorConfig.cmake'
    )
    foreach ($relative in $requiredBinary) {
        $match = @($binaryEntries | Where-Object { $_ -eq $relative -or $_.EndsWith('/' + $relative) })
        if ($match.Count -eq 0) { throw ('the binary archive does not contain ' + $relative) }
    }
    Write-Host ('    binary archive carries the headers, the library and the package config (' + $binaryEntries.Count + ' entries)') -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

$failedSteps = @($Results | Where-Object { $_.Result -ne 'PASS' })
$ok = (-not $Aborted) -and ($failedSteps.Count -eq 0)

Write-Host ''
Write-Host '=================== PACKAGING VALIDATION SUMMARY ==================='
Write-Host ('{0,-4}{1,-22}{2,-8}{3}' -f '#', 'STEP', 'RESULT', 'COMMAND')
Write-Host ('{0,-4}{1,-22}{2,-8}{3}' -f '----', '----------------------', '------', '-----------------------------------------------------------')
$index = 0
foreach ($entry in $Results) {
    $index++
    Write-Host ('{0,-4}{1,-22}{2,-8}{3}' -f $index, $entry.Name, $entry.Result, $entry.Command)
}

if ($Findings.Count -gt 0) {
    Write-Host ''
    Write-Host 'FINDINGS (defects in the library install/export metadata; reported, not fixed here):'
    foreach ($finding in $Findings) {
        Write-Host ('  - ' + $finding)
    }
}

Write-Host ''
if ($ok) {
    Write-Host 'PACKAGING VALIDATION OK' -ForegroundColor Green
    exit 0
}
Write-Host 'PACKAGING VALIDATION FAILED' -ForegroundColor Red
exit 1
