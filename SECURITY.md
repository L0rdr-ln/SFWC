# Security policy

SFWC is alpha software: it runs as your session's compositor and starts programs from its
config, so please read this before sharing configs or themes.

## Reporting a problem

Use GitHub's **private vulnerability reporting** (Security tab → "Report a vulnerability") for
anything that could be exploited. Please do not open a public issue for those. Other bugs go to
the normal issue tracker.

## What counts, and what does not

- A theme, config or template that makes SFWC or its helper `sfwc-theme-apply` do something the
  documentation says it cannot is a bug worth reporting.
- **By design:** a config file can run commands (`spawn:`, `[autostart]`), and a theme that ships
  its own templates can make *other* programs (for example mako) run commands. Treat both like
  scripts; see [Safety: what installing a theme trusts](docs/THEMES.md#safety-what-installing-a-theme-trusts).
- The virtual keyboard/pointer protocols (which let any client inject input) are only offered when
  started with `SFWC_ENABLE_VIRTUAL_INPUT=1`, which exists for the tests. Do not set it in a session.

## Supported versions

Only the latest release and `main` get fixes.
