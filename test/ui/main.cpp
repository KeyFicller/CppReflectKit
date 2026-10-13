#include "ui.h"

// Opens a window and runs its frame loop, so it never returns on its own and
// cannot run unattended. Registered as a manual ctest: run it directly to
// watch.
int main() {
    ui_reflection();
    return 0;
}
