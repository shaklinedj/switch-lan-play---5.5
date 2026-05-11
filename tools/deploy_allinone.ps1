
# Automatiza la copia de los binarios finales recién compilados a all_in_one
$root = "c:\Users\Dell\Desktop\switch-lan-play\switch-lan-play"

# Mapeo origen -> destino
$copies = @(
    # lan-play: configurador, sysmodule y debug
    @{ src = "$root\hbapp\lanplay-setup.nro";                              dst = "$root\all_in_one\switch\lan-play\lanplay-setup.nro" },
    @{ src = "$root\sysmodule\lanplay-sys-debug.nro";                      dst = "$root\all_in_one\switch\lan-play\lanplay-sys-debug.nro" },
    @{ src = "$root\sysmodule\lanplay-debug.nro";                          dst = "$root\all_in_one\switch\lan-play\lanplay-debug.nro" },
    # lan-play sysmodule: exefs.nsp (formato requerido por Atmosphere 1.11+)
    @{ src = "$root\sysmodule\exefs.nsp";                                      dst = "$root\all_in_one\atmosphere\contents\42000000000000B1\exefs.nsp" },

    # lan-play sysmodule: toolbox.json y boot2.flag
    @{ src = "$root\sysmodule\toolbox.json";                                          dst = "$root\all_in_one\atmosphere\contents\42000000000000B1\toolbox.json" },
    @{ src = "$root\sysmodule\atmosphere\contents\42000000000000B1\flags\boot2.flag"; dst = "$root\all_in_one\atmosphere\contents\42000000000000B1\flags\boot2.flag" },

    # ldn_mitm: sysmodule NSP (TID 4200000000000010)
    @{ src = "$root\ldn_mitm-1.25.1\ldn_mitm\ldn_mitm.nsp";               dst = "$root\all_in_one\atmosphere\contents\4200000000000010\exefs.nsp" },

    # ldn_mitm: boot2.flag y toolbox.json (desde release oficial)
    @{ src = "C:\Users\Dell\Desktop\ldn_mitm_v1.25.1_release\atmosphere\contents\4200000000000010\flags\boot2.flag"; dst = "$root\all_in_one\atmosphere\contents\4200000000000010\flags\boot2.flag" },
    @{ src = "C:\Users\Dell\Desktop\ldn_mitm_v1.25.1_release\atmosphere\contents\4200000000000010\toolbox.json";    dst = "$root\all_in_one\atmosphere\contents\4200000000000010\toolbox.json" },

    # ldn_mitm: configurador NRO
    @{ src = "$root\ldn_mitm-1.25.1\ldnmitm_config\ldnmitm_config.nro";   dst = "$root\all_in_one\switch\ldnmitm_config\ldnmitm_config.nro" },

    # ldn_mitm: overlay (Tesla) - desde release oficial pre-compilado
    @{ src = "C:\Users\Dell\Desktop\ldn_mitm_v1.25.1_release\switch\.overlays\ldnmitm_config.ovl"; dst = "$root\all_in_one\switch\.overlays\ldnmitm_config.ovl" }
)

foreach ($item in $copies) {
    $d = Split-Path $item.dst -Parent
    if ($d.Length -gt 3 -and !(Test-Path -LiteralPath $d)) {
        New-Item -ItemType Directory -Path $d -Force | Out-Null
    }
    if (Test-Path $item.src) {
        Copy-Item $item.src $item.dst -Force
        Write-Host "OK: $(Split-Path $item.dst -Leaf)"
    } else {
        Write-Warning "NO ENCONTRADO: $($item.src)"
    }
}

Write-Host "Copia completada. Verifica fechas y tamaños en all_in_one."