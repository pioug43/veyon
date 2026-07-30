# Plugin CourseRestrictions — restrictions de cours

Blocage d'applications et de sites web **à la volée pendant un cours**, sans le
formalisme du mode examen.

## Positionnement par rapport au mode examen

| | `exammode` | `courserestrictions` |
|---|---|---|
| Usage | épreuve surveillée | cours ordinaire |
| Profil | signé (RSA-SHA512), digest, anti-rejeu | simple liste, aucune signature |
| Durée | bail (60–3600 s) ré-appliqué périodiquement, dead-man systemd | tant que l'enseignant ne lève pas |
| Réseau | hosts, PAC Windows, **nftables egress** (fail-closed) | hosts, PAC Windows |
| Prévention de lancement | IFEO Windows, **fanotify Linux** (noyau) | IFEO Windows uniquement |
| Blocage USB, forçage plein écran | oui | non |
| Priorité | **prioritaire** | s'efface devant le mode examen |

Les deux plugins partagent les mêmes backends (bibliothèque statique
`veyon-restrictions`, dans `plugins/common/restrictions/`) mais possèdent des
**marqueurs et fichiers d'état distincts** : ils ne peuvent jamais nettoyer
l'état l'un de l'autre.

**Arbitrage** : tant qu'un examen est actif sur le poste, une demande de
restrictions de cours est refusée (`REJECTED` / `EXAM_MODE_ACTIVE`) ; si un
examen démarre alors que des restrictions de cours sont en place, celles-ci sont
levées automatiquement dans les 5 secondes. Sans cet arbitrage, les deux plugins
se disputeraient les mêmes valeurs de registre (politiques proxy des
navigateurs), qui ne peuvent avoir qu'un seul propriétaire.

## Utilisation depuis la console (Veyon Master)

Les profils sont définis dans **Veyon Configurator → Restrictions de cours**
(nom, applications interdites, sites, case « liste blanche »). Chaque profil
apparaît ensuite dans le menu déroulant du bouton « Restrictions de cours » de
la console. Un nouveau clic sur le bouton (déjà actif) lève les restrictions.

Le poste de l'enseignant n'est jamais restreint (`removeLocalHostInterfaces`).

## Pilotage par la Web API

Aucun développement Web API n'est nécessaire : le plugin est piloté par les
routes génériques de fonctionnalité, comme n'importe quel outil d'administration
externe peut le faire.

1. Résoudre l'UUID **par son nom** (ne jamais le coder en dur) :

```http
GET /api/v1/feature
→ [ { "name": "CourseRestrictions", "uid": "9f2c4b81-6d35-4e7a-8c19-5b3ea0d7f264", ... } ]
```

2. Appliquer un profil — forme simple :

```http
PUT /api/v1/feature/9f2c4b81-6d35-4e7a-8c19-5b3ea0d7f264
{
  "active": true,
  "arguments": {
    "profileName": "Internet coupé",
    "blockedApplications": ["steam.exe", "discord.exe"],
    "websites": ["example.com", "example.net"],
    "websiteMode": "block"
  }
}
```

   …ou forme structurée (règles ordonnées, par système d'exploitation) :

```http
PUT /api/v1/feature/9f2c4b81-6d35-4e7a-8c19-5b3ea0d7f264
{
  "active": true,
  "arguments": {
    "profileName": "web-liste-blanche",
    "urlDefaultAction": "block",
    "urlRules": [ { "action": "allow", "expression": "*.example.org" } ],
    "processRules": [
      { "os": "windows", "executable": "steam.exe", "action": "block", "preventLaunch": true }
    ]
  }
}
```

Les deux formes se cumulent. `websiteMode: "allow"` équivaut à
`urlDefaultAction: "block"` + une règle `allow` par domaine.

3. Lever les restrictions :

```http
PUT /api/v1/feature/9f2c4b81-6d35-4e7a-8c19-5b3ea0d7f264
{ "active": false }
```

4. Suivre l'état par poste (interrogation périodique, ~5 s recommandé) :

```http
GET /api/v1/feature/9f2c4b81-6d35-4e7a-8c19-5b3ea0d7f264
→ {
    "active": true,
    "status": {
      "status": "APPLIED",
      "profileId": "…", "profileName": "Internet coupé",
      "errorCode": "", "errorMessage": "",
      "backendResults": { "process": "APPLIED", "launchPrevention": "APPLIED", "sites": "APPLIED" },
      "appliedAt": "2026-07-25T09:12:44Z"
    }
  }
```

### Valeurs de `status`

| Valeur | Signification |
|---|---|
| `APPLIED` | tous les backends demandés sont en place |
| `DEGRADED` | application partielle — voir `backendResults` |
| `REJECTED` | rien appliqué (`FEATURE_DISABLED`, `EXAM_MODE_ACTIVE`, `INVALID_URL_DEFAULT_ACTION`) |
| `IDLE` | aucune restriction active |
| `PENDING` | ordre envoyé, réponse du poste pas encore reçue |
| `UNKNOWN` | aucun état connu pour ce poste |

### Valeurs de `backendResults`

`APPLIED`, `IDLE` (rien à faire), `FAILED`, `UNSUPPORTED`.

`launchPrevention: UNSUPPORTED` est **normal sous Linux** : la prévention de
lancement au niveau noyau (fanotify) reste réservée au mode examen ; le mode
cours s'appuie sur la terminaison périodique (toutes les 1,5 s).

`sites: UNSUPPORTED` sous Linux/macOS signale une politique irréalisable via le
fichier hosts : liste blanche (`urlDefaultAction: "block"`) ou expression
régulière. Ces politiques exigent le backend PAC (Windows).

## Limites connues

- **Backend hosts (Linux/macOS)** : ne bloque que `domaine` et `www.domaine` ;
  les autres sous-domaines et l'accès par IP directe restent joignables. Pour
  une étanchéité réelle, utiliser le mode examen (nftables).
- **Backend PAC (Windows)** : couvre Chrome, Edge et Firefox via les politiques
  HKLM, avec DNS-over-HTTPS désactivé. Un navigateur portable non soumis aux
  politiques n'est pas filtré — le bloquer par `blockedApplications`.
- Les restrictions sont **silencieuses** côté élève : prévenir la classe par un
  message (fonction TextMessage) reste à la main de l'enseignant.

## Sûreté

Chaque mutation système est transactionnelle : l'état antérieur (valeurs de
registre, PAC précédent) est persisté dans
`%ProgramData%\Veyon\courserestrictions-*.json` (ou `/var/lib/veyon/…`) **avant**
modification, avec une ACL SYSTEM + Administrateurs sous Windows. Au démarrage
du service, tout état résiduel laissé par un arrêt brutal est restauré : un
poste ne reste jamais bloqué après un plantage ou une coupure.
