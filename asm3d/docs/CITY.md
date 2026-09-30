# City, vehicles and traffic

ASM3D can generate a whole city, fill it with traffic and let the player
drive. The example game **Neon Tide** (`games/NeonTide`) is built on these
pieces.

## Sol Harbor, the procedural city

`engine/world/a3_citygen.c` builds **Sol Harbor**, a fictional coastal city.
Its layout is loosely inspired by Miami's geography; it is not a map of
the real city, and no real names, brands or buildings are used:

| Area | What is there |
|---|---|
| Mainland (west) | Glass towers downtown next to the bay (taller toward the water), a colorful low-rise district with shop neon in the west, warehouses to the north, mid-rise blocks, parks |
| Bayfront | A boulevard with palms along the water |
| The bay | Three causeways with lamp posts, a chain of small islands with villas, boats |
| Barrier island (east) | A beach avenue and a beachfront drive with pastel Art Deco hotels, neon trim and vertical signs, condo towers further north, a palm park, a wide beach with lifeguard towers and umbrellas, the ocean |
| Port | Container stacks and cranes with red beacons |

+X is east, -Z is north, 1 unit = 1 meter. The city is about 1.6 x 1.6 km,
roughly 6,000 objects including ~800 lamp posts with real point lights. It
generates in about 0.1 s from a seed, so scenes can hold it in a few lines.

Everything is ordinary objects: boxes, cylinders, procedural meshes
(`builtin:palm_crown`, `builtin:car_body`, `builtin:car_glass`,
`builtin:wheel`) and the builtin procedural materials (`builtin:building`,
`artdeco`, `tower`, `road`, `sidewalk`, `sand`, `water`, `glass`, `neon`,
`carpaint`, `palm_trunk`, `foliage`, `metal`). The materials compute windows,
lane markings, sidewalk joints, waves and so on from the world position, so
the city needs no texture files, and lit windows and neon switch on when the
sun goes down.

Three ways to get a city:

- **City component** (World category): generates the city when the game
  starts. Fields: Seed, Density, Time of Day (Day / Sunset / Night), Street
  Lights, Neon.
- **Command line:** `asm3d_cli world city <project> [--seed 1] [--time night]
  [--cars 40] [--pedestrians 60] [--startup]` writes
  `Assets/Scenes/City.a3scene` (about 5 MB) and `Assets/City/roads.json`.
- **C API:** `a3_city_generate(world, &desc, &stats, &roads_json)` and
  `a3_city_apply_time(world, A3_CITY_SUNSET)`.

The road graph (`asm3d.roads` JSON) lists intersections as nodes and roads
as two-way edges with a lane count. Nearly every node is in one connected
network; the tests check it.

## Vehicles

The **Vehicle** component (`engine/physics/a3_vehicle.c`) has two physics
models, picked with the **Physics** field.

### Realistic (default)

A rigid body with 6 degrees of freedom on four raycast wheels, stepped 4
times per frame:

- **suspension**: one ray per wheel along the body's down axis; spring and
  damper per wheel (springs sized to each axle's share of the weight, so the
  car sits level), anti-roll bars (stiffer at the front) and bump stops. The
  wheels move up and down with it.
- **weight transfer is not faked**: tire forces act at the contact patches,
  below the center of mass, so the body squats when accelerating, dives
  under braking and rolls out of corners, and the loads on the tires change
  accordingly.
- **tires**: the slip angle goes through a Pacejka-style curve (peak grip
  near 8 degrees, about 85% when fully sliding) and a friction circle shared
  with traction and braking, so too much throttle or brake in a corner makes
  the car slide. More heavily loaded tires grip less per newton (load
  sensitivity). The handbrake locks the rear wheels.
- **drivetrain**: engine torque curve with a rev limiter, clutch slip when
  pulling away, 6-speed automatic gearbox, rear / front / all-wheel drive,
  engine braking, brakes with front bias and ABS. The gearing is chosen so
  the car reaches **Top Speed** at the redline in top gear, and the drag area
  so the engine's power runs out there.
- **Stability Control** (on by default): traction control keeps cornering
  grip first and gives the engine what is left; stability control applies a
  yaw moment (like braking one outer wheel) and cuts power when the car
  rotates faster than the steering asks. Off while the handbrake is held, so
  drifts still work.
- **steering** is speed-sensitive: at speed, full lock asks for about the
  angle of a 1 g turn plus the tires' peak slip angle.
- weight distribution follows the layout (front-drive 62/38, all-wheel
  56/44, rear-drive 52/48).

The per-wheel math (suspension load, slip angle, tire curve, friction
circle, slip amount) runs for all realistic cars in one call per substep to
the x86-64 SSE assembly kernel `a3_vk_wheels` (`engine/physics/a3_vehicle_x64.S`,
one SSE lane per wheel). `a3_vehicle_ref.c` is its C reference (used for
WebAssembly); a test checks the two give bit-identical results. The ray
casts, the drivetrain logic and the rigid-body integration are C.

`spawn_car` tunes each body style: sedan (rear drive, 1450 kg, 360 N m),
sports (rear, 1350 kg, 540 N m, stiff springs), SUV (all-wheel, 2050 kg, soft
and tall), hatch and taxi (front drive), police (rear, 500 N m). Read-only
fields show **Rpm**, **Gear**, **Wheel Slip** and **Body Roll**.

Every car spawned with `spawn_car` has an **Engine Sound** (a synthesized
4-cylinder loop whose pitch follows the rpm and whose volume follows the
throttle) and a **Tire Sound** (squeal that fades in with wheel slip).
Sliding tires smoke (two particle emitters at the rear wheels, emission
following each tire's slip) and leave skid marks on the road (thin dark
strips from a recycled pool of 320, so the oldest marks disappear). Traffic
cars have no sound loops or smoke emitters (there are dozens of them).

### Arcade

The simple kinematic model, used by the traffic AI and scripted chases:
throttle/brake/reverse with a speed-dependent acceleration curve,
bicycle-model steering with a cornering limit of about 2 g, grip that pulls
the velocity toward the heading (the handbrake lowers it, so the car drifts),
two ground rays for height and pitch.

### Both

- the body box is pushed out of walls; the speed into the wall is removed
  and reported as `last_impact`, hard hits add to `damage`; dynamic objects
  are shoved aside;
- the wheels spin and the front wheels steer; a chase camera follows the
  player's car and keeps out of buildings;
- moving the car more than 3 m in one step (a script teleport) resets it.

Player Controlled cars read W/S (throttle/brake; holding brake at a stop
selects reverse), A/D (steer) and Space (handbrake). Otherwise scripts or
the traffic system write `throttle`, `steer` and `handbrake`.
`spawn_car(name, position, yaw, color, style)` in A3Script and
`a3_vehicle_spawn_car_style` in C build a complete car.

Not simulated: wheel spin as its own degree of freedom (wheelspin is the
friction circle saturating), tire temperature and wear, clutch/manual
gears, damage that changes the car's shape.

## Traffic

The **Traffic** component (`engine/world/a3_traffic.c`) spawns AI cars and
pedestrians when the game starts. Roads = `generated` uses the City
component's road graph; a path uses a JSON file.

- Cars drive on the right, pick a random way at each intersection (no U-turn
  unless it is a dead end), slow down for corners, and brake for anything
  ahead of them: other cars, the player's car, pedestrians and characters.
  A car that has waited for 4 s creeps on, so intersections never lock up.
- Pedestrians walk the sidewalks, turn at corners and cross at the ends of
  blocks.

Script helpers: `road_point(center, min, max)` (a random point on a road,
used for mission targets and police spawns), `nearest_road(position)` and
`city_time("night")`.

Not yet: traffic lights, lane changes, pedestrians reacting to cars,
navigation meshes for off-road AI.

## Neon Tide

`games/NeonTide` is a small open-world game in Sol Harbor at night:

- walk (WASD, Shift, mouse look), take any car with **E**, get out with E;
- courier jobs: reach the cyan beam before the timer runs out for cash and
  a time bonus; streaks and cash are saved between runs;
- crashes raise a wanted level; police cars (with flashing light bars) chase
  you; get far away for a while to lose them, or get busted if they box you
  in;
- HUD: cash, wanted stars, job timer and distance, speedometer, a radar with
  traffic, police and the target;
- driving into the sea tows the car back to the road.

The game logic is one script: `Assets/Scripts/Game.a3script`. The scenes
are rebuilt from the command line by `tools/make_neon_tide.sh`.

### Trailer

`tools/make_trailer.sh` renders the trailer: the Trailer scene has a
scripted camera (`Assets/Scripts/Director.a3script`) with seven shots (a
sunset flyover, the neon beachfront drive, a car chase, a police pursuit,
downtown at night, the bay, the title card). `asm3d_cli screenshot --record`
saves every frame, `tools/trailer_music.py` synthesizes a soundtrack, and
ffmpeg encodes the MP4. Everything in the video is rendered in real time by
the engine (with Mesa's software OpenGL on a server it takes about half a
second per frame to capture).

## Rendering the city

The look comes from the renderer's screen-space effects on OpenGL 3.3:
SSAO, screen-space reflections (wet roads, water, glass), exponential
height fog with a sun glow, bloom, color grading, ACES tone mapping and
FXAA, plus up to 8 point lights per object out of up to 1024 per frame.
This is not hardware ray tracing: reflections only show what is on screen
and fall back to the sky gradient.
