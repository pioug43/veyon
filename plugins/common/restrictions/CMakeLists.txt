include(SetDefaultTargetProperties)

# Backends de restriction partagés entre le plugin exammode (profil d'examen
# signé, à bail) et le plugin courserestrictions (restrictions de cours, sans
# bail ni signature). Bibliothèque statique : elle est éditée de liens dans
# chaque plugin, donc compilée en PIC.
add_library(veyon-restrictions STATIC
	ExamModeProfile.cpp
	ExamModeProfile.h
	ExamModeWindowsNative.cpp
	ExamModeWindowsNative.h
	RegistryPolicyGuard.cpp
	RegistryPolicyGuard.h
	RestrictionEnforcer.cpp
	RestrictionEnforcer.h
	RestrictionRules.h
	)

target_include_directories(veyon-restrictions PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(veyon-restrictions PUBLIC veyon-core)

set_default_target_properties(veyon-restrictions)
set_property(TARGET veyon-restrictions PROPERTY POSITION_INDEPENDENT_CODE ON)

if(WIN32)
	# advapi32 : registre (IFEO, politiques navigateur) et ACL des fichiers d'état.
	target_link_libraries(veyon-restrictions PUBLIC advapi32)
endif()
