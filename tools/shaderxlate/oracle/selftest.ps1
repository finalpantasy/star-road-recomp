# vkdiff self-test. Usage: selftest.ps1 [-Validate]
param([switch]$Validate)
$ErrorActionPreference = 'Stop'
$sdk  = 'A:\AI\Recomps\new-pokemon-snap\tools\vulkan_sdk'
$glsl = Join-Path $sdk 'Bin\glslangValidator.exe'
$exe  = 'A:\AI\Recomps\smrpg-work\build\vkdiff\cmake\vkdiff.exe'
$src  = Join-Path $PSScriptRoot 'selftest'
$out  = 'A:\AI\Recomps\smrpg-work\build\vkdiff\selftest'
New-Item -ItemType Directory -Force $out | Out-Null
function Compile($stage, $file, $spv, $defs) {
  & $glsl -V --target-env vulkan1.3 -S $stage @defs (Join-Path $src $file) -o (Join-Path $out $spv) | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "glslang failed: $file" }
}
Compile vert helper.vert helper.vert.spv @()
Compile geom helper.geom helper.geom.spv @()
Compile frag helper_flat.frag helper_flat.frag.spv @()
Compile frag fs_test.frag fsA.spv @('-DSET=1','-DUBO_B=1','-DTEX_B=3')
Compile frag fs_test.frag fsB.spv @('-DSET=0','-DUBO_B=7','-DTEX_B=8')
Compile frag fs_test.frag fsC.spv @('-DSET=0','-DUBO_B=7','-DTEX_B=8','-DSCALE=1.01')
Compile vert vs_test.vert vsA.spv @('-DSET=1','-DUBO_B=1')
Compile vert vs_test.vert vsB.spv @('-DSET=2','-DUBO_B=4')
$o = { param($n) Join-Path $out $n }
$s = { param($n) Join-Path $src $n }
$hv = & $o 'helper.vert.spv'; $hg = & $o 'helper.geom.spv'; $hf = & $o 'helper_flat.frag.spv'
$jobs = @(
 "fs_equal`tfs`t$hv`t-`t-`t$(& $o 'fsA.spv')`t$(& $s 'fs_A.layout')`t$(& $o 'fsB.spv')`t$(& $s 'fs_B.layout')`t7",
 "fs_differ`tfs`t$hv`t-`t-`t$(& $o 'fsA.spv')`t$(& $s 'fs_A.layout')`t$(& $o 'fsC.spv')`t$(& $s 'fs_B.layout')`t7",
 "vs_equal`tvs`t-`t$hg`t$hf`t$(& $o 'vsA.spv')`t$(& $s 'vs_A.layout')`t$(& $o 'vsB.spv')`t$(& $s 'vs_B.layout')`t9",
 "missing_file`tfs`t$hv`t-`t-`t$(& $o 'nope.spv')`t$(& $s 'fs_A.layout')`t$(& $o 'fsB.spv')`t$(& $s 'fs_B.layout')`t1"
)
$jobFile = & $o 'jobs.tsv'; $resFile = & $o 'results.tsv'
[IO.File]::WriteAllText($jobFile, (($jobs -join "`n") + "`n"))
$lvp = 'A:\AI\Recomps\tools-ext\mesa\x\x64\lvp_icd.x86_64.json'
$env:VK_ICD_FILENAMES = $lvp; $env:VK_DRIVER_FILES = $lvp
$a = @($jobFile, $resFile, '--time', '--dump', $out)
if ($Validate) { $env:VK_LAYER_PATH = Join-Path $sdk 'Bin'; $a += '--validate' }
& $exe @a
Write-Host "exit code $LASTEXITCODE"
Get-Content $resFile
