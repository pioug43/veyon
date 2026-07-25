/*
 * RegistryPolicyGuard.h - transactional Windows registry policy helper
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Applique un jeu de valeurs de registre en mémorisant d'abord l'état
 * antérieur dans un fichier d'état, ce qui permet de le restaurer même après
 * un arrêt brutal. C'est le même schéma que celui du mode examen (IFEO,
 * politiques navigateur, stockage USB), factorisé ici parce que plusieurs
 * fonctionnalités posent désormais des politiques de la même façon.
 *
 * Sans effet hors Windows : apply() renvoie false et remove() ne fait rien.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QList>
#include <QString>

class RegistryPolicyGuard
{
public:
	struct Policy
	{
		QString key;	// ex. HKLM\SOFTWARE\Policies\...
		QString name;
		QString type;	// REG_DWORD | REG_SZ | REG_EXPAND_SZ
		QString data;
	};

	/**
	 * @param id identifiant du jeu de politiques ; il nomme le fichier d'état,
	 *           donc deux jeux distincts ne peuvent pas se nettoyer l'un l'autre
	 * @param logPrefix préfixe des messages de journal
	 */
	RegistryPolicyGuard( const QString& id, const QString& logPrefix );

	/** Applique les politiques après avoir sauvegardé l'état antérieur. */
	bool apply( const QList<Policy>& policies );

	/** Restaure l'état antérieur. Idempotent, sûr même sans état présent. */
	void remove();

	/** Un état est-il posé (y compris hérité d'un arrêt brutal) ? */
	bool isApplied() const;

	QString stateFile() const;

private:
	const QString m_id;
	const QString m_logPrefix;
};
