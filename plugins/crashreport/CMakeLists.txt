include(BuildVeyonPlugin)

build_veyon_plugin(crashreport
	NAME CrashReport
	SOURCES
	CrashReportPlugin.cpp
	CrashReportPlugin.h
	CrashHandler.cpp
	CrashHandler.h
	)

# Commit exact du build, joint à chaque rapport : permet de symboliser les traces
# (lib+offset) avec les binaires correspondants et de regrouper par build.
set(VEYON_CRASHREPORT_BUILD_COMMIT "unknown")
if(GIT_FOUND)
	execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
		WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
		OUTPUT_STRIP_TRAILING_WHITESPACE
		OUTPUT_VARIABLE _crashreport_commit
		RESULT_VARIABLE _crashreport_commit_result
		ERROR_QUIET)
	if(_crashreport_commit_result EQUAL 0 AND _crashreport_commit)
		set(VEYON_CRASHREPORT_BUILD_COMMIT "${_crashreport_commit}")
	endif()
endif()
target_compile_definitions(crashreport PRIVATE VEYON_CRASHREPORT_BUILD_COMMIT="${VEYON_CRASHREPORT_BUILD_COMMIT}")

# Téléversement des rapports vers le portail (QNetworkAccessManager).
target_link_libraries(crashreport PRIVATE Qt${QT_MAJOR_VERSION}::Network)

# Windows : minidump via DbgHelp (MiniDumpWriteDump). MinGW/MXE fournit la lib
# d'import dbghelp.
if(WIN32)
	target_link_libraries(crashreport PRIVATE dbghelp)
endif()
