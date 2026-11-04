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
| `com.mojang.brigadier` | `dispatcher.h` | `CommandDispatcher`, `ParseResults` |
| `com.mojang.brigadier` | `reader.h` | `StringReader` |
| `arguments` | `arguments.h` | `ArgumentType`, `StringArgumentType`, `BoolArgumentType`, `IntegerArgumentType`, `LongArgumentType`, `FloatArgumentType`, `DoubleArgumentType` |
| `builder` | `builder.h` | `ArgumentBuilder`, `LiteralArgumentBuilder`, `RequiredArgumentBuilder` |
| `context` | `context.h` | `CommandContext`, `Command`, `RedirectModifier`, `SingleRedirectModifier`, `ResultConsumer`, `CommandContextBuilder`, `ParsedArgument`, `ParsedCommandNode`, `StringRange`, `SuggestionContext`, `ContextChain` |
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
- `CommandSyntaxException` derives from `std::exception`, and `what()` is its `getMessage()`. It keeps a non-owning pointer to its `CommandExceptionType`, which must outlive it, as the `BuiltInExceptions` members do. `BUILT_IN_EXCEPTIONS` is a constant `BuiltInExceptions`, with no `BuiltInExceptionProvider` to swap and no `ENABLE_COMMAND_STACK_TRACES`.
- `Dynamic` exception arguments (`Object`) are `std::string`. A nullable `String` or `Message` is `std::optional`.
- `IllegalStateException` and `UnsupportedOperationException` are `std::logic_error`.
- Alias templates cannot be forward declared, so `Command` lives in `context.h` beside `CommandContext`, and `Predicate` in `tree.h`.
- `register` and `requires` are C++ keywords: `CommandDispatcher::registerCommand` and `ArgumentBuilder::requires_`.
- `ArgumentValue` replaces `T`, so `ParsedArgument`, `RequiredArgumentBuilder` and `ArgumentCommandNode` have no argument-type parameter, and `ParsedArgument` none for `S`. `ArgumentType` is not templated on `S`, so its `parse(reader, source)` and `listSuggestions(context, builder)` overloads wait for the first type that needs them, and an `ArgumentCommandNode` with no `SuggestionProvider` suggests nothing.
- `ArgumentType` is held as `std::shared_ptr<const ArgumentType>`. `StringType` constants are `SingleWord`, `QuotablePhrase` and `GreedyPhrase`, as `.clang-tidy` names enum constants.
- A class with a private constructor that is made with `std::make_shared` takes a private `Passkey` first, as `StringArgumentType` does.
- A builder's chaining methods return `T &` through `ArgumentBuilder<S, T>`'s CRTP `getThis`, and `then` accepts any builder. A builder copy shares its children, as a Java reference does.
- `createBuilder` is on `LiteralCommandNode` and `ArgumentCommandNode`, returning their own builders, since C++ has no covariant return for it on `CommandNode`; `RootCommandNode` has none.
- `CommandNode`, `CommandContext` and the other classes holding a `std::function` have no `equals` or `hashCode`, since a `std::function` cannot be compared. `ParsedCommandNode` compares its node by identity.
- A `Map` whose order is observable (`ParseResults::getExceptions`, `getSmartUsage`) is a `std::vector` of pairs in insertion order. `getAllUsage` returns a `std::vector`, `getPath` and `findNode` take one.
- `StringReader` reads bytes, so cursors and ranges are UTF-8 byte offsets, and case folding is ASCII only. `Suggestions::create` sorts case-insensitively and breaks ties by descending text.
- Redirects and forks are ported whole. `CommandNode::getRedirect` is a raw `CommandNode<S> *`, `ArgumentBuilder::redirect` and `fork` take the target as a `std::shared_ptr`, and `forward` takes the raw pointer. `CommandContextBuilder` and `CommandContext` hold their child in a `std::shared_ptr`. `RedirectModifier` returns a `std::vector<S>`.
- `CommandNode::removeChildByName` and `CommandContext::getArguments` are not Mojang's: Velocity runs on Paper's Brigadier fork, which adds them so a command can be unregistered and a suggestion provider can see the arguments parsed so far.
- Not ported yet: `AmbiguityConsumer` and `findAmbiguities`; `IntegerSuggestion`, so `SuggestionsBuilder` has no `suggest(int)`; `buildFuture`.
