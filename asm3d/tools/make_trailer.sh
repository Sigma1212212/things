#!/bin/sh
# Renders the Neon Tide trailer: records every frame of the Trailer scene
# (scripted camera, Assets/Scripts/Director.a3script) with asm3d_cli, makes
# the soundtrack with tools/trailer_music.py and encodes an H.264 video with
# ffmpeg (an offline tool only; the engine does not link it).
#
#   tools/make_trailer.sh [out.mp4] [asm3d_cli] [ffmpeg]
#
# Needs an OpenGL 3.3 context (a desktop, or Xvfb + Mesa). ~1500 frames.
set -e
OUT=${1:-NeonTide_Trailer.mp4}
CLI=${2:-build/asm3d_cli}
FFMPEG=${3:-ffmpeg}
WORK=${WORK:-$(mktemp -d)}
FPS=30
LEAD=2          # seconds of city life before the first shot (Director.lead)
LENGTH=47       # Director.starts[-1]
FRAMES=$(( (LEAD + LENGTH) * FPS ))
echo "recording $FRAMES frames into $WORK/frames ..."
"$CLI" screenshot games/NeonTide --scene Assets/Scenes/Trailer.a3scene --size 1280x720 \
    --dt 0.0333333 --frames $FRAMES --record-from $(( LEAD * FPS )) --record "$WORK/frames" > "$WORK/record.json"
python3 tools/trailer_music.py "$WORK/music.wav" $LENGTH
"$FFMPEG" -y -loglevel error -framerate $FPS -i "$WORK/frames/frame_%05d.png" -i "$WORK/music.wav" \
    -c:v libx264 -preset slow -crf 19 -pix_fmt yuv420p -c:a aac -b:a 192k -shortest -movflags +faststart "$OUT"
echo "trailer written to $OUT"
