-- Run in a clean REAPER 7.85+ test profile with the extension loaded.
-- Create frzz-subtitle-undo-test.enabled in that profile's resource directory first.
local r = reaper
local root = r.GetResourcePath()
local marker = io.open(root .. "/frzz-subtitle-undo-test.enabled", "r")
if not marker then return end
marker:close()

local report = assert(io.open(root .. "/single-undo-probe.txt", "w"))
local function log(message)
  report:write(message, "\n")
  report:flush()
end

local function checked(fn)
  local ok, err = xpcall(fn, debug.traceback)
  if not ok then
    log(err)
    log("DONE")
    report:close()
  end
end

local function after_timer(fn)
  local deadline = r.time_precise() + 0.4
  local function wait()
    if r.time_precise() < deadline then r.defer(wait) else checked(fn) end
  end
  r.defer(wait)
end

local path = root .. "/frzz-subtitle-undo-test.ass"
local subtitle = "[Events]\n" ..
  "Dialogue: 0,0:00:00.00,0:00:00.00,Default,Actor,0,0,0,,Original\n" ..
  "Dialogue: 0,0:00:01.25,0:00:02.50,Default,,0,0,0,,Line 10\\NLine 2\n"
local function write_subtitle()
  local file = assert(io.open(path, "wb"))
  assert(file:write(subtitle))
  file:close()
end

local function check_items()
  assert(r.CountMediaItems(0) == 2)
  for i, cue in ipairs({{3, 0, "[Actor] Original"}, {4.25, 1.25, "|Line 10\n|Line 2"}}) do
    local item = r.GetMediaItem(0, i-1)
    assert(r.CountTakes(item) == 0)
    assert(math.abs(r.GetMediaItemInfo_Value(item, "D_POSITION") - cue[1]) < 1e-9)
    assert(math.abs(r.GetMediaItemInfo_Value(item, "D_LENGTH") - cue[2]) < 1e-9)
    local ok, chunk = r.GetItemStateChunk(item, "", false)
    assert(ok and chunk:gsub("\r\n", "\n"):find(cue[3], 1, true), "Missing original text")
  end
end

local function check_trim()
  write_subtitle()
  r.SetOnlyTrackSelected(r.GetTrack(0, 0))
  r.SetEditCurPos(9, false, false)
  assert(r.InsertMedia(path, 0) == 1)
  local placeholder = assert(r.GetSelectedMediaItem(0, 0))
  assert(r.SetMediaItemInfo_Value(placeholder, "D_LENGTH", 0.4))
  after_timer(function()
    assert(r.CountMediaItems(0) == 3)
    assert(r.ValidatePtr2(0, placeholder, "MediaItem*") and r.CountTakes(placeholder) == 1)
    assert(math.abs(r.GetMediaItemInfo_Value(placeholder, "D_LENGTH") - 0.4) < 1e-9)
    log("Right trim preserved")
    log("PASS")
    log("DONE")
    report:close()
  end)
end

r.defer(function() checked(function()
  assert(r.CountTracks(0) == 0 and r.CountMediaItems(0) == 0, "Test requires a blank project")
  assert(r.Undo_GetCurEntry, "Test requires REAPER with the Undo history API")
  write_subtitle()

  r.Undo_BeginBlock2(0)
  r.InsertTrackAtIndex(0, true)
  local track = assert(r.GetTrack(0, 0))
  r.SetOnlyTrackSelected(track)
  r.Undo_EndBlock2(0, "Test setup", -1)

  local preview = assert(r.PCM_Source_CreateFromFile(path))
  local setup_entry = r.Undo_GetCurEntry(0)
  r.Undo_BeginBlock2(0)
  r.GetSetMediaTrackInfo_String(track, "P_NAME", "Edit during preview", true)
  r.Undo_EndBlock2(0, "Test edit", -1)
  r.PCM_Source_Destroy(preview)
  local before = r.Undo_GetCurEntry(0)
  assert(before == setup_entry + 1 and r.Undo_CanUndo2(0) == "Test edit")
  log("Preview leaves undo history available for unrelated edits")

  r.SetEditCurPos(3, false, false)
  assert(r.InsertMedia(path, 0) == 1)
  after_timer(function()
    check_items()
    assert(r.Undo_GetCurEntry(0) == before + 1, "Import added more than one undo entry")
    assert(r.Undo_DoUndo2(0) ~= 0 and r.CountMediaItems(0) == 0)
    assert(r.Undo_GetCurEntry(0) == before)
    assert(os.remove(path))
    assert(r.Undo_DoRedo2(0) ~= 0)
    after_timer(function()
      check_items()
      assert(r.Undo_GetCurEntry(0) == before + 1, "Redo added an undo entry")
      log("One undo removes import and one redo restores original text without the subtitle file")
      check_trim()
    end)
  end)
end) end)
