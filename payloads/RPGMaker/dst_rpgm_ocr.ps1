# 本地图片文字识别工作进程。只通过标准输入接收已解码 PNG，不读游戏文件，不联网。
# 每个请求独立报告解码/OCR 故障；失败不返回伪造文字，父进程仍保留原图。
$ErrorActionPreference = 'Stop'
[Console]::InputEncoding = New-Object Text.UTF8Encoding($false)
[Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
Add-Type -AssemblyName System.Runtime.WindowsRuntime
Add-Type -AssemblyName System.Drawing
$null = [Windows.Media.Ocr.OcrEngine,Windows.Foundation,ContentType=WindowsRuntime]
$null = [Windows.Globalization.Language,Windows.Foundation,ContentType=WindowsRuntime]
$null = [Windows.Graphics.Imaging.BitmapDecoder,Windows.Graphics.Imaging,ContentType=WindowsRuntime]
$null = [Windows.Graphics.Imaging.SoftwareBitmap,Windows.Graphics.Imaging,ContentType=WindowsRuntime]
$awaitMethod = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
} | Select-Object -First 1
function Await-OcrOperation($Operation, [Type]$ResultType) {
    $task = $awaitMethod.MakeGenericMethod($ResultType).Invoke($null, @($Operation))
    if (-not $task.Wait(12000)) { $Operation.Cancel(); throw 'Windows OCR operation timed out' }
    return $task.GetAwaiter().GetResult()
}
$language = [Windows.Media.Ocr.OcrEngine]::AvailableRecognizerLanguages | Where-Object { $_.LanguageTag -match '^en-' } | Select-Object -First 1
if (-not $language) { throw 'Windows English OCR language pack is not installed' }
$engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromLanguage($language)
if (-not $engine) { throw 'Windows English OCR engine could not be created' }
function Invoke-ImageOcr([string]$Encoded) {
    if (-not $Encoded -or $Encoded.Length -gt 2796204) { throw 'PNG input exceeds 2 MiB' }
    $bytes = [Convert]::FromBase64String($Encoded)
    $inputStream = [IO.MemoryStream]::new($bytes, $false)
    try {
        $inputBitmap = [Drawing.Bitmap]::new($inputStream)
        try {
            $width = $inputBitmap.Width; $height = $inputBitmap.Height
            if ($width -lt 1 -or $height -lt 1 -or $width * $height -gt 2097152) { throw 'Decoded image size exceeds bounds' }
            $scale = [Math]::Min(3, [Math]::Floor([Windows.Media.Ocr.OcrEngine]::MaxImageDimension / [Math]::Max($width, $height)))
            if ($scale -lt 1) { throw 'Image exceeds Windows OCR maximum dimension' }
            $passes = @()
            foreach ($background in @('Black', 'White')) {
                $flat = [Drawing.Bitmap]::new($width * $scale, $height * $scale, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
                try {
                    $graphics = [Drawing.Graphics]::FromImage($flat)
                    try {
                        $graphics.Clear([Drawing.Color]::FromName($background))
                        $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                        $graphics.DrawImage($inputBitmap, 0, 0, $flat.Width, $flat.Height)
                    } finally { $graphics.Dispose() }
                    $memory = [IO.MemoryStream]::new()
                    try {
                        $flat.Save($memory, [Drawing.Imaging.ImageFormat]::Png); $memory.Position = 0
                        $random = [IO.WindowsRuntimeStreamExtensions]::AsRandomAccessStream($memory)
                        try {
                            $decoder = Await-OcrOperation ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($random)) ([Windows.Graphics.Imaging.BitmapDecoder])
                            $bitmap = Await-OcrOperation ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
                            try {
                                $result = Await-OcrOperation ($engine.RecognizeAsync($bitmap)) ([Windows.Media.Ocr.OcrResult])
                                $lines = @(foreach ($line in $result.Lines) {
                                    [ordered]@{ words = @(foreach ($word in $line.Words) {
                                        $r = $word.BoundingRect
                                        [ordered]@{ text = $word.Text; x = $r.X / $scale; y = $r.Y / $scale; w = $r.Width / $scale; h = $r.Height / $scale }
                                    }) }
                                })
                                $passes += [ordered]@{ background = $background; lines = $lines }
                            } finally { $bitmap.Dispose() }
                        } finally { $random.Dispose() }
                    } finally { $memory.Dispose() }
                } finally { $flat.Dispose() }
            }
            return [ordered]@{ width = $width; height = $height; passes = $passes }
        } finally { $inputBitmap.Dispose() }
    } finally { $inputStream.Dispose() }
}
[Console]::WriteLine('{"ready":true,"version":1}')
while ($null -ne ($line = [Console]::ReadLine())) {
    $request = $null
    try { $request = $line | ConvertFrom-Json }
    catch { [Console]::Error.WriteLine('image-ocr-request-json: ' + $_.Exception.Message); exit 2 }
    if (-not $request -or $request.id -notmatch '^\d{1,10}$') { [Console]::Error.WriteLine('image-ocr-request: invalid request id'); exit 2 }
    # 这里只包围外部 PNG 解码和系统 OCR 请求；故障带请求号返回，不吞掉工作进程错误。
    try { $recognized = Invoke-ImageOcr $request.png }
    catch {
        $message = 'image-ocr-decode-recognize id=' + $request.id + ': ' + $_.Exception.Message
        [Console]::Error.WriteLine($message)
        [Console]::WriteLine((@{ id = $request.id; error = $message } | ConvertTo-Json -Compress))
        continue
    }
    [Console]::WriteLine((@{ id = $request.id; result = $recognized } | ConvertTo-Json -Depth 8 -Compress))
}
