/*
 * RegistryPolicyGuardTest.cpp - tests for RegistryPolicyGuard
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * L'invariant vérifié ici est celui qui empêche deux consommateurs (mode
 * examen, restrictions de cours, contrôles matériels) de se nettoyer
 * mutuellement : chacun doit posséder son propre fichier d'état. Une collision
 * de nom ferait qu'une levée de restrictions restaurerait le registre d'un
 * autre — un examen pourrait ainsi se retrouver désarmé sans que personne ne
 * l'ait demandé.
 *
 * Les écritures de registre elles-mêmes ne sont pas testables ici : elles sont
 * propres à Windows et modifieraient la machine de compilation.
 */

#include <QTest>

#include "RegistryPolicyGuard.h"

class RegistryPolicyGuardTest : public QObject
{
	Q_OBJECT
private slots:
	void distinctScopesNeverShareAStateFile();
	void theStateFileCarriesTheScopeIdentifier();
	void anUnusedScopeIsNotConsideredApplied();
};


void RegistryPolicyGuardTest::distinctScopesNeverShareAStateFile()
{
	const RegistryPolicyGuard usb{ QStringLiteral("devicecontrol-usb"), QStringLiteral("DeviceControl") };
	const RegistryPolicyGuard webcam{ QStringLiteral("devicecontrol-webcam"), QStringLiteral("DeviceControl") };
	const RegistryPolicyGuard exam{ QStringLiteral("exammode-usb"), QStringLiteral("ExamMode") };

	QVERIFY( usb.stateFile() != webcam.stateFile() );
	QVERIFY( usb.stateFile() != exam.stateFile() );
	QVERIFY( webcam.stateFile() != exam.stateFile() );

	// Le préfixe de journalisation ne doit PAS entrer dans le nom du fichier :
	// deux consommateurs peuvent légitimement partager le même préfixe.
	const RegistryPolicyGuard sameLogPrefix{ QStringLiteral("devicecontrol-usb"),
											 QStringLiteral("AutreChose") };
	QCOMPARE( sameLogPrefix.stateFile(), usb.stateFile() );
}



void RegistryPolicyGuardTest::theStateFileCarriesTheScopeIdentifier()
{
	const RegistryPolicyGuard guard{ QStringLiteral("devicecontrol-usb"), QStringLiteral("DeviceControl") };

	QVERIFY( guard.stateFile().contains( QStringLiteral("devicecontrol-usb") ) );
	// un fichier d'état reste un fichier d'état : sous un répertoire dédié
	QVERIFY( guard.stateFile().contains( QStringLiteral("Veyon") ) ||
			 guard.stateFile().contains( QStringLiteral("veyon") ) );
}



void RegistryPolicyGuardTest::anUnusedScopeIsNotConsideredApplied()
{
	// identifiant volontairement improbable : aucun fichier ne peut exister
	RegistryPolicyGuard guard{ QStringLiteral("test-scope-jamais-utilise-6f21a"),
							   QStringLiteral("Test") };

	QVERIFY( guard.isApplied() == false );

	// remove() sur un état absent ne doit rien tenter ni rien casser
	guard.remove();
	QVERIFY( guard.isApplied() == false );
}


QTEST_GUILESS_MAIN( RegistryPolicyGuardTest )
#include "RegistryPolicyGuardTest.moc"
