[简体中文](/zh/ui/) | **English**

# UI Design

This section is the authoritative record of Falcon's desktop UI design. It is deliberately separate from the core/developer documentation: core pages describe what the code does, while UI pages describe how it should look and behave.

The current design is a single interactive prototype at `docs/design/ui-next/`. It replaces all earlier UI explorations, which have been removed. The prototype is dependency-free and opens directly in a browser.

## Pages

- [Design Specification](/ui/design-spec) — the full 24-screen interface inventory, theme system, cross-device / account & sharing model, protocol decisions, and platform differences.
- [Desktop Redesign](/ui/desktop-redesign) — the original visual-refresh direction, information architecture, design tokens, and Qt implementation strategy.
- [Design Prototypes](/ui/design-prototypes) — the prototype history, theme-token comparison, and exported screenshot gallery.

## Core principles

- **One design, switchable accent.** Warm orange is the default (the existing brand). Cold blue and graphite green are selectable themes, not competing products: layout, components, spacing, and interactions are identical; only tokens change.
- **Light / dark / follow-system.** Theme mode lives in Preferences → Appearance, and follows `prefers-color-scheme` when set to auto.
- **Prototype only.** The UI prototype never changes production code.

## Module boundary

| Module | Scope | Location |
|---|---|---|
| Core / Developer | Architecture, protocols, API, CLI, daemon, P2SP, MCP, packaging | `/guide`, `/protocols`, `/api`, `/developer` |
| UI Design | Screens, interaction patterns, tokens, cross-device & sharing UX | `/ui` |

See [Design Specification](/ui/design-spec) for the complete screen list and decisions.
