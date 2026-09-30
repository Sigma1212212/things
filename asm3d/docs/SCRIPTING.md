# A3Script

A3Script is the scripting language of ASM3D. It is small, readable and safe:
scripts compile to bytecode for a stack virtual machine, can only call the
functions listed below, and cannot freeze or crash the game.

```
let speed = 4                           // top-level variables belong to the object

fn on_start() {
    print("Hello from " + self.name)
}

fn on_update(dt) {
    if key_down("space") and self.position.y < 3 {
        self.position += vec3(0, speed * dt, 0)
    }
    self.Light.intensity = 1 + sin(time() * 4) * 0.5
}

fn on_trigger_enter(other) {
    if other.name == "Player" { play_sound("coin"); destroy(self) }
}
```

## Using scripts

1. Select an object, **Add Component > Script**, press **Create New Script**.
2. The file opens in the Code panel. Problems are marked on the line while you
   type; hover the red dot for the explanation.
3. Press Play. Saving a script while playing reloads it on every object that
   uses it and keeps the values of its top-level variables.

Each object runs its own instance, so `let` variables at the top of the file
are per object. When a script fails, the Console shows the file, line,
function, object and a plain explanation (often with a "Did you mean ...?"
suggestion). Only that object's script stops; fixing the file restarts it.

## Events

| Function | When it runs |
|---|---|
| `on_start()` | Once, before the first `on_update` |
| `on_update(dt)` | Every frame (`dt` = seconds since the last frame) |
| `on_fixed_update(dt)` | Every physics step (60 per second) |
| `on_trigger_enter(other)` | Another object (or the player) entered this object's trigger collider |
| `on_trigger_exit(other)` | It left the trigger |
| `on_collision(other)` | This object started touching another one |

Every event is optional. Parameters may be left out (`fn on_update() { }` is fine).

## The language

| Feature | Example |
|---|---|
| Numbers, text, true/false, nil | `3`, `2.5`, `1e6`, `1_000`, `"hi"`, `'hi'`, `true`, `nil` |
| Vectors | `vec3(1, 2, 3)`, `v.x`, `v * 2`, `a + b`, `v[0]` |
| Lists | `[1, 2, 3]`, `l[0]`, `l[-1]` (last), `push(l, 4)`, `len(l)` |
| Variables | `let x = 1`, `x += 2` (also `-=`, `*=`, `/=`) |
| Math | `+ - * / %` (`%` keeps the sign of the divisor), `==  !=  <  <=  >  >=` |
| Logic | `and`, `or`, `not` (also `&&`, `\|\|`, `!`); `a or b` returns the first true value |
| Conditions | `if x > 1 { } else if x < 0 { } else { }` |
| Loops | `while x < 10 { }`, `for i in 0..10 { }` (10 excluded), `for item in list { }`, `break`, `continue` |
| Functions | `fn add(a, b) { return a + b }` (top level only; can be called before they are written) |
| Comments | `// line`, `# line`, `/* block */` |
| Objects | `self`, `find("Door")`, `obj.position`, `obj.RigidBody.mass = 2` |

Truthiness: `nil`, `false`, `0`, `""` and `[]` count as false; everything else is true.
Lists are shared (assigning a list does not copy it; use `copy(list)`).
Vectors are values (changing `v.x` of a copy does not change the original).

### Objects and components

| Expression | Meaning |
|---|---|
| `obj.position` | World position (vec3). Assigning moves the object. |
| `obj.local_position` | Position relative to the parent |
| `obj.rotation` | Rotation in degrees (pitch, yaw, roll) |
| `obj.scale` | Size multiplier |
| `obj.forward`, `obj.right`, `obj.up` | Direction vectors (read only) |
| `obj.name`, `obj.active` | Name, and whether it is enabled |
| `obj.velocity` | RigidBody velocity (m/s) |
| `obj.parent` | Parent object, or nil (assignable) |
| `obj.Light` (any component name) | The component; then `.field` reads or writes its settings |
| `obj.health` (a script variable) | Top-level variable of the object's own script |

Component fields use the names shown in the Docs panel's Component Reference
(for example `self.Light.intensity`, `self.Camera.fov`, `self.RigidBody.mass`).
Colors are vec3 (r, g, b from 0 to 1). Option lists (like a Light's type)
accept their name as text: `self.Light.type = "Spot"`.

### Safety limits

- A call may run 5,000,000 instructions; an endless loop stops that script with
  an explanation instead of freezing the game.
- 128 nested calls (runaway recursion is reported), 200 local variables per function.
- Scripts can only reach the functions below (no files, network or OS access).
  `save_value` writes one file, `saves.a3save`, in the game folder.

### Not in the language yet

Dictionaries/maps, closures, classes and methods, string formatting beyond
`+` and `format_number`, and a debugger with breakpoints.

## Built-in functions

Also listed in the editor under **Docs > Script API** (102 functions). `pi` and `tau` are
constants.

### Basics

| Function | What it does |
|---|---|
| `print(a, b, ...)` | Writes values to the console. |
| `str(x)` | Turns any value into text. |
| `num(text)` | Turns text like "3.5" into a number (nil if it is not a number). |
| `type(x)` | The kind of value: "number", "text", "list", "vec3", "bool", "object"... |
| `assert(condition, message)` | Stops the script with an error if the condition is false. |

### Math

| Function | What it does |
|---|---|
| `sin(radians)` | Sine of an angle in radians. |
| `cos(radians)` | Cosine of an angle in radians. |
| `tan(radians)` | Tangent of an angle in radians. |
| `asin(x)` | Inverse sine, in radians. |
| `acos(x)` | Inverse cosine, in radians. |
| `atan(x)` | Inverse tangent, in radians. |
| `atan2(y, x)` | Angle of the direction (x, y), in radians. |
| `sqrt(x)` | Square root. |
| `pow(x, y)` | x to the power y. |
| `exp(x)` | e to the power x. |
| `log(x)` | Natural logarithm. |
| `abs(x)` | Distance from zero (works on vec3 too). |
| `floor(x)` | Rounds down. |
| `ceil(x)` | Rounds up. |
| `round(x, decimals)` | Rounds to the nearest number (optionally keeping some decimals). |
| `sign(x)` | -1, 0 or 1. |
| `min(a, b, ...)` | The smallest number (also accepts one list). |
| `max(a, b, ...)` | The largest number (also accepts one list). |
| `clamp(x, lo, hi)` | Keeps x between lo and hi. |
| `lerp(a, b, t)` | Blends from a (t = 0) to b (t = 1). Works on numbers and vec3. |
| `move_toward(x, target, step)` | Moves x toward target by at most step. |
| `radians(degrees)` | Converts degrees to radians. |
| `degrees(radians)` | Converts radians to degrees. |
| `random() or random(lo, hi)` | A random number from 0 to 1, or between lo and hi. |
| `random_int(lo, hi)` | A random whole number from lo to hi (both included). |
| `random_seed(n)` | Makes the random numbers repeat the same way every run. |

### Vectors

| Function | What it does |
|---|---|
| `vec3(x, y, z)` | A 3D vector (position, direction or color). vec3(s) fills all three. |
| `length(v)` | Length of a vec3. |
| `normalize(v)` | The same direction with length 1. |
| `dot(a, b)` | Dot product of two vec3 values. |
| `cross(a, b)` | Cross product of two vec3 values. |
| `distance(a, b)` | Distance between two points. |

### Lists and text

| Function | What it does |
|---|---|
| `len(x)` | Number of items in a list, or letters in a text. |
| `list(count, fill)` | A new list with count copies of fill. |
| `range(lo, hi, step)` | A list of numbers from lo up to (not including) hi. |
| `push(list, value)` | Adds a value at the end of a list. |
| `pop(list)` | Removes and returns the last item (nil if empty). |
| `insert(list, index, value)` | Inserts a value at a position. |
| `remove_at(list, index)` | Removes and returns the item at a position. |
| `remove(list, value)` | Removes the first matching item. Returns true if found. |
| `contains(list_or_text, value)` | True if the value is in the list (or the text contains it). |
| `index_of(list_or_text, value)` | Position of the value, or -1. |
| `clear(list)` | Removes every item. |
| `copy(list)` | A new list with the same items (lists are shared otherwise). |
| `join(list, separator)` | Joins items into one text. |
| `split(text, separator)` | Splits text into a list. |
| `upper(text)` | UPPER CASE copy. |
| `lower(text)` | lower case copy. |
| `substr(text, start, count)` | Part of a text. |
| `trim(text)` | Removes spaces at both ends. |
| `starts_with(text, prefix)` | True if text begins with prefix. |
| `format_number(x, decimals)` | Text with a fixed number of decimals, e.g. "3.50". |

### Objects

| Function | What it does |
|---|---|
| `find(name)` | The object with this name, or nil. |
| `find_all(name_start)` | A list of every object whose name starts with this text. |
| `find_with(component)` | A list of every object that has this component, e.g. find_with("Light"). |
| `exists(obj)` | True if the object is still in the scene. |
| `spawn(name, position)` | Creates an empty object (add components with add_component). |
| `clone(obj, position)` | Copies an object with all its components and children. |
| `destroy(obj)` | Removes an object at the end of the frame. |
| `add_component(obj, name)` | Adds a component, e.g. add_component(self, "RigidBody"). |
| `has_component(obj, name)` | True if the object has this component. |
| `remove_component(obj, name)` | Removes a component. |
| `look_at(obj, target)` | Turns obj to face a position or another object. |
| `send(obj, "function", values...)` | Calls a function in another object's script. |

### Input

| Function | What it does |
|---|---|
| `key_down("space")` | True while the key is held. |
| `key_pressed("e")` | True on the frame the key goes down. |
| `key_released("e")` | True on the frame the key goes up. |
| `action_down("jump")` | True while an input action is held (Project Settings > Input). |
| `action_pressed("fire")` | True on the frame the action starts. |
| `action_released("fire")` | True on the frame the action ends. |
| `action_value("move_x")` | Axis value from -1 to 1. |
| `mouse_down(button)` | True while a mouse button is held (0 left, 1 right, 2 middle). |
| `mouse_pressed(button)` | True on the frame the button goes down. |
| `mouse_released(button)` | True on the frame the button goes up. |
| `mouse_position()` | Mouse position in HUD units (1280 x 720). |
| `mouse_delta()` | Mouse movement this frame, in pixels. |

### Game

| Function | What it does |
|---|---|
| `time()` | Seconds since the game started. |
| `delta_time()` | Seconds since the last frame. |
| `play_sound(sound, volume, pitch)` | Plays a .wav file or a built-in sound such as "coin" or "jump". |
| `burst(obj)` | Restarts the ParticleEmitter of an object (great with the Explosion preset). |
| `play_animation(obj)` | Plays the Animator of an object from the start. |
| `stop_animation(obj)` | Stops the Animator of an object. |
| `load_scene("Scenes/Level2.a3scene")` | Switches to another scene after this frame. |
| `save_value(key, value)` | Remembers a number, text or true/false between game sessions. |
| `load_value(key, default)` | A saved value, or the default when nothing was saved. |

### Physics

| Function | What it does |
|---|---|
| `add_impulse(obj, force)` | Pushes a RigidBody, e.g. add_impulse(self, vec3(0, 5, 0)). |
| `raycast(origin, direction, max_distance)` | The first object hit by a ray, or nil (ignores the object running the script). |
| `raycast_hit(origin, direction, max_distance)` | [object, point, normal, distance] of the first hit, or nil. |
| `overlap_sphere(center, radius)` | A list of objects with colliders inside a sphere. |

### City

| Function | What it does |
|---|---|
| `spawn_car(name, position, yaw, color, style)` | Creates a drivable car (Vehicle) facing yaw degrees; style is sedan, sports, suv, hatch, taxi or police. Set car.Vehicle.use_input = true to drive it. |
| `road_point(center, min_distance, max_distance)` | A random point on a road of the city, between the distances from center (nil without roads). |
| `nearest_road(position)` | [point, direction] of the closest road center line, or nil. |
| `city_time("night")` | Changes the sky, sun and look to "day", "sunset" or "night". |
| `weather(0.8)` | Rain from 0 (dry) to 1 (downpour): wet streets and puddles, rain clouds, lightning in heavy rain, less tire grip. |

### HUD

| Function | What it does |
|---|---|
| `hud_text(text, x, y, size, color, alpha, align)` | Draws text this frame. The screen is 1280 x 720 units; align is "left", "center" or "right". |
| `hud_rect(x, y, width, height, color, alpha)` | Draws a filled rectangle this frame. |
| `hud_bar(x, y, width, height, fraction, color)` | Draws a bar filled from 0 to 1 (health, stamina...). |
