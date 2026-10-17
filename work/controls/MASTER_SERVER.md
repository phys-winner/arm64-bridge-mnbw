# Warband master server selection

The multiplayer profile's **Master server** control selects LAN only, the
original TaleWorlds server, `oreo922.cn`, or a saved custom server. The choice
applies immediately to the running game and is saved in `rgl_config.txt` in the
app's external files directory. A fresh install starts in LAN-only mode.
The bridge updates both the original URL literals and the active `basic_game`
URL/host strings; changing only the literals would leave the current process
connected to the previous master. The guest browser source mapping patch at
`0x126FE8` is included in the packaged `libMBExpMobile.so`.

For a manual custom-server configuration, use:

```ini
master_server_mode = custom
master_server_host = master.example.org
```

Use only a DNS hostname or IPv4 address, without a scheme, path, or port. The
guest client builds the stock HTTP server-list requests and appends its existing
`handlerservers.ashx` query. The hostname must be at most 26 ASCII characters,
which is the capacity of the original Warband 1.071 guest-library URL slot. If
custom mode has no valid host, the client uses LAN-only mode. Official and
`oreo922.cn` modes do not need this entry. Direct edits to the config file are
read on the next launch; use the in-game control to switch without a restart.

The host must serve the same Warband mobile list response expected by the guest
client. This setting changes only the master-server destination; it does not
implement server heartbeats or change dedicated-server authentication.
