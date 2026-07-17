include(BuildVeyonPlugin)

build_veyon_plugin(crashreport
	NAME CrashReport
	SOURCES
	CrashReportPlugin.cpp
	CrashReportPlugin.h
	CrashHandler.cpp
	CrashHandler.h
	)

# Téléversement des rapports vers le portail (QNetworkAccessManager).
target_link_libraries(crashreport PRIVATE Qt${QT_MAJOR_VERSION}::Network)

# Windows : minidump via DbgHelp (MiniDumpWriteDump). MinGW/MXE fournit la lib
# d'import dbghelp.
if(WIN32)
	target_link_libraries(crashreport PRIVATE dbghelp)
endif()
