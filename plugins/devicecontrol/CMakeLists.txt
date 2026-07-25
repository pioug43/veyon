include(BuildVeyonPlugin)

build_veyon_plugin(devicecontrol
	NAME DeviceControl
	SOURCES
	DeviceControlPlugin.cpp DeviceControlPlugin.h
	DeviceControlWindowsAudio.cpp DeviceControlWindowsAudio.h)

# INITGUID ne définit les GUID COM que si guiddef.h n'a pas déjà été inclus.
# Ce fichier inclut windows.h et les en-têtes COM : on le garde hors du build
# unifié pour que leurs macros ne débordent pas sur le reste du plugin.
# Même remède que LinuxCoreFunctions.cpp.
set_source_files_properties(DeviceControlWindowsAudio.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE)

# RegistryPolicyGuard : transactions de politiques registre partagées
target_link_libraries(devicecontrol PRIVATE veyon-restrictions)

if(WIN32)
	# ole32 : COM (CoreAudio, coupure du son du périphérique par défaut)
	target_link_libraries(devicecontrol PRIVATE ole32)
endif()
