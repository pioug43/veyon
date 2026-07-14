# Veyon WebAPI

The WebAPI exposes Veyon's authenticated computer-control connection over HTTP. It uses the VNC/RFB server built into Veyon; no additional VNC server or desktop VNC client is required.

Enable the WebAPI in Veyon Configurator, configure HTTPS for every non-isolated network, and open `https://SERVER:11080/remote-control` for the built-in browser client.

## Authentication

1. `GET /api/v1/authentication/{computer}` lists the authentication method UUIDs offered by the computer.
2. `POST /api/v1/authentication/{computer}` with `{"method":"UUID","credentials":{...}}` returns a `connection-uid`.
3. Send that value in the `Connection-Uid` header on every subsequent request.
4. `DELETE /api/v1/authentication/{computer}` closes the connection.

The connection UID is a bearer credential. Do not log it, put it in a URL, or expose the WebAPI without TLS and network access controls.

## Browser remote control

- `GET /api/v1/connection` returns connection, framebuffer, server-version, and screen information.
- `GET /api/v1/framebuffer?format=jpg&quality=70` returns the current framebuffer.
- `POST /api/v1/input/pointer` accepts `{"x":100,"y":80,"buttonMask":1}`. RFB button bits are left `1`, middle `2`, right `4`, wheel up `8`, and wheel down `16`.
- `POST /api/v1/input/key` accepts an X11/RFB keysym, for example `{"keyCode":65293,"pressed":true}` for Enter.
- `POST /api/v1/input/clipboard` accepts `{"text":"..."}` (maximum 1 MiB characters).

The bundled page implements these endpoints with an HTML canvas. Integrations can use the same API without noVNC because Veyon performs the RFB authentication and input transport itself.

## Features

`GET /api/v1/feature` returns every feature registered by installed Veyon plugins, including its UUID, hierarchy, localized labels, description, icon, flags, and active state. Start or stop a feature using `PUT /api/v1/feature/{uuid}` with:

```json
{"active": true, "arguments": {}}
```

## Teacher screen broadcast

Authenticate one connection to the teacher computer and one connection to every target computer. Then call the WebAPI server that owns all these connections:

```http
POST /api/v1/broadcast/start
Connection-Uid: TEACHER_CONNECTION_UID
Content-Type: application/json

{
  "targetConnectionUids": ["STUDENT_1_UID", "STUDENT_2_UID"],
  "mode": "fullScreen",
  "host": "10.20.0.10"
}
```

`mode` is `fullScreen` or `window`. `host` must be the teacher address reachable by students; it is required when the teacher connection used `localhost`. `port` is optional and defaults to Veyon's demonstration-server port. The response includes the generated session token and effective host/port.

Stop both fullscreen and window clients and the teacher demo server with:

```http
POST /api/v1/broadcast/stop
Connection-Uid: TEACHER_CONNECTION_UID
Content-Type: application/json

{"targetConnectionUids":["STUDENT_1_UID","STUDENT_2_UID"]}
```

The demonstration stream is Veyon's native real-time broadcast, optimized for one teacher and many viewers. Only one active broadcast should be orchestrated by a WebAPI server at a time.

## File transfer

The WebAPI exposes all three feature operations through `POST /api/v1/feature/{uuid}/control`: `initialize`, `start`, and `stop`. This is required by the file-transfer plugin; the legacy `PUT /feature/{uuid}` Start/Stop endpoint alone cannot initialize a distribution.

To distribute files, first initialize feature `4a70bd5a-fab2-4a4b-a92a-a1e81d2b75ed`, then start it. Paths in `files` refer to files on the WebAPI server, not files in the caller's browser:

```json
{
  "operation": "initialize",
  "targetConnectionUids": ["ANOTHER_STUDENT_UID"],
  "arguments": {
    "files": ["/srv/veyon-distribution/exercise.pdf"],
    "destinationDirectory": "Documents/Course",
    "overwriteExistingFile": false,
    "openFileInApplication": false,
    "openTransferFolder": false
  }
}
```

Repeat the request with `"operation":"start"`. The connection in the `Connection-Uid` header and every optional `targetConnectionUids` entry receive the same files.

To collect files, start feature `5a14c971-e93c-457f-97a0-0b8f1058a58e` with optional `sourceDirectory`, `filePattern`, `collectRecursively`, and `destinationDirectory` arguments. The destination is on the WebAPI server. Use the `stop` operation to cancel either workflow.

File distribution and collection are asynchronous Veyon operations. Limit the service account's read/write permissions to dedicated distribution and collection directories because these endpoints intentionally access its local filesystem.

## Operational notes

- Restrict TCP/11080 to trusted application servers or administrative networks.
- Prefer key-file authentication and least-privilege Veyon access-control rules.
- Reverse proxies must preserve the `Connection-Uid` request header and should disable caching for framebuffer responses.
- A `503` with error code `13` means the Veyon connection is authenticated but not ready for remote-control events.
