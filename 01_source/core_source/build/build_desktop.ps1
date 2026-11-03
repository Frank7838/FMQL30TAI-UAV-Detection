# 构建桌面界面和SSH组件，输出完整运行目录。
param(
    [Parameter(Mandatory=$true)][string]$PythonExecutable,
    [string]$DependencyPath = ''
)
$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = Join-Path $sourceRoot 'build_output/desktop'
if (Test-Path -LiteralPath $buildRoot) { throw 'Output already exists; preserve it and choose a separate build directory.' }
New-Item -ItemType Directory -Path $buildRoot | Out-Null
$agentArgs = @('-B','-m','PyInstaller','--noconfirm','--onedir','--console','--name','UAVControlAgent',
    '--exclude-module','torch','--exclude-module','numpy','--exclude-module','cv2',
    '--exclude-module','tkinter','--exclude-module','matplotlib',
    '--distpath',(Join-Path $buildRoot 'dist'),'--workpath',(Join-Path $buildRoot 'work'),
    '--specpath',$buildRoot)
if ($DependencyPath) { $agentArgs += @('--paths',$DependencyPath) }
$agentArgs += (Join-Path $sourceRoot 'src/desktop/agent.py')
& $PythonExecutable @agentArgs *> (Join-Path $buildRoot 'agent_build.log')
if ($LASTEXITCODE -ne 0) { throw 'Agent build failed; inspect agent_build.log.' }
$packageRoot = Join-Path $buildRoot 'UAVDetectionSystem'
New-Item -ItemType Directory -Path (Join-Path $packageRoot 'engine') -Force | Out-Null
Copy-Item -Path (Join-Path $buildRoot 'dist/UAVControlAgent/*') -Destination (Join-Path $packageRoot 'engine') -Recurse
& "$env:WINDIR/Microsoft.NET/Framework64/v4.0.30319/csc.exe" /nologo /target:winexe /platform:x64 /codepage:65001 "/out:$packageRoot/无人机检测系统.exe" /reference:System.Drawing.dll /reference:System.Windows.Forms.dll /reference:System.Web.Extensions.dll (Join-Path $sourceRoot 'src/desktop/Desktop.cs') *> (Join-Path $buildRoot 'desktop_build.log')
if ($LASTEXITCODE -ne 0) { throw 'Desktop build failed; inspect desktop_build.log.' }
Write-Output "Built locally: $packageRoot"
