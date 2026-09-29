/*
 * ASM3D - test_ecs.c : JSON, ECS, reflection, scene serialization
 */
#include "a3_test.h"
#include "../engine/core/a3_string.h"
#include "../engine/core/a3_json.h"
#include "../engine/ecs/a3_ecs.h"
#include "../engine/scene/a3_components.h"
#include "../engine/scene/a3_scene_io.h"

static void setup(void) { a3_register_core_components(); }

A3_TEST(json_parse_write) {
    const char *src = "{ // comment\n \"a\": 1.5, \"b\": [1, 2, 3,], \"s\": \"he said \\\"hi\\\"\\n\", \"t\": true, \"n\": null, \"o\": {\"x\": -2e2} }";
    A3Arena ar;
    a3_arena_init(&ar, A3_MEM_TEMP, 4096);
    A3JsonError err;
    A3Json *root = a3_json_parse(src, a3_strlen(src), &ar, &err);
    A3_CHECK(root != 0);
    A3_CHECK_NEAR(a3_json_get_number(root, "a", 0), 1.5, 0);
    A3_CHECK_EQ_INT(a3_json_count(a3_json_get(root, "b")), 3);
    A3_CHECK_STR(a3_json_get_string(root, "s", ""), "he said \"hi\"\n");
    A3_CHECK(a3_json_get_bool(root, "t", 0));
    A3_CHECK(a3_json_get(root, "n")->type == A3_JSON_NULL);
    A3_CHECK_NEAR(a3_json_get_number(a3_json_get(root, "o"), "x", 0), -200, 0);
    /* round trip */
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_TEMP);
    A3JsonWriter jw;
    a3_jw_init(&jw, &sb, 0);
    a3_jw_node(&jw, root);
    A3Json *again = a3_json_parse(sb.data, sb.len, &ar, &err);
    A3_CHECK(again && a3_streq(a3_json_get_string(again, "s", ""), "he said \"hi\"\n"));
    a3_strbuf_clear(&sb);
    a3_jw_init(&jw, &sb, 1);
    f32 v[3] = { 0.1f, 1.0f, -3.25f };
    a3_jw_begin_object(&jw);
    a3_jw_kv_floats(&jw, "v", v, 3);
    a3_jw_kv_number(&jw, "pi", 3.14159274f);
    a3_jw_end_object(&jw);
    A3_CHECK_STR(sb.data, "{\"v\":[0.1,1,-3.25],\"pi\":3.1415927}");
    a3_strbuf_free(&sb);
    a3_arena_release(&ar);
}

A3_TEST(json_errors) {
    A3Arena ar;
    a3_arena_init(&ar, A3_MEM_TEMP, 4096);
    A3JsonError err;
    const char *bad = "{\n  \"a\": 1,\n  \"b\": [1, 2\n}";
    A3_CHECK(a3_json_parse(bad, a3_strlen(bad), &ar, &err) == 0);
    A3_CHECK_EQ_INT(err.line, 4);
    A3_CHECK(err.message[0] != 0);
    const char *bad2 = "{\"a\" 1}";
    A3_CHECK(a3_json_parse(bad2, a3_strlen(bad2), &ar, &err) == 0);
    A3_CHECK(a3_stristr(err.message, "':'") != 0);
    A3_CHECK(a3_json_parse("", 0, &ar, &err) == 0);
    a3_arena_release(&ar);
}

A3_TEST(ecs_entities) {
    setup();
    A3World *w = a3_world_create("test");
    A3Entity a = a3_entity_create(w, "A");
    A3Entity b = a3_entity_create(w, "B");
    A3_CHECK(a3_entity_valid(w, a) && a3_entity_valid(w, b));
    A3_CHECK(a3_entity_guid(w, a) != a3_entity_guid(w, b));
    A3_CHECK_STR(a3_entity_name(w, a), "A");
    a3_entity_destroy(w, a);
    A3_CHECK(!a3_entity_valid(w, a));
    A3Entity c = a3_entity_create(w, "C");   /* reuses A's slot */
    A3_CHECK_EQ_INT(c.index, a.index);
    A3_CHECK(c.gen != a.gen);
    A3_CHECK(!a3_entity_valid(w, a));         /* stale handle stays invalid */
    a3_log_set_console(0);
    A3_CHECK(a3_component_add(w, a, A3_T_TRANSFORM) == 0); /* stale handle rejected */
    a3_log_set_console(1);
    A3_CHECK(a3_entity_eq(a3_entity_find_by_name(w, "C"), c));
    A3_CHECK(a3_entity_eq(a3_entity_find_by_guid(w, a3_entity_guid(w, b)), b));
    a3_entity_destroy_deferred(w, b);
    A3_CHECK(a3_entity_valid(w, b));
    A3_CHECK(!a3_entity_active(w, b));
    a3_world_flush(w);
    A3_CHECK(!a3_entity_valid(w, b));
    A3_CHECK_EQ_INT(a3_world_entity_count(w), 1);
    a3_world_destroy(w);
}

A3_TEST(ecs_components_query) {
    setup();
    A3World *w = a3_world_create("test");
    A3Entity es[100];
    for (int i = 0; i < 100; ++i) {
        es[i] = a3_entity_create(w, "E");
        A3CTransform *t = A3_ADD(w, es[i], A3CTransform, A3_T_TRANSFORM);
        A3_CHECK(t && t->scale.x == 1.0f);      /* defaults applied */
        t->position.x = (f32)i;
        if (i % 3 == 0) A3_ADD(w, es[i], A3CMeshRenderer, A3_T_MESH_RENDERER);
    }
    /* MeshRenderer requires Transform: adding to a bare entity adds both */
    A3Entity lone = a3_entity_create(w, "Lone");
    A3_CHECK(a3_component_add(w, lone, A3_T_MESH_RENDERER) != 0);
    A3_CHECK(a3_component_has(w, lone, A3_T_TRANSFORM));
    u32 types[2] = { A3_T_TRANSFORM, A3_T_MESH_RENDERER };
    A3Query q = a3_query_begin(w, types, 2);
    int n = 0;
    f32 sum = 0;
    while (a3_query_next(&q)) { n++; sum += ((A3CTransform *)q.components[0])->position.x; }
    A3_CHECK_EQ_INT(n, 35);
    A3_CHECK_NEAR(sum, 1683.0, 0.001); /* 0+3+...+99 */
    /* inactive entities are skipped */
    a3_entity_set_active(w, es[0], 0);
    q = a3_query_begin(w, types, 2);
    n = 0;
    while (a3_query_next(&q)) n++;
    A3_CHECK_EQ_INT(n, 34);
    /* removal keeps others intact */
    A3_CHECK(a3_component_remove(w, es[3], A3_T_MESH_RENDERER));
    A3_CHECK(!a3_component_has(w, es[3], A3_T_MESH_RENDERER));
    A3_CHECK_EQ_INT(A3_GET(w, es[99], A3CTransform, A3_T_TRANSFORM)->position.x, 99);
    A3_CHECK_EQ_INT(a3_component_count(w, A3_T_MESH_RENDERER), 34);
    A3_CHECK(a3_component_get_by_name(w, es[6], "MeshRenderer") != 0);
    a3_log_set_console(0);
    A3_CHECK(a3_component_add_by_name(w, es[6], "NoSuchComponent") == 0);
    a3_log_set_console(1);
    a3_world_destroy(w);
}

A3_TEST(ecs_hierarchy_transforms) {
    setup();
    A3World *w = a3_world_create("test");
    A3Entity root = a3_entity_create(w, "Root");
    A3Entity child = a3_entity_create(w, "Child");
    A3Entity grand = a3_entity_create(w, "Grand");
    A3CTransform *rt = A3_ADD(w, root, A3CTransform, A3_T_TRANSFORM);
    rt->position = a3_v3(10, 0, 0);
    rt->rotation = a3_quat_axis_angle(a3_v3(0, 1, 0), A3_HALF_PI);
    A3CTransform *ct = A3_ADD(w, child, A3CTransform, A3_T_TRANSFORM);
    ct->position = a3_v3(0, 0, -2);
    A3_ADD(w, grand, A3CTransform, A3_T_TRANSFORM)->position = a3_v3(0, 1, 0);
    A3_CHECK(a3_entity_set_parent(w, child, root));
    A3_CHECK(a3_entity_set_parent(w, grand, child));
    a3_log_set_console(0);
    A3_CHECK(!a3_entity_set_parent(w, root, grand)); /* cycle rejected */
    a3_log_set_console(1);
    A3_CHECK(a3_entity_is_ancestor(w, root, grand));
    A3_CHECK_EQ_INT(a3_entity_child_count(w, root), 1);
    a3_transform_system_update(w);
    A3Vec3 gp = a3_mat4_get_translation(&A3_GET(w, grand, A3CTransform, A3_T_TRANSFORM)->world);
    /* root rotated 90deg about Y: local -Z becomes world -X */
    A3_CHECK(a3_v3_nearly_equal(gp, a3_v3(8, 1, 0), 1e-5f));
    A3_CHECK(a3_v3_nearly_equal(a3_transform_world_position(w, grand), a3_v3(8, 1, 0), 1e-5f));
    a3_transform_set_world_position(w, grand, a3_v3(0, 0, 0));
    A3_CHECK(a3_v3_nearly_equal(a3_transform_world_position(w, grand), a3_v3(0, 0, 0), 1e-4f));
    /* destroying the root destroys the subtree */
    a3_entity_destroy(w, root);
    A3_CHECK(!a3_entity_valid(w, child) && !a3_entity_valid(w, grand));
    A3_CHECK_EQ_INT(a3_world_entity_count(w), 0);
    a3_world_destroy(w);
}

A3_TEST(ecs_custom_components) {
    setup();
    A3CustomFieldDef f[3];
    a3_zero(f, sizeof(f));
    a3_strcpy(f[0].name, A3_NAME_MAX, "health"); f[0].type = A3_FIELD_F32; f[0].default_value[0] = 100;
    a3_strcpy(f[1].name, A3_NAME_MAX, "speed"); f[1].type = A3_FIELD_F32; f[1].default_value[0] = 7;
    a3_strcpy(f[2].name, A3_NAME_MAX, "team"); f[2].type = A3_FIELD_STRING; a3_strcpy(f[2].default_string, A3_NAME_MAX, "Player");
    u32 id = a3_custom_component_define("TestStats", f, 3, "Beginner-defined stats");
    A3_CHECK(id != 0xFFFFFFFFu);
    A3ComponentType *t = a3_component_type(id);
    A3World *w = a3_world_create("test");
    A3Entity e = a3_entity_create(w, "Hero");
    u8 *data = (u8 *)a3_component_add(w, e, id);
    A3_CHECK(data != 0);
    const A3FieldDesc *hf = a3_component_find_field(t, "health");
    const A3FieldDesc *tf = a3_component_find_field(t, "team");
    A3_CHECK(hf && tf);
    A3_CHECK_NEAR(*(f32 *)(data + hf->offset), 100, 0);
    A3_CHECK_STR((char *)(data + tf->offset), "Player");
    *(f32 *)(data + hf->offset) = 42;
    /* redefine: remove speed, add armor; health value must survive */
    a3_strcpy(f[1].name, A3_NAME_MAX, "armor"); f[1].default_value[0] = 5;
    A3_CHECK(a3_custom_component_define("TestStats", f, 3, "v2") == id);
    data = (u8 *)a3_component_get(w, e, id);
    hf = a3_component_find_field(t, "health");
    const A3FieldDesc *af = a3_component_find_field(t, "armor");
    A3_CHECK(af && !a3_component_find_field(t, "speed"));
    A3_CHECK_NEAR(*(f32 *)(data + hf->offset), 42, 0);
    A3_CHECK_NEAR(*(f32 *)(data + af->offset), 5, 0);
    char label[64];
    a3_make_label("maxSpeed", label, sizeof(label));
    A3_CHECK_STR(label, "Max Speed");
    a3_make_label("jump_height", label, sizeof(label));
    A3_CHECK_STR(label, "Jump Height");
    a3_world_destroy(w);
}

A3_TEST(ecs_clone) {
    setup();
    A3World *w = a3_world_create("edit");
    A3Entity a = a3_entity_create(w, "A");
    A3_ADD(w, a, A3CTransform, A3_T_TRANSFORM)->position = a3_v3(1, 2, 3);
    A3Entity b = a3_entity_create(w, "B");
    a3_entity_set_parent(w, b, a);
    A3World *play = a3_world_clone(w, "play");
    A3_CHECK(play != 0);
    A3_CHECK(a3_entity_valid(play, a) && a3_entity_valid(play, b)); /* same handles valid */
    A3_GET(play, a, A3CTransform, A3_T_TRANSFORM)->position.x = 99;
    A3_CHECK_NEAR(A3_GET(w, a, A3CTransform, A3_T_TRANSFORM)->position.x, 1, 0); /* edit world untouched */
    A3_CHECK(a3_entity_eq(a3_entity_parent(play, b), a));
    a3_entity_destroy(play, a);
    A3_CHECK(a3_entity_valid(w, b));
    a3_world_destroy(play);
    a3_world_destroy(w);
}

A3_TEST(scene_roundtrip) {
    setup();
    A3World *w = a3_world_create("Level 1");
    A3Entity player = a3_entity_create(w, "Player");
    A3CTransform *t = A3_ADD(w, player, A3CTransform, A3_T_TRANSFORM);
    t->position = a3_v3(1.5f, 2, -3);
    t->rotation = a3_quat_euler(0, 90 * A3_DEG2RAD, 0);
    t->scale = a3_v3(1, 2, 1);
    A3CMeshRenderer *mr = A3_ADD(w, player, A3CMeshRenderer, A3_T_MESH_RENDERER);
    mr->primitive = A3_PRIM_CAPSULE;
    mr->base_color = a3_v4(0.2f, 0.4f, 0.9f, 1);
    a3_strcpy(mr->texture.path, sizeof(mr->texture.path), "Assets/Textures/hero.png");
    A3Entity cam = a3_entity_create(w, "Camera");
    A3_ADD(w, cam, A3CCamera, A3_T_CAMERA)->fov = 85;
    a3_entity_set_parent(w, cam, player);
    A3Entity sun = a3_entity_create(w, "Sun");
    A3CLight *l = A3_ADD(w, sun, A3CLight, A3_T_LIGHT);
    l->type = A3_LIGHT_DIRECTIONAL;
    a3_entity_set_active(w, sun, 0);
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_TEMP);
    A3_CHECK(a3_scene_save_json(w, &sb, 0) == A3_OK);
    A3_CHECK(a3_strstr(sb.data, "\"primitive\": \"Capsule\"") != 0);
    A3_CHECK(a3_strstr(sb.data, "\"rotation\": [0, 90, 0]") != 0);

    A3World *w2 = a3_world_create("load");
    A3SceneLoadReport rep;
    A3_CHECK(a3_scene_load_json(w2, sb.data, sb.len, &rep) == A3_OK);
    A3_CHECK_EQ_INT(rep.entities_loaded, 3);
    A3_CHECK_EQ_INT(rep.warning_count, 0);
    A3Entity p2 = a3_entity_find_by_guid(w2, a3_entity_guid(w, player));
    A3_CHECK(a3_entity_valid(w2, p2));
    A3CTransform *t2 = A3_GET(w2, p2, A3CTransform, A3_T_TRANSFORM);
    A3_CHECK(a3_v3_nearly_equal(t2->position, a3_v3(1.5f, 2, -3), 1e-6f));
    A3_CHECK_NEAR(a3_absf(a3_quat_dot(t2->rotation, t->rotation)), 1.0, 1e-5);
    A3CMeshRenderer *mr2 = A3_GET(w2, p2, A3CMeshRenderer, A3_T_MESH_RENDERER);
    A3_CHECK_EQ_INT(mr2->primitive, A3_PRIM_CAPSULE);
    A3_CHECK_STR(mr2->texture.path, "Assets/Textures/hero.png");
    A3Entity c2 = a3_entity_find_by_name(w2, "Camera");
    A3_CHECK(a3_entity_eq(a3_entity_parent(w2, c2), p2));
    A3_CHECK_NEAR(A3_GET(w2, c2, A3CCamera, A3_T_CAMERA)->fov, 85, 0);
    A3_CHECK(!a3_entity_active(w2, a3_entity_find_by_name(w2, "Sun")));
    /* saving the loaded world reproduces the same text (stable diffs) */
    A3StrBuf sb2;
    a3_strbuf_init(&sb2, A3_MEM_TEMP);
    a3_scene_save_json(w2, &sb2, 0);
    A3_CHECK_STR(sb2.data, sb.data);
    a3_strbuf_free(&sb);
    a3_strbuf_free(&sb2);
    a3_world_destroy(w);
    a3_world_destroy(w2);
}

A3_TEST(scene_unknown_and_version) {
    setup();
    const char *scene =
        "{\"format\":\"asm3d.scene\",\"version\":1,\"entities\":[{\"guid\":\"00000000000000aa\",\"name\":\"Door\","
        "\"components\":{\"Transform\":{\"position\":[1,2,3]},\"FancyPluginDoor\":{\"speed\":2,\"sound\":\"creak\"}}}]}";
    A3World *w = a3_world_create("t");
    A3SceneLoadReport rep;
    a3_log_set_console(0);
    A3_CHECK(a3_scene_load_json(w, scene, a3_strlen(scene), &rep) == A3_OK);
    a3_log_set_console(1);
    A3_CHECK_EQ_INT(rep.unknown_components, 1);
    A3_CHECK_EQ_INT(rep.warning_count, 1);
    A3_CHECK(rep.missing_fields > 0); /* rotation/scale absent -> defaults */
    A3Entity door = a3_entity_find_by_guid(w, 0xaa);
    A3_CHECK(A3_GET(w, door, A3CTransform, A3_T_TRANSFORM)->scale.y == 1.0f);
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_TEMP);
    a3_scene_save_json(w, &sb, 0);
    A3_CHECK(a3_strstr(sb.data, "FancyPluginDoor") && a3_strstr(sb.data, "creak")); /* preserved */
    a3_strbuf_free(&sb);
    a3_world_destroy(w);

    const char *future = "{\"format\":\"asm3d.scene\",\"version\":999,\"engine\":\"9.0\",\"entities\":[]}";
    w = a3_world_create("t");
    a3_log_set_console(0);
    A3_CHECK(a3_scene_load_json(w, future, a3_strlen(future), &rep) == A3_ERR_VERSION);
    A3_CHECK(a3_stristr(rep.hint, "newer") != 0);
    const char *broken = "{\"format\":\"asm3d.scene\",\n\"version\":1,\n\"entities\":[{\"name\": }]}";
    A3_CHECK(a3_scene_load_json(w, broken, a3_strlen(broken), &rep) == A3_ERR_PARSE);
    a3_log_set_console(1);
    A3_CHECK(a3_stristr(rep.error, "line 3") != 0);
    A3_CHECK(rep.hint[0] != 0);
    a3_world_destroy(w);
}

A3_TEST(scene_prefab_remap) {
    setup();
    A3CustomFieldDef f;
    a3_zero_struct(&f);
    a3_strcpy(f.name, A3_NAME_MAX, "target");
    f.type = A3_FIELD_ENTITY;
    u32 follow = a3_custom_component_define("TestFollow", &f, 1, 0);
    A3World *w = a3_world_create("t");
    A3Entity car = a3_entity_create(w, "Car");
    A3Entity wheel = a3_entity_create(w, "Wheel");
    a3_entity_set_parent(w, wheel, car);
    A3EntityRef *ref = (A3EntityRef *)a3_component_add(w, car, follow);
    ref->guid = a3_entity_guid(w, wheel);
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_TEMP);
    A3_CHECK(a3_entities_save_json(w, &car, 1, &sb) == A3_OK);
    A3Entity roots[4];
    u32 n = a3_entities_load_json(w, sb.data, sb.len, A3_ENTITY_NULL, roots, 4);
    A3_CHECK_EQ_INT(n, 1);
    A3Entity car2 = roots[0];
    A3_CHECK(!a3_entity_eq(car2, car));
    A3Entity wheel2 = a3_entity_first_child(w, car2);
    A3_CHECK(a3_entity_valid(w, wheel2) && a3_entity_guid(w, wheel2) != a3_entity_guid(w, wheel));
    A3EntityRef *ref2 = (A3EntityRef *)a3_component_get(w, car2, follow);
    A3_CHECK(ref2 && ref2->guid == a3_entity_guid(w, wheel2)); /* internal reference remapped */
    A3Entity dup = a3_entity_duplicate(w, car);
    A3_CHECK(a3_entity_child_count(w, dup) == 1);
    a3_strbuf_free(&sb);
    a3_world_destroy(w);
}
