Add-Type -AssemblyName System.Drawing
$fs = [System.IO.File]::OpenRead((Resolve-Path "public\icons\icon.ico").Path)
$bf = new-object System.IO.BinaryReader($fs)
$h = $bf.ReadBytes($fs.Length)
$fs.Close()
$ms = new-object System.IO.MemoryStream($h, 0, $h.Length)
$img = [System.Drawing.Image]::FromStream($ms)
$ms.Dispose()

$resDir = "android\app\src\main\res"
$sizes = @{
    "mipmap-mdpi" = 48
    "mipmap-hdpi" = 72
    "mipmap-xhdpi" = 96
    "mipmap-xxhdpi" = 144
    "mipmap-xxxhdpi" = 192
}

foreach ($entry in $sizes.GetEnumerator()) {
    $dir = "$resDir\$($entry.Key)"
    $sz = $entry.Value
    $bmp = New-Object System.Drawing.Bitmap($sz, $sz)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($img, 0, 0, $sz, $sz)
    $g.Dispose()
    $bmp.Save("$dir\ic_launcher.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Save("$dir\ic_launcher_round.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Save("$dir\ic_launcher_foreground.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "Generated $($entry.Key) ($($sz)x$($sz))"
}

$img.Dispose()
Write-Host "All icons generated"
