/*
 * ExamModeWindowsNative.h - shell-free Windows enforcement primitives
 *
 * Copyright (c) 2026 Pierrick Belledent
 */

#pragma once

#include <QJsonObject>
#include <QStringList>

namespace ExamModeWindowsNative
{

QJsonObject readRegistryValue( const QString& key, const QString& name );
bool setRegistryValue( const QString& key, const QString& name, const QString& type, const QString& data );
bool restoreRegistryValue( const QString& key, const QString& name, const QJsonObject& value );
bool deleteRegistryValue( const QString& key, const QString& name );

struct EnforcementResult
{
	QStringList detected;
	QStringList terminated;
	QStringList failed;
};

EnforcementResult terminateProcesses( const QStringList& imageNames );
bool restrictFileToAdministratorsAndSystem( const QString& path );

}
