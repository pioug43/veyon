include(BuildVeyonPlugin)

build_veyon_plugin(devicecontrol
	NAME DeviceControl
	SOURCES
	DeviceControlPlugin.cpp DeviceControlPlugin.h
	DeviceControlWindowsAudio.cpp DeviceControlWindowsAudio.h)

# RegistryPolicyGuard : transactions de politiques registre partagées
target_link_libraries(devicecontrol PRIVATE veyon-restrictions)

if(WIN32)
	# ole32 : COM (CoreAudio, coupure du son du périphérique par défaut)
	target_link_libraries(devicecontrol PRIVATE ole32)
endif()
