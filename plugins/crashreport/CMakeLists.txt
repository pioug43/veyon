include(BuildVeyonPlugin)

build_veyon_plugin(crashreport
	NAME CrashReport
	SOURCES
	CrashReportPlugin.cpp
	CrashReportPlugin.h
	CrashHandler.cpp
	CrashHandler.h
	)

# Windows : minidump via DbgHelp (MiniDumpWriteDump). MinGW/MXE fournit la lib
# d'import dbghelp.
if(WIN32)
	target_link_libraries(crashreport PRIVATE dbghelp)
endif()
