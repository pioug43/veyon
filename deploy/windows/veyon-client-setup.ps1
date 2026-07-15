<#
.SYNOPSIS
    Configure, en ligne de commande et de facon idempotente, un POSTE Windows du
    pool pour la surveillance Veyon par le portail VDI.

    Authentification = KEYFILE : la cle PUBLIQUE du portail (integree ci-dessous,
    non secrete) est importee sur le poste ; la cle PRIVEE reste dans le portail.
    Le poste ne connait jamais le portail : c'est le SERVEUR Web API Veyon
    (sur l'hote Windows, api_url cote portail) qui vient chercher chaque ecran
    via le service Veyon du poste (port 11100).

    Surveillance = 100% Veyon. AUCUN VNC n'est installe (par choix : la prise de
    main se fait via l'application Veyon Master). Le script n'installe donc jamais
    de serveur VNC tiers.

    Le build installe est celui du dépôt pioug43/veyon : il embarque le
    plugin "exammode" (mode examen supervise). A defaut de -VeyonUrl explicite,
    l'installeur win64 de la DERNIERE release du fork est resolu automatiquement.

.PARAMETER PortalIP
    IP SOURCE du SERVEUR Web API Veyon (l'hote) telle que ce poste la voit :
    adresse autorisee dans le pare-feu. Ce n'est PAS l'IP du poste.

.EXAMPLE
    # 134.214.252.70 = IP du SERVEUR Veyon (hote) vue par le poste
    powershell -ExecutionPolicy Bypass -File .\veyon-client-setup.ps1 -PortalIP 134.214.252.70

.NOTES
    A executer en tant que FICHIER (voir .EXAMPLE) ou via  irm <url> | iex  :
    ne PAS coller le contenu ligne par ligne dans la console (l'analyse
    interactive casse les blocs multi-lignes).
#>
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [string] $PortalIP    = "134.214.181.2",
    [string] $VeyonRepo   = "pioug43/veyon",
    [string] $VeyonUrl    = "",
    [string] $KeyName     = "portail"
)

$ErrorActionPreference = "Stop"
$Cli = "C:\Program Files\Veyon\veyon-cli.exe"
$AUTH_KEYFILE = "{0c69b301-81b4-42d6-8fae-128cdd113314}"
$FwNames = @("Veyon Server (serveur portail)")

# Cle PUBLIQUE partagee du portail (NON secrete) - integree pour l'autonomie.
$EmbeddedPublicKey = @'
-----BEGIN PUBLIC KEY-----
MIICIjANBgkqhkiG9w0BAQEFAAOCAg8AMIICCgKCAgEA6n9hAMKN5RsWT1H5rM0a
RHCmFiL+gqmS5mOFIgZuMTYjSFAm2rZ+tDIl2Ol5NsQnlWsNZ+uYusglHHfY2P2v
WtNAow04ivrIgdFUj48vzFEc39RSQX3oLSF2oZ1b7E+HH6WiKza4O0Gbu6FbxrAP
9qJ5uV+p8N7qZjdLU7tJsVyqJzJmo+WCcTqz08v+ShRNS9GPZb3rYjWhUB1WW48Y
jKh1TUi1RJPx/HhealozPleO/jnAR+Y9YA/H2IP0nu7lzMIJS1P5iK/IBE+gMjw9
dlOIWiFSyW6jCk49oy2rY84DEEcMN9xiQfL5HnlbmdBDMFANLBEvFNPtPIVBE+63
kDKVFiG+E8fFAdBrCnHgduushoD9m4BclpJhg1LRfklLOhcAfpl1rPrqw7aHy+uS
T04I3BUZKtLKmjRlbsKOHEmxeQPQHmLo54ihZBInjR8N6JSQq/+OdMo2GiuV2o+v
lykjcuFB+FtQE2Lsec+SC8/9iSkmxJU1Oy8ROtgXG4Je38wKoAIj/17ZqRgDs0pk
vndoxaN7T+iUKhMRA8//wq4WISv9xm6uFt7bBsyd10sev9D13i0F5IpFnVdstu1y
z2rCsMkUKsRhId1HSgR5CkpPCrZCQ4CKqClT1Qy7baV1NAOAnTyDrLA8gywqAfrV
CJCMklKYpdQfjJmyRKvgDtECAwEAAQ==
-----END PUBLIC KEY-----
'@

function Step($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }

# veyon-cli.exe ecrit ses journaux ([INFO]...) sur stderr. Sous
# $ErrorActionPreference='Stop', PowerShell transforme toute ecriture stderr
# d'un binaire natif en erreur terminante (NativeCommandError) meme avec 2>$null.
# On fusionne stderr dans stdout (2>&1) et on bascule localement en 'Continue'
# pour que ces journaux informatifs n'interrompent pas le script.
function Invoke-Cli {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $Cli @args 2>&1
        return ($out | ForEach-Object { "$_" })
    } finally {
        $ErrorActionPreference = $prev
    }
}

# ---------------------------------------------------------------- 0. Nettoyage
Step "Nettoyage de l'etat precedent (cles Veyon + regles pare-feu)"
foreach ($n in $FwNames) {
    Get-NetFirewallRule -DisplayName $n -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue
}
if (Test-Path $Cli) {
    $keys = @(Invoke-Cli authkeys list | Where-Object { $_ -match '/(public|private)\s*$' })
    foreach ($k in $keys) { Invoke-Cli authkeys delete $k.Trim() | Out-Null }
    Write-Host ("  cles Veyon supprimees : {0}" -f $keys.Count)
}

# ---------------------------------------------------------------- A. Veyon
# A defaut d'URL explicite, resoudre l'installeur win64 de la derniere release
# du fork pioug43 (embarque le plugin exammode). Le nom du .exe varie (numero de
# build) : on interroge l'API GitHub plutot que de coder une URL en dur.
if (-not $VeyonUrl) {
    Step "Resolution de l'installeur Veyon (fork $VeyonRepo, avec plugin examen)"
    # A defaut de release Windows du fork, on retombe sur Veyon officiel : le
    # poste reste supervisable/pilotable, mais SANS mode examen (pas d'exammode).
    $OfficialUrl = "https://github.com/veyon/veyon/releases/download/v4.9.4/veyon-4.9.4.0-win64-setup.exe"
    try {
        $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$VeyonRepo/releases/latest" `
            -Headers @{ "User-Agent" = "veyon-client-setup"; "Accept" = "application/vnd.github+json" }
        $asset = $rel.assets | Where-Object { $_.name -match "win64.*setup\.exe$" } | Select-Object -First 1
    } catch { $asset = $null }
    if ($asset) {
        $VeyonUrl = $asset.browser_download_url
        Write-Host ("  Installeur : {0} ({1})" -f $asset.name, $rel.tag_name)
    } else {
        $VeyonUrl = $OfficialUrl
        Write-Warning "  Aucune release Windows du fork $VeyonRepo : repli sur Veyon officiel (mode examen INDISPONIBLE sur ce poste)."
    }
}

Step "Veyon : telechargement + installation silencieuse"
$veyonExe = Join-Path $env:TEMP "veyon-setup.exe"
Invoke-WebRequest -Uri $VeyonUrl -OutFile $veyonExe
Start-Process $veyonExe -ArgumentList "/S" -Wait
if (-not (Test-Path $Cli)) { throw "veyon-cli introuvable ($Cli) : installation echouee." }
# Verifie que le plugin mode examen est bien present (build du fork)
if (Test-Path "C:\Program Files\Veyon\plugins\exammode.dll") {
    Write-Host "  Plugin mode examen : present (exammode.dll)"
} else {
    Write-Warning "  Plugin mode examen ABSENT : ce build n'est pas celui du fork pioug43 - le mode examen ne s'appliquera pas."
}

Step "Veyon : methode keyfile + import de la cle publique du portail"
Invoke-Cli config set Authentication/Method $AUTH_KEYFILE | Out-Null
$pub = Join-Path $env:TEMP "$KeyName-public.pem"
Set-Content -Path $pub -Value $EmbeddedPublicKey -Encoding Ascii
Invoke-Cli authkeys import "$KeyName/public" $pub | Out-Null
Remove-Item $pub -Force
Restart-Service VeyonService

# ---------------------------------------------------------------- B. Pare-feu
Step "Pare-feu : Veyon Server (11100) depuis le serveur portail"
New-NetFirewallRule -DisplayName "Veyon Server (serveur portail)" -Direction Inbound -Protocol TCP `
    -LocalPort 11100 -RemoteAddress $PortalIP -Action Allow | Out-Null

# ---------------------------------------------------------------- C. Resume
Step "Termine - poste pret (keyfile, cle publique integree)"
Write-Host ("  Poste            : {0}" -f $env:COMPUTERNAME)
Write-Host  "  Auth Veyon       : keyfile (cle publique du portail importee)"
Write-Host  "  Mode examen      : plugin exammode embarque (pilote par le portail via la Web API)"
Write-Host ("  Source serveur   : {0} (Veyon Server 11100)" -f $PortalIP)
Write-Host  "  Prise de main    : via appli Veyon Master (aucun VNC installe)"
Write-Host  "Verif : Get-Service VeyonService ; Get-NetTCPConnection -LocalPort 11100 -State Listen"
