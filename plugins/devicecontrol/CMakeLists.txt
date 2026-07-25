include(BuildVeyonPlugin)

build_veyon_plugin(devicecontrol
	NAME DeviceControl
	SOURCES
	DeviceControlPlugin.cpp DeviceControlPlugin.h
	DeviceControlWindowsAudio.cpp DeviceControlWindowsAudio.h)

# INITGUID ne définit les GUID COM que si guiddef.h n'a pas déjà été inclus.
# En build unifié, DeviceControlPlugin.cpp tire windows.h avant ce fichier :
# les GUID ne seraient alors que déclarés, et l'édition de liens échouerait sur
# CLSID_MMDeviceEnumerator & consorts. Même remède que LinuxCoreFunctions.cpp.
set_source_files_properties(DeviceControlWindowsAudio.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE)

# RegistryPolicyGuard : transactions de politiques registre partagées
target_link_libraries(devicecontrol PRIVATE veyon-restrictions)

if(WIN32)
	# ole32 : COM (CoreAudio, coupure du son du périphérique par défaut)
	target_link_libraries(devicecontrol PRIVATE ole32)
endif()
