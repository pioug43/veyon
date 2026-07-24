include(BuildVeyonPlugin)

build_veyon_plugin(survey
	NAME Survey
	SOURCES
	SurveyPlugin.cpp SurveyPlugin.h
	SurveyStudentDialog.cpp SurveyStudentDialog.h SurveyStudentDialog.ui)
