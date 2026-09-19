<#
.SYNOPSIS
  Tiepolo Pro版のライセンス署名用ECDSA P-256鍵ペアを生成する。

.DESCRIPTION
  秘密鍵は -OutFile で指定したファイル(既定: tiepolo_license.privatekey.json)に
  保存される。このファイルは絶対に配布せず、このリポジトリの外の安全な場所に
  保管すること(gitignore対象にするか、リポジトリ内には置かないこと)。

  公開鍵のX/Y座標はC++の配列初期化子として標準出力にも表示されるので、
  src/backend/LicensePublicKey.h の kTiepoloLicensePublicKeyX / Y の中身へ
  そのまま貼り付ける。

.PARAMETER OutFile
  秘密鍵の保存先パス。

.EXAMPLE
  ./New-TiepoloLicenseKeyPair.ps1 -OutFile C:\secure\tiepolo_license.privatekey.json
#>
param(
    [string]$OutFile = "tiepolo_license.privatekey.json"
)

$ecdsa = [System.Security.Cryptography.ECDsa]::Create([System.Security.Cryptography.ECCurve+NamedCurves]::nistP256)
$ecParams = $ecdsa.ExportParameters($true)

$obj = [ordered]@{
    curve = "nistP256"
    d     = [Convert]::ToBase64String($ecParams.D)
    x     = [Convert]::ToBase64String($ecParams.Q.X)
    y     = [Convert]::ToBase64String($ecParams.Q.Y)
}
$obj | ConvertTo-Json | Set-Content -Path $OutFile -Encoding utf8

function Format-CppByteArray([byte[]]$bytes) {
    ($bytes | ForEach-Object { "0x{0:x2}" -f $_ }) -join ", "
}

Write-Host ""
Write-Host "秘密鍵を保存しました: $OutFile"
Write-Host "*** このファイルは絶対に配布しないでください。安全な場所に保管してください。 ***"
Write-Host ""
Write-Host "以下を src/backend/LicensePublicKey.h の配列の中身にそのまま貼り付けてください:"
Write-Host ""
Write-Host "kTiepoloLicensePublicKeyX = {"
Write-Host "    $(Format-CppByteArray $ecParams.Q.X),"
Write-Host "};"
Write-Host "kTiepoloLicensePublicKeyY = {"
Write-Host "    $(Format-CppByteArray $ecParams.Q.Y),"
Write-Host "};"
