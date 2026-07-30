include(BuildVeyonPlugin)
include(SetDefaultTargetProperties)

build_veyon_plugin(quiz
	NAME Quiz
	SOURCES
	QuizPlugin.cpp QuizPlugin.h
	QuizQuestions.cpp QuizQuestions.h
	QuizStudentDialog.cpp QuizStudentDialog.h)

if(WITH_TESTS)
	# La normalisation des questions porte la garantie « le corrigé ne descend
	# jamais sur le poste » : elle est testée à part du plugin, qui exigerait
	# sinon tout l'environnement Veyon.
	add_executable(QuizQuestionsTest QuizQuestionsTest.cpp QuizQuestions.cpp)
	target_link_libraries(QuizQuestionsTest PRIVATE Qt${QT_MAJOR_VERSION}::Test Qt${QT_MAJOR_VERSION}::Core)
	set_test_target_properties(QuizQuestionsTest)
	add_test(NAME QuizQuestionsTest COMMAND QuizQuestionsTest)
endif()
