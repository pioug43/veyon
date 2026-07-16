include(BuildVeyonPlugin)

build_veyon_plugin(exammode
	NAME ExamMode
	SOURCES
	ExamModeFeaturePlugin.cpp
	ExamModeFeaturePlugin.h
	ExamModeLinuxExecGuard.cpp
	ExamModeLinuxExecGuard.h
	ExamModeProfile.cpp
	ExamModeProfile.h
	ExamModeSession.cpp
	ExamModeSession.h
	ExamModeVdiClient.cpp
	ExamModeVdiClient.h
	ExamModeWindowsNative.cpp
	ExamModeWindowsNative.h
	)

if(WIN32)
	# advapi32 : registre/ACL ; user32 : énumération de fenêtres + moniteurs ;
	# wtsapi32 : message dans la session interactive (WTSSendMessage). Ensemble :
	# contrôle/forçage plein écran du client VDI Omnissa Horizon + avertissement.
	target_link_libraries(exammode PRIVATE advapi32 user32 wtsapi32)
endif()

# Contrôle/forçage plein écran du client VDI sous Linux : X11/EWMH. Optionnel —
# sans X11 (build headless), le module retombe sur un no-op (Unknown/false).
if(UNIX AND NOT APPLE)
	find_package(X11)
	if(X11_FOUND)
		target_compile_definitions(exammode PRIVATE EXAMMODE_HAVE_X11)
		target_include_directories(exammode PRIVATE ${X11_INCLUDE_DIR})
		target_link_libraries(exammode PRIVATE ${X11_LIBRARIES})
	endif()
endif()

if(WITH_TESTS)
	add_executable(ExamModeProfileTest ExamModeProfileTest.cpp ExamModeProfile.cpp)
	target_link_libraries(ExamModeProfileTest PRIVATE Qt${QT_MAJOR_VERSION}::Test Qt${QT_MAJOR_VERSION}::Core Qt${QT_MAJOR_VERSION}::Network)
	add_test(NAME ExamModeProfileTest COMMAND ExamModeProfileTest)

	add_executable(ExamModeSessionTest ExamModeSessionTest.cpp ExamModeSession.cpp)
	target_link_libraries(ExamModeSessionTest PRIVATE Qt${QT_MAJOR_VERSION}::Test Qt${QT_MAJOR_VERSION}::Core)
	add_test(NAME ExamModeSessionTest COMMAND ExamModeSessionTest)
endif()
