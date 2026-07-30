include(BuildVeyonPlugin)
include(SetDefaultTargetProperties)

build_veyon_plugin(exammode
	NAME ExamMode
	SOURCES
	ExamModeFeaturePlugin.cpp
	ExamModeFeaturePlugin.h
	ExamModeLinuxExecGuard.cpp
	ExamModeLinuxExecGuard.h
	ExamModeSession.cpp
	ExamModeSession.h
	ExamModeVdiClient.cpp
	ExamModeVdiClient.h
	)

# ExamModeProfile (modèle de règles, PAC, nftables) et ExamModeWindowsNative
# (registre, terminaison de processus) vivent désormais dans la bibliothèque
# partagée veyon-restrictions : ils sont utilisés à l'identique par le plugin
# courserestrictions. Les fichiers ont été déplacés SANS modification de code.
target_link_libraries(exammode PRIVATE veyon-restrictions)

if(WIN32)
	# user32 : énumération de fenêtres + moniteurs ; wtsapi32 : message dans la
	# session interactive (WTSSendMessage). Ensemble : contrôle/forçage plein
	# écran de l'application cliente surveillée + avertissement. (advapi32 est
	# fourni par veyon-restrictions.)
	target_link_libraries(exammode PRIVATE user32 wtsapi32)
endif()

# Contrôle/forçage plein écran du client VDI sous Linux : X11/EWMH. Optionnel —
# sans X11 (build headless), le module retombe sur un no-op (Unknown/false).
if(UNIX AND NOT APPLE)
	find_package(X11)
	if(X11_FOUND)
		target_compile_definitions(exammode PRIVATE EXAMMODE_HAVE_X11)
		target_include_directories(exammode PRIVATE ${X11_INCLUDE_DIR})
		target_link_libraries(exammode PRIVATE ${X11_LIBRARIES})
		# XRandR (optionnel) : énumération des écrans de la session distante pour
		# la détection plein écran en invité VDI Linux (comptage d'écrans fiable ;
		# sans lui, repli sur la seule boîte englobante de l'écran X).
		if(X11_Xrandr_FOUND)
			target_compile_definitions(exammode PRIVATE EXAMMODE_HAVE_XRANDR)
			target_link_libraries(exammode PRIVATE ${X11_Xrandr_LIB})
		endif()
	endif()
endif()

if(WITH_TESTS)
	add_executable(ExamModeProfileTest ExamModeProfileTest.cpp)
	target_link_libraries(ExamModeProfileTest PRIVATE veyon-restrictions Qt${QT_MAJOR_VERSION}::Test Qt${QT_MAJOR_VERSION}::Core Qt${QT_MAJOR_VERSION}::Network)
	set_test_target_properties(ExamModeProfileTest)
	add_test(NAME ExamModeProfileTest COMMAND ExamModeProfileTest)

	add_executable(ExamModeSessionTest ExamModeSessionTest.cpp ExamModeSession.cpp)
	target_link_libraries(ExamModeSessionTest PRIVATE Qt${QT_MAJOR_VERSION}::Test Qt${QT_MAJOR_VERSION}::Core)
	set_test_target_properties(ExamModeSessionTest)
	add_test(NAME ExamModeSessionTest COMMAND ExamModeSessionTest)
endif()
