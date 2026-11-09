REM psx.bas - the BASIC binding to the libpsx PlayStation SDK.
REM
REM A program reaches the PlayStation with `IMPORT psx` (dotted, psx.GpuInit) or
REM `FROM psx IMPORT GpuInit, SetBg, ...` (bare). Each routine is an EXTERNAL
REM declaration bound to a libpsx entry point; its INT arguments and results
REM cross at the C `int` width. This is the BASIC counterpart of units/psx.pas
REM (the Pascal block) and units/psx/*.kp (the Kobold leaves); it binds the
REM pointer-free scalar and integer-handle subset of libpsx/psx.h. The pointer
REM and struct entry points (TIM upload, raw GTE arrays, memory-card buffers,
REM debug-text strings) stay on the C side, reached through small C accessors
REM such as examples/psx/sprite_asset.c, exactly as the other languages do.
MODULE psx

REM ---- GPU and display ----
EXPORT DECLARE PROC GpuInit() EXTERNAL "psx_gpu_init"
EXPORT DECLARE PROC IrqInit() EXTERNAL "psx_irq_init"
EXPORT DECLARE PROC DmaInit() EXTERNAL "psx_dma_init"
EXPORT DECLARE PROC SetBg(r AS INT, g AS INT, b AS INT) EXTERNAL "psx_set_bg"
EXPORT DECLARE PROC DrawRect(x AS INT, y AS INT, w AS INT, h AS INT, r AS INT, g AS INT, b AS INT) EXTERNAL "psx_draw_rect"
EXPORT DECLARE PROC DrawRectAlpha(x AS INT, y AS INT, w AS INT, h AS INT, r AS INT, g AS INT, b AS INT, mode AS INT) EXTERNAL "psx_draw_rect_alpha"
EXPORT DECLARE PROC DrawPoly4(x0 AS INT, y0 AS INT, x1 AS INT, y1 AS INT, x2 AS INT, y2 AS INT, x3 AS INT, y3 AS INT, r AS INT, g AS INT, b AS INT) EXTERNAL "psx_draw_poly4"
EXPORT DECLARE PROC Vsync() EXTERNAL "psx_vsync"
EXPORT DECLARE PROC DisplaySwap() EXTERNAL "psx_display_swap"
EXPORT DECLARE FN VblankCount() AS INT EXTERNAL "psx_vblank_count"

REM Blend modes for DrawRectAlpha.
EXPORT CONST BlendHalf = 0
EXPORT CONST BlendAdd = 1
EXPORT CONST BlendSub = 2
EXPORT CONST BlendQuarter = 3

REM ---- textures by integer handle ----
REM The handle comes from a C accessor that calls psx_tex_upload on an embedded
REM TIM (the raw pointer stays in C); the draws take that handle.
EXPORT DECLARE PROC TexDraw(tex AS INT, x AS INT, y AS INT) EXTERNAL "psx_tex_draw"
EXPORT DECLARE PROC TexDrawUv(tex AS INT, x AS INT, y AS INT, u AS INT, v AS INT, w AS INT, h AS INT) EXTERNAL "psx_tex_draw_uv"
EXPORT DECLARE PROC TexRot(tex AS INT, cx AS INT, cy AS INT, angle AS INT, scale AS INT) EXTERNAL "psx_tex_rot"
EXPORT DECLARE PROC DebugInit() EXTERNAL "psx_debug_init"

REM ---- controller ----
EXPORT DECLARE FN PadRead() AS INT EXTERNAL "psx_pad_read"
EXPORT DECLARE FN PadId() AS INT EXTERNAL "psx_pad_id"
EXPORT DECLARE PROC PadInitAnalog() EXTERNAL "psx_pad_init_analog"
EXPORT DECLARE FN StickRx() AS INT EXTERNAL "psx_pad_stick_rx"
EXPORT DECLARE FN StickRy() AS INT EXTERNAL "psx_pad_stick_ry"
EXPORT DECLARE FN StickLx() AS INT EXTERNAL "psx_pad_stick_lx"
EXPORT DECLARE FN StickLy() AS INT EXTERNAL "psx_pad_stick_ly"

REM Button bits returned by PadRead (1 = pressed).
EXPORT CONST PadSelect = 1
EXPORT CONST PadStart = 8
EXPORT CONST PadUp = 16
EXPORT CONST PadRight = 32
EXPORT CONST PadDown = 64
EXPORT CONST PadLeft = 128
EXPORT CONST PadL2 = 256
EXPORT CONST PadR2 = 512
EXPORT CONST PadL1 = 1024
EXPORT CONST PadR1 = 2048
EXPORT CONST PadTriangle = 4096
EXPORT CONST PadCircle = 8192
EXPORT CONST PadCross = 16384
EXPORT CONST PadSquare = 32768

REM ---- fixed-point math (16.16) ----
EXPORT DECLARE FN Fmul(a AS INT, b AS INT) AS INT EXTERNAL "psx_fmul"
EXPORT DECLARE FN Fdiv(a AS INT, b AS INT) AS INT EXTERNAL "psx_fdiv"
EXPORT DECLARE FN Fsin(angle AS INT) AS INT EXTERNAL "psx_fsin"
EXPORT DECLARE FN Fcos(angle AS INT) AS INT EXTERNAL "psx_fcos"
EXPORT CONST FixOne = 65536

REM ---- sound (SPU) ----
EXPORT DECLARE PROC SpuInit() EXTERNAL "psx_spu_init"
EXPORT DECLARE PROC SpuPlay(voice AS INT, addr AS INT, pitch AS INT, vol AS INT) EXTERNAL "psx_spu_play"
EXPORT DECLARE PROC SpuStop(voice AS INT) EXTERNAL "psx_spu_stop"
EXPORT DECLARE FN SpuMakeAdsr(attack AS INT, decay AS INT, sustain AS INT, release AS INT) AS INT EXTERNAL "psx_spu_make_adsr"
EXPORT DECLARE PROC SpuPlayAdsr(voice AS INT, addr AS INT, pitch AS INT, vol AS INT, adsr AS INT) EXTERNAL "psx_spu_play_adsr"
EXPORT DECLARE PROC SpuReverb(preset AS INT) EXTERNAL "psx_spu_reverb"
EXPORT DECLARE PROC SpuReverbOff() EXTERNAL "psx_spu_reverb_off"
EXPORT DECLARE PROC SpuVoiceReverb(voice AS INT, on AS INT) EXTERNAL "psx_spu_voice_reverb"
EXPORT DECLARE PROC Beep() EXTERNAL "psx_beep"
EXPORT CONST SpuPitch44100 = 4096
EXPORT CONST SpuPitch22050 = 2048
EXPORT CONST ReverbHall = 0
EXPORT CONST ReverbRoom = 1

REM ---- CD-ROM bring-up and audio ----
EXPORT DECLARE PROC CdInit() EXTERNAL "psx_cd_init"
EXPORT DECLARE PROC CdAudioEnable(vol AS INT) EXTERNAL "psx_cd_audio_enable"
EXPORT DECLARE FN CdPlayTrack(track AS INT) AS INT EXTERNAL "psx_cd_play_track"
EXPORT DECLARE FN CdPlayXa(lba AS INT, file AS INT, channel AS INT) AS INT EXTERNAL "psx_cd_play_xa"
EXPORT DECLARE PROC CdAudioStop() EXTERNAL "psx_cd_audio_stop"
EXPORT DECLARE FN CdTrackCount() AS INT EXTERNAL "psx_cd_track_count"
EXPORT DECLARE FN CdStatus() AS INT EXTERNAL "psx_cd_status"

REM ---- root counters (hardware timers) ----
EXPORT DECLARE PROC TimerStart(counter AS INT, source AS INT) EXTERNAL "psx_timer_start"
EXPORT DECLARE FN TimerRead(counter AS INT) AS INT EXTERNAL "psx_timer_read"
EXPORT CONST TimerSysclk = 0
EXPORT CONST TimerHblank = 1
EXPORT CONST TimerDotclk = 1
EXPORT CONST TimerSysclk8 = 2

REM ---- 3D through the GTE (pointer-free helpers) ----
EXPORT DECLARE PROC GteSetScreen(h AS INT, ofx AS INT, ofy AS INT) EXTERNAL "psx_gte_set_screen"
EXPORT DECLARE PROC GteSetTrans(tx AS INT, ty AS INT, tz AS INT) EXTERNAL "psx_gte_set_trans"
EXPORT DECLARE PROC GteRotXyz(ax AS INT, ay AS INT, az AS INT) EXTERNAL "psx_gte_rot_xyz"
EXPORT DECLARE PROC GteProject(vx AS INT, vy AS INT, vz AS INT) EXTERNAL "psx_gte_project"
EXPORT DECLARE FN GteSx() AS INT EXTERNAL "psx_gte_sx"
EXPORT DECLARE FN GteSy() AS INT EXTERNAL "psx_gte_sy"
EXPORT DECLARE FN GteSz() AS INT EXTERNAL "psx_gte_sz"

REM ---- ordering table (depth-sorted drawing) ----
EXPORT DECLARE PROC OtClear() EXTERNAL "psx_ot_clear"
EXPORT DECLARE PROC OtAddPoly3(x0 AS INT, y0 AS INT, x1 AS INT, y1 AS INT, x2 AS INT, y2 AS INT, r AS INT, g AS INT, b AS INT, z AS INT) EXTERNAL "psx_ot_add_poly3"
EXPORT DECLARE PROC OtAddPoly4(x0 AS INT, y0 AS INT, x1 AS INT, y1 AS INT, x2 AS INT, y2 AS INT, x3 AS INT, y3 AS INT, r AS INT, g AS INT, b AS INT, z AS INT) EXTERNAL "psx_ot_add_poly4"
EXPORT DECLARE PROC OtAddGouraud3(x0 AS INT, y0 AS INT, c0 AS INT, x1 AS INT, y1 AS INT, c1 AS INT, x2 AS INT, y2 AS INT, c2 AS INT, z AS INT) EXTERNAL "psx_ot_add_gouraud3"
EXPORT DECLARE PROC OtAddGouraud4(x0 AS INT, y0 AS INT, c0 AS INT, x1 AS INT, y1 AS INT, c1 AS INT, x2 AS INT, y2 AS INT, c2 AS INT, x3 AS INT, y3 AS INT, c3 AS INT, z AS INT) EXTERNAL "psx_ot_add_gouraud4"
EXPORT DECLARE PROC OtAddTex4(handle AS INT, x0 AS INT, y0 AS INT, x1 AS INT, y1 AS INT, x2 AS INT, y2 AS INT, x3 AS INT, y3 AS INT, z AS INT) EXTERNAL "psx_ot_add_tex4"
EXPORT DECLARE PROC OtLink() EXTERNAL "psx_ot_link"
