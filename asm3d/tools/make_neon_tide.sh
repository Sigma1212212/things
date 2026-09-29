#!/bin/sh
# Rebuilds the Neon Tide game scene (games/NeonTide/Assets/Scenes/Main.a3scene)
# with asm3d_cli. The city itself is generated when the game starts (City
# component), so the scene stays small.
#
#   tools/make_neon_tide.sh [path/to/asm3d_cli]
set -e
CLI=${1:-build/asm3d_cli}
G=games/NeonTide
S=$G/Assets/Scenes/Main.a3scene
q() { "$CLI" "$@" > /dev/null || { echo "failed: $*" >&2; "$CLI" "$@"; exit 1; }; }

if [ ! -f $G/project.a3proj ]; then
    TMP=$(mktemp -d)
    q project new "$TMP/NeonTide" --name "Neon Tide"
    cp "$TMP/NeonTide/project.a3proj" "$TMP/NeonTide/.gitignore" $G/
    rm -rf "$TMP"
fi
mkdir -p $G/Assets/Scenes
rm -f $S
q scene new $S --empty
q entity add $S "World Settings" --with WorldSettings
q entity add $S "Sun" --rotation -38,120,0 --with Light
q entity set $S "Sun" Light.type Directional Light.cast_shadows true
q entity add $S "Sol Harbor" --with City
q entity set $S "Sol Harbor" City.seed 1 City.time Night
q entity add $S "City Traffic" --with Traffic
q entity set $S "City Traffic" Traffic.roads generated Traffic.cars 70 Traffic.pedestrians 140 Traffic.seed 3

q entity add $S Player --at 406.8,0.7,54 --rotation 0,90,0 --with CharacterController
q entity set $S Player CharacterController.camera_mode "Third Person" CharacterController.camera_distance 4.5 CharacterController.walk_speed 4 CharacterController.sprint_speed 8
q entity add $S Body --parent Player --at 0,0.9,0 --scale 0.5,0.9,0.38 --primitive capsule
q entity set $S Player/Body MeshRenderer.base_color 0.95,0.3,0.6,1 MeshRenderer.roughness 0.6
q entity add $S Head --parent Player --at 0,1.68,0 --scale 0.26,0.26,0.26 --primitive sphere
q entity set $S Player/Head MeshRenderer.base_color 0.8,0.58,0.42,1
# the Character Controller added "Player Camera"; the game script looks for "Main Camera"
q entity rename $S "Player/Player Camera" "Main Camera"
q entity set $S "Player/Main Camera" Camera.primary true Camera.far_plane 3000 Camera.fov 62 Camera.near_plane 0.1

q entity add $S Checkpoint --at 0,30,0 --scale 5,60,5 --primitive cylinder
q entity set $S Checkpoint MeshRenderer.material builtin:neon MeshRenderer.base_color 0.2,1,1,1 MeshRenderer.cast_shadows false
q entity add $S "Lightbar Template" --scale 1.3,0.14,0.32 --primitive cube
q entity set $S "Lightbar Template" MeshRenderer.material builtin:neon MeshRenderer.base_color 1,0.1,0.1,1 active false
q entity add $S Game --script Assets/Scripts/Game.a3script
q validate $G
echo "Neon Tide scene written to $S"
