/* The game's debug printf, to game_log.txt. See gamelog.c. */
#ifndef PSP2I_GAMELOG_H
#define PSP2I_GAMELOG_H

#include <stdint.h>

void gamelog_init(const char *exe_dir);
/* A line of our own in game_log.txt (and online_log.txt). */
void gamelog_note(const char *fmt, ...);
/* The latest player object the game called local (0x08A3DE1C), 0 until then
 * and in PSP2I_PROD builds. The VR probe scores camera candidates with it. */
uint32_t gamelog_local_player(void);

#endif
