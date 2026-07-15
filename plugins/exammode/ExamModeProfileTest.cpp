#include <QtTest>

#include "ExamModeProfile.h"

class ExamModeProfileTest : public QObject
{
	Q_OBJECT

private slots:
	void normalizesDomains()
	{
		QStringList rejected;
		const auto domains = ExamModeProfile::normalizeDomains(
			{ QStringLiteral(" HTTPS://Exemple.FR/ "), QStringLiteral("*.sub.example.org"),
			  QStringLiteral("example.org/path"), QStringLiteral("bad host") }, &rejected );

		QCOMPARE( domains, QStringList( { QStringLiteral("exemple.fr"), QStringLiteral("sub.example.org") } ) );
		QCOMPARE( rejected.size(), 2 );
	}

	void normalizesApplicationsDeterministically()
	{
		const auto first = ExamModeProfile::normalizeApplications(
			{ QStringLiteral("App.EXE"), QStringLiteral("app.exe"), QStringLiteral(" beta "), QStringLiteral("") } );
		const auto second = ExamModeProfile::normalizeApplications(
			{ QStringLiteral("beta"), QStringLiteral("app.exe"), QStringLiteral("App.EXE") } );

		QCOMPARE( first.size(), 2 );
		// même ensemble d'entrées (à la casse près) => même liste normalisée => digest stable
		QCOMPARE( first.first().toLower(), QStringLiteral("app.exe") );
		QCOMPARE( first.last().toLower(), QStringLiteral("beta") );
		QCOMPARE( first.size(), second.size() );
	}

	void rejectsUnsafeDomainSyntax()
	{
		QStringList rejected;
		const auto domains = ExamModeProfile::normalizeDomains(
			{ QStringLiteral("user@example.org"), QStringLiteral("example.org:8080"),
			  QStringLiteral("example.org?q=x"), QStringLiteral("localhost") }, &rejected );

		QVERIFY( domains.isEmpty() );
		QCOMPARE( rejected.size(), 4 );
	}

	void buildsAllowPac()
	{
		const auto pac = ExamModeProfile::buildPac( { QStringLiteral("example.org") },
			ExamModeProfile::SiteMode::Allow );
		QVERIFY( pac.contains( "dnsDomainIs(host, '.' + d)" ) );
		QVERIFY( pac.contains( "return 'DIRECT'" ) );
		QVERIFY( pac.contains( "return 'PROXY 127.0.0.1:1'" ) );
	}

	void resolvesPlatformProcessRulesAndAllowsExceptions()
	{
		const QVariantList rules{
			QVariantMap{ { QStringLiteral("executable"), QStringLiteral("PowerShell.exe") },
				{ QStringLiteral("os"), QStringLiteral("windows") }, { QStringLiteral("action"), QStringLiteral("block") } },
			QVariantMap{ { QStringLiteral("executable"), QStringLiteral("calc.exe") },
				{ QStringLiteral("os"), QStringLiteral("linux") }, { QStringLiteral("action"), QStringLiteral("block") } },
			QVariantMap{ { QStringLiteral("executable"), QStringLiteral("powershell.exe") },
				{ QStringLiteral("os"), QStringLiteral("windows") }, { QStringLiteral("action"), QStringLiteral("allow") } },
			QVariantMap{ { QStringLiteral("executable"), QStringLiteral("cmd.exe") },
				{ QStringLiteral("os"), QStringLiteral("windows") }, { QStringLiteral("action"), QStringLiteral("block") },
				{ QStringLiteral("strongKill"), false }, { QStringLiteral("preventLaunch"), true } },
		};
		const auto policy = ExamModeProfile::resolveProcessRules( rules, QStringLiteral("windows") );
		QVERIFY( policy.terminateApplications.isEmpty() );
		QCOMPARE( policy.preventLaunchApplications, QStringList{ QStringLiteral("cmd.exe") } );
	}

	void validatesOrderedUrlRulesAndBuildsPac()
	{
		QStringList rejected;
		const QVariantList input{
			QVariantMap{ { QStringLiteral("expression"), QStringLiteral("*.exam.example/*") },
				{ QStringLiteral("action"), QStringLiteral("allow") } },
			QVariantMap{ { QStringLiteral("expression"), QStringLiteral("^https://chat\\.") },
				{ QStringLiteral("regex"), true }, { QStringLiteral("action"), QStringLiteral("block") } },
			QVariantMap{ { QStringLiteral("expression"), QStringLiteral("[invalid") },
				{ QStringLiteral("regex"), true }, { QStringLiteral("action"), QStringLiteral("allow") } },
		};
		const auto rules = ExamModeProfile::normalizeUrlRules( input, &rejected );
		QCOMPARE( rules.size(), 2 );
		QCOMPARE( rejected.size(), 1 );
		QCOMPARE( rules.first().action, ExamModeProfile::RuleAction::Allow );
		const auto pac = ExamModeProfile::buildPac( rules, ExamModeProfile::RuleAction::Block );
		QVERIFY( pac.indexOf( "*.exam.example/*" ) < pac.indexOf( "^https://chat" ) );
		QVERIFY( pac.contains( "new RegExp(r.expression)" ) );
		QVERIFY( pac.contains( "return 'PROXY 127.0.0.1:1';" ) );
	}

	void computesStableVersionedProfileDigest()
	{
		const ExamModeProfile::ProcessPolicy processPolicy{
			{ QStringLiteral("chat.exe") }, { QStringLiteral("chat.exe") },
		};
		const QList<ExamModeProfile::UrlRule> rules{
			{ ExamModeProfile::RuleAction::Allow, QStringLiteral("exam.example"), false },
		};
		const auto first = ExamModeProfile::profileDigest( QStringLiteral("math-2026"), 4, processPolicy,
			rules, ExamModeProfile::RuleAction::Block, 300 );
		const auto again = ExamModeProfile::profileDigest( QStringLiteral("math-2026"), 4, processPolicy,
			rules, ExamModeProfile::RuleAction::Block, 300 );
		const auto revised = ExamModeProfile::profileDigest( QStringLiteral("math-2026"), 5, processPolicy,
			rules, ExamModeProfile::RuleAction::Block, 300 );
		QCOMPARE( first.size(), 64 );
		QCOMPARE( first, again );
		QVERIFY( first != revised );
	}

	void validatesSiteMode()
	{
		bool valid = false;
		QCOMPARE( ExamModeProfile::siteModeFromString( QStringLiteral("ALLOW"), &valid ),
			ExamModeProfile::SiteMode::Allow );
		QVERIFY( valid );
		QCOMPARE( ExamModeProfile::siteModeFromString( QStringLiteral("unexpected"), &valid ),
			ExamModeProfile::SiteMode::Block );
		QVERIFY( valid == false );
	}

	void validatesNetworkBackend()
	{
		bool valid = false;
		QCOMPARE( ExamModeProfile::networkBackendFromString( QStringLiteral("FireWall"), &valid ),
			ExamModeProfile::NetworkBackend::Firewall );
		QVERIFY( valid );
		QCOMPARE( ExamModeProfile::networkBackendFromString( QString(), &valid ),
			ExamModeProfile::NetworkBackend::Hosts );
		QVERIFY( valid );
		QCOMPARE( ExamModeProfile::networkBackendFromString( QStringLiteral("bogus"), &valid ),
			ExamModeProfile::NetworkBackend::Hosts );
		QVERIFY( valid == false );
	}

	void strictModeCoversInterpretersPerPlatform()
	{
		const auto windows = ExamModeProfile::strictModeApplications( QStringLiteral("windows") );
		QVERIFY( windows.contains( QStringLiteral("powershell.exe") ) );
		QVERIFY( windows.contains( QStringLiteral("cmd.exe") ) );
		QVERIFY( windows.contains( QStringLiteral("taskmgr.exe") ) );		// anti-arrêt d'ExamMode
		const auto linuxApps = ExamModeProfile::strictModeApplications( QStringLiteral("linux") );
		QVERIFY( linuxApps.contains( QStringLiteral("bash") ) );
		QVERIFY( linuxApps.contains( QStringLiteral("python3") ) );
		QVERIFY( linuxApps.contains( QStringLiteral("gnome-terminal") ) );
		// les interpréteurs strict deviennent des règles de blocage effectives
		QVariantList rules;
		for( const auto& app : linuxApps )
		{
			rules.append( QVariantMap{ { QStringLiteral("executable"), app },
				{ QStringLiteral("action"), QStringLiteral("block") } } );
		}
		const auto policy = ExamModeProfile::resolveProcessRules( rules, QStringLiteral("linux") );
		QVERIFY( policy.preventLaunchApplications.contains( QStringLiteral("bash") ) );
	}

	void normalizesAndDeduplicatesNetworks()
	{
		QStringList rejected;
		const auto networks = ExamModeProfile::normalizeNetworks(
			{ QStringLiteral(" 10.0.0.5/24 "), QStringLiteral("10.0.0.99/24"),		// même sous-réseau => dédup
			  QStringLiteral("192.168.1.10"), QStringLiteral("2001:db8::1/64"),
			  QStringLiteral("not-an-ip"), QStringLiteral("10.0.0.0/40") }, &rejected );

		QVERIFY( networks.contains( QStringLiteral("10.0.0.0/24") ) );
		QVERIFY( networks.contains( QStringLiteral("192.168.1.10/32") ) );		// IP nue => /32
		QCOMPARE( networks.count( QStringLiteral("10.0.0.0/24") ), 1 );
		QCOMPARE( rejected.size(), 2 );		// "not-an-ip" + préfixe /40 invalide
	}

	void buildsFailClosedNftablesRuleset()
	{
		const auto ruleset = QString::fromUtf8( ExamModeProfile::buildNftablesRuleset(
			{ QStringLiteral("10.0.0.0/24") }, { QStringLiteral("10.0.0.53") },
			{ QStringLiteral("192.168.50.0/24") } ) );

		QVERIFY( ruleset.contains( QStringLiteral("policy drop") ) );			// fail-closed
		QVERIFY( ruleset.contains( QStringLiteral("oif \"lo\" accept") ) );		// loopback
		QVERIFY( ruleset.contains( QStringLiteral("ct state established,related accept") ) );
		QVERIFY( ruleset.contains( QStringLiteral("ip daddr 10.0.0.0/24 accept") ) );
		QVERIFY( ruleset.contains( QStringLiteral("ip daddr 192.168.50.0/24 accept") ) );	// supervision
		QVERIFY( ruleset.contains( QStringLiteral("udp dport 53 accept") ) );	// DNS résolveur autorisé
		QVERIFY( ruleset.contains( QStringLiteral("delete table inet veyon_exammode") ) );	// idempotent
	}

	void enforcesRuleCountCaps()
	{
		QVariantList many;
		for( int i = 0; i < ExamModeProfile::MaxUrlRules + 50; ++i )
		{
			many.append( QVariantMap{ { QStringLiteral("expression"), QStringLiteral("d%1.example").arg( i ) },
				{ QStringLiteral("action"), QStringLiteral("block") } } );
		}
		QStringList rejected;
		const auto rules = ExamModeProfile::normalizeUrlRules( many, &rejected );
		QVERIFY( rules.size() <= ExamModeProfile::MaxUrlRules );
		QVERIFY( rejected.isEmpty() == false );
	}
};

QTEST_GUILESS_MAIN( ExamModeProfileTest )
#include "ExamModeProfileTest.moc"
