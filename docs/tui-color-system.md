# TUI color system: research, implementation and validation

## User context and concrete failure

The user found the existing terminal colors unacceptable. This is a usability
requirement, not a request for a new networking, layout or terminal engine.
Inspection of the pinned FTXUI 7.0.3 `component_options.cpp` shows that
`InputOption::Default().transform` applies `inverted` to the entire focused input.
The application had not overridden it. The pure view also left separators using
the same bright foreground as body copy, while widgets and view used independent
hard-coded colors. Merely replacing the old teal RGB would leave these defects.

## Relevant practice and research

- [Primer functional color roles](https://primer.style/product/getting-started/foundations/color-usage/)
  distinguish foreground, background, border and interaction intent. Adopt role
  names rather than scattering widget-specific literal colors. The chosen neutral
  charcoal/blue direction is inspired by Primer; it is not an exact copy or a claim
  of Primer compliance, and adds no Primer/React dependency.
- [Catppuccin's official style guide](https://github.com/catppuccin/catppuccin/blob/main/docs/style-guide.md)
  separates main/secondary panes, surfaces, body copy, secondary labels, selection
  and semantic status colors. Adopt the hierarchy, not every pastel accent.
- [W3C contrast guidance](https://www.w3.org/WAI/WCAG22/Understanding/contrast-minimum)
  supplies the 4.5:1 reference for ordinary text. Use explicit foreground/background
  pairs and automated sRGB relative-luminance checks. This is an engineering
  readability gate for canonical truecolor values, not a claim of complete WCAG
  certification or guaranteed contrast after arbitrary terminal palette remapping.
- [W3C use of color](https://www.w3.org/WAI/WCAG22/Understanding/use-of-color.html)
  motivates preserving textual status, an active-room marker and underlined
  keyboard focus instead of using hue alone.
- [FTXUI InputOption](https://www.arthursonzogni.com/FTXUI/doc/structftxui_1_1InputOption.html)
  provides supported transforms; the local pinned source, not possibly newer online
  examples, determines the API. Menu entries use `entries_option.transform` in 7.0.3.
- [While and Sarvghad, IEEE VIS 2024](https://arxiv.org/abs/2409.10841)
  found individual differences in dark/light visualization performance and a
  mismatch between preference and performance. Its tasks were charts, not chat;
  it does not prove one universally best chat palette. Keep roles centralized so
  a later light/high-contrast option need not rewrite widgets; do not add a theme
  framework or infer that darker automatically means more readable.

## Chosen implementation

`src/client/theme.hpp` is one small semantic palette and three FTXUI transforms.
It contains no IO, model state, configuration parser or terminal rendering engine.
The conversation view, Input, Menu and send action consume the same roles.

| Role | RGB | Application |
| --- | --- | --- |
| canvas | #161b22 | Conversation/background |
| sidebar | #0d1117 | Secondary navigation surface |
| input | #21262d | Dark composer and action surface |
| selection | #263750 | Selected room; never an inverse white slab |
| text | #e6edf3 | Message bodies, headings, peer author |
| secondary | #9da7b3 | Time, read/sent metadata, labels, placeholder |
| outline | #303943 | Decorative separators, not required text/focus |
| accent | #79c0ff | Active room, unread, own author and send affordance |
| success / warning / error | #7ee787 / #e3b341 / #ff7b72 | Online / uncertain / failed; retain words |

Input and send focus no longer invert their whole surface. FTXUI continues owning
its native text cursor, grapheme editing and terminal lifecycle. Active rooms keep
`>` and bold text; keyboard-focused menu/send entries add underlining. Peer names
are neutral instead of adding a second decorative hue. Only failures and uncertain
sends introduce red/amber. Header branding and divider lines no longer compete
with message content. The existing layout dimensions, history visibility/read
calculation, wire protocol, cache, CLI and transport are unchanged. FTXUI retains
responsibility for color capability detection and NO_COLOR handling.

Canonical contrast: body/canvas 14.64:1; secondary/canvas 7.09:1;
secondary/input 6.24:1; secondary/selection 4.94:1. All 24 checked text-role versus
surface-role pairs exceed 4.5:1 (minimum 4.77:1, error on selection). Decorative
rules intentionally have lower contrast and are not the keyboard focus indicator.

## Actual evidence

Windows MSVC Debug full CTest: 13/13 passed, 19.77 s, including actual native
ConPTY Unicode input/edit/send and console restoration. `conversation_view` adds
24 contrast-pair checks and rendered Cell assertions for focused dark input,
non-color room/focus cues and non-inverted send focus; existing 120 seeded layout
and control-safety cases remain in the same suite.

Real hidden ConPTY showcase journeys passed at 120x32 and 40x12; independent peers
confirmed message/read/unread behavior and unsent-draft preservation. Captures have
no unknown CSI commands. Actual captured screen/SGR output was reconstructed to SVG
and rendered with headless Edge; the resulting PNGs were visually inspected. These
are real terminal-output projections, not physical Windows Terminal screenshots or
handwritten mockups. Artifacts are `.cache/tui-colors/{wide,narrow}/conversation.*`.
No visible user windows or retained hybrid services were started.

WSL Linux PostgreSQL-enabled build: full 13/13 CTest passed in 44.62 s,
including both real PTY suites and the complete bounded-history/cancellation
journey with the new component styles.
