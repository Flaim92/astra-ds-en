"""Генерирует src/roots.h: встроенный набор корневых CA для Linux (из ca-certificates Debian 12)."""
import hashlib
import ssl
import subprocess

NAMES = [
    "GTS_Root_R1", "GTS_Root_R2", "GTS_Root_R3", "GTS_Root_R4",
    "GlobalSign_Root_CA", "GlobalSign_Root_CA_-_R3", "GlobalSign_Root_R46", "GlobalSign_ECC_Root_CA_-_R5",
    "ISRG_Root_X1", "ISRG_Root_X2",
    "DigiCert_Global_Root_G2", "DigiCert_Global_Root_G3", "DigiCert_Global_Root_CA",
    "Baltimore_CyberTrust_Root",
    "USERTrust_RSA_Certification_Authority", "USERTrust_ECC_Certification_Authority",
    "Amazon_Root_CA_1", "Amazon_Root_CA_3",
]

out = []
out.append("/* Корневые удостоверяющие центры, которым mincord доверяет на Linux (Windows: системное хранилище")
out.append(" * Schannel). 18 корней вместо ~150 системных: цепочки Discord (Cloudflare / Google Trust Services)")
out.append(" * и запас крупных центров. Меньше центров — меньше тех, кто может подделать сертификат, и ~0,8 МБ")
out.append(" * памяти экономии на каждом хранилище. Источник: пакет ca-certificates Debian 12.")
out.append(" * Сгенерировано 09.11.2026 (tools: gen_roots.py); отпечатки SHA256 — в комментариях. */")
out.append("static const char ROOTS_PEM[] =")
for n in NAMES:
    pem = subprocess.run(["openssl", "x509", "-in", f"/etc/ssl/certs/{n}.pem", "-outform", "PEM"],
                         check=True, capture_output=True, text=True).stdout
    der = ssl.PEM_cert_to_DER_cert(pem)
    fp = hashlib.sha256(der).hexdigest().upper()
    fp = ":".join(fp[i:i + 2] for i in range(0, len(fp), 2))
    out.append(f"    /* {n}")
    out.append(f"     * SHA256 {fp} */")
    for line in pem.strip().split("\n"):
        out.append('    "' + line + '\\n"')
out.append("    ;")
open("/root/roots.h", "w").write("\n".join(out) + "\n")
print("корней:", len(NAMES))
