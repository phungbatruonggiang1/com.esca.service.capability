# ESCA AI Capability Provider

Every ESCA device on the network says what it can do, so work can go to the
device best placed to run it.

    esca-inferenced ─RegisterManifestInfo("ai")─► esca-capabilityd ◄─CoAP─► peer boards
                                                        ▲ mDNS (Avahi)
    application ──FindDevices("/sections/ai/tasks", "stt")──┘

## The manifest

One JSON document per device: identity, basic device information, network,
live performance, and named sections other services register (`ai`,
sensors, anything else). It is checked against a schema, carries a version
that rises with every change and a SHA-256 hash, and a copy that arrives
late never replaces a newer one.

## Shape

| Part | Does |
| --- | --- |
| Capability manager | Owns this device's manifest and what is known about the others |
| Discovery | Publishes `_esca-cap._udp` over Avahi; TXT carries id, version and hash |
| Connection manager | Serves `/manifest` over CoAP on UDP 5683 (GET, PUT, block-wise) |
| Device manager | Keeps peers' manifests, drops a peer that leaves or goes silent |
| Network monitor | rtnetlink: an address change republishes the manifest |
| Performance monitor | CPU and memory, published only on a significant change |

## D-Bus: `vn.sparc.esca.Capability1`

Methods `GetLocalId`, `GetDeviceManifest`, `ListDevices`, `FindDevices`,
`RegisterManifestInfo`, `RemoveManifestInfo`, `GetNetworkInfo`, `Sync`;
signals `DeviceAdded`, `DeviceRemoved`, `ManifestChanged`.

Applications link `libesca-capability-client` instead of talking to the bus.

## From a terminal

    esca-capctl list
    esca-capctl show [ID]
    esca-capctl find /sections/ai/tasks stt
    esca-capctl register sensors @sensors.json
    esca-capctl watch

## Files

`/etc/esca/capability.conf`, the base manifest `device.json`, the systemd
unit and the bus policy are in `data/`. Built by the `esca-capability`
recipe in meta-sparc-esca.
