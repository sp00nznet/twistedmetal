/*
 * The game's own hooks, empty for twistedmetal_smoke.
 *
 * boot_main.cpp calls these functions from src/hle_extra.cpp, which targets
 * specific functions of the lifted game that the smoke title does not have.
 * The smoke target links these empty versions instead, so it checks the
 * harness alone (loader, vblank clock, backend, exit) without any game data.
 */
extern "C" {
void tm_hle_register_extra(void) {}
void tm_init_title_metadata(void) {}
void tm_fbdump_tick(void) {}
void tm_ef_kick_tick(void) {}
void tm_loaddone_tick(void) {}
void tm_fifowatch_tick(void) {}
void tm_memdump_tick(void) {}
}
