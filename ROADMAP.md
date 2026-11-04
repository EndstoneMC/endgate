# Endgate roadmap: seamless switching for Bedrock

Oct 30, 2026 · Vincent

Endgate becomes a Velocity-like Bedrock proxy in seven phases, ending with a player moving between same-version backends behind a camera fade, with no loading screen. Phases 1 to 4 are the critical path; 5 to 7 can run beside them once the login works. The plugin API and cross-version translation are out of scope.

## At a glance

Phases 1 to 4 run in order; 5 and 7 can start once login works, and 6 once switching works.

## Where endgate is today

Endgate logs a player in on both legs, rewrites the player's actor IDs per backend, switches a real client between two backends under a camera fade, sends each player only to backends on its own version, and has Velocity's commands and proxy features. Phase 7's identity forwarding is next.

| Area | State |
| --- | --- |
| RakNet and NetherNet on one UDP port, all four player/backend pairings, backend transport detection | Built |
| Netty-style event loops and a typed pipeline: cipher, compression, framing and decoding | Built |
| Ping over RakNet and NetherNet, per client protocol | Built |
| Console, endgate.toml, Velocity's log lines | Built |
| Online mode: MCS keys, token validation, the NetherNet offer identity | Built |
| Login and encryption on both legs, entity ID rewriting, the client's state per backend | Built |
| Switching, failover, the guard and transfers | Built |
| Version routing: each backend's protocol from its ping, try and failover among backends on the player's version | Built |
| Velocity's commands on Brigadier, on the console and injected into the client's AvailableCommands, with the command rate limit | Built |
| Forced hosts, ping passthrough, GameSpy4 query, address logging, config reload, the shutdown kick, bStats metrics | Built |
| PROXY protocol | Researched; waits for decisions |

Endgate supports protocols 2168, 2169, 2193 and 2225 (1.26.40 to 1.26.60). bedrock-protocol models 729 to 2225, so adding a release is a bump and a line in version.h.

## Decisions the roadmap rests on

Endgate terminates both legs, as Velocity does, because a proxy can only rewrite packets it can decrypt.

1. **Endgate terminates the login and the encryption on both legs.** Bedrock's session key comes from the client's identity key, which a proxy never holds. So endgate logs in to each backend under its own key.
2. **Backends stay in online mode and trust endgate's signing key beside Microsoft's**, as Paper trusts Velocity's modern forwarding, so endgate's login carries the player's real xuid, pfid and name. Endstone gets the backend half (Phase 7).
3. **Multi-version means routing, not translation.** A 1.26.50 player reaches only 1.26.50 backends and a 1.26.60 player only 1.26.60 ones. Endweave stays out of endgate.
4. **bedrock-protocol is the codec.** It emits one struct per version, and ActorRuntimeID and ActorUniqueID are distinct C++ types its reflection can find. So entity-ID rewriting is generated, not written per packet.
5. **A switch never shows a loading screen.** Endgate never switches with Transfer or a ChangeDimension trick, whose loading screen feels like the menu. CameraInstruction's fade hides the swap; ChangeDimension is used only when the player really changes dimension.

## Phase 1: protocol in the pipeline

**Gate:** endgate decodes and re-encodes every packet of each supported version byte for byte.

- Bring in bedrock-protocol as endweave does: FetchContent of a pinned release src.zip, whose generated headers need no uv or bpc (rules/protocol.md).
- Give each supported version its own packet registry: `MinecraftDecoder<Packets...>` instantiated per protocol from bedrock-protocol's `packet_of_t`. A connection picks its registry once it learns the client's protocol.
- Switch compression on from NetworkSettings: its algorithm and threshold.
- Implement the cipher stages, which throw today. Check Bedrock's encryption against BDS before writing it.
- Decompress every forwarded batch and compress it again, as Velocity does with every packet it forwards.

## Phase 2: login, the Velocity way

**Gate:** a real client logs in through endgate in online and offline mode, endgate holds its verified identity, and the backend accepts endgate's own login.

This follows Velocity's handlers in order: HandshakeSessionHandler, InitialLoginSessionHandler, AuthSessionHandler, InitialConnectSessionHandler, then the backend's LoginSessionHandler.

1. Answer RequestNetworkSettings with endgate's own NetworkSettings.
2. Take the Login. Online mode validates its token (GameServerToken, built). For a NetherNet player it also checks that the login's xid and cpk equal the offer token's. Offline mode accepts a self-signed login.
3. Handle a second login with the same xuid as Velocity's online-mode-kick-existing-players does.
4. Start encryption with the client: ServerToClientHandshake signed with the server identity key, then ClientToServerHandshake.
5. Build the ConnectedPlayer: xuid, name, the UUID from pocket-auth-1-xuid, and the client data (skin, device, language, ServerAddress).
6. Log in to the first backend under endgate's own key, then start encryption with it. Until Phase 7 the login is self-signed: xname and ThirdPartyName carry the name, leguuid the UUID, and xuid and pfid stay empty.
7. Run the resource pack exchange with the client once, for the pack set the backends share.

Velocity's login-ratelimit, connection-timeout and read-timeout keys arrive here too.

## Phase 3: play with a client state model

**Gate:** endgate knows everything the client holds from its backend, maps every entity ID, and reads every packet that creates or destroys client state.

- **Entity ID map per backend.** The client keeps the runtime and unique IDs from its first StartGame for the whole session, and each later backend gives the player different ones. Rewrite the player's own IDs in both directions, and swap any backend entity that collides with them.
- **Generated rewriting.** Walk each packet with bedrock-protocol's reflection and rewrite every ActorRuntimeID and ActorUniqueID field. The schema references them 141 times across 24 files today; a hand-written list would drift.
- **Decode only what needs it:** packets with entity IDs while the map isn't the identity, and packets that change client state (see World state). Everything else forwards as bytes.
- **State per backend, kept player-side as Velocity keeps it.** The player owns the tab list, and ClientPlaySessionHandler keeps the backend's boss bars, actors, objectives, the player's own effects and an open container, each fed by its own handler in BackendPlaySessionHandler. The switch takes them back, and resets the title, sounds, forms, HUD, fog and camera unconditionally, as Velocity resets the title.
- **Settings replay.** Keep what the client told its first backend, such as RequestChunkRadius and ClientCacheStatus, to send to the next one, as Velocity replays ClientSettings.

Built: the entity map and its generated rewriting, the client's settings, and the state each backend leaves, chunks and debug shapes included, kept player-side and taken back at the switch. Not rewritten yet: the camera's target and attach IDs, the pick request's and the telemetry's, which bedrock-protocol still types as plain integers. The switch-time reset runs in Phase 4's two-backend test.

## Phase 4: the seamless switch

**Gate:** /server moves a player between two same-version backends with no loading screen, and the player sees only the new world and can play.

This follows Velocity's ConnectedPlayer#createConnectionRequest and TransitionSessionHandler, with the same results: SUCCESS, ALREADY\_CONNECTED, CONNECTION\_IN\_PROGRESS, CONNECTION\_CANCELLED and SERVER\_DISCONNECTED.

1. Connect and log in to the new backend while the player stays on the old one, up to its StartGame and the chunks around spawn.
2. Fade the camera out.
3. Take back what the old backend left (see World state), and overwrite its chunks with empty LevelChunks so the client asks the new backend for its terrain.
4. Turn the new StartGame into packets the client accepts mid-session: game rules, game type, difficulty, time and spawn. The new backend's RespawnPacket places the player, as on any join, or a real ChangeDimension when it puts them in another dimension.
5. Replay the client's settings to the new backend, tell it the loading screen started, flush what it queued, and disconnect the old backend.
6. At the new backend's PlayStatus PlayerSpawn, tell it the loading screen ended and the player is initialised, then fade back in.

- **Guard.** Refuse a switch the client can't survive. That means a different block registry checksum or hashing flag, item registry, pack set or movement authority.
- **Failover.** On a kick or an unexpected disconnect, move the player to the next server in try, as Velocity's failover-on-unexpected-server-disconnect does.
- **Transfers.** A backend's TransferPacket naming a registered server becomes a seamless switch instead.

Built: the switch, with the console's send; failover; the guard on the new StartGame, with the item registry and dimension data checked once they arrive; transfers; and a two-backend integration test. A real 1.26.60 client, between a NetherNet and a RakNet backend, has switched there and back, switched repeatedly, failed over after a BDS kick, and switched into and out of the Nether. The first StartGame reaches the client with client-side generation off and seed 0, since the client keeps both for the session. A switch into another dimension leaves the fade to the camera clear, so its loading screen hides the swap and nothing fades in after it. Left: a player-facing /server, which needs Phase 6's commands, and refusing a different item registry before the switch commits.

## Phase 5: version routing

**Gate:** 1.26.50 and 1.26.60 clients share one proxy, and each reaches only backends on its own protocol.

- The supported versions are endgate's list where bedrock-protocol models them. Adding a release means bumping bedrock-protocol and adding a line to version.h.
- Read each backend's protocol from its ping: the RakNet pong or the NetherNet /v1/join answer. Cache it beside the detected transport. BDS fills both only with enable-lan-visibility=true; without it, the pong's payload and the /v1/join body are empty.
- try, forced hosts, /server and failover choose only among backends on the player's protocol. A player with none is told why it can't join.
- Each version gets its own registry from Phase 1. The ping already answers each client with its own protocol.

Built: each backend's protocol read from its RakNet pong or NetherNet /v1/join and cached beside the transport; try and failover skip backends on another version; a connection request to one is refused with the versions named; a player with no backend on their version is told so; and a warning, once per backend, when one doesn't say which version it runs. A real 1.26.60 client reached the 1.26.60 backend and a real 1.26.51 client the 1.26.52 one, through one proxy at the same time, and endgate refused to send the 1.26.51 client to the 1.26.60 backend. Left: forced hosts and a player-facing /server, which arrive with Phase 6 and pass the same check. A backend without LAN visibility is routed to every version, so BDS refuses the wrong one itself.

## Phase 6: Velocity's proxy features

**Gate:** every row below works on a real client.

| Velocity | Endgate on Bedrock | Phase |
| --- | --- | --- |
| /server, /glist, /send, /velocity (info, reload, dump), shutdown | Merged into the backend's AvailableCommands; endgate intercepts their CommandRequest. /endgate replaces /velocity | 6 |
| Forced hosts | Chosen from the ServerAddress in the client data | 6 |
| Ping passthrough (disabled, description, all) | Taken from the backend's pong or NetherNet ping | 6 |
| Modern player info forwarding | A signed login token Endstone trusts | 7 |
| online-mode-kick-existing-players | Same key, checked at login | 2 |
| Tab list cleared on each switch | PlayerList entries from the state model | 3, 4 |
| Boss bars, titles and resource packs across switches | State model; packs fixed per session | 3, 4 |
| Kick and disconnect messages | Disconnect packets in Velocity's wording | 2, 4 |
| login-ratelimit, connection-timeout, read-timeout | Same keys in \[advanced\] | 2 |
| Packet limiter, command rate limit | Per-player limits on the client leg | 6 |
| failover-on-unexpected-server-disconnect, accept-transfers | Same keys | 4 |
| Query | GameSpy4 on the game port, off by default; BDS's support unverified | 6 |
| proxy-protocol | HAProxy PROXY v2, which carries UDP | 6 |
| enable-player-address-logging, config reload, shutdown kick message | Same behaviour | 6 |

Left out: the plugin API (events, plugins, scheduler) and Java-only features such as Forge, legacy ping, signed chat and the BungeeCord channel.

Built: Velocity's command layer on a 1:1 port of Brigadier. /server, /glist, /send, /shutdown and /endgate (info, reload) run on the console with tab completion. For players they are injected into each backend's AvailableCommands in BDS's own registry terms, with descriptions and soft enums kept current, and a player's CommandRequest runs on the proxy under Velocity's command rate limit. Replies take their colours from BDS and Endstone, red only for errors. Also built: forced hosts from the client data's ServerAddress; ping passthrough, without Velocity's MODS, which has no Bedrock form; GameSpy4 query on the game port, since BDS answers none; enable-player-address-logging; config reload; the shutdown kick; and bStats metrics. The default bind is dual-stack, since a Bedrock client tries IPv6 first. Real 1.26.60 and 1.26.51 clients have checked the commands, completion after a reload, a /server switch between a NetherNet and a RakNet backend, ping passthrough, forced hosts, query and the shutdown kick. Left: the PROXY protocol, researched but waiting for decisions; /endgate dump, left out because Velocity uploads it to a paste service; a packet limiter, which Velocity doesn't have; and why a client drops a Disconnect sent right after LoginSuccess, now avoided by refusing before it.

## Phase 7: identity forwarding to backends

**Gate:** an Endstone backend behind endgate sees the player's real xuid, pfid, name and address, and refuses any login that didn't come through endgate.

- **Endgate signs a real login token,** as Velocity's modern forwarding signs its payload. It copies the claims of the player's validated Microsoft token, replaces only cpk, exp and iat, and signs with its own P-384 key (ES384, its own kid). The backend stays in online mode and sees the xuid, pfid and name a direct join gives.
- **Endstone trusts endgate's public key beside Microsoft's.** The backend trusts one P-384 public key, the PEM (SPKI) file `keys/proxy_public_key.pem`, beside BDS's own `keys/server_identity_key.pem`, as a server trusts a certificate. A hook on MinecraftServiceKeyManager::tryGetKey, a new \[\[signatures\]\] entry, returns that key when the kid names it and otherwise asks the original. BDS's GameServerToken::\_validate checks only exp, aud, then tryGetKey and the signature, and getTrustedInfo reads the claims (1.26.60). tryGetKey only reads a locked map and never fetches on a miss. A key pair replaces Velocity's forwarding secret, so the backend holds no secret.
- **The kid is the key's base64 SPKI,** the body of its PEM file without line breaks, so Endstone matches a kid with a string comparison and needs no hashing. Endgate writes the public half of its key as `proxy_public_key.pem` on first run, which the admin copies into each backend's `keys/`.
- **A hook, not keys written into the manager.** MinecraftServiceKeyManager has no interface to swap (tryGetKey isn't virtual), and every fetch replaces both of its key maps, at startup and on the daily refresh. An injected key would need re-adding through subscribeToKeysUpdated, itself a new symbol, plus the manager's layout and lock (1.26.60).
- **The address rides in the same signed token** as an extra claim, the IP only, as Velocity's payload carries it. getTrustedInfo copies the claims it knows into PlayerAuthenticationInfo and drops the rest (1.26.60), so Endstone re-parses the raw token when it creates the player. It reads `endgate:player_info` only when the kid is the proxy's key, refuses a version above its maximum, and makes the address the player's.
- **Endgate gets Velocity's player-info-forwarding-mode.** none keeps today's self-signed login, with the backend in offline mode; the new mode signs the token. Endgate refuses the new mode while it runs offline, since it would vouch for xuids it never checked.
- **The signed token also works on an offline-mode backend.** \_validateBase checks a token that isn't self-signed the same way in both modes; online mode only sets mAllowSelfSigned. Offline mode also admits direct self-signed logins, so the gate's refusal needs online mode on plus a proxy-only setting, under which the hook returns nothing for a kid that isn't the proxy's key and a direct Microsoft login fails as a bad signature.
- **A vanilla BDS backend never gets the xuid or pfid.** It accepts endgate's self-signed login only with online mode off and the world's LANBroadcastIntent set (bedrock.md). It takes the name and UUID from that login, but getTrustedInfo reads xid and mid only from a trusted token (1.26.32). It still keeps each player's save: endgate sets the client data's SelfSignedId to the player's pocket-auth-1-xuid UUID, the id an online-mode join saves under as MsaId, so the backend loads the same save as a direct online-mode join (1.26.60). A client that reaches the backend directly could claim the same id.
- **Proxy-only holds whatever online mode says.** A self-signed token is checked against its own cpk and never reaches tryGetKey, so with online mode off the hook alone would still admit a direct self-signed join under any name and UUID, even another player's SelfSignedId. Endstone's proxy-only therefore also refuses, when it creates the player, any login not signed by the proxy's key, telling the player the server only accepts players through its proxy. Admins coming from Paper, who turn online mode off behind a proxy, get the same protection.
- **Endstone fails loudly on a bad setup.** It refuses to start with proxy-only on and no `keys/proxy_public_key.pem`, or with one that isn't a P-384 public key. It warns when the key file exists, proxy-only is off and online mode is off, since direct joins then go unauthenticated. A login signed by an unknown kid while the key file exists is logged with that kid, to catch a regenerated endgate key; an expired proxy token is logged as possible clock skew; a player-info version above Endstone's maximum is refused with a message to update Endstone.
- Backends must be unreachable except through endgate, as Velocity's docs require.

Velocity's modern forwarding maps onto Bedrock's login as follows. Bedrock's login is one message the server never asks for, so the request step disappears and the payload becomes claims in the token BDS already verifies.

| Velocity modern forwarding | Endgate and Endstone |
| --- | --- |
| The backend asks on `velocity:player_info` | No request: the data rides in the Login's token |
| HMAC-SHA256 with the shared secret | ES384 signature with endgate's key, verified by BDS through Endstone's trusted-key hook |
| Varint version, negotiated | A version field; fields are only added, and Endstone refuses a version above its maximum |
| address | address, the IP only |
| UUID | Nothing to send: BDS derives it from xid (makePlayerUUIDForXUID) |
| name | xname, BDS's own claim |
| Profile properties (signed textures) | The client data (skin, device), signed by the token's cpk |
| Player key and signer UUID | Dropped, as Velocity's own version 4 did; endgate holds the encryption, so the player's key is no use to the backend |
| No equivalent | xid, mid, nid, pid and ap, copied from the Microsoft token: BDS's own identity fields |
| Fails closed when the backend never asks | Closed by design: a backend that doesn't trust endgate's key refuses the token, and endgate shows Velocity's modern-forwarding-failed message |
| Replay: no nonce or expiry, so you firewall | The token expires, and its cpk ties it to endgate's key: the encryption handshake on RakNet, the offer identity on NetherNet |
| player-info-forwarding-mode: none, legacy, bungeeguard, modern | none and modern; legacy and bungeeguard serve pre-1.13 Java servers |
| forwarding-secret-file | A private key file endgate creates on first run, with its public half written as `proxy_public_key.pem` for each backend's `keys/` |

The token keeps BDS's own claims and adds one namespaced object, named after Velocity's channel. Endstone reads it only when the kid is the proxy's key.

```json
{
  "xid": "…", "mid": "…", "xname": "…",
  "cpk": "<endgate's key>", "aud": "api://auth-minecraft-services/multiplayer",
  "exp": 0, "iat": 0,
  "endgate:player_info": { "version": 1, "address": "203.0.113.5" }
}
```

This phase is one hook and two settings in Endstone, plus the signed token and forwarding mode in endgate. It can start now that Phase 2 has a working backend login.

## World state the client holds

A switch clears or replaces every row but the last; the last must match across backends, or the switch is refused. Packet names are bedrock-protocol's.

| State | Created or changed by | On a switch |
| --- | --- | --- |
| Actors: mobs, players, items, paintings | AddActor, AddPlayer, AddItemActor, AddPainting, SetActorData, SetActorLink | RemoveActor for each tracked ID |
| Chunks and block actors | LevelChunk, SubChunk, NetworkChunkPublisherUpdate | Overwritten with empty LevelChunks; the client requests the new backend's terrain |
| Player list | PlayerList add | PlayerList remove for the old backend's entries |
| Boss bars | BossEvent add | BossEvent remove |
| Scoreboard | SetDisplayObjective, SetScore | RemoveObjective |
| Effects on the player | MobEffect add | MobEffect remove |
| Forms and open containers | ModalFormRequest, ContainerOpen | ClientboundCloseForm, ContainerClose |
| Sounds and music | PlaySound | StopSound for all |
| Fog, HUD, camera | PlayerFog, SetHud, CameraPresets, CameraAimAssistPresets | The new backend's, or defaults |
| Titles, toasts, debug shapes | SetTitle, ToastRequest, ServerScriptDebugDrawer | Title cleared, shapes removed |
| Weather and time | LevelEvent, SetTime | The new backend's values |
| Game rules, game type, difficulty, spawn, abilities, attributes | StartGame, GameRulesChanged, SetPlayerGameType, SetDifficulty, SetSpawnPosition, UpdateAbilities, UpdateAdventureSettings, UpdateAttributes | The new StartGame's values, sent as these packets |
| Inventory | InventoryContent, InventorySlot | The new backend's InventoryContent |
| Commands, recipes, creative items | AvailableCommands, CraftingData, CreativeContent | The new backend's; endgate merges its own commands in |
| Fixed for the session | StartGame's block properties, block\_network\_ids\_are\_hashes, server\_block\_type\_registry\_checksum and movement settings; ItemRegistry; BiomeDefinitionList; DimensionData; resource packs | Must match, or the switch is refused |

## Open questions

Ticked items are decided. Resource packs needs your decision; the rest are checks, against BDS in IDA or on a real client.

- [x] **Dimensions.** Decided: ChangeDimension is banned only as a switching trick, so a switch into another dimension uses a real ChangeDimension (architecture.md).
- [ ] **Resource packs.** Bedrock applies packs only at login. Require one pack set across backends, or have endgate serve a pack set from its own config? Gates Phase 2.
- [x] **Packaging.** Decided: FetchContent of a pinned release src.zip and its generated headers, never uv or bpc (rules/protocol.md).
- [x] **Unloading chunks without ChangeDimension.** Answered on a real client: empty LevelChunks. The client requests each one's terrain again, so the new backend fills it in; a far NetworkChunkPublisherUpdate unloads nothing. CubeCraft's capture switches the same way, under a camera fade.
- [x] **Vanilla BDS identity.** Answered: a self-signed token gives BDS the name and UUID, never the xuid or pfid (1.26.32). Vanilla backends work without xuids, and keep each player's save through SelfSignedId (1.26.60).
- [ ] **Encryption of a self-signed session.** Does BDS still encrypt it? Decides the backend leg's cipher setup in Phase 2.
- [ ] **The offer token's audience.** Confirm it is the multiplayer audience on the next real-client capture.

## Testing

Each gate is proven in three layers. No BDS runs locally.

| Layer | What it proves |
| --- | --- |
| Unit (gtest) | Per-version round trips over bedrock-protocol's golden bytes, the entity ID map, each state tracker, the switch state machine |
| Integration (pytest) | A test backend built on bedrock-protocol, like the NetherNet echo harness, plays a minimal BDS: login, StartGame, chunks, actors, a kick. The harness drives a client through endgate to two such backends |
| Real client | You check each phase gate on a real client against real backends |
