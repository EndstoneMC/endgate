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
