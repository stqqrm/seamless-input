Themes for the seamless-input window
====================================

Every *.theme file in this folder shows up in the "Theme" menu (File | Service | Security | Theme).
Put your own themes in the "theme" folder of the data directory (Theme menu > Open theme folder);
they survive updates, and a file there replaces a built-in theme with the same name.
Choose "Reload themes" in the menu after adding or editing a file.

Format: one "key = value" per line. Lines starting with # or ; are comments.

  name   = My Theme        shown in the menu
  dark   = true            true or false: dark window decorations, drawing details (default: from page)
  page   = #1e1e2e         window background
  card   = #313244         panels / cards
  border = #45475a         outlines
  track  = #585b70         sliders, tab strip, hovered items
  text   = #cdd6f4         main text
  sub    = #a6adc8         secondary text
  accent = #89b4fa         highlights, this PC in the monitor map
  ok     = #a6e3a1         "connected", "running"
  warn   = #f9e2af         warnings, other PCs in the monitor map
  bad    = #f38ba8         errors, muted

Colours are #RRGGBB. A missing key uses the built-in Dark or Light value (chosen by "dark").
Use ten different colours: the window recognises a colour by its value when it switches themes,
so two keys with the same value are nudged apart by one step.
Files that cannot be read are skipped; the reason is written to the activity log.
