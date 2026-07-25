/*
 * QuizQuestionsTest.cpp - tests for the quiz question sanitizer
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * La garantie centrale du plugin — le corrigé ne descend jamais sur le poste
 * de l'élève — est vérifiée ici. Elle repose sur une reconstruction depuis une
 * liste blanche de champs, et non sur un filtrage : ces tests le prouvent en
 * soumettant des noms de champ qui n'existaient pas quand le code a été écrit.
 */

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include "QuizQuestions.h"

class QuizQuestionsTest : public QObject
{
	Q_OBJECT
private slots:
	void keepsOnlyWhitelistedFields();
	void dropsAnyAnswerKeyWhateverItIsCalled();
	void rejectsUnusableQuestions();
	void normalizesTypeAndAssignsMissingIds();
	void honoursTheQuestionLimit();
	void keepsFreeTextQuestionsWithoutOptions();

private:
	static QJsonObject choiceQuestion();
};


QJsonObject QuizQuestionsTest::choiceQuestion()
{
	return QJsonObject{
		{ QStringLiteral("id"), QStringLiteral("q1") },
		{ QStringLiteral("type"), QStringLiteral("single") },
		{ QStringLiteral("text"), QStringLiteral("Capitale de la France ?") },
		{ QStringLiteral("options"), QJsonArray{ QStringLiteral("Paris"), QStringLiteral("Lyon") } },
	};
}



void QuizQuestionsTest::keepsOnlyWhitelistedFields()
{
	const auto sanitized = QuizQuestions::sanitize( QJsonArray{ choiceQuestion() }, 100 );

	QCOMPARE( sanitized.count(), 1 );

	const auto question = sanitized.at( 0 ).toObject();
	auto keys = question.keys();
	keys.sort();

	QCOMPARE( keys, ( QStringList{ QStringLiteral("id"), QStringLiteral("options"),
								   QStringLiteral("text"), QStringLiteral("type") } ) );
	QCOMPARE( question.value( QStringLiteral("text") ).toString(),
			  QStringLiteral("Capitale de la France ?") );
	QCOMPARE( question.value( QStringLiteral("options") ).toArray().count(), 2 );
}



/**
 * Le point crucial : peu importe le nom donné au corrigé par l'appelant, il ne
 * doit pas survivre. Un filtrage par liste noire laisserait passer le premier
 * nom auquel personne n'a pensé.
 */
void QuizQuestionsTest::dropsAnyAnswerKeyWhateverItIsCalled()
{
	auto question = choiceQuestion();
	question.insert( QStringLiteral("correct"), QJsonArray{ QStringLiteral("Paris") } );
	question.insert( QStringLiteral("correctAnswer"), QStringLiteral("Paris") );
	question.insert( QStringLiteral("solution"), QStringLiteral("Paris") );
	question.insert( QStringLiteral("answer"), QStringLiteral("Paris") );
	question.insert( QStringLiteral("points"), 3 );
	question.insert( QStringLiteral("bareme"), 3 );
	question.insert( QStringLiteral("teacherNote"), QStringLiteral("piège classique") );

	const auto sanitized = QuizQuestions::sanitize( QJsonArray{ question }, 100 );
	const auto result = sanitized.at( 0 ).toObject();

	for( const auto& forbidden : { "correct", "correctAnswer", "solution", "answer",
								   "points", "bareme", "teacherNote" } )
	{
		QVERIFY2( result.contains( QLatin1String(forbidden) ) == false, forbidden );
	}

	// et le corrigé ne doit pas non plus se retrouver ailleurs par accident
	const auto serialized = QString::fromUtf8( QJsonDocument( sanitized ).toJson( QJsonDocument::Compact ) );
	QVERIFY( serialized.contains( QStringLiteral("piège classique") ) == false );
}



void QuizQuestionsTest::rejectsUnusableQuestions()
{
	auto emptyText = choiceQuestion();
	emptyText.insert( QStringLiteral("text"), QStringLiteral("   ") );

	auto singleOption = choiceQuestion();
	singleOption.insert( QStringLiteral("options"), QJsonArray{ QStringLiteral("Paris") } );

	auto blankOptions = choiceQuestion();
	blankOptions.insert( QStringLiteral("options"),
						 QJsonArray{ QStringLiteral(" "), QStringLiteral("") } );

	const QJsonArray input{
		QJsonValue{ 42 },			// pas un objet
		emptyText,					// énoncé vide
		singleOption,				// une seule proposition : pas un choix
		blankOptions,				// propositions vides une fois élaguées
		choiceQuestion(),			// la seule exploitable
	};

	QCOMPARE( QuizQuestions::sanitize( input, 100 ).count(), 1 );
}



void QuizQuestionsTest::normalizesTypeAndAssignsMissingIds()
{
	auto unknownType = choiceQuestion();
	unknownType.insert( QStringLiteral("type"), QStringLiteral("carte-a-gratter") );
	unknownType.remove( QStringLiteral("id") );

	const auto sanitized = QuizQuestions::sanitize( QJsonArray{ unknownType }, 100 );
	const auto question = sanitized.at( 0 ).toObject();

	QCOMPARE( question.value( QStringLiteral("type") ).toString(), QStringLiteral("single") );
	// identifiant de position, pour que les réponses restent rattachables
	QCOMPARE( question.value( QStringLiteral("id") ).toString(), QStringLiteral("1") );
}



void QuizQuestionsTest::honoursTheQuestionLimit()
{
	QJsonArray input;
	for( int index = 0; index < 10; ++index )
	{
		input.append( choiceQuestion() );
	}

	QCOMPARE( QuizQuestions::sanitize( input, 3 ).count(), 3 );
	QCOMPARE( QuizQuestions::sanitize( input, 0 ).count(), 0 );
}



void QuizQuestionsTest::keepsFreeTextQuestionsWithoutOptions()
{
	QJsonObject freeText{
		{ QStringLiteral("id"), QStringLiteral("q7") },
		{ QStringLiteral("type"), QStringLiteral("text") },
		{ QStringLiteral("text"), QStringLiteral("Citez un langage compilé.") },
		{ QStringLiteral("options"), QJsonArray{} },
	};

	const auto sanitized = QuizQuestions::sanitize( QJsonArray{ freeText }, 100 );

	QCOMPARE( sanitized.count(), 1 );
	QCOMPARE( sanitized.at( 0 ).toObject().value( QStringLiteral("type") ).toString(),
			  QStringLiteral("text") );
}


QTEST_GUILESS_MAIN( QuizQuestionsTest )
#include "QuizQuestionsTest.moc"
