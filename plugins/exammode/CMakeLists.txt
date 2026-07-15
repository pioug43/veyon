include(BuildVeyonPlugin)

build_veyon_plugin(exammode
	NAME ExamMode
	SOURCES
	ExamModeFeaturePlugin.cpp
	ExamModeFeaturePlugin.h
	ExamModeProfile.cpp
	ExamModeProfile.h
	)

if(WITH_TESTS)
	add_executable(ExamModeProfileTest ExamModeProfileTest.cpp ExamModeProfile.cpp)
	target_link_libraries(ExamModeProfileTest PRIVATE Qt${QT_MAJOR_VERSION}::Test Qt${QT_MAJOR_VERSION}::Core Qt${QT_MAJOR_VERSION}::Network)
	add_test(NAME ExamModeProfileTest COMMAND ExamModeProfileTest)
endif()
