include(BuildVeyonPlugin)

build_veyon_plugin(survey
	NAME Survey
	SOURCES
	SurveyPlugin.cpp SurveyPlugin.h
	SurveyMasterDialog.cpp SurveyMasterDialog.h SurveyMasterDialog.ui
	SurveyResultsWindow.cpp SurveyResultsWindow.h SurveyResultsWindow.ui
	SurveyStudentDialog.cpp SurveyStudentDialog.h SurveyStudentDialog.ui)
