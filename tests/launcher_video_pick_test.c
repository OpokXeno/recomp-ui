/* Renderer and VSync as LISTS, not cycles.
 *
 * These two rows were click-to-cycle buttons. With a host-supplied renderer
 * vocabulary of five entries, reaching the last one meant four clicks and
 * counting, so they became dropdowns -- which needs enumerate-and-set rather
 * than next(). The risk in that change is the three-way precedence the label
 * getter has always had (game labels, then console profile pair, then the
 * legacy pair): a dropdown that enumerates one vocabulary while the label
 * shows another would offer entries that do not match what is selected.
 */
#include "launcher_model.c"

#include <stdio.h>
#include <string.h>

void launcher_binds_set_zapper(int a, int b);
void launcher_binds_set_zapper(int a, int b) { (void)a; (void)b; }

static int fails;
static void ok(int cond, const char* what) {
    printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) fails++;
}

int main(void) {
    static LauncherModel m;
    static const char* const kLabels[] = {
        "Auto", "Direct3D 11", "Direct3D 9", "OpenGL", "Software",
    };

    /* ---- game-supplied vocabulary (the Gundam Wing case) ---------------- */
    memset(&m, 0, sizeof(m));
    m.has_renderer = true;
    m.renderer_labels = kLabels;
    m.num_renderers = 5;

    ok(launcher_model_renderer_count(&m) == 5, "count follows the game's vocabulary");
    ok(strcmp(launcher_model_renderer_label_at(&m, 0), "Auto") == 0,
       "index 0 is Auto");
    ok(strcmp(launcher_model_renderer_label_at(&m, 4), "Software") == 0,
       "the last entry is reachable directly, without cycling to it");

    launcher_model_set_renderer(&m, 4);
    ok(m.s.renderer == 4, "set lands on the requested index");
    ok(strcmp(launcher_model_renderer_label(&m), "Software") == 0,
       "and the label the combo shows agrees with what is selected");

    launcher_model_set_renderer(&m, 99);
    ok(m.s.renderer == 4, "an out-of-range index clamps rather than wrapping to Auto");
    launcher_model_set_renderer(&m, -1);
    ok(m.s.renderer == 0, "and clamps at the bottom too");

    /* ---- legacy pair: no game vocabulary, no profile -------------------- */
    memset(&m, 0, sizeof(m));
    m.has_renderer = true;
    ok(launcher_model_renderer_count(&m) == 2, "legacy surface enumerates two");
    ok(strcmp(launcher_model_renderer_label_at(&m, 0), "Software") == 0 &&
       strcmp(launcher_model_renderer_label_at(&m, 1), "OpenGL") == 0,
       "with the legacy pair, in the order the label getter uses");
    launcher_model_set_renderer(&m, 1);
    ok(strcmp(launcher_model_renderer_label(&m), "OpenGL") == 0,
       "label still agrees on the legacy surface");

    /* ---- vsync takes VALUES, not indices -------------------------------- */
    memset(&m, 0, sizeof(m));
    m.has_vsync = true;
    launcher_model_set_vsync(&m, RECOMP_LAUNCHER_VSYNC_ADAPTIVE);
    ok(m.s.vsync == RECOMP_LAUNCHER_VSYNC_ADAPTIVE &&
       strcmp(launcher_model_vsync_label(&m), "Adaptive") == 0,
       "adaptive is selectable directly");
    launcher_model_set_vsync(&m, RECOMP_LAUNCHER_VSYNC_OFF);
    ok(strcmp(launcher_model_vsync_label(&m), "Off") == 0, "and Off");
    launcher_model_set_vsync(&m, 12345);
    ok(m.s.vsync == RECOMP_LAUNCHER_VSYNC_OFF,
       "a value outside the vocabulary is refused, not stored");

    /* A host that does not offer the control must not be edited by it. */
    memset(&m, 0, sizeof(m));
    launcher_model_set_renderer(&m, 3);
    launcher_model_set_vsync(&m, RECOMP_LAUNCHER_VSYNC_ON);
    ok(m.s.renderer == 0 && m.s.vsync == 0,
       "neither setter writes when the console does not have the control");

    /* Loading and accepting settings must preserve every valid FPS target,
     * including custom values absent from the preset dropdown. */
    {
        RecompLauncherCGameInfo game = {0};
        game.has_frame_interp = 1;
        int preserved = 1, labels_match = 1;
        for (int fps = 30; fps <= 240; ++fps) {
            RecompLauncherCSettings settings = {0}, committed = {0};
            settings.fps = fps;
            launcher_model_init(&m, &settings, &game, NULL);
            launcher_model_commit(&m, &committed);
            preserved &= m.s.fps == fps && committed.fps == fps;
            char expected[24];
            snprintf(expected, sizeof(expected), "%d FPS", fps);
            const char *label = launcher_model_fps_label(&m);
            labels_match &= strcmp(label, fps == 30 ? "Original" : expected) == 0;
            const int index = launcher_model_fps_index(&m);
            labels_match &= index < 0 || strcmp(launcher_model_fps_label_at(index), label) == 0;
        }
        ok(preserved, "opening/accepting the launcher preserves all valid FPS settings");
        ok(labels_match, "FPS label and selection match presets and custom targets");
        for (int fps = 0; fps <= 300; fps += 300) {
            RecompLauncherCSettings settings = {0};
            settings.fps = fps;
            launcher_model_init(&m, &settings, &game, NULL);
            ok(m.s.fps == 30, "invalid FPS defaults to Original");
        }
    }

    /* Internal resolution belongs to the render target, not the window or
     * renderer dropdown. Accepting settings must retain the exact pair. */
    {
        static const char* const apis[] = {"OpenGL", "Vulkan (Recommended)"};
        RecompLauncherCGameInfo game = {0};
        RecompLauncherCSettings settings = {0}, committed = {0};
        game.has_renderer = game.has_supersampling = game.has_native_resolution = 1;
        game.native_resolution_renderer_index = 1;
        game.renderer_labels = apis;
        game.num_renderers = 2;
        settings.renderer = 1;
        settings.internal_width = 3840;
        settings.internal_height = 2160;
        settings.supersampling = 4;
        launcher_model_init(&m, &settings, &game, NULL);
        ok(strcmp(launcher_model_supersampling_label(&m), "3840 x 2160") == 0,
           "Vulkan (Recommended) shows the exact internal dimensions");
        m.s.window_width = 640;
        launcher_model_commit(&m, &committed);
        ok(committed.internal_width == 3840 && committed.internal_height == 2160,
           "changing window size and accepting settings preserves the 4K raster");
        launcher_model_set_renderer(&m, 0);
        launcher_model_cycle_supersampling(&m);
        launcher_model_set_renderer(&m, 1);
        ok(m.s.internal_width == 3840 && m.s.internal_height == 2160 && m.s.supersampling == 5,
           "integer-scale settings and fixed resolution remain independent");
        launcher_model_cycle_supersampling(&m);
        ok(m.s.internal_width == 640 && m.s.internal_height == 480,
           "the fixed resolution selector wraps after 4K");
        settings.internal_width = settings.internal_height = 0;
        launcher_model_init(&m, &settings, &game, NULL);
        ok(m.s.internal_width == 1920 && m.s.internal_height == 1080,
           "settings without a fixed resolution start at 1920 x 1080");
        static const char* const translated_apis[] = {"OpenGL", "Vulkan (Recomendado)"};
        m.renderer_labels = translated_apis;
        ok(launcher_model_native_resolution_active(&m),
           "translated renderer labels retain the fixed resolution control");
        m.native_resolution_renderer_index = 0;
        launcher_model_set_renderer(&m, 0);
        ok(launcher_model_native_resolution_active(&m),
           "the host can supply a different renderer order without relying on labels");
    }

    printf(fails ? "FAILED\n" : "PASSED\n");
    return fails;
}
