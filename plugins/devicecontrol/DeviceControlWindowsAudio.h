/*
 * DeviceControlWindowsAudio.h - muting the default audio endpoint on Windows
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Isolé dans son propre fichier : les en-têtes COM audio de Windows sont
 * volumineux et définissent des GUID, ce qui n'a pas sa place dans le corps du
 * plugin.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

namespace DeviceControlWindowsAudio
{

/** Coupe ou rétablit le périphérique de sortie audio par défaut. */
bool setDefaultOutputMuted( bool muted );

}
