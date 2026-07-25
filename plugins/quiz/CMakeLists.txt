include(BuildVeyonPlugin)

build_veyon_plugin(quiz
	NAME Quiz
	SOURCES
	QuizPlugin.cpp QuizPlugin.h
	QuizStudentDialog.cpp QuizStudentDialog.h)
