---
paths:
  - "src/endgate/brigadier/**"
  - "tests/unit/brigadier/**"
---

# Brigadier

Velocity runs its commands on Mojang's Brigadier, and so does endgate, ported the way Netty is. Velocity's command layer (`velocity.md`) sits on top of it.

Mojang's `com.mojang.brigadier` (github.com/Mojang/brigadier), 1:1: class names, method names, signatures and semantics. It is a library of its own, `endgate::brigadier` in `src/endgate/brigadier/`, built as `endgate_brigadier` against the standard library alone, so it can move to Endstone unchanged. Each Java package is one file:

| Package | File | Classes |
| --- | --- | --- |
| `com.mojang.brigadier` | `dispatcher.h` | `CommandDispatcher`, `ParseResults`, `Command`, `RedirectModifier`, `SingleRedirectModifier`, `ResultConsumer`, `AmbiguityConsumer` |
| `com.mojang.brigadier` | `reader.h` | `StringReader` |
| `arguments` | `arguments.h` | `ArgumentType`, `StringArgumentType`, `BoolArgumentType`, `IntegerArgumentType`, `LongArgumentType`, `FloatArgumentType`, `DoubleArgumentType` |
| `builder` | `builder.h` | `ArgumentBuilder`, `LiteralArgumentBuilder`, `RequiredArgumentBuilder` |
| `context` | `context.h` | `CommandContext`, `CommandContextBuilder`, `ParsedArgument`, `ParsedCommandNode`, `StringRange`, `SuggestionContext`, `ContextChain` |
| `exceptions` | `exceptions.h` | `CommandSyntaxException`, `CommandExceptionType`, `SimpleCommandExceptionType`, `DynamicCommandExceptionType` and its `2`, `3`, `4` and `N` forms, `BuiltInExceptions` |
| `suggestion` | `suggestion.h` | `Suggestion`, `Suggestions`, `SuggestionsBuilder`, `SuggestionProvider`, `IntegerSuggestion` |
| `tree` | `tree.h` | `CommandNode`, `RootCommandNode`, `LiteralCommandNode`, `ArgumentCommandNode` |

- Port a class when endgate first needs it, whole and 1:1, into the file the table names. Leave the rest out rather than stubbing it.
- Port Brigadier's own tests for what is ported, JUnit to gtest, in `tests/unit/brigadier/`.

Deviations from Brigadier:

- The source `<S>` is a template parameter held by value, so it is a cheap handle. Endgate's is `std::shared_ptr<CommandSource>`.
- Java's other generics are template parameters too. An argument's result is an `ArgumentValue` (`arguments.h`), a `std::variant` of every `ArgumentType`'s result type: `bool`, `int`, `std::int64_t`, `float`, `double` and `std::string`. A new `ArgumentType` adds its result type there. `CommandContext::getArgument<V>(name)` reads it with `std::get_if`, and throws `std::invalid_argument` where Java throws `IllegalArgumentException`.
- Suggestions are synchronous. `getCompletionSuggestions`, `listSuggestions` and a `SuggestionProvider` return `Suggestions`, not `CompletableFuture<Suggestions>`, since a command runs on one loop.
- `Message` and `LiteralMessage` are `std::string`. Bedrock text is a string, so there is no rich message to carry.
- `ImmutableStringReader` is `const StringReader &`.
- `Command`, `Predicate<S>`, `RedirectModifier` and `SuggestionProvider` are `std::function`s. `Command.SINGLE_SUCCESS` is `SINGLE_SUCCESS` in the namespace.
- A node is a `std::shared_ptr<CommandNode<S>>`, owned by its parents and by the dispatcher's root. A redirect is a non-owning pointer, since it may point back up the tree.
- Children keep insertion order, as Java's `LinkedHashMap` does, so usage, suggestions and the client's overloads come out in registration order.
- `CommandSyntaxException` derives from `std::exception`, and `what()` is its `getMessage()`.
