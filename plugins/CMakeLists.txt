file(GLOB plugins RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/*)

foreach(plugin ${plugins})
	if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${plugin})
		# le plugin LDAP exige OpenLDAP (find_package(Ldap REQUIRED)) : on peut le
		# désactiver (WITH_LDAP=OFF) pour les cibles sans OpenLDAP (build Windows/MXE).
		if(plugin STREQUAL "ldap" AND NOT WITH_LDAP)
			message(STATUS "Skipping LDAP plugin (WITH_LDAP=OFF)")
		else()
			add_subdirectory(${plugin})
		endif()
	endif()
endforeach()
