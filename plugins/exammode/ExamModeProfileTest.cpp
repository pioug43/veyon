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
};

QTEST_GUILESS_MAIN( ExamModeProfileTest )
#include "ExamModeProfileTest.moc"
