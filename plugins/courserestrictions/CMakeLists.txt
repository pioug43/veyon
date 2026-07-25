include(BuildVeyonPlugin)

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
