param()
$cmd = ($args -join ' ')
$port = New-Object System.IO.Ports.SerialPort
$port.PortName = "COM3"
$port.BaudRate = 115200
$port.Parity = "None"
$port.DataBits = 8
$port.StopBits = "One"
$port.ReadTimeout = 500
$port.WriteTimeout = 500
try { $port.Open() } catch { Write-Output "OPEN FAIL: $_"; exit 1 }
Start-Sleep -Milliseconds 300
try { $port.DiscardInBuffer() } catch {}
$port.Write("$cmd`r`n")
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$out = ""
while ($sw.Elapsed.TotalSeconds -lt 7) {
    try { $out += $port.ReadExisting() } catch {}
    Start-Sleep -Milliseconds 150
}
$port.Close()
Write-Output $out
