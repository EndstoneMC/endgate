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

### Fetching MCS's signing keys

This is BDS's `MinecraftServiceKeyManager`; https://gist.github.com/wu-vincent/259cef5137af450d0f4b61fcb71218c7 is a working Python reference.

1. Discovery: `GET https://client.discovery.minecraft-services.net/api/v1.0/discovery/MinecraftDedicatedServer/builds/{version}`, where `{version}` is a BDS build such as `1.21.111.1`. The auth service is `result.serviceEnvironments.auth.prod.serviceUri`, without its trailing `/`.
2. `GET {auth}/.well-known/openid-configuration` gives `jwks_uri` and `issuer`. The response's `Date` header is the service's clock.
3. `GET {jwks_uri}` gives `keys`. Each entry is an RSA JWK (`kty` `RSA`, `kid`, base64url `n` and `e`) and becomes a public key indexed by `kid`.
4. Send a User-Agent on every request, as the gist does.
5. Keep the skew between the local clock and the service's `Date`, and judge token expiry by service time.

### Validating a login

This is `ServerConnectionAuthValidator::validate`, which runs `GameServerToken::validate`:

- An empty token is refused with 53.
- Unless the token is self-signed:
  - its RS256 signature must verify under the MCS key for its header's `kid`, or the login fails with 123, 124 or 46 (46 refreshes the keys);
  - `aud` must be `api://auth-minecraft-services/multiplayer`;
  - `exp` must not have passed in service time, or the login fails with 144.
- The token's claims include `xid`, `xname`, `cpk`, `pid` and `mid`. The player's UUID derives from `pocket-auth-1-xuid:{xid}`.
- The client-data JWT's `x5u` must equal the token's `cpk` and verify under it, or the login fails with 46.
- Self-signed tokens are accepted only where BDS allows them (`mAllowSelfSigned`); how that flag follows `online-mode` is unverified.
