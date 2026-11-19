#!/usr/bin/env python3
"""Revisa que un commit o un push no lleve claves ni archivos de configuración local.

Lo llaman los hooks globales `pre-commit` y `pre-push` de esta carpeta, que `instalar.ps1` o
`instalar.sh` dejan activos para todos los repos de la PC. Solo usa la biblioteca estándar.

    revisar_secretos.py pre-commit          # lo que está en el área de preparación
    revisar_secretos.py pre-push < refs     # los commits que se van a subir (entrada de git)

Sale con 1 si encuentra algo, y lo muestra sin el valor de la clave.

Las reglas de contenido son las de `.gitleaks.toml` (el CI de aura-firmware), más la forma
`#define ALGO_PASS "…"`. Un valor de ejemplo ("CAMBIAR", "", "<clave>", bytes en cero) pasa.
"""
import re
import subprocess
import sys
from dataclasses import dataclass

# --- Archivos que nunca se suben -------------------------------------------------------------

NOMBRES_PROHIBIDOS = {"config_local.h", "credenciales.h", ".env"}
EXTENSIONES_PROHIBIDAS = (".pem", ".key")


def archivo_prohibido(ruta: str) -> bool:
    """True si el archivo no se versiona nunca: configuración local o clave privada."""
    partes = ruta.replace("\\", "/").split("/")
    nombre = partes[-1]
    return (nombre in NOMBRES_PROHIBIDOS
            or nombre.lower().endswith(EXTENSIONES_PROHIBIDAS)
            or "claves" in partes[:-1])


# --- Reglas de contenido ---------------------------------------------------------------------

# Un valor que es un ejemplo, una variable o código, no una clave.
_EJEMPLO = re.compile(r"(?i)^(?:|CAMBIAR\w*|TU_CLAVE|changeme|x+|\*+|\.\.\.|…|<[^>]*>|\$\{?[A-Z_]+\}?)$")
_CONSTANTE_C = re.compile(r"^\(?[A-Z][A-Z0-9]*_[A-Z0-9_]+(?:\[\d+\])?$")
_CODIGO = re.compile(r"^[A-Za-z_][\w.]*\(|^[a-z_]\w*(?:\.[a-z_]\w*)+$")
_BYTES_EN_CERO = re.compile(r"(?i)^(?:0x00\s*,\s*)*0x00$")


def _es_ejemplo(valor: str) -> bool:
    valor = valor.strip()
    return bool(_EJEMPLO.match(valor) or _CONSTANTE_C.match(valor)
                or _CODIGO.match(valor) or _BYTES_EN_CERO.match(valor) or re.fullmatch(r"0+", valor))


@dataclass
class Regla:
    nombre: str
    patron: re.Pattern


REGLAS = [
    Regla("clave privada", re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY( BLOCK)?-----()")),
    Regla("clave en un #define", re.compile(
        r"(?i)#\s*define\s+\w*(?:pass|passwd|password|clave|secret|token|psk)\w*\s+\"([^\"]*)\"")),
    Regla("clave de WiFi", re.compile(
        r"(?i)(?:wifi_?pass(?:word)?|ssid_?pass|wifi_?clave)\s*(?:=|\s)\s*\"([^\"]*)\"")),
    Regla("clave de la mesh o del router", re.compile(
        r"(?i)(?:mesh_?clave|mesh_?ap_?pass|router_?clave|router_?pass(?:word)?)\s*(?:=|\s)\s*\"([^\"]*)\"")),
    Regla("ID de la mesh", re.compile(
        r"(?i)mesh_?id\w*[^\n]*?((?:0x[0-9a-f]{2}\s*,\s*){5}0x[0-9a-f]{2})")),
    Regla("clave AES (bytes)", re.compile(
        r"(?i)(?:app_?key|nwk_?key|pmk|lmk|aes_?key)[^=\n]*=\s*\{\s*((?:0x[0-9a-f]{2}\s*,\s*){15}0x[0-9a-f]{2})")),
    Regla("clave AES (hexadecimal)", re.compile(
        r"(?i)(?:app_?key|nwk_?key|pmk|lmk|aes_?key)[^\n]{0,20}?[\"']([0-9a-f]{32})[\"']")),
    Regla("contraseña o token", re.compile(
        r"(?i)(?:password|passwd|contrase(?:n|ñ)a|mqtt_?pass|api_?token|secret)\s*[:=]\s*[\"']?([^\s\"';,]{6,})")),
]


@dataclass
class Hallazgo:
    regla: str
    linea_tapada: str  # la línea con el valor reemplazado por ****


def revisar_linea(linea: str) -> list:
    """Hallazgos de una línea de texto. Una línea da a lo sumo un hallazgo."""
    for regla in REGLAS:
        for m in regla.patron.finditer(linea):
            valor = m.group(1) if regla.nombre != "clave privada" else "x"
            if regla.nombre != "clave privada" and _es_ejemplo(valor):
                continue
            if regla.nombre == "clave privada":
                tapada = linea.strip()
            else:
                tapada = (linea[:m.start(1)] + "****" + linea[m.end(1):]).strip()
            return [Hallazgo(regla.nombre, tapada[:120])]
    return []


# --- Lectura de lo que git va a commitear o subir --------------------------------------------

def _git(*args: str) -> str:
    r = subprocess.run(["git", "-c", "core.quotepath=off", *args],
                       capture_output=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout


def _existe(commit: str) -> bool:
    return subprocess.run(["git", "cat-file", "-e", f"{commit}^{{commit}}"],
                          capture_output=True).returncode == 0


def _lineas_agregadas(diff: str):
    """(ruta, número de línea, texto) de cada línea agregada en un diff con -U0."""
    ruta, n = None, 0
    for linea in diff.splitlines():
        if linea.startswith("+++ "):
            ruta = linea[6:] if linea.startswith("+++ b/") else None
        elif linea.startswith("@@"):
            m = re.match(r"@@ -\S+ \+(\d+)", linea)
            n = int(m.group(1)) if m else 0
        elif linea.startswith("+") and ruta is not None:
            yield ruta, n, linea[1:]
            n += 1


def _archivos_tocados(nombres: str) -> list:
    """Rutas agregadas, modificadas, copiadas o renombradas de un --name-status -z."""
    campos = nombres.split("\0")
    rutas, i = [], 0
    while i < len(campos) and campos[i]:
        estado = campos[i]
        if estado[0] in "RC":
            rutas.append(campos[i + 2])
            i += 3
        else:
            if estado[0] != "D":
                rutas.append(campos[i + 1])
            i += 2
    return rutas


def revisar_cambio(diff_args: list) -> list:
    """[(dónde, qué)] para un cambio descrito con argumentos de `git diff`."""
    encontrados = []
    for ruta in _archivos_tocados(_git("diff", "--name-status", "-z", "-M", *diff_args)):
        if archivo_prohibido(ruta):
            encontrados.append((ruta, "archivo que nunca se sube (configuración local o clave)"))
    for ruta, n, texto in _lineas_agregadas(_git("diff", "-U0", "-M", "--no-color", *diff_args)):
        if archivo_prohibido(ruta):
            continue
        for h in revisar_linea(texto):
            encontrados.append((f"{ruta}:{n}", f"{h.regla}:  {h.linea_tapada}"))
    return encontrados


# El árbol vacío de git (SHA-1): contra él se compara el primer commit de un repo.
ARBOL_VACIO = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"


def commits_a_subir(entrada: str) -> list:
    """Commits nuevos de un push, a partir de lo que git le pasa al pre-push por stdin."""
    commits = []
    for linea in entrada.splitlines():
        partes = linea.split()
        if len(partes) != 4:
            continue
        _, local, _, remoto = partes
        if set(local) == {"0"}:  # se borra una rama
            continue
        if set(remoto) == {"0"} or not _existe(remoto):
            # Rama nueva, o el remoto tiene commits que esta PC no bajó (push --force): se
            # revisa todo lo que no está en ninguna rama remota conocida.
            rango = [local, "--not", "--remotes"]
        else:
            rango = [f"{remoto}..{local}"]
        commits += _git("rev-list", "--reverse", *rango).split()
    return list(dict.fromkeys(commits))


def revisar_commit(commit: str) -> list:
    """Lo que agrega un commit respecto de su primer padre (o de nada, si es el primero)."""
    padres = _git("rev-list", "--parents", "-n", "1", commit).split()[1:]
    return revisar_cambio([padres[0] if padres else ARBOL_VACIO, commit])


# --- Mensajes --------------------------------------------------------------------------------

AYUDA_COMMIT = """\
Qué hacer:
  - Sacalo del commit:  git restore --staged <archivo>
  - Agregá ese archivo al .gitignore, o dejá en el código un valor de ejemplo ("CAMBIAR").
  - Las claves van en config_local.h (o credenciales.h), que no se sube nunca.
  - Si esa clave ya estaba subida antes, borrarla no alcanza: queda en el historial.
    Avisale a la cátedra para que la cambie."""

AYUDA_PUSH = """\
Esos commits no se pueden subir así. Qué hacer:
  - Si es el último commit: sacá la clave o el archivo y corregilo con  git commit --amend
  - Si es uno anterior, o no sabés cómo seguir, consultá a la cátedra antes de subir.
  - Ojo con "Revert" y "cherry-pick": pueden volver a poner una clave que ya habías sacado.
  - Si alguna vez se subió, borrarla no alcanza: queda en el historial. Avisale a la cátedra."""


def _informar(titulo: str, encontrados: list, ayuda: str) -> None:
    print(f"\nAURA - revisión de secretos: {titulo}\n", file=sys.stderr)
    for donde, que in encontrados:
        print(f"  {donde}\n      {que}", file=sys.stderr)
    print(f"\n{ayuda}\n\nSi es un falso positivo, consultá a la cátedra.\n", file=sys.stderr)


def main(argv: list) -> int:
    modo = argv[1] if len(argv) > 1 else ""
    if modo == "pre-commit":
        encontrados = revisar_cambio(["--cached"])
        if encontrados:
            _informar("el commit lleva algo que parece una clave", encontrados, AYUDA_COMMIT)
            return 1
        return 0
    if modo == "pre-push":
        encontrados = []
        for commit in commits_a_subir(sys.stdin.read()):
            corto = commit[:7]
            encontrados += [(f"{donde}  (commit {corto})", que) for donde, que in revisar_commit(commit)]
        if encontrados:
            _informar("el push lleva algo que parece una clave", encontrados, AYUDA_PUSH)
            return 1
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except RuntimeError as e:
        print(f"\nAURA - revisión de secretos: no pude revisar ({e}).\n"
              "Consultá a la cátedra antes de saltear la revisión.\n", file=sys.stderr)
        sys.exit(1)
