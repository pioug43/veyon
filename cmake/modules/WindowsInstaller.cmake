set(WINDOWS_INSTALL_FILES "veyon-${VEYON_WINDOWS_ARCH}-${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}.${VERSION_BUILD}")

# Sous MXE (qt-cmake), la toolchain n'est pas Win64Toolchain.cmake, donc
# MINGW_PREFIX/MINGW_TOOL_PREFIX ne sont pas définis. On les dérive du sysroot
# (CMAKE_FIND_ROOT_PATH) et du compilateur pour que le packaging fonctionne aussi
# via MXE, sans -D manuel (CI comprise).
if(NOT MINGW_PREFIX AND CMAKE_FIND_ROOT_PATH)
	list(GET CMAKE_FIND_ROOT_PATH 0 MINGW_PREFIX)
endif()
if(NOT MINGW_TOOL_PREFIX)
	# MXE range strip/objcopy dans usr/bin (sur le PATH), mais le compilateur dans
	# usr/<host-tuple>/bin — dossiers différents. On utilise donc le préfixe NU
	# (ex. « x86_64-w64-mingw32.shared- ») résolu via le PATH, pas le dossier du gcc.
	get_filename_component(_cc_name "${CMAKE_C_COMPILER}" NAME)
	string(REGEX REPLACE "gcc(\\.exe)?$" "" MINGW_TOOL_PREFIX "${_cc_name}")
endif()

set(DLLDIR "${MINGW_PREFIX}/bin")
set(DLLDIR_LIB "${MINGW_PREFIX}/lib")
string(REGEX MATCH "^[^.]+" GCC_VERSION_MAJOR ${CMAKE_CXX_COMPILER_VERSION})

# Détection du layout de la toolchain : MXE place Qt6 sous qt6/, le runtime gcc et
# zlib/pthread dans bin/ ; le cross-mingw « classique » place Qt6 dans bin/ et le
# runtime gcc dans /usr/lib/gcc/... On s'adapte aux deux.
if(EXISTS "${MINGW_PREFIX}/qt6/bin/Qt6Core.dll")
	set(QT_DLLDIR "${MINGW_PREFIX}/qt6/bin")
	set(QT_PLUGINDIR "${MINGW_PREFIX}/qt6/plugins")
else()
	set(QT_DLLDIR "${DLLDIR}")
	set(QT_PLUGINDIR "${MINGW_PREFIX}/plugins")
endif()
if(EXISTS "${DLLDIR}/libstdc++-6.dll")
	set(DLLDIR_GCC "${DLLDIR}")
else()
	set(DLLDIR_GCC "/usr/lib/gcc/${MINGW_TARGET}/${GCC_VERSION_MAJOR}-posix")
endif()
if(EXISTS "${DLLDIR}/zlib1.dll")
	set(DLLDIR_RUNTIME "${DLLDIR}")
else()
	set(DLLDIR_RUNTIME "${DLLDIR_LIB}")
endif()
if(VEYON_BUILD_WIN64)
	set(DLL_GCC "libgcc_s_seh-1.dll")
	set(DLL_DDENGINE "ddengine64.dll")
else()
	set(DLL_GCC "libgcc_s_dw2-1.dll")
	set(DLL_DDENGINE "ddengine.dll")
endif()

# DLL LDAP/SASL copiées seulement si le plugin LDAP est construit (WITH_LDAP).
set(WINDOWS_LDAP_DLL_COMMANDS)
if(WITH_LDAP)
	set(WINDOWS_LDAP_DLL_COMMANDS
		COMMAND cp ${DLLDIR}/libsasl2-3.dll ${WINDOWS_INSTALL_FILES}
		COMMAND cp ${DLLDIR}/libldap.dll ${DLLDIR}/liblber.dll ${WINDOWS_INSTALL_FILES})
endif()

add_custom_target(windows-binaries
	COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIGURATION>
	COMMAND rm -rf ${WINDOWS_INSTALL_FILES}*
	COMMAND mkdir -p ${WINDOWS_INSTALL_FILES}/interception
	COMMAND cp ${CMAKE_SOURCE_DIR}/3rdparty/interception/* ${WINDOWS_INSTALL_FILES}/interception
	COMMAND cp ${CMAKE_SOURCE_DIR}/3rdparty/ddengine/${DLL_DDENGINE} ${WINDOWS_INSTALL_FILES}
	COMMAND cp core/veyon-core.dll ${WINDOWS_INSTALL_FILES}
	COMMAND find . -mindepth 2 -name 'veyon-*.exe' -exec cp '{}' ${WINDOWS_INSTALL_FILES}/ '\;'
	COMMAND mkdir -p ${WINDOWS_INSTALL_FILES}/plugins
	COMMAND find plugins/ -name '*.dll' -exec cp '{}' ${WINDOWS_INSTALL_FILES}/plugins/ '\;'
	# find -exec mv : tolérant au no-match (les plugins Veyon ne sont pas préfixés
	# « lib » et vnchooks.dll peut être absent) — contrairement à « mv lib*.dll » qui
	# casse la chaîne && sur glob vide. Même style que les find -exec cp ci-dessus.
	COMMAND find ${WINDOWS_INSTALL_FILES}/plugins -maxdepth 1 -name 'lib*.dll' -exec mv '{}' ${WINDOWS_INSTALL_FILES}/ '\;'
	COMMAND find ${WINDOWS_INSTALL_FILES}/plugins -maxdepth 1 -name 'vnchooks.dll' -exec mv '{}' ${WINDOWS_INSTALL_FILES}/ '\;'
	COMMAND mkdir -p ${WINDOWS_INSTALL_FILES}/translations
	COMMAND cp translations/*qm ${WINDOWS_INSTALL_FILES}/translations/
	COMMAND cp ${DLLDIR}/libjpeg-*.dll ${WINDOWS_INSTALL_FILES}
	COMMAND cp ${DLLDIR}/libpng16-16.dll ${WINDOWS_INSTALL_FILES}
	COMMAND cp ${DLLDIR}/libcrypto-3*.dll ${DLLDIR}/libssl-3*.dll ${WINDOWS_INSTALL_FILES}
	COMMAND cp ${DLLDIR}/libqca-qt6.dll ${WINDOWS_INSTALL_FILES}
	${WINDOWS_LDAP_DLL_COMMANDS}
	COMMAND cp ${DLLDIR}/interception.dll ${WINDOWS_INSTALL_FILES}
	COMMAND cp ${DLLDIR}/liblzo2-2.dll ${WINDOWS_INSTALL_FILES}
	# libvnc bundlé (WITH_BUNDLED_LIBVNC) : les DLL sont dans l'arbre de build, pas
	# dans le sysroot. find tolère l'absence (build système = déjà dans le sysroot).
	COMMAND find ${CMAKE_BINARY_DIR} -name 'libvncclient.dll' -exec cp '{}' ${WINDOWS_INSTALL_FILES}/ '\;'
	COMMAND find ${CMAKE_BINARY_DIR} -name 'libvncserver.dll' -exec cp '{}' ${WINDOWS_INSTALL_FILES}/ '\;'
	COMMAND cp ${DLLDIR_RUNTIME}/zlib1.dll ${WINDOWS_INSTALL_FILES}
	COMMAND cp ${DLLDIR_RUNTIME}/libwinpthread-1.dll ${WINDOWS_INSTALL_FILES}
	COMMAND cp ${DLLDIR_GCC}/libstdc++-6.dll ${WINDOWS_INSTALL_FILES}
	COMMAND cp ${DLLDIR_GCC}/libssp-0.dll ${WINDOWS_INSTALL_FILES}
	COMMAND cp ${DLLDIR_GCC}/${DLL_GCC} ${WINDOWS_INSTALL_FILES}
	COMMAND mkdir -p ${WINDOWS_INSTALL_FILES}/crypto
	COMMAND cp ${DLLDIR_LIB}/qca-qt6/crypto/libqca-ossl.dll ${WINDOWS_INSTALL_FILES}/crypto
	COMMAND cp ${QT_DLLDIR}/Qt6Core.dll
				${QT_DLLDIR}/Qt6Core5Compat.dll
				${QT_DLLDIR}/Qt6Gui.dll
				${QT_DLLDIR}/Qt6Widgets.dll
				${QT_DLLDIR}/Qt6Network.dll
				${QT_DLLDIR}/Qt6Concurrent.dll
				${QT_DLLDIR}/Qt6HttpServer.dll
				${QT_DLLDIR}/Qt6WebSockets.dll
			${WINDOWS_INSTALL_FILES}
	# Dépendances transitives de Qt6 (fermeture objdump) — sans elles l'application
	# ne démarre pas (ex. libbrotlidec, ICU, pcre2, harfbuzz, freetype…).
	COMMAND cp ${DLLDIR}/libbrotlidec.dll ${DLLDIR}/libbrotlicommon.dll
				${DLLDIR}/libbz2.dll
				${DLLDIR}/libfreetype-6.dll
				${DLLDIR}/libglib-2.0-0.dll
				${DLLDIR}/libharfbuzz-0.dll
				${DLLDIR}/libiconv-2.dll
				${DLLDIR}/libintl-8.dll
				${DLLDIR}/libpcre2-16-0.dll
				${DLLDIR}/libpcre2-8-0.dll
				${DLLDIR}/libzstd.dll
			${WINDOWS_INSTALL_FILES}
	COMMAND cp ${DLLDIR}/icudt*.dll ${DLLDIR}/icuin*.dll ${DLLDIR}/icuuc*.dll ${WINDOWS_INSTALL_FILES}
	COMMAND mkdir -p ${WINDOWS_INSTALL_FILES}/imageformats
	COMMAND cp ${QT_PLUGINDIR}/imageformats/qjpeg.dll ${WINDOWS_INSTALL_FILES}/imageformats
	COMMAND mkdir -p ${WINDOWS_INSTALL_FILES}/platforms
	COMMAND cp ${QT_PLUGINDIR}/platforms/qwindows.dll ${WINDOWS_INSTALL_FILES}/platforms
	COMMAND mkdir -p ${WINDOWS_INSTALL_FILES}/styles
	COMMAND cp ${QT_PLUGINDIR}/styles/*.dll ${WINDOWS_INSTALL_FILES}/styles
	COMMAND mkdir -p ${WINDOWS_INSTALL_FILES}/tls
	COMMAND cp ${QT_PLUGINDIR}/tls/qopensslbackend.dll ${WINDOWS_INSTALL_FILES}/tls
	COMMAND ${MINGW_TOOL_PREFIX}strip ${WINDOWS_INSTALL_FILES}/*.dll ${WINDOWS_INSTALL_FILES}/*.exe ${WINDOWS_INSTALL_FILES}/plugins/*.dll ${WINDOWS_INSTALL_FILES}/platforms/*.dll ${WINDOWS_INSTALL_FILES}/styles/*.dll ${WINDOWS_INSTALL_FILES}/crypto/*.dll
	COMMAND cp ${CMAKE_SOURCE_DIR}/COPYING ${WINDOWS_INSTALL_FILES}
	COMMAND cp ${CMAKE_SOURCE_DIR}/COPYING ${WINDOWS_INSTALL_FILES}/LICENSE.TXT
	COMMAND cp ${CMAKE_SOURCE_DIR}/README.md ${WINDOWS_INSTALL_FILES}/README.TXT
	COMMAND todos ${WINDOWS_INSTALL_FILES}/*.TXT
	COMMAND cp -ra ${CMAKE_SOURCE_DIR}/nsis ${WINDOWS_INSTALL_FILES}
	COMMAND cp ${CMAKE_BINARY_DIR}/nsis/veyon.nsi ${WINDOWS_INSTALL_FILES}
	COMMAND find ${WINDOWS_INSTALL_FILES} -ls
)

add_custom_target(create-windows-installer
	COMMAND makensis ${WINDOWS_INSTALL_FILES}/veyon.nsi
	COMMAND mv ${WINDOWS_INSTALL_FILES}/veyon-*setup.exe .
	COMMAND rm -rf ${WINDOWS_INSTALL_FILES}
	DEPENDS windows-binaries
)

add_custom_target(prepare-dev-nsi
	COMMAND sed -i ${WINDOWS_INSTALL_FILES}/veyon.nsi -e "s,/SOLID lzma,zlib,g"
	DEPENDS windows-binaries)

add_custom_target(dev-nsi
	DEPENDS prepare-dev-nsi create-windows-installer
)

