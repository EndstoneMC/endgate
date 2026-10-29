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

## Online mode

BDS authenticates a login against Minecraft Services (MCS), not Xbox Live directly.

### MCS's signing keys

BDS's `MinecraftServiceKeyManager` fetches them; https://gist.github.com/wu-vincent/259cef5137af450d0f4b61fcb71218c7 is a working Python reference.

1. Discovery (`getDiscoveryServiceURL`): `GET https://client.discovery.minecraft-services.net/api/v1.0/discovery/MinecraftDedicatedServer/builds/{version}`, where `{version}` is `Common::getGameVersionStringNet()`. The auth service is `result.serviceEnvironments.auth.prod.serviceUri`, without its trailing `/`. Dev, perf and stage environments use other discovery hosts.
2. `GET {auth}/.well-known/openid-configuration` gives `jwks_uri` and `issuer`. Both must be non-empty or the fetch counts as failed. The response's `date` header is the service's clock.
3. `GET {jwks_uri}` gives `keys`. They are stored by `kid`.
4. Send a User-Agent on every request, as the gist does.
5. Time correction: the offset between local time and the `date` header is added to local time wherever a token's expiry is checked. BDS logs a warning when the two differ by 601 s or more.

Renewal (`onTick`, driven by `Minecraft::update`):

- The keys are fetched again once 7 days have passed since the last fetch started, or when the clock has gone backwards past it. A failed fetch keeps the old keys and still waits the full 7 days.
- An unknown `kid` never triggers a fetch; the lookup just finds no key.
- At startup the server fetches until it has keys. When it can't reach MCS it reports "Could not connect to Minecraft services. This is required to accept connections in online mode."
- The trusted keys are MCS's keys plus the server's own host key under `kid` `host`.

### Validating a login

This is `ServerConnectionAuthValidator::_validateBase`, called from `ServerNetworkHandler::_validateLoginPacket`. Failures name a `DisconnectFailReason`:

- A malformed request fails with 46 `NotAuthenticated`, and an empty token with 53 `LoginPacketNoCert`.
- A token that isn't self-signed goes through `GameServerToken::_validate`:
  - `exp` must not have passed in corrected time; the login path always checks expiry;
  - `aud` must be `api://auth-minecraft-services/multiplayer`;
  - the signature must verify under the trusted key for the token's `kid`.
- When that fails, the reason is:
  - 124 `HostWithoutKeys` if the server has no MCS keys;
  - otherwise 123 `DiscoveryEnvironmentMismatch` if the token's `iss` differs from the OpenID `issuer`;
  - otherwise 46.
  - `iss` is compared only to choose this reason, never on success.
  - An expired token therefore fails as 124, 123 or 46, not 144 `ExpiredToken`.
- A self-signed token is accepted only when the validator's `mAllowSelfSigned` is set, otherwise it fails with 46. It is checked for expiry and verified under its own `cpk`. What sets `mAllowSelfSigned` is unverified.
- The client-data JWT must verify under the token's public key (`WebToken::verifyWithIncludedKey`), or the login fails with 46.
- The player's UUID comes from `mce::makePlayerUUIDForXUID`: `pocket-auth-1-xuid:{xid}`, with PSN and Nintendo variants.
