<#
.SYNOPSIS
  秘密鍵ファイルを使ってTiepolo Proのライセンスファイル(*.tiepololicense)を発行する。

.DESCRIPTION
  New-TiepoloLicenseKeyPair.ps1 で生成した秘密鍵ファイルを使い、購入者向けの
  署名付きライセンスファイルを1つ作る。このファイルをアプリの
  %APPDATA%/pwxwx/Tiepolo/license.tiepololicense に配置する(またはファイル選択で
  読み込む)とPro機能が解放される。

.PARAMETER KeyFile
  New-TiepoloLicenseKeyPair.ps1で生成した秘密鍵ファイル。

.PARAMETER Licensee
  ライセンスの発行先(表示用。例: メールアドレスや購入者名)。

.PARAMETER ExpiresAt
  有効期限(YYYY-MM-DD)。省略時は無期限ライセンスになる。

.PARAMETER OutFile
  出力するライセンスファイルのパス。

.EXAMPLE
  ./New-TiepoloLicense.ps1 -KeyFile C:\secure\tiepolo_license.privatekey.json `
      -Licensee "customer@example.com" -OutFile customer_license.tiepololicense

.EXAMPLE
  # 1年間の期限付きライセンス
  ./New-TiepoloLicense.ps1 -KeyFile C:\secure\tiepolo_license.privatekey.json `
      -Licensee "customer@example.com" -ExpiresAt "2027-07-19" -OutFile customer_license.tiepololicense
#>
param(
    [Parameter(Mandatory=$true)][string]$KeyFile,
    [Parameter(Mandatory=$true)][string]$Licensee,
    [string]$ExpiresAt = $null,
    [string]$OutFile = "license.tiepololicense"
)

$keyData = Get-Content -Path $KeyFile -Raw | ConvertFrom-Json

$ecParams = New-Object System.Security.Cryptography.ECParameters
$ecParams.Curve = [System.Security.Cryptography.ECCurve+NamedCurves]::nistP256
$ecParams.D = [Convert]::FromBase64String($keyData.d)
$q = New-Object System.Security.Cryptography.ECPoint
$q.X = [Convert]::FromBase64String($keyData.x)
$q.Y = [Convert]::FromBase64String($keyData.y)
$ecParams.Q = $q

$ecdsa = [System.Security.Cryptography.ECDsa]::Create($ecParams)

$payload = [ordered]@{
    product   = "TiepoloPro"
    licensee  = $Licensee
    issuedAt  = (Get-Date -Format "yyyy-MM-dd")
    expiresAt = $ExpiresAt
    features  = @("pro")
}
# -Compress: 署名対象のバイト列を一意に固定するため、余計な改行・インデントを含まないJSONにする。
$payloadJson  = $payload | ConvertTo-Json -Compress
$payloadBytes = [System.Text.Encoding]::UTF8.GetBytes($payloadJson)

# ECDsa.SignData の既定の戻り値は IEEE P1363 形式 (r||s を単純連結した64バイト、DERラップ無し)。
# Windows CNG の BCryptVerifySignature が ECDSA 鍵に対して期待する形式と同じなので変換は不要。
$signature = $ecdsa.SignData($payloadBytes, [System.Security.Cryptography.HashAlgorithmName]::SHA256)

$license = [ordered]@{
    payload   = [Convert]::ToBase64String($payloadBytes)
    signature = [Convert]::ToBase64String($signature)
}
$license | ConvertTo-Json | Set-Content -Path $OutFile -Encoding utf8

Write-Host "ライセンスファイルを発行しました: $OutFile"
Write-Host "内容: $payloadJson"
