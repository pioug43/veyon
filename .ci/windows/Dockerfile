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

# Dépendances de build MXE + NSIS (installeur) + outils utilisés par le CMake Veyon
RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf automake autopoint bash bison bzip2 flex g++ gettext git gperf \
        intltool libtool libtool-bin libssl-dev libxml-parser-perl lzip make \
        ninja-build openssl p7zip-full patch perl python3 python3-mako \
        python3-packaging ruby sed unzip wget xz-utils nsis tofrodos ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# MXE : cross-toolchain + Qt6 + deps requis par Veyon (voir find_package dans
# CMakeLists.txt : PNG, JPEG, LZO, OpenSSL, OpenLDAP/SASL). Build long.
RUN git clone --depth 1 https://github.com/mxe/mxe.git /opt/mxe
WORKDIR /opt/mxe
RUN make MXE_TARGETS="${MXE_TARGET}" JOBS="$(nproc)" \
        cc cmake ninja \
        qt6-qtbase qt6-qttools qt6-qtsvg qt6-qtwebsockets \
        openssl libjpeg-turbo libpng lzo openldap

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
        -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DQCA_PLUGIN_TYPE=STATIC \
        -DWITH_ossl_PLUGIN=ON \
 && ninja -C /tmp/qca/build \
 && ninja -C /tmp/qca/build install \
 && rm -rf /tmp/qca

WORKDIR /
