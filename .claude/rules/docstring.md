---
paths:
  - "**/*.{h,cpp}"
---

# Docstrings

- A class or method that has a counterpart elsewhere carries a Doxygen docstring naming it with `@see`, so the correspondence is discoverable from the code. Counterparts are Velocity, Netty, log4j, a RakNet or BDS symbol, or an RFC section.
- Nothing else gets a docstring.
- Write the summary as the counterpart's own documentation adapted to endgate, such as Velocity's javadoc with "Velocity" replaced by what we do.
- Use JAVADOC_AUTOBRIEF, so no `@brief`. List every counterpart, API interface first:

  ```cpp
  /**
   * Implements the console, including running commands and receiving messages from plugins.
   *
   * @see com.velocitypowered.proxy.console.VelocityConsole
   */
  ```

  ```cpp
  /**
   * @see com.velocitypowered.api.proxy.ProxyServer
   * @see com.velocitypowered.proxy.VelocityServer
   */
  ```
