include(BuildVeyonPlugin)

build_veyon_plugin(chat
	NAME Chat
	SOURCES
	ChatPlugin.cpp ChatPlugin.h
	ChatConfiguration.h
	ChatConfigurationPage.cpp ChatConfigurationPage.h ChatConfigurationPage.ui
	ChatMasterWindow.cpp ChatMasterWindow.h ChatMasterWindow.ui
	ChatStudentWindow.cpp ChatStudentWindow.h ChatStudentWindow.ui
	chat.qrc)
