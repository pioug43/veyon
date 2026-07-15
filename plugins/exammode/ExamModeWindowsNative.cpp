/*
 * ExamModeWindowsNative.cpp - shell-free Windows enforcement primitives
 *
 * Copyright (c) 2026 Pierrick Belledent
 */

#include "ExamModeWindowsNative.h"

#include <QByteArray>
#include <QSet>

#include <cstring>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#endif

namespace ExamModeWindowsNative
{

#if defined(Q_OS_WIN)
namespace
{

struct RegistryKey
{
	HKEY root{nullptr};
	QString subKey;
};


RegistryKey parseRegistryKey( const QString& key )
{
	const auto separator = key.indexOf( QLatin1Char('\\') );
	const auto rootName = ( separator < 0 ? key : key.left( separator ) ).toUpper();
	RegistryKey result;
	if( rootName == QStringLiteral("HKLM") || rootName == QStringLiteral("HKEY_LOCAL_MACHINE") )
	{
		result.root = HKEY_LOCAL_MACHINE;
	}
	else if( rootName == QStringLiteral("HKCU") || rootName == QStringLiteral("HKEY_CURRENT_USER") )
	{
		result.root = HKEY_CURRENT_USER;
	}
	result.subKey = separator < 0 ? QString{} : key.mid( separator + 1 );
	return result;
}


QString typeName( DWORD type )
{
	switch( type )
	{
	case REG_SZ: return QStringLiteral("REG_SZ");
	case REG_EXPAND_SZ: return QStringLiteral("REG_EXPAND_SZ");
	case REG_DWORD: return QStringLiteral("REG_DWORD");
	case REG_QWORD: return QStringLiteral("REG_QWORD");
	case REG_MULTI_SZ: return QStringLiteral("REG_MULTI_SZ");
	case REG_BINARY: return QStringLiteral("REG_BINARY");
	default: return QStringLiteral("REG_NONE");
	}
}


QString displayData( DWORD type, const QByteArray& raw )
{
	if( type == REG_DWORD && raw.size() >= static_cast<int>( sizeof( DWORD ) ) )
	{
		DWORD value = 0;
		memcpy( &value, raw.constData(), sizeof( value ) );
		return QString::number( value );
	}
	if( type == REG_QWORD && raw.size() >= static_cast<int>( sizeof( quint64 ) ) )
	{
		quint64 value = 0;
		memcpy( &value, raw.constData(), sizeof( value ) );
		return QString::number( value );
	}
	if( type == REG_SZ || type == REG_EXPAND_SZ )
	{
		const auto* data = reinterpret_cast<const wchar_t*>( raw.constData() );
		const auto characters = raw.size() / static_cast<int>( sizeof( wchar_t ) );
		auto value = QString::fromWCharArray( data, characters );
		while( value.endsWith( QChar::Null ) )
		{
			value.chop( 1 );
		}
		return value;
	}
	return QString::fromLatin1( raw.toBase64() );
}


bool writeRawValue( const QString& key, const QString& name, DWORD type, const QByteArray& raw )
{
	const auto parsed = parseRegistryKey( key );
	if( parsed.root == nullptr || parsed.subKey.isEmpty() || name.isEmpty() )
	{
		return false;
	}
	HKEY handle = nullptr;
	DWORD disposition = 0;
	const auto openResult = RegCreateKeyExW( parsed.root,
		reinterpret_cast<LPCWSTR>( parsed.subKey.utf16() ), 0, nullptr, 0,
		KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &handle, &disposition );
	Q_UNUSED( disposition )
	if( openResult != ERROR_SUCCESS )
	{
		return false;
	}
	const auto result = RegSetValueExW( handle, reinterpret_cast<LPCWSTR>( name.utf16() ), 0, type,
		reinterpret_cast<const BYTE*>( raw.constData() ), static_cast<DWORD>( raw.size() ) );
	RegCloseKey( handle );
	return result == ERROR_SUCCESS;
}

}
#endif


QJsonObject readRegistryValue( const QString& key, const QString& name )
{
#if defined(Q_OS_WIN)
	const auto parsed = parseRegistryKey( key );
	if( parsed.root == nullptr || parsed.subKey.isEmpty() || name.isEmpty() )
	{
		return { { QStringLiteral("ok"), false }, { QStringLiteral("exists"), false },
			{ QStringLiteral("error"), QStringLiteral("invalid registry path") } };
	}
	HKEY handle = nullptr;
	const auto openResult = RegOpenKeyExW( parsed.root, reinterpret_cast<LPCWSTR>( parsed.subKey.utf16() ),
		0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &handle );
	if( openResult == ERROR_FILE_NOT_FOUND )
	{
		return { { QStringLiteral("ok"), true }, { QStringLiteral("exists"), false } };
	}
	if( openResult != ERROR_SUCCESS )
	{
		return { { QStringLiteral("ok"), false }, { QStringLiteral("exists"), false },
			{ QStringLiteral("win32Error"), static_cast<qint64>( openResult ) } };
	}

	DWORD type = REG_NONE;
	DWORD size = 0;
	auto result = RegQueryValueExW( handle, reinterpret_cast<LPCWSTR>( name.utf16() ), nullptr, &type, nullptr, &size );
	if( result == ERROR_FILE_NOT_FOUND )
	{
		RegCloseKey( handle );
		return { { QStringLiteral("ok"), true }, { QStringLiteral("exists"), false } };
	}
	if( result != ERROR_SUCCESS )
	{
		RegCloseKey( handle );
		return { { QStringLiteral("ok"), false }, { QStringLiteral("exists"), false },
			{ QStringLiteral("win32Error"), static_cast<qint64>( result ) } };
	}
	QByteArray raw;
	raw.resize( static_cast<int>( size ) );
	result = RegQueryValueExW( handle, reinterpret_cast<LPCWSTR>( name.utf16() ), nullptr, &type,
		reinterpret_cast<BYTE*>( raw.data() ), &size );
	RegCloseKey( handle );
	if( result != ERROR_SUCCESS )
	{
		return { { QStringLiteral("ok"), false }, { QStringLiteral("exists"), false },
			{ QStringLiteral("win32Error"), static_cast<qint64>( result ) } };
	}
	raw.resize( static_cast<int>( size ) );
	return {
		{ QStringLiteral("ok"), true }, { QStringLiteral("exists"), true },
		{ QStringLiteral("nativeType"), static_cast<qint64>( type ) },
		{ QStringLiteral("type"), typeName( type ) },
		{ QStringLiteral("data"), displayData( type, raw ) },
		{ QStringLiteral("raw"), QString::fromLatin1( raw.toBase64() ) },
	};
#else
	Q_UNUSED( key )
	Q_UNUSED( name )
	return { { QStringLiteral("ok"), false }, { QStringLiteral("exists"), false },
		{ QStringLiteral("error"), QStringLiteral("unsupported platform") } };
#endif
}


bool setRegistryValue( const QString& key, const QString& name, const QString& type, const QString& data )
{
#if defined(Q_OS_WIN)
	if( type.compare( QStringLiteral("REG_DWORD"), Qt::CaseInsensitive ) == 0 )
	{
		bool ok = false;
		const auto value = data.toUInt( &ok, 0 );
		if( ok == false ) { return false; }
		QByteArray raw( reinterpret_cast<const char*>( &value ), sizeof( value ) );
		return writeRawValue( key, name, REG_DWORD, raw );
	}
	if( type.compare( QStringLiteral("REG_SZ"), Qt::CaseInsensitive ) == 0 ||
		type.compare( QStringLiteral("REG_EXPAND_SZ"), Qt::CaseInsensitive ) == 0 )
	{
		const auto nativeType = type.compare( QStringLiteral("REG_EXPAND_SZ"), Qt::CaseInsensitive ) == 0 ?
			REG_EXPAND_SZ : REG_SZ;
		const auto bytes = static_cast<int>( ( data.size() + 1 ) * sizeof( wchar_t ) );
		return writeRawValue( key, name, nativeType,
			QByteArray( reinterpret_cast<const char*>( data.utf16() ), bytes ) );
	}
	return false;
#else
	Q_UNUSED( key )
	Q_UNUSED( name )
	Q_UNUSED( type )
	Q_UNUSED( data )
	return false;
#endif
}


bool restoreRegistryValue( const QString& key, const QString& name, const QJsonObject& value )
{
#if defined(Q_OS_WIN)
	if( value.value( QStringLiteral("ok") ).toBool() == false )
	{
		return false;
	}
	if( value.value( QStringLiteral("exists") ).toBool() == false )
	{
		return deleteRegistryValue( key, name );
	}
	const auto type = static_cast<DWORD>( value.value( QStringLiteral("nativeType") ).toVariant().toUInt() );
	const auto raw = QByteArray::fromBase64( value.value( QStringLiteral("raw") ).toString().toLatin1() );
	return type != REG_NONE && writeRawValue( key, name, type, raw );
#else
	Q_UNUSED( key )
	Q_UNUSED( name )
	Q_UNUSED( value )
	return false;
#endif
}


bool deleteRegistryValue( const QString& key, const QString& name )
{
#if defined(Q_OS_WIN)
	const auto parsed = parseRegistryKey( key );
	if( parsed.root == nullptr || parsed.subKey.isEmpty() || name.isEmpty() )
	{
		return false;
	}
	HKEY handle = nullptr;
	const auto openResult = RegOpenKeyExW( parsed.root, reinterpret_cast<LPCWSTR>( parsed.subKey.utf16() ),
		0, KEY_SET_VALUE | KEY_WOW64_64KEY, &handle );
	if( openResult == ERROR_FILE_NOT_FOUND )
	{
		return true;
	}
	if( openResult != ERROR_SUCCESS )
	{
		return false;
	}
	const auto result = RegDeleteValueW( handle, reinterpret_cast<LPCWSTR>( name.utf16() ) );
	RegCloseKey( handle );
	return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
#else
	Q_UNUSED( key )
	Q_UNUSED( name )
	return false;
#endif
}


EnforcementResult terminateProcesses( const QStringList& imageNames )
{
	EnforcementResult result;
#if defined(Q_OS_WIN)
	QSet<QString> blocked;
	for( const auto& image : imageNames )
	{
		blocked.insert( image.toLower() );
	}
	if( blocked.isEmpty() )
	{
		return result;
	}
	const auto snapshot = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );
	if( snapshot == INVALID_HANDLE_VALUE )
	{
		result.failed.append( QStringLiteral("CreateToolhelp32Snapshot") );
		return result;
	}
	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof( entry );
	for( BOOL more = Process32FirstW( snapshot, &entry ); more; more = Process32NextW( snapshot, &entry ) )
	{
		const auto image = QString::fromWCharArray( entry.szExeFile );
		if( blocked.contains( image.toLower() ) == false )
		{
			continue;
		}
		result.detected.append( QStringLiteral("%1:%2").arg( image ).arg( entry.th32ProcessID ) );
		if( entry.th32ProcessID == GetCurrentProcessId() )
		{
			result.failed.append( result.detected.constLast() );
			continue;
		}
		const auto process = OpenProcess( PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID );
		if( process != nullptr && TerminateProcess( process, 1 ) )
		{
			result.terminated.append( result.detected.constLast() );
		}
		else
		{
			result.failed.append( result.detected.constLast() );
		}
		if( process != nullptr ) { CloseHandle( process ); }
	}
	CloseHandle( snapshot );
#else
	Q_UNUSED( imageNames )
#endif
	return result;
}


bool restrictFileToAdministratorsAndSystem( const QString& path )
{
#if defined(Q_OS_WIN)
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	if( ConvertStringSecurityDescriptorToSecurityDescriptorW(
		L"D:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr ) == FALSE )
	{
		return false;
	}
	BOOL present = FALSE;
	BOOL defaulted = FALSE;
	PACL dacl = nullptr;
	const bool descriptorValid = GetSecurityDescriptorDacl( descriptor, &present, &dacl, &defaulted ) != FALSE && present;
	const auto result = descriptorValid ? SetNamedSecurityInfoW(
		const_cast<LPWSTR>( reinterpret_cast<LPCWSTR>( path.utf16() ) ), SE_FILE_OBJECT,
		DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
		nullptr, nullptr, dacl, nullptr ) : ERROR_INVALID_SECURITY_DESCR;
	LocalFree( descriptor );
	return result == ERROR_SUCCESS;
#else
	Q_UNUSED( path )
	return true;
#endif
}

}
