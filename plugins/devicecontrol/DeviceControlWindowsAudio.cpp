/*
 * DeviceControlWindowsAudio.cpp - muting the default audio endpoint on Windows
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "DeviceControlWindowsAudio.h"

#if defined(_WIN32)

#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

namespace
{

// Les GUID de l'API MMDevice sont écrits ici plutôt que repris des en-têtes :
// le compilateur croisé MinGW utilisé pour l'installeur ne les fournit dans
// aucune bibliothèque d'import, et la recette habituelle (INITGUID avant
// windows.h) est inopérante en compilation unifiée, où un autre fichier a déjà
// inclus windows.h. Ces valeurs sont fixées par Microsoft et ne changent pas.
constexpr GUID ClsidMMDeviceEnumerator =
	{ 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
constexpr GUID IidIMMDeviceEnumerator =
	{ 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
constexpr GUID IidIAudioEndpointVolume =
	{ 0x5CDF2C82, 0x841E, 0x4546, { 0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A } };

}

namespace DeviceControlWindowsAudio
{

bool setDefaultOutputMuted( bool muted )
{
	// Le thread appelant peut déjà avoir initialisé COM (Qt le fait) : dans ce
	// cas on ne doit surtout pas le désinitialiser en sortant.
	// S_FALSE = COM était déjà initialisé sur ce thread, mais le compteur a tout
	// de même été incrémenté : il faut équilibrer par un CoUninitialize().
	// Seul RPC_E_CHANGED_MODE n'incrémente rien.
	const auto initResult = CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED );
	const bool weInitialized = ( initResult == S_OK || initResult == S_FALSE );
	if( initResult != S_OK && initResult != S_FALSE && initResult != RPC_E_CHANGED_MODE )
	{
		return false;
	}

	bool success = false;

	IMMDeviceEnumerator* enumerator = nullptr;
	if( CoCreateInstance( ClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
						  IidIMMDeviceEnumerator, reinterpret_cast<void **>( &enumerator ) ) == S_OK &&
		enumerator != nullptr )
	{
		IMMDevice* device = nullptr;
		if( enumerator->GetDefaultAudioEndpoint( eRender, eConsole, &device ) == S_OK && device != nullptr )
		{
			IAudioEndpointVolume* endpointVolume = nullptr;
			if( device->Activate( IidIAudioEndpointVolume, CLSCTX_ALL, nullptr,
								  reinterpret_cast<void **>( &endpointVolume ) ) == S_OK &&
				endpointVolume != nullptr )
			{
				success = endpointVolume->SetMute( muted ? TRUE : FALSE, nullptr ) == S_OK;
				endpointVolume->Release();
			}

			device->Release();
		}

		enumerator->Release();
	}

	if( weInitialized )
	{
		CoUninitialize();
	}

	return success;
}

}

#else

namespace DeviceControlWindowsAudio
{

bool setDefaultOutputMuted( bool muted )
{
	(void) muted;

	return false;
}

}

#endif
