#!/usr/bin/env bash
#
# build-i686-elf.sh
# Собирает i686-elf кросс-компилятор (binutils + gcc) для os-tutorial.
# Тестировалось под Arch/CachyOS. Требует sudo для установки зависимостей.
#
set -euo pipefail

BINUTILS_VER="2.42"
GCC_VER="13.2.0"

export PREFIX="$HOME/opt/cross"
export TARGET=i686-elf
export PATH="$PREFIX/bin:$PATH"

# /tmp на Arch часто смонтирован как tmpfs (в оперативной памяти) с ограниченным
# размером — сборки gcc/binutils легко съедают несколько ГБ временных файлов
# и упираются в "не осталось места" или "превышена дисковая квота".
# Держим всё на диске, в домашней папке.
SRC_DIR="$HOME/i686-elf-src"
export TMPDIR="$SRC_DIR/tmp"
mkdir -p "$TMPDIR"
JOBS="$(nproc)"

log() { echo -e "\n\033[1;32m==> $*\033[0m"; }

# --- 0. Проверка: что уже готово? ---
BINUTILS_DONE=false
GCC_DONE=false

if [[ -x "$PREFIX/bin/${TARGET}-ld" ]]; then
    log "binutils уже установлен: $PREFIX/bin/${TARGET}-ld"
    BINUTILS_DONE=true
fi

if [[ -x "$PREFIX/bin/${TARGET}-gcc" ]]; then
    log "gcc уже установлен: $PREFIX/bin/${TARGET}-gcc"
    GCC_DONE=true
fi

if $BINUTILS_DONE && $GCC_DONE; then
    "$PREFIX/bin/${TARGET}-gcc" --version | head -n1
    echo "Всё уже собрано. Добавь в PATH: export PATH=\"$PREFIX/bin:\$PATH\""
    read -rp "Пересобрать всё заново с нуля? [y/N] " ans
    if [[ ! "$ans" =~ ^[Yy]$ ]]; then
        exit 0
    fi
    BINUTILS_DONE=false
    GCC_DONE=false
fi

# --- 1. Зависимости ---
log "Устанавливаю зависимости через pacman"
sudo pacman -S --needed --noconfirm base-devel gmp libmpc mpfr texinfo curl

# --- 2. Подготовка каталогов ---
log "Готовлю каталоги: $SRC_DIR, $PREFIX"
mkdir -p "$SRC_DIR" "$PREFIX"
cd "$SRC_DIR"

# --- 3. binutils ---
if $BINUTILS_DONE; then
    log "binutils уже установлен — пропускаю"
else
    BINUTILS_TAR="binutils-${BINUTILS_VER}.tar.gz"
    if [[ ! -f "$BINUTILS_TAR" ]]; then
        log "Скачиваю binutils-${BINUTILS_VER}"
        curl -LO "https://ftp.gnu.org/gnu/binutils/${BINUTILS_TAR}"
    fi
    if [[ ! -d "binutils-${BINUTILS_VER}" ]]; then
        tar xf "$BINUTILS_TAR"
    fi

    log "Собираю binutils"
    mkdir -p build-binutils
    cd build-binutils
    if [[ ! -f Makefile ]]; then
        ../binutils-${BINUTILS_VER}/configure \
            --target="$TARGET" \
            --prefix="$PREFIX" \
            --with-sysroot \
            --disable-nls \
            --disable-werror
    fi
    make -j"$JOBS"
    make install
    cd "$SRC_DIR"
fi

# --- 4. gcc ---
if $GCC_DONE; then
    log "gcc уже установлен — пропускаю"
else
    GCC_TAR="gcc-${GCC_VER}.tar.gz"
    if [[ ! -f "$GCC_TAR" ]]; then
        log "Скачиваю gcc-${GCC_VER}"
        curl -LO "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/${GCC_TAR}"
    fi
    if [[ ! -d "gcc-${GCC_VER}" ]]; then
        tar xf "$GCC_TAR"
    fi

    # libcody — host-утилита для C++20 modules, configure подключает её
    # независимо от --enable-languages. Нам она не нужна (нужен только C),
    # а на новых системных gcc она не собирается из-за char8_t/S2C конфликта.
    # Проще всего убрать её из дерева исходников.
    if [[ -d "gcc-${GCC_VER}/libcody" ]]; then
        log "Удаляю libcody из исходников gcc (не нужна для C, ломается на новых gcc)"
        rm -rf "gcc-${GCC_VER}/libcody"
    fi

    log "Собираю gcc (только C, без headers — займёт время)"

    # Сборке gcc нужно 3-5+ ГБ временных файлов
    AVAIL_KB=$(df -Pk "$SRC_DIR" | awk 'NR==2 {print $4}')
    AVAIL_GB=$((AVAIL_KB / 1024 / 1024))
    if [[ "$AVAIL_GB" -lt 5 ]]; then
        echo "Внимание: на разделе с $SRC_DIR свободно всего ${AVAIL_GB} ГБ — для сборки gcc рекомендуется 5+ ГБ."
        read -rp "Продолжить всё равно? [y/N] " ans
        [[ "$ans" =~ ^[Yy]$ ]] || exit 1
    fi

    rm -rf build-gcc
    mkdir build-gcc
    cd build-gcc
    ../gcc-${GCC_VER}/configure \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --disable-nls \
        --enable-languages=c \
        --without-headers
    make -j"$JOBS" all-gcc
    make -j"$JOBS" all-target-libgcc
    make install-gcc
    make install-target-libgcc
    cd "$SRC_DIR"
fi

# --- 5. Проверка ---
log "Готово. Проверка бинарников:"
"${TARGET}-gcc" --version | head -n1
"${TARGET}-ld" --version | head -n1

cat <<EOF

Кросс-компилятор собран в: $PREFIX/bin
Добавь в ~/.bashrc (или ~/.zshrc):

    export PATH="$PREFIX/bin:\$PATH"

Затем можно собирать os-tutorial:

    cd os-tutorial/23-fixes
    make run

EOF
