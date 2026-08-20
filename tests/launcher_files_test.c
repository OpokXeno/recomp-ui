#include "common/launcher_files.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int condition, const char* message) {
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

int main(void) {
    char paths[128];
    int count = launcher_parse_file_list(
        "/mods/one.psxmod|/mods/two.psxmod\n/mods/three.psxmod\r\n",
        paths, sizeof(paths));
    check(count == 3, "mixed native separators produce three paths");
    const char* path = paths;
    check(strcmp(path, "/mods/one.psxmod") == 0, "first path preserved");
    path += strlen(path) + 1;
    check(strcmp(path, "/mods/two.psxmod") == 0, "second path preserved");
    path += strlen(path) + 1;
    check(strcmp(path, "/mods/three.psxmod") == 0, "third path preserved");
    path += strlen(path) + 1;
    check(path[0] == '\0', "path list has a double-NUL terminator");

    char in_place[64] = "first.psxmod|second.psxmod";
    count = launcher_parse_file_list(in_place, in_place, sizeof(in_place));
    check(count == 2, "in-place native result conversion works");
    check(strcmp(in_place, "first.psxmod") == 0, "in-place first path preserved");
    check(strcmp(in_place + strlen(in_place) + 1, "second.psxmod") == 0,
          "in-place second path preserved");

    char too_small[5] = "data";
    check(launcher_parse_file_list("one|two", too_small, sizeof(too_small)) == -1,
          "small output buffer is rejected");
    check(too_small[0] == '\0', "failed conversion clears output");
    check(launcher_parse_file_list("|||\r\n", paths, sizeof(paths)) == 0,
          "separator-only result is empty");

    if (!failures) puts("launcher file-list tests passed");
    return failures ? 1 : 0;
}
