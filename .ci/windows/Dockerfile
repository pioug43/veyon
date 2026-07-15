# Image CI Windows pour Veyon
# ----------------------------------------
# Reproduit une toolchain de cross-compilation MinGW-w64 + Qt6 + QCA au layout
# attendu par .ci/windows/build.sh :
#   - préfixe  /usr/x86_64-w64-mingw32
#   - wrapper  /usr/x86_64-w64-mingw32/bin/qt-cmake
#   - DLLs     libqca-qt6, libcrypto-3*, libssl-3*, libjpeg-62, libpng16-16,
#              libsasl2-3, libldap, liblber, liblzo2-2 dans .../bin
#
# ATTENTION : build TRÈS long (MXE compile toute la chaîne, ~1-3 h) et à valider
# de façon itérative — la cross-compilation de QCA pour Qt6 est le point délicat.
# Construite/poussée par .github/workflows/windows-image.yml, PAS à chaque CI.
FROM debian:12

ARG MXE_TARGET=x86_64-w64-mingw32.shared
ENV DEBIAN_FRONTEND=noninteractive
ENV PATH="/opt/mxe/usr/bin:${PATH}"

# Dépendances de build MXE (jeu de requirements Debian officiel) + NSIS
# (installeur) + tofrodos (todos, utilisé par WindowsInstaller.cmake).
RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf automake autopoint bash bison bzip2 flex g++ g++-multilib \
        gettext git gperf intltool libc6-dev-i386 \
        libgdk-pixbuf2.0-dev libgdk-pixbuf2.0-bin libltdl-dev libgl-dev \
        libpcre3-dev libssl-dev libtool libtool-bin libxml-parser-perl lzip make \
        ninja-build openssl p7zip-full patch perl python3 python3-mako \
        python3-packaging python-is-python3 ruby sed sqlite3 unzip wget xz-utils \
        nsis tofrodos ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# MXE : cross-toolchain + Qt6 + deps requis par Veyon (voir find_package dans
# CMakeLists.txt : PNG, JPEG, LZO, OpenSSL, OpenLDAP/SASL). Build long.
RUN git clone --depth 1 https://github.com/mxe/mxe.git /opt/mxe
WORKDIR /opt/mxe
# NB : MXE ne fournit pas de paquet « openldap » → le build Windows du fork se
# fait SANS LDAP (postes = auth keyfile ; cf. WITH_LDAP=OFF côté build Veyon).
RUN make MXE_TARGETS="${MXE_TARGET}" JOBS="$(nproc)" \
        cc cmake ninja \
        qt6-qtbase qt6-qttools qt6-qtsvg qt6-qtwebsockets \
        openssl libjpeg-turbo libpng lzo

# QCA (git master) exige Qt6::Core5Compat depuis 2024 (QRegExp/QTextCodec déplacés
# hors de qtbase en Qt6). On construit le module qt5compat dans une couche SÉPARÉE
# pour ne pas invalider le cache (long) de la couche qtbase ci-dessus.
RUN make MXE_TARGETS="${MXE_TARGET}" JOBS="$(nproc)" qt6-qt5compat

# Module Qt HttpServer (Qt6::HttpServer) requis par le plugin webapi (le portail
# pilote ExamMode via WebAPI). Déps déjà bâties (qtbase/qtwebsockets) → build court.
# Couche séparée pour préserver le cache des couches précédentes.
RUN make MXE_TARGETS="${MXE_TARGET}" JOBS="$(nproc)" qt6-qthttpserver

# Layout attendu par build.sh (/usr/x86_64-w64-mingw32 + qt-cmake wrapper)
RUN ln -sfn /opt/mxe/usr/${MXE_TARGET} /usr/x86_64-w64-mingw32 \
 && printf '#!/bin/sh\nexec /opt/mxe/usr/bin/%s-cmake "$@"\n' "${MXE_TARGET}" \
        > /usr/x86_64-w64-mingw32/bin/qt-cmake \
 && chmod +x /usr/x86_64-w64-mingw32/bin/qt-cmake

# QCA pour Qt6 : non fourni par MXE → cross-build depuis les sources contre la
# Qt6/OpenSSL de MXE. Produit libqca-qt6.dll attendu par l'installeur.
RUN git clone --depth 1 https://invent.kde.org/libraries/qca.git /tmp/qca \
 && /usr/x86_64-w64-mingw32/bin/qt-cmake -S /tmp/qca -B /tmp/qca/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/mxe/usr/${MXE_TARGET} \
        -DQT6=ON -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF \
 && ninja -C /tmp/qca/build \
 && ninja -C /tmp/qca/build install \
 && rm -rf /tmp/qca

WORKDIR /
