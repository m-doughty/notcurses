local wezterm = require 'wezterm'
local output = assert(os.getenv('NC_ZOOM_OUTPUT'))
wezterm.on('user-var-changed', function(window, pane, name, value)
  if name == 'fontzoom' then
    if value == '2' then
      for _ = 1, 2 do window:perform_action(wezterm.action.IncreaseFontSize, pane) end
    elseif value == '3' then
      for _ = 1, 4 do window:perform_action(wezterm.action.DecreaseFontSize, pane) end
    elseif value == '4' then
      window:perform_action(wezterm.action.ResetFontSize, pane)
    end
  elseif name == 'physicalcapture' and value:match('^[1-5]$') then
    local f = assert(io.open(output .. '/actual-' .. value .. '.txt', 'w'))
    f:write(pane:get_lines_as_text()); f:close()
  end
end)
return {
  font_size = 12, initial_rows = 24, initial_cols = 80,
  adjust_window_size_when_changing_font_size = os.getenv('NC_ZOOM_FIXED_GRID') == '1',
  enable_tab_bar = false, window_close_confirmation = 'NeverPrompt',
}
