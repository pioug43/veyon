/*
 * CourseRestrictionsProfileTest.cpp - tests for CourseRestrictionsProfile
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Les listes d'applications et de sites sont saisies à la main par les
 * enseignants : la tolérance des séparateurs et la stabilité de l'aller-retour
 * avec la configuration sont ce qui évite les profils silencieusement vides.
 */

#include <QJsonObject>
#include <QTest>

#include "CourseRestrictionsProfile.h"

class CourseRestrictionsProfileTest : public QObject
{
	Q_OBJECT
private slots:
	void acceptsTheSeparatorsTeachersActuallyType();
	void dropsDuplicatesAndBlanks();
	void survivesAJsonRoundTrip();
	void normalizesTheWebsiteMode();
	void keepsTheProvidedUidAndGeneratesOneOtherwise();
	void rejectsAProfileWithoutName();
};


void CourseRestrictionsProfileTest::acceptsTheSeparatorsTeachersActuallyType()
{
	// point-virgule, virgule, espace, retour à la ligne : tous acceptés, parce
	// qu'aucune consigne ne survit à une liste tapée à la main
	const auto values = CourseRestrictionsProfile::splitList(
		QStringLiteral("steam.exe; discord.exe,minecraft.exe\n  spotify.exe  ") );

	QCOMPARE( values, ( QStringList{ QStringLiteral("steam.exe"), QStringLiteral("discord.exe"),
									 QStringLiteral("minecraft.exe"), QStringLiteral("spotify.exe") } ) );
}



void CourseRestrictionsProfileTest::dropsDuplicatesAndBlanks()
{
	const auto values = CourseRestrictionsProfile::splitList(
		QStringLiteral(";; steam.exe ;; steam.exe ;  ; discord.exe ;") );

	QCOMPARE( values, ( QStringList{ QStringLiteral("steam.exe"), QStringLiteral("discord.exe") } ) );
	QVERIFY( CourseRestrictionsProfile::splitList( QStringLiteral("   ") ).isEmpty() );
	QVERIFY( CourseRestrictionsProfile::splitList( {} ).isEmpty() );
}



/**
 * Un profil relu depuis la configuration doit être identique à celui qui y a
 * été écrit : sans quoi rouvrir la page de configuration viderait des listes.
 */
void CourseRestrictionsProfileTest::survivesAJsonRoundTrip()
{
	const auto original = CourseRestrictionsProfile::fromValues(
		QStringLiteral("Internet coupé"),
		QStringLiteral("steam.exe; discord.exe"),
		QStringLiteral("example.com, example.net"),
		QStringLiteral("block") );

	const CourseRestrictionsProfile reloaded{ original.toJson() };

	QCOMPARE( reloaded.uid(), original.uid() );
	QCOMPARE( reloaded.name(), QStringLiteral("Internet coupé") );
	QCOMPARE( reloaded.blockedApplications(), original.blockedApplications() );
	QCOMPARE( reloaded.websites(), original.websites() );
	QCOMPARE( reloaded.websiteMode(), QStringLiteral("block") );
	QVERIFY( reloaded.isValid() );
}



void CourseRestrictionsProfileTest::normalizesTheWebsiteMode()
{
	// « allow » est la seule alternative ; tout le reste retombe sur « block »,
	// le mode le moins surprenant s'il y a doute sur l'intention
	const auto allow = CourseRestrictionsProfile::fromValues(
		QStringLiteral("Liste blanche"), {}, QStringLiteral("example.org"), QStringLiteral("allow") );
	QCOMPARE( allow.websiteMode(), QStringLiteral("allow") );

	for( const auto& mode : { "block", "BLOCK", "n'importe quoi", "" } )
	{
		const auto profile = CourseRestrictionsProfile::fromValues(
			QStringLiteral("P"), {}, {}, QString::fromLatin1( mode ) );
		QCOMPARE( profile.websiteMode(), QStringLiteral("block") );
	}

	// et un objet JSON dont le mode est absent ou aberrant se relit en « block »
	const CourseRestrictionsProfile fromJson{ QJsonObject{
		{ QStringLiteral("Uid"), QStringLiteral("{2a1e6f80-51d3-4c7b-9a08-3f5b2c9d7e14}") },
		{ QStringLiteral("Name"), QStringLiteral("P") },
		{ QStringLiteral("WebsiteMode"), QStringLiteral("autre") },
	} };
	QCOMPARE( fromJson.websiteMode(), QStringLiteral("block") );
}



void CourseRestrictionsProfileTest::keepsTheProvidedUidAndGeneratesOneOtherwise()
{
	const auto generated = CourseRestrictionsProfile::fromValues( QStringLiteral("P"), {}, {}, {} );
	QVERIFY( generated.uid().isNull() == false );

	// modifier un profil existant ne doit pas changer son identifiant, sinon la
	// sous-fonction correspondante de la console changerait d'identité
	const auto kept = CourseRestrictionsProfile::fromValues(
		QStringLiteral("P"), {}, {}, {}, generated.uid() );
	QCOMPARE( kept.uid(), generated.uid() );
}



void CourseRestrictionsProfileTest::rejectsAProfileWithoutName()
{
	// un profil sans nom n'est pas affichable dans le menu : il est écarté
	const auto unnamed = CourseRestrictionsProfile::fromValues( QStringLiteral("   "), {}, {}, {} );
	QVERIFY( unnamed.isValid() == false );

	const CourseRestrictionsProfile empty{ QJsonObject{} };
	QVERIFY( empty.isValid() == false );
}


QTEST_GUILESS_MAIN( CourseRestrictionsProfileTest )
#include "CourseRestrictionsProfileTest.moc"
