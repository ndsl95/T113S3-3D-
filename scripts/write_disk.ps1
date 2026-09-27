$diskNumber = 4
$imgPath = "H:\disk_small.img"
$logPath = "H:\write_log.txt"
$oneMB = 1048576
$oneGB = 1073741824

try {
    $disk = Get-Disk -Number $diskNumber
    "Target: Disk $($disk.Number) - $($disk.FriendlyName) - $([math]::Round($disk.Size/$oneGB,2)) GB" | Out-File $logPath
    $imgSize = (Get-Item $imgPath).Length
    "Image: $([math]::Round($imgSize/$oneGB,2)) GB" | Out-File $logPath -Append
    "" | Out-File $logPath -Append

    # 移除所有分区盘符
    Get-Partition -DiskNumber $diskNumber | ForEach-Object {
        if ($_.DriveLetter) {
            "Removing drive letter $($_.DriveLetter):" | Out-File $logPath -Append
            mountvol "$($_.DriveLetter):" /P 2>&1 | Out-File $logPath -Append
        }
    }
    Start-Sleep -Seconds 2

    "Writing..." | Out-File $logPath -Append

    $fs = New-Object System.IO.FileStream("\\.\PhysicalDrive$diskNumber", [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
    $imgFs = [System.IO.File]::OpenRead($imgPath)

    $bufSize = 4 * $oneMB
    $buffer = New-Object byte[] $bufSize
    $totalWritten = 0
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $lastLog = 0

    while ($totalWritten -lt $imgSize) {
        $read = $imgFs.Read($buffer, 0, $buffer.Length)
        if ($read -eq 0) { break }
        $fs.Write($buffer, 0, $read)
        $totalWritten += $read
        $elapsed = $sw.Elapsed.TotalSeconds
        if ($elapsed - $lastLog -ge 5) {
            $percent = [math]::Round($totalWritten / $imgSize * 100, 1)
            $speed = [math]::Round($totalWritten / $oneMB / $elapsed, 1)
            "$percent% - $speed MB/s - $([math]::Round($totalWritten/$oneGB,2))/$([math]::Round($imgSize/$oneGB,2)) GB" | Out-File $logPath -Append
            $lastLog = $elapsed
        }
    }

    $fs.Flush()
    $fs.Close()
    $imgFs.Close()
    $sw.Stop()

    "" | Out-File $logPath -Append
    "Done! Written: $([math]::Round($totalWritten/$oneGB,2)) GB in $([math]::Round($sw.Elapsed.TotalSeconds,1))s, avg $([math]::Round($totalWritten/$oneMB/$sw.Elapsed.TotalSeconds,1)) MB/s" | Out-File $logPath -Append
} catch {
    "ERROR: $_" | Out-File $logPath -Append
}
