include(BuildVeyonPlugin)
include(SetDefaultTargetProperties)

build_veyon_plugin(courserestrictions
	NAME CourseRestrictions
	SOURCES
	CourseRestrictionsPlugin.cpp CourseRestrictionsPlugin.h
	CourseRestrictionsConfiguration.h
	CourseRestrictionsConfigurationPage.cpp CourseRestrictionsConfigurationPage.h CourseRestrictionsConfigurationPage.ui
	CourseRestrictionsProfile.cpp CourseRestrictionsProfile.h
	)

# backends d'application des restrictions, partagés avec le plugin exammode
target_link_libraries(courserestrictions PRIVATE veyon-restrictions)

if(WITH_TESTS)
	# Les listes de profils sont saisies à la main : leur analyse est testée à
	# part du plugin, qui exigerait sinon tout l'environnement Veyon.
	add_executable(CourseRestrictionsProfileTest CourseRestrictionsProfileTest.cpp CourseRestrictionsProfile.cpp)
	target_link_libraries(CourseRestrictionsProfileTest PRIVATE Qt${QT_MAJOR_VERSION}::Test Qt${QT_MAJOR_VERSION}::Core)
	set_test_target_properties(CourseRestrictionsProfileTest)
	add_test(NAME CourseRestrictionsProfileTest COMMAND CourseRestrictionsProfileTest)
endif()
