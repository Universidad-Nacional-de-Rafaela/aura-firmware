#!/bin/sh
# Instala la revisión de secretos de AURA en todos los repos git de esta PC (Linux o macOS).
#   sh instalar.sh                # instala o actualiza
#   sh instalar.sh --desinstalar
set -e
ORIGEN=$(cd "$(dirname "$0")" && pwd)
DESTINO="${XDG_CONFIG_HOME:-$HOME/.config}/aura/git-hooks"

actual=$(git config --global --get core.hooksPath || true)

if [ "$1" = "--desinstalar" ]; then
    if [ "$actual" = "$DESTINO" ]; then
        git config --global --unset core.hooksPath
    fi
    rm -rf "$DESTINO"
    echo "Listo: se quitó la revisión de secretos de AURA."
    exit 0
fi

if [ -n "$actual" ] && [ "$actual" != "$DESTINO" ]; then
    echo "Ya tenés hooks globales en: $actual" >&2
    echo "No los piso. Consultá a la cátedra para combinarlos." >&2
    exit 1
fi

if ! python3 -c "import sys; sys.exit(sys.version_info < (3, 8))" 2>/dev/null; then
    echo "Hace falta Python 3.8 o más nuevo (python3)." >&2
    exit 1
fi

mkdir -p "$DESTINO"
for f in pre-commit pre-push comun.sh revisar_secretos.py; do
    cp "$ORIGEN/$f" "$DESTINO/$f"
done
chmod +x "$DESTINO/pre-commit" "$DESTINO/pre-push"
git config --global core.hooksPath "$DESTINO"

echo "Listo: desde ahora git revisa cada commit y cada push, en todos tus repos."
echo "Hooks en: $DESTINO"
