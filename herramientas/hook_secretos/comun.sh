# Funciones compartidas por pre-commit y pre-push (se cargan con ".", no se ejecutan solas).

# Python 3: en Windows suele estar como "py -3" o "python"; "python3" puede ser el acceso
# directo de la Microsoft Store, que no hace nada. Se prueba cada uno de verdad.
PY=""
for candidato in "py -3" python3 python; do
    if $candidato -c "import sys; sys.exit(sys.version_info < (3, 8))" >/dev/null 2>&1; then
        PY="$candidato -I"
        break
    fi
done
if [ -z "$PY" ]; then
    echo "AURA - revisión de secretos: no encuentro Python 3, así que no puedo revisar." >&2
    echo "Instalalo (viene con ESP-IDF) o consultá a la cátedra." >&2
    exit 1
fi

# El hook del mismo nombre que el repo tenga en .git/hooks. Con core.hooksPath global, git ya
# no lo corre solo: lo corremos nosotros, con los mismos argumentos y la misma entrada.
correr_hook_del_repo() {
    nombre=$1
    shift
    propio="$(git rev-parse --git-common-dir)/hooks/$nombre"
    if [ -x "$propio" ]; then
        "$propio" "$@"
    fi
}
