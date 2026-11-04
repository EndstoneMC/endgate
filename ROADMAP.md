# Endgate roadmap

Oct 30, 2026 · Vincent

A Velocity-like Bedrock proxy that switches a player between same-version backends behind a camera fade, with no loading screen. Out of scope: the plugin API and cross-version translation.

Protocols 2168, 2169, 2193 and 2225 (1.26.40 to 1.26.60). A new release is a bedrock-protocol bump and a line in `version.h`.

## Decisions

1. Endgate terminates login and encryption on both legs and logs in to each backend under its own key.
2. Backends stay in online mode and trust endgate's key beside Microsoft's (Phase 7).
3. Multi-version is routing, not translation. Endweave stays out.
4. bedrock-protocol is the codec; entity ID rewriting is generated from its reflection.
5. A switch never uses Transfer or a ChangeDimension trick; ChangeDimension only for a real dimension change.

## Phase 1: protocol in the pipeline

Gate: every packet of each supported version decodes and re-encodes byte for byte.

- [x] bedrock-protocol from a pinned release src.zip
- [x] A packet registry per version
- [x] Compression from NetworkSettings
- [x] Cipher stages
- [x] Every forwarded batch decompressed and compressed again

## Phase 2: login

Gate: a real client logs in, online and offline; the backend accepts endgate's login.

- [x] NetworkSettings, Login and token validation; a NetherNet login's xid and cpk match its offer
- [x] online-mode-kick-existing-players
- [x] Encryption with the client and with the backend
- [x] ConnectedPlayer from the login and client data
- [x] Backend login under endgate's key, self-signed until Phase 7, with SelfSignedId set so vanilla BDS keeps each player's save
- [x] Resource packs: the first backend's set, fixed for the session
- [x] login-ratelimit, connection-timeout, read-timeout
- [ ] Decide: keep the first backend's pack set, or serve one from endgate's config

## Phase 3: client state model

Gate: endgate maps every entity ID and tracks every packet that creates or destroys client state.

- [x] Entity ID map per backend, rewritten from bedrock-protocol's reflection
- [x] State per backend, kept player-side (see World state)
- [x] Client settings replayed to the next backend
- [ ] Rewrite the camera's target and attach IDs, the pick request's and the telemetry's (bedrock-protocol types them as plain integers)

## Phase 4: seamless switch

Gate: /server moves a player between two same-version backends with no loading screen.

- [x] Switch under a camera fade, with Velocity's connection results
- [x] Guard: block registry checksum and hashing, packs, movement authority
- [x] Failover on a kick or unexpected disconnect
- [x] A Transfer to a registered server becomes a switch
- [x] Real ChangeDimension into another dimension
- [ ] Refuse a different item registry before the switch commits

## Phase 5: version routing

Gate: 1.26.50 and 1.26.60 clients share one proxy, each reaching only backends on its own version.

- [x] Backend protocol from its RakNet pong or `/v1/join`, cached with the transport
- [x] try, forced hosts, /server and failover choose only backends on the player's protocol
- [x] A player with no backend on its version is told so
- [x] A backend that announces no version (BDS without `enable-lan-visibility`) is warned about once and routed to every version

## Phase 6: Velocity's proxy features

Gate: each item works on a real client.

- [x] Brigadier port; /server, /glist, /send, /shutdown and /endgate (info, reload) on the console and in the client's AvailableCommands
- [x] Command rate limit
- [x] Forced hosts from the client data's ServerAddress
- [x] Ping passthrough: disabled, description, all
- [x] GameSpy4 query on the game port
- [x] enable-player-address-logging, config reload, shutdown kick
- [x] bStats
- [x] Dual-stack default bind
- [ ] PROXY protocol v2 (researched, waits for decisions)
- [ ] Packet limiter (Velocity has none)
- [ ] Find why a client drops a Disconnect sent right after LoginSuccess (avoided by refusing before it)

Not ported: /endgate dump, ping passthrough's mods, and Java-only features (Forge, legacy ping, signed chat, BungeeCord channel).

## Phase 7: identity forwarding

Gate: an Endstone backend sees the player's real xuid, pfid, name and address, and refuses any login that didn't come through endgate.

Endgate:

- [ ] `player-info-forwarding-mode`: none (today's self-signed login) and modern; modern refused in offline mode
- [ ] Modern signs a login token: the player's Microsoft claims with new cpk, exp and iat, ES384 under endgate's P-384 key, kid = the key's base64 SPKI
- [ ] The token carries `"endgate:player_info": {"version": 1, "address": "<ip>"}`
- [ ] Key pair created on first run; the public half written as `keys/endgate_public_key.pem`

Endstone:

- [ ] Hook `MinecraftServiceKeyManager::tryGetKey`: return endgate's key for its kid, else call the original
- [ ] On player creation, read `endgate:player_info` from the raw token for endgate's kid only; refuse a version above the maximum; set the address
- [ ] `[endgate]` in `endstone.toml`: `enabled = false` and `public-key-file = "keys/endgate_public_key.pem"`; enabled refuses every login not signed by endgate's key, self-signed included, with Paper's "This server requires you to connect with endgate."
- [ ] Refuse to start when enabled and the key file is missing, saying to copy `keys/endgate_public_key.pem` from endgate, or isn't a P-384 key; log an unknown kid, an expired proxy token and a too-new version

Facts the design rests on (1.26.60):

- `tryGetKey` isn't virtual, and every key fetch replaces both key maps, so a hook beats inserting a key.
- `getTrustedInfo` drops claims it doesn't know, hence the raw token re-parse.
- A self-signed token never reaches `tryGetKey`, so `enabled` must also refuse self-signed logins.
- A non-self-signed token is checked the same in online and offline mode.
- Vanilla BDS never gets the xuid or pfid: it reads xid and mid only from a trusted token.
- Backends must be reachable only through endgate.

## World state

A switch clears or replaces every row but the last; the last must match across backends, or the switch is refused.

| State | Created or changed by | On a switch |
| --- | --- | --- |
| Actors | AddActor, AddPlayer, AddItemActor, AddPainting, SetActorData, SetActorLink | RemoveActor for each tracked ID |
| Chunks and block actors | LevelChunk, SubChunk, NetworkChunkPublisherUpdate | Empty LevelChunks; the client requests the new terrain |
| Player list | PlayerList add | PlayerList remove |
| Boss bars | BossEvent add | BossEvent remove |
| Scoreboard | SetDisplayObjective, SetScore | RemoveObjective |
| Effects on the player | MobEffect add | MobEffect remove |
| Forms and open containers | ModalFormRequest, ContainerOpen | ClientboundCloseForm, ContainerClose |
| Sounds and music | PlaySound | StopSound for all |
| Fog, HUD, camera | PlayerFog, SetHud, CameraPresets, CameraAimAssistPresets | The new backend's, or defaults |
| Titles, toasts, debug shapes | SetTitle, ToastRequest, ServerScriptDebugDrawer | Title cleared, shapes removed |
| Weather and time | LevelEvent, SetTime | The new backend's |
| Game rules, game type, difficulty, spawn, abilities, attributes | StartGame, GameRulesChanged, SetPlayerGameType, SetDifficulty, SetSpawnPosition, UpdateAbilities, UpdateAdventureSettings, UpdateAttributes | The new StartGame's, as these packets |
| Inventory | InventoryContent, InventorySlot | The new backend's InventoryContent |
| Commands, recipes, creative items | AvailableCommands, CraftingData, CreativeContent | The new backend's, with endgate's commands merged in |
| Fixed for the session | StartGame's block properties, block\_network\_ids\_are\_hashes, server\_block\_type\_registry\_checksum and movement settings; ItemRegistry; BiomeDefinitionList; DimensionData; resource packs | Must match |

## Testing

Each gate passes unit tests (gtest), integration tests against bedrock-protocol test backends (pytest), and a real client against real BDS.
