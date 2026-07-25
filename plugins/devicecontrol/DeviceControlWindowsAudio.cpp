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

// INITGUID définit les GUID COM dans cette unité de compilation : cela évite
// de dépendre d'une bibliothèque d'import pour CLSID_MMDeviceEnumerator et
// consorts, ce que toutes les chaînes de compilation Windows ne fournissent
// pas de la même façon.
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

namespace DeviceControlWindowsAudio
{

bool setDefaultOutputMuted( bool muted )
{
	// Le thread appelant peut déjà avoir initialisé COM (Qt le fait) : dans ce
	// cas on ne doit surtout pas le désinitialiser en sortant.
	const auto initResult = CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED );
	const bool weInitialized = ( initResult == S_OK );
	if( initResult != S_OK && initResult != S_FALSE && initResult != RPC_E_CHANGED_MODE )
	{
		return false;
	}

	bool success = false;

	IMMDeviceEnumerator* enumerator = nullptr;
	if( CoCreateInstance( CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
						  IID_IMMDeviceEnumerator, reinterpret_cast<void **>( &enumerator ) ) == S_OK &&
		enumerator != nullptr )
	{
		IMMDevice* device = nullptr;
		if( enumerator->GetDefaultAudioEndpoint( eRender, eConsole, &device ) == S_OK && device != nullptr )
		{
			IAudioEndpointVolume* endpointVolume = nullptr;
			if( device->Activate( IID_IAudioEndpointVolume, CLSCTX_ALL, nullptr,
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
