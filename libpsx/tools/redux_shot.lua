-- redux_shot.lua - capture the PlayStation framebuffer headlessly.
--
-- Drives PCSX-Redux through its Lua API to grab the displayed framebuffer
-- after a number of frames, write it to a file, and quit. It opens no window:
-- run redux with -cli -softgpu so the software GPU renders into VRAM with no
-- OpenGL context, and this script reads that VRAM through PCSX.GPU.takeScreenShot.
--
-- Invocation (see the test-psx-shot-redux Makefile target):
--   pcsx-redux -cli -softgpu -testmode -run -no-gui-log \
--       -bios <bios> -exe <file> -dofile libpsx/tools/redux_shot.lua
--
-- Environment:
--   PSX_SHOT         output path (default build/psx/shot.bin)
--   PSX_SHOT_FRAMES  frames to run before the capture (default 90)
--
-- Output file layout (all integers little-endian):
--   u16 width, u16 height, u16 bpp (0 = 16-bit BGR555, 1 = 24-bit),
--   then width*height pixels in that format, row-major, top row first.
-- The host tool shotcheck.c decodes and verifies it.

local outfile = os.getenv('PSX_SHOT') or 'build/psx/shot.bin'
local target = tonumber(os.getenv('PSX_SHOT_FRAMES')) or 90

local count = 0
local done = false

-- The capture runs from nextTick, not from inside the vsync callback: taking
-- the screenshot while still on the event callback's stack, mid-frame, crashes
-- the software GPU. Running on the next main-loop tick, between frames, is
-- clean.
--
-- The listener is never removed from inside its own callback: redux deletes the
-- listener object there while it is still dispatching, a use-after-free that
-- crashes. The `done` flag makes later firings no-ops instead.
--
-- The script does not call PCSX.quit: quitting the emulator from Lua in -cli
-- mode crashes on teardown, while the guest's own exit (pcsx_exit in test mode)
-- is clean. So the sample must run past PSX_SHOT_FRAMES and then exit on its
-- own; the frame is already written to disk by the time it does. timeout in
-- the Makefile backstops a sample that never exits.
local function capture()
    local ss = PCSX.GPU.takeScreenShot()
    local f = Support.File.open(outfile, 'TRUNCATE')
    f:writeU16(ss.width)
    f:writeU16(ss.height)
    f:writeU16(ss.bpp)
    f:writeMoveSlice(ss.data)
    f:close()
end

PCSX.Events.createEventListener('GPU::Vsync', function()
    count = count + 1
    if count >= target and not done then
        done = true
        PCSX.nextTick(capture)
    end
end)
