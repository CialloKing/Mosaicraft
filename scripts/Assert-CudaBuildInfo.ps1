function Assert-CudaBuildInfo([hashtable]$Info)
{
    $fields = @('CudaArchitectures', 'CudaPtx', 'GpuValidationDevice', 'GpuValidationComputeCapability', 'GpuValidationDriver')
    $present = @($fields | Where-Object { $Info.ContainsKey($_) })
    if ($present.Count -eq 0) { return } # Historical archives remain verifiable.
    if ($present.Count -ne $fields.Count -or $Info.Runtime -ne 'cuda') { throw 'Incomplete or misplaced CUDA build metadata' }
    if ($Info.CudaArchitectures -ne 'sm_75;sm_80;sm_86;sm_89;sm_90;sm_120' -or $Info.CudaPtx -ne 'compute_75')
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
            if ($Info[$field] -ne 'not-run') { throw 'Candidate cannot claim a validated GPU' }
        }
    }
    else { throw 'Invalid CUDA validation state' }
}
