include(BuildVeyonPlugin)

build_veyon_plugin(raisehand
	NAME RaiseHand
	SOURCES
	RaiseHandPlugin.cpp RaiseHandPlugin.h
	RaiseHandMasterWindow.cpp RaiseHandMasterWindow.h
	RaiseHandStudentWidget.cpp RaiseHandStudentWidget.h)
