# Plugin DeviceControl — contrôles matériels

Quatre contrôles indépendants, activables séparément pendant un cours : couper
le son, bloquer le stockage USB, empêcher l'impression, désactiver les webcams.
Couper le son n'empêche pas les clés USB.

| Fonctionnalité | UUID | Windows | Linux |
|---|---|---|---|
| `MuteAudio` | `e1b7d340-92af-4c65-8d13-6a0f5b2c7e94` | CoreAudio (périphérique de sortie par défaut) | `pactl set-sink-mute` |
| `BlockUsbStorage` | `4f8a1c07-63be-4d92-a5f1-8c204e7b93d6` | politiques `USBSTOR` + `RemovableStorageDevices` | règle udev refusant `usb-storage` |
| `BlockPrinting` | `9d05e2b8-71c4-4a3f-b628-0e5f1a94c37b` | arrêt du service `Spooler` | arrêt du service `cups` |
| `BlockWebcam` | `2a63f9d1-08e5-471c-9b40-d7c3e85216af` | politique `ConsentStore\webcam` = `Deny` | retrait des droits sur `/dev/video*` |

## Pilotage par la Web API

```http
PUT /api/v1/feature/{uuid}   { "active": true }    # poser le contrôle
PUT /api/v1/feature/{uuid}   { "active": false }   # le lever
GET /api/v1/feature/{uuid}
→ { "active": true, "status": { "usb": "APPLIED", "timestamp": 1769… } }
```

Valeurs d'état : `APPLIED`, `IDLE`, `REJECTED`, `UNSUPPORTED`, et
`EXAM_MODE_ACTIVE` pour l'USB pendant un examen.

## Priorité du mode examen

Le mode examen pose son propre blocage USB, à bail et signé. Il reste
**prioritaire** : tant qu'un examen est actif sur le poste, le blocage USB de ce
plugin est refusé. Sans cet arbitrage, les deux écriraient dans les mêmes
valeurs de registre et se restaureraient de travers. Les trois autres contrôles
ne sont pas disputés et restent disponibles pendant un examen.

## Sûreté

Les mutations de registre passent par `RegistryPolicyGuard` (bibliothèque
partagée `veyon-restrictions`) : l'état antérieur est persisté avant écriture,
et une valeur modifiée entre-temps par un tiers n'est jamais écrasée à la
restauration. Au démarrage du service, un blocage résiduel laissé par un arrêt
brutal est levé — un poste ne redémarre jamais muet ou sans clé USB.

## Limites connues

- **USB sous Linux** : la règle udev ne vaut que pour les branchements
  suivants ; un volume déjà monté n'est pas démonté.
- **Impression** : arrêter le service coupe l'impression pour toute la machine,
  y compris pour une tâche administrative en cours.
- **Webcam sous Windows** : la politique `ConsentStore` couvre les applications
  qui respectent le cadre de confidentialité de Windows.
