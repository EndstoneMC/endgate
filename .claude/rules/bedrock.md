# Bedrock facts

Never infer how the game behaves. Check anything touching the protocol, the wire format or BDS's own types against a real source before writing it:

| Source | Holds |
| --- | --- |
| EndstoneMC/bedrock-headers | BDS's headers |
| IDA databases of `bedrock_server` | the bodies those headers omit |
| EndstoneMC/endstone `src/bedrock` | reconstructions already cross-checked |
| EndstoneMC/protocol-docs | dumps taken from a running server |
| EndstoneMC/bedrock-protocol | packet schemas with golden bytes |
| EndstoneMC/nethernet | NetherNet, reconstructed from BDS |

- Two independent sources beat one plausible pattern. A header that declares without defining is a gap to take to the disassembly, not one to fill with something that reads correctly.
- For NetherNet, EndstoneMC's reconstructions outrank clients built from observation, such as df-mc/go-nethernet.
- When a source is unavailable, say so and leave the question open rather than guessing past it.

## The client during a switch

Seen on real clients, 1.26.50 and 1.26.60:

- A LevelChunk with no subchunks and a biome-only payload (`01 02` per section) replaces the chunk; the client then sends SubChunkRequests for it and draws what comes back, within a frame.
- A NetworkChunkPublisherUpdate centred far away unloads nothing.
- CubeCraft switches without a dimension change: a 0.5 s, 1.5 s, 0.5 s camera fade; RemoveActor, PlayerList, RemoveObjective, MobEffect, close form, StopSound, SetHud and inventory clears; a MovePlayer teleport; the new world's state as individual packets; a publisher update and request-mode LevelChunks (limit 7) at the new spawn; then PlayStatus PlayerSpawn again. Its worlds sit thousands of blocks apart, so it never overwrites old chunks.
- Hive switches with ChangeDimension to the Nether at Y 4000 and straight back, which shows the loading screen.
- Hive and CubeCraft run their own server software, built for minigames rather than SMP. Their captures show what the client tolerates, never what a BDS backend sends or what a switch must do; Velocity, built for SMP, settles that.

## Online mode

BDS authenticates a login against Minecraft Services (MCS), not Xbox Live directly.

### MCS's signing keys

BDS's `MinecraftServiceKeyManager` fetches them; https://gist.github.com/wu-vincent/259cef5137af450d0f4b61fcb71218c7 is a working Python reference.

1. Discovery (`getDiscoveryServiceURL`): `GET https://client.discovery.minecraft-services.net/api/v1.0/discovery/MinecraftDedicatedServer/builds/{version}`, where `{version}` is `Common::getGameVersionStringNet()`. The auth service is `result.serviceEnvironments.auth.prod.serviceUri`, without its trailing `/`. Dev, perf and stage environments use other discovery hosts.
2. `GET {auth}/.well-known/openid-configuration` gives `jwks_uri` and `issuer`. Both must be non-empty or the fetch counts as failed. The response's `date` header is the service's clock.
3. `GET {jwks_uri}` must answer 2xx with `keys`.
   - Each entry with a `kid` is parsed: RSA with a 1024, 2048 or 4096-bit modulus, or EC on P-256, P-384 or P-521.
   - Entries that don't parse are skipped, and the first of a duplicate `kid` wins.
   - A fetch that ends with no keys fails.
4. Send a User-Agent on every request, as the gist does.
5. Time correction: the offset between local time and the `date` header is added to local time wherever a token's expiry is checked. BDS logs a warning when the two differ by 601 s or more.

Renewal:

- `onTick`, driven by `Minecraft::update`, fetches again once 7 days have passed since the last fetch started, or when the clock has gone backwards past it.
- A token whose signature fails, an unknown `kid` included, forces a fetch when the last one started more than 600 s ago.
- A failed fetch keeps the old keys.
- At startup the server fetches until it has keys. When it can't reach MCS it reports "Could not connect to Minecraft services. This is required to accept connections in online mode."
- The trusted keys are MCS's keys plus the server's own host key under `kid` `host`.

### Validating a login

This is `ServerConnectionAuthValidator::_validateBase`, called from `ServerNetworkHandler::_validateLoginPacket`. Failures name a `DisconnectFailReason`, in this order:

1. A malformed request fails with 46 `NotAuthenticated`, and an empty token with 53 `LoginPacketNoCert`.
2. A token that isn't self-signed goes through `GameServerToken::validate`:
   - an expired `exp` (in corrected time) fails with 144 `ExpiredToken`; the login path always checks expiry;
   - any other failure while the server holds no MCS keys fails with 124 `HostWithoutKeys`;
   - a signature that doesn't verify under the trusted key for the token's `kid` fails with 46, after the forced fetch above;
   - an `aud` other than `api://auth-minecraft-services/multiplayer` fails with 123 `DiscoveryEnvironmentMismatch` when the token's `iss` differs from the OpenID `issuer`, else with 46. `iss` is consulted only here.
3. A self-signed token needs `mAllowSelfSigned`, else 46. It is then checked for expiry (144) and verified under its own `cpk` (46).
   - `mAllowSelfSigned` is the world's `LANBroadcastIntent` and not `requireTrustedAuthentication`, which equals `online-mode` on BDS.
   - `LANBroadcastIntent` is a byte tag in `level.dat`. A new world takes it from its `LevelSettings`, and `enable-lan-visibility` doesn't set it.
   - So a self-signed login is accepted only with `online-mode = false` and the world's `LANBroadcastIntent` set.
4. The client-data JWT's `x5u` must equal the token's `cpk` and verify under it (`WebToken::verifyWithIncludedKey`), else 46.
5. An empty display name fails with 143 `InvalidName`.

The player's UUID comes from `mce::makePlayerUUIDForXUID`: `pocket-auth-1-xuid:{xid}`, with PSN and Nintendo variants.
