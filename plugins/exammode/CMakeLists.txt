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
	ExamModeWindowsNative.cpp
	ExamModeWindowsNative.h
	)

if(WIN32)
	target_link_libraries(exammode PRIVATE advapi32)
endif()

if(WITH_TESTS)
	add_executable(ExamModeProfileTest ExamModeProfileTest.cpp ExamModeProfile.cpp)
	target_link_libraries(ExamModeProfileTest PRIVATE Qt${QT_MAJOR_VERSION}::Test Qt${QT_MAJOR_VERSION}::Core Qt${QT_MAJOR_VERSION}::Network)
	add_test(NAME ExamModeProfileTest COMMAND ExamModeProfileTest)

	add_executable(ExamModeSessionTest ExamModeSessionTest.cpp ExamModeSession.cpp)
	target_link_libraries(ExamModeSessionTest PRIVATE Qt${QT_MAJOR_VERSION}::Test Qt${QT_MAJOR_VERSION}::Core)
	add_test(NAME ExamModeSessionTest COMMAND ExamModeSessionTest)
endif()
