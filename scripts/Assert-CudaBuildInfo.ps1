function Assert-CudaBuildInfo([hashtable]$Info)
{
    $toolkitFields = @('CudaToolkit', 'CudaRuntimeMode', 'GpuValidationRuntime')
    $toolkitPresent = @($toolkitFields | Where-Object { $Info.ContainsKey($_) })
    if ($toolkitPresent.Count -gt 0)
    {
        if ($toolkitPresent.Count -ne $toolkitFields.Count -or $Info.Runtime -ne 'cuda' -or
            $Info.CudaToolkit -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$' -or
            ($Info.Platform -eq 'windows' -and $Info.CudaRuntimeMode -ne 'hybrid') -or
            ($Info.Platform -eq 'linux' -and $Info.CudaRuntimeMode -ne 'shared') -or
            $Info.Platform -notin @('windows', 'linux'))
        {
            throw 'Incomplete or invalid CUDA toolchain/runtime metadata'
        }
        if (($Info.GpuValidation -eq 'passed' -and $Info.GpuValidationRuntime -notmatch '^[1-9][0-9]+$') -or
            ($Info.GpuValidation -eq 'not-run' -and $Info.GpuValidationRuntime -ne 'not-run'))
        {
            throw 'CUDA runtime validation must describe the actual GPU acceptance state'
        }
    }
    $fields = @('CudaArchitectures', 'CudaPtx', 'GpuValidationDevice', 'GpuValidationComputeCapability', 'GpuValidationDriver')
    $present = @($fields | Where-Object { $Info.ContainsKey($_) })
    if ($present.Count -eq 0 -and $toolkitPresent.Count -eq 0) { return } # Historical archives remain verifiable.
    if ($present.Count -ne $fields.Count -or $Info.Runtime -ne 'cuda') { throw 'Incomplete or misplaced CUDA build metadata' }
    $universal = 'sm_75;sm_80;sm_86;sm_89;sm_90;sm_100;sm_120'
    $current = $Info.CudaArchitectures -eq $universal -and $Info.CudaPtx -eq ($universal -replace 'sm_', 'compute_')
    # Ordinary verification still accepts the earlier six-target/one-PTX format.
    # New package creation independently requires the current universal images.
    $legacy = $Info.CudaArchitectures -eq 'sm_75;sm_80;sm_86;sm_89;sm_90;sm_120' -and $Info.CudaPtx -eq 'compute_75'
    if (-not $current -and -not $legacy)
    {
        throw 'CUDA archive does not describe the universal native/PTX targets'
    }
    if ($Info.GpuValidation -eq 'passed')
    {
        if ([string]::IsNullOrWhiteSpace($Info.GpuValidationDevice) -or $Info.GpuValidationDevice -eq 'not-run' -or
            $Info.GpuValidationComputeCapability -notmatch '^sm_[0-9]+$' -or $Info.GpuValidationDriver -notmatch '^[1-9][0-9]+$')
        {
            throw 'Passed GPU validation requires the actual device, compute capability and driver'
        }
    }
    elseif ($Info.GpuValidation -eq 'not-run')
    {
        foreach ($field in $fields | Select-Object -Skip 2)
        {
            if ($Info[$field] -ne 'not-run') { throw 'An unvalidated package cannot claim a validated GPU' }
        }
    }
    else { throw 'Invalid CUDA validation state' }
}

function Assert-CudaPackageName([hashtable]$Info, [string]$PackageName)
{
    if ($Info['Runtime'] -ne 'cuda' -or $Info['GpuValidation'] -ne 'not-run') { return }
    if ($PackageName -match '_candidate\.(zip|tar\.gz)$') { return }
    # Linux releases may ship without GPU hardware acceptance. Preserve not-run;
    # Windows formal CUDA packages still require actual GPU validation.
    if ($Info['Platform'] -eq 'linux' -and $Info['Arch'] -eq 'x64' -and
        $PackageName -match '^Mosaicraft_v[0-9]+\.[0-9]+\.[0-9]+_linux-x64_cuda\.tar\.gz$') { return }
    throw 'Unvalidated CUDA packages require a candidate name, except Linux x64 release tar.gz'
}
