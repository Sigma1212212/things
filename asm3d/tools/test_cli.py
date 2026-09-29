#!/usr/bin/env python3
"""End-to-end test of asm3d_cli: every response must be one valid JSON object
with the documented shape, and a scripted project must validate, simulate
and build.

    python3 tools/test_cli.py build/asm3d_cli
    python3 tools/test_cli.py "wine build-win/asm3d_cli.exe"      (Windows build)
"""
import json, os, shlex, shutil, subprocess, sys, tempfile

cli = shlex.split(sys.argv[1] if len(sys.argv) > 1 else "build/asm3d_cli")
cli = [os.path.abspath(c) if os.path.exists(c) else c for c in cli]
tmp = tempfile.mkdtemp(prefix="asm3d_cli_test_")
failures = 0
checks = 0


def run(*args, stdin=None, expect_ok=True):
    global failures, checks
    p = subprocess.run(cli + list(args), input=stdin, capture_output=True, text=True, cwd=tmp, timeout=300)
    lines = [l for l in p.stdout.splitlines() if l.strip()]
    out = []
    for l in lines:
        try:
            out.append(json.loads(l))
        except Exception:
            print("NOT JSON:", l[:200])
            failures += 1
    checks += 1
    if not out:
        print("FAIL no output for", args, p.stderr[-500:])
        failures += 1
        return {}
    r = out[-1] if stdin is None else out
    if stdin is None:
        for k in ("ok", "command", "log"):
            if k not in r:
                print("FAIL missing key", k, "in", args)
                failures += 1
        if r.get("ok") != expect_ok:
            print("FAIL", args, "expected ok =", expect_ok, "got", json.dumps(r)[:400])
            failures += 1
        if expect_ok and p.returncode != 0 or (not expect_ok and p.returncode == 0):
            print("FAIL exit code", p.returncode, "for", args)
            failures += 1
    return r


def check(cond, what):
    global failures, checks
    checks += 1
    if not cond:
        print("FAIL:", what)
        failures += 1


try:
    h = run("help")
    check(len(h["result"]["commands"]) >= 20, "help lists commands")
    check(run("version")["result"]["version"], "version")
    comps = run("components")["result"]["components"]
    check(any(c["name"] == "RigidBody" for c in comps), "components lists RigidBody")
    light = run("components", "Light")["result"]["component"]
    check(any(f["name"] == "type" and "options" in f for f in light["fields"]), "Light.type has options")
    api = run("api")["result"]["functions"]
    check(len(api) >= 90 and any(f["name"] == "raycast" for f in api), "api lists functions")

    r = run("project", "new", "Game", "--name", "Test Game")
    check(r["result"]["name"] == "Test Game", "project name")
    scene = "Game/Assets/Scenes/Main.a3scene"
    run("project", "new", "Game", expect_ok=False)  # already exists
    r = run("entity", "add", scene, "Crate", "--at", "0,3,0", "--primitive", "cube", "--with", "Collider,RigidBody")
    guid = r["result"]["object"]["guid"]
    check(len(guid) == 16, "guid returned")
    r = run("entity", "set", scene, "Crate", "RigidBody.mass", "5", "Collider.size", "1,1,1", "MeshRenderer.base_color", "[1,0.5,0,1]")
    check(r["result"]["object"]["components"]["RigidBody"]["mass"] == 5, "mass set")
    check(run("entity", "get", scene, guid, "RigidBody.mass")["result"]["value"] == 5, "get by guid")
    e = run("entity", "get", scene, "Crat", expect_ok=False)
    check("Crate" in e.get("hint", ""), "suggestion for misspelled object")
    e = run("entity", "set", scene, "Crate", "RigidBody.mas", "1", expect_ok=False)
    check("mass" in e.get("hint", ""), "suggestion for misspelled field")
    run("entity", "add", scene, "Lid", "--parent", "Crate", "--at", "0,0.6,0")
    check(run("entity", "get", scene, "Crate/Lid")["result"]["object"]["name"] == "Lid", "path lookup")
    run("entity", "duplicate", scene, "Crate", "--name", "Crate2", "--at", "3,3,0")
    run("entity", "rename", scene, "Crate2", "Box")
    run("component", "add", scene, "Box", "Light")
    run("component", "remove", scene, "Box", "Light")
    run("entity", "remove", scene, "Box")
    lst = run("entity", "list", scene)["result"]["objects"]
    names = [o["name"] for o in lst]
    check("Crate" in names and "Lid" in names and "Box" not in names, "entity list")
    lid = [o for o in lst if o["name"] == "Lid"][0]
    check(lid["depth"] == 1, "hierarchy depth")
    # dry run leaves the file unchanged
    run("entity", "set", scene, "Crate", "RigidBody.mass", "99", "--dry-run")
    check(run("entity", "get", scene, "Crate", "RigidBody.mass")["result"]["value"] == 5, "dry run does not save")

    # scripts
    os.makedirs(os.path.join(tmp, "Game/Assets/Scripts"), exist_ok=True)
    with open(os.path.join(tmp, "Game/Assets/Scripts/Lift.a3script"), "w") as f:
        f.write("let lifts = 0\nfn on_update(dt) {\n    lifts += 1\n    if key_down(\"space\") { self.position = vec3(0, 10, 0) }\n    hud_text(\"lifts \" + lifts, 10, 10)\n}\n")
    with open(os.path.join(tmp, "bad.a3script"), "w") as f:
        f.write("fn f() {\n  retrun 1\n}\n")
    run("entity", "add", scene, "Lifter", "--script", "Assets/Scripts/Lift.a3script")
    c = run("script", "check", "Game/Assets/Scripts/Lift.a3script")
    check(c["result"]["failed"] == 0, "script check ok")
    c = run("script", "check", "bad.a3script", expect_ok=False)
    check(c["error"].startswith("1 of 1"), "script check failure")
    ev = run("script", "eval", "clamp(15, 0, 10) + len(\"abc\")")
    check(ev["result"]["return"] == 13, "eval")
    with open(os.path.join(tmp, "calc.a3script"), "w") as f:
        f.write("let total = 1\nprint(\"hello\")\nfn add(a, b) { total = a + b\n return total }\n")
    sr = run("script", "run", "calc.a3script", "--call", "add", "--args", "[2, 40]")
    check(sr["result"]["return"] == 42 and sr["result"]["globals"]["total"] == 42 and sr["result"]["output"] == ["hello"], "script run")
    er = run("script", "eval", "1 / 0", expect_ok=False)
    check("division by zero" in er["error"], "runtime error reported")

    v = run("validate", "Game")
    check(v["result"]["buildable"] and v["result"]["scripts"] == 1, "validate")
    sim = run("simulate", "Game", "--frames", "60", "--watch", "Crate,Lifter", "--keys", "space@30-31")
    res = sim["result"]
    check(res["objects"]["Crate"]["position"][1] < 2.9, "crate fell")
    check(abs(res["objects"]["Lifter"]["position"][1] - 10) < 1e-4, "key input reached the script")
    check(res["hud_text"] == ["lifts 60"], "hud text")
    check(res["script_errors"] == [], "no script errors")
    b = run("build", "Game", "--out", os.path.join(tmp, "out"))
    exe = "Test_Game.exe" if "wine" in cli[0] else "Test_Game"
    check(os.path.exists(os.path.join(tmp, "out", exe)), "built executable")
    check(os.path.exists(os.path.join(tmp, "out", "data", "Assets", "Scripts", "Lift.a3script")), "packaged script")

    # a broken script blocks the build
    shutil.copy(os.path.join(tmp, "bad.a3script"), os.path.join(tmp, "Game/Assets/Scripts/Bad.a3script"))
    v = run("validate", "Game")
    check(not v["result"]["buildable"] and v["result"]["issues"][0]["line"] == 2, "validate reports script line")
    run("build", "Game", expect_ok=False)

    # batch: one JSON line per command
    lines = run("batch", stdin="version\n# comment\nentity list \"%s\"\nnope\n" % scene)
    check(len(lines) == 3 and lines[0]["ok"] and lines[1]["ok"] and not lines[2]["ok"], "batch")
    run("unknown", expect_ok=False)
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print("asm3d_cli: %d checks, %d failures" % (checks, failures))
sys.exit(1 if failures else 0)
