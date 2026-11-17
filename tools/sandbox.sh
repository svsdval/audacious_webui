#!/bin/sh
# Запускает отдельный экземпляр Audacious с плагином Web Remote, не трогая
# ни системный каталог плагинов, ни настройки и плейлисты пользователя.
#
# Audacious вычисляет свои каталоги относительно собственного исполняемого
# файла, поэтому достаточно скопировать бинарник в «песочницу» и собрать
# рядом каталог плагинов из ссылок на системные плагины.
#
#   tools/sandbox.sh build/webremote.so /usr/lib/x86_64-linux-gnu/audacious [аргументы audacious]
#
# Переменные: SANDBOX — каталог песочницы, PORT — порт веб-интерфейса.
set -e

plugin=$(realpath "$1")
sys_plugins=${2%/}
shift 2

sandbox=${SANDBOX:-$(dirname "$plugin")/sandbox}
port=${PORT:-8422}
bin=$(command -v audacious)
prefix=$(dirname "$(dirname "$bin")")            # обычно /usr
rel=${sys_plugins#"$prefix"/}                    # lib/<arch>/audacious

rm -rf "$sandbox/bin" "$sandbox/${rel%%/*}"
mkdir -p "$sandbox/bin"
cp "$bin" "$sandbox/bin/audacious"
[ -e "$sandbox/share" ] || ln -s "$prefix/share" "$sandbox/share"

for dir in "$sys_plugins"/*/; do
    type=$(basename "$dir")
    mkdir -p "$sandbox/$rel/$type"
    ln -s "$dir"*.so "$sandbox/$rel/$type/"
done
ln -sf "$plugin" "$sandbox/$rel/General/webremote.so"
ln -sf "$(dirname "$plugin")/webremote-stream.so" "$sandbox/$rel/Effect/webremote-stream.so"

# Экземпляр №9: свой каталог настроек и своё имя в D-Bus, поэтому он не
# пересекается с уже запущенным Audacious.
cfg="$sandbox/config/audacious-9"
registry="$cfg/plugin-registry"
mkdir -p "$cfg"
[ -e "$cfg/config" ] || printf '[web_remote]\nport=%s\n' "$port" > "$cfg/config"

run() { XDG_CONFIG_HOME="$sandbox/config" "$sandbox/bin/audacious" -9 "$@"; }

# Плагин по умолчанию выключен, а реестр плагинов Audacious записывает при
# выходе. Поэтому при первом запуске даём ему стартовать и завершиться,
# после чего отмечаем плагин в реестре включённым.
if ! grep -q 'webremote\.so' "$registry" 2>/dev/null; then
    echo "Первый запуск: регистрация плагина…"
    XDG_CONFIG_HOME="$sandbox/config" "$sandbox/bin/audacious" -9 -H >/dev/null 2>&1 &
    pid=$!
    sleep 3
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    grep -q 'webremote\.so' "$registry" 2>/dev/null || { echo "Audacious не увидел плагин" >&2; exit 1; }
    awk '/^[a-z]+ \// { ours = /webremote\.so$/ } ours && /^enabled / { $0 = "enabled 1" } { print }' \
        "$registry" > "$registry.tmp" && mv "$registry.tmp" "$registry"
fi

echo "Песочница: $sandbox"
echo "Веб-интерфейс: http://localhost:$(sed -n 's/^port=//p' "$cfg/config" | head -1)/"
run "$@"
