# Copyright (C) 2002-2012 Nikolaus Gebhardt
# This file is part of the "Irrlicht Engine".
# For conditions of distribution and use, see copyright notice in irrlicht.h
#
# Compiles every .vert/.frag in this directory to SPIR-V and rewrites
# ../../CVulkanDefaultShaders.h with the result. The .glsl files are shared includes and are
# not compiled on their own.
#
#   powershell -ExecutionPolicy Bypass -File generate.ps1 [-Glslang <path to glslang.exe>]
#
# Without -Glslang the script looks for glslang / glslangValidator on PATH, then in
# $env:VULKAN_SDK\Bin.

param(
    [string]$Glslang = ""
)

$ErrorActionPreference = "Stop"

$shaderDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$headerPath = Join-Path $shaderDir "..\..\CVulkanDefaultShaders.h"
$headerPath = [System.IO.Path]::GetFullPath($headerPath)

# --- locate the compiler ---------------------------------------------------------------
if (-not $Glslang) {
    foreach ($n in @("glslang", "glslangValidator")) {
        $c = Get-Command $n -ErrorAction SilentlyContinue
        if ($c) { $Glslang = $c.Source; break }
    }
}
if ((-not $Glslang) -and $env:VULKAN_SDK) {
    foreach ($n in @("glslang.exe", "glslangValidator.exe")) {
        $p = Join-Path $env:VULKAN_SDK "Bin\$n"
        if (Test-Path $p) { $Glslang = $p; break }
    }
}
if ((-not $Glslang) -or (-not (Test-Path $Glslang))) {
    throw "glslang not found. Pass -Glslang <path>, put it on PATH, or set VULKAN_SDK."
}

# --- file name -> C++ identifier -------------------------------------------------------
$tokenOverrides = @{
    "2tcoords" = "2TCoords"; "2layer" = "2Layer"; "uv2" = "UV2"; "m2" = "M2"; "m4" = "M4"
}
function Convert-NameToIdentifier([string]$baseName, [string]$stage) {
    $parts = $baseName.Split("_")
    $out = ""
    foreach ($p in $parts) {
        if ($tokenOverrides.ContainsKey($p)) { $out += $tokenOverrides[$p] }
        else { $out += $p.Substring(0, 1).ToUpper() + $p.Substring(1) }
    }
    $suffix = if ($stage -eq "vert") { "Vs" } else { "Fs" }
    return "Vulkan" + $out + $suffix + "Spv"
}

# --- compile ---------------------------------------------------------------------------
$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("irrvkspv_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

$entries = @()
try {
    $sources = Get-ChildItem -Path $shaderDir -File |
        Where-Object { $_.Extension -eq ".vert" -or $_.Extension -eq ".frag" } |
        Sort-Object Extension, Name

    foreach ($src in $sources) {
        $stage = $src.Extension.TrimStart(".")
        $spv = Join-Path $tempDir ($src.BaseName + "." + $stage + ".spv")

        & $Glslang -V --target-env vulkan1.2 -S $stage "-I$shaderDir" $src.FullName -o $spv | Out-Null
        if ($LASTEXITCODE -ne 0) { throw ("glslang failed on " + $src.Name) }

        $bytes = [System.IO.File]::ReadAllBytes($spv)
        if ($bytes.Length -eq 0 -or ($bytes.Length % 4) -ne 0) {
            throw ("bad SPIR-V size for " + $src.Name)
        }

        $words = New-Object 'System.UInt32[]' ($bytes.Length / 4)
        for ($i = 0; $i -lt $words.Length; $i++) {
            $words[$i] = [System.BitConverter]::ToUInt32($bytes, $i * 4)
        }

        $entries += [pscustomobject]@{
            Name  = $src.Name
            Ident = (Convert-NameToIdentifier $src.BaseName $stage)
            Words = $words
            Bytes = $bytes.Length
        }
    }
}
finally {
    Remove-Item -Recurse -Force $tempDir -ErrorAction SilentlyContinue
}

if ($entries.Count -eq 0) { throw "no shader sources found in $shaderDir" }

# --- emit the header --------------------------------------------------------------------
$sb = New-Object System.Text.StringBuilder
function W([string]$s = "") { [void]$sb.AppendLine($s) }

W "// Copyright (C) 2002-2012 Nikolaus Gebhardt"
W "// This file is part of the ""Irrlicht Engine""."
W "// For conditions of distribution and use, see copyright notice in irrlicht.h"
W ""
W "// GENERATED FILE -- do not edit by hand."
W "//"
W "// SPIR-V for the built-in material shaders, compiled from the GLSL sources in"
W "// source/Irrlicht/vulkan/shaders/. Committing the binaries keeps the build free of any"
W "// shader toolchain, the same way CD3D12DefaultShaders.h embeds its HLSL source."
W "// Regenerate with source/Irrlicht/vulkan/shaders/generate.ps1 (needs glslang)."
W "//"
W "// Bindings: driver constants in descriptor set 4 (b0 world, b1 view/proj/camera,"
W "// b2 clip planes, b3 lighting, b4 fog), material textures in set 0 (b0 base, b1 layer 1)."
W "// Vertex shaders negate clip-space y for Vulkan NDC; depth is left to the reversed-Z"
W "// projection matrix. Look a module up by source file name with getVulkanDefaultShader()."
W ""
W "#ifndef __C_VULKAN_DEFAULT_SHADERS_H_INCLUDED__"
W "#define __C_VULKAN_DEFAULT_SHADERS_H_INCLUDED__"
W ""
W "#ifdef _IRR_COMPILE_WITH_VULKAN_"
W ""
W "#include <stddef.h>"
W "#include <stdint.h>"
W "#include <string.h>"
W ""
W "namespace irr"
W "{"
W "`tnamespace video"
W "`t{"

foreach ($e in $entries) {
    W ("`t`t// " + $e.Name + " -- " + $e.Bytes + " bytes")
    W ("`t`tstatic const uint32_t " + $e.Ident + "[] = {")
    $line = "`t`t`t"
    $n = 0
    foreach ($w in $e.Words) {
        $line += ("0x{0:x8}u," -f $w)
        $n++
        if ($n % 8 -eq 0) { W $line; $line = "`t`t`t" } else { $line += " " }
    }
    if ($n % 8 -ne 0) { W $line.TrimEnd() }
    W "`t`t};"
    W ("`t`tstatic const size_t " + $e.Ident + "Size = sizeof(" + $e.Ident + ");")
    W ""
}

W "`t`t//! One built-in SPIR-V module. CodeSize is in bytes (VkShaderModuleCreateInfo)."
W "`t`tstruct SVulkanDefaultShader"
W "`t`t{"
W "`t`t`tconst char* Name;"
W "`t`t`tconst uint32_t* Code;"
W "`t`t`tsize_t CodeSize;"
W "`t`t};"
W ""
W "`t`tstatic const SVulkanDefaultShader VulkanDefaultShaders[] = {"
foreach ($e in $entries) {
    W ("`t`t`t{ """ + $e.Name + """, " + $e.Ident + ", " + $e.Ident + "Size },")
}
W "`t`t};"
W ""
W ("`t`tstatic const size_t VulkanDefaultShaderCount = " + $entries.Count + ";")
W ""
W "`t`t//! Returns the module compiled from the given source file name (""solid.frag""), or 0."
W "`t`tinline const SVulkanDefaultShader* getVulkanDefaultShader(const char* name)"
W "`t`t{"
W "`t`t`tif (!name)"
W "`t`t`t`treturn 0;"
W "`t`t`tfor (size_t i = 0; i < VulkanDefaultShaderCount; ++i)"
W "`t`t`t`tif (strcmp(VulkanDefaultShaders[i].Name, name) == 0)"
W "`t`t`t`t`treturn &VulkanDefaultShaders[i];"
W "`t`t`treturn 0;"
W "`t`t}"
W "`t}"
W "}"
W ""
W "#endif // _IRR_COMPILE_WITH_VULKAN_"
W ""
W "#endif"

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($headerPath, $sb.ToString(), $utf8NoBom)

Write-Host ("Wrote " + $headerPath)
foreach ($e in $entries) {
    Write-Host ("  {0,-40} {1,7} bytes  {2}" -f $e.Name, $e.Bytes, $e.Ident)
}
